#include "hd_wallet_backend.hpp"

#include "platform.hpp"
#include "bech32.hpp"
#include "hash.hpp"
#include "mnemonic.hpp"
#include "script.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace bincoin {
namespace {

constexpr const char* LEGACY_MAGIC = "BINARYCOIN_TESTNET_WALLET_V1";
constexpr const char* HD_MAGIC = "BINARYCOIN_TESTNET_HD_WALLET_V2";
constexpr std::uint32_t LOOKAHEAD = 20;

std::size_t estimated_signed_size(Transaction transaction) {
    for (TxInput& input : transaction.inputs) {
        input.script_sig.assign(74, 0U);
        input.script_sig.insert(input.script_sig.begin(), 74U);
    }
    return transaction.serialize().size();
}

void restrict_permissions(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace,
        error);
    if (error) throw std::runtime_error("Unable to restrict wallet permissions: " + error.message());
}

std::map<std::string, std::string> read_fields(std::istream& input) {
    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(input, line)) {
        const auto equals = line.find('=');
        if (equals != std::string::npos) fields[line.substr(0, equals)] = line.substr(equals + 1);
    }
    return fields;
}

std::uint32_t parse_u32_field(const std::map<std::string, std::string>& fields, const char* name) {
    const auto found = fields.find(name);
    if (found == fields.end()) throw std::runtime_error(std::string("Wallet is missing field: ") + name);
    std::size_t used = 0;
    const unsigned long value = std::stoul(found->second, &used, 10);
    if (used != found->second.size() || value > 1'000'000UL) throw std::runtime_error("Invalid wallet index");
    return static_cast<std::uint32_t>(value);
}

} // namespace

HdWalletBackend::HdWalletBackend(std::filesystem::path data_directory)
    : data_directory_(std::move(data_directory)),
      wallet_path_(data_directory_ / "wallet-v2.dat"),
      legacy_path_(data_directory_ / "wallet-v1.dat") {}

std::string HdWalletBackend::create() {
    if (exists()) throw std::runtime_error("Wallet already exists in: " + data_directory_.string());
    const Entropy256 entropy = generate_entropy256();
    set_hd_entropy(entropy);
    save_hd();
    return entropy_to_mnemonic(entropy);
}

void HdWalletBackend::import_mnemonic(const std::string& phrase) {
    if (exists()) throw std::runtime_error("Wallet already exists in: " + data_directory_.string());
    set_hd_entropy(mnemonic_to_entropy(phrase));
    save_hd();
}

std::string HdWalletBackend::upgrade_legacy() {
    if (std::filesystem::exists(wallet_path_)) throw std::runtime_error("HD wallet already exists");
    if (!std::filesystem::exists(legacy_path_)) throw std::runtime_error("No legacy wallet exists to upgrade");
    load();
    if (kind_ != WalletKind::LegacySingleKey || !legacy_key_) throw std::runtime_error("Legacy wallet could not be loaded");
    const Secp256k1Key preserved = *legacy_key_;
    const Entropy256 entropy = generate_entropy256();
    set_hd_entropy(entropy);
    legacy_key_ = preserved;
    save_hd();
    return entropy_to_mnemonic(entropy);
}

void HdWalletBackend::set_hd_entropy(const Entropy256& entropy) {
    entropy_ = entropy;
    const std::string phrase = entropy_to_mnemonic(entropy);
    const auto seed = mnemonic_to_seed(phrase);
    master_ = bip32_master(seed);
    kind_ = WalletKind::HdBip32;
    next_receive_ = 1;
    next_change_ = 1;
    owned_cache_valid_ = false;
    owned_cache_.clear();
}

void HdWalletBackend::save_hd() const {
    if (!entropy_ || !master_) throw std::runtime_error("HD wallet is not initialized");
    std::filesystem::create_directories(wallet_path_.parent_path());
    const auto temporary = wallet_path_.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("Unable to create HD wallet file");
    output << HD_MAGIC << '\n'
           << "entropy=" << bytes_to_hex(*entropy_) << '\n'
           << "next_receive=" << next_receive_ << '\n'
           << "next_change=" << next_change_ << '\n';
    if (legacy_key_) output << "legacy_secret=" << legacy_key_->secret_hex() << '\n';
    output.flush();
    if (!output) throw std::runtime_error("Unable to write HD wallet file");
    output.close();
    replace_file_atomically(temporary, wallet_path_);
    restrict_permissions(wallet_path_);
}

