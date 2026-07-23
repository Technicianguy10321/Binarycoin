#pragma once

#include "json.hpp"
#include "platform.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace bincoin {

class RpcError : public std::runtime_error {
public:
    RpcError(int code, std::string message);
    [[nodiscard]] int code() const noexcept;

private:
    int code_;
};

struct RpcRequest {
    std::string method;
    Json::Array params;
    Json id;
};

using RpcHandler = std::function<Json(const RpcRequest&)>;

class RpcServer {
public:
    RpcServer(
        std::string bind_address,
        std::uint16_t port,
        std::string cookie,
        RpcHandler handler
    );
    ~RpcServer();

    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    void start();
    void request_stop();
    void wait();
    [[nodiscard]] std::uint16_t bound_port() const noexcept;

private:
    std::string bind_address_;
    std::uint16_t port_{0};
    std::string cookie_;
    RpcHandler handler_;
    Socket listen_socket_{INVALID_SOCKET_VALUE};
    std::atomic<bool> stop_requested_{false};
    std::thread accept_thread_;
    std::vector<std::thread> client_threads_;

    void accept_loop();
    void handle_client(Socket socket);
};

[[nodiscard]] std::string create_rpc_cookie(const std::filesystem::path& data_directory);
[[nodiscard]] std::string read_rpc_cookie(const std::filesystem::path& data_directory);
void remove_rpc_cookie(const std::filesystem::path& data_directory) noexcept;

[[nodiscard]] Json rpc_call(
    const std::string& host,
    std::uint16_t port,
    const std::string& cookie,
    const std::string& method,
    const Json::Array& params
);

} // namespace bincoin
