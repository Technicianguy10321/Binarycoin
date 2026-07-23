#include "node_app.hpp"

#include "logger.hpp"
#include "platform.hpp"
#include "rpc.hpp"
#include "rpc_commands.hpp"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace bincoin {
namespace {

std::atomic<bool>* signal_stop = nullptr;

void handle_signal(int) {
    if (signal_stop != nullptr) signal_stop->store(true);
}

#ifdef _WIN32
BOOL WINAPI handle_console_control(const DWORD control_type) {
    switch (control_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (signal_stop != nullptr) signal_stop->store(true);
            return TRUE;
        default:
            return FALSE;
    }
}
#endif

class DataDirectoryLock {
public:
    explicit DataDirectoryLock(const std::filesystem::path& data_directory) {
        std::filesystem::create_directories(data_directory);
        path_ = data_directory / ".lock";
#ifdef _WIN32
        handle_ = ::CreateFileW(
            path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error(
                "Cannot obtain a lock on data directory " + data_directory.string() +
                ". BinaryCoin Core is probably already running. Windows error=" +
                std::to_string(::GetLastError()));
        }
#else
        descriptor_ = ::open(path_.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        if (descriptor_ < 0) {
            throw std::runtime_error("Unable to open data directory lock: " + std::string(std::strerror(errno)));
        }
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error(
                "Cannot obtain a lock on data directory " + data_directory.string() +
                ". BinaryCoin Core is probably already running.");
        }
#endif
    }

    ~DataDirectoryLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
#else
        if (descriptor_ >= 0) {
            (void)::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
        }
#endif
    }

    DataDirectoryLock(const DataDirectoryLock&) = delete;
    DataDirectoryLock& operator=(const DataDirectoryLock&) = delete;

private:
    std::filesystem::path path_;
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int descriptor_{-1};
#endif
};

class PidFile {
public:
    explicit PidFile(const std::filesystem::path& data_directory)
        : path_(data_directory / "binarycoind.pid") {
        std::ofstream output(path_, std::ios::trunc);
        if (!output) throw std::runtime_error("Unable to create PID file: " + path_.string());
        output << current_process_id() << '\n';
    }
    ~PidFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
private:
    std::filesystem::path path_;
};

#ifndef _WIN32
void notify_parent(const int descriptor, const char status) noexcept {
    if (descriptor < 0) return;
    (void)::write(descriptor, &status, 1);
    ::close(descriptor);
}

void detach_process() {
    if (::setsid() < 0) throw std::runtime_error("setsid failed: " + std::string(std::strerror(errno)));
    ::umask(S_IRWXG | S_IRWXO);
    if (::chdir("/") != 0) throw std::runtime_error("Unable to change daemon working directory");
    const int null_fd = ::open("/dev/null", O_RDWR);
    if (null_fd < 0) throw std::runtime_error("Unable to open /dev/null");
    (void)::dup2(null_fd, STDIN_FILENO);
    (void)::dup2(null_fd, STDOUT_FILENO);
    (void)::dup2(null_fd, STDERR_FILENO);
    if (null_fd > STDERR_FILENO) ::close(null_fd);
}
#endif

