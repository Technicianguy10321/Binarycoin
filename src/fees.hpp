#pragma once

#include "amount.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace bincoin {

enum class FeeEstimateMode {
    Economical,
    Conservative,
};

enum class FeeEstimateSource {
    Historical,
    Fallback,
    MempoolMinimum,
    RequiredMinimum,
};

class FeeRate {
public:
    explicit constexpr FeeRate(const Amount bits_per_kvb = 0) : bits_per_kvb_(bits_per_kvb) {}

    [[nodiscard]] Amount fee(std::size_t virtual_bytes) const;
    [[nodiscard]] constexpr Amount bits_per_kvb() const noexcept { return bits_per_kvb_; }

    friend constexpr bool operator<(const FeeRate lhs, const FeeRate rhs) noexcept {
        return lhs.bits_per_kvb_ < rhs.bits_per_kvb_;
    }

private:
    Amount bits_per_kvb_;
};

struct FeeQuote {
    FeeRate rate;
    Amount fee{0};
    std::uint32_t confirmation_target{0};
    FeeEstimateMode mode{FeeEstimateMode::Economical};
    FeeEstimateSource source{FeeEstimateSource::Fallback};
};

class TestnetFeeEstimator {
public:
    [[nodiscard]] FeeQuote estimate(
        std::size_t virtual_bytes,
        std::uint32_t confirmation_target,
        FeeEstimateMode mode
    ) const;

    [[nodiscard]] static constexpr FeeRate relay_minimum() noexcept { return FeeRate(1'000); }
    [[nodiscard]] static constexpr FeeRate fallback_economical() noexcept { return FeeRate(1'000); }
    [[nodiscard]] static constexpr FeeRate fallback_conservative() noexcept { return FeeRate(2'000); }
};

FeeEstimateMode parse_fee_mode(const std::string& text);
std::string fee_mode_name(FeeEstimateMode mode);
std::string fee_source_name(FeeEstimateSource source);

} // namespace bincoin
