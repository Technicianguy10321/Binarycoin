#include "chain.hpp"
#include "net.hpp"
#include "rpc.hpp"
#include "rpc_commands.hpp"
#include "wallet.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <thread>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bincoin::NetworkPolicy local_policy() {
    bincoin::NetworkPolicy policy;
    policy.use_compiled_seeds = false;
    return policy;
}

} // namespace

int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() / "binarycoin-v014-rpc-test";
        std::filesystem::remove_all(root);

        bincoin::TestnetChain chain(root);
        chain.initialize();
        bincoin::WalletManager wallet(root);
        (void)wallet.create();

        bincoin::PersistentTestnetNode node(
            root, "127.0.0.1", 0, std::vector<bincoin::PeerEndpoint>{}, local_policy());
        node.start();

        std::atomic<bool> stop_requested{false};
        const std::string cookie = bincoin::create_rpc_cookie(root);
        require(std::filesystem::exists(root / ".cookie"), "RPC cookie was not created");
#ifndef _WIN32
        struct stat cookie_stat{};
        require(::stat((root / ".cookie").c_str(), &cookie_stat) == 0, "RPC cookie was not readable");
        require((cookie_stat.st_mode & 0777) == 0600, "RPC cookie permissions are not 0600");
#endif

        bincoin::RpcCommandHandler commands(
            root, node, stop_requested, "127.0.0.1", node.bound_port());
        bincoin::RpcServer server(
            "127.0.0.1", 0, cookie,
            [&commands](const bincoin::RpcRequest& request) { return commands(request); });
        server.start();

        const auto blockchain = bincoin::rpc_call(
            "127.0.0.1", server.bound_port(), cookie, "getblockchaininfo", {});
        require(blockchain.is_object(), "getblockchaininfo did not return an object");
        require(blockchain.at("chain").as_string() == "test", "Wrong RPC chain name");
        require(blockchain.at("blocks").as_integer() == 0, "Fresh RPC chain height is not zero");

        const auto network = bincoin::rpc_call(
            "127.0.0.1", server.bound_port(), cookie, "getnetworkinfo", {});
        require(network.at("protocolversion").as_integer() == bincoin::P2P_PROTOCOL_VERSION,
                "RPC protocol version mismatch");

        const auto address = bincoin::rpc_call(
            "127.0.0.1", server.bound_port(), cookie, "getnewaddress", {});
        require(address.is_string() && address.as_string().starts_with("tbin1"),
                "getnewaddress returned an invalid testnet address");

        const auto generated = bincoin::rpc_call(
            "127.0.0.1", server.bound_port(), cookie, "generate", {1});
        require(generated.is_array() && generated.as_array().size() == 1,
                "generate did not return one block hash");
        const auto block_count = bincoin::rpc_call(
            "127.0.0.1", server.bound_port(), cookie, "getblockcount", {});
        require(block_count.as_integer() == 1, "RPC-generated block was not persisted");

        const auto logging = bincoin::rpc_call(
            "127.0.0.1", server.bound_port(), cookie, "logging",
            {bincoin::Json::Array{bincoin::Json("rpc")}});
        require(logging.at("rpc").as_bool(), "Runtime RPC logging category was not enabled");

        bool rejected_bad_cookie = false;
        try {
            (void)bincoin::rpc_call(
                "127.0.0.1", server.bound_port(), "__cookie__:wrong", "getblockcount", {});
        } catch (const std::exception&) {
            rejected_bad_cookie = true;
        }
        require(rejected_bad_cookie, "RPC server accepted an invalid cookie");

        const auto stop = bincoin::rpc_call(
            "127.0.0.1", server.bound_port(), cookie, "stop", {});
        require(stop.is_string(), "stop RPC did not return its shutdown message");
        for (int attempt = 0; attempt < 40 && !stop_requested.load(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        require(stop_requested.load(), "stop RPC did not request shutdown");

        server.request_stop();
        node.request_stop();
        server.wait();
        node.wait();
        bincoin::remove_rpc_cookie(root);
        require(!std::filesystem::exists(root / ".cookie"), "RPC cookie was not removed");

        std::filesystem::remove_all(root);
        std::cout << "BinaryCoin Testnet Alpha v0.1.4 RPC and cookie-authentication tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RPC test failure: " << error.what() << '\n';
        return 1;
    }
}
