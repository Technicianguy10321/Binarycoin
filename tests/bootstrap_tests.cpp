#include "bootstrap.hpp"
#include "params.hpp"
#include "peer_store.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        static_assert(bincoin::TESTNET_DNS_SEEDS.size() == 1);
        static_assert(bincoin::TESTNET_FIXED_SEEDS.size() == 1);
        require(bincoin::TESTNET_DNS_SEEDS.front() ==
                    "binarycoin-testnet.ezgateway.net",
                "Compiled DNS seed mismatch");
        require(bincoin::TESTNET_FIXED_SEEDS.front().host ==
                    "binarycoin-testnet.ezgateway.net",
                "Compiled seed endpoint mismatch");
        require(bincoin::TESTNET_FIXED_SEEDS.front().port == 26001,
                "Compiled seed port mismatch");

        const auto directory = std::filesystem::temp_directory_path() /
            "binarycoin-testnet-v011-bootstrap-test";
        std::filesystem::remove_all(directory);

        bincoin::PeerStore store(directory);
        store.load();
        const auto result = bincoin::bootstrap_testnet_seeds(
            store,
            [](const std::string_view hostname, const std::uint16_t port) {
                require(hostname == "binarycoin-testnet.ezgateway.net",
                        "Resolver received wrong DNS seed");
                require(port == 26001, "Resolver received wrong seed port");
                return std::vector<bincoin::PeerEndpoint>{
                    {.host = "8.8.8.8", .port = port},
                    {.host = "2001:4860:4860::8888", .port = port},
                };
            });

        require(result.fixed_entries_added == 1,
                "Compiled seed endpoint was not inserted");
        require(result.dns_entries_added == 2,
                "Resolved DNS addresses were not inserted");
        require(result.dns_names_resolved == 1,
                "DNS seed result was not recorded");

        bincoin::PeerStore reloaded(directory);
        reloaded.load();
        require(reloaded.records().size() == 3,
                "Seed entries were not persisted in peers.dat");

        const auto duplicate = bincoin::bootstrap_testnet_seeds(
            reloaded,
            [](std::string_view, const std::uint16_t port) {
                return std::vector<bincoin::PeerEndpoint>{
                    {.host = "8.8.8.8", .port = port},
                    {.host = "2001:4860:4860::8888", .port = port},
                };
            });
        require(duplicate.fixed_entries_added == 0 &&
                    duplicate.dns_entries_added == 0,
                "Seed bootstrap failed to deduplicate peers");

        require(bincoin::resolve_dns_seed("localhost", 26001).empty(),
                "DNS resolver accepted loopback as a public peer");
        std::filesystem::remove_all(directory);
        std::cout << "BinaryCoin Testnet Alpha v0.1.4 bootstrap tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
