#include "rpc.hpp"

#include "logger.hpp"

#include <algorithm>
#include <cctype>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#ifndef _WIN32
#include <netdb.h>
#include <netinet/in.h>
#endif
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace bincoin {
namespace {

constexpr std::size_t MAX_RPC_REQUEST = 1024U * 1024U;

class SocketHandle {
public:
    explicit SocketHandle(const Socket socket = INVALID_SOCKET_VALUE) : socket_(socket) {}
    ~SocketHandle() { if (socket_ != INVALID_SOCKET_VALUE) close_socket(socket_); }
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    SocketHandle(SocketHandle&& other) noexcept : socket_(other.socket_) { other.socket_ = INVALID_SOCKET_VALUE; }
    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this != &other) {
            if (socket_ != INVALID_SOCKET_VALUE) close_socket(socket_);
            socket_ = other.socket_;
            other.socket_ = INVALID_SOCKET_VALUE;
        }
        return *this;
    }
    [[nodiscard]] Socket get() const noexcept { return socket_; }
    [[nodiscard]] Socket release() noexcept { const Socket socket = socket_; socket_ = INVALID_SOCKET_VALUE; return socket; }
private:
    Socket socket_;
};

[[noreturn]] void socket_error(const std::string& prefix) {
    const int error = last_socket_error();
    throw std::runtime_error(prefix + ": " + socket_error_text(error));
}

std::string base64_encode(const std::string_view input) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2U) / 3U) * 4U);
    std::size_t index = 0;
    while (index + 3U <= input.size()) {
        const auto a = static_cast<unsigned char>(input[index++]);
        const auto b = static_cast<unsigned char>(input[index++]);
        const auto c = static_cast<unsigned char>(input[index++]);
        output.push_back(alphabet[a >> 2U]);
        output.push_back(alphabet[((a & 0x03U) << 4U) | (b >> 4U)]);
        output.push_back(alphabet[((b & 0x0fU) << 2U) | (c >> 6U)]);
        output.push_back(alphabet[c & 0x3fU]);
    }
    const std::size_t remaining = input.size() - index;
    if (remaining == 1U) {
        const auto a = static_cast<unsigned char>(input[index]);
        output.push_back(alphabet[a >> 2U]);
        output.push_back(alphabet[(a & 0x03U) << 4U]);
        output += "==";
    } else if (remaining == 2U) {
        const auto a = static_cast<unsigned char>(input[index]);
        const auto b = static_cast<unsigned char>(input[index + 1U]);
        output.push_back(alphabet[a >> 2U]);
        output.push_back(alphabet[((a & 0x03U) << 4U) | (b >> 4U)]);
        output.push_back(alphabet[(b & 0x0fU) << 2U]);
        output.push_back('=');
    }
    return output;
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) ++begin;
    return value.substr(begin);
}

struct HttpMessage {
    std::string start_line;
    std::map<std::string, std::string> headers;
    std::string body;
};

void send_all(const Socket socket, const std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const std::ptrdiff_t result = socket_send(socket, bytes.data() + sent, bytes.size() - sent, socket_send_flags());
        if (result < 0) {
            if (socket_error_interrupted(last_socket_error())) continue;
            socket_error("RPC send failed");
        }
        if (result == 0) throw std::runtime_error("RPC socket closed while sending");
        sent += static_cast<std::size_t>(result);
    }
}

HttpMessage receive_http(const Socket socket) {
    std::string data;
    data.reserve(4096);
    std::array<char, 4096> buffer{};
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const std::ptrdiff_t count = socket_receive(socket, buffer.data(), buffer.size(), 0);
        if (count < 0) {
            if (socket_error_interrupted(last_socket_error())) continue;
            socket_error("RPC receive failed");
        }
        if (count == 0) throw std::runtime_error("RPC connection closed before HTTP headers");
        data.append(buffer.data(), static_cast<std::size_t>(count));
        if (data.size() > MAX_RPC_REQUEST) throw std::runtime_error("RPC request exceeds size limit");
        header_end = data.find("\r\n\r\n");
    }

    HttpMessage message;
    std::istringstream headers(data.substr(0, header_end));
    std::getline(headers, message.start_line);
    message.start_line = trim(message.start_line);
    std::string line;
    std::size_t content_length = 0;
    while (std::getline(headers, line)) {
        line = trim(line);
        if (line.empty()) continue;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) throw std::runtime_error("Malformed RPC HTTP header");
        std::string key = lower_ascii(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1U));
        message.headers.insert_or_assign(std::move(key), std::move(value));
    }
    if (message.headers.contains("content-length")) {
        std::size_t used = 0;
        const unsigned long long parsed = std::stoull(message.headers.at("content-length"), &used, 10);
        if (used != message.headers.at("content-length").size() || parsed > MAX_RPC_REQUEST) {
            throw std::runtime_error("Invalid RPC Content-Length");
        }
        content_length = static_cast<std::size_t>(parsed);
    }
    const std::size_t body_begin = header_end + 4U;
    while (data.size() - body_begin < content_length) {
        const std::ptrdiff_t count = socket_receive(socket, buffer.data(), buffer.size(), 0);
        if (count < 0) {
            if (socket_error_interrupted(last_socket_error())) continue;
            socket_error("RPC receive failed");
        }
        if (count == 0) throw std::runtime_error("RPC connection closed before HTTP body");
        data.append(buffer.data(), static_cast<std::size_t>(count));
        if (data.size() > MAX_RPC_REQUEST + header_end + 4U) {
            throw std::runtime_error("RPC request exceeds size limit");
        }
    }
    message.body = data.substr(body_begin, content_length);
    return message;
}

