#include "net.hpp"

#include "bootstrap.hpp"

#include "chain.hpp"
#include "branch_store.hpp"
#include "ban_store.hpp"
#include "peer_store.hpp"
#include "hash.hpp"
#include "logger.hpp"
#include "mempool.hpp"
#include "params.hpp"
#include "pow.hpp"
#include "serialize.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <map>
#include <unordered_map>
#include <set>
#include <thread>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#endif
#include <random>
#include <stdexcept>
#include <string_view>

namespace bincoin {
namespace {

constexpr std::size_t MESSAGE_HEADER_SIZE = 24;
constexpr std::size_t COMMAND_SIZE = 12;

class SocketHandle {
public:
    explicit SocketHandle(const Socket socket = INVALID_SOCKET_VALUE) : socket_(socket) {}
    ~SocketHandle() { reset(); }
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    SocketHandle(SocketHandle&& other) noexcept : socket_(other.release()) {}
    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    [[nodiscard]] Socket get() const noexcept { return socket_; }
    [[nodiscard]] Socket release() noexcept {
        const Socket result = socket_;
        socket_ = INVALID_SOCKET_VALUE;
        return result;
    }
    void reset(const Socket replacement = INVALID_SOCKET_VALUE) noexcept {
        if (socket_ != INVALID_SOCKET_VALUE) close_socket(socket_);
        socket_ = replacement;
    }
private:
    Socket socket_{INVALID_SOCKET_VALUE};
};

using Message = DecodedP2pMessage;

[[noreturn]] void throw_socket_error(const std::string& action) {
    const int error = last_socket_error();
    throw std::runtime_error(action + ": " + socket_error_text(error));
}

void configure_socket(const Socket socket, const int timeout_seconds = 10) {
    if (timeout_seconds <= 0 || timeout_seconds > 300) {
        throw std::invalid_argument("Socket timeout is outside the allowed range");
    }
    set_socket_timeouts(socket, timeout_seconds);
}

void send_all(const Socket socket, std::span<const std::uint8_t> bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const std::ptrdiff_t result = socket_send(
            socket,
            bytes.data() + static_cast<std::ptrdiff_t>(sent),
            bytes.size() - sent,
            socket_send_flags()
        );
        if (result < 0) {
            if (socket_error_interrupted(last_socket_error())) continue;
            throw_socket_error("send failed");
        }
        if (result == 0) throw std::runtime_error("Peer closed connection during send");
        sent += static_cast<std::size_t>(result);
    }
}

std::vector<std::uint8_t> receive_exact(const Socket socket, const std::size_t count) {
    std::vector<std::uint8_t> bytes(count);
    std::size_t received = 0;
    while (received < count) {
        const std::ptrdiff_t result = socket_receive(
            socket,
            bytes.data() + static_cast<std::ptrdiff_t>(received),
            count - received,
            0
        );
        if (result < 0) {
            if (socket_error_interrupted(last_socket_error())) continue;
            throw_socket_error("recv failed");
        }
        if (result == 0) throw std::runtime_error("Peer closed connection unexpectedly");
        received += static_cast<std::size_t>(result);
    }
    return bytes;
}

std::array<std::uint8_t, 4> payload_checksum(const std::span<const std::uint8_t> payload) {
    const Hash256 digest = sha256d(payload);
    return {digest[0], digest[1], digest[2], digest[3]};
}

void validate_command(const std::string& command) {
    if (command.empty() || command.size() > COMMAND_SIZE) {
        throw std::invalid_argument("P2P command length is invalid");
    }
    for (const char raw_character : command) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character < 0x20U || character > 0x7eU) {
            throw std::invalid_argument("P2P command contains invalid character");
        }
    }
}

std::vector<std::uint8_t> encode_p2p_frame_impl(
    const std::string& command,
    const std::span<const std::uint8_t> payload
) {
    validate_command(command);
    if (payload.size() > MAX_P2P_PAYLOAD) throw std::runtime_error("P2P payload exceeds limit");

    std::vector<std::uint8_t> frame;
    frame.reserve(MESSAGE_HEADER_SIZE + payload.size());
    frame.insert(frame.end(), TESTNET_MESSAGE_START.begin(), TESTNET_MESSAGE_START.end());
    for (std::size_t index = 0; index < COMMAND_SIZE; ++index) {
        frame.push_back(index < command.size() ? static_cast<std::uint8_t>(command[index]) : 0U);
    }
    append_u32_le(frame, static_cast<std::uint32_t>(payload.size()));
    const auto checksum = payload_checksum(payload);
    frame.insert(frame.end(), checksum.begin(), checksum.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

Message decode_p2p_frame_impl(const std::span<const std::uint8_t> frame) {
    if (frame.size() < MESSAGE_HEADER_SIZE) throw std::runtime_error("Truncated P2P message header");
    if (!std::equal(TESTNET_MESSAGE_START.begin(), TESTNET_MESSAGE_START.end(), frame.begin())) {
        throw std::runtime_error("Peer sent incorrect network message start; expected TBIT");
    }

    std::string command;
    bool padding_started = false;
    for (std::size_t index = 4; index < 4 + COMMAND_SIZE; ++index) {
        const std::uint8_t byte = frame[index];
        if (byte == 0) {
            padding_started = true;
            continue;
        }
        if (padding_started || byte < 0x20U || byte > 0x7eU) {
            throw std::runtime_error("Peer sent malformed command field");
        }
        command.push_back(static_cast<char>(byte));
    }
    validate_command(command);

    std::vector<std::uint8_t> header(frame.begin(), frame.begin() + MESSAGE_HEADER_SIZE);
    std::size_t offset = 16;
    const std::uint32_t payload_length = read_u32_le(header, offset);
    if (payload_length > MAX_P2P_PAYLOAD) throw std::runtime_error("Peer payload exceeds limit");
    const std::size_t expected_size = MESSAGE_HEADER_SIZE + static_cast<std::size_t>(payload_length);
    if (frame.size() != expected_size) throw std::runtime_error("P2P frame length does not match header");
    const auto expected_checksum_bytes = read_bytes(header, offset, 4);
    std::vector<std::uint8_t> payload(frame.begin() + MESSAGE_HEADER_SIZE, frame.end());
    const auto actual_checksum = payload_checksum(payload);
    if (!std::equal(actual_checksum.begin(), actual_checksum.end(), expected_checksum_bytes.begin())) {
        throw std::runtime_error("P2P payload checksum mismatch");
    }
    return Message{.command = std::move(command), .payload = std::move(payload)};
}

void send_message(
    const Socket socket,
    const std::string& command,
    const std::span<const std::uint8_t> payload = {}
) {
    const auto frame = encode_p2p_frame_impl(command, payload);
    send_all(socket, frame);
}

Message receive_message(const Socket socket) {
    const auto header = receive_exact(socket, MESSAGE_HEADER_SIZE);
    if (!std::equal(TESTNET_MESSAGE_START.begin(), TESTNET_MESSAGE_START.end(), header.begin())) {
        throw std::runtime_error("Peer sent incorrect network message start; expected TBIT");
    }
    std::string command;
    bool padding_started = false;
    for (std::size_t index = 4; index < 4 + COMMAND_SIZE; ++index) {
        const std::uint8_t byte = header[index];
        if (byte == 0) {
            padding_started = true;
            continue;
        }
        if (padding_started || byte < 0x20U || byte > 0x7eU) {
            throw std::runtime_error("Peer sent malformed command field");
        }
        command.push_back(static_cast<char>(byte));
    }
    validate_command(command);
    std::size_t offset = 16;
    const std::uint32_t payload_length = read_u32_le(header, offset);
    if (payload_length > MAX_P2P_PAYLOAD) throw std::runtime_error("Peer payload exceeds limit");
    const auto payload = receive_exact(socket, payload_length);
    std::vector<std::uint8_t> frame;
    frame.reserve(MESSAGE_HEADER_SIZE + payload.size());
    frame.insert(frame.end(), header.begin(), header.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return decode_p2p_frame_impl(frame);
}

std::uint64_t random_nonce() {
    std::random_device device;
    const std::uint64_t high = static_cast<std::uint64_t>(device()) << 32U;
    return high ^ static_cast<std::uint64_t>(device());
}

PeerVersion local_version(const TestnetChain& chain, const std::uint64_t fixed_nonce = 0) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    return PeerVersion{
        .protocol_version = P2P_PROTOCOL_VERSION,
        .services = NODE_NETWORK_SERVICE,
        .timestamp = static_cast<std::int64_t>(now),
        .best_height = chain.tip().height,
        .genesis_hash = TESTNET_GENESIS_HASH,
        .tip_hash = chain.tip().block.header.hash_hex(),
        .chainwork = chain.chainwork_hex(),
        .nonce = fixed_nonce == 0 ? random_nonce() : fixed_nonce,
        .user_agent = "/BinaryCoinTestnetAlpha:0.1.4/",
    };
}

std::vector<std::uint8_t> serialize_version_impl(const PeerVersion& version) {
    std::vector<std::uint8_t> payload;
    append_u32_le(payload, version.protocol_version);
    append_u64_le(payload, version.services);
    append_i64_le(payload, version.timestamp);
    append_u64_le(payload, version.best_height);
    const auto genesis = display_hex_to_wire_bytes(version.genesis_hash);
    payload.insert(payload.end(), genesis.begin(), genesis.end());
    const auto tip = display_hex_to_wire_bytes(version.tip_hash);
    payload.insert(payload.end(), tip.begin(), tip.end());
    const auto chainwork = display_hex_to_wire_bytes(version.chainwork);
    payload.insert(payload.end(), chainwork.begin(), chainwork.end());
    append_u64_le(payload, version.nonce);
    append_compact_size(payload, version.user_agent.size());
    payload.insert(payload.end(), version.user_agent.begin(), version.user_agent.end());
    return payload;
}

PeerVersion deserialize_version_impl(const std::vector<std::uint8_t>& payload) {
    std::size_t offset = 0;
    PeerVersion version;
    version.protocol_version = read_u32_le(payload, offset);
    version.services = read_u64_le(payload, offset);
    version.timestamp = read_i64_le(payload, offset);
    version.best_height = read_u64_le(payload, offset);

    const auto genesis_vector = read_bytes(payload, offset, 32);
    std::array<std::uint8_t, 32> genesis{};
    std::copy(genesis_vector.begin(), genesis_vector.end(), genesis.begin());
    version.genesis_hash = wire_bytes_to_display_hex(genesis);

    const auto tip_vector = read_bytes(payload, offset, 32);
    std::array<std::uint8_t, 32> tip{};
    std::copy(tip_vector.begin(), tip_vector.end(), tip.begin());
    version.tip_hash = wire_bytes_to_display_hex(tip);

    const auto chainwork_vector = read_bytes(payload, offset, 32);
    std::array<std::uint8_t, 32> chainwork{};
    std::copy(chainwork_vector.begin(), chainwork_vector.end(), chainwork.begin());
    version.chainwork = wire_bytes_to_display_hex(chainwork);

    version.nonce = read_u64_le(payload, offset);
    const std::uint64_t user_agent_size = read_compact_size(payload, offset);
    if (user_agent_size > 256 || user_agent_size > payload.size() - offset) {
        throw std::runtime_error("Peer user agent is too large");
    }
    const auto user_agent = read_bytes(payload, offset, static_cast<std::size_t>(user_agent_size));
    version.user_agent.assign(user_agent.begin(), user_agent.end());
    if (offset != payload.size()) throw std::runtime_error("Trailing bytes in version message");
    if (version.protocol_version != P2P_PROTOCOL_VERSION) {
        throw std::runtime_error("Unsupported BinaryCoin P2P protocol version");
    }
    if (version.genesis_hash != TESTNET_GENESIS_HASH) {
        throw std::runtime_error("Peer belongs to a different genesis/network");
    }
    return version;
}

void expect_command(const Message& message, const std::string_view expected) {
    if (message.command != expected) {
        throw std::runtime_error("Expected P2P command " + std::string(expected) +
                                 ", received " + message.command);
    }
}

PeerVersion client_handshake(const Socket socket, const TestnetChain& chain, const std::uint64_t local_nonce = 0) {
    send_message(socket, "version", serialize_version_impl(local_version(chain, local_nonce)));
    const Message remote_version_message = receive_message(socket);
    expect_command(remote_version_message, "version");
    const PeerVersion remote = deserialize_version_impl(remote_version_message.payload);
    send_message(socket, "verack");
    const Message verack = receive_message(socket);
    expect_command(verack, "verack");
    if (!verack.payload.empty()) throw std::runtime_error("verack payload must be empty");
    return remote;
}

PeerVersion server_handshake(const Socket socket, const TestnetChain& chain, const std::uint64_t local_nonce = 0) {
    const Message remote_version_message = receive_message(socket);
    expect_command(remote_version_message, "version");
    const PeerVersion remote = deserialize_version_impl(remote_version_message.payload);
    send_message(socket, "version", serialize_version_impl(local_version(chain, local_nonce)));
    const Message verack = receive_message(socket);
    expect_command(verack, "verack");
    if (!verack.payload.empty()) throw std::runtime_error("verack payload must be empty");
    send_message(socket, "verack");
    return remote;
}

SocketHandle connect_socket(const PeerEndpoint& endpoint) {
    initialize_socket_runtime();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* raw_results = nullptr;
    const std::string service = std::to_string(endpoint.port);
    const int lookup = ::getaddrinfo(endpoint.host.c_str(), service.c_str(), &hints, &raw_results);
    if (lookup != 0) throw std::runtime_error("Unable to resolve peer: " + std::string(gai_strerror(lookup)));
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> results(raw_results, ::freeaddrinfo);

    for (addrinfo* candidate = results.get(); candidate != nullptr; candidate = candidate->ai_next) {
        SocketHandle socket(::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol));
        if (socket.get() == INVALID_SOCKET_VALUE) continue;
        try {
            configure_socket(socket.get());
        } catch (...) {
            continue;
        }
        if (::connect(socket.get(), candidate->ai_addr, candidate->ai_addrlen) == 0) return socket;
    }
    throw std::runtime_error("Unable to connect to peer " + endpoint.host + ":" + service);
}

Socket create_listen_socket(const std::string& bind_address, const std::uint16_t port, std::uint16_t& bound_port) {
    initialize_socket_runtime();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* raw_results = nullptr;
    const std::string service = std::to_string(port);
    const char* host = bind_address.empty() ? nullptr : bind_address.c_str();
    const int lookup = ::getaddrinfo(host, service.c_str(), &hints, &raw_results);
    if (lookup != 0) throw std::runtime_error("Unable to resolve bind address: " + std::string(gai_strerror(lookup)));
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> results(raw_results, ::freeaddrinfo);

    for (addrinfo* candidate = results.get(); candidate != nullptr; candidate = candidate->ai_next) {
        SocketHandle socket(::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol));
        if (socket.get() == INVALID_SOCKET_VALUE) continue;
        set_socket_reuse_address(socket.get());
        if (::bind(socket.get(), candidate->ai_addr, candidate->ai_addrlen) != 0) continue;
        if (::listen(socket.get(), 16) != 0) continue;

        sockaddr_storage local{};
        SocketLength local_length = sizeof(local);
        if (::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&local), &local_length) != 0) {
            throw_socket_error("getsockname failed");
        }
        if (local.ss_family == AF_INET) {
            bound_port = ntohs(reinterpret_cast<const sockaddr_in*>(&local)->sin_port);
        } else if (local.ss_family == AF_INET6) {
            bound_port = ntohs(reinterpret_cast<const sockaddr_in6*>(&local)->sin6_port);
        } else {
            throw std::runtime_error("Unsupported listening socket family");
        }
        return socket.release();
    }
    throw std::runtime_error("Unable to bind P2P listener to " + bind_address + ":" + service);
}

