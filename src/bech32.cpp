#include "bech32.hpp"

#include "key.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <string_view>

namespace bincoin {
namespace {

constexpr std::string_view CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
constexpr std::string_view HRP = "tbin";

std::uint32_t polymod(const std::vector<std::uint8_t>& values) {
    static constexpr std::array<std::uint32_t, 5> GENERATOR{
        0x3b6a57b2U, 0x26508e6dU, 0x1ea119faU, 0x3d4233ddU, 0x2a1462b3U
    };
    std::uint32_t checksum = 1;
    for (const auto value : values) {
        const std::uint8_t top = static_cast<std::uint8_t>(checksum >> 25U);
        checksum = ((checksum & 0x1ffffffU) << 5U) ^ value;
        for (std::size_t index = 0; index < GENERATOR.size(); ++index) {
            if (((top >> index) & 1U) != 0U) checksum ^= GENERATOR[index];
        }
    }
    return checksum;
}

std::vector<std::uint8_t> hrp_expand(const std::string_view hrp) {
    std::vector<std::uint8_t> output;
    output.reserve(hrp.size() * 2 + 1);
    for (const char character : hrp) output.push_back(static_cast<std::uint8_t>(character >> 5));
    output.push_back(0);
    for (const char character : hrp) output.push_back(static_cast<std::uint8_t>(character & 31));
    return output;
}

std::vector<std::uint8_t> checksum(const std::string_view hrp, const std::vector<std::uint8_t>& data) {
    auto values = hrp_expand(hrp);
    values.insert(values.end(), data.begin(), data.end());
    values.insert(values.end(), 6, 0U);
    const std::uint32_t value = polymod(values) ^ 1U;
    std::vector<std::uint8_t> result(6);
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>((value >> (5U * (5U - index))) & 31U);
    }
    return result;
}

std::vector<std::uint8_t> convert_bits(
    const std::span<const std::uint8_t> input,
    const int from_bits,
    const int to_bits,
    const bool pad
) {
    std::uint32_t accumulator = 0;
    int bits = 0;
    const std::uint32_t maximum = (1U << to_bits) - 1U;
    const std::uint32_t maximum_accumulator = (1U << (from_bits + to_bits - 1)) - 1U;
    std::vector<std::uint8_t> output;
    for (const auto value : input) {
        if ((value >> from_bits) != 0U) throw std::invalid_argument("Bech32 data value exceeds source width");
        accumulator = ((accumulator << from_bits) | value) & maximum_accumulator;
        bits += from_bits;
        while (bits >= to_bits) {
            bits -= to_bits;
            output.push_back(static_cast<std::uint8_t>((accumulator >> bits) & maximum));
        }
    }
    if (pad) {
        if (bits != 0) output.push_back(static_cast<std::uint8_t>((accumulator << (to_bits - bits)) & maximum));
    } else if (bits >= from_bits || ((accumulator << (to_bits - bits)) & maximum) != 0U) {
        throw std::invalid_argument("Invalid Bech32 padding");
    }
    return output;
}

} // namespace

std::string encode_testnet_address(const std::span<const std::uint8_t> compressed_public_key) {
    if (!valid_compressed_public_key(compressed_public_key)) throw std::invalid_argument("Invalid address public key");
    std::vector<std::uint8_t> data{0U};
    const auto converted = convert_bits(compressed_public_key, 8, 5, true);
    data.insert(data.end(), converted.begin(), converted.end());
    const auto check = checksum(HRP, data);
    std::string result(HRP);
    result.push_back('1');
    for (const auto value : data) result.push_back(CHARSET[value]);
    for (const auto value : check) result.push_back(CHARSET[value]);
    return result;
}

std::vector<std::uint8_t> decode_testnet_address(const std::string& address) {
    if (address.size() < 8 || address.size() > 90) throw std::invalid_argument("Invalid BinaryCoin address length");
    bool lower = false;
    bool upper = false;
    for (const char raw_character : address) {
        const auto character = static_cast<unsigned char>(raw_character);
        lower = lower || std::islower(character) != 0;
        upper = upper || std::isupper(character) != 0;
    }
    if (lower && upper) throw std::invalid_argument("Mixed-case Bech32 address");
    std::string normalized = address;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto separator = normalized.rfind('1');
    if (separator == std::string::npos || normalized.substr(0, separator) != HRP ||
        separator + 7 > normalized.size()) {
        throw std::invalid_argument("Wrong or missing BinaryCoin testnet address prefix");
    }
    std::vector<std::uint8_t> values;
    values.reserve(normalized.size() - separator - 1);
    for (std::size_t index = separator + 1; index < normalized.size(); ++index) {
        const auto found = CHARSET.find(normalized[index]);
        if (found == std::string_view::npos) throw std::invalid_argument("Invalid Bech32 character");
        values.push_back(static_cast<std::uint8_t>(found));
    }
    auto expanded = hrp_expand(HRP);
    expanded.insert(expanded.end(), values.begin(), values.end());
    if (polymod(expanded) != 1U) throw std::invalid_argument("BinaryCoin address checksum mismatch");
    values.resize(values.size() - 6);
    if (values.empty() || values.front() != 0U) throw std::invalid_argument("Unsupported BinaryCoin address version");
    const auto public_key = convert_bits(
        std::span<const std::uint8_t>(values.data() + 1, values.size() - 1), 5, 8, false);
    if (!valid_compressed_public_key(public_key)) throw std::invalid_argument("Address contains an invalid public key");
    return public_key;
}

bool valid_testnet_address(const std::string& address) noexcept {
    try {
        (void)decode_testnet_address(address);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace bincoin
