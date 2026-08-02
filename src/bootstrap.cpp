#include "bootstrap.hpp"

#include "params.hpp"
#include "platform.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#ifndef _WIN32
#include <netdb.h>
#endif
#include <stdexcept>
#include <string>
#include <utility>
#ifndef _WIN32
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace bincoin {
namespace {

std::int64_t now_seconds() {
    return static_cast<std::int64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

bool endpoint_equal(const PeerEndpoint& left, const PeerEndpoint& right) {
    return left.host == right.host && left.port == right.port;
}

bool public_ipv4(const in_addr& address) {
    const std::uint32_t host = ntohl(address.s_addr);
    const std::uint8_t a = static_cast<std::uint8_t>((host >> 24U) & 0xffU);
    const std::uint8_t b = static_cast<std::uint8_t>((host >> 16U) & 0xffU);
    const std::uint8_t c = static_cast<std::uint8_t>((host >> 8U) & 0xffU);

    if (a == 0 || a == 10 || a == 127 || a >= 224) return false;
    if (a == 100 && b >= 64 && b <= 127) return false; // CGNAT
    if (a == 169 && b == 254) return false;
    if (a == 172 && b >= 16 && b <= 31) return false;
    if (a == 192 && b == 168) return false;
    if (a == 198 && (b == 18 || b == 19)) return false;
    if (a == 192 && b == 0 && c == 2) return false;
    if (a == 198 && b == 51 && c == 100) return false;
    if (a == 203 && b == 0 && c == 113) return false;
    return true;
}

bool public_ipv6(const in6_addr& address) {
    if (IN6_IS_ADDR_UNSPECIFIED(&address) || IN6_IS_ADDR_LOOPBACK(&address) ||
        IN6_IS_ADDR_MULTICAST(&address) || IN6_IS_ADDR_LINKLOCAL(&address)) return false;
    const auto* bytes = address.s6_addr;
    if ((bytes[0] & 0xfeU) == 0xfcU) return false; // unique-local fc00::/7
    if (bytes[0] == 0x20U && bytes[1] == 0x01U && bytes[2] == 0x0dU && bytes[3] == 0xb8U) {
        return false; // documentation 2001:db8::/32
    }
    return true;
}

bool public_sockaddr(const sockaddr* address) {
    if (address->sa_family == AF_INET) {
        return public_ipv4(reinterpret_cast<const sockaddr_in*>(address)->sin_addr);
    }
    if (address->sa_family == AF_INET6) {
        return public_ipv6(reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr);
    }
    return false;
}

} // namespace

std::vector<PeerEndpoint> resolve_dns_seed(
    const std::string_view hostname,
    const std::uint16_t port
) {
    if (hostname.empty() || port == 0) return {};
    initialize_socket_runtime();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_ADDRCONFIG;

    addrinfo* raw = nullptr;
    const std::string host(hostname);
    const std::string service = std::to_string(port);
    const int result = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &raw);
    if (result != 0) return {};

    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(raw, ::freeaddrinfo);
    std::vector<PeerEndpoint> endpoints;
    for (const addrinfo* current = addresses.get(); current != nullptr; current = current->ai_next) {
        if (!current->ai_addr || !public_sockaddr(current->ai_addr)) continue;
        std::array<char, NI_MAXHOST> numeric{};
        if (::getnameinfo(
                current->ai_addr,
                current->ai_addrlen,
                numeric.data(),
                static_cast<SocketLength>(numeric.size()),
                nullptr,
                0,
                NI_NUMERICHOST) != 0) {
            continue;
        }
        PeerEndpoint endpoint{.host = numeric.data(), .port = port};
        if (std::none_of(endpoints.begin(), endpoints.end(), [&](const PeerEndpoint& existing) {
                return endpoint_equal(existing, endpoint);
            })) {
            endpoints.push_back(std::move(endpoint));
        }
    }
    return endpoints;
}

BootstrapResult bootstrap_testnet_seeds(
    PeerStore& peer_store,
    const SeedResolver& resolver
) {
    BootstrapResult result;
    const std::int64_t now = now_seconds();

    for (const CompiledSeedEndpoint& seed : TESTNET_FIXED_SEEDS) {
        if (peer_store.add_if_missing(PeerRecord{
                .endpoint = PeerEndpoint{.host = std::string(seed.host), .port = seed.port},
                .services = 1,
                .last_seen = now,
                .last_success = 0,
                .failures = 0,
                .manual = false,
            })) {
            ++result.fixed_entries_added;
        }
    }

    for (const std::string_view hostname : TESTNET_DNS_SEEDS) {
        const auto endpoints = resolver(hostname, 26001);
        if (!endpoints.empty()) ++result.dns_names_resolved;
        for (const PeerEndpoint& endpoint : endpoints) {
            if (peer_store.add_if_missing(PeerRecord{
                    .endpoint = endpoint,
                    .services = 1,
                    .last_seen = now,
                    .last_success = 0,
                    .failures = 0,
                    .manual = false,
                })) {
                ++result.dns_entries_added;
            }
        }
    }

    peer_store.save();
    return result;
}

BootstrapResult bootstrap_testnet_seeds(PeerStore& peer_store) {
    return bootstrap_testnet_seeds(peer_store, resolve_dns_seed);
}

} // namespace bincoin
