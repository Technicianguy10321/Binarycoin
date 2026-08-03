#include "chain.hpp"

#include "hash.hpp"
#include "merkle.hpp"
#include "pow.hpp"
#include "serialize.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>

namespace bincoin {
namespace {

constexpr const char* LEGACY_FILE_MAGIC = "BINARYCOIN_TESTNET_CHAIN_V3_UTXO";

std::vector<std::string> split(const std::string& text, const char separator) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t end = text.find(separator, begin);
        fields.push_back(text.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return fields;
}

template <typename Integer>
Integer parse_decimal(const std::string& text, const char* field_name) {
    Integer value{};
    const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (error != std::errc{} || pointer != text.data() + text.size()) {
        throw std::runtime_error(std::string("Invalid decimal ") + field_name + ": " + text);
    }
    return value;
}

std::uint32_t parse_hex_u32(const std::string& text, const char* field_name) {
    std::uint32_t value{};
    const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (error != std::errc{} || pointer != text.data() + text.size()) {
        throw std::runtime_error(std::string("Invalid hexadecimal ") + field_name + ": " + text);
    }
    return value;
}

void add_coinbase_outputs(const Transaction& coinbase, const std::uint64_t height, UtxoSet& utxos) {
    const std::string transaction_id = coinbase.txid();
    for (std::size_t output_index = 0; output_index < coinbase.outputs.size(); ++output_index) {
        OutPoint outpoint{
            .txid = transaction_id,
            .index = static_cast<std::uint32_t>(output_index),
        };
        if (utxos.contains(outpoint)) throw std::runtime_error("Duplicate coinbase output identifier");
        utxos.emplace(std::move(outpoint), Coin{
            .output = coinbase.outputs[output_index],
            .height = height,
            .coinbase = true,
        });
    }
}

std::set<std::string> transaction_ids(const std::vector<StoredBlock>& blocks) {
    std::set<std::string> result;
    for (const StoredBlock& stored : blocks) {
        for (const Transaction& transaction : stored.block.transactions) result.insert(transaction.txid());
    }
    return result;
}

bool utxo_sets_equal(const UtxoSet& left, const UtxoSet& right) {
    if (left.size() != right.size()) return false;
    auto a = left.begin();
    auto b = right.begin();
    for (; a != left.end(); ++a, ++b) {
        if (a->first.txid != b->first.txid || a->first.index != b->first.index ||
            a->second.output.value != b->second.output.value ||
            a->second.output.script_pubkey != b->second.output.script_pubkey ||
            a->second.height != b->second.height || a->second.coinbase != b->second.coinbase) {
            return false;
        }
    }
    return true;
}

} // namespace

std::uint32_t expected_testnet_bits(
    const std::span<const StoredBlock> previous_blocks,
    const std::uint64_t next_height
) {
    if (next_height == 0) return TESTNET_GENESIS_BITS;
    if (previous_blocks.size() != next_height) {
        throw std::invalid_argument("Difficulty history does not match the requested height");
    }
    if (next_height < TESTNET_DIFFICULTY_FORK_HEIGHT) return TESTNET_BITS;

    const StoredBlock& previous = previous_blocks.back();
    if ((next_height - TESTNET_DIFFICULTY_FORK_HEIGHT) % TESTNET_RETARGET_INTERVAL != 0) {
        return previous.block.header.bits;
    }

    const std::uint64_t first_height = next_height - TESTNET_RETARGET_INTERVAL;
    const StoredBlock& first = previous_blocks[static_cast<std::size_t>(first_height)];
    const std::uint64_t raw_timespan = previous.block.header.time > first.block.header.time
        ? static_cast<std::uint64_t>(previous.block.header.time - first.block.header.time)
        : 1U;
    const std::uint64_t target_timespan =
        (TESTNET_RETARGET_INTERVAL - 1U) * TESTNET_TARGET_BLOCK_SECONDS;
    const std::uint64_t minimum_timespan = std::max<std::uint64_t>(1U, target_timespan / 4U);
    const std::uint64_t maximum_timespan = target_timespan * 4U;
    const std::uint64_t actual_timespan =
        std::clamp(raw_timespan, minimum_timespan, maximum_timespan);

    boost::multiprecision::cpp_int target = compact_to_target(previous.block.header.bits);
    target *= actual_timespan;
    target /= target_timespan;

    const boost::multiprecision::cpp_int pow_limit = compact_to_target(TESTNET_POW_LIMIT_BITS);
    if (target > pow_limit) target = pow_limit;
    if (target < 1) target = 1;
    return target_to_compact(target);
}

TestnetChain::TestnetChain(std::filesystem::path data_directory)
    : data_directory_(std::move(data_directory)),
      legacy_v3_path_(data_directory_ / "chain-v3.tsv"),
      legacy_v2_path_(data_directory_ / "chain-v2.tsv"),
      legacy_v1_path_(data_directory_ / "chain.tsv"),
      storage_(data_directory_) {}

void TestnetChain::initialize() {
    if (data_directory_.empty()) throw std::invalid_argument("Data directory must not be empty");
    std::filesystem::create_directories(data_directory_);
    if (storage_.exists()) {
        load();
        return;
    }
    if (std::filesystem::exists(legacy_v3_path_)) {
        load_legacy_v3();
        verify();
        utxos_ = build_utxo_set(blocks_);
        storage_.mark_migrated_legacy();
        save();
        return;
    }
    if (std::filesystem::exists(legacy_v2_path_) || std::filesystem::exists(legacy_v1_path_)) {
        throw std::runtime_error(
            "Found an unsupported pre-v0.3 disposable chain. Use a fresh --datadir or remove the old directory."
        );
    }

    Block genesis = testnet_genesis_block();
    blocks_.assign(1, StoredBlock{.height = 0, .block = std::move(genesis)});
    utxos_.clear();
    verify();
    save();
}

void TestnetChain::load_legacy_v3() {
    std::ifstream input(legacy_v3_path_);
    if (!input) throw std::runtime_error("Unable to open legacy chain file: " + legacy_v3_path_.string());

    std::string line;
    if (!std::getline(input, line) || line != LEGACY_FILE_MAGIC) {
        throw std::runtime_error("Invalid or unsupported legacy chain file header");
    }

    blocks_.clear();
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split(line, '\t');
        if (fields.size() != 10) throw std::runtime_error("Malformed legacy chain record");

        StoredBlock stored;
        stored.height = parse_decimal<std::uint64_t>(fields[0], "height");
        stored.block.header.version = parse_decimal<std::int32_t>(fields[1], "version");
        stored.block.header.time = parse_decimal<std::uint32_t>(fields[2], "time");
        stored.block.header.bits = parse_hex_u32(fields[3], "bits");
        stored.block.header.nonce = parse_decimal<std::uint32_t>(fields[4], "nonce");
        stored.block.header.previous_hash = fields[5];
        stored.block.header.merkle_root = fields[6];
        if (stored.block.header.hash_hex() != fields[7]) {
            throw std::runtime_error("Legacy block hash mismatch at height " + std::to_string(stored.height));
        }
        if (fields[8] != "testnet") throw std::runtime_error("Legacy record belongs to another network");
        if (fields[9] != "-") {
            for (const std::string& transaction_hex : split(fields[9], ';')) {
                if (transaction_hex.empty()) throw std::runtime_error("Empty legacy transaction record");
                stored.block.transactions.push_back(Transaction::deserialize(hex_to_bytes(transaction_hex)));
            }
        }
        blocks_.push_back(std::move(stored));
    }
}

