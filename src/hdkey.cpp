#include "hdkey.hpp"

#include "hash.hpp"

#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace bincoin {
namespace {

std::array<std::uint8_t, 64> hmac_sha512(
    const std::span<const std::uint8_t> key,
    const std::span<const std::uint8_t> data
) {
    std::array<std::uint8_t, 64> output{};
    unsigned int length = 0;
    if (HMAC(
            EVP_sha512(), key.data(), static_cast<int>(key.size()),
            data.data(), data.size(), output.data(), &length) == nullptr || length != output.size()) {
        throw std::runtime_error("HMAC-SHA512 failed");
    }
    return output;
}

void append_u32_be(std::vector<std::uint8_t>& output, const std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

} // namespace

Secp256k1Key HdPrivateKey::key() const {
    return Secp256k1Key::from_secret_bytes(secret);
}

HdPrivateKey HdPrivateKey::derive(const std::uint32_t child) const {
    std::vector<std::uint8_t> data;
    if ((child & HARDENED) != 0U) {
        data.push_back(0U);
        data.insert(data.end(), secret.begin(), secret.end());
    } else {
        const auto public_key = key().compressed_public_key();
        data.insert(data.end(), public_key.begin(), public_key.end());
    }
    append_u32_be(data, child);
    const auto digest = hmac_sha512(chain_code, data);
    std::array<std::uint8_t, 32> tweak{};
    std::copy_n(digest.begin(), 32, tweak.begin());
    const auto child_secret = add_secret_tweak(secret, tweak);

    HdPrivateKey result;
    result.secret = child_secret;
    std::copy_n(digest.begin() + 32, 32, result.chain_code.begin());
    result.depth = static_cast<std::uint8_t>(depth + 1U);
    result.child_number = child;
    return result;
}

HdPrivateKey bip32_master(const std::span<const std::uint8_t> seed) {
    static constexpr std::array<std::uint8_t, 12> key{
        'B','i','t','c','o','i','n',' ','s','e','e','d'
    };
    const auto digest = hmac_sha512(key, seed);
    HdPrivateKey result;
    std::copy_n(digest.begin(), 32, result.secret.begin());
    std::copy_n(digest.begin() + 32, 32, result.chain_code.begin());
    if (!valid_secret_key(result.secret)) throw std::runtime_error("BIP32 master key is invalid");
    return result;
}

HdPrivateKey derive_testnet_wallet_key(
    const HdPrivateKey& master,
    const std::uint32_t branch,
    const std::uint32_t index
) {
    // m/44'/1'/0'/branch/index. Coin type 1 is reserved for test networks.
    return master
        .derive(44U | HARDENED)
        .derive(1U | HARDENED)
        .derive(0U | HARDENED)
        .derive(branch)
        .derive(index);
}

std::string master_fingerprint(const HdPrivateKey& master) {
    const auto digest = sha256(master.key().compressed_public_key());
    return bytes_to_hex(std::span<const std::uint8_t>(digest.data(), 4));
}

} // namespace bincoin
