#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bincoin {

void append_u32_le(std::vector<std::uint8_t>& output, std::uint32_t value);
void append_i32_le(std::vector<std::uint8_t>& output, std::int32_t value);
void append_u64_le(std::vector<std::uint8_t>& output, std::uint64_t value);
void append_i64_le(std::vector<std::uint8_t>& output, std::int64_t value);
void append_compact_size(std::vector<std::uint8_t>& output, std::uint64_t value);

std::uint32_t read_u32_le(std::span<const std::uint8_t> input, std::size_t& offset);
std::int32_t read_i32_le(std::span<const std::uint8_t> input, std::size_t& offset);
std::uint64_t read_u64_le(std::span<const std::uint8_t> input, std::size_t& offset);
std::int64_t read_i64_le(std::span<const std::uint8_t> input, std::size_t& offset);
std::uint64_t read_compact_size(std::span<const std::uint8_t> input, std::size_t& offset);
std::vector<std::uint8_t> read_bytes(
    std::span<const std::uint8_t> input,
    std::size_t& offset,
    std::size_t count
);

} // namespace bincoin
