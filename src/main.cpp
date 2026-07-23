#include "amount.hpp"
#include "bech32.hpp"
#include "block.hpp"
#include "branch_store.hpp"
#include "ban_store.hpp"
#include "chain.hpp"
#include "fees.hpp"
#include "hash.hpp"
#include "key.hpp"
#include "mempool.hpp"
#include "net.hpp"
#include "node_app.hpp"
#include "params.hpp"
#include "platform.hpp"
#include "pow.hpp"
#include "peer_store.hpp"
#include "script.hpp"
#include "serialize.hpp"
#include "transaction.hpp"
#include "utxo.hpp"
#include "wallet.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

void print_help() {
    std::cout << R"HELP(BinaryCoin Testnet Alpha v0.1.3

Usage:
  binarycoind -testnet [options]                 Start node in foreground
  binarycoind -testnet -daemon                  Start node in background
  binarycoind -testnet [options] <command>       Run legacy one-shot command

Server options:
  -daemon                 Run in the background
  -daemonwait             Run in background and wait until initialization completes
  -datadir=<dir>          Specify data directory
  -bind=<addr>            P2P bind address (default 0.0.0.0)
  -port=<port>            P2P port (default 26001)
  -rpcbind=<addr>         RPC bind address (default 127.0.0.1)
  -rpcport=<port>         RPC port (default 25001)
  -debug=<category>       Enable debug category (net, rpc, addrman, validation, wallet)
  -printtoconsole         Print logs to the console
  -addnode=<host:port>    Add a manual outbound peer

Chain commands:
  verify-genesis
  init
  status
  storage-info
  reindex
  generate <count>
  print-chain

HD wallet commands:
  wallet-create [--type hd]
  wallet-import [--type hd]
  wallet-upgrade
  wallet-info
  getnewaddress
  validateaddress <tbin1...>
  getbalance
  listunspent
  sendtoaddress <tbin1...> <amount-BIN> [target=6] [economical|conservative]
  sendtopubkey <compressed-pubkey-hex> <amount-BIN> [target=6] [economical|conservative]

Local P2P commands:
  serve [port=26001] [bind=127.0.0.1]
  syncfrom <host:port>
  run-node [port=26001] [bind=0.0.0.0] [manual-peer ...] (deprecated)
  peer-info
  ban-info
  clear-bans
  branch-info

Network options for run-node:
  --max-inbound N     Maximum inbound sessions (default 32)
  --max-outbound N    Maximum saved/manual outbound peers (default 8)
  --ban-seconds N     Persistent non-loopback ban duration (default 86400)

Mempool and transaction commands:
  getrawmempool
  estimatefee <vbytes> [target=6] [economical|conservative]
  decode-tx <hex>

Notes:
  - Blocks use an append-only checksummed store with generation-based indexes and UTXO snapshots.
  - tbin1 addresses are BinaryCoin Bech32-PK wrappers around compressed public keys.
  - Coinbase outputs mature after 100 blocks.
  - generate includes all valid local mempool transactions in the first mined block.
  - Crash recovery ignores incomplete generations; reindex reconstructs from block records.
  - Synchronization uses block locators, headers-first validation, and missing blocks only.
  - Strictly greater accumulated work is still required for live reorganization.
  - Protocol v5 enforces connection limits, duplicate/self rejection, rate limits and ping deadlines.
  - Compiled DNS and fixed bootstrap seeds are always loaded; there are no bootstrap-disable flags.
)HELP";
}


std::uint64_t parse_u64(const std::string& text, const char* field, const std::uint64_t maximum) {
    std::size_t used = 0;
    const unsigned long long value = std::stoull(text, &used, 10);
    if (used != text.size()) throw std::invalid_argument(std::string(field) + " is not a valid integer");
    if (value > maximum) throw std::invalid_argument(std::string(field) + " exceeds allowed maximum");
    return static_cast<std::uint64_t>(value);
}

