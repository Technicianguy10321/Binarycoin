#include "serialize.hpp"

#include <limits>
#include <stdexcept>

namespace bincoin {
namespace {

void require_available(
    const std::span<const std::uint8_t> input,
    const std::size_t offset,
    const std::size_t count
) {
    if (offset > input.size() || count > input.size() - offset) {
        throw std::runtime_error("Unexpected end of serialized data");
    }
}

} // namespace

void append_u32_le(std::vector<std::uint8_t>& output, const std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_i32_le(std::vector<std::uint8_t>& output, const std::int32_t value) {
    append_u32_le(output, static_cast<std::uint32_t>(value));
}

void append_u64_le(std::vector<std::uint8_t>& output, const std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_i64_le(std::vector<std::uint8_t>& output, const std::int64_t value) {
    append_u64_le(output, static_cast<std::uint64_t>(value));
}

void append_compact_size(std::vector<std::uint8_t>& output, const std::uint64_t value) {
    if (value < 253) {
        output.push_back(static_cast<std::uint8_t>(value));
    } else if (value <= std::numeric_limits<std::uint16_t>::max()) {
        output.push_back(253);
        output.push_back(static_cast<std::uint8_t>(value));
        output.push_back(static_cast<std::uint8_t>(value >> 8U));
    } else if (value <= std::numeric_limits<std::uint32_t>::max()) {
        output.push_back(254);
        append_u32_le(output, static_cast<std::uint32_t>(value));
    } else {
        output.push_back(255);
        append_u64_le(output, value);
    }
}

std::uint32_t read_u32_le(const std::span<const std::uint8_t> input, std::size_t& offset) {
    require_available(input, offset, 4);
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(input[offset++]) << (8U * index);
    }
    return value;
}

std::int32_t read_i32_le(const std::span<const std::uint8_t> input, std::size_t& offset) {
    return static_cast<std::int32_t>(read_u32_le(input, offset));
}

std::uint64_t read_u64_le(const std::span<const std::uint8_t> input, std::size_t& offset) {
    require_available(input, offset, 8);
    std::uint64_t value = 0;
    for (unsigned int index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(input[offset++]) << (8U * index);
    }
    return value;
}

std::int64_t read_i64_le(const std::span<const std::uint8_t> input, std::size_t& offset) {
    return static_cast<std::int64_t>(read_u64_le(input, offset));
}

std::uint64_t read_compact_size(const std::span<const std::uint8_t> input, std::size_t& offset) {
    require_available(input, offset, 1);
    const std::uint8_t marker = input[offset++];
    if (marker < 253) return marker;

    if (marker == 253) {
        require_available(input, offset, 2);
        const std::uint64_t value = static_cast<std::uint64_t>(input[offset]) |
                                    (static_cast<std::uint64_t>(input[offset + 1]) << 8U);
        offset += 2;
        if (value < 253) throw std::runtime_error("Non-canonical CompactSize value");
        return value;
    }
    if (marker == 254) {
        const std::uint64_t value = read_u32_le(input, offset);
        if (value <= std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("Non-canonical CompactSize value");
        }
        return value;
    }

    const std::uint64_t value = read_u64_le(input, offset);
    if (value <= std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Non-canonical CompactSize value");
    }
    return value;
}

std::vector<std::uint8_t> read_bytes(
    const std::span<const std::uint8_t> input,
    std::size_t& offset,
    const std::size_t count
) {
    require_available(input, offset, count);
    std::vector<std::uint8_t> output(input.begin() + static_cast<std::ptrdiff_t>(offset),
                                     input.begin() + static_cast<std::ptrdiff_t>(offset + count));
    offset += count;
    return output;
}

} // namespace bincoin
