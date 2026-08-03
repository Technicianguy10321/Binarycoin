#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bincoin {

[[nodiscard]] std::string encode_testnet_address(std::span<const std::uint8_t> compressed_public_key);
[[nodiscard]] std::vector<std::uint8_t> decode_testnet_address(const std::string& address);
[[nodiscard]] bool valid_testnet_address(const std::string& address) noexcept;

} // namespace bincoin
