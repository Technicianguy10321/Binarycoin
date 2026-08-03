#include "wallet.hpp"

#include "hd_wallet_backend.hpp"

#include <stdexcept>
#include <utility>

namespace bincoin {
namespace {

[[nodiscard]] bool is_hd_name(const std::string_view name) {
    return name == "hd" || name == "hd-bip32";
}

[[noreturn]] void unavailable_backend(const std::string_view name) {
    throw std::invalid_argument("Unknown wallet backend: " + std::string(name));
}

} // namespace

WalletManager::WalletManager(std::filesystem::path data_directory)
    : data_directory_(std::move(data_directory)) {}

WalletManager::~WalletManager() = default;
WalletManager::WalletManager(WalletManager&&) noexcept = default;
WalletManager& WalletManager::operator=(WalletManager&&) noexcept = default;

void WalletManager::select_backend(const std::string_view backend_type) {
    if (!is_hd_name(backend_type)) unavailable_backend(backend_type);
    backend_ = std::make_unique<HdWalletBackend>(data_directory_);
}

std::string WalletManager::create(const std::string_view backend_type) {
    select_backend(backend_type);
    return dynamic_cast<HdWalletBackend&>(*backend_).create();
}

void WalletManager::import_mnemonic(const std::string& phrase, const std::string_view backend_type) {
    select_backend(backend_type);
    dynamic_cast<HdWalletBackend&>(*backend_).import_mnemonic(phrase);
}

std::string WalletManager::upgrade_legacy() {
    select_backend("hd");
    return dynamic_cast<HdWalletBackend&>(*backend_).upgrade_legacy();
}

void WalletManager::load() {
    select_backend("hd");
    dynamic_cast<HdWalletBackend&>(*backend_).load();
}

bool WalletManager::exists() const {
    HdWalletBackend probe(data_directory_);
    return probe.exists();
}

WalletBackend& WalletManager::backend() {
    if (!backend_) throw std::runtime_error("Wallet backend is not loaded");
    return *backend_;
}

const WalletBackend& WalletManager::backend() const {
    if (!backend_) throw std::runtime_error("Wallet backend is not loaded");
    return *backend_;
}

WalletKind WalletManager::kind() const { return backend().kind(); }
const char* WalletManager::kind_name() const { return backend().kind_name(); }
std::string WalletManager::public_key_hex() const { return backend().public_key_hex(); }
std::string WalletManager::address() const { return backend().address(); }
std::string WalletManager::get_new_address() { return backend().get_new_address(); }
std::string WalletManager::master_fingerprint_hex() const { return backend().master_fingerprint_hex(); }
std::vector<std::uint8_t> WalletManager::locking_script() const { return backend().locking_script(); }
bool WalletManager::owns_script(const std::span<const std::uint8_t> script_pubkey) const {
    return backend().owns_script(script_pubkey);
}
WalletBalance WalletManager::balance(const UtxoSet& utxos, const std::uint64_t spend_height) const {
    return backend().balance(utxos, spend_height);
}
Transaction WalletManager::create_payment(
    const UtxoSet& utxos,
    const std::uint64_t spend_height,
    const std::span<const std::uint8_t> destination_public_key,
    const Amount amount,
    const FeeRate& fee_rate
) const {
    return backend().create_payment(utxos, spend_height, destination_public_key, amount, fee_rate);
}
const std::filesystem::path& WalletManager::path() const { return backend().path(); }

} // namespace bincoin