std::vector<std::uint8_t> serialize_block_record(const StoredBlock& stored) {
    std::vector<std::uint8_t> output;
    append_u64_le(output, stored.height);
    const auto header = stored.block.header.serialize();
    output.insert(output.end(), header.begin(), header.end());
    append_compact_size(output, stored.block.transactions.size());
    for (const Transaction& transaction : stored.block.transactions) {
        const auto transaction_bytes = transaction.serialize();
        append_compact_size(output, transaction_bytes.size());
        output.insert(output.end(), transaction_bytes.begin(), transaction_bytes.end());
    }
    return output;
}

StoredBlock deserialize_block_record(const std::span<const std::uint8_t> input, std::size_t& offset) {
    StoredBlock stored;
    stored.height = read_u64_le(input, offset);
    const auto header_bytes = read_bytes(input, offset, 80);
    stored.block.header = BlockHeader::deserialize(header_bytes);
    const std::uint64_t transaction_count = read_compact_size(input, offset);
    if (transaction_count > 100'000) throw std::runtime_error("Block transaction count exceeds limit");
    stored.block.transactions.reserve(static_cast<std::size_t>(transaction_count));
    for (std::uint64_t index = 0; index < transaction_count; ++index) {
        const std::uint64_t transaction_size = read_compact_size(input, offset);
        if (transaction_size > 4'000'000 || transaction_size > input.size() - offset) {
            throw std::runtime_error("Transaction payload exceeds limit");
        }
        const auto transaction_bytes = read_bytes(input, offset, static_cast<std::size_t>(transaction_size));
        stored.block.transactions.push_back(Transaction::deserialize(transaction_bytes));
    }
    return stored;
}

std::vector<std::uint8_t> serialize_transactions(const std::vector<Transaction>& transactions) {
    std::vector<std::uint8_t> output;
    append_compact_size(output, transactions.size());
    for (const auto& transaction : transactions) {
        const auto bytes = transaction.serialize();
        append_compact_size(output, bytes.size());
        output.insert(output.end(), bytes.begin(), bytes.end());
    }
    return output;
}

std::vector<Transaction> deserialize_transactions(const std::vector<std::uint8_t>& payload) {
    std::size_t offset = 0;
    const std::uint64_t count = read_compact_size(payload, offset);
    if (count > 100'000) throw std::runtime_error("Mempool transaction count exceeds limit");
    std::vector<Transaction> transactions;
    transactions.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        const std::uint64_t size = read_compact_size(payload, offset);
        if (size > 4'000'000 || size > payload.size() - offset) {
            throw std::runtime_error("Mempool transaction payload exceeds limit");
        }
        const auto bytes = read_bytes(payload, offset, static_cast<std::size_t>(size));
        transactions.push_back(Transaction::deserialize(bytes));
    }
    if (offset != payload.size()) throw std::runtime_error("Trailing bytes in mempool response");
    return transactions;
}


enum class InventoryType : std::uint32_t {
    Transaction = 1,
    Block = 2,
};

struct InventoryItem {
    InventoryType type{InventoryType::Transaction};
    std::string hash;
};

std::vector<std::uint8_t> serialize_inventory(const std::vector<InventoryItem>& items) {
    if (items.size() > MAX_INVENTORY_ITEMS) {
        throw std::runtime_error("Inventory exceeds item limit");
    }
    std::vector<std::uint8_t> payload;
    append_compact_size(payload, items.size());
    for (const InventoryItem& item : items) {
        append_u32_le(payload, static_cast<std::uint32_t>(item.type));
        const auto hash = display_hex_to_wire_bytes(item.hash);
        payload.insert(payload.end(), hash.begin(), hash.end());
    }
    return payload;
}

std::vector<InventoryItem> deserialize_inventory(const std::vector<std::uint8_t>& payload) {
    std::size_t offset = 0;
    const std::uint64_t count = read_compact_size(payload, offset);
    if (count > MAX_INVENTORY_ITEMS) throw std::runtime_error("Peer inventory exceeds item limit");
    std::vector<InventoryItem> items;
    items.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        const std::uint32_t raw_type = read_u32_le(payload, offset);
        if (raw_type != static_cast<std::uint32_t>(InventoryType::Transaction) &&
            raw_type != static_cast<std::uint32_t>(InventoryType::Block)) {
            throw std::runtime_error("Peer inventory contains unknown type");
        }
        const auto hash_vector = read_bytes(payload, offset, 32);
        std::array<std::uint8_t, 32> hash{};
        std::copy(hash_vector.begin(), hash_vector.end(), hash.begin());
        items.push_back(InventoryItem{
            .type = static_cast<InventoryType>(raw_type),
            .hash = wire_bytes_to_display_hex(hash),
        });
    }
    if (offset != payload.size()) throw std::runtime_error("Trailing bytes in inventory payload");
    return items;
}

bool socket_readable(const Socket socket, const int timeout_milliseconds) {
    return wait_socket_readable(socket, timeout_milliseconds);
}

std::optional<StoredBlock> find_block(const TestnetChain& chain, const std::string& hash) {
    for (const StoredBlock& stored : chain.blocks()) {
        if (stored.block.header.hash_hex() == hash) return stored;
    }
    return std::nullopt;
}

std::optional<Transaction> find_transaction(
    const TestnetChain& chain,
    const TestnetMempool& mempool,
    const std::string& txid
) {
    for (const Transaction& transaction : mempool.transactions()) {
        if (transaction.txid() == txid) return transaction;
    }
    for (const StoredBlock& stored : chain.blocks()) {
        for (const Transaction& transaction : stored.block.transactions) {
            if (transaction.txid() == txid) return transaction;
        }
    }
    return std::nullopt;
}

bool chain_has_block(const TestnetChain& chain, const std::string& hash) {
    return find_block(chain, hash).has_value();
}

bool node_has_transaction(
    const TestnetChain& chain,
    const TestnetMempool& mempool,
    const std::string& txid
) {
    return find_transaction(chain, mempool, txid).has_value();
}