Socket create_listen_socket(const std::string& bind_address, const std::uint16_t port, std::uint16_t& bound_port) {
    initialize_socket_runtime();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* raw = nullptr;
    const std::string service = std::to_string(port);
    const int result = ::getaddrinfo(bind_address.c_str(), service.c_str(), &hints, &raw);
    if (result != 0) throw std::runtime_error("Unable to resolve RPC bind address: " + std::string(gai_strerror(result)));
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(raw, ::freeaddrinfo);
    for (addrinfo* current = addresses.get(); current != nullptr; current = current->ai_next) {
        SocketHandle socket(::socket(current->ai_family, current->ai_socktype, current->ai_protocol));
        if (socket.get() == INVALID_SOCKET_VALUE) continue;
        set_socket_reuse_address(socket.get());
        if (::bind(socket.get(), current->ai_addr, current->ai_addrlen) != 0) continue;
        if (::listen(socket.get(), 16) != 0) continue;
        sockaddr_storage local{};
        SocketLength length = sizeof(local);
        if (::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&local), &length) != 0) {
            socket_error("RPC getsockname failed");
        }
        if (local.ss_family == AF_INET) {
            bound_port = ntohs(reinterpret_cast<const sockaddr_in*>(&local)->sin_port);
        } else if (local.ss_family == AF_INET6) {
            bound_port = ntohs(reinterpret_cast<const sockaddr_in6*>(&local)->sin6_port);
        }
        return socket.release();
    }
    throw std::runtime_error("Unable to bind RPC server to " + bind_address + ':' + service);
}

SocketHandle connect_socket(const std::string& host, const std::uint16_t port) {
    initialize_socket_runtime();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* raw = nullptr;
    const std::string service = std::to_string(port);
    const int result = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &raw);
    if (result != 0) throw std::runtime_error("Unable to resolve RPC host: " + std::string(gai_strerror(result)));
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(raw, ::freeaddrinfo);
    for (addrinfo* current = addresses.get(); current != nullptr; current = current->ai_next) {
        SocketHandle socket(::socket(current->ai_family, current->ai_socktype, current->ai_protocol));
        if (socket.get() == INVALID_SOCKET_VALUE) continue;
        if (::connect(socket.get(), current->ai_addr, current->ai_addrlen) == 0) return socket;
    }
    throw std::runtime_error("Could not connect to BinaryCoin RPC server at " + host + ':' + service);
}

std::string random_hex(const std::size_t bytes) {
    std::random_device source;
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string output;
    output.reserve(bytes * 2U);
    for (std::size_t index = 0; index < bytes; ++index) {
        const unsigned int value = source() & 0xffU;
        output.push_back(alphabet[(value >> 4U) & 0x0fU]);
        output.push_back(alphabet[value & 0x0fU]);
    }
    return output;
}

Json rpc_error_object(const int code, const std::string& message) {
    return Json::Object{{"code", code}, {"message", message}};
}

std::string http_response(const int status, const std::string_view reason, const std::string& body) {
    std::ostringstream output;
    output << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
           << "Content-Type: application/json\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n\r\n"
           << body;
    return output.str();
}

} // namespace

