#include "platform.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <aclapi.h>
#include <sddl.h>
#else
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bincoin {

void initialize_socket_runtime() {
#ifdef _WIN32
    static std::once_flag once;
    static int startup_error = 0;
    std::call_once(once, [] {
        WSADATA data{};
        startup_error = ::WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (startup_error != 0) {
        throw std::runtime_error("WSAStartup failed: " + std::to_string(startup_error));
    }
#endif
}

void close_socket(const Socket socket) noexcept {
    if (socket == INVALID_SOCKET_VALUE) return;
#ifdef _WIN32
    (void)::closesocket(socket);
#else
    (void)::close(socket);
#endif
}

void shutdown_socket(const Socket socket) noexcept {
    if (socket == INVALID_SOCKET_VALUE) return;
#ifdef _WIN32
    (void)::shutdown(socket, SD_BOTH);
#else
    (void)::shutdown(socket, SHUT_RDWR);
#endif
}

int last_socket_error() noexcept {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

bool socket_error_interrupted(const int error) noexcept {
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

std::string socket_error_text(const int error) {
#ifdef _WIN32
    char* buffer = nullptr;
    const DWORD length = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(error),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&buffer),
        0,
        nullptr);
    std::string message = length != 0 && buffer != nullptr
        ? std::string(buffer, buffer + length)
        : "Windows socket error " + std::to_string(error);
    if (buffer != nullptr) ::LocalFree(buffer);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
        message.pop_back();
    }
    return message;
#else
    return std::strerror(error);
#endif
}

int socket_send_flags() noexcept {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

std::ptrdiff_t socket_send(const Socket socket, const void* data, const std::size_t size, const int flags) {
    const std::size_t chunk = std::min<std::size_t>(size, static_cast<std::size_t>(INT_MAX));
#ifdef _WIN32
    const int result = ::send(socket, static_cast<const char*>(data), static_cast<int>(chunk), flags);
    return result == SOCKET_ERROR ? -1 : static_cast<std::ptrdiff_t>(result);
#else
    return static_cast<std::ptrdiff_t>(::send(socket, data, chunk, flags));
#endif
}

std::ptrdiff_t socket_receive(const Socket socket, void* data, const std::size_t size, const int flags) {
    const std::size_t chunk = std::min<std::size_t>(size, static_cast<std::size_t>(INT_MAX));
#ifdef _WIN32
    const int result = ::recv(socket, static_cast<char*>(data), static_cast<int>(chunk), flags);
    return result == SOCKET_ERROR ? -1 : static_cast<std::ptrdiff_t>(result);
#else
    return static_cast<std::ptrdiff_t>(::recv(socket, data, chunk, flags));
#endif
}

void set_socket_timeouts(const Socket socket, const int timeout_seconds) {
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(timeout_seconds * 1000);
    if (::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0 ||
        ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) {
        const int error = last_socket_error();
        throw std::runtime_error("Unable to configure socket timeout: " + socket_error_text(error));
    }
#else
    const timeval timeout{.tv_sec = timeout_seconds, .tv_usec = 0};
    if (::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        const int error = last_socket_error();
        throw std::runtime_error("Unable to configure socket timeout: " + socket_error_text(error));
    }
#endif
}

void set_socket_reuse_address(const Socket socket) {
    const int reuse = 1;
#ifdef _WIN32
    (void)::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    (void)::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
}

bool wait_socket_readable(const Socket socket, const int timeout_milliseconds) {
    while (true) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket, &read_set);
        fd_set error_set;
        FD_ZERO(&error_set);
        FD_SET(socket, &error_set);
        timeval timeout{
            .tv_sec = timeout_milliseconds / 1000,
            .tv_usec = (timeout_milliseconds % 1000) * 1000,
        };
#ifdef _WIN32
        const int result = ::select(0, &read_set, nullptr, &error_set, &timeout);
#else
        const int result = ::select(socket + 1, &read_set, nullptr, &error_set, &timeout);
#endif
        if (result < 0) {
            const int error = last_socket_error();
            if (socket_error_interrupted(error)) continue;
            throw std::runtime_error("select failed: " + socket_error_text(error));
        }
        if (result == 0) return false;
        if (FD_ISSET(socket, &error_set)) throw std::runtime_error("Peer connection closed");
        return FD_ISSET(socket, &read_set) != 0;
    }
}

std::uint32_t current_process_id() noexcept {
#ifdef _WIN32
    return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

std::filesystem::path default_testnet_datadir() {
#ifdef _WIN32
    auto environment_path = [](const wchar_t* name) -> std::filesystem::path {
        const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0) return {};
        std::wstring value(required, L'\0');
        const DWORD written = ::GetEnvironmentVariableW(name, value.data(), required);
        if (written == 0 || written >= required) return {};
        value.resize(written);
        return std::filesystem::path(value);
    };
    if (const auto appdata = environment_path(L"APPDATA"); !appdata.empty()) {
        return appdata / "BinaryCoin" / "testnet";
    }
    if (const auto userprofile = environment_path(L"USERPROFILE"); !userprofile.empty()) {
        return userprofile / "AppData" / "Roaming" / "BinaryCoin" / "testnet";
    }
#else
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".binarycoin" / "testnet";
    }
#endif
    return std::filesystem::current_path() / ".binarycoin" / "testnet";
}

void protect_file_for_current_user(const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                "Unable to open process token for RPC cookie protection");
    }

    DWORD size = 0;
    (void)::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    if (size == 0 || !::GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(token);
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "Unable to read current Windows user SID");
    }
    ::CloseHandle(token);

    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | DELETE | READ_CONTROL | WRITE_DAC;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(user->User.Sid);

    PACL acl = nullptr;
    const DWORD acl_result = ::SetEntriesInAclW(1, &access, nullptr, &acl);
    if (acl_result != ERROR_SUCCESS) {
        throw std::system_error(static_cast<int>(acl_result), std::system_category(),
                                "Unable to create RPC cookie ACL");
    }
    const DWORD result = ::SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        acl,
        nullptr);
    ::LocalFree(acl);
    if (result != ERROR_SUCCESS) {
        throw std::system_error(static_cast<int>(result), std::system_category(),
                                "Unable to protect RPC cookie file");
    }
#else
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        throw std::system_error(errno, std::generic_category(), "Unable to protect RPC cookie file");
    }
#endif
}

void replace_file_atomically(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination
) {
#ifdef _WIN32
    if (!::MoveFileExW(
            temporary.c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = ::GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "Unable to atomically replace file");
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        throw std::system_error(error, "Unable to atomically replace file");
    }
#endif
}

} // namespace bincoin
