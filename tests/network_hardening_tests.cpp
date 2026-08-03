#include "ban_store.hpp"
#include "chain.hpp"
#include "net.hpp"
#include "params.hpp"
#include "serialize.hpp"
#include "wallet.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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
        std::this_thread::sleep_for(20ms);
    }
    throw std::runtime_error(failure_message);
}

int connect_loopback(const std::uint16_t port) {
    const int socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket < 0) throw std::runtime_error("Unable to create test socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket);
        throw std::runtime_error("Unable to connect test socket");
    }
    return socket;
}

void send_all(const int socket, const std::vector<std::uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t result = ::send(
            socket,
            bytes.data() + static_cast<std::ptrdiff_t>(sent),
            bytes.size() - sent,
            MSG_NOSIGNAL);
        if (result <= 0) throw std::runtime_error("Test socket send failed");
        sent += static_cast<std::size_t>(result);
    }
}

std::vector<std::uint8_t> receive_exact(const int socket, const std::size_t count) {
    std::vector<std::uint8_t> bytes(count);
    std::size_t received = 0;
    while (received < count) {
        const ssize_t result = ::recv(
            socket,
            bytes.data() + static_cast<std::ptrdiff_t>(received),
            count - received,
            0);
        if (result <= 0) throw std::runtime_error("Test socket receive failed");
        received += static_cast<std::size_t>(result);
    }
    return bytes;
}

bincoin::DecodedP2pMessage receive_message(const int socket) {
    auto frame = receive_exact(socket, 24);
    const std::uint32_t payload_size =
        static_cast<std::uint32_t>(frame[16]) |
        (static_cast<std::uint32_t>(frame[17]) << 8U) |
        (static_cast<std::uint32_t>(frame[18]) << 16U) |
        (static_cast<std::uint32_t>(frame[19]) << 24U);
    const auto payload = receive_exact(socket, payload_size);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return bincoin::decode_p2p_frame(frame);
}

void client_handshake(
    const int socket,
    const std::filesystem::path& datadir,
    const std::uint64_t nonce
) {
    bincoin::TestnetChain chain(datadir);
    chain.load();
    const auto now = static_cast<std::int64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    const bincoin::PeerVersion version{
        .protocol_version = bincoin::P2P_PROTOCOL_VERSION,
        .services = bincoin::NODE_NETWORK_SERVICE,
        .timestamp = now,
        .best_height = chain.tip().height,
        .genesis_hash = bincoin::TESTNET_GENESIS_HASH,
        .tip_hash = chain.tip().block.header.hash_hex(),
        .chainwork = chain.chainwork_hex(),
        .nonce = nonce,
        .user_agent = "/BinaryCoinTestnetAlphaHardening:0.1.5/",
    };
    send_all(socket, bincoin::encode_p2p_frame("version", bincoin::serialize_peer_version(version)));
    const auto remote_version = receive_message(socket);
    require(remote_version.command == "version", "Server did not answer test version");
    (void)bincoin::deserialize_peer_version(remote_version.payload);
    send_all(socket, bincoin::encode_p2p_frame("verack"));
    const auto verack = receive_message(socket);
    require(verack.command == "verack" && verack.payload.empty(), "Server verack is invalid");
}

void stop_node(bincoin::PersistentTestnetNode& node) {
    node.request_stop();
    node.wait();
}

bincoin::SyncResult sync_once(
    const std::filesystem::path& source,
    const std::filesystem::path& destination
) {
    bincoin::TestnetP2pServer server(source, "127.0.0.1", 0);
    std::thread server_thread([&] { server.serve_once(); });
    try {
        const auto result = bincoin::sync_from_peer(
            destination,
            bincoin::PeerEndpoint{.host = "127.0.0.1", .port = server.bound_port()});
        server_thread.join();
        return result;
    } catch (...) {
        server.request_stop();
        if (server_thread.joinable()) server_thread.join();
        throw;
    }
}

} // namespace

