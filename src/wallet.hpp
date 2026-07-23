#pragma once

#include "wallet_backend.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bincoin {

class WalletManager final {
public:
    explicit WalletManager(std::filesystem::path data_directory);
    ~WalletManager();

    WalletManager(const WalletManager&) = delete;
    WalletManager& operator=(const WalletManager&) = delete;
    WalletManager(WalletManager&&) noexcept;
    WalletManager& operator=(WalletManager&&) noexcept;

    [[nodiscard]] std::string create(std::string_view backend_type = "hd");
    void import_mnemonic(const std::string& phrase, std::string_view backend_type = "hd");
    [[nodiscard]] std::string upgrade_legacy();
    void load();
    [[nodiscard]] bool exists() const;

    [[nodiscard]] WalletKind kind() const;
    [[nodiscard]] const char* kind_name() const;
    [[nodiscard]] std::string public_key_hex() const;
    [[nodiscard]] std::string address() const;
    [[nodiscard]] std::string get_new_address();
    [[nodiscard]] std::string master_fingerprint_hex() const;
    [[nodiscard]] std::vector<std::uint8_t> locking_script() const;
    [[nodiscard]] bool owns_script(std::span<const std::uint8_t> script_pubkey) const;
    [[nodiscard]] WalletBalance balance(const UtxoSet& utxos, std::uint64_t spend_height) const;
    [[nodiscard]] Transaction create_payment(
        const UtxoSet& utxos,
        std::uint64_t spend_height,
        std::span<const std::uint8_t> destination_public_key,
        Amount amount,
        const FeeRate& fee_rate
    ) const;
    [[nodiscard]] const std::filesystem::path& path() const;

private:
    [[nodiscard]] WalletBackend& backend();
    [[nodiscard]] const WalletBackend& backend() const;
    void select_backend(std::string_view backend_type);

    std::filesystem::path data_directory_;
    std::unique_ptr<WalletBackend> backend_;
};

using TestnetWallet = WalletManager;

[[nodiscard]] Amount parse_bin_amount(const std::string& text);
[[nodiscard]] std::string format_bin_amount(Amount amount);

} // namespace bincoin