std::vector<std::uint8_t> serialize_getheaders(
    const std::vector<std::string>& locator,
    const std::string& stop_hash = std::string(64, '0')
) {
    if (locator.empty() || locator.size() > 101) throw std::runtime_error("Invalid block locator size");
    std::vector<std::uint8_t> payload;
    append_compact_size(payload, locator.size());
    for (const std::string& hash_text : locator) {
        const auto hash = display_hex_to_wire_bytes(hash_text);
        payload.insert(payload.end(), hash.begin(), hash.end());
    }
    const auto stop = display_hex_to_wire_bytes(stop_hash);
    payload.insert(payload.end(), stop.begin(), stop.end());
    return payload;
}

struct GetHeadersRequest {
    std::vector<std::string> locator;
    std::string stop_hash;
};

GetHeadersRequest deserialize_getheaders(const std::vector<std::uint8_t>& payload) {
    std::size_t offset = 0;
    const std::uint64_t count = read_compact_size(payload, offset);
    if (count == 0 || count > 101) throw std::runtime_error("Peer block locator exceeds limit");
    GetHeadersRequest request;
    request.locator.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto raw = read_bytes(payload, offset, 32);
        std::array<std::uint8_t, 32> hash{};
        std::copy(raw.begin(), raw.end(), hash.begin());
        request.locator.push_back(wire_bytes_to_display_hex(hash));
    }
    const auto stop_raw = read_bytes(payload, offset, 32);
    std::array<std::uint8_t, 32> stop{};
    std::copy(stop_raw.begin(), stop_raw.end(), stop.begin());
    request.stop_hash = wire_bytes_to_display_hex(stop);
    if (offset != payload.size()) throw std::runtime_error("Trailing bytes in getheaders payload");
    return request;
}

std::vector<std::uint8_t> serialize_headers(const std::vector<HeaderRecord>& headers) {
    if (headers.size() > MAX_HEADERS_PER_RESPONSE) throw std::runtime_error("Too many headers");
    std::vector<std::uint8_t> payload;
    append_compact_size(payload, headers.size());
    for (const HeaderRecord& record : headers) {
        append_u64_le(payload, record.height);
        const auto header = record.header.serialize();
        payload.insert(payload.end(), header.begin(), header.end());
        append_compact_size(payload, 0); // Bitcoin-style zero transaction count marker.
    }
    return payload;
}

std::vector<HeaderRecord> deserialize_headers(const std::vector<std::uint8_t>& payload) {
    std::size_t offset = 0;
    const std::uint64_t count = read_compact_size(payload, offset);
    if (count > MAX_HEADERS_PER_RESPONSE) throw std::runtime_error("Peer sent too many headers");
    std::vector<HeaderRecord> headers;
    headers.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        HeaderRecord record;
        record.height = read_u64_le(payload, offset);
        record.header = BlockHeader::deserialize(read_bytes(payload, offset, 80));
        if (read_compact_size(payload, offset) != 0) {
            throw std::runtime_error("Header record transaction count must be zero");
        }
        if (compact_to_target(record.header.bits) > compact_to_target(TESTNET_POW_LIMIT_BITS) ||
            !check_proof_of_work(record.header.raw_hash(), record.header.bits)) {
            throw std::runtime_error("Invalid proof of work in header chain");
        }
        headers.push_back(std::move(record));
    }
    if (offset != payload.size()) throw std::runtime_error("Trailing bytes in headers payload");
    return headers;
}

std::size_t locate_fork_index(
    const std::vector<StoredBlock>& blocks,
    const std::vector<std::string>& locator
) {
    for (const std::string& locator_hash : locator) {
        for (std::size_t index = blocks.size(); index-- > 0;) {
            if (blocks[index].block.header.hash_hex() == locator_hash) return index;
        }
    }
    return 0;
}

std::vector<HeaderRecord> headers_after_locator(
    const TestnetChain& chain,
    const GetHeadersRequest& request
) {
    const std::size_t fork_index = locate_fork_index(chain.blocks(), request.locator);
    std::vector<HeaderRecord> result;
    for (std::size_t index = fork_index + 1;
         index < chain.blocks().size() && result.size() < MAX_HEADERS_PER_RESPONSE;
         ++index) {
        const StoredBlock& stored = chain.blocks()[index];
        result.push_back(HeaderRecord{.height = stored.height, .header = stored.block.header});
        if (request.stop_hash != std::string(64, '0') &&
            stored.block.header.hash_hex() == request.stop_hash) break;
    }
    return result;
}

std::vector<std::uint8_t> serialize_addrv2(const std::vector<PeerRecord>& records) {
    struct EncodedAddress {
        const PeerRecord* record;
        std::uint8_t network_id;
        std::array<std::uint8_t, 16> address;
        std::size_t size;
    };
    std::vector<EncodedAddress> encodable;
    for (const PeerRecord& record : records) {
        EncodedAddress encoded{.record = &record, .network_id = 0, .address = {}, .size = 0};
        if (::inet_pton(AF_INET, record.endpoint.host.c_str(), encoded.address.data()) == 1) {
            encoded.network_id = 1;
            encoded.size = 4;
        } else if (::inet_pton(AF_INET6, record.endpoint.host.c_str(), encoded.address.data()) == 1) {
            encoded.network_id = 2;
            encoded.size = 16;
        } else {
            continue;
        }
        encodable.push_back(encoded);
        if (encodable.size() == MAX_ADDRV2_ITEMS) break;
    }

    std::vector<std::uint8_t> payload;
    append_compact_size(payload, encodable.size());
    for (const EncodedAddress& encoded : encodable) {
        const std::int64_t clamped_time = std::clamp<std::int64_t>(
            encoded.record->last_seen, 0, std::numeric_limits<std::uint32_t>::max());
        append_u32_le(payload, static_cast<std::uint32_t>(clamped_time));
        append_compact_size(payload, encoded.record->services);
        payload.push_back(encoded.network_id);
        append_compact_size(payload, encoded.size);
        payload.insert(
            payload.end(), encoded.address.begin(),
            encoded.address.begin() + static_cast<std::ptrdiff_t>(encoded.size));
        payload.push_back(static_cast<std::uint8_t>((encoded.record->endpoint.port >> 8U) & 0xffU));
        payload.push_back(static_cast<std::uint8_t>(encoded.record->endpoint.port & 0xffU));
    }
    return payload;
}

std::vector<PeerRecord> deserialize_addrv2(const std::vector<std::uint8_t>& payload) {
    std::size_t offset = 0;
    const std::uint64_t count = read_compact_size(payload, offset);
    if (count > MAX_ADDRV2_ITEMS) throw std::runtime_error("Peer addrv2 count exceeds limit");
    std::vector<PeerRecord> records;
    for (std::uint64_t index = 0; index < count; ++index) {
        const std::uint32_t timestamp = read_u32_le(payload, offset);
        const std::uint64_t services = read_compact_size(payload, offset);
        if (offset >= payload.size()) throw std::runtime_error("Truncated addrv2 network id");
        const std::uint8_t network_id = payload[offset++];
        const std::uint64_t address_size = read_compact_size(payload, offset);
        const std::size_t expected_size = network_id == 1 ? 4U : network_id == 2 ? 16U : 0U;
        if (expected_size == 0 || address_size != expected_size) {
            if (address_size > payload.size() - offset) throw std::runtime_error("Truncated addrv2 address");
            offset += static_cast<std::size_t>(address_size);
            if (payload.size() - offset < 2) throw std::runtime_error("Truncated addrv2 port");
            offset += 2;
            continue;
        }
        const auto address = read_bytes(payload, offset, expected_size);
        if (payload.size() - offset < 2) throw std::runtime_error("Truncated addrv2 port");
        const std::uint16_t port = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(payload[offset]) << 8U) |
            static_cast<std::uint16_t>(payload[offset + 1]));
        offset += 2;
        char text[INET6_ADDRSTRLEN]{};
        if (::inet_ntop(network_id == 1 ? AF_INET : AF_INET6, address.data(), text, sizeof(text)) == nullptr) {
            continue;
        }
        if (port == 0) continue;
        records.push_back(PeerRecord{
            .endpoint = PeerEndpoint{.host = text, .port = port},
            .services = services,
            .last_seen = static_cast<std::int64_t>(timestamp),
            .last_success = 0,
            .failures = 0,
            .manual = false,
        });
    }
    if (offset != payload.size()) throw std::runtime_error("Trailing bytes in addrv2 payload");
    return records;
}

PeerEndpoint endpoint_from_sockaddr(const sockaddr_storage& address) {
    char host[INET6_ADDRSTRLEN]{};
    std::uint16_t port = 0;
    if (address.ss_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
        (void)::inet_ntop(AF_INET, &ipv4->sin_addr, host, sizeof(host));
        port = ntohs(ipv4->sin_port);
    } else if (address.ss_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
        (void)::inet_ntop(AF_INET6, &ipv6->sin6_addr, host, sizeof(host));
        port = ntohs(ipv6->sin6_port);
    }
    return PeerEndpoint{.host = host, .port = port};
}

} // namespace

std::vector<std::uint8_t> encode_p2p_frame(
    const std::string& command,
    const std::span<const std::uint8_t> payload
) {
    return encode_p2p_frame_impl(command, payload);
}

DecodedP2pMessage decode_p2p_frame(const std::span<const std::uint8_t> frame) {
    return decode_p2p_frame_impl(frame);
}

std::vector<std::uint8_t> serialize_peer_version(const PeerVersion& version) {
    return serialize_version_impl(version);
}

PeerVersion deserialize_peer_version(const std::vector<std::uint8_t>& payload) {
    return deserialize_version_impl(payload);
}

PeerEndpoint parse_peer_endpoint(const std::string& text) {
    if (text.empty()) throw std::invalid_argument("Peer endpoint cannot be empty");
    PeerEndpoint endpoint;
    if (text.front() == '[') {
        const std::size_t end = text.find(']');
        if (end == std::string::npos) throw std::invalid_argument("Malformed bracketed IPv6 endpoint");
        endpoint.host = text.substr(1, end - 1);
        if (end + 1 < text.size()) {
            if (text[end + 1] != ':') throw std::invalid_argument("Malformed bracketed IPv6 endpoint");
            const unsigned long value = std::stoul(text.substr(end + 2));
            if (value == 0 || value > std::numeric_limits<std::uint16_t>::max()) {
                throw std::invalid_argument("Peer port outside valid range");
            }
            endpoint.port = static_cast<std::uint16_t>(value);
        }
        return endpoint;
    }

    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos) {
        endpoint.host = text;
        return endpoint;
    }
    if (text.find(':') != colon) throw std::invalid_argument("IPv6 endpoints must use [address]:port syntax");
    endpoint.host = text.substr(0, colon);
    const unsigned long value = std::stoul(text.substr(colon + 1));
    if (endpoint.host.empty() || value == 0 || value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("Peer endpoint is invalid");
    }
    endpoint.port = static_cast<std::uint16_t>(value);
    return endpoint;
}

