#pragma once

#include "hash.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bincoin {

class Secp256k1Key {
public:
    static Secp256k1Key generate();
    static Secp256k1Key from_secret_hex(const std::string& secret_hex);
    static Secp256k1Key from_secret_bytes(const std::array<std::uint8_t, 32>& secret);

    [[nodiscard]] std::string secret_hex() const;
    [[nodiscard]] const std::array<std::uint8_t, 32>& secret_bytes() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> compressed_public_key() const;
    [[nodiscard]] std::string compressed_public_key_hex() const;
    [[nodiscard]] std::vector<std::uint8_t> sign_der(const Hash256& message_hash) const;

private:
    explicit Secp256k1Key(std::array<std::uint8_t, 32> secret);
    std::array<std::uint8_t, 32> secret_{};
};

[[nodiscard]] bool verify_ecdsa_der(
    const Hash256& message_hash,
    std::span<const std::uint8_t> der_signature,
    std::span<const std::uint8_t> public_key
);

[[nodiscard]] bool valid_secret_key(std::span<const std::uint8_t> secret);
[[nodiscard]] std::array<std::uint8_t, 32> add_secret_tweak(
    std::span<const std::uint8_t> secret,
    std::span<const std::uint8_t> tweak
);
[[nodiscard]] const char* crypto_backend_name() noexcept;

[[nodiscard]] bool valid_compressed_public_key(std::span<const std::uint8_t> public_key);

} // namespace bincoin