void TestnetChain::load() {
    if (!storage_.exists()) {
        if (std::filesystem::exists(legacy_v3_path_)) {
            load_legacy_v3();
            verify();
            utxos_ = build_utxo_set(blocks_);
            storage_.mark_migrated_legacy();
            save();
            return;
        }
        throw std::runtime_error("Durable chainstate does not exist; run init");
    }

    blocks_ = storage_.load(utxos_);
    verify();
    const UtxoSet rebuilt = build_utxo_set(blocks_);
    if (utxos_.empty() && !rebuilt.empty()) {
        utxos_ = rebuilt;
        save(); // recovered an index generation whose snapshot/manifest was incomplete
    } else if (!utxo_sets_equal(utxos_, rebuilt)) {
        throw std::runtime_error("Committed UTXO snapshot does not match the active block index; run reindex");
    }
}

void TestnetChain::save() {
    storage_.commit(blocks_, utxos_);
}

void TestnetChain::verify_blocks(const std::vector<StoredBlock>& blocks) {
    if (blocks.empty()) throw std::runtime_error("Testnet chain has no genesis block");

    const StoredBlock& genesis = blocks.front();
    if (genesis.height != 0 ||
        genesis.block.header.hash_hex() != TESTNET_GENESIS_HASH ||
        genesis.block.header.merkle_root != TESTNET_GENESIS_MERKLE ||
        genesis.block.header.previous_hash != std::string(64, '0') ||
        genesis.block.header.bits != TESTNET_GENESIS_BITS ||
        genesis.block.transactions.size() != 1 ||
        genesis.block.transactions.front().txid() != TESTNET_GENESIS_TXID ||
        !genesis.block.transactions.front().is_coinbase() ||
        sum_outputs(genesis.block.transactions.front()) != INITIAL_SUBSIDY) {
        throw std::runtime_error("Testnet genesis constants do not match the locked genesis block");
    }

    UtxoSet utxos;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const StoredBlock& stored = blocks[index];
        if (stored.height != index) throw std::runtime_error("Non-contiguous block height");
        if (!check_proof_of_work(stored.block.header.raw_hash(), stored.block.header.bits)) {
            throw std::runtime_error("Invalid proof of work at height " + std::to_string(stored.height));
        }
        if (index == 0) continue;
        const std::uint32_t expected_bits = expected_testnet_bits(
            std::span<const StoredBlock>(blocks.data(), index), stored.height);
        if (stored.block.header.bits != expected_bits) {
            throw std::runtime_error(
                "Unexpected testnet difficulty bits at height " + std::to_string(stored.height));
        }
        const StoredBlock& previous = blocks[index - 1];
        if (stored.block.header.previous_hash != previous.block.header.hash_hex()) {
            throw std::runtime_error("Broken previous-hash link at height " + std::to_string(stored.height));
        }
        if (stored.block.header.time <= previous.block.header.time) {
            throw std::runtime_error("Block timestamp did not increase at height " + std::to_string(stored.height));
        }
        if (stored.block.transactions.empty() || !stored.block.transactions.front().is_coinbase()) {
            throw std::runtime_error("Block must begin with exactly one coinbase transaction");
        }
        for (std::size_t tx_index = 1; tx_index < stored.block.transactions.size(); ++tx_index) {
            if (stored.block.transactions[tx_index].is_coinbase()) {
                throw std::runtime_error("Block contains more than one coinbase transaction");
            }
        }
        if (merkle_root_hex(stored.block.transactions) != stored.block.header.merkle_root) {
            throw std::runtime_error("Transaction Merkle root mismatch at height " + std::to_string(stored.height));
        }

        Amount fees = 0;
        for (std::size_t tx_index = 1; tx_index < stored.block.transactions.size(); ++tx_index) {
            const Amount fee = apply_transaction(stored.block.transactions[tx_index], stored.height, utxos);
            if (fees > MAX_MONEY - fee) throw std::runtime_error("Block fee overflow");
            fees += fee;
        }

        const Transaction& coinbase = stored.block.transactions.front();
        const Amount coinbase_total = sum_outputs(coinbase);
        const Amount subsidy = block_subsidy(stored.height);
        if (subsidy > MAX_MONEY - fees || coinbase_total != subsidy + fees) {
            throw std::runtime_error("Incorrect coinbase subsidy or fee claim at height " + std::to_string(stored.height));
        }
        add_coinbase_outputs(coinbase, stored.height, utxos);
    }
}

