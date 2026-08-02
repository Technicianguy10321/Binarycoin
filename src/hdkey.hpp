#pragma once

#include "key.hpp"
#include "mnemonic.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace bincoin {

struct HdPrivateKey {
    std::array<std::uint8_t, 32> secret{};
    std::array<std::uint8_t, 32> chain_code{};
    std::uint8_t depth{0};
    std::uint32_t child_number{0};

    [[nodiscard]] Secp256k1Key key() const;
    [[nodiscard]] HdPrivateKey derive(std::uint32_t child) const;
};

[[nodiscard]] HdPrivateKey bip32_master(std::span<const std::uint8_t> seed);
[[nodiscard]] HdPrivateKey derive_testnet_wallet_key(
    const HdPrivateKey& master,
    std::uint32_t branch,
    std::uint32_t index
);
[[nodiscard]] std::string master_fingerprint(const HdPrivateKey& master);

inline constexpr std::uint32_t HARDENED = 0x80000000U;

} // namespace bincoin
