#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace bincoin {

#ifdef _WIN32
using Socket = SOCKET;
using SocketLength = int;
inline constexpr Socket INVALID_SOCKET_VALUE = INVALID_SOCKET;
#else
using Socket = int;
using SocketLength = socklen_t;
inline constexpr Socket INVALID_SOCKET_VALUE = -1;
#endif

void initialize_socket_runtime();
void close_socket(Socket socket) noexcept;
void shutdown_socket(Socket socket) noexcept;
[[nodiscard]] int last_socket_error() noexcept;
[[nodiscard]] bool socket_error_interrupted(int error) noexcept;
[[nodiscard]] std::string socket_error_text(int error);
[[nodiscard]] int socket_send_flags() noexcept;
[[nodiscard]] std::ptrdiff_t socket_send(Socket socket, const void* data, std::size_t size, int flags = 0);
[[nodiscard]] std::ptrdiff_t socket_receive(Socket socket, void* data, std::size_t size, int flags = 0);
void set_socket_timeouts(Socket socket, int timeout_seconds);
void set_socket_reuse_address(Socket socket);
[[nodiscard]] bool wait_socket_readable(Socket socket, int timeout_milliseconds);

[[nodiscard]] std::uint32_t current_process_id() noexcept;
[[nodiscard]] std::filesystem::path default_testnet_datadir();
void protect_file_for_current_user(const std::filesystem::path& path);
void replace_file_atomically(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination);

} // namespace bincoin
