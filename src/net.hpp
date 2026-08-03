#pragma once

#include "block.hpp"
#include "peer_store.hpp"
#include "transaction.hpp"
#include "platform.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace bincoin {

inline constexpr std::uint32_t P2P_PROTOCOL_VERSION = 5;
inline constexpr std::uint16_t DEFAULT_TESTNET_P2P_PORT = 26001;
inline constexpr std::uint16_t RESERVED_TESTNET_RPC_PORT = 25001;
inline constexpr std::uint64_t NODE_NETWORK_SERVICE = 1;
inline constexpr std::size_t MAX_P2P_PAYLOAD = 16U * 1024U * 1024U;
inline constexpr std::size_t MAX_BLOCKS_PER_RESPONSE = 128;
inline constexpr std::size_t MAX_HEADERS_PER_RESPONSE = 2000;
inline constexpr std::size_t MAX_INVENTORY_ITEMS = 500;
inline constexpr std::size_t MAX_ADDRV2_ITEMS = 1000;
inline constexpr std::size_t DEFAULT_MAX_OUTBOUND = 8;
inline constexpr std::size_t DEFAULT_MAX_INBOUND = 32;

struct NetworkPolicy {
    std::size_t max_outbound{DEFAULT_MAX_OUTBOUND};
    std::size_t max_inbound{DEFAULT_MAX_INBOUND};
    std::uint32_t ban_threshold{100};
    std::int64_t ban_seconds{24 * 60 * 60};
    std::chrono::milliseconds ping_interval{5000};
    std::chrono::milliseconds ping_timeout{15000};
    std::chrono::milliseconds message_window{10000};
    std::chrono::milliseconds address_refresh_interval{10000};
    std::size_t max_messages_per_window{2000};
    int socket_timeout_seconds{10};
    // Internal test harness control only. The public binary never exposes this setting.
    bool use_compiled_seeds{true};
    bool allow_loopback_discovery_for_tests{false};
};

struct PeerVersion {
    std::uint32_t protocol_version{P2P_PROTOCOL_VERSION};
    std::uint64_t services{NODE_NETWORK_SERVICE};
    std::int64_t timestamp{0};
    std::uint64_t best_height{0};
    std::string genesis_hash;
    std::string tip_hash;
    std::string chainwork;
    std::uint64_t nonce{0};
    std::string user_agent{"/BinaryCoinTestnetAlpha:0.1.5/"};
};

struct DecodedP2pMessage {
    std::string command;
    std::vector<std::uint8_t> payload;
};

struct HeaderRecord {
    std::uint64_t height{0};
    BlockHeader header;
};

struct SyncResult {
    PeerVersion peer;
    std::uint64_t local_height_before{0};
    std::uint64_t local_height_after{0};
    std::string local_chainwork_before;
    std::string local_chainwork_after;
    std::string candidate_chainwork;
    std::uint64_t fork_height{0};
    std::size_t headers_received{0};
    std::size_t blocks_received{0};
    std::size_t blocks_reused_from_side_store{0};
    std::size_t disconnected_blocks{0};
    std::size_t connected_blocks{0};
    std::size_t resurrected_transactions{0};
    std::size_t mempool_received{0};
    bool reorganized{false};
    bool activated_candidate{false};
};

struct LivePeerInfo {
    std::uint64_t id{0};
    PeerEndpoint endpoint;
    bool inbound{false};
    std::int64_t connected_since{0};
    std::uint32_t protocol_version{0};
    std::uint64_t services{0};
    std::uint64_t starting_height{0};
    std::string user_agent;
    std::string tip_hash;
    std::string chainwork;
};

struct LiveNodeStats {
    std::size_t active_peers{0};
    std::size_t active_inbound{0};
    std::uint64_t accepted_connections{0};
    std::uint64_t outbound_connections{0};
    std::uint64_t relayed_blocks{0};
    std::uint64_t relayed_transactions{0};
    std::uint64_t headers_received{0};
    std::uint64_t addresses_received{0};
    std::uint64_t rejected_messages{0};
    std::uint64_t rejected_connections{0};
    std::uint64_t duplicate_connections{0};
    std::uint64_t self_connections{0};
    std::uint64_t banned_connections{0};
    std::uint64_t timeout_disconnects{0};
    std::uint64_t rate_limited_connections{0};
};

[[nodiscard]] PeerEndpoint parse_peer_endpoint(const std::string& text);
[[nodiscard]] std::vector<std::uint8_t> encode_p2p_frame(
    const std::string& command,
    std::span<const std::uint8_t> payload = {}
);
[[nodiscard]] DecodedP2pMessage decode_p2p_frame(std::span<const std::uint8_t> frame);
[[nodiscard]] std::vector<std::uint8_t> serialize_peer_version(const PeerVersion& version);
[[nodiscard]] PeerVersion deserialize_peer_version(const std::vector<std::uint8_t>& payload);
[[nodiscard]] std::vector<std::uint8_t> serialize_stored_blocks(
    const std::vector<StoredBlock>& blocks,
    std::size_t begin_index,
    std::size_t maximum_count
);
[[nodiscard]] std::vector<StoredBlock> deserialize_stored_blocks(
    const std::vector<std::uint8_t>& payload
);
[[nodiscard]] std::vector<std::string> make_block_locator(const std::vector<StoredBlock>& blocks);

