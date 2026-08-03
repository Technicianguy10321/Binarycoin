#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace bincoin {

inline constexpr std::array<std::uint8_t, 4> MAINNET_MESSAGE_START{'B', 'I', 'T', 'S'};
inline constexpr std::array<std::uint8_t, 4> TESTNET_MESSAGE_START{'T', 'B', 'I', 'T'};

struct CompiledSeedEndpoint {
    std::string_view host;
    std::uint16_t port;
};

// Testnet bootstrap is a consensus-adjacent network parameter. The public CLI
// deliberately provides no switch that disables or replaces these entries.
inline constexpr std::array<std::string_view, 1> TESTNET_DNS_SEEDS{
    "binarycoin-testnet.ezgateway.net",
};

// The first alpha uses the same permanent hostname as its compiled bootstrap
// endpoint. DNS answers are converted to numeric endpoints and persisted in
// peers.dat, so a successful first bootstrap leaves a durable local peer cache.
inline constexpr std::array<CompiledSeedEndpoint, 1> TESTNET_FIXED_SEEDS{{
    {"binarycoin-testnet.ezgateway.net", 26001},
}};

} // namespace bincoin
