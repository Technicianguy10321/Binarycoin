#include "chain.hpp"
#include "net.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void wait_for_condition(
    const std::function<bool()>& condition,
    const std::chrono::milliseconds timeout,
    const char* failure_message
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return;
        std::this_thread::sleep_for(25ms);
    }
    throw std::runtime_error(failure_message);
}

bool has_outbound_to(const bincoin::PersistentTestnetNode& node, const std::uint16_t port) {
    for (const bincoin::LivePeerInfo& peer : node.peer_info()) {
        if (!peer.inbound && peer.endpoint.host == "127.0.0.1" && peer.endpoint.port == port) {
            return true;
        }
    }
    return false;
}

void stop_node(bincoin::PersistentTestnetNode& node) {
    node.request_stop();
    node.wait();
}
} // namespace

int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() /
            "binarycoin-v015-peer-discovery";
        std::filesystem::remove_all(root);
        const auto seed_dir = root / "seed";
        const auto client_a_dir = root / "client-a";
        const auto client_b_dir = root / "client-b";
        bincoin::TestnetChain(seed_dir).initialize();
        bincoin::TestnetChain(client_a_dir).initialize();
        bincoin::TestnetChain(client_b_dir).initialize();

        bincoin::NetworkPolicy seed_policy;
        seed_policy.use_compiled_seeds = false;
        seed_policy.max_outbound = 0;
        seed_policy.allow_loopback_discovery_for_tests = true;
        seed_policy.address_refresh_interval = 200ms;
        seed_policy.ping_interval = 1s;
        seed_policy.ping_timeout = 3s;

        bincoin::PersistentTestnetNode seed(seed_dir, "127.0.0.1", 0, {}, seed_policy);
        seed.start();

        bincoin::NetworkPolicy client_policy = seed_policy;
        client_policy.max_outbound = 4;
        const bincoin::PeerEndpoint seed_endpoint{
            .host = "127.0.0.1",
            .port = seed.bound_port(),
        };

        bincoin::PersistentTestnetNode client_a(
            client_a_dir, "127.0.0.1", 0, {seed_endpoint}, client_policy);
        client_a.start();
        wait_for_condition([&] { return seed.stats().active_inbound >= 1; }, 5s,
                           "First client did not connect to the seed");

        bincoin::PersistentTestnetNode client_b(
            client_b_dir, "127.0.0.1", 0, {seed_endpoint}, client_policy);
        client_b.start();
        wait_for_condition([&] { return seed.stats().active_inbound >= 2; }, 5s,
                           "Second client did not connect to the seed");

        wait_for_condition([&] {
            return has_outbound_to(client_a, client_b.bound_port()) ||
                   has_outbound_to(client_b, client_a.bound_port());
        }, 10s, "Clients did not discover and dial each other through the seed");

        require(client_a.stats().addresses_received > 0,
                "First client did not receive gossiped addresses");
        require(client_b.stats().addresses_received > 0,
                "Second client did not receive gossiped addresses");

        stop_node(client_b);
        stop_node(client_a);
        stop_node(seed);
        std::filesystem::remove_all(root);
        std::cout << "BinaryCoin Testnet Alpha v0.1.5 peer discovery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
