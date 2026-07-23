#pragma once

#include "fees.hpp"
#include "transaction.hpp"
#include "utxo.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace bincoin {

struct WalletBalance {
    Amount spendable{0};
    Amount immature{0};
    Amount total{0};
};

enum class WalletKind {
    LegacySingleKey,
    HdBip32,
};

class WalletBackend {
public:
    virtual ~WalletBackend() = default;

    [[nodiscard]] virtual WalletKind kind() const noexcept = 0;
    [[nodiscard]] virtual const char* kind_name() const noexcept = 0;
    [[nodiscard]] virtual std::string public_key_hex() const = 0;
    [[nodiscard]] virtual std::string address() const = 0;
    [[nodiscard]] virtual std::string get_new_address() = 0;
    [[nodiscard]] virtual std::string master_fingerprint_hex() const = 0;
    [[nodiscard]] virtual std::vector<std::uint8_t> locking_script() const = 0;
    [[nodiscard]] virtual bool owns_script(std::span<const std::uint8_t> script_pubkey) const = 0;
    [[nodiscard]] virtual WalletBalance balance(const UtxoSet& utxos, std::uint64_t spend_height) const = 0;
    [[nodiscard]] virtual Transaction create_payment(
        const UtxoSet& utxos,
        std::uint64_t spend_height,
        std::span<const std::uint8_t> destination_public_key,
        Amount amount,
        const FeeRate& fee_rate
    ) const = 0;
    [[nodiscard]] virtual const std::filesystem::path& path() const noexcept = 0;
};

} // namespace bincoin