int run_node_child(const NodeAppOptions& options, const int ready_descriptor) {
    bool ready_sent = false;
    try {
        DataDirectoryLock lock(options.data_directory);
        Logger::instance().configure(
            options.data_directory / "debug.log",
            options.print_to_console && !options.daemon,
            options.debug_categories);
        PidFile pid_file(options.data_directory);

        log_info("init", "BinaryCoin Core version v0.1.3");
        log_info("init", "Using data directory " + options.data_directory.string());
        log_info("init", "Using config file " + (options.data_directory.parent_path() / "binarycoin.conf").string());
        log_info("init", "P2P protocol version " + std::to_string(P2P_PROTOCOL_VERSION));

        std::atomic<bool> stop_requested{false};
        signal_stop = &stop_requested;
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
#ifdef _WIN32
        if (!::SetConsoleCtrlHandler(handle_console_control, TRUE)) {
            throw std::runtime_error("Unable to install Windows console control handler");
        }
#else
        std::signal(SIGHUP, SIG_IGN);
        std::signal(SIGPIPE, SIG_IGN);
#endif

        PersistentTestnetNode node(
            options.data_directory,
            options.p2p_bind,
            options.p2p_port,
            options.manual_peers,
            options.network_policy);
        node.start();

        const std::string cookie = create_rpc_cookie(options.data_directory);
        RpcCommandHandler command_handler(
            options.data_directory,
            node,
            stop_requested,
            options.p2p_bind,
            node.bound_port());
        RpcServer rpc_server(
            options.rpc_bind,
            options.rpc_port,
            cookie,
            [&command_handler](const RpcRequest& request) { return command_handler(request); });
        rpc_server.start();

        log_info("init", "Done loading");
        log_info("init", "BinaryCoin server ready: P2P=" + options.p2p_bind + ':' +
            std::to_string(node.bound_port()) + " RPC=" + options.rpc_bind + ':' +
            std::to_string(rpc_server.bound_port()));
#ifndef _WIN32
        notify_parent(ready_descriptor, '1');
#else
        (void)ready_descriptor;
#endif
        ready_sent = true;

        while (!stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        log_info("shutdown", "Shutdown requested");
        rpc_server.request_stop();
        node.request_stop();
        rpc_server.wait();
        node.wait();
        const LiveNodeStats stats = node.stats();
        log_info("shutdown", "Node stopped: accepted=" + std::to_string(stats.accepted_connections) +
            " outbound=" + std::to_string(stats.outbound_connections) +
            " relayed_blocks=" + std::to_string(stats.relayed_blocks) +
            " relayed_transactions=" + std::to_string(stats.relayed_transactions));
        remove_rpc_cookie(options.data_directory);
#ifdef _WIN32
        (void)::SetConsoleCtrlHandler(handle_console_control, FALSE);
#endif
        signal_stop = nullptr;
        log_info("shutdown", "Shutdown complete");
        Logger::instance().flush();
        return 0;
    } catch (const std::exception& error) {
#ifndef _WIN32
        if (!ready_sent) notify_parent(ready_descriptor, '0');
#else
        (void)ready_sent;
        (void)ready_descriptor;
#endif
        try {
            log_error("init", error.what());
        } catch (...) {
            if (!options.daemon) std::cerr << "error: " << error.what() << '\n';
        }
        remove_rpc_cookie(options.data_directory);
#ifdef _WIN32
        (void)::SetConsoleCtrlHandler(handle_console_control, FALSE);
#endif
        signal_stop = nullptr;
        return 1;
    }
}

#ifdef _WIN32
int launch_windows_daemon(const NodeAppOptions& options) {
    std::wstring command_line = ::GetCommandLineW();
    command_line += L" -daemonchild";
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    if (!::CreateProcessW(
            nullptr, mutable_command.data(), nullptr, nullptr, FALSE, flags,
            nullptr, nullptr, &startup, &process)) {
        throw std::runtime_error("Unable to start BinaryCoin daemon. Windows error=" +
                                 std::to_string(::GetLastError()));
    }
    ::CloseHandle(process.hThread);

    if (options.daemon_wait) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        const std::string rpc_host =
            (options.rpc_bind == "0.0.0.0" || options.rpc_bind == "::") ? "127.0.0.1" : options.rpc_bind;
        bool ready = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (::WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
            try {
                const std::string cookie = read_rpc_cookie(options.data_directory);
                (void)rpc_call(rpc_host, options.rpc_port, cookie, "uptime", {});
                ready = true;
                break;
            } catch (...) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        if (!ready) {
            ::CloseHandle(process.hProcess);
            std::cerr << "Error: BinaryCoin server failed to start. Check debug.log.\n";
            return 1;
        }
    }
    ::CloseHandle(process.hProcess);
    std::cout << "BinaryCoin server starting\n";
    return 0;
}
#endif

} // namespace

int launch_node(const NodeAppOptions& options) {
    if (!options.daemon) return run_node_child(options, -1);

#ifdef _WIN32
    if (options.daemon_child) return run_node_child(options, -1);
    return launch_windows_daemon(options);
#else
    int readiness[2]{-1, -1};
    if (options.daemon_wait && ::pipe(readiness) != 0) {
        throw std::runtime_error("Unable to create daemon readiness pipe");
    }

    const pid_t child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed: " + std::string(std::strerror(errno)));
    if (child > 0) {
        if (options.daemon_wait) {
            ::close(readiness[1]);
            char status = '0';
            const ssize_t read_count = ::read(readiness[0], &status, 1);
            ::close(readiness[0]);
            if (read_count != 1 || status != '1') {
                std::cerr << "Error: BinaryCoin server failed to start. Check debug.log.\n";
                return 1;
            }
        }
        std::cout << "BinaryCoin server starting\n";
        return 0;
    }

    int ready_descriptor = -1;
    if (options.daemon_wait) {
        ::close(readiness[0]);
        ready_descriptor = readiness[1];
    }
    try {
        detach_process();
    } catch (...) {
        notify_parent(ready_descriptor, '0');
        _exit(1);
    }
    const int result = run_node_child(options, ready_descriptor);
    _exit(result);
#endif
}

} // namespace bincoin