void TestnetChain::verify() const { verify_blocks(blocks_); }

boost::multiprecision::cpp_int TestnetChain::calculate_chainwork(const std::vector<StoredBlock>& blocks) {
    boost::multiprecision::cpp_int total = 0;
    for (const StoredBlock& stored : blocks) total += block_proof(stored.block.header.bits);
    return total;
}

boost::multiprecision::cpp_int TestnetChain::chainwork() const { return calculate_chainwork(blocks_); }
std::string TestnetChain::chainwork_hex() const { return uint256_to_hex(chainwork()); }

void TestnetChain::reindex() {
    const auto records = storage_.scan_all_blocks();
    if (records.empty()) throw std::runtime_error("Append-only block store contains no valid records");

    std::map<std::string, StoredBlock> by_hash;
    for (const StoredBlock& block : records) by_hash.emplace(block.block.header.hash_hex(), block);
    if (!by_hash.contains(TESTNET_GENESIS_HASH)) throw std::runtime_error("Block store does not contain testnet genesis");

    std::vector<StoredBlock> best;
    boost::multiprecision::cpp_int best_work = -1;
    for (const auto& [tip_hash, tip_block] : by_hash) {
        (void)tip_hash;
        std::vector<StoredBlock> reverse;
        std::set<std::string> visited;
        StoredBlock current = tip_block;
        bool complete = false;
        while (visited.insert(current.block.header.hash_hex()).second) {
            reverse.push_back(current);
            if (current.block.header.hash_hex() == TESTNET_GENESIS_HASH) {
                complete = true;
                break;
            }
            const auto parent = by_hash.find(current.block.header.previous_hash);
            if (parent == by_hash.end()) break;
            current = parent->second;
        }
        if (!complete) continue;
        std::reverse(reverse.begin(), reverse.end());
        try {
            verify_blocks(reverse);
            const auto work = calculate_chainwork(reverse);
            if (work > best_work) {
                best = std::move(reverse);
                best_work = work;
            }
        } catch (const std::exception&) {
            continue;
        }
    }
    if (best.empty()) throw std::runtime_error("No fully valid chain could be reconstructed from block records");
    blocks_ = std::move(best);
    utxos_ = build_utxo_set(blocks_);
    save();
}

