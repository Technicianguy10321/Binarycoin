#pragma once

#include "hdkey.hpp"
#include "key.hpp"
#include "wallet_backend.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bincoin {

class HdWalletBackend final : public WalletBackend {
public:
    explicit HdWalletBackend(std::filesystem::path data_directory);

    [[nodiscard]] std::string create();
    void import_mnemonic(const std::string& phrase);
    [[nodiscard]] std::string upgrade_legacy();
    void load();
    [[nodiscard]] bool exists() const;

    [[nodiscard]] WalletKind kind() const noexcept override;
    [[nodiscard]] const char* kind_name() const noexcept override;
    [[nodiscard]] std::string public_key_hex() const override;
    [[nodiscard]] std::string address() const override;
    [[nodiscard]] std::string get_new_address() override;
    [[nodiscard]] std::string master_fingerprint_hex() const override;
    [[nodiscard]] std::vector<std::uint8_t> locking_script() const override;
    [[nodiscard]] bool owns_script(std::span<const std::uint8_t> script_pubkey) const override;
    [[nodiscard]] WalletBalance balance(const UtxoSet& utxos, std::uint64_t spend_height) const override;
    [[nodiscard]] Transaction create_payment(
        const UtxoSet& utxos,
        std::uint64_t spend_height,
        std::span<const std::uint8_t> destination_public_key,
        Amount amount,
        const FeeRate& fee_rate
    ) const override;
    [[nodiscard]] const std::filesystem::path& path() const noexcept override;

private:
    void save_hd() const;
    void set_hd_entropy(const Entropy256& entropy);
    [[nodiscard]] Secp256k1Key hd_key(std::uint32_t branch, std::uint32_t index) const;
    void rebuild_owned_cache() const;
    [[nodiscard]] std::optional<Secp256k1Key> key_for_script(std::span<const std::uint8_t> script) const;

    std::filesystem::path data_directory_;
    std::filesystem::path wallet_path_;
    std::filesystem::path legacy_path_;
    WalletKind kind_{WalletKind::HdBip32};
    std::optional<Entropy256> entropy_;
    std::optional<HdPrivateKey> master_;
    std::optional<Secp256k1Key> legacy_key_;
    std::uint32_t next_receive_{1};
    std::uint32_t next_change_{1};
    mutable bool owned_cache_valid_{false};
    mutable std::vector<std::pair<std::vector<std::uint8_t>, Secp256k1Key>> owned_cache_;
};

} // namespace bincoin
