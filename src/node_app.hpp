#pragma once

#include "net.hpp"

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace bincoin {

struct NodeAppOptions {
    std::filesystem::path data_directory;
    std::string p2p_bind{"0.0.0.0"};
    std::uint16_t p2p_port{DEFAULT_TESTNET_P2P_PORT};
    std::string rpc_bind{"127.0.0.1"};
    std::uint16_t rpc_port{RESERVED_TESTNET_RPC_PORT};
    std::vector<PeerEndpoint> manual_peers;
    NetworkPolicy network_policy;
    bool daemon{false};
    bool daemon_wait{false};
    bool daemon_child{false};
    bool print_to_console{true};
    std::set<std::string> debug_categories;
};

int launch_node(const NodeAppOptions& options);

} // namespace bincoin