int main() {
    try {
        // Shared parser round-trip and deterministic malformed-frame fuzzing.
        const std::vector<std::uint8_t> payload{0, 1, 2, 3, 4, 5};
        const auto valid_frame = bincoin::encode_p2p_frame("ping", payload);
        const auto valid_message = bincoin::decode_p2p_frame(valid_frame);
        require(valid_message.command == "ping" && valid_message.payload == payload,
                "P2P frame round-trip failed");

        std::mt19937_64 generator(0xB10C01A5ULL);
        std::uniform_int_distribution<std::size_t> length_distribution(0, 512);
        std::uniform_int_distribution<unsigned int> byte_distribution(0, 255);
        std::size_t fuzz_rejections = 0;
        for (std::size_t iteration = 0; iteration < 25'000; ++iteration) {
            std::vector<std::uint8_t> frame(length_distribution(generator));
            for (std::uint8_t& byte : frame) {
                byte = static_cast<std::uint8_t>(byte_distribution(generator));
            }
            try {
                const auto decoded = bincoin::decode_p2p_frame(frame);
                require(!decoded.command.empty(), "Fuzzer accepted an empty command");
                require(frame.size() == 24 + decoded.payload.size(),
                        "Fuzzer accepted an inconsistent frame size");
            } catch (const std::exception&) {
                ++fuzz_rejections;
            }
        }
        require(fuzz_rejections > 24'900, "Malformed-frame fuzzer rejected too few random frames");

        const auto root = std::filesystem::temp_directory_path() / "binarycoin-v010-hardening";
        std::filesystem::remove_all(root);
        const auto node_dir = root / "node";
        {
            bincoin::TestnetChain chain(node_dir);
            chain.initialize();
        }

        // Persistent ban records, expiry pruning and clearing.
        {
            bincoin::BanStore bans(node_dir);
            bans.load();
            bans.ban("198.51.100.42", 3600, 125, "invalid-checksum-flood");
            bincoin::BanStore reloaded(node_dir);
            reloaded.load();
            require(reloaded.is_banned("198.51.100.42"), "Persistent ban did not reload");
            require(reloaded.records().size() == 1, "Unexpected persistent ban count");
            reloaded.prune_expired(std::numeric_limits<std::int64_t>::max());
            require(reloaded.records().empty(), "Expired ban did not prune");
            reloaded.clear();
        }

        // Duplicate and self-connection rejection by process-level peer nonce.
        {
            bincoin::NetworkPolicy policy;
            policy.use_compiled_seeds = false;
            policy.max_inbound = 8;
            policy.ping_interval = 10s;
            policy.ping_timeout = 20s;
            bincoin::PersistentTestnetNode node(node_dir, "127.0.0.1", 0, {}, policy);
            node.start();

            const int first = connect_loopback(node.bound_port());
            client_handshake(first, node_dir, 0x1122334455667788ULL);
            const int duplicate = connect_loopback(node.bound_port());
            client_handshake(duplicate, node_dir, 0x1122334455667788ULL);
            wait_for_condition([&] { return node.stats().duplicate_connections >= 1; }, 2s,
                               "Duplicate peer nonce was not rejected");

            const int self = connect_loopback(node.bound_port());
            client_handshake(self, node_dir, node.node_nonce());
            wait_for_condition([&] { return node.stats().self_connections >= 1; }, 2s,
                               "Self-connection nonce was not rejected");

            ::close(self);
            ::close(duplicate);
            ::close(first);
            stop_node(node);
        }

        // Inbound exhaustion: one slow handshake occupies the only slot.
        {
            bincoin::NetworkPolicy policy;
            policy.use_compiled_seeds = false;
            policy.max_inbound = 1;
            policy.socket_timeout_seconds = 2;
            policy.ping_interval = 5s;
            policy.ping_timeout = 10s;
            bincoin::PersistentTestnetNode node(node_dir, "127.0.0.1", 0, {}, policy);
            node.start();
            const int slow = connect_loopback(node.bound_port());
            wait_for_condition([&] { return node.stats().active_inbound == 1; }, 1s,
                               "Slow inbound did not reserve a connection slot");
            const int excess = connect_loopback(node.bound_port());
            wait_for_condition([&] { return node.stats().rejected_connections >= 1; }, 1s,
                               "Excess inbound connection was not rejected");
            ::close(excess);
            ::close(slow);
            stop_node(node);
        }

        // Message-rate limiting after a valid handshake.
        {
            bincoin::NetworkPolicy policy;
            policy.use_compiled_seeds = false;
            policy.max_inbound = 4;
            policy.max_messages_per_window = 8;
            policy.message_window = 10s;
            policy.ping_interval = 10s;
            policy.ping_timeout = 20s;
            bincoin::PersistentTestnetNode node(node_dir, "127.0.0.1", 0, {}, policy);
            node.start();
            const int flood = connect_loopback(node.bound_port());
            client_handshake(flood, node_dir, 0xABCDEF1234567890ULL);
            const auto unknown = bincoin::encode_p2p_frame("unknown");
            for (int index = 0; index < 20; ++index) {
                try { send_all(flood, unknown); } catch (const std::exception&) { break; }
            }
            wait_for_condition([&] { return node.stats().rate_limited_connections >= 1; }, 2s,
                               "Message flood was not rate-limited");
            ::close(flood);
            stop_node(node);
        }

        // A peer that ignores ping must be disconnected by the deadline.
        {
            bincoin::NetworkPolicy policy;
            policy.use_compiled_seeds = false;
            policy.max_inbound = 4;
            policy.ping_interval = 100ms;
            policy.ping_timeout = 350ms;
            policy.socket_timeout_seconds = 2;
            bincoin::PersistentTestnetNode node(node_dir, "127.0.0.1", 0, {}, policy);
            node.start();
            const int silent = connect_loopback(node.bound_port());
            client_handshake(silent, node_dir, 0x5555666677778888ULL);
            wait_for_condition([&] { return node.stats().timeout_disconnects >= 1; }, 2s,
                               "Silent peer did not hit the ping timeout");
            ::close(silent);
            stop_node(node);
        }

        // Repeated restart verification and a moderately long append-only chain.
        const auto stress_dir = root / "storage-stress";
        bincoin::TestnetWallet stress_wallet(stress_dir);
        {
            bincoin::TestnetChain chain(stress_dir);
            chain.initialize();
            (void)stress_wallet.create();
            for (int batch = 0; batch < 5; ++batch) {
                stress_wallet.load();
                chain.generate(5, stress_wallet.locking_script());
                bincoin::TestnetChain reloaded(stress_dir);
                reloaded.load();
                reloaded.verify();
                require(reloaded.tip().height == static_cast<std::uint64_t>((batch + 1) * 5),
                        "Restart stress height mismatch");
                chain = std::move(reloaded);
            }
        }
        {
            bincoin::TestnetChain final_chain(stress_dir);
            final_chain.load();
            require(final_chain.tip().height == 25, "Long-chain stress final height mismatch");
            final_chain.verify();
        }

        // Repeated two-way partitions: both sides mine, the greater-work branch
        // wins, the loser reorgs, and both restart from the same durable tip.
        const auto partition_a = root / "partition-a";
        const auto partition_b = root / "partition-b";
        bincoin::TestnetWallet wallet_a(partition_a);
        bincoin::TestnetWallet wallet_b(partition_b);
        {
            bincoin::TestnetChain chain_a(partition_a);
            bincoin::TestnetChain chain_b(partition_b);
            chain_a.initialize();
            chain_b.initialize();
            (void)wallet_a.create();
            (void)wallet_b.create();
            chain_a.generate(5, wallet_a.locking_script());
        }
        (void)sync_once(partition_a, partition_b);

        for (int cycle = 0; cycle < 3; ++cycle) {
            wallet_a.load();
            wallet_b.load();
            bincoin::TestnetChain chain_a(partition_a);
            bincoin::TestnetChain chain_b(partition_b);
            chain_a.load();
            chain_b.load();
            require(chain_a.tip().block.header.hash_hex() == chain_b.tip().block.header.hash_hex(),
                    "Partition cycle did not begin from a common tip");

            const bool a_wins = cycle % 2 == 0;
            chain_a.generate(a_wins ? 3 : 2, wallet_a.locking_script());
            chain_b.generate(a_wins ? 2 : 3, wallet_b.locking_script());
            const auto result = a_wins
                ? sync_once(partition_a, partition_b)
                : sync_once(partition_b, partition_a);
            require(result.activated_candidate && result.reorganized,
                    "Partition loser did not activate the greater-work branch");

            bincoin::TestnetChain restarted_a(partition_a);
            bincoin::TestnetChain restarted_b(partition_b);
            restarted_a.load();
            restarted_b.load();
            restarted_a.verify();
            restarted_b.verify();
            require(restarted_a.tip().block.header.hash_hex() == restarted_b.tip().block.header.hash_hex(),
                    "Partition nodes did not converge after restart");
        }

        std::filesystem::remove_all(root);
        std::cout << "BinaryCoin Testnet Alpha v0.1.5 hardening tests passed: 25,000 frame fuzz cases, "
                     "ban persistence, duplicate/self rejection, connection limits, rate limits, "
                     "ping timeout, 25-block restart stress and three partition reorg cycles.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Hardening test failure: " << error.what() << '\n';
        return 1;
    }
}