void HdWalletBackend::load() {
    entropy_.reset();
    master_.reset();
    legacy_key_.reset();

    if (std::filesystem::exists(wallet_path_)) {
        std::ifstream input(wallet_path_);
        std::string magic;
        if (!input || !std::getline(input, magic) || magic != HD_MAGIC) {
            throw std::runtime_error("Invalid or unsupported HD wallet file");
        }
        const auto fields = read_fields(input);
        const auto found_entropy = fields.find("entropy");
        if (found_entropy == fields.end()) throw std::runtime_error("HD wallet is missing entropy");
        const auto bytes = hex_to_bytes(found_entropy->second);
        if (bytes.size() != 32) throw std::runtime_error("HD wallet entropy has invalid length");
        Entropy256 entropy{};
        std::copy(bytes.begin(), bytes.end(), entropy.begin());
        set_hd_entropy(entropy);
        next_receive_ = parse_u32_field(fields, "next_receive");
        next_change_ = parse_u32_field(fields, "next_change");
        owned_cache_valid_ = false;
        owned_cache_.clear();
        const auto legacy = fields.find("legacy_secret");
        if (legacy != fields.end()) legacy_key_ = Secp256k1Key::from_secret_hex(legacy->second);
        return;
    }

    std::ifstream input(legacy_path_);
    if (!input) throw std::runtime_error("Wallet does not exist; run wallet-create first");
    std::string magic;
    std::string secret_line;
    if (!std::getline(input, magic) || magic != LEGACY_MAGIC ||
        !std::getline(input, secret_line) || !secret_line.starts_with("secret=")) {
        throw std::runtime_error("Invalid or unsupported legacy wallet file");
    }
    legacy_key_ = Secp256k1Key::from_secret_hex(secret_line.substr(7));
    kind_ = WalletKind::LegacySingleKey;
    owned_cache_valid_ = false;
    owned_cache_.clear();
}

bool HdWalletBackend::exists() const {
    return std::filesystem::exists(wallet_path_) || std::filesystem::exists(legacy_path_);
}

WalletKind HdWalletBackend::kind() const noexcept { return kind_; }
const char* HdWalletBackend::kind_name() const noexcept {
    return kind_ == WalletKind::HdBip32 ? "hd-bip32" : "legacy-single-key";
}

Secp256k1Key HdWalletBackend::hd_key(const std::uint32_t branch, const std::uint32_t index) const {
    if (!master_) throw std::runtime_error("HD wallet is not loaded");
    return derive_testnet_wallet_key(*master_, branch, index).key();
}

std::string HdWalletBackend::public_key_hex() const {
    return kind_ == WalletKind::HdBip32
        ? hd_key(0, 0).compressed_public_key_hex()
        : legacy_key_->compressed_public_key_hex();
}

std::string HdWalletBackend::address() const {
    const auto key = kind_ == WalletKind::HdBip32 ? hd_key(0, 0) : *legacy_key_;
    return encode_testnet_address(key.compressed_public_key());
}

std::string HdWalletBackend::get_new_address() {
    if (kind_ != WalletKind::HdBip32) throw std::runtime_error("Legacy wallet has one key; run wallet-upgrade first");
    const auto result = encode_testnet_address(hd_key(0, next_receive_).compressed_public_key());
    ++next_receive_;
    owned_cache_valid_ = false;
    owned_cache_.clear();
    save_hd();
    return result;
}

std::string HdWalletBackend::master_fingerprint_hex() const {
    return master_ ? master_fingerprint(*master_) : "none";
}

std::vector<std::uint8_t> HdWalletBackend::locking_script() const {
    const auto key = kind_ == WalletKind::HdBip32 ? hd_key(0, 0) : *legacy_key_;
    return make_p2pk_script(key.compressed_public_key());
}

void HdWalletBackend::rebuild_owned_cache() const {
    if (owned_cache_valid_) return;
    owned_cache_.clear();
    if (kind_ == WalletKind::HdBip32) {
        const std::uint32_t receive_limit = next_receive_ + LOOKAHEAD;
        const std::uint32_t change_limit = next_change_ + LOOKAHEAD;
        owned_cache_.reserve(static_cast<std::size_t>(receive_limit + change_limit + (legacy_key_ ? 1U : 0U)));
        for (std::uint32_t index = 0; index < receive_limit; ++index) {
            auto key = hd_key(0, index);
            owned_cache_.emplace_back(make_p2pk_script(key.compressed_public_key()), std::move(key));
        }
        for (std::uint32_t index = 0; index < change_limit; ++index) {
            auto key = hd_key(1, index);
            owned_cache_.emplace_back(make_p2pk_script(key.compressed_public_key()), std::move(key));
        }
    }
    if (legacy_key_) {
        owned_cache_.emplace_back(make_p2pk_script(legacy_key_->compressed_public_key()), *legacy_key_);
    }
    owned_cache_valid_ = true;
}