RpcError::RpcError(const int code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

int RpcError::code() const noexcept { return code_; }

RpcServer::RpcServer(
    std::string bind_address,
    const std::uint16_t port,
    std::string cookie,
    RpcHandler handler
) : bind_address_(std::move(bind_address)),
    port_(port),
    cookie_(std::move(cookie)),
    handler_(std::move(handler)) {}

RpcServer::~RpcServer() {
    request_stop();
    wait();
    if (listen_socket_ != INVALID_SOCKET_VALUE) close_socket(listen_socket_);
}

void RpcServer::start() {
    if (accept_thread_.joinable()) throw std::runtime_error("RPC server is already started");
    listen_socket_ = create_listen_socket(bind_address_, port_, port_);
    accept_thread_ = std::thread([this] { accept_loop(); });
    log_info("rpc", "RPC server listening on " + bind_address_ + ':' + std::to_string(port_));
}

void RpcServer::request_stop() {
    stop_requested_ = true;
    if (listen_socket_ != INVALID_SOCKET_VALUE) shutdown_socket(listen_socket_);
}

void RpcServer::wait() {
    if (accept_thread_.joinable()) accept_thread_.join();
    for (std::thread& thread : client_threads_) {
        if (thread.joinable()) thread.join();
    }
    client_threads_.clear();
}

std::uint16_t RpcServer::bound_port() const noexcept { return port_; }

void RpcServer::accept_loop() {
    while (!stop_requested_) {
        const Socket socket = ::accept(listen_socket_, nullptr, nullptr);
        if (socket == INVALID_SOCKET_VALUE) {
            if (stop_requested_) return;
            if (socket_error_interrupted(last_socket_error())) continue;
            continue;
        }
        client_threads_.emplace_back([this, socket] { handle_client(socket); });
    }
}

void RpcServer::handle_client(const Socket raw_socket) {
    SocketHandle socket(raw_socket);
    Json id(nullptr);
    try {
        const HttpMessage http = receive_http(socket.get());
        if (!http.start_line.starts_with("POST ")) {
            send_all(socket.get(), http_response(405, "Method Not Allowed", "{}"));
            return;
        }
        const std::string expected = "Basic " + base64_encode(cookie_);
        const auto auth = http.headers.find("authorization");
        if (auth == http.headers.end() || auth->second != expected) {
            log_error("rpc", "RPC authentication failed");
            send_all(socket.get(), http_response(401, "Unauthorized", "{}"));
            return;
        }

        const Json request_json = Json::parse(http.body);
        if (!request_json.is_object()) throw RpcError(-32600, "Invalid Request");
        if (request_json.contains("id")) id = request_json.at("id");
        if (!request_json.contains("method") || !request_json.at("method").is_string()) {
            throw RpcError(-32600, "RPC method must be a string");
        }
        RpcRequest request;
        request.method = request_json.at("method").as_string();
        request.id = id;
        if (request_json.contains("params")) {
            if (!request_json.at("params").is_array()) throw RpcError(-32602, "RPC params must be an array");
            request.params = request_json.at("params").as_array();
        }
        log_debug("rpc", "RPC request method=" + request.method);
        Json result = handler_(request);
        const Json response(Json::Object{{"result", std::move(result)}, {"error", nullptr}, {"id", id}});
        send_all(socket.get(), http_response(200, "OK", response.dump()));
    } catch (const RpcError& error) {
        const Json response(Json::Object{
            {"result", nullptr}, {"error", rpc_error_object(error.code(), error.what())}, {"id", id}});
        send_all(socket.get(), http_response(500, "Internal Server Error", response.dump()));
    } catch (const std::exception& error) {
        log_error("rpc", error.what());
        const Json response(Json::Object{
            {"result", nullptr}, {"error", rpc_error_object(-32603, error.what())}, {"id", id}});
        try {
            send_all(socket.get(), http_response(500, "Internal Server Error", response.dump()));
        } catch (...) {
        }
    }
}

std::string create_rpc_cookie(const std::filesystem::path& data_directory) {
    std::filesystem::create_directories(data_directory);
    const std::filesystem::path path = data_directory / ".cookie";
    const std::string cookie = "__cookie__:" + random_hex(32);
    {
        std::ofstream output(path, std::ios::trunc);
        if (!output) throw std::runtime_error("Unable to create RPC cookie file: " + path.string());
        output << cookie << '\n';
    }
    try {
        protect_file_for_current_user(path);
    } catch (...) {
        std::filesystem::remove(path);
        throw;
    }
    return cookie;
}

std::string read_rpc_cookie(const std::filesystem::path& data_directory) {
    const std::filesystem::path path = data_directory / ".cookie";
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Could not read RPC cookie file " + path.string() + ". Is binarycoind running?");
    std::string cookie;
    std::getline(input, cookie);
    if (cookie.empty()) throw std::runtime_error("RPC cookie file is empty");
    return cookie;
}

void remove_rpc_cookie(const std::filesystem::path& data_directory) noexcept {
    std::error_code error;
    std::filesystem::remove(data_directory / ".cookie", error);
}

Json rpc_call(
    const std::string& host,
    const std::uint16_t port,
    const std::string& cookie,
    const std::string& method,
    const Json::Array& params
) {
    SocketHandle socket = connect_socket(host, port);
    const Json request(Json::Object{
        {"jsonrpc", "1.0"}, {"id", "binarycoin-cli"}, {"method", method}, {"params", params}});
    const std::string body = request.dump();
    std::ostringstream http;
    http << "POST / HTTP/1.1\r\n"
         << "Host: " << host << '\r' << '\n'
         << "Authorization: Basic " << base64_encode(cookie) << "\r\n"
         << "Content-Type: application/json\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << body;
    send_all(socket.get(), http.str());
    const HttpMessage response = receive_http(socket.get());
    const Json parsed = Json::parse(response.body);
    if (!parsed.is_object() || !parsed.contains("result") || !parsed.contains("error")) {
        throw std::runtime_error("Malformed response from BinaryCoin RPC server");
    }
    if (!parsed.at("error").is_null()) {
        const Json& error = parsed.at("error");
        const int code = error.contains("code") ? static_cast<int>(error.at("code").as_integer()) : -32603;
        const std::string message = error.contains("message") ? error.at("message").as_string() : "Unknown RPC error";
        throw RpcError(code, message);
    }
    return parsed.at("result");
}

} // namespace bincoin