void TestnetChain::append_blocks(const std::vector<StoredBlock>& incoming) {
    if (incoming.empty()) return;
    if (blocks_.empty()) initialize();

    const auto original_blocks = blocks_;
    const auto original_utxos = utxos_;
    try {
        for (const StoredBlock& stored : incoming) {
            const std::uint64_t expected_height = blocks_.back().height + 1;
            if (stored.height != expected_height) throw std::runtime_error("Received non-contiguous block height");
            if (stored.block.header.previous_hash != blocks_.back().block.header.hash_hex()) {
                throw std::runtime_error("Received block does not extend local tip");
            }
            blocks_.push_back(stored);
        }
        verify();
        utxos_ = build_utxo_set(blocks_);
        save();
    } catch (...) {
        blocks_ = original_blocks;
        utxos_ = original_utxos;
        throw;
    }
}

ChainActivationResult TestnetChain::activate_if_more_work(const std::vector<StoredBlock>& candidate) {
    verify_blocks(candidate);

    ChainActivationResult result;
    result.old_height = tip().height;
    result.new_height = candidate.back().height;
    result.old_chainwork = chainwork();
    result.candidate_chainwork = calculate_chainwork(candidate);

    std::size_t common_count = 0;
    const std::size_t limit = std::min(blocks_.size(), candidate.size());
    while (common_count < limit &&
           blocks_[common_count].block.header.hash_hex() == candidate[common_count].block.header.hash_hex()) {
        ++common_count;
    }
    if (common_count == 0) throw std::runtime_error("Candidate does not share BinaryCoin testnet genesis");
    result.fork_height = static_cast<std::uint64_t>(common_count - 1);
    result.disconnected_blocks = blocks_.size() - common_count;
    result.connected_blocks = candidate.size() - common_count;
    if (result.candidate_chainwork <= result.old_chainwork) return result;

    const auto candidate_txids = transaction_ids(candidate);
    for (std::size_t block_index = common_count; block_index < blocks_.size(); ++block_index) {
        const auto& transactions = blocks_[block_index].block.transactions;
        for (std::size_t tx_index = 1; tx_index < transactions.size(); ++tx_index) {
            if (!candidate_txids.contains(transactions[tx_index].txid())) {
                result.disconnected_transactions.push_back(transactions[tx_index]);
            }
        }
    }

    const auto original_blocks = blocks_;
    const auto original_utxos = utxos_;
    try {
        blocks_ = candidate;
        verify();
        utxos_ = build_utxo_set(blocks_);
        save();
        result.activated = true;
    } catch (...) {
        blocks_ = original_blocks;
        utxos_ = original_utxos;
        throw;
    }
    return result;
}