std::vector<std::uint8_t> serialize_stored_blocks(
    const std::vector<StoredBlock>& blocks,
    const std::size_t begin_index,
    const std::size_t maximum_count
) {
    if (begin_index > blocks.size()) throw std::out_of_range("Block begin index outside chain");
    const std::size_t count = std::min(maximum_count, blocks.size() - begin_index);
    std::vector<std::uint8_t> output;
    append_compact_size(output, count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto record = serialize_block_record(blocks[begin_index + index]);
        append_compact_size(output, record.size());
        output.insert(output.end(), record.begin(), record.end());
    }
    if (output.size() > MAX_P2P_PAYLOAD) throw std::runtime_error("Block response exceeds P2P payload limit");
    return output;
}

std::vector<StoredBlock> deserialize_stored_blocks(const std::vector<std::uint8_t>& payload) {
    std::size_t offset = 0;
    const std::uint64_t count = read_compact_size(payload, offset);
    if (count > MAX_BLOCKS_PER_RESPONSE) throw std::runtime_error("Peer sent too many blocks");
    std::vector<StoredBlock> blocks;
    blocks.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        const std::uint64_t record_size = read_compact_size(payload, offset);
        if (record_size > MAX_P2P_PAYLOAD || record_size > payload.size() - offset) {
            throw std::runtime_error("Block record exceeds payload limit");
        }
        const auto record = read_bytes(payload, offset, static_cast<std::size_t>(record_size));
        std::size_t record_offset = 0;
        blocks.push_back(deserialize_block_record(record, record_offset));
        if (record_offset != record.size()) throw std::runtime_error("Trailing bytes in block record");
    }
    if (offset != payload.size()) throw std::runtime_error("Trailing bytes in block response");
    return blocks;
}

std::vector<std::string> make_block_locator(const std::vector<StoredBlock>& blocks) {
    if (blocks.empty()) return {};
    std::vector<std::string> locator;
    std::size_t index = blocks.size() - 1;
    std::size_t step = 1;
    while (true) {
        locator.push_back(blocks[index].block.header.hash_hex());
        if (index == 0 || locator.size() >= 101) break;
        if (locator.size() > 10) step *= 2;
        index = index > step ? index - step : 0;
    }
    if (locator.back() != blocks.front().block.header.hash_hex()) {
        locator.push_back(blocks.front().block.header.hash_hex());
    }
    return locator;
}

TestnetP2pServer::TestnetP2pServer(
    std::filesystem::path data_directory,
    std::string bind_address,
    const std::uint16_t port
) : data_directory_(std::move(data_directory)), bind_address_(std::move(bind_address)), port_(port) {
    listen_socket_ = create_listen_socket(bind_address_, port_, port_);
}

TestnetP2pServer::~TestnetP2pServer() {
    if (listen_socket_ != INVALID_SOCKET_VALUE) close_socket(listen_socket_);
}

void TestnetP2pServer::handle_client(const Socket client_socket) {
    TestnetChain chain(data_directory_);
    chain.initialize();
    (void)server_handshake(client_socket, chain);

    while (!stop_requested_) {
        Message request;
        try {
            request = receive_message(client_socket);
        } catch (const std::runtime_error& error) {
            const std::string message = error.what();
            if (message.find("closed connection") != std::string::npos ||
                message.find("Resource temporarily unavailable") != std::string::npos) {
                return;
            }
            throw;
        }

        if (request.command == "getheaders") {
            chain.load();
            const auto headers = headers_after_locator(chain, deserialize_getheaders(request.payload));
            send_message(client_socket, "headers", serialize_headers(headers));
            continue;
        }

        if (request.command == "getaddr") {
            if (!request.payload.empty()) throw std::runtime_error("getaddr payload must be empty");
            PeerStore peers(data_directory_);
            peers.load();
            send_message(client_socket, "addrv2", serialize_addrv2(peers.records()));
            continue;
        }

        if (request.command == "getdata") {
            const auto inventory = deserialize_inventory(request.payload);
            std::vector<InventoryItem> not_found;
            chain.load();
            SideBranchStore branches(data_directory_);
            branches.load();
            TestnetMempool mempool(data_directory_);
            mempool.load();
            for (const InventoryItem& item : inventory) {
                if (item.type == InventoryType::Block) {
                    auto block = find_block(chain, item.hash);
                    if (!block) block = branches.find(item.hash);
                    if (block) send_message(client_socket, "block", serialize_stored_blocks({*block}, 0, 1));
                    else not_found.push_back(item);
                } else {
                    const auto transaction = find_transaction(chain, mempool, item.hash);
                    if (transaction) send_message(client_socket, "tx", transaction->serialize());
                    else not_found.push_back(item);
                }
            }
            if (!not_found.empty()) send_message(client_socket, "notfound", serialize_inventory(not_found));
            continue;
        }

        if (request.command == "getblocks") {
            std::size_t offset = 0;
            const std::uint64_t start_height = read_u64_le(request.payload, offset);
            const std::uint32_t requested = read_u32_le(request.payload, offset);
            if (offset != request.payload.size()) throw std::runtime_error("Malformed getblocks payload");
            if (requested == 0 || requested > MAX_BLOCKS_PER_RESPONSE) {
                throw std::runtime_error("Invalid getblocks count");
            }
            chain.load();
            const std::size_t begin = start_height > chain.tip().height
                ? chain.blocks().size()
                : static_cast<std::size_t>(start_height);
            send_message(client_socket, "blocks", serialize_stored_blocks(chain.blocks(), begin, requested));
            continue;
        }

        if (request.command == "getmempool") {
            if (!request.payload.empty()) throw std::runtime_error("getmempool payload must be empty");
            TestnetMempool mempool(data_directory_);
            mempool.load();
            send_message(client_socket, "txs", serialize_transactions(mempool.transactions()));
            continue;
        }

        if (request.command == "tx") {
            const Transaction transaction = Transaction::deserialize(request.payload);
            TestnetMempool mempool(data_directory_);
            mempool.load();
            chain.load();
            mempool.add(transaction, chain.utxo_set(), chain.tip().height + 1);
            send_message(client_socket, "txack", transaction.raw_hash());
            continue;
        }

        if (request.command == "ping") {
            send_message(client_socket, "pong", request.payload);
            continue;
        }

        if (request.command == "bye") return;
        throw std::runtime_error("Unsupported P2P command: " + request.command);
    }
}

void TestnetP2pServer::serve_once() {
    while (!stop_requested_ && !wait_socket_readable(listen_socket_, 200)) {
    }
    if (stop_requested_) return;

    sockaddr_storage remote{};
    SocketLength remote_length = sizeof(remote);
    const Socket accepted = ::accept(listen_socket_, reinterpret_cast<sockaddr*>(&remote), &remote_length);
    if (accepted == INVALID_SOCKET_VALUE) {
        if (socket_error_interrupted(last_socket_error()) && stop_requested_) return;
        throw_socket_error("accept failed");
    }
    SocketHandle client(accepted);
    configure_socket(client.get());
    handle_client(client.get());
}

void TestnetP2pServer::serve_forever() {
    while (!stop_requested_) {
        try {
            serve_once();
        } catch (const std::exception&) {
            if (stop_requested_) return;
            // A malformed peer must not stop the listening node.
        }
    }
}

void TestnetP2pServer::request_stop() {
    stop_requested_ = true;
}

std::uint16_t TestnetP2pServer::bound_port() const noexcept { return port_; }

PersistentTestnetNode::PersistentTestnetNode(
    std::filesystem::path data_directory,
    std::string bind_address,
    const std::uint16_t port,
    std::vector<PeerEndpoint> outbound_peers,
    NetworkPolicy policy
) : data_directory_(std::move(data_directory)),
    bind_address_(std::move(bind_address)),
    port_(port),
    outbound_peers_(std::move(outbound_peers)),
    policy_(std::move(policy)),
    node_nonce_(random_nonce()) {
    if (policy_.max_outbound > 64 || policy_.max_inbound > 1024) {
        throw std::invalid_argument("Peer connection limit is outside the allowed range");
    }
    if (policy_.ban_threshold == 0 || policy_.max_messages_per_window == 0 ||
        policy_.socket_timeout_seconds <= 0 || policy_.ping_timeout <= policy_.ping_interval) {
        throw std::invalid_argument("Network hardening policy is invalid");
    }
    TestnetChain chain(data_directory_);
    chain.initialize();
    PeerStore peer_store(data_directory_);
    peer_store.load();
    if (policy_.use_compiled_seeds) {
        const BootstrapResult bootstrap = bootstrap_testnet_seeds(peer_store);
        log_info("net", "Loaded compiled seeds: dns_resolved=" +
            std::to_string(bootstrap.dns_names_resolved) + " dns_added=" +
            std::to_string(bootstrap.dns_entries_added) + " fixed_added=" +
            std::to_string(bootstrap.fixed_entries_added));
    }
    const auto now = static_cast<std::int64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    for (const PeerEndpoint& endpoint : outbound_peers_) {
        peer_store.add_or_update(PeerRecord{
            .endpoint = endpoint,
            .services = NODE_NETWORK_SERVICE,
            .last_seen = now,
            .last_success = 0,
            .failures = 0,
            .manual = true,
        });
    }
    listen_socket_ = create_listen_socket(bind_address_, port_, port_);
    if (!bind_address_.empty() && bind_address_ != "0.0.0.0" && bind_address_ != "::") {
        peer_store.add_or_update(PeerRecord{
            .endpoint = PeerEndpoint{.host = bind_address_, .port = port_},
            .services = NODE_NETWORK_SERVICE,
            .last_seen = now,
            .last_success = now,
            .failures = 0,
            .manual = false,
        });
    }
    peer_store.save();
}

PersistentTestnetNode::~PersistentTestnetNode() {
    request_stop();
    wait();
    if (listen_socket_ != INVALID_SOCKET_VALUE) {
        close_socket(listen_socket_);
        listen_socket_ = INVALID_SOCKET_VALUE;
    }
}

std::vector<PeerEndpoint> PersistentTestnetNode::initial_outbound_peers() {
    std::lock_guard lock(peer_store_mutex_);
    PeerStore peer_store(data_directory_);
    peer_store.load();
    std::vector<PeerEndpoint> excluded{
        PeerEndpoint{.host = bind_address_, .port = port_},
        PeerEndpoint{.host = "127.0.0.1", .port = port_},
        PeerEndpoint{.host = "::1", .port = port_},
    };
    return peer_store.select_outbound(policy_.max_outbound, excluded);
}

void PersistentTestnetNode::remember_peer(const PeerRecord& peer) {
    std::lock_guard lock(peer_store_mutex_);
    PeerStore peer_store(data_directory_);
    peer_store.load();
    peer_store.add_or_update(peer);
    peer_store.save();
}