bincoin::FeeEstimateMode mode_from_positional(const std::vector<std::string>& positional, const std::size_t index) {
    return index < positional.size()
        ? bincoin::parse_fee_mode(positional[index])
        : bincoin::FeeEstimateMode::Economical;
}

std::uint32_t target_from_positional(const std::vector<std::string>& positional, const std::size_t index) {
    return index < positional.size()
        ? static_cast<std::uint32_t>(parse_u64(positional[index], "Confirmation target", 1'008))
        : 6U;
}

void print_transaction(const bincoin::Transaction& transaction) {
    std::cout << "txid=" << transaction.txid() << '\n'
              << "version=" << transaction.version << '\n'
              << "vsize=" << transaction.virtual_size() << '\n'
              << "inputs=" << transaction.inputs.size() << '\n'
              << "outputs=" << transaction.outputs.size() << '\n'
              << "coinbase=" << (transaction.is_coinbase() ? "yes" : "no") << '\n'
              << "locktime=" << transaction.lock_time << '\n';
    for (std::size_t index = 0; index < transaction.outputs.size(); ++index) {
        const auto& output = transaction.outputs[index];
        std::cout << "output[" << index << "].bits=" << output.value << '\n'
                  << "output[" << index << "].bin=" << bincoin::format_bin_amount(output.value) << '\n'
                  << "output[" << index << "].script=" << bincoin::bytes_to_hex(output.script_pubkey) << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        bool testnet = false;
        std::filesystem::path datadir = bincoin::default_testnet_datadir();
        std::vector<std::string> positional;
        bincoin::NetworkPolicy network_policy;
        bincoin::NodeAppOptions node_options;
        node_options.data_directory = datadir;

        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--testnet" || argument == "-testnet") {
                testnet = true;
            } else if (argument == "--datadir") {
                if (++index >= argc) throw std::invalid_argument("--datadir requires a path");
                datadir = argv[index];
            } else if (argument.starts_with("-datadir=")) {
                datadir = argument.substr(9);
            } else if (argument == "-daemonchild") {
                node_options.daemon = true;
                node_options.daemon_child = true;
                node_options.print_to_console = false;
            } else if (argument == "-daemon") {
                node_options.daemon = true;
                node_options.print_to_console = false;
            } else if (argument == "-daemonwait") {
                node_options.daemon = true;
                node_options.daemon_wait = true;
                node_options.print_to_console = false;
            } else if (argument == "-printtoconsole") {
                node_options.print_to_console = true;
            } else if (argument.starts_with("-printtoconsole=")) {
                node_options.print_to_console = argument.substr(16) != "0";
            } else if (argument.starts_with("-bind=")) {
                node_options.p2p_bind = argument.substr(6);
            } else if (argument.starts_with("-port=")) {
                node_options.p2p_port = static_cast<std::uint16_t>(
                    parse_u64(argument.substr(6), "P2P port", 65535));
                if (node_options.p2p_port == 0) throw std::invalid_argument("P2P port must be non-zero");
            } else if (argument.starts_with("-rpcbind=")) {
                node_options.rpc_bind = argument.substr(9);
            } else if (argument.starts_with("-rpcport=")) {
                node_options.rpc_port = static_cast<std::uint16_t>(
                    parse_u64(argument.substr(9), "RPC port", 65535));
                if (node_options.rpc_port == 0) throw std::invalid_argument("RPC port must be non-zero");
            } else if (argument.starts_with("-debug=")) {
                node_options.debug_categories.insert(argument.substr(7));
            } else if (argument == "-debug") {
                node_options.debug_categories.insert("1");
            } else if (argument.starts_with("-addnode=")) {
                node_options.manual_peers.push_back(bincoin::parse_peer_endpoint(argument.substr(9)));
            } else if (argument == "--max-inbound") {
                if (++index >= argc) throw std::invalid_argument("--max-inbound requires a value");
                network_policy.max_inbound = static_cast<std::size_t>(
                    parse_u64(argv[index], "Maximum inbound peers", 1024));
            } else if (argument.starts_with("-maxconnections=")) {
                const auto maximum = static_cast<std::size_t>(
                    parse_u64(argument.substr(16), "Maximum connections", 1088));
                network_policy.max_inbound = maximum > network_policy.max_outbound
                    ? maximum - network_policy.max_outbound : 0;
            } else if (argument == "--max-outbound") {
                if (++index >= argc) throw std::invalid_argument("--max-outbound requires a value");
                network_policy.max_outbound = static_cast<std::size_t>(
                    parse_u64(argv[index], "Maximum outbound peers", 64));
            } else if (argument == "--ban-seconds") {
                if (++index >= argc) throw std::invalid_argument("--ban-seconds requires a value");
                network_policy.ban_seconds = static_cast<std::int64_t>(
                    parse_u64(argv[index], "Ban duration", 31'536'000));
            } else if (argument == "--help" || argument == "-help" || argument == "-h") {
                print_help();
                return 0;
            } else {
                positional.push_back(argument);
            }
        }

        if (!testnet) throw std::invalid_argument("This build is BinaryCoin Testnet Alpha. Pass -testnet.");
        if (datadir.empty()) throw std::invalid_argument("Data directory cannot be empty");
        node_options.data_directory = datadir;
        node_options.network_policy = network_policy;

        if (positional.empty()) {
            return bincoin::launch_node(node_options);
        }
        const std::string& command = positional[0];
        if (command == "run-node") {
            if (positional.size() > 3 + bincoin::MAX_INVENTORY_ITEMS) {
                throw std::invalid_argument("run-node received too many manual peers");
            }
            if (positional.size() >= 2) {
                node_options.p2p_port = static_cast<std::uint16_t>(
                    parse_u64(positional[1], "P2P port", 65535));
            }
            if (positional.size() >= 3) node_options.p2p_bind = positional[2];
            for (std::size_t index = 3; index < positional.size(); ++index) {
                node_options.manual_peers.push_back(bincoin::parse_peer_endpoint(positional[index]));
            }
            std::cerr << "warning: run-node is deprecated; start binarycoind -testnet directly.\n";
            return bincoin::launch_node(node_options);
        }

        if (command == "verify-genesis") {
            const bincoin::BlockHeader genesis = bincoin::testnet_genesis_header();
            const std::string hash = genesis.hash_hex();
            const bool pow_valid = bincoin::check_proof_of_work(genesis.raw_hash(), genesis.bits);
            std::cout << "Network:      testnet\n"
                      << "MessageStart: TBIT\n"
                      << "Version:      " << genesis.version << '\n'
                      << "Time:         " << genesis.time << '\n'
                      << "Bits:         0x" << std::hex << genesis.bits << std::dec << '\n'
                      << "Nonce:        " << genesis.nonce << '\n'
                      << "Merkle root:  " << genesis.merkle_root << '\n'
                      << "Genesis hash: " << hash << '\n'
                      << "PoW valid:    " << (pow_valid ? "yes" : "no") << '\n'
                      << "Genesis txid: " << bincoin::TESTNET_GENESIS_TXID << '\n'
                      << "Genesis BIN:  50.00000000 (excluded from the UTXO set)\n";
            if (hash != bincoin::TESTNET_GENESIS_HASH || !pow_valid) {
                throw std::runtime_error("Testnet genesis verification failed");
            }
            return 0;
        }

        if (command == "estimatefee") {
            if (positional.size() < 2 || positional.size() > 4) {
                throw std::invalid_argument("estimatefee requires vbytes and optional target/mode");
            }
            const std::size_t vbytes = static_cast<std::size_t>(parse_u64(positional[1], "Virtual size", 4'000'000));
            const std::uint32_t target = target_from_positional(positional, 2);
            const auto mode = mode_from_positional(positional, 3);
            const auto quote = bincoin::TestnetFeeEstimator{}.estimate(vbytes, target, mode);
            std::cout << "vbytes=" << vbytes << '\n'
                      << "target=" << quote.confirmation_target << '\n'
                      << "mode=" << bincoin::fee_mode_name(quote.mode) << '\n'
                      << "source=" << bincoin::fee_source_name(quote.source) << '\n'
                      << "feerate_bits_per_kvb=" << quote.rate.bits_per_kvb() << '\n'
                      << "fee_bits=" << quote.fee << '\n';
            return 0;
        }

        if (command == "decode-tx") {
            if (positional.size() != 2) throw std::invalid_argument("decode-tx requires one transaction hex string");
            const auto transaction = bincoin::Transaction::deserialize(bincoin::hex_to_bytes(positional[1]));
            print_transaction(transaction);
            std::cout << "canonical_hex=" << bincoin::bytes_to_hex(transaction.serialize()) << '\n';
            return 0;
        }

        bincoin::TestnetChain chain(datadir);
        if (command == "init") {
            chain.initialize();
            std::cout << "Initialized BinaryCoin Testnet Alpha chain at " << chain.data_directory() << '\n'
                      << "Genesis: " << chain.tip().block.header.hash_hex() << '\n';
            return 0;
        }

        if (command == "wallet-create") {
            std::string backend_type = "hd";
            if (positional.size() == 3 && positional[1] == "--type") backend_type = positional[2];
            else if (positional.size() != 1) throw std::invalid_argument("wallet-create accepts optional --type <backend>");
            chain.initialize();
            bincoin::WalletManager wallet(datadir);
            const std::string phrase = wallet.create(backend_type);
            std::cout << "Created BinaryCoin Testnet Alpha wallet\n"
                      << "wallet=" << wallet.path().string() << '\n'
                      << "wallet_type=" << wallet.kind_name() << '\n'
                      << "crypto_backend=" << bincoin::crypto_backend_name() << '\n'
                      << "address=" << wallet.address() << '\n'
                      << "import_phrase_words=24\n"
                      << phrase << '\n';
            return 0;
        }

        if (command == "wallet-import") {
            std::string backend_type = "hd";
            if (positional.size() == 3 && positional[1] == "--type") backend_type = positional[2];
            else if (positional.size() != 1) throw std::invalid_argument("wallet-import accepts optional --type <backend>");
            chain.initialize();
            bincoin::WalletManager wallet(datadir);
            std::cout << "Enter the 24-word import phrase, then press Enter:\n";
            std::string phrase;
            if (!std::getline(std::cin, phrase) || phrase.empty()) {
                throw std::invalid_argument("No import phrase was entered");
            }
            wallet.import_mnemonic(phrase, backend_type);
            std::cout << "Imported BinaryCoin wallet\n"
                      << "wallet_type=" << wallet.kind_name() << '\n'
                      << "wallet=" << wallet.path().string() << '\n'
                      << "address=" << wallet.address() << '\n';
            return 0;
        }

        if (command == "wallet-upgrade") {
            chain.initialize();
            bincoin::WalletManager wallet(datadir);
            const std::string phrase = wallet.upgrade_legacy();
            std::cout << "Migrated legacy wallet into an HD wallet\n"
                      << "legacy_key_preserved=yes\n"
                      << "address=" << wallet.address() << '\n'
                      << "import_phrase_words=24\n"
                      << phrase << "\n";
            return 0;
        }

        if (command == "reindex") {
            if (positional.size() != 1) throw std::invalid_argument("reindex accepts no arguments");
            chain.reindex();
            const auto stats = chain.storage_stats();
            std::cout << "reindexed=yes\n"
                      << "blocks=" << chain.tip().height << '\n'
                      << "bestblockhash=" << chain.tip().block.header.hash_hex() << '\n'
                      << "utxos=" << chain.utxo_set().size() << '\n'
                      << "storage_generation=" << stats.generation << '\n';
            return 0;
        }

        if (command == "validateaddress") {
            if (positional.size() != 2) throw std::invalid_argument("validateaddress requires one address");
            const bool valid = bincoin::valid_testnet_address(positional[1]);
            std::cout << "isvalid=" << (valid ? "yes" : "no") << '\n';
            if (valid) {
                std::cout << "network=testnet\n"
                          << "address_type=bech32-pk-v0\n"
                          << "public_key=" << bincoin::bytes_to_hex(bincoin::decode_testnet_address(positional[1])) << '\n';
            }
            return valid ? 0 : 1;
        }

        chain.initialize();

        if (command == "serve") {
            if (positional.size() > 3) {
                throw std::invalid_argument("serve accepts optional port and bind address");
            }
            const std::uint16_t port = positional.size() >= 2
                ? static_cast<std::uint16_t>(parse_u64(positional[1], "P2P port", 65535))
                : bincoin::DEFAULT_TESTNET_P2P_PORT;
            if (port == 0) throw std::invalid_argument("P2P port must be non-zero");
            const std::string bind = positional.size() >= 3 ? positional[2] : "127.0.0.1";
            bincoin::TestnetP2pServer server(datadir, bind, port);
            std::cout << "BinaryCoin testnet P2P listening\n"
                      << "bind=" << bind << '\n'
                      << "port=" << server.bound_port() << '\n'
                      << "message_start=TBIT\n"
                      << "protocol=" << bincoin::P2P_PROTOCOL_VERSION << '\n'
                      << "Press Ctrl+C to stop.\n";
            std::cout.flush();
            server.serve_forever();
            return 0;
        }

        if (command == "syncfrom") {
            if (positional.size() != 2) throw std::invalid_argument("syncfrom requires host:port");
            const auto endpoint = bincoin::parse_peer_endpoint(positional[1]);
            const auto result = bincoin::sync_from_peer(datadir, endpoint);
            std::cout << "peer=" << endpoint.host << ':' << endpoint.port << '\n'
                      << "peer_user_agent=" << result.peer.user_agent << '\n'
                      << "peer_protocol=" << result.peer.protocol_version << '\n'
                      << "peer_height=" << result.peer.best_height << '\n'
                      << "peer_tip=" << result.peer.tip_hash << '\n'
                      << "peer_chainwork=" << result.peer.chainwork << '\n'
                      << "local_height_before=" << result.local_height_before << '\n'
                      << "local_chainwork_before=" << result.local_chainwork_before << '\n'
                      << "headers_received=" << result.headers_received << '\n'
                      << "blocks_received=" << result.blocks_received << '\n'
                      << "blocks_reused_from_side_store=" << result.blocks_reused_from_side_store << '\n'
                      << "candidate_chainwork=" << result.candidate_chainwork << '\n'
                      << "candidate_activated=" << (result.activated_candidate ? "yes" : "no") << '\n'
                      << "reorganized=" << (result.reorganized ? "yes" : "no") << '\n'
                      << "fork_height=" << result.fork_height << '\n'
                      << "disconnected_blocks=" << result.disconnected_blocks << '\n'
                      << "connected_blocks=" << result.connected_blocks << '\n'
                      << "resurrected_transactions=" << result.resurrected_transactions << '\n'
                      << "mempool_received=" << result.mempool_received << '\n'
                      << "local_height_after=" << result.local_height_after << '\n'
                      << "local_chainwork_after=" << result.local_chainwork_after << '\n';
            return 0;
        }

        if (command == "peer-info") {
            bincoin::PeerStore peers(datadir);
            peers.load();
            std::size_t index = 0;
            for (const auto& peer : peers.records()) {
                std::cout << "peer[" << index++ << "]=" << peer.endpoint.host << ':' << peer.endpoint.port
                          << " services=" << peer.services
                          << " last_seen=" << peer.last_seen
                          << " last_success=" << peer.last_success
                          << " failures=" << peer.failures
                          << " manual=" << (peer.manual ? "yes" : "no") << '\n';
            }
            std::cout << "count=" << index << '\n'
                      << "path=" << peers.path().string() << '\n';
            return 0;
        }

        if (command == "ban-info") {
            bincoin::BanStore bans(datadir);
            bans.load();
            std::size_t index = 0;
            for (const auto& ban : bans.records()) {
                std::cout << "ban[" << index++ << "]=" << ban.host
                          << " until=" << ban.banned_until
                          << " score=" << ban.score
                          << " reason=" << ban.reason << '\n';
            }
            std::cout << "count=" << index << '\n'
                      << "path=" << bans.path().string() << '\n';
            return 0;
        }

        if (command == "clear-bans") {
            bincoin::BanStore bans(datadir);
            bans.load();
            bans.clear();
            std::cout << "cleared=yes\npath=" << bans.path().string() << '\n';
            return 0;
        }

        if (command == "branch-info") {
            bincoin::SideBranchStore branches(datadir);
            branches.load();
            std::cout << "side_branch_blocks=" << branches.size() << '\n'
                      << "path=" << branches.path().string() << '\n';
            for (const auto& block : branches.all()) {
                std::cout << "height=" << block.height
                          << " hash=" << block.block.header.hash_hex()
                          << " previous=" << block.block.header.previous_hash << '\n';
            }
            return 0;
        }

        if (command == "storage-info") {
            const auto stats = chain.storage_stats();
            std::cout << "backend=append-only-v1\n"
                      << "generation=" << stats.generation << '\n'
                      << "active_height=" << stats.active_height << '\n'
                      << "block_records=" << stats.block_records << '\n'
                      << "block_file_bytes=" << stats.block_file_bytes << '\n'
                      << "recovered=" << (stats.recovered ? "yes" : "no") << '\n'
                      << "migrated_legacy=" << (stats.migrated_legacy ? "yes" : "no") << '\n'
                      << "blocks_path=" << (datadir / "blocks" / "blk00000.dat").string() << '\n'
                      << "manifest_path=" << (datadir / "chainstate" / "manifest-v1").string() << '\n';
            return 0;
        }

        if (command == "status") {
            chain.verify();
            bincoin::TestnetMempool mempool(datadir);
            mempool.load();
            bincoin::PeerStore peer_store(datadir);
            peer_store.load();
            bincoin::SideBranchStore branches(datadir);
            branches.load();
            const auto& tip = chain.tip();
            const auto storage = chain.storage_stats();
            std::cout << "network=testnet\n"
                      << "message_start=TBIT\n"
                      << "datadir=" << chain.data_directory().string() << '\n'
                      << "blocks=" << tip.height << '\n'
                      << "bestblockhash=" << tip.block.header.hash_hex() << '\n'
                      << "chainwork=" << chain.chainwork_hex() << '\n'
                      << "utxos=" << chain.utxo_set().size() << '\n'
                      << "storage_backend=append-only-v1\n"
                      << "storage_generation=" << storage.generation << '\n'
                      << "block_records=" << storage.block_records << '\n'
                      << "block_file_bytes=" << storage.block_file_bytes << '\n'
                      << "storage_recovered=" << (storage.recovered ? "yes" : "no") << '\n'
                      << "mempool_transactions=" << mempool.transactions().size() << '\n'
                      << "known_peers=" << peer_store.records().size() << '\n'
                      << "side_branch_blocks=" << branches.size() << '\n'
                      << "issued_bits=" << chain.issued_supply() << '\n'
                      << "issued_bin=" << bincoin::format_bin_amount(chain.issued_supply()) << '\n'
                      << "halving_interval=" << bincoin::TESTNET_HALVING_INTERVAL << '\n'
                      << "coinbase_maturity=" << bincoin::COINBASE_MATURITY << '\n';
            return 0;
        }

        if (command == "wallet-info" || command == "getnewaddress" || command == "getbalance" || command == "listunspent" ||
            command == "sendtoaddress" || command == "sendtopubkey" || command == "generate") {
            bincoin::WalletManager wallet(datadir);
            wallet.load();

            bincoin::TestnetMempool mempool(datadir);
            mempool.load();

            if (command == "wallet-info") {
                std::cout << "wallet_type=" << wallet.kind_name() << '\n'
                          << "wallet_path=" << wallet.path().string() << '\n'
                          << "crypto_backend=" << bincoin::crypto_backend_name() << '\n'
                          << "master_fingerprint=" << wallet.master_fingerprint_hex() << '\n'
                          << "address=" << wallet.address() << '\n'
                          << "public_key=" << wallet.public_key_hex() << '\n'
                          << "locking_script=" << bincoin::bytes_to_hex(wallet.locking_script()) << '\n';
                return 0;
            }

            if (command == "getnewaddress") {
                if (positional.size() != 1) throw std::invalid_argument("getnewaddress accepts no arguments");
                std::cout << wallet.get_new_address() << '\n';
                return 0;
            }

            if (command == "generate") {
                if (positional.size() != 2) throw std::invalid_argument("generate requires exactly one count");
                const auto count = parse_u64(positional[1], "Block count", 100'000);
                if (count == 0) throw std::invalid_argument("Block count must be positive");
                const std::size_t included = mempool.transactions().size();
                chain.generate(count, wallet.locking_script(), mempool.transactions());
                mempool.clear();
                std::cout << "Generated " << count << " block(s)\n"
                          << "Included mempool transactions: " << included << '\n'
                          << "Height: " << chain.tip().height << '\n'
                          << "Tip: " << chain.tip().block.header.hash_hex() << '\n'
                          << "Chainwork: " << chain.chainwork_hex() << '\n';
                return 0;
            }

            const std::uint64_t spend_height = chain.tip().height + 1;
            const auto chain_utxos = chain.utxo_set();
            const auto effective_utxos = mempool.effective_utxos(chain_utxos, spend_height);

            if (command == "getbalance") {
                const auto balance = wallet.balance(effective_utxos, spend_height);
                std::cout << "spendable_bits=" << balance.spendable << '\n'
                          << "spendable_bin=" << bincoin::format_bin_amount(balance.spendable) << '\n'
                          << "immature_bits=" << balance.immature << '\n'
                          << "immature_bin=" << bincoin::format_bin_amount(balance.immature) << '\n'
                          << "total_bits=" << balance.total << '\n'
                          << "total_bin=" << bincoin::format_bin_amount(balance.total) << '\n';
                return 0;
            }

            if (command == "listunspent") {
                std::size_t count = 0;
                for (const auto& [outpoint, coin] : effective_utxos) {
                    if (!wallet.owns_script(coin.output.script_pubkey)) continue;
                    const bool mature = !coin.coinbase || spend_height >= coin.height + bincoin::COINBASE_MATURITY;
                    std::cout << "txid=" << outpoint.txid
                              << " vout=" << outpoint.index
                              << " amount=" << bincoin::format_bin_amount(coin.output.value)
                              << " height=" << coin.height
                              << " coinbase=" << (coin.coinbase ? "yes" : "no")
                              << " mature=" << (mature ? "yes" : "no") << '\n';
                    ++count;
                }
                std::cout << "count=" << count << '\n';
                return 0;
            }

            if (command == "sendtoaddress") {
                if (positional.size() < 3 || positional.size() > 5) {
                    throw std::invalid_argument("sendtoaddress requires address, amount and optional target/mode");
                }
                const auto destination = bincoin::decode_testnet_address(positional[1]);
                const bincoin::Amount amount = bincoin::parse_bin_amount(positional[2]);
                const std::uint32_t target = target_from_positional(positional, 3);
                const auto mode = mode_from_positional(positional, 4);
                const auto fee_quote = bincoin::TestnetFeeEstimator{}.estimate(1, target, mode);
                const auto transaction = wallet.create_payment(
                    effective_utxos, spend_height, destination, amount, fee_quote.rate);
                bincoin::UtxoSet validation = effective_utxos;
                const bincoin::Amount fee = bincoin::apply_transaction(transaction, spend_height, validation);
                mempool.add(transaction, chain_utxos, spend_height);
                std::cout << "txid=" << transaction.txid() << '\n'
                          << "destination=" << positional[1] << '\n'
                          << "amount_bin=" << bincoin::format_bin_amount(amount) << '\n'
                          << "fee_bits=" << fee << '\n'
                          << "vsize=" << transaction.virtual_size() << '\n'
                          << "fee_mode=" << bincoin::fee_mode_name(mode) << '\n'
                          << "fee_source=fallback\n"
                          << "mempool=yes\n";
                return 0;
            }

            if (command == "sendtopubkey") {
                if (positional.size() < 3 || positional.size() > 5) {
                    throw std::invalid_argument("sendtopubkey requires pubkey, amount and optional target/mode");
                }
                const auto destination = bincoin::hex_to_bytes(positional[1]);
                if (!bincoin::valid_compressed_public_key(destination)) {
                    throw std::invalid_argument("Destination must be a compressed secp256k1 public key");
                }
                const bincoin::Amount amount = bincoin::parse_bin_amount(positional[2]);
                const std::uint32_t target = target_from_positional(positional, 3);
                const auto mode = mode_from_positional(positional, 4);
                const auto fee_quote = bincoin::TestnetFeeEstimator{}.estimate(1, target, mode);
                const auto transaction = wallet.create_payment(
                    effective_utxos, spend_height, destination, amount, fee_quote.rate);
                bincoin::UtxoSet validation = effective_utxos;
                const bincoin::Amount fee = bincoin::apply_transaction(transaction, spend_height, validation);
                mempool.add(transaction, chain_utxos, spend_height);
                std::cout << "txid=" << transaction.txid() << '\n'
                          << "amount_bin=" << bincoin::format_bin_amount(amount) << '\n'
                          << "fee_bits=" << fee << '\n'
                          << "vsize=" << transaction.virtual_size() << '\n'
                          << "fee_mode=" << bincoin::fee_mode_name(mode) << '\n'
                          << "fee_source=fallback\n"
                          << "mempool=yes\n";
                return 0;
            }
        }

        if (command == "getrawmempool") {
            bincoin::TestnetMempool mempool(datadir);
            mempool.load();
            for (const auto& transaction : mempool.transactions()) {
                std::cout << transaction.txid() << '\n';
            }
            std::cout << "count=" << mempool.transactions().size() << '\n';
            return 0;
        }

        if (command == "print-chain") {
            boost::multiprecision::cpp_int cumulative_work = 0;
            for (const bincoin::StoredBlock& stored : chain.blocks()) {
                cumulative_work += bincoin::block_proof(stored.block.header.bits);
                std::cout << "height=" << stored.height
                          << " hash=" << stored.block.header.hash_hex()
                          << " previous=" << stored.block.header.previous_hash
                          << " merkle=" << stored.block.header.merkle_root
                          << " txs=" << stored.block.transactions.size()
                          << " time=" << stored.block.header.time
                          << " nonce=" << stored.block.header.nonce
                          << " chainwork=" << bincoin::uint256_to_hex(cumulative_work) << '\n';
                for (const auto& transaction : stored.block.transactions) {
                    std::cout << "  txid=" << transaction.txid()
                              << " coinbase=" << (transaction.is_coinbase() ? "yes" : "no")
                              << " vsize=" << transaction.virtual_size() << '\n';
                }
            }
            return 0;
        }

        throw std::invalid_argument("Unknown command: " + command);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
