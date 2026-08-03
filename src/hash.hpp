#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bincoin {

using Hash256 = std::array<std::uint8_t, 32>;

Hash256 sha256(std::span<const std::uint8_t> data);
Hash256 sha256d(std::span<const std::uint8_t> data);
Hash256 sha256d(const std::string& text);

std::string hash_to_display_hex(const Hash256& raw_digest);
Hash256 display_hex_to_raw_digest(const std::string& display_hex);
std::array<std::uint8_t, 32> display_hex_to_wire_bytes(const std::string& display_hex);
std::string wire_bytes_to_display_hex(const std::array<std::uint8_t, 32>& wire_bytes);

std::vector<std::uint8_t> hex_to_bytes(const std::string& hex);
std::string bytes_to_hex(std::span<const std::uint8_t> bytes);

} // namespace bincoin
