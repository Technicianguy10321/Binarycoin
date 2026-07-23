#include "mempool.hpp"

#include "platform.hpp"
#include "serialize.hpp"

#include <fstream>
#include <set>
#include <stdexcept>

namespace bincoin {
namespace {
constexpr const char* MEMPOOL_MAGIC = "BINARYCOIN_TESTNET_MEMPOOL_V1";
}

TestnetMempool::TestnetMempool(std::filesystem::path data_directory)
    : path_(std::move(data_directory) / "mempool-v1.tsv") {}

void TestnetMempool::load() {
    transactions_.clear();
    if (!std::filesystem::exists(path_)) return;
    std::ifstream input(path_);
    if (!input) throw std::runtime_error("Unable to open mempool file");
    std::string line;
    if (!std::getline(input, line) || line != MEMPOOL_MAGIC) throw std::runtime_error("Invalid mempool file");
    while (std::getline(input, line)) {
        if (!line.empty()) transactions_.push_back(Transaction::deserialize(hex_to_bytes(line)));
    }
}

void TestnetMempool::save() const {
    std::filesystem::create_directories(path_.parent_path());
    const auto temporary = std::filesystem::path(path_.string() + ".tmp");
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("Unable to create mempool file");
    output << MEMPOOL_MAGIC << '\n';
    for (const Transaction& transaction : transactions_) {
        output << bytes_to_hex(transaction.serialize()) << '\n';
    }
    output.flush();
    if (!output) throw std::runtime_error("Unable to write mempool file");
    output.close();
    replace_file_atomically(temporary, path_);
}

void TestnetMempool::clear() {
    transactions_.clear();
    save();
}

UtxoSet TestnetMempool::effective_utxos(const UtxoSet& chain_utxos, const std::uint64_t spend_height) const {
    UtxoSet result = chain_utxos;
    for (const Transaction& transaction : transactions_) {
        (void)apply_transaction(transaction, spend_height, result);
    }
    return result;
}

Amount TestnetMempool::total_fees(const UtxoSet& chain_utxos, const std::uint64_t spend_height) const {
    UtxoSet result = chain_utxos;
    Amount total = 0;
    for (const Transaction& transaction : transactions_) {
        const Amount fee = apply_transaction(transaction, spend_height, result);
        if (total > MAX_MONEY - fee) throw std::runtime_error("Mempool fee overflow");
        total += fee;
    }
    return total;
}

void TestnetMempool::add(
    const Transaction& transaction,
    const UtxoSet& chain_utxos,
    const std::uint64_t spend_height
) {
    if (transaction.is_coinbase()) throw std::runtime_error("Coinbase transactions cannot enter mempool");
    for (const Transaction& existing : transactions_) {
        if (existing.txid() == transaction.txid()) throw std::runtime_error("Transaction already in mempool");
    }
    UtxoSet result = effective_utxos(chain_utxos, spend_height);
    (void)apply_transaction(transaction, spend_height, result);
    transactions_.push_back(transaction);
    save();
}


void TestnetMempool::revalidate(
    const UtxoSet& chain_utxos,
    const std::uint64_t spend_height,
    const std::set<std::string>& confirmed_txids
) {
    UtxoSet effective = chain_utxos;
    std::vector<Transaction> retained;
    for (const Transaction& transaction : transactions_) {
        if (confirmed_txids.contains(transaction.txid())) continue;
        try {
            (void)apply_transaction(transaction, spend_height, effective);
            retained.push_back(transaction);
        } catch (const std::exception&) {
            // Drop transactions made invalid by the new chain or a prior retained transaction.
        }
    }
    transactions_ = std::move(retained);
    save();
}

const std::vector<Transaction>& TestnetMempool::transactions() const noexcept { return transactions_; }

} // namespace bincoin
