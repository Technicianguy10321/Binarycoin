#pragma once

#include <cstdint>
#include <stdexcept>

namespace bincoin {

using Amount = std::int64_t;

inline constexpr Amount BITS_PER_BIN = 100'000'000;
inline constexpr Amount INITIAL_SUBSIDY = 50 * BITS_PER_BIN;
inline constexpr Amount MAX_MONEY = 21'000'000 * BITS_PER_BIN;
inline constexpr std::uint32_t TESTNET_HALVING_INTERVAL = 210'000;
inline constexpr std::uint32_t COINBASE_MATURITY = 100;

[[nodiscard]] constexpr bool money_range(const Amount value) noexcept {
    return value >= 0 && value <= MAX_MONEY;
}

[[nodiscard]] constexpr Amount block_subsidy(
    const std::uint64_t height,
    const std::uint32_t halving_interval = TESTNET_HALVING_INTERVAL
) {
    if (halving_interval == 0) {
        throw std::invalid_argument("Halving interval cannot be zero");
    }
    const std::uint64_t halvings = height / halving_interval;
    if (halvings >= 63) return 0;
    return INITIAL_SUBSIDY >> halvings;
}

} // namespace bincoin
