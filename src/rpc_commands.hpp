#pragma once

#include "json.hpp"
#include "net.hpp"
#include "rpc.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace bincoin {

class RpcCommandHandler {
public:
    RpcCommandHandler(
        std::filesystem::path data_directory,
        PersistentTestnetNode& node,
        std::atomic<bool>& stop_requested,
        std::string p2p_bind,
        std::uint16_t p2p_port
    );

    [[nodiscard]] Json operator()(const RpcRequest& request);

private:
    std::filesystem::path data_directory_;
    PersistentTestnetNode& node_;
    std::atomic<bool>& stop_requested_;
    std::string p2p_bind_;
    std::uint16_t p2p_port_;
    std::chrono::steady_clock::time_point started_at_;

    [[nodiscard]] Json dispatch(const std::string& method, const Json::Array& params);
};

} // namespace bincoin
