#include "fees.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace bincoin {

Amount FeeRate::fee(const std::size_t virtual_bytes) const {
    if (bits_per_kvb_ < 0) throw std::runtime_error("Negative fee rates are invalid");
    if (virtual_bytes == 0 || bits_per_kvb_ == 0) return 0;

    const auto size = static_cast<std::uint64_t>(virtual_bytes);
    const auto rate = static_cast<std::uint64_t>(bits_per_kvb_);
    if (size > (std::numeric_limits<std::uint64_t>::max() - 999U) / rate) {
        throw std::overflow_error("Fee calculation overflow");
    }
    const std::uint64_t rounded = (rate * size + 999U) / 1'000U;
    if (rounded > static_cast<std::uint64_t>(MAX_MONEY)) {
        throw std::overflow_error("Calculated fee exceeds money range");
    }
    return static_cast<Amount>(rounded);
}

FeeQuote TestnetFeeEstimator::estimate(
    const std::size_t virtual_bytes,
    const std::uint32_t confirmation_target,
    const FeeEstimateMode mode
) const {
    if (confirmation_target == 0 || confirmation_target > 1'008) {
        throw std::invalid_argument("Confirmation target must be between 1 and 1008 blocks");
    }

    // Testnet has no meaningful public fee market or confirmation history yet.
    // Keep the Bitcoin-style interface and use an explicit, deterministic fallback.
    FeeRate selected = mode == FeeEstimateMode::Conservative
        ? fallback_conservative()
        : fallback_economical();
    FeeEstimateSource source = FeeEstimateSource::Fallback;

    if (selected < relay_minimum()) {
        selected = relay_minimum();
        source = FeeEstimateSource::RequiredMinimum;
    }

    return FeeQuote{
        .rate = selected,
        .fee = selected.fee(virtual_bytes),
        .confirmation_target = confirmation_target,
        .mode = mode,
        .source = source,
    };
}

FeeEstimateMode parse_fee_mode(const std::string& text) {
    if (text == "economical") return FeeEstimateMode::Economical;
    if (text == "conservative") return FeeEstimateMode::Conservative;
    throw std::invalid_argument("Fee mode must be economical or conservative");
}

std::string fee_mode_name(const FeeEstimateMode mode) {
    return mode == FeeEstimateMode::Conservative ? "conservative" : "economical";
}

std::string fee_source_name(const FeeEstimateSource source) {
    switch (source) {
        case FeeEstimateSource::Historical: return "historical";
        case FeeEstimateSource::Fallback: return "fallback";
        case FeeEstimateSource::MempoolMinimum: return "mempool-minimum";
        case FeeEstimateSource::RequiredMinimum: return "required-minimum";
    }
    throw std::logic_error("Unknown fee estimate source");
}

} // namespace bincoin
