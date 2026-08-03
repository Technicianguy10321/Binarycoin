#include "json.hpp"
#include "net.hpp"
#include "rpc.hpp"
#include "platform.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {


void print_help() {
    std::cout << R"HELP(BinaryCoin Core RPC client version v0.1.5

Usage:
  binarycoin-cli -testnet [options] <command> [params]

Options:
  -testnet                 Use BinaryCoin testnet
  -datadir=<dir>           Specify data directory
  -rpcconnect=<ip>         RPC server host (default 127.0.0.1)
  -rpcport=<port>          RPC server port (default 25001)
  -rpcwait                 Wait for RPC server to start
  -rpcwaittimeout=<sec>    Maximum wait time (default 60)
  -help                    Show this help

Examples:
  binarycoin-cli -testnet getblockchaininfo
  binarycoin-cli -testnet getpeerinfo
  binarycoin-cli -testnet getbalance
  binarycoin-cli -testnet getnewaddress
  binarycoin-cli -testnet stop
)HELP";
}

std::uint16_t parse_port(const std::string& text) {
    std::size_t used = 0;
    const unsigned long value = std::stoul(text, &used, 10);
    if (used != text.size() || value == 0 || value > 65535) throw std::invalid_argument("Invalid RPC port");
    return static_cast<std::uint16_t>(value);
}

bincoin::Json parse_parameter(const std::string& method, const std::size_t index, const std::string& text) {
    const bool integer = (method == "generate" || method == "generatetoaddress" ||
                          method == "getblockhash" || method == "estimatesmartfee") && index == 0;
    if (integer) {
        std::size_t used = 0;
        const long long value = std::stoll(text, &used, 10);
        if (used != text.size()) throw std::invalid_argument("Expected integer parameter: " + text);
        return static_cast<std::int64_t>(value);
    }
    if (method == "getblock" && index == 1) {
        std::size_t used = 0;
        const long long value = std::stoll(text, &used, 10);
        if (used == text.size()) return static_cast<std::int64_t>(value);
    }
    if (method == "logging" && !text.empty() && text.front() == '[') {
        return bincoin::Json::parse(text);
    }
    return text;
}

} // namespace

int main(int argc, char** argv) {
    try {
        bool testnet = false;
        bool rpcwait = false;
        int rpcwaittimeout = 60;
        std::filesystem::path datadir = bincoin::default_testnet_datadir();
        std::string rpcconnect = "127.0.0.1";
        std::uint16_t rpcport = bincoin::RESERVED_TESTNET_RPC_PORT;
        std::vector<std::string> positional;

        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "-testnet" || argument == "--testnet") {
                testnet = true;
            } else if (argument.starts_with("-datadir=")) {
                datadir = argument.substr(9);
            } else if (argument == "--datadir" && index + 1 < argc) {
                datadir = argv[++index];
            } else if (argument.starts_with("-rpcconnect=")) {
                rpcconnect = argument.substr(12);
            } else if (argument.starts_with("-rpcport=")) {
                rpcport = parse_port(argument.substr(9));
            } else if (argument == "-rpcwait") {
                rpcwait = true;
            } else if (argument.starts_with("-rpcwaittimeout=")) {
                rpcwaittimeout = std::stoi(argument.substr(16));
                if (rpcwaittimeout < 1 || rpcwaittimeout > 3600) throw std::invalid_argument("Invalid RPC wait timeout");
            } else if (argument == "-help" || argument == "--help" || argument == "-h") {
                print_help();
                return 0;
            } else if (!argument.empty() && argument.front() == '-') {
                throw std::invalid_argument("Unknown option: " + argument);
            } else {
                positional.push_back(argument);
            }
        }

        if (!testnet) throw std::invalid_argument("This build is BinaryCoin Testnet Alpha. Pass -testnet.");
        if (positional.empty()) {
            print_help();
            return 1;
        }

        const std::string method = positional.front();
        bincoin::Json::Array params;
        for (std::size_t index = 1; index < positional.size(); ++index) {
            params.push_back(parse_parameter(method, index - 1, positional[index]));
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(rpcwaittimeout);
        while (true) {
            try {
                const std::string cookie = bincoin::read_rpc_cookie(datadir);
                const bincoin::Json result = bincoin::rpc_call(rpcconnect, rpcport, cookie, method, params);
                if (result.is_string()) std::cout << result.as_string() << '\n';
                else std::cout << result.dump(true) << '\n';
                return 0;
            } catch (const bincoin::RpcError& error) {
                std::cerr << "error code: " << error.code() << "\nerror message:\n" << error.what() << '\n';
                return 1;
            } catch (const std::exception&) {
                if (!rpcwait || std::chrono::steady_clock::now() >= deadline) throw;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