void TestnetChain::generate(
    const std::uint64_t count,
    const std::span<const std::uint8_t> coinbase_script_pubkey,
    const std::vector<Transaction>& pending_transactions
) {
    if (blocks_.empty()) initialize();
    UtxoSet current_utxos = utxos_;

    for (std::uint64_t generated = 0; generated < count; ++generated) {
        const StoredBlock& previous = blocks_.back();
        const std::uint64_t height = previous.height + 1;
        const std::vector<Transaction> transactions = generated == 0
            ? pending_transactions
            : std::vector<Transaction>{};

        UtxoSet candidate_utxos = current_utxos;
        Amount fees = 0;
        for (const Transaction& transaction : transactions) {
            const Amount fee = apply_transaction(transaction, height, candidate_utxos);
            if (fees > MAX_MONEY - fee) throw std::runtime_error("Candidate block fee overflow");
            fees += fee;
        }

        const std::uint32_t bits = expected_testnet_bits(
            std::span<const StoredBlock>(blocks_), height);
        Block block = mine_testnet_block(
            height,
            previous.block.header.hash_hex(),
            previous.block.header.time,
            bits,
            coinbase_script_pubkey,
            transactions,
            fees
        );
        add_coinbase_outputs(block.transactions.front(), height, candidate_utxos);
        blocks_.push_back(StoredBlock{.height = height, .block = std::move(block)});
        current_utxos = std::move(candidate_utxos);
    }
    verify();
    utxos_ = std::move(current_utxos);
    save();
}

const std::vector<StoredBlock>& TestnetChain::blocks() const noexcept { return blocks_; }

const StoredBlock& TestnetChain::tip() const {
    if (blocks_.empty()) throw std::runtime_error("Chain is not initialized");
    return blocks_.back();
}

const std::filesystem::path& TestnetChain::data_directory() const noexcept { return data_directory_; }

Amount TestnetChain::issued_supply() const {
    Amount total = 0;
    for (std::size_t index = 1; index < blocks_.size(); ++index) {
        const Amount subsidy = block_subsidy(blocks_[index].height);
        if (total > MAX_MONEY - subsidy) throw std::runtime_error("Issued supply overflow");
        total += subsidy;
    }
    return total;
}

UtxoSet TestnetChain::utxo_set() const { return utxos_; }
std::set<std::string> TestnetChain::confirmed_txids() const { return transaction_ids(blocks_); }
ChainStorageStats TestnetChain::storage_stats() const { return storage_.stats(); }

} // namespace bincoin
