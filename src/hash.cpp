#include "hash.hpp"

#include <algorithm>
#include <iomanip>
#include <openssl/evp.h>
#include <sstream>
#include <stdexcept>

namespace bincoin {
namespace {

Hash256 sha256_once(std::span<const std::uint8_t> data) {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        throw std::runtime_error("EVP_MD_CTX_new failed");
    }

    Hash256 digest{};
    unsigned int digest_length = 0;

    const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                    EVP_DigestUpdate(context, data.data(), data.size()) == 1 &&
                    EVP_DigestFinal_ex(context, digest.data(), &digest_length) == 1;

    EVP_MD_CTX_free(context);

    if (!ok || digest_length != digest.size()) {
        throw std::runtime_error("SHA-256 operation failed");
    }

    return digest;
}

int hex_value(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    throw std::invalid_argument("Invalid hexadecimal character");
}

} // namespace

Hash256 sha256(const std::span<const std::uint8_t> data) {
    return sha256_once(data);
}

Hash256 sha256d(std::span<const std::uint8_t> data) {
    const Hash256 first = sha256_once(data);
    return sha256_once(first);
}

Hash256 sha256d(const std::string& text) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(text.data());
    return sha256d(std::span<const std::uint8_t>(begin, text.size()));
}

std::vector<std::uint8_t> hex_to_bytes(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        throw std::invalid_argument("Hexadecimal input must contain an even number of characters");
    }

    std::vector<std::uint8_t> result(hex.size() / 2);
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(
            (hex_value(hex[index * 2]) << 4) | hex_value(hex[index * 2 + 1]));
    }
    return result;
}

std::string bytes_to_hex(std::span<const std::uint8_t> bytes) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const std::uint8_t byte : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

std::string hash_to_display_hex(const Hash256& raw_digest) {
    Hash256 reversed = raw_digest;
    std::reverse(reversed.begin(), reversed.end());
    return bytes_to_hex(reversed);
}

Hash256 display_hex_to_raw_digest(const std::string& display_hex) {
    const std::vector<std::uint8_t> bytes = hex_to_bytes(display_hex);
    if (bytes.size() != 32) {
        throw std::invalid_argument("A 256-bit hash must contain exactly 64 hexadecimal characters");
    }

    Hash256 raw{};
    std::reverse_copy(bytes.begin(), bytes.end(), raw.begin());
    return raw;
}

std::array<std::uint8_t, 32> display_hex_to_wire_bytes(const std::string& display_hex) {
    return display_hex_to_raw_digest(display_hex);
}

std::string wire_bytes_to_display_hex(const std::array<std::uint8_t, 32>& wire_bytes) {
    return hash_to_display_hex(wire_bytes);
}

} // namespace bincoin
