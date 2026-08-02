#include "rpc_commands.hpp"

#include "amount.hpp"
#include "bech32.hpp"
#include "branch_store.hpp"
#include "chain.hpp"
#include "fees.hpp"
#include "logger.hpp"
#include "mempool.hpp"
#include "params.hpp"
#include "peer_store.hpp"
#include "pow.hpp"
#include "script.hpp"
#include "serialize.hpp"
#include "wallet.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>

namespace bincoin {
namespace {

void require_params(const Json::Array& params, const std::size_t minimum, const std::size_t maximum) {
    if (params.size() < minimum || params.size() > maximum) {
        throw RpcError(-1, "Incorrect number of parameters");
    }
}

std::string string_param(const Json& value, const char* name) {
    if (!value.is_string()) throw RpcError(-3, std::string(name) + " must be a string");
    return value.as_string();
}

std::int64_t integer_param(const Json& value, const char* name) {
    if (!value.is_number()) throw RpcError(-3, std::string(name) + " must be a number");
    return value.as_integer();
}

std::string amount_param(const Json& value) {
    if (value.is_string()) return value.as_string();
    if (!value.is_number()) throw RpcError(-3, "Amount must be a number or decimal string");
    std::ostringstream output;
    output.setf(std::ios::fixed);
    output.precision(8);
    output << value.as_number();
    return output.str();
}

double amount_json(const Amount amount) {
    return static_cast<double>(amount) / static_cast<double>(BITS_PER_BIN);
}

Json::Array category_array(const Json& value) {
    if (value.is_null()) return {};
    if (value.is_string()) return Json::Array{value};
    if (!value.is_array()) throw RpcError(-3, "Logging categories must be an array of strings");
    return value.as_array();
}

std::string help_text() {
    return R"HELP(== Blockchain ==
getbestblockhash
getblock "blockhash" ( verbosity )
getblockchaininfo
getblockcount
getblockhash height

== Control ==
help ( "command" )
logging ( [include,...] [exclude,...] )
stop
uptime

== Generating ==
generate nblocks
generatetoaddress nblocks "address"
getmininginfo

== Mempool ==
getmempoolinfo
getrawmempool
estimatesmartfee conf_target ( "economical"|"conservative" )

== Network ==
getconnectioncount
getnetworkinfo
getpeerinfo

== Wallet ==
getbalance
getnewaddress
getwalletinfo
listunspent
sendtoaddress "address" amount
validateaddress "address")HELP";
}

std::string compact_bits_hex(const std::uint32_t bits) {
    std::ostringstream output;
    output << std::hex << bits;
    return output.str();
}

double difficulty_from_bits(const std::uint32_t bits) {
    const long double limit = compact_to_target(TESTNET_POW_LIMIT_BITS).convert_to<long double>();
    const long double target = compact_to_target(bits).convert_to<long double>();
    return static_cast<double>(limit / target);
}

std::uint64_t next_retarget_height(const std::uint64_t next_height) {
    if (next_height <= TESTNET_DIFFICULTY_FORK_HEIGHT) {
        return TESTNET_DIFFICULTY_FORK_HEIGHT;
    }
    const std::uint64_t since_fork = next_height - TESTNET_DIFFICULTY_FORK_HEIGHT;
    const std::uint64_t remainder = since_fork % TESTNET_RETARGET_INTERVAL;
    return remainder == 0 ? next_height : next_height + (TESTNET_RETARGET_INTERVAL - remainder);
}

const StoredBlock& block_by_hash(const TestnetChain& chain, const std::string& hash) {
    const auto iterator = std::find_if(chain.blocks().begin(), chain.blocks().end(), [&](const StoredBlock& block) {
        return block.block.header.hash_hex() == hash;
    });
    if (iterator == chain.blocks().end()) throw RpcError(-5, "Block not found");
    return *iterator;
}

Json block_json(const StoredBlock& stored, const std::uint64_t confirmations, const std::string& chainwork) {
    Json::Array txids;
    for (const Transaction& transaction : stored.block.transactions) txids.emplace_back(transaction.txid());
    Json::Object object{
        {"hash", stored.block.header.hash_hex()},
        {"confirmations", confirmations},
        {"height", stored.height},
        {"version", static_cast<std::int64_t>(stored.block.header.version)},
        {"merkleroot", stored.block.header.merkle_root},
        {"time", static_cast<std::uint64_t>(stored.block.header.time)},
        {"nonce", static_cast<std::uint64_t>(stored.block.header.nonce)},
        {"bits", compact_bits_hex(stored.block.header.bits)},
        {"chainwork", chainwork},
        {"nTx", static_cast<std::uint64_t>(stored.block.transactions.size())},
        {"tx", std::move(txids)},
    };
    if (stored.height > 0) object.insert_or_assign("previousblockhash", stored.block.header.previous_hash);
    return object;
}

} // namespace