class TestnetP2pServer {
public:
    TestnetP2pServer(
        std::filesystem::path data_directory,
        std::string bind_address,
        std::uint16_t port
    );
    ~TestnetP2pServer();

    TestnetP2pServer(const TestnetP2pServer&) = delete;
    TestnetP2pServer& operator=(const TestnetP2pServer&) = delete;

    void serve_once();
    void serve_forever();
    void request_stop();
    [[nodiscard]] std::uint16_t bound_port() const noexcept;

private:
    std::filesystem::path data_directory_;
    std::string bind_address_;
    std::uint16_t port_{0};
    Socket listen_socket_{INVALID_SOCKET_VALUE};
    std::atomic<bool> stop_requested_{false};

    void handle_client(Socket client_socket);
};

class PersistentTestnetNode {
public:
    PersistentTestnetNode(
        std::filesystem::path data_directory,
        std::string bind_address,
        std::uint16_t port,
        std::vector<PeerEndpoint> outbound_peers = {},
        NetworkPolicy policy = {}
    );
    ~PersistentTestnetNode();

    PersistentTestnetNode(const PersistentTestnetNode&) = delete;
    PersistentTestnetNode& operator=(const PersistentTestnetNode&) = delete;

    void start();
    void request_stop();
    void wait();
    [[nodiscard]] std::uint16_t bound_port() const noexcept;
    [[nodiscard]] std::uint64_t node_nonce() const noexcept;
    [[nodiscard]] LiveNodeStats stats() const noexcept;
    [[nodiscard]] std::vector<LivePeerInfo> peer_info() const;
    [[nodiscard]] std::mutex& data_mutex() noexcept;

private:
    struct SessionThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    struct OutboundThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
        PeerEndpoint endpoint;
    };

    std::filesystem::path data_directory_;
    std::string bind_address_;
    std::uint16_t port_{0};
    std::vector<PeerEndpoint> outbound_peers_;
    NetworkPolicy policy_;
    Socket listen_socket_{INVALID_SOCKET_VALUE};
    std::uint64_t node_nonce_{0};
    std::atomic<bool> started_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::size_t> active_peers_{0};
    std::atomic<std::size_t> active_inbound_{0};
    std::atomic<std::uint64_t> accepted_connections_{0};
    std::atomic<std::uint64_t> outbound_connections_{0};
    std::atomic<std::uint64_t> relayed_blocks_{0};
    std::atomic<std::uint64_t> relayed_transactions_{0};
    std::atomic<std::uint64_t> headers_received_{0};
    std::atomic<std::uint64_t> addresses_received_{0};
    std::atomic<std::uint64_t> rejected_messages_{0};
    std::atomic<std::uint64_t> rejected_connections_{0};
    std::atomic<std::uint64_t> duplicate_connections_{0};
    std::atomic<std::uint64_t> self_connections_{0};
    std::atomic<std::uint64_t> banned_connections_{0};
    std::atomic<std::uint64_t> timeout_disconnects_{0};
    std::atomic<std::uint64_t> rate_limited_connections_{0};
    std::mutex data_mutex_;
    std::mutex peer_store_mutex_;
    std::mutex ban_store_mutex_;
    std::mutex session_threads_mutex_;
    std::mutex session_state_mutex_;
    std::mutex outbound_threads_mutex_;
    mutable std::mutex local_endpoints_mutex_;
    std::thread accept_thread_;
    std::thread outbound_manager_thread_;
    std::vector<OutboundThread> outbound_threads_;
    std::vector<SessionThread> session_threads_;
    std::set<std::string> outbound_targets_;
    std::set<std::string> local_endpoint_keys_;
    std::map<std::string, std::chrono::steady_clock::time_point> outbound_retry_after_;
    std::set<std::uint64_t> active_peer_nonces_;
    std::set<Socket> active_sockets_;
    std::atomic<std::uint64_t> next_peer_id_{0};
    mutable std::mutex live_peers_mutex_;
    std::map<std::uint64_t, LivePeerInfo> live_peers_;

    void accept_loop();
    void outbound_manager_loop();
    void outbound_loop(PeerEndpoint endpoint);
    void start_outbound_target(PeerEndpoint endpoint);
    void reap_outbound_threads();
    void start_session(Socket socket, bool outbound, PeerEndpoint endpoint);
    void session_loop(Socket socket, bool outbound, PeerEndpoint endpoint);
    void remember_peer(const PeerRecord& peer);
    void remember_local_endpoint(const PeerEndpoint& endpoint);
    [[nodiscard]] bool is_local_endpoint(const PeerEndpoint& endpoint) const;
    void reap_sessions();
    void ban_peer(const PeerEndpoint& endpoint, std::uint32_t score, const std::string& reason);
    [[nodiscard]] bool peer_is_banned(const PeerEndpoint& endpoint);
    [[nodiscard]] std::vector<PeerEndpoint> initial_outbound_peers();
};

[[nodiscard]] SyncResult sync_from_peer(
    const std::filesystem::path& data_directory,
    const PeerEndpoint& endpoint
);

} // namespace bincoin