std::optional<Secp256k1Key> HdWalletBackend::key_for_script(const std::span<const std::uint8_t> script) const {
    rebuild_owned_cache();
    for (const auto& [owned_script, key] : owned_cache_) {
        if (owned_script.size() == script.size() &&
            std::equal(owned_script.begin(), owned_script.end(), script.begin())) {
            return key;
        }
    }
    return std::nullopt;
}

bool HdWalletBackend::owns_script(const std::span<const std::uint8_t> script_pubkey) const {
    return key_for_script(script_pubkey).has_value();
}

WalletBalance HdWalletBackend::balance(const UtxoSet& utxos, const std::uint64_t spend_height) const {
    WalletBalance result;
    for (const auto& [outpoint, coin] : utxos) {
        (void)outpoint;
        if (!owns_script(coin.output.script_pubkey)) continue;
        if (result.total > MAX_MONEY - coin.output.value) throw std::runtime_error("Wallet balance overflow");
        result.total += coin.output.value;
        if (coin.coinbase && spend_height < coin.height + COINBASE_MATURITY) result.immature += coin.output.value;
        else result.spendable += coin.output.value;
    }
    return result;
}

Transaction HdWalletBackend::create_payment(
    const UtxoSet& utxos,
    const std::uint64_t spend_height,
    const std::span<const std::uint8_t> destination_public_key,
    const Amount amount,
    const FeeRate& fee_rate
) const {
    if (amount <= 0 || !money_range(amount)) throw std::invalid_argument("Payment amount is outside money range");
    const auto destination_script = make_p2pk_script(destination_public_key);
    const auto change_key = kind_ == WalletKind::HdBip32 ? hd_key(1, 0) : *legacy_key_;
    const auto change_script = make_p2pk_script(change_key.compressed_public_key());

    struct Available { OutPoint outpoint; Coin coin; Secp256k1Key key; };
    std::vector<Available> available;
    for (const auto& [outpoint, coin] : utxos) {
        const auto key = key_for_script(coin.output.script_pubkey);
        if (!key) continue;
        if (coin.coinbase && spend_height < coin.height + COINBASE_MATURITY) continue;
        available.push_back(Available{outpoint, coin, *key});
    }
    std::sort(available.begin(), available.end(), [](const auto& left, const auto& right) {
        if (left.coin.output.value != right.coin.output.value) return left.coin.output.value < right.coin.output.value;
        return OutPointLess{}(left.outpoint, right.outpoint);
    });

    Transaction transaction;
    transaction.version = 1;
    Amount input_total = 0;
    std::vector<TxOutput> previous_outputs;
    std::vector<Secp256k1Key> signing_keys;

    for (const auto& item : available) {
        transaction.inputs.push_back(TxInput{.previous_output = item.outpoint, .script_sig = {}, .sequence = 0xffffffffU});
        previous_outputs.push_back(item.coin.output);
        signing_keys.push_back(item.key);
        if (input_total > MAX_MONEY - item.coin.output.value) throw std::runtime_error("Selected input overflow");
        input_total += item.coin.output.value;
        transaction.outputs = {
            TxOutput{.value = amount, .script_pubkey = destination_script},
            TxOutput{.value = 0, .script_pubkey = change_script},
        };
        const Amount estimated_fee = fee_rate.fee(estimated_signed_size(transaction));
        if (input_total >= amount && input_total - amount >= estimated_fee) break;
    }
    if (input_total < amount) throw std::runtime_error("Insufficient mature balance");

    transaction.outputs = {
        TxOutput{.value = amount, .script_pubkey = destination_script},
        TxOutput{.value = 0, .script_pubkey = change_script},
    };
    const Amount reserved_fee = fee_rate.fee(estimated_signed_size(transaction));
    if (input_total < amount + reserved_fee) throw std::runtime_error("Insufficient balance after fee");
    const Amount change = input_total - amount - reserved_fee;
    if (change == 0) transaction.outputs.pop_back();
    else transaction.outputs.back().value = change;

    for (TxInput& input : transaction.inputs) input.script_sig.clear();
    for (std::size_t index = 0; index < transaction.inputs.size(); ++index) {
        sign_p2pk_input(transaction, index, previous_outputs[index].script_pubkey, signing_keys[index]);
    }
    if (reserved_fee < fee_rate.fee(transaction.virtual_size())) {
        throw std::runtime_error("Reserved transaction fee was unexpectedly too small");
    }
    return transaction;
}

const std::filesystem::path& HdWalletBackend::path() const noexcept {
    return kind_ == WalletKind::HdBip32 ? wallet_path_ : legacy_path_;
}

} // namespace bincoin