void PersistentTestnetNode::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        throw std::runtime_error("Persistent node is already started");
    }
    log_info("net", "P2P listening on " + bind_address_ + ':' + std::to_string(port_));
    accept_thread_ = std::thread([this] { accept_loop(); });
    const auto selected = initial_outbound_peers();
    log_info("net", "Selected " + std::to_string(selected.size()) + " outbound peer(s)");
    for (const PeerEndpoint& endpoint : selected) {
        outbound_threads_.emplace_back([this, endpoint] { outbound_loop(endpoint); });
    }
}

void PersistentTestnetNode::request_stop() {
    stop_requested_ = true;
    std::lock_guard lock(session_state_mutex_);
    for (const Socket socket : active_sockets_) {
        shutdown_socket(socket);
    }
}

void PersistentTestnetNode::reap_sessions() {
    std::vector<SessionThread> completed;
    {
        std::lock_guard lock(session_threads_mutex_);
        auto iterator = session_threads_.begin();
        while (iterator != session_threads_.end()) {
            if (iterator->done->load()) {
                completed.push_back(std::move(*iterator));
                iterator = session_threads_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }
    for (SessionThread& session : completed) {
        if (session.thread.joinable()) session.thread.join();
    }
}

void PersistentTestnetNode::wait() {
    log_debug("net", "wait begin port=" + std::to_string(port_));
    if (accept_thread_.joinable()) {
        log_debug("net", "joining accept thread port=" + std::to_string(port_));
        accept_thread_.join();
    }
    if (listen_socket_ != INVALID_SOCKET_VALUE) {
        close_socket(listen_socket_);
        listen_socket_ = INVALID_SOCKET_VALUE;
    }
    log_debug("net", "joining outbound threads port=" + std::to_string(port_) +
        " count=" + std::to_string(outbound_threads_.size()));
    for (std::thread& thread : outbound_threads_) {
        if (thread.joinable()) thread.join();
    }
    outbound_threads_.clear();

    std::vector<SessionThread> sessions;
    {
        std::lock_guard lock(session_threads_mutex_);
        sessions.swap(session_threads_);
    }
    log_debug("net", "joining session threads port=" + std::to_string(port_) +
        " count=" + std::to_string(sessions.size()));
    for (SessionThread& session : sessions) {
        if (session.thread.joinable()) session.thread.join();
    }
    log_debug("net", "wait complete port=" + std::to_string(port_));
}

std::uint16_t PersistentTestnetNode::bound_port() const noexcept { return port_; }
std::uint64_t PersistentTestnetNode::node_nonce() const noexcept { return node_nonce_; }

LiveNodeStats PersistentTestnetNode::stats() const noexcept {
    return LiveNodeStats{
        .active_peers = active_peers_.load(),
        .active_inbound = active_inbound_.load(),
        .accepted_connections = accepted_connections_.load(),
        .outbound_connections = outbound_connections_.load(),
        .relayed_blocks = relayed_blocks_.load(),
        .relayed_transactions = relayed_transactions_.load(),
        .headers_received = headers_received_.load(),
        .addresses_received = addresses_received_.load(),
        .rejected_messages = rejected_messages_.load(),
        .rejected_connections = rejected_connections_.load(),
        .duplicate_connections = duplicate_connections_.load(),
        .self_connections = self_connections_.load(),
        .banned_connections = banned_connections_.load(),
        .timeout_disconnects = timeout_disconnects_.load(),
        .rate_limited_connections = rate_limited_connections_.load(),
    };
}

std::vector<LivePeerInfo> PersistentTestnetNode::peer_info() const {
    std::lock_guard lock(live_peers_mutex_);
    std::vector<LivePeerInfo> peers;
    peers.reserve(live_peers_.size());
    for (const auto& [id, peer] : live_peers_) {
        (void)id;
        peers.push_back(peer);
    }
    return peers;
}

std::mutex& PersistentTestnetNode::data_mutex() noexcept { return data_mutex_; }

bool PersistentTestnetNode::peer_is_banned(const PeerEndpoint& endpoint) {
    if (endpoint.host.empty()) return false;
    std::lock_guard lock(ban_store_mutex_);
    BanStore bans(data_directory_);
    bans.load();
    return bans.is_banned(endpoint.host);
}

void PersistentTestnetNode::ban_peer(
    const PeerEndpoint& endpoint,
    const std::uint32_t score,
    const std::string& reason
) {
    if (endpoint.host.empty() || policy_.ban_seconds <= 0) return;
    // Testnet commonly runs many independent nodes on loopback. Never persist a
    // host-wide loopback ban because one malformed local test peer would block
    // every legitimate local node.
    if (endpoint.host == "127.0.0.1" || endpoint.host == "::1") return;
    std::lock_guard lock(ban_store_mutex_);
    BanStore bans(data_directory_);
    bans.load();
    bans.ban(endpoint.host, policy_.ban_seconds, score, reason);
}

void PersistentTestnetNode::start_session(
    const Socket socket,
    const bool outbound,
    PeerEndpoint endpoint
) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    SessionThread session;
    session.done = done;
    session.thread = std::thread([this, socket, outbound, endpoint = std::move(endpoint), done] {
        try {
            session_loop(socket, outbound, endpoint);
        } catch (const std::exception&) {
            // A single peer must never terminate the persistent node.
        }
        done->store(true);
    });
    {
        std::lock_guard lock(session_threads_mutex_);
        session_threads_.push_back(std::move(session));
    }
    reap_sessions();
}

void PersistentTestnetNode::accept_loop() {
    while (!stop_requested_) {
        if (!wait_socket_readable(listen_socket_, 200)) continue;
        if (stop_requested_) return;

        sockaddr_storage remote{};
        SocketLength remote_length = sizeof(remote);
        const Socket accepted = ::accept(
            listen_socket_, reinterpret_cast<sockaddr*>(&remote), &remote_length);
        if (accepted == INVALID_SOCKET_VALUE) {
            if (stop_requested_) return;
            if (socket_error_interrupted(last_socket_error())) continue;
            continue;
        }
        const PeerEndpoint endpoint = endpoint_from_sockaddr(remote);
        log_info("net", "Accepted connection from " + endpoint.host + ':' + std::to_string(endpoint.port));
        if (peer_is_banned(endpoint)) {
            log_info("net", "Rejected banned peer " + endpoint.host);
            ++banned_connections_;
            close_socket(accepted);
            continue;
        }
        const std::size_t previous = active_inbound_.fetch_add(1);
        if (previous >= policy_.max_inbound) {
            --active_inbound_;
            ++rejected_connections_;
            close_socket(accepted);
            continue;
        }
        try {
            configure_socket(accepted, policy_.socket_timeout_seconds);
            ++accepted_connections_;
            start_session(accepted, false, endpoint);
        } catch (...) {
            --active_inbound_;
            close_socket(accepted);
        }
    }
}

void PersistentTestnetNode::outbound_loop(PeerEndpoint endpoint) {
    while (!stop_requested_) {
        if (peer_is_banned(endpoint)) {
            ++banned_connections_;
        } else {
            try {
                log_debug("net", "Trying outbound peer " + endpoint.host + ':' + std::to_string(endpoint.port));
                SocketHandle socket = connect_socket(endpoint);
                configure_socket(socket.get(), policy_.socket_timeout_seconds);
                ++outbound_connections_;
                log_info("net", "Connected outbound to " + endpoint.host + ':' + std::to_string(endpoint.port));
                session_loop(socket.release(), true, endpoint);
            } catch (const std::exception& error) {
                log_debug("net", "Outbound peer failed " + endpoint.host + ':' +
                    std::to_string(endpoint.port) + " reason=" + error.what());
                std::lock_guard lock(peer_store_mutex_);
                PeerStore peer_store(data_directory_);
                peer_store.load();
                peer_store.mark_failure(endpoint);
            }
        }
        for (int index = 0; index < 10 && !stop_requested_; ++index) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void PersistentTestnetNode::session_loop(
    const Socket raw_socket,
    const bool outbound,
    const PeerEndpoint endpoint
) {
    SocketHandle socket(raw_socket);
    ++active_peers_;
    {
        std::lock_guard lock(session_state_mutex_);
        active_sockets_.insert(raw_socket);
    }
    std::uint64_t registered_nonce = 0;
    std::uint64_t peer_id = 0;
    auto cleanup = std::shared_ptr<void>(nullptr, [&](void*) {
        {
            std::lock_guard lock(session_state_mutex_);
            active_sockets_.erase(raw_socket);
            if (registered_nonce != 0) active_peer_nonces_.erase(registered_nonce);
        }
        if (peer_id != 0) {
            std::lock_guard peer_lock(live_peers_mutex_);
            live_peers_.erase(peer_id);
        }
        if (peer_id != 0) {
            log_info("net", "Disconnected peer=" + std::to_string(peer_id) + " addr=" +
                endpoint.host + ':' + std::to_string(endpoint.port));
        }
        --active_peers_;
        if (!outbound) --active_inbound_;
    });

    TestnetChain handshake_chain(data_directory_);
    {
        std::lock_guard lock(data_mutex_);
        handshake_chain.initialize();
    }
    PeerVersion peer;
    try {
        peer = outbound
            ? client_handshake(socket.get(), handshake_chain, node_nonce_)
            : server_handshake(socket.get(), handshake_chain, node_nonce_);
    } catch (const std::exception& error) {
        ++rejected_messages_;
        log_info("net", "Handshake failed with " + endpoint.host + ':' +
            std::to_string(endpoint.port) + " reason=" + error.what());
        ban_peer(endpoint, policy_.ban_threshold, "invalid-handshake");
        throw;
    }

    {
        std::lock_guard lock(session_state_mutex_);
        if (peer.nonce == node_nonce_) {
            ++self_connections_;
            return;
        }
        if (!active_peer_nonces_.insert(peer.nonce).second) {
            ++duplicate_connections_;
            return;
        }
        registered_nonce = peer.nonce;
    }

    peer_id = next_peer_id_.fetch_add(1) + 1;
    const auto connected_now = static_cast<std::int64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    {
        std::lock_guard peer_lock(live_peers_mutex_);
        live_peers_.insert_or_assign(peer_id, LivePeerInfo{
            .id = peer_id,
            .endpoint = endpoint,
            .inbound = !outbound,
            .connected_since = connected_now,
            .protocol_version = peer.protocol_version,
            .services = peer.services,
            .starting_height = peer.best_height,
            .user_agent = peer.user_agent,
            .tip_hash = peer.tip_hash,
            .chainwork = peer.chainwork,
        });
    }
    log_info("net", "New " + std::string(outbound ? "outbound" : "inbound") +
        " peer connected peer=" + std::to_string(peer_id) + " addr=" + endpoint.host + ':' +
        std::to_string(endpoint.port) + " version=" + std::to_string(peer.protocol_version) +
        " subver=" + peer.user_agent + " height=" + std::to_string(peer.best_height));

    if (outbound) {
        std::lock_guard lock(peer_store_mutex_);
        PeerStore store(data_directory_);
        store.load();
        store.mark_success(endpoint, peer.services);
    }
    send_message(socket.get(), "getaddr");

    struct CandidateSync {
        bool active{false};
        std::uint64_t expected_height{0};
        std::string expected_tip;
        std::string expected_chainwork;
        std::uint64_t fork_height{0};
        std::vector<StoredBlock> prefix;
        std::vector<HeaderRecord> headers;
        std::map<std::string, StoredBlock> blocks;
        std::set<std::string> pending;
    } candidate;

    std::set<std::string> remote_knows_blocks;
    std::set<std::string> remote_knows_transactions;
    remote_knows_blocks.insert(peer.tip_hash);
    int misbehavior_score = 0;

    auto request_headers = [&](const std::vector<std::string>& locator) {
        send_message(socket.get(), "getheaders", serialize_getheaders(locator));
    };

    auto start_candidate_sync = [&](const std::uint64_t height,
                                    const std::string& tip,
                                    const std::string& work) {
        if (height > 1'000'000) throw std::runtime_error("Peer chain height exceeds safety limit");
        TestnetChain chain(data_directory_);
        {
            std::lock_guard lock(data_mutex_);
            chain.load();
        }
        candidate = CandidateSync{
            .active = true,
            .expected_height = height,
            .expected_tip = tip,
            .expected_chainwork = work,
            .fork_height = chain.tip().height,
            .prefix = chain.blocks(),
            .headers = {},
            .blocks = {},
            .pending = {},
        };
        request_headers(make_block_locator(chain.blocks()));
    };

    auto activate_candidate = [&] {
        if (!candidate.active) return;
        std::vector<StoredBlock> full = candidate.prefix;
        for (const HeaderRecord& header : candidate.headers) {
            const std::string hash = header.header.hash_hex();
            const auto iterator = candidate.blocks.find(hash);
            if (iterator == candidate.blocks.end()) {
                throw std::runtime_error("Candidate block body is missing after headers-first download");
            }
            if (iterator->second.height != header.height ||
                iterator->second.block.header.serialize() != header.header.serialize()) {
                throw std::runtime_error("Candidate block body does not match announced header");
            }
            full.push_back(iterator->second);
        }
        if (full.empty() || full.back().height != candidate.expected_height ||
            full.back().block.header.hash_hex() != candidate.expected_tip) {
            throw std::runtime_error("Headers-first candidate tip does not match announcement");
        }
        const std::string calculated_work = uint256_to_hex(TestnetChain::calculate_chainwork(full));
        if (!candidate.expected_chainwork.empty() && calculated_work != candidate.expected_chainwork) {
            throw std::runtime_error("Headers-first candidate chainwork does not match announcement");
        }

        std::lock_guard lock(data_mutex_);
        TestnetChain chain(data_directory_);
        chain.load();
        SideBranchStore branches(data_directory_);
        branches.load();
        if (candidate.fork_height + 1 < chain.blocks().size()) {
            std::vector<StoredBlock> disconnected(
                chain.blocks().begin() + static_cast<std::ptrdiff_t>(candidate.fork_height + 1),
                chain.blocks().end());
            branches.put_many(disconnected);
        }
        const ChainActivationResult activation = chain.activate_if_more_work(full);
        if (activation.activated) {
            branches.remove_active(chain.blocks());
        }
        TestnetMempool mempool(data_directory_);
        mempool.load();
        mempool.revalidate(chain.utxo_set(), chain.tip().height + 1, chain.confirmed_txids());
        for (const Transaction& transaction : activation.disconnected_transactions) {
            try {
                mempool.add(transaction, chain.utxo_set(), chain.tip().height + 1);
            } catch (const std::exception&) {
                // A winning branch may make a disconnected transaction invalid.
            }
        }
        if (activation.activated) {
            relayed_blocks_.fetch_add(static_cast<std::uint64_t>(activation.connected_blocks));
        }
        candidate = CandidateSync{};
    };

    auto request_candidate_blocks = [&] {
        SideBranchStore branches(data_directory_);
        branches.load();
        TestnetChain chain(data_directory_);
        {
            std::lock_guard lock(data_mutex_);
            chain.load();
        }
        for (const HeaderRecord& header : candidate.headers) {
            const std::string hash = header.header.hash_hex();
            if (const auto active = find_block(chain, hash)) {
                candidate.blocks.emplace(hash, *active);
                continue;
            }
            if (const auto stored = branches.find(hash)) {
                candidate.blocks.emplace(hash, *stored);
                continue;
            }
            candidate.pending.insert(hash);
        }
        if (candidate.pending.empty()) {
            activate_candidate();
            return;
        }
        std::vector<InventoryItem> batch;
        for (const std::string& hash : candidate.pending) {
            batch.push_back(InventoryItem{.type = InventoryType::Block, .hash = hash});
            if (batch.size() == MAX_INVENTORY_ITEMS) {
                send_message(socket.get(), "getdata", serialize_inventory(batch));
                batch.clear();
            }
        }
        if (!batch.empty()) send_message(socket.get(), "getdata", serialize_inventory(batch));
    };

    if (outbound) {
        bool should_sync = false;
        {
            std::lock_guard lock(data_mutex_);
            TestnetChain chain(data_directory_);
            chain.load();
            should_sync = peer.tip_hash != chain.tip().block.header.hash_hex() &&
                          peer.chainwork > chain.chainwork_hex();
        }
        if (should_sync) start_candidate_sync(peer.best_height, peer.tip_hash, peer.chainwork);
    }

    auto last_inventory_scan = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto last_ping = std::chrono::steady_clock::now();
    auto ping_sent = last_ping;
    std::optional<std::uint64_t> outstanding_ping;
    auto message_window_started = std::chrono::steady_clock::now();
    std::size_t messages_in_window = 0;

    while (!stop_requested_) {
        if (misbehavior_score >= static_cast<int>(policy_.ban_threshold)) {
            ban_peer(endpoint, static_cast<std::uint32_t>(misbehavior_score), "misbehavior-threshold");
            return;
        }
        try {
            if (socket_readable(socket.get(), 100)) {
                const Message message = receive_message(socket.get());
                const auto received_at = std::chrono::steady_clock::now();
                if (received_at - message_window_started >= policy_.message_window) {
                    message_window_started = received_at;
                    messages_in_window = 0;
                }
                ++messages_in_window;
                if (messages_in_window > policy_.max_messages_per_window) {
                    ++rate_limited_connections_;
                    ++rejected_messages_;
                    ban_peer(endpoint, policy_.ban_threshold, "message-rate-limit");
                    return;
                }

                if (message.command == "getheaders") {
                    const GetHeadersRequest request = deserialize_getheaders(message.payload);
                    std::lock_guard lock(data_mutex_);
                    TestnetChain chain(data_directory_);
                    chain.load();
                    send_message(socket.get(), "headers", serialize_headers(headers_after_locator(chain, request)));
                    continue;
                }

                if (message.command == "headers") {
                    if (!candidate.active) {
                        misbehavior_score += 10;
                        ++rejected_messages_;
                        continue;
                    }
                    const auto headers = deserialize_headers(message.payload);
                    if (headers.empty()) {
                        throw std::runtime_error("Peer ended headers before advertised tip");
                    }
                    if (candidate.headers.empty()) {
                        std::size_t fork_index = candidate.prefix.size();
                        for (std::size_t index = candidate.prefix.size(); index-- > 0;) {
                            if (candidate.prefix[index].block.header.hash_hex() ==
                                headers.front().header.previous_hash) {
                                fork_index = index;
                                break;
                            }
                        }
                        if (fork_index == candidate.prefix.size() ||
                            headers.front().height != candidate.prefix[fork_index].height + 1) {
                            throw std::runtime_error("Headers do not connect to a locator block");
                        }
                        candidate.fork_height = candidate.prefix[fork_index].height;
                        candidate.prefix.resize(fork_index + 1);
                    } else {
                        const HeaderRecord& previous = candidate.headers.back();
                        if (headers.front().height != previous.height + 1 ||
                            headers.front().header.previous_hash != previous.header.hash_hex()) {
                            throw std::runtime_error("Headers response is not contiguous");
                        }
                    }
                    std::vector<StoredBlock> difficulty_history = candidate.prefix;
                    for (const HeaderRecord& previous_header : candidate.headers) {
                        difficulty_history.push_back(StoredBlock{
                            .height = previous_header.height,
                            .block = Block{.header = previous_header.header, .transactions = {}},
                        });
                    }
                    for (const HeaderRecord& header : headers) {
                        if (header.header.bits != expected_testnet_bits(std::span<const StoredBlock>(difficulty_history), header.height)) {
                            throw std::runtime_error("Candidate header has unexpected difficulty");
                        }
                        if (!candidate.headers.empty()) {
                            const HeaderRecord& previous = candidate.headers.back();
                            if (header.height != previous.height + 1 ||
                                header.header.previous_hash != previous.header.hash_hex() ||
                                header.header.time <= previous.header.time) {
                                throw std::runtime_error("Invalid headers-first chain linkage");
                            }
                        } else if (header.header.time <= candidate.prefix.back().block.header.time) {
                            throw std::runtime_error("First candidate header timestamp is not increasing");
                        }
                        candidate.headers.push_back(header);
                        difficulty_history.push_back(StoredBlock{
                            .height = header.height,
                            .block = Block{.header = header.header, .transactions = {}},
                        });
                    }
                    headers_received_.fetch_add(static_cast<std::uint64_t>(headers.size()));
                    const HeaderRecord& last = candidate.headers.back();
                    if (last.height > candidate.expected_height) {
                        throw std::runtime_error("Peer sent headers beyond advertised height");
                    }
                    if (last.header.hash_hex() == candidate.expected_tip) {
                        if (last.height != candidate.expected_height) {
                            throw std::runtime_error("Advertised tip height does not match headers");
                        }
                        request_candidate_blocks();
                    } else {
                        request_headers({last.header.hash_hex(), TESTNET_GENESIS_HASH});
                    }
                    continue;
                }

                if (message.command == "getaddr") {
                    if (!message.payload.empty()) throw std::runtime_error("getaddr payload must be empty");
                    std::lock_guard lock(peer_store_mutex_);
                    PeerStore store(data_directory_);
                    store.load();
                    send_message(socket.get(), "addrv2", serialize_addrv2(store.records()));
                    continue;
                }

                if (message.command == "addrv2") {
                    const auto records = deserialize_addrv2(message.payload);
                    const auto now = static_cast<std::int64_t>(
                        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
                    std::lock_guard lock(peer_store_mutex_);
                    PeerStore store(data_directory_);
                    store.load();
                    std::size_t accepted = 0;
                    for (PeerRecord record : records) {
                        if ((record.endpoint.host == bind_address_ ||
                             (record.endpoint.host == "127.0.0.1" && bind_address_ == "0.0.0.0")) &&
                            record.endpoint.port == port_) continue;
                        record.last_seen = std::min(record.last_seen, now + 600);
                        store.add_or_update(record);
                        ++accepted;
                    }
                    store.save();
                    addresses_received_.fetch_add(static_cast<std::uint64_t>(accepted));
                    continue;
                }

                if (message.command == "getblocks") {
                    std::size_t offset = 0;
                    const std::uint64_t start_height = read_u64_le(message.payload, offset);
                    const std::uint32_t requested = read_u32_le(message.payload, offset);
                    if (offset != message.payload.size() || requested == 0 ||
                        requested > MAX_BLOCKS_PER_RESPONSE) {
                        throw std::runtime_error("Malformed legacy getblocks request");
                    }
                    std::lock_guard lock(data_mutex_);
                    TestnetChain chain(data_directory_);
                    chain.load();
                    const std::size_t begin = start_height > chain.tip().height
                        ? chain.blocks().size()
                        : static_cast<std::size_t>(start_height);
                    send_message(socket.get(), "blocks",
                                 serialize_stored_blocks(chain.blocks(), begin, requested));
                    continue;
                }

                if (message.command == "blocks") {
                    misbehavior_score += 10;
                    ++rejected_messages_;
                    continue;
                }

                if (message.command == "getmempool") {
                    if (!message.payload.empty()) throw std::runtime_error("getmempool payload must be empty");
                    std::lock_guard lock(data_mutex_);
                    TestnetMempool mempool(data_directory_);
                    mempool.load();
                    send_message(socket.get(), "txs", serialize_transactions(mempool.transactions()));
                    continue;
                }

                if (message.command == "getstatus") {
                    if (!message.payload.empty()) throw std::runtime_error("getstatus payload must be empty");
                    std::lock_guard lock(data_mutex_);
                    TestnetChain chain(data_directory_);
                    chain.load();
                    send_message(socket.get(), "status", serialize_version_impl(local_version(chain)));
                    continue;
                }

                if (message.command == "status") {
                    const PeerVersion status = deserialize_version_impl(message.payload);
                    bool should_sync = false;
                    {
                        std::lock_guard lock(data_mutex_);
                        TestnetChain chain(data_directory_);
                        chain.load();
                        should_sync = !candidate.active &&
                                      status.tip_hash != chain.tip().block.header.hash_hex() &&
                                      status.chainwork > chain.chainwork_hex();
                    }
                    if (should_sync) {
                        start_candidate_sync(status.best_height, status.tip_hash, status.chainwork);
                    }
                    continue;
                }

                if (message.command == "inv") {
                    const auto inventory = deserialize_inventory(message.payload);
                    std::vector<InventoryItem> missing;
                    {
                        std::lock_guard lock(data_mutex_);
                        TestnetChain chain(data_directory_);
                        chain.load();
                        TestnetMempool mempool(data_directory_);
                        mempool.load();
                        SideBranchStore branches(data_directory_);
                        branches.load();
                        for (const InventoryItem& item : inventory) {
                            if (item.type == InventoryType::Block) {
                                remote_knows_blocks.insert(item.hash);
                                if (!chain_has_block(chain, item.hash) && !branches.find(item.hash)) {
                                    missing.push_back(item);
                                }
                            } else {
                                remote_knows_transactions.insert(item.hash);
                                if (!node_has_transaction(chain, mempool, item.hash)) missing.push_back(item);
                            }
                        }
                    }
                    if (!missing.empty()) send_message(socket.get(), "getdata", serialize_inventory(missing));
                    continue;
                }

                if (message.command == "getdata") {
                    const auto inventory = deserialize_inventory(message.payload);
                    std::vector<InventoryItem> not_found;
                    std::lock_guard lock(data_mutex_);
                    TestnetChain chain(data_directory_);
                    chain.load();
                    TestnetMempool mempool(data_directory_);
                    mempool.load();
                    SideBranchStore branches(data_directory_);
                    branches.load();
                    for (const InventoryItem& item : inventory) {
                        if (item.type == InventoryType::Block) {
                            auto block = find_block(chain, item.hash);
                            if (!block) block = branches.find(item.hash);
                            if (block) {
                                const std::vector<StoredBlock> one{*block};
                                send_message(socket.get(), "block", serialize_stored_blocks(one, 0, 1));
                            } else {
                                not_found.push_back(item);
                            }
                        } else {
                            const auto transaction = find_transaction(chain, mempool, item.hash);
                            if (transaction) {
                                send_message(socket.get(), "tx", transaction->serialize());
                            } else {
                                not_found.push_back(item);
                            }
                        }
                    }
                    if (!not_found.empty()) {
                        send_message(socket.get(), "notfound", serialize_inventory(not_found));
                    }
                    continue;
                }

                if (message.command == "tx") {
                    const Transaction transaction = Transaction::deserialize(message.payload);
                    remote_knows_transactions.insert(transaction.txid());
                    try {
                        std::lock_guard lock(data_mutex_);
                        TestnetChain chain(data_directory_);
                        chain.load();
                        TestnetMempool mempool(data_directory_);
                        mempool.load();
                        if (!node_has_transaction(chain, mempool, transaction.txid())) {
                            mempool.add(transaction, chain.utxo_set(), chain.tip().height + 1);
                            ++relayed_transactions_;
                        }
                    } catch (const std::exception&) {
                        misbehavior_score += 25;
                        ++rejected_messages_;
                    }
                    continue;
                }

                if (message.command == "block") {
                    const auto blocks = deserialize_stored_blocks(message.payload);
                    if (blocks.size() != 1) throw std::runtime_error("block message must contain one block");
                    const StoredBlock stored = blocks.front();
                    const std::string hash = stored.block.header.hash_hex();
                    remote_knows_blocks.insert(hash);

                    if (candidate.active && candidate.pending.contains(hash)) {
                        const auto header_iterator = std::find_if(
                            candidate.headers.begin(), candidate.headers.end(),
                            [&](const HeaderRecord& header) { return header.header.hash_hex() == hash; });
                        if (header_iterator == candidate.headers.end() ||
                            header_iterator->height != stored.height ||
                            header_iterator->header.serialize() != stored.block.header.serialize()) {
                            throw std::runtime_error("Received candidate block does not match requested header");
                        }
                        SideBranchStore branches(data_directory_);
                        branches.load();
                        branches.put(stored);
                        branches.save();
                        candidate.blocks.emplace(hash, stored);
                        candidate.pending.erase(hash);
                        if (candidate.pending.empty()) activate_candidate();
                        continue;
                    }

                    bool side_branch = false;
                    try {
                        std::lock_guard lock(data_mutex_);
                        TestnetChain chain(data_directory_);
                        chain.load();
                        if (chain_has_block(chain, hash)) continue;
                        if (stored.height == chain.tip().height + 1 &&
                            stored.block.header.previous_hash == chain.tip().block.header.hash_hex()) {
                            chain.append_blocks({stored});
                            SideBranchStore branches(data_directory_);
                            branches.load();
                            branches.remove_active(chain.blocks());
                            TestnetMempool mempool(data_directory_);
                            mempool.load();
                            mempool.revalidate(
                                chain.utxo_set(), chain.tip().height + 1, chain.confirmed_txids());
                            ++relayed_blocks_;
                        } else {
                            SideBranchStore branches(data_directory_);
                            branches.load();
                            branches.put(stored);
                            branches.save();
                            side_branch = true;
                        }
                    } catch (const std::exception&) {
                        misbehavior_score += 50;
                        ++rejected_messages_;
                    }
                    if (side_branch && !candidate.active) {
                        send_message(socket.get(), "getstatus");
                    }
                    continue;
                }

                if (message.command == "ping") {
                    if (message.payload.size() != 8) throw std::runtime_error("ping nonce must be 8 bytes");
                    send_message(socket.get(), "pong", message.payload);
                    continue;
                }
                if (message.command == "pong") {
                    if (message.payload.size() != 8) {
                        misbehavior_score += 10;
                        ++rejected_messages_;
                        continue;
                    }
                    std::size_t pong_offset = 0;
                    const std::uint64_t nonce = read_u64_le(message.payload, pong_offset);
                    if (!outstanding_ping || nonce != *outstanding_ping) {
                        misbehavior_score += 5;
                        ++rejected_messages_;
                    } else {
                        outstanding_ping.reset();
                    }
                    continue;
                }
                if (message.command == "txack" || message.command == "notfound") {
                    continue;
                }
                if (message.command == "bye") return;

                misbehavior_score += 10;
                ++rejected_messages_;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - last_inventory_scan >= std::chrono::milliseconds(200)) {
                std::vector<InventoryItem> inventory;
                try {
                    std::lock_guard lock(data_mutex_);
                    TestnetChain chain(data_directory_);
                    chain.load();
                    TestnetMempool mempool(data_directory_);
                    mempool.load();
                    const std::string tip_hash = chain.tip().block.header.hash_hex();
                    if (!remote_knows_blocks.contains(tip_hash)) {
                        inventory.push_back(InventoryItem{.type = InventoryType::Block, .hash = tip_hash});
                        remote_knows_blocks.insert(tip_hash);
                    }
                    for (const Transaction& transaction : mempool.transactions()) {
                        if (inventory.size() >= MAX_INVENTORY_ITEMS) break;
                        const std::string txid = transaction.txid();
                        if (!remote_knows_transactions.contains(txid)) {
                            inventory.push_back(InventoryItem{
                                .type = InventoryType::Transaction,
                                .hash = txid,
                            });
                            remote_knows_transactions.insert(txid);
                        }
                    }
                } catch (const std::exception&) {
                    // Another local process may be atomically replacing a file.
                    // Retry during the next scan rather than dropping the peer.
                }
                if (!inventory.empty()) send_message(socket.get(), "inv", serialize_inventory(inventory));
                last_inventory_scan = now;
            }

            if (outstanding_ping && now - ping_sent >= policy_.ping_timeout) {
                ++timeout_disconnects_;
                return;
            }
            if (!outstanding_ping && now - last_ping >= policy_.ping_interval) {
                const std::uint64_t nonce = random_nonce();
                std::vector<std::uint8_t> ping;
                append_u64_le(ping, nonce);
                send_message(socket.get(), "ping", ping);
                outstanding_ping = nonce;
                ping_sent = now;
                last_ping = now;
            }

            if (misbehavior_score >= static_cast<int>(policy_.ban_threshold)) {
                ban_peer(endpoint, static_cast<std::uint32_t>(misbehavior_score), "misbehavior-threshold");
                return;
            }
        } catch (const std::runtime_error& error) {
            const std::string message = error.what();
            if (message.starts_with("recv failed:") ||
                message.starts_with("send failed:") ||
                message.starts_with("select failed:")) {
                ++timeout_disconnects_;
                return;
            }
            if (message.find("closed") != std::string::npos ||
                message.find("Connection reset") != std::string::npos ||
                message.find("Broken pipe") != std::string::npos) {
                return;
            }
            if (message.find("Resource temporarily unavailable") != std::string::npos ||
                message.find("Operation timed out") != std::string::npos) {
                ++timeout_disconnects_;
                return;
            }
            ++rejected_messages_;
            misbehavior_score += 25;
            if (misbehavior_score >= static_cast<int>(policy_.ban_threshold)) {
                ban_peer(endpoint, static_cast<std::uint32_t>(misbehavior_score), message);
                return;
            }
        }
    }

    try {
        send_message(socket.get(), "bye");
    } catch (const std::exception&) {
    }
}

SyncResult sync_from_peer(
    const std::filesystem::path& data_directory,
    const PeerEndpoint& endpoint
) {
    TestnetChain chain(data_directory);
    chain.initialize();
    TestnetMempool mempool(data_directory);
    mempool.load();
    PeerStore peer_store(data_directory);
    peer_store.load();
    SideBranchStore branches(data_directory);
    branches.load();

    const std::uint64_t before = chain.tip().height;
    const std::string before_work = chain.chainwork_hex();
    SocketHandle socket = connect_socket(endpoint);
    const PeerVersion peer = client_handshake(socket.get(), chain);
    peer_store.mark_success(endpoint, peer.services);

    auto receive_until = [&](const std::vector<std::string_view>& expected) -> Message {
        for (std::size_t attempts = 0; attempts < 10'000; ++attempts) {
            Message message = receive_message(socket.get());
            if (std::any_of(expected.begin(), expected.end(), [&](const std::string_view command) {
                    return message.command == command;
                })) {
                return message;
            }
            if (message.command == "getaddr") {
                send_message(socket.get(), "addrv2", serialize_addrv2(peer_store.records()));
                continue;
            }
            if (message.command == "addrv2") {
                for (const PeerRecord& record : deserialize_addrv2(message.payload)) {
                    peer_store.add_or_update(record);
                }
                peer_store.save();
                continue;
            }
            if (message.command == "ping") {
                if (message.payload.size() != 8) throw std::runtime_error("ping nonce must be 8 bytes");
                send_message(socket.get(), "pong", message.payload);
                continue;
            }
            if (message.command == "getstatus") {
                chain.load();
                send_message(socket.get(), "status", serialize_version_impl(local_version(chain)));
                continue;
            }
            if (message.command == "inv" || message.command == "status" ||
                message.command == "pong" || message.command == "txack") {
                continue;
            }
            throw std::runtime_error(
                "Unexpected P2P command while waiting for response: " + message.command);
        }
        throw std::runtime_error("Too many unrelated P2P messages while waiting for response");
    };

    send_message(socket.get(), "getaddr");
    const Message address_response = receive_until({"addrv2"});
    for (const PeerRecord& record : deserialize_addrv2(address_response.payload)) {
        peer_store.add_or_update(record);
    }
    peer_store.save();

    ChainActivationResult activation;
    activation.old_height = before;
    activation.new_height = before;
    activation.fork_height = before;
    activation.old_chainwork = chain.chainwork();
    activation.candidate_chainwork = chain.chainwork();
    std::size_t headers_received_count = 0;
    std::size_t received_blocks = 0;
    std::size_t reused_blocks = 0;
    std::string candidate_work = before_work;

    if (peer.tip_hash == chain.tip().block.header.hash_hex()) {
        if (peer.best_height != chain.tip().height || peer.chainwork != before_work) {
            throw std::runtime_error("Peer announced inconsistent height or chainwork for matching tip");
        }
    } else if (peer.chainwork >= before_work) {
        if (peer.best_height > 1'000'000) {
            throw std::runtime_error("Peer testnet chain height exceeds synchronization safety limit");
        }

        std::vector<HeaderRecord> headers;
        std::vector<std::string> locator = make_block_locator(chain.blocks());
        std::size_t fork_index = chain.blocks().size();
        while (true) {
            send_message(socket.get(), "getheaders", serialize_getheaders(locator));
            const Message response = receive_until({"headers"});
            expect_command(response, "headers");
            const auto batch = deserialize_headers(response.payload);
            if (batch.empty()) throw std::runtime_error("Peer stopped headers before advertised tip");

            if (headers.empty()) {
                for (std::size_t index = chain.blocks().size(); index-- > 0;) {
                    if (chain.blocks()[index].block.header.hash_hex() ==
                        batch.front().header.previous_hash) {
                        fork_index = index;
                        break;
                    }
                }
                if (fork_index == chain.blocks().size() ||
                    batch.front().height != chain.blocks()[fork_index].height + 1) {
                    throw std::runtime_error("Peer headers do not connect to local block locator");
                }
            } else if (batch.front().height != headers.back().height + 1 ||
                       batch.front().header.previous_hash != headers.back().header.hash_hex()) {
                throw std::runtime_error("Peer returned non-contiguous header batches");
            }

            std::vector<StoredBlock> difficulty_history(
                chain.blocks().begin(),
                chain.blocks().begin() + static_cast<std::ptrdiff_t>(fork_index + 1));
            for (const HeaderRecord& previous_header : headers) {
                difficulty_history.push_back(StoredBlock{
                    .height = previous_header.height,
                    .block = Block{.header = previous_header.header, .transactions = {}},
                });
            }
            for (const HeaderRecord& header : batch) {
                if (header.header.bits != expected_testnet_bits(std::span<const StoredBlock>(difficulty_history), header.height)) {
                    throw std::runtime_error("Peer header has unexpected difficulty");
                }
                if (!headers.empty()) {
                    const HeaderRecord& previous = headers.back();
                    if (header.height != previous.height + 1 ||
                        header.header.previous_hash != previous.header.hash_hex() ||
                        header.header.time <= previous.header.time) {
                        throw std::runtime_error("Invalid headers-first sequence");
                    }
                } else if (header.header.time <= chain.blocks()[fork_index].block.header.time) {
                    throw std::runtime_error("Candidate header time does not increase from fork");
                }
                headers.push_back(header);
                difficulty_history.push_back(StoredBlock{
                    .height = header.height,
                    .block = Block{.header = header.header, .transactions = {}},
                });
            }
            headers_received_count += batch.size();
            const HeaderRecord& last = headers.back();
            if (last.height > peer.best_height) {
                throw std::runtime_error("Peer headers exceed advertised height");
            }
            if (last.header.hash_hex() == peer.tip_hash) {
                if (last.height != peer.best_height) {
                    throw std::runtime_error("Peer tip height does not match final header");
                }
                break;
            }
            locator = {last.header.hash_hex(), TESTNET_GENESIS_HASH};
        }

        std::vector<StoredBlock> candidate(
            chain.blocks().begin(),
            chain.blocks().begin() + static_cast<std::ptrdiff_t>(fork_index + 1));
        std::map<std::string, StoredBlock> bodies;
        std::vector<InventoryItem> missing;
        for (const HeaderRecord& header : headers) {
            const std::string hash = header.header.hash_hex();
            if (const auto stored = branches.find(hash)) {
                if (stored->height == header.height &&
                    stored->block.header.serialize() == header.header.serialize()) {
                    bodies.emplace(hash, *stored);
                    ++reused_blocks;
                    continue;
                }
            }
            missing.push_back(InventoryItem{.type = InventoryType::Block, .hash = hash});
        }

        for (std::size_t begin = 0; begin < missing.size(); begin += MAX_INVENTORY_ITEMS) {
            const std::size_t end_index = std::min(begin + MAX_INVENTORY_ITEMS, missing.size());
            const std::vector<InventoryItem> batch(
                missing.begin() + static_cast<std::ptrdiff_t>(begin),
                missing.begin() + static_cast<std::ptrdiff_t>(end_index));
            send_message(socket.get(), "getdata", serialize_inventory(batch));
            for (std::size_t index = begin; index < end_index; ++index) {
                const Message response = receive_until({"block", "notfound"});
                if (response.command == "notfound") {
                    throw std::runtime_error("Peer did not provide a requested block body");
                }
                expect_command(response, "block");
                const auto decoded = deserialize_stored_blocks(response.payload);
                if (decoded.size() != 1) throw std::runtime_error("Peer block response count mismatch");
                const StoredBlock& stored = decoded.front();
                const std::string hash = stored.block.header.hash_hex();
                const auto header = std::find_if(headers.begin(), headers.end(), [&](const HeaderRecord& item) {
                    return item.header.hash_hex() == hash;
                });
                if (header == headers.end() || stored.height != header->height ||
                    stored.block.header.serialize() != header->header.serialize()) {
                    throw std::runtime_error("Downloaded block does not match announced header");
                }
                bodies.emplace(hash, stored);
                branches.put(stored);
                ++received_blocks;
            }
        }
        branches.save();

        for (const HeaderRecord& header : headers) {
            const auto iterator = bodies.find(header.header.hash_hex());
            if (iterator == bodies.end()) throw std::runtime_error("Missing candidate block body");
            candidate.push_back(iterator->second);
        }
        candidate_work = uint256_to_hex(TestnetChain::calculate_chainwork(candidate));
        if (candidate_work != peer.chainwork) {
            throw std::runtime_error("Peer advertised chainwork does not match validated headers and blocks");
        }

        if (fork_index + 1 < chain.blocks().size()) {
            branches.put_many(std::vector<StoredBlock>(
                chain.blocks().begin() + static_cast<std::ptrdiff_t>(fork_index + 1),
                chain.blocks().end()));
        }
        activation = chain.activate_if_more_work(candidate);
        if (activation.activated) {
            branches.remove_active(chain.blocks());
        }
    }

    const auto confirmed = chain.confirmed_txids();
    mempool.revalidate(chain.utxo_set(), chain.tip().height + 1, confirmed);

    std::size_t resurrected = 0;
    for (const Transaction& transaction : activation.disconnected_transactions) {
        try {
            mempool.add(transaction, chain.utxo_set(), chain.tip().height + 1);
            ++resurrected;
        } catch (const std::exception&) {
        }
    }

    send_message(socket.get(), "getmempool");
    const Message mempool_response = receive_until({"txs"});
    expect_command(mempool_response, "txs");
    const auto remote_transactions = deserialize_transactions(mempool_response.payload);

    std::size_t mempool_received = 0;
    for (const Transaction& transaction : remote_transactions) {
        try {
            mempool.add(transaction, chain.utxo_set(), chain.tip().height + 1);
            ++mempool_received;
        } catch (const std::exception&) {
        }
    }
    send_message(socket.get(), "bye");

    return SyncResult{
        .peer = peer,
        .local_height_before = before,
        .local_height_after = chain.tip().height,
        .local_chainwork_before = before_work,
        .local_chainwork_after = chain.chainwork_hex(),
        .candidate_chainwork = candidate_work,
        .fork_height = activation.fork_height,
        .headers_received = headers_received_count,
        .blocks_received = received_blocks,
        .blocks_reused_from_side_store = reused_blocks,
        .disconnected_blocks = activation.disconnected_blocks,
        .connected_blocks = activation.connected_blocks,
        .resurrected_transactions = resurrected,
        .mempool_received = mempool_received,
        .reorganized = activation.activated && activation.disconnected_blocks != 0,
        .activated_candidate = activation.activated,
    };
}

} // namespace bincoin
