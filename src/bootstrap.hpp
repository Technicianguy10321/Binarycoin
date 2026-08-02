#pragma once

#include "peer_store.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace bincoin {

using SeedResolver = std::function<std::vector<PeerEndpoint>(std::string_view, std::uint16_t)>;

struct BootstrapResult {
    std::size_t fixed_entries_added{0};
    std::size_t dns_entries_added{0};
    std::size_t dns_names_resolved{0};
};

[[nodiscard]] std::vector<PeerEndpoint> resolve_dns_seed(
    std::string_view hostname,
    std::uint16_t port
);

[[nodiscard]] BootstrapResult bootstrap_testnet_seeds(
    PeerStore& peer_store,
    const SeedResolver& resolver
);

[[nodiscard]] BootstrapResult bootstrap_testnet_seeds(PeerStore& peer_store);

} // namespace bincoin