RpcCommandHandler::RpcCommandHandler(
    std::filesystem::path data_directory,
    PersistentTestnetNode& node,
    std::atomic<bool>& stop_requested,
    std::string p2p_bind,
    const std::uint16_t p2p_port
) : data_directory_(std::move(data_directory)),
    node_(node),
    stop_requested_(stop_requested),
    p2p_bind_(std::move(p2p_bind)),
    p2p_port_(p2p_port),
    started_at_(std::chrono::steady_clock::now()) {}

Json RpcCommandHandler::operator()(const RpcRequest& request) {
    try {
        return dispatch(request.method, request.params);
    } catch (const RpcError&) {
        throw;
    } catch (const std::invalid_argument& error) {
        throw RpcError(-8, error.what());
    } catch (const std::exception& error) {
        throw RpcError(-32603, error.what());
    }
}

Json RpcCommandHandler::dispatch(const std::string& method, const Json::Array& params) {
    if (method == "help") {
        require_params(params, 0, 1);
        return help_text();
    }

    if (method == "stop") {
        require_params(params, 0, 0);
        stop_requested_ = true;
        log_info("rpc", "Shutdown requested by RPC");
        return "BinaryCoin Core stopping";
    }

    if (method == "uptime") {
        require_params(params, 0, 0);
        return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started_at_).count());
    }

    if (method == "logging") {
        require_params(params, 0, 2);
        std::set<std::string> categories = Logger::instance().debug_categories();
        if (!params.empty()) {
            for (const Json& item : category_array(params[0])) {
                categories.insert(string_param(item, "Logging category"));
            }
        }
        if (params.size() >= 2) {
            for (const Json& item : category_array(params[1])) {
                categories.erase(string_param(item, "Logging category"));
            }
        }
        Logger::instance().set_debug_categories(categories);
        Json::Object result;
        for (const std::string category : {"net", "rpc", "http", "addrman", "validation", "wallet", "mempool"}) {
            result.emplace(category, categories.contains("all") || categories.contains("1") || categories.contains(category));
        }
        return result;
    }

    if (method == "getconnectioncount") {
        require_params(params, 0, 0);
        return static_cast<std::uint64_t>(node_.stats().active_peers);
    }

    if (method == "getnetworkinfo") {
        require_params(params, 0, 0);
        const LiveNodeStats stats = node_.stats();
        Json::Array local_addresses;
        local_addresses.emplace_back(Json::Object{
            {"address", p2p_bind_}, {"port", static_cast<std::uint64_t>(p2p_port_)}, {"score", 1}});
        return Json::Object{
            {"version", 10400},
            {"subversion", "/BinaryCoinTestnetAlpha:0.1.4/"},
            {"protocolversion", static_cast<std::uint64_t>(P2P_PROTOCOL_VERSION)},
            {"localservices", "0000000000000001"},
            {"localrelay", true},
            {"timeoffset", 0},
            {"networkactive", true},
            {"connections", static_cast<std::uint64_t>(stats.active_peers)},
            {"connections_in", static_cast<std::uint64_t>(stats.active_inbound)},
            {"connections_out", static_cast<std::uint64_t>(stats.active_peers - stats.active_inbound)},
            {"relayfee", 0.00001000},
            {"incrementalfee", 0.00001000},
            {"localaddresses", std::move(local_addresses)},
            {"warnings", "BinaryCoin Testnet Alpha software"},
        };
    }

    if (method == "getpeerinfo") {
        require_params(params, 0, 0);
        Json::Array result;
        for (const LivePeerInfo& peer : node_.peer_info()) {
            result.emplace_back(Json::Object{
                {"id", peer.id},
                {"addr", peer.endpoint.host + ':' + std::to_string(peer.endpoint.port)},
                {"network", peer.endpoint.host.find(':') == std::string::npos ? "ipv4" : "ipv6"},
                {"services", [&] {
                    std::ostringstream output;
                    output << std::hex << peer.services;
                    return output.str();
                }()},
                {"relaytxes", true},
                {"lastsend", 0},
                {"lastrecv", 0},
                {"bytessent", 0},
                {"bytesrecv", 0},
                {"conntime", peer.connected_since},
                {"version", static_cast<std::uint64_t>(peer.protocol_version)},
                {"subver", peer.user_agent},
                {"inbound", peer.inbound},
                {"startingheight", peer.starting_height},
                {"synced_headers", peer.starting_height},
                {"synced_blocks", peer.starting_height},
            });
        }
        return result;
    }

    if (method == "getblockchaininfo" || method == "getblockcount" ||
        method == "getbestblockhash" || method == "getblockhash" || method == "getblock" ||
        method == "getmininginfo") {
        std::lock_guard lock(node_.data_mutex());
        TestnetChain chain(data_directory_);
        chain.load();

        if (method == "getblockcount") {
            require_params(params, 0, 0);
            return chain.tip().height;
        }
        if (method == "getbestblockhash") {
            require_params(params, 0, 0);
            return chain.tip().block.header.hash_hex();
        }
        if (method == "getblockhash") {
            require_params(params, 1, 1);
            const std::int64_t height = integer_param(params[0], "Height");
            if (height < 0 || static_cast<std::uint64_t>(height) > chain.tip().height) {
                throw RpcError(-8, "Block height out of range");
            }
            return chain.blocks()[static_cast<std::size_t>(height)].block.header.hash_hex();
        }
        if (method == "getblock") {
            require_params(params, 1, 2);
            const std::string hash = string_param(params[0], "Block hash");
            const StoredBlock& stored = block_by_hash(chain, hash);
            const std::uint64_t confirmations = chain.tip().height - stored.height + 1;
            return block_json(stored, confirmations, chain.chainwork_hex());
        }
        if (method == "getmininginfo") {
            require_params(params, 0, 0);
            const std::uint64_t next_height = chain.tip().height + 1;
            const std::uint32_t next_bits = expected_testnet_bits(
                std::span<const StoredBlock>(chain.blocks()), next_height);
            return Json::Object{
                {"blocks", chain.tip().height},
                {"currentblockweight", 0},
                {"currentblocktx", 0},
                {"difficulty", difficulty_from_bits(next_bits)},
                {"networkhashps", 0.0},
                {"pooledtx", 0},
                {"chain", "test"},
                {"nextblockheight", next_height},
                {"nextblockbits", compact_bits_hex(next_bits)},
                {"difficultyforkheight", TESTNET_DIFFICULTY_FORK_HEIGHT},
                {"retargetinterval", TESTNET_RETARGET_INTERVAL},
                {"targetspacing", static_cast<std::uint64_t>(TESTNET_TARGET_BLOCK_SECONDS)},
                {"nextretargetheight", next_retarget_height(next_height)},
                {"warnings", "BinaryCoin Testnet Alpha software"},
            };
        }
        require_params(params, 0, 0);
        const auto storage = chain.storage_stats();
        const std::uint64_t next_height = chain.tip().height + 1;
        const std::uint32_t next_bits = expected_testnet_bits(
            std::span<const StoredBlock>(chain.blocks()), next_height);
        return Json::Object{
            {"chain", "test"},
            {"blocks", chain.tip().height},
            {"headers", chain.tip().height},
            {"bestblockhash", chain.tip().block.header.hash_hex()},
            {"difficulty", difficulty_from_bits(chain.tip().block.header.bits)},
            {"bits", compact_bits_hex(chain.tip().block.header.bits)},
            {"nextblockbits", compact_bits_hex(next_bits)},
            {"difficultyforkheight", TESTNET_DIFFICULTY_FORK_HEIGHT},
            {"retargetinterval", TESTNET_RETARGET_INTERVAL},
            {"targetspacing", static_cast<std::uint64_t>(TESTNET_TARGET_BLOCK_SECONDS)},
            {"nextretargetheight", next_retarget_height(next_height)},
            {"mediantime", static_cast<std::uint64_t>(chain.tip().block.header.time)},
            {"verificationprogress", 1.0},
            {"initialblockdownload", false},
            {"chainwork", chain.chainwork_hex()},
            {"size_on_disk", static_cast<std::uint64_t>(storage.block_file_bytes)},
            {"pruned", false},
            {"warnings", "BinaryCoin Testnet Alpha software"},
        };
    }

    if (method == "getmempoolinfo" || method == "getrawmempool") {
        std::lock_guard lock(node_.data_mutex());
        TestnetMempool mempool(data_directory_);
        mempool.load();
        if (method == "getrawmempool") {
            require_params(params, 0, 1);
            Json::Array txids;
            for (const Transaction& transaction : mempool.transactions()) txids.emplace_back(transaction.txid());
            return txids;
        }
        require_params(params, 0, 0);
        std::uint64_t bytes = 0;
        for (const Transaction& transaction : mempool.transactions()) {
            bytes += static_cast<std::uint64_t>(transaction.virtual_size());
        }
        return Json::Object{
            {"loaded", true},
            {"size", static_cast<std::uint64_t>(mempool.transactions().size())},
            {"bytes", bytes},
            {"usage", bytes},
            {"maxmempool", static_cast<std::uint64_t>(300U * 1024U * 1024U)},
            {"mempoolminfee", 0.00001000},
            {"minrelaytxfee", 0.00001000},
        };
    }

    if (method == "estimatesmartfee") {
        require_params(params, 1, 2);
        const std::int64_t target = integer_param(params[0], "Confirmation target");
        if (target < 1 || target > 1008) throw RpcError(-8, "Confirmation target out of range");
        FeeEstimateMode mode = FeeEstimateMode::Economical;
        if (params.size() == 2) mode = parse_fee_mode(string_param(params[1], "Estimate mode"));
        const auto quote = TestnetFeeEstimator{}.estimate(1000, static_cast<std::uint32_t>(target), mode);
        return Json::Object{
            {"feerate", amount_json(quote.rate.bits_per_kvb())},
            {"blocks", target},
        };
    }

    if (method == "validateaddress") {
        require_params(params, 1, 1);
        const std::string address = string_param(params[0], "Address");
        const bool valid = valid_testnet_address(address);
        Json::Object result{{"isvalid", valid}};
        if (valid) {
            result.emplace("address", address);
            result.emplace("scriptPubKey", bytes_to_hex(make_p2pk_script(decode_testnet_address(address))));
            result.emplace("isscript", false);
            result.emplace("iswitness", true);
            result.emplace("witness_version", 0);
        }
        return result;
    }

    if (method == "getwalletinfo" || method == "getbalance" || method == "getnewaddress" ||
        method == "listunspent" || method == "sendtoaddress" || method == "generate" ||
        method == "generatetoaddress") {
        std::lock_guard lock(node_.data_mutex());
        TestnetChain chain(data_directory_);
        chain.load();
        WalletManager wallet(data_directory_);
        if (!wallet.exists()) throw RpcError(-18, "No wallet is loaded. Create one with binarycoind --testnet wallet-create.");
        wallet.load();
        TestnetMempool mempool(data_directory_);
        mempool.load();
        const std::uint64_t spend_height = chain.tip().height + 1;
        const UtxoSet chain_utxos = chain.utxo_set();
        const UtxoSet effective_utxos = mempool.effective_utxos(chain_utxos, spend_height);

        if (method == "getwalletinfo") {
            require_params(params, 0, 0);
            const WalletBalance balance = wallet.balance(effective_utxos, spend_height);
            return Json::Object{
                {"walletname", "default"},
                {"walletversion", 1},
                {"format", wallet.kind_name()},
                {"balance", amount_json(balance.spendable)},
                {"unconfirmed_balance", 0.0},
                {"immature_balance", amount_json(balance.immature)},
                {"txcount", static_cast<std::uint64_t>(mempool.transactions().size())},
                {"keypoololdest", 0},
                {"keypoolsize", 1000},
                {"paytxfee", 0.0},
                {"private_keys_enabled", true},
                {"descriptors", false},
                {"avoid_reuse", false},
                {"scanning", false},
            };
        }
        if (method == "getbalance") {
            require_params(params, 0, 3);
            return amount_json(wallet.balance(effective_utxos, spend_height).spendable);
        }
        if (method == "getnewaddress") {
            require_params(params, 0, 2);
            const std::string address = wallet.get_new_address();
            log_info("wallet", "Generated new receiving address");
            return address;
        }
        if (method == "listunspent") {
            require_params(params, 0, 5);
            Json::Array result;
            for (const auto& [outpoint, coin] : effective_utxos) {
                if (!wallet.owns_script(coin.output.script_pubkey)) continue;
                const bool mature = !coin.coinbase || spend_height >= coin.height + COINBASE_MATURITY;
                const std::uint64_t confirmations = chain.tip().height >= coin.height
                    ? chain.tip().height - coin.height + 1 : 0;
                result.emplace_back(Json::Object{
                    {"txid", outpoint.txid},
                    {"vout", static_cast<std::uint64_t>(outpoint.index)},
                    {"amount", amount_json(coin.output.value)},
                    {"confirmations", confirmations},
                    {"spendable", mature},
                    {"solvable", true},
                    {"safe", true},
                });
            }
            return result;
        }
        if (method == "sendtoaddress") {
            require_params(params, 2, 10);
            const std::string address = string_param(params[0], "Address");
            const auto destination = decode_testnet_address(address);
            const Amount amount = parse_bin_amount(amount_param(params[1]));
            const auto quote = TestnetFeeEstimator{}.estimate(1, 6, FeeEstimateMode::Economical);
            const Transaction transaction = wallet.create_payment(
                effective_utxos, spend_height, destination, amount, quote.rate);
            mempool.add(transaction, chain_utxos, spend_height);
            log_info("wallet", "Transaction added to mempool txid=" + transaction.txid());
            return transaction.txid();
        }
        if (method == "generate" || method == "generatetoaddress") {
            require_params(params, method == "generate" ? 1U : 2U, method == "generate" ? 1U : 3U);
            const std::int64_t count_value = integer_param(params[0], "Block count");
            if (count_value <= 0 || count_value > 100000) throw RpcError(-8, "Block count out of range");
            const std::uint64_t count = static_cast<std::uint64_t>(count_value);
            std::vector<std::uint8_t> script = wallet.locking_script();
            if (method == "generatetoaddress") {
                script = make_p2pk_script(decode_testnet_address(string_param(params[1], "Address")));
            }
            chain.generate(count, script, mempool.transactions());
            mempool.clear();
            Json::Array hashes;
            const std::size_t start = chain.blocks().size() - static_cast<std::size_t>(count);
            for (std::size_t index = start; index < chain.blocks().size(); ++index) {
                hashes.emplace_back(chain.blocks()[index].block.header.hash_hex());
            }
            log_info("validation", "Generated " + std::to_string(count) + " block(s), height=" +
                std::to_string(chain.tip().height));
            return hashes;
        }
    }

    throw RpcError(-32601, "Method not found: " + method);
}

} // namespace bincoin
