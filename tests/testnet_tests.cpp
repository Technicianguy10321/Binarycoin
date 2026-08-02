#include "amount.hpp"
#include "bech32.hpp"
#include "block.hpp"
#include "bootstrap.hpp"
#include "branch_store.hpp"
#include "chain.hpp"
#include "fees.hpp"
#include "hash.hpp"
#include "hdkey.hpp"
#include "key.hpp"
#include "mempool.hpp"
#include "mnemonic.hpp"
#include "net.hpp"
#include "merkle.hpp"
#include "params.hpp"
#include "pow.hpp"
#include "platform.hpp"
#include "script.hpp"
#include "serialize.hpp"
#include "transaction.hpp"
#include "utxo.hpp"
#include "wallet.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <memory>
#include <exception>
#include <stdexcept>
#include <thread>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}


bincoin::NetworkPolicy local_test_policy() {
    bincoin::NetworkPolicy policy;
    policy.use_compiled_seeds = false;
    policy.ping_interval = std::chrono::milliseconds(250);
    policy.ping_timeout = std::chrono::milliseconds(1500);
    policy.socket_timeout_seconds = 2;
    return policy;
}

void wait_for_condition(
    const std::function<bool()>& condition,
    const std::chrono::milliseconds timeout,
    const char* failure_message
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            if (condition()) return;
        } catch (const std::exception&) {
            // Atomic file replacement can briefly race a test-side read.
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error(failure_message);
}

} // namespace

int main() {
    try {
        static_assert(bincoin::MAINNET_MESSAGE_START[0] == 'B');
        static_assert(bincoin::MAINNET_MESSAGE_START[1] == 'I');
        static_assert(bincoin::MAINNET_MESSAGE_START[2] == 'T');
        static_assert(bincoin::MAINNET_MESSAGE_START[3] == 'S');
        static_assert(bincoin::TESTNET_DNS_SEEDS.size() == 1);
        static_assert(bincoin::TESTNET_FIXED_SEEDS.size() == 1);
        require(bincoin::TESTNET_DNS_SEEDS.front() == "binarycoin-testnet.ezgateway.net",
                "Compiled DNS seed mismatch");
        require(bincoin::TESTNET_FIXED_SEEDS.front().host ==
                    "binarycoin-testnet.ezgateway.net",
                "Compiled fixed seed host mismatch");
        require(bincoin::TESTNET_FIXED_SEEDS.front().port == 26001,
                "Compiled fixed seed port mismatch");

        const auto bootstrap_directory = std::filesystem::temp_directory_path() /
            "binarycoin-v014-bootstrap-test";
        std::filesystem::remove_all(bootstrap_directory);
        bincoin::PeerStore bootstrap_store(bootstrap_directory);
        bootstrap_store.load();
        const auto bootstrap_result = bincoin::bootstrap_testnet_seeds(
            bootstrap_store,
            [](const std::string_view hostname, const std::uint16_t port) {
                require(hostname == "binarycoin-testnet.ezgateway.net",
                        "Resolver received wrong DNS seed");
                require(port == 26001, "Resolver received wrong seed port");
                return std::vector<bincoin::PeerEndpoint>{
                    {.host = "198.51.100.10", .port = port},
                    {.host = "2001:db8::10", .port = port},
                };
            });
        require(bootstrap_result.fixed_entries_added == 1,
                "Compiled fixed seed was not inserted");
        require(bootstrap_result.dns_entries_added == 2,
                "Resolved DNS seed endpoints were not inserted");
        require(bootstrap_result.dns_names_resolved == 1,
                "DNS seed resolution result was not recorded");
        bincoin::PeerStore bootstrap_reloaded(bootstrap_directory);
        bootstrap_reloaded.load();
        require(bootstrap_reloaded.records().size() == 3,
                "Seed endpoints were not persisted in peers.dat");
        const auto duplicate_result = bincoin::bootstrap_testnet_seeds(
            bootstrap_reloaded,
            [](std::string_view, std::uint16_t port) {
                return std::vector<bincoin::PeerEndpoint>{
                    {.host = "198.51.100.10", .port = port},
                    {.host = "2001:db8::10", .port = port},
                };
            });
        require(duplicate_result.fixed_entries_added == 0 &&
                    duplicate_result.dns_entries_added == 0,
                "Seed bootstrap did not deduplicate existing peers");
        require(bincoin::resolve_dns_seed("localhost", 26001).empty(),
                "DNS resolver accepted a non-public loopback answer");
        std::filesystem::remove_all(bootstrap_directory);

        const auto genesis = bincoin::testnet_genesis_header();
        require(genesis.serialize().size() == 80, "Genesis header must be exactly 80 bytes");
        require(genesis.hash_hex() == bincoin::TESTNET_GENESIS_HASH, "Genesis hash mismatch");
        require(bincoin::check_proof_of_work(genesis.raw_hash(), genesis.bits), "Genesis PoW invalid");
        require(bincoin::block_subsidy(209'999) == 50 * bincoin::BITS_PER_BIN, "Pre-halving subsidy mismatch");
        require(bincoin::block_subsidy(210'000) == 25 * bincoin::BITS_PER_BIN, "Halving subsidy mismatch");

        // v0.1.4 hard-fork difficulty schedule.
        std::vector<bincoin::StoredBlock> difficulty_history;
        difficulty_history.reserve(static_cast<std::size_t>(
            bincoin::TESTNET_DIFFICULTY_FORK_HEIGHT + 1));
        for (std::uint64_t height = 0; height < bincoin::TESTNET_DIFFICULTY_FORK_HEIGHT; ++height) {
            bincoin::Block block;
            block.header.time = 1'800'000'000U + static_cast<std::uint32_t>(height);
            block.header.bits = height == 0 ? bincoin::TESTNET_GENESIS_BITS : bincoin::TESTNET_BITS;
            difficulty_history.push_back(bincoin::StoredBlock{
                .height = height,
                .block = std::move(block),
            });
        }
        require(bincoin::expected_testnet_bits(
                    std::span<const bincoin::StoredBlock>(
                        difficulty_history.data(), static_cast<std::size_t>(29)), 29) ==
                    bincoin::TESTNET_BITS,
                "Difficulty fork activated before block 30");

        const auto quarter_target = bincoin::compact_to_target(bincoin::TESTNET_BITS) / 4;
        const std::uint32_t first_retarget_bits = bincoin::target_to_compact(quarter_target);
        require(bincoin::expected_testnet_bits(
                    std::span<const bincoin::StoredBlock>(difficulty_history), 30) ==
                    first_retarget_bits,
                "Fast 20-block window did not apply the 4x hardening clamp");

        bincoin::Block fork_block;
        fork_block.header.time = difficulty_history.back().block.header.time + 1U;
        fork_block.header.bits = first_retarget_bits;
        difficulty_history.push_back(bincoin::StoredBlock{
            .height = 30,
            .block = std::move(fork_block),
        });
        require(bincoin::expected_testnet_bits(
                    std::span<const bincoin::StoredBlock>(difficulty_history), 31) ==
                    first_retarget_bits,
                "Difficulty changed outside a retarget boundary");

        // v0.8 wallet vectors: physical mnemonic round-trip, BIP32 vector 1,
        // and BinaryCoin testnet Bech32-PK address integrity.
        bincoin::Entropy256 zero_entropy{};
        const std::string zero_phrase = bincoin::entropy_to_mnemonic(zero_entropy);
        require(bincoin::mnemonic_words(zero_phrase).size() == 24, "24-word mnemonic length mismatch");
        require(bincoin::mnemonic_to_entropy(zero_phrase) == zero_entropy, "Mnemonic entropy round trip mismatch");

        const auto vector_seed = bincoin::hex_to_bytes("000102030405060708090a0b0c0d0e0f");
        const auto vector_master = bincoin::bip32_master(vector_seed);
        require(bincoin::bytes_to_hex(vector_master.secret) ==
                    "e8f32e723decf4051aefac8e2c93c9c5b214313817cdb01a1494b917c8436b35",
                "BIP32 master secret vector mismatch");
        require(bincoin::bytes_to_hex(vector_master.chain_code) ==
                    "873dff81c02f525623fd1fe5167eac3a55a049de3d314bb42ee227ffed37d508",
                "BIP32 master chain-code vector mismatch");
        const auto vector_child = vector_master.derive(bincoin::HARDENED);
        require(bincoin::bytes_to_hex(vector_child.secret) ==
                    "edb2e14f9ee77d26dd93b4ecede8d16ed408ce149b6cd80b0715a2d911a0afea",
                "BIP32 hardened child secret vector mismatch");
        require(bincoin::bytes_to_hex(vector_child.chain_code) ==
                    "47fdacbd0f1097043b78c63c20c34ef4ed9a111d980047ad16282c7ae6236141",
                "BIP32 hardened child chain-code vector mismatch");

        const auto address_key = bincoin::Secp256k1Key::generate();
        const std::string address = bincoin::encode_testnet_address(address_key.compressed_public_key());
        require(address.starts_with("tbin1"), "Testnet address prefix mismatch");
        require(bincoin::decode_testnet_address(address) == address_key.compressed_public_key(),
                "Testnet address public-key round trip mismatch");
        std::string damaged_address = address;
        damaged_address.back() = damaged_address.back() == 'q' ? 'p' : 'q';
        require(!bincoin::valid_testnet_address(damaged_address), "Damaged address checksum was accepted");

        const auto hd_import_root = std::filesystem::temp_directory_path() / "binarycoin-testnet-alpha-hd-import-test";
        std::filesystem::remove_all(hd_import_root);
        bincoin::TestnetWallet generated_wallet(hd_import_root / "generated");
        const std::string generated_phrase = generated_wallet.create();
        const std::string generated_address = generated_wallet.address();
        bincoin::TestnetWallet imported_wallet(hd_import_root / "imported");
        imported_wallet.import_mnemonic(generated_phrase);
        require(imported_wallet.address() == generated_address, "Imported HD wallet address mismatch");
        require(imported_wallet.get_new_address() != generated_address, "HD wallet did not derive a new address");
        std::filesystem::remove_all(hd_import_root);

        const auto signing_key = bincoin::Secp256k1Key::generate();
        const auto locking_script = bincoin::make_p2pk_script(signing_key.compressed_public_key());
        const auto coinbase = bincoin::make_testnet_coinbase(
            1, bincoin::block_subsidy(1), locking_script);
        require(coinbase.is_coinbase(), "Coinbase detection failed");
        const auto serialized = coinbase.serialize();
        const auto decoded = bincoin::Transaction::deserialize(serialized);
        require(decoded.serialize() == serialized, "Transaction round trip mismatch");
        require(decoded.txid() == coinbase.txid(), "Transaction ID changed after decoding");
        require(bincoin::merkle_root_hex(std::vector<bincoin::Transaction>{coinbase}) == coinbase.txid(),
                "Single-transaction Merkle root mismatch");

        bincoin::Transaction signature_test;
        signature_test.inputs.push_back(bincoin::TxInput{
            .previous_output = bincoin::OutPoint{.txid = std::string(64, '1'), .index = 0},
            .script_sig = {},
            .sequence = 0xffffffffU,
        });
        signature_test.outputs.push_back(bincoin::TxOutput{
            .value = bincoin::BITS_PER_BIN,
            .script_pubkey = locking_script,
        });
        bincoin::sign_p2pk_input(signature_test, 0, locking_script, signing_key);
        require(bincoin::verify_p2pk_input(signature_test, 0, locking_script), "P2PK signature failed");
        signature_test.outputs[0].value += 1;
        require(!bincoin::verify_p2pk_input(signature_test, 0, locking_script),
                "Modified signed transaction was accepted");

        const auto economical = bincoin::TestnetFeeEstimator{}.estimate(141, 6, bincoin::FeeEstimateMode::Economical);
        require(economical.rate.bits_per_kvb() == 1000, "Economical fallback rate mismatch");
        require(economical.fee == 141, "Fee rounding mismatch");

        const auto temporary = std::filesystem::temp_directory_path() / "binarycoin-testnet-v030-test";
        std::filesystem::remove_all(temporary);

        std::string payment_txid;
        std::string recipient_public_key;
        {
            bincoin::TestnetChain chain(temporary);
            chain.initialize();

            bincoin::TestnetWallet wallet(temporary);
            (void)wallet.create();
            wallet.load();
            require(wallet.public_key_hex().size() == 66, "Compressed wallet public key length mismatch");

            chain.generate(101, wallet.locking_script());
            require(chain.tip().height == 101, "Unexpected generated chain height");
            chain.verify();
            const auto locator = bincoin::make_block_locator(chain.blocks());
            require(!locator.empty(), "Block locator is empty");
            require(locator.front() == chain.tip().block.header.hash_hex(),
                    "Block locator does not begin at active tip");
            require(locator.back() == bincoin::TESTNET_GENESIS_HASH,
                    "Block locator does not terminate at genesis");
            require(locator.size() < chain.blocks().size(),
                    "Block locator did not use exponential backoff");

            const std::uint64_t spend_height = chain.tip().height + 1;
            const auto chain_utxos = chain.utxo_set();
            const auto balance = wallet.balance(chain_utxos, spend_height);
            require(balance.spendable == 2 * 50 * bincoin::BITS_PER_BIN,
                    "Unexpected mature wallet balance at height 101");
            require(balance.immature == 99 * 50 * bincoin::BITS_PER_BIN,
                    "Unexpected immature wallet balance at height 101");

            const auto recipient = bincoin::Secp256k1Key::generate();
            recipient_public_key = recipient.compressed_public_key_hex();
            const auto fee_rate = bincoin::TestnetFeeEstimator{}
                .estimate(1, 6, bincoin::FeeEstimateMode::Economical).rate;
            const auto payment = wallet.create_payment(
                chain_utxos,
                spend_height,
                recipient.compressed_public_key(),
                bincoin::BITS_PER_BIN,
                fee_rate);
            payment_txid = payment.txid();

            bincoin::UtxoSet validation = chain_utxos;
            const bincoin::Amount fee = bincoin::apply_transaction(payment, spend_height, validation);
            require(fee > 0, "Payment did not pay a fee");
            require(validation.contains(bincoin::OutPoint{.txid = payment_txid, .index = 0}),
                    "Recipient output missing after transaction application");

            bincoin::TestnetMempool mempool(temporary);
            mempool.load();
            mempool.add(payment, chain_utxos, spend_height);
            require(mempool.transactions().size() == 1, "Mempool did not accept payment");

            bool double_spend_rejected = false;
            try {
                bincoin::UtxoSet already_spent = validation;
                (void)bincoin::apply_transaction(payment, spend_height, already_spent);
            } catch (const std::exception&) {
                double_spend_rejected = true;
            }
            require(double_spend_rejected, "Double-spend was not rejected");

            chain.generate(1, wallet.locking_script(), mempool.transactions());
            mempool.clear();
            require(chain.tip().height == 102, "Payment block height mismatch");
            require(chain.tip().block.transactions.size() == 2, "Payment block transaction count mismatch");
            require(chain.tip().block.transactions[1].txid() == payment_txid, "Payment txid changed in block");
            chain.verify();
        }
        {
            bincoin::TestnetChain reloaded(temporary);
            reloaded.load();
            require(reloaded.tip().height == 102, "Reloaded chain height mismatch");
            require(reloaded.utxo_set().contains(bincoin::OutPoint{.txid = payment_txid, .index = 0}),
                    "Recipient UTXO missing after reload");

            bincoin::TestnetWallet wallet(temporary);
            wallet.load();
            require(!wallet.public_key_hex().empty(), "Reloaded wallet public key missing");

            bincoin::TestnetMempool mempool(temporary);
            mempool.load();
            require(mempool.transactions().empty(), "Mempool was not cleared after mining");
            reloaded.verify();
        }

        // v0.9 durable storage: committed files, manifest recovery, damaged UTXO recovery, and reindex.
        require(std::filesystem::exists(temporary / "blocks" / "blk00000.dat"),
                "Append-only block file was not created");
        require(std::filesystem::exists(temporary / "chainstate" / "manifest-v1"),
                "Chainstate manifest was not created");
        {
            std::ofstream broken_manifest(temporary / "chainstate" / "manifest-v1", std::ios::trunc);
            broken_manifest << "interrupted manifest\n";
        }
        {
            bincoin::TestnetChain recovered(temporary);
            recovered.load();
            require(recovered.tip().height == 102, "Manifest recovery changed active height");
            require(recovered.storage_stats().recovered, "Manifest recovery was not reported");
        }
        {
            std::filesystem::path newest_utxo;
            for (const auto& entry : std::filesystem::directory_iterator(temporary / "chainstate")) {
                const auto name = entry.path().filename().string();
                if (name.starts_with("utxo-v1-") && name.ends_with(".tsv") &&
                    (newest_utxo.empty() || entry.path().filename() > newest_utxo.filename())) {
                    newest_utxo = entry.path();
                }
            }
            require(!newest_utxo.empty(), "No UTXO generation was found");
            std::ofstream damaged_utxo(newest_utxo, std::ios::trunc);
            damaged_utxo << "damaged snapshot\n";
        }
        {
            bincoin::TestnetChain recovered(temporary);
            recovered.load();
            require(recovered.tip().height == 102, "UTXO recovery changed active height");
            require(recovered.utxo_set().contains(bincoin::OutPoint{.txid = payment_txid, .index = 0}),
                    "UTXO recovery lost recipient output");
        }
        {
            std::ofstream tail(temporary / "blocks" / "blk00000.dat", std::ios::binary | std::ios::app);
            tail << "INTERRUPTED_BLOCK_TAIL";
        }
        {
            bincoin::TestnetChain reindexed(temporary);
            reindexed.reindex();
            require(reindexed.tip().height == 102, "Reindex did not preserve the best valid chain");
            require(reindexed.utxo_set().contains(bincoin::OutPoint{.txid = payment_txid, .index = 0}),
                    "Reindex lost recipient output");
        }
        std::filesystem::remove_all(temporary);

        const auto p2p_root = std::filesystem::temp_directory_path() / "binarycoin-testnet-v040-p2p-test";
        const auto server_directory = p2p_root / "server";
        const auto client_directory = p2p_root / "client";
        std::filesystem::remove_all(p2p_root);

        bincoin::TestnetWallet client_wallet(client_directory);
        {
            bincoin::TestnetChain client_chain(client_directory);
            client_chain.initialize();
            (void)client_wallet.create();
            client_wallet.load();
        }

        std::string network_payment_txid;
        {
            bincoin::TestnetChain server_chain(server_directory);
            server_chain.initialize();
            bincoin::TestnetWallet server_wallet(server_directory);
            (void)server_wallet.create();
            server_wallet.load();
            server_chain.generate(101, server_wallet.locking_script());

            const auto chain_utxos = server_chain.utxo_set();
            const auto payment = server_wallet.create_payment(
                chain_utxos,
                server_chain.tip().height + 1,
                bincoin::hex_to_bytes(client_wallet.public_key_hex()),
                125'000'000,
                bincoin::FeeRate(1000));
            network_payment_txid = payment.txid();
            bincoin::TestnetMempool server_mempool(server_directory);
            server_mempool.load();
            server_mempool.add(payment, chain_utxos, server_chain.tip().height + 1);
            server_chain.generate(1, server_wallet.locking_script(), server_mempool.transactions());
            server_mempool.clear();
        }

        bincoin::TestnetP2pServer server(server_directory, "127.0.0.1", 0);
        std::exception_ptr server_error;
        std::thread server_thread([&] {
            try {
                server.serve_once();
            } catch (...) {
                server_error = std::current_exception();
            }
        });

        const auto sync = bincoin::sync_from_peer(
            client_directory,
            bincoin::PeerEndpoint{.host = "127.0.0.1", .port = server.bound_port()});
        server_thread.join();
        if (server_error) std::rethrow_exception(server_error);

        require(sync.local_height_before == 0, "P2P client did not begin at genesis");
        require(sync.local_height_after == 102, "P2P client did not reach server height");
        require(sync.headers_received == 102, "P2P headers-first count mismatch");
        require(sync.blocks_received == 102, "P2P missing-block download count mismatch");
        require(sync.blocks_reused_from_side_store == 0, "Initial sync unexpectedly reused side blocks");
        require(sync.activated_candidate, "P2P candidate chain was not activated");
        require(!sync.reorganized, "Initial genesis-only sync was incorrectly marked as reorganization");
        require(sync.local_chainwork_after == sync.peer.chainwork, "P2P chainwork mismatch after sync");
        require(sync.peer.genesis_hash == bincoin::TESTNET_GENESIS_HASH, "P2P genesis identity mismatch");

        {
            bincoin::TestnetChain synchronized(client_directory);
            synchronized.load();
            require(synchronized.tip().height == 102, "Synchronized chain reload height mismatch");
            require(synchronized.utxo_set().contains(
                bincoin::OutPoint{.txid = network_payment_txid, .index = 0}),
                "P2P recipient output missing after synchronization");
            client_wallet.load();
            const auto recipient_balance = client_wallet.balance(
                synchronized.utxo_set(), synchronized.tip().height + 1);
            require(recipient_balance.spendable == 125'000'000,
                    "P2P recipient wallet did not detect synchronized payment");
        }
        std::filesystem::remove_all(p2p_root);

        // Accumulated-chainwork fork selection and safe reorganization.
        const auto reorg_root = std::filesystem::temp_directory_path() / "binarycoin-testnet-v070-reorg-test";
        const auto node_a_directory = reorg_root / "node-a";
        const auto node_b_directory = reorg_root / "node-b";
        std::filesystem::remove_all(reorg_root);

        bincoin::TestnetWallet wallet_a(node_a_directory);
        bincoin::TestnetWallet wallet_b(node_b_directory);
        {
            bincoin::TestnetChain chain_a(node_a_directory);
            chain_a.initialize();
            (void)wallet_a.create();
            wallet_a.load();
            chain_a.generate(101, wallet_a.locking_script());

            bincoin::TestnetChain chain_b(node_b_directory);
            chain_b.initialize();
            (void)wallet_b.create();
            wallet_b.load();
        }

        // Establish a common prefix through height 101.
        {
            bincoin::TestnetP2pServer common_server(node_a_directory, "127.0.0.1", 0);
            std::exception_ptr common_error;
            std::thread common_thread([&] {
                try {
                    common_server.serve_once();
                } catch (...) {
                    common_error = std::current_exception();
                }
            });
            const auto common_sync = bincoin::sync_from_peer(
                node_b_directory,
                bincoin::PeerEndpoint{.host = "127.0.0.1", .port = common_server.bound_port()});
            common_thread.join();
            if (common_error) std::rethrow_exception(common_error);
            require(common_sync.local_height_after == 101, "Common-prefix sync height mismatch");
        }

        std::string disconnected_payment_txid;
        std::string losing_tip;
        {
            bincoin::TestnetChain chain_a(node_a_directory);
            chain_a.load();
            wallet_a.load();
            wallet_b.load();

            const auto payment = wallet_a.create_payment(
                chain_a.utxo_set(),
                chain_a.tip().height + 1,
                bincoin::hex_to_bytes(wallet_b.public_key_hex()),
                125'000'000,
                bincoin::FeeRate(1000));
            disconnected_payment_txid = payment.txid();

            bincoin::TestnetMempool mempool_a(node_a_directory);
            mempool_a.load();
            mempool_a.add(payment, chain_a.utxo_set(), chain_a.tip().height + 1);
            chain_a.generate(1, wallet_a.locking_script(), mempool_a.transactions());
            mempool_a.clear();
            chain_a.generate(1, wallet_a.locking_script());
            require(chain_a.tip().height == 103, "Losing branch height mismatch");
            losing_tip = chain_a.tip().block.header.hash_hex();
        }
        {
            bincoin::TestnetChain chain_b(node_b_directory);
            chain_b.load();
            wallet_b.load();
            chain_b.generate(2, wallet_b.locking_script());
            require(chain_b.tip().height == 103, "Equal-work competing branch height mismatch");
        }

        // Equal accumulated work must not replace the active tip.
        {
            bincoin::TestnetP2pServer equal_server(node_b_directory, "127.0.0.1", 0);
            std::exception_ptr equal_error;
            std::thread equal_thread([&] {
                try {
                    equal_server.serve_once();
                } catch (...) {
                    equal_error = std::current_exception();
                }
            });
            const auto equal_sync = bincoin::sync_from_peer(
                node_a_directory,
                bincoin::PeerEndpoint{.host = "127.0.0.1", .port = equal_server.bound_port()});
            equal_thread.join();
            if (equal_error) std::rethrow_exception(equal_error);
            require(!equal_sync.activated_candidate, "Equal-work fork incorrectly replaced active chain");
            require(!equal_sync.reorganized, "Equal-work fork incorrectly reported reorganization");
            require(equal_sync.headers_received == 2, "Equal-work fork header count mismatch");
            require(equal_sync.blocks_received == 2, "Equal-work fork missing-block count mismatch");
            bincoin::SideBranchStore stored_equal_branch(node_a_directory);
            stored_equal_branch.load();
            require(stored_equal_branch.size() >= 2,
                    "Equal-work side branch was not persisted to durable storage");
            bincoin::TestnetChain unchanged(node_a_directory);
            unchanged.load();
            require(unchanged.tip().block.header.hash_hex() == losing_tip,
                    "Equal-work fork changed the active tip");
        }

        {
            bincoin::TestnetChain chain_b(node_b_directory);
            chain_b.load();
            wallet_b.load();
            chain_b.generate(1, wallet_b.locking_script());
            require(chain_b.tip().height == 104, "Winning branch height mismatch");
        }

        bincoin::SyncResult reorg_sync;
        {
            bincoin::TestnetP2pServer winning_server(node_b_directory, "127.0.0.1", 0);
            std::exception_ptr winning_error;
            std::thread winning_thread([&] {
                try {
                    winning_server.serve_once();
                } catch (...) {
                    winning_error = std::current_exception();
                }
            });
            reorg_sync = bincoin::sync_from_peer(
                node_a_directory,
                bincoin::PeerEndpoint{.host = "127.0.0.1", .port = winning_server.bound_port()});
            winning_thread.join();
            if (winning_error) std::rethrow_exception(winning_error);
        }

        require(reorg_sync.activated_candidate, "Greater-work candidate was not activated");
        require(reorg_sync.reorganized, "Fork switch was not reported as a reorganization");
        require(reorg_sync.fork_height == 101, "Unexpected reorganization fork height");
        require(reorg_sync.disconnected_blocks == 2, "Unexpected disconnected block count");
        require(reorg_sync.connected_blocks == 3, "Unexpected connected block count");
        require(reorg_sync.headers_received == 3, "Winning branch header count mismatch");
        require(reorg_sync.blocks_reused_from_side_store >= 2,
                "Winning branch did not reuse persisted side-branch blocks");
        require(reorg_sync.blocks_received <= 1,
                "Winning branch downloaded blocks already available in side storage");
        require(reorg_sync.resurrected_transactions == 1,
                "Disconnected valid transaction was not returned to mempool");
        require(reorg_sync.local_height_after == 104, "Reorganized chain height mismatch");
        require(reorg_sync.local_chainwork_after == reorg_sync.peer.chainwork,
                "Reorganized chainwork does not match winning peer");

        {
            bincoin::TestnetChain reorganized(node_a_directory);
            reorganized.load();
            require(reorganized.tip().block.header.hash_hex() != losing_tip,
                    "Losing fork tip remained active");
            require(!reorganized.confirmed_txids().contains(disconnected_payment_txid),
                    "Disconnected payment remained confirmed");

            bincoin::TestnetMempool mempool_a(node_a_directory);
            mempool_a.load();
            require(mempool_a.transactions().size() == 1,
                    "Reorganization mempool did not contain resurrected transaction");
            require(mempool_a.transactions().front().txid() == disconnected_payment_txid,
                    "Wrong transaction resurrected after reorganization");

            bincoin::SideBranchStore durable_branches(node_a_directory);
            durable_branches.load();
            require(durable_branches.size() >= 2,
                    "Disconnected active blocks were not retained as side-branch data");
            require(durable_branches.find(losing_tip).has_value(),
                    "Disconnected losing tip is missing from side-branch storage");

            wallet_a.load();
            reorganized.generate(1, wallet_a.locking_script(), mempool_a.transactions());
            mempool_a.clear();
            require(reorganized.confirmed_txids().contains(disconnected_payment_txid),
                    "Resurrected transaction could not be mined on winning chain");
            reorganized.verify();
        }
        std::filesystem::remove_all(reorg_root);

        // v0.6 persistent sessions: three nodes synchronize, relay a live
        // transaction, relay its confirming block, then reconnect after the
        // hub disappears and comes back with another block.
        const auto live_root = std::filesystem::temp_directory_path() /
            "binarycoin-v070-live-relay-test";
        std::filesystem::remove_all(live_root);
        const auto live_a_directory = live_root / "node-a";
        const auto live_b_directory = live_root / "node-b";
        const auto live_c_directory = live_root / "node-c";

        bincoin::TestnetWallet live_wallet_a(live_a_directory);
        bincoin::TestnetWallet live_wallet_b(live_b_directory);
        bincoin::TestnetWallet live_wallet_c(live_c_directory);
        {
            bincoin::TestnetChain chain_a(live_a_directory);
            bincoin::TestnetChain chain_b(live_b_directory);
            bincoin::TestnetChain chain_c(live_c_directory);
            chain_a.initialize();
            chain_b.initialize();
            chain_c.initialize();
            (void)live_wallet_a.create();
            (void)live_wallet_b.create();
            (void)live_wallet_c.create();
            chain_a.generate(101, live_wallet_a.locking_script());
        }

        auto live_node_a = std::make_unique<bincoin::PersistentTestnetNode>(
            live_a_directory, "127.0.0.1", 0, std::vector<bincoin::PeerEndpoint>{},
            local_test_policy());
        const std::uint16_t live_a_port = live_node_a->bound_port();
        live_node_a->start();

        // A malformed connection must be isolated without stopping the node.
        {
            bincoin::initialize_socket_runtime();
            const bincoin::Socket bad_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            require(bad_socket != bincoin::INVALID_SOCKET_VALUE,
                    "Unable to create malformed-peer test socket");
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(live_a_port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            require(::connect(
                bad_socket,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0,
                "Unable to connect malformed-peer test socket");
            const std::array<unsigned char, 24> malformed{};
            (void)bincoin::socket_send(bad_socket, malformed.data(), malformed.size());
            bincoin::close_socket(bad_socket);
        }

        bincoin::PersistentTestnetNode live_node_b(
            live_b_directory,
            "127.0.0.1",
            0,
            {bincoin::PeerEndpoint{.host = "127.0.0.1", .port = live_a_port}},
            local_test_policy());
        bincoin::PersistentTestnetNode live_node_c(
            live_c_directory,
            "127.0.0.1",
            0,
            {bincoin::PeerEndpoint{.host = "127.0.0.1", .port = live_a_port}},
            local_test_policy());
        live_node_b.start();
        live_node_c.start();

        wait_for_condition([&] {
            bincoin::TestnetChain chain_b(live_b_directory);
            bincoin::TestnetChain chain_c(live_c_directory);
            chain_b.load();
            chain_c.load();
            return chain_b.tip().height == 101 && chain_c.tip().height == 101;
        }, std::chrono::seconds(10), "Persistent peers did not complete initial synchronization");

        std::string live_payment_txid;
        {
            bincoin::TestnetChain chain_a(live_a_directory);
            chain_a.load();
            live_wallet_a.load();
            live_wallet_b.load();
            const auto payment = live_wallet_a.create_payment(
                chain_a.utxo_set(),
                chain_a.tip().height + 1,
                bincoin::hex_to_bytes(live_wallet_b.public_key_hex()),
                125'000'000,
                bincoin::FeeRate(1000));
            live_payment_txid = payment.txid();
            bincoin::TestnetMempool mempool_a(live_a_directory);
            mempool_a.load();
            mempool_a.add(payment, chain_a.utxo_set(), chain_a.tip().height + 1);
        }

        wait_for_condition([&] {
            bincoin::TestnetMempool mempool_b(live_b_directory);
            bincoin::TestnetMempool mempool_c(live_c_directory);
            mempool_b.load();
            mempool_c.load();
            return mempool_b.transactions().size() == 1 &&
                   mempool_c.transactions().size() == 1 &&
                   mempool_b.transactions().front().txid() == live_payment_txid &&
                   mempool_c.transactions().front().txid() == live_payment_txid;
        }, std::chrono::seconds(10), "Live transaction did not propagate to both peers");

        {
            bincoin::TestnetChain chain_a(live_a_directory);
            chain_a.load();
            live_wallet_a.load();
            bincoin::TestnetMempool mempool_a(live_a_directory);
            mempool_a.load();
            chain_a.generate(1, live_wallet_a.locking_script(), mempool_a.transactions());
            mempool_a.clear();
        }

        wait_for_condition([&] {
            bincoin::TestnetChain chain_b(live_b_directory);
            bincoin::TestnetChain chain_c(live_c_directory);
            chain_b.load();
            chain_c.load();
            bincoin::TestnetMempool mempool_b(live_b_directory);
            bincoin::TestnetMempool mempool_c(live_c_directory);
            mempool_b.load();
            mempool_c.load();
            live_wallet_b.load();
            const auto balance_b = live_wallet_b.balance(
                chain_b.utxo_set(), chain_b.tip().height + 1);
            return chain_b.tip().height == 102 && chain_c.tip().height == 102 &&
                   mempool_b.transactions().empty() && mempool_c.transactions().empty() &&
                   balance_b.spendable == 125'000'000;
        }, std::chrono::seconds(10), "Confirming block did not propagate or update recipient balance");

        live_node_a->request_stop();
        live_node_a->wait();
        live_node_a.reset();

        {
            bincoin::TestnetChain chain_a(live_a_directory);
            chain_a.load();
            live_wallet_a.load();
            chain_a.generate(1, live_wallet_a.locking_script());
            require(chain_a.tip().height == 103, "Offline hub block height mismatch");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        live_node_a = std::make_unique<bincoin::PersistentTestnetNode>(
            live_a_directory, "127.0.0.1", live_a_port,
            std::vector<bincoin::PeerEndpoint>{}, local_test_policy());
        live_node_a->start();

        wait_for_condition([&] {
            bincoin::TestnetChain chain_b(live_b_directory);
            bincoin::TestnetChain chain_c(live_c_directory);
            chain_b.load();
            chain_c.load();
            return chain_b.tip().height == 103 && chain_c.tip().height == 103;
        }, std::chrono::seconds(20), "Outbound peers did not reconnect and receive the new block");

        // v0.7 may reconnect in either direction after addrv2 exchange. The
        // important invariant is that peers.dat contains a usable address and
        // a node can restart with no manual peer arguments.
        bincoin::PeerStore stored_b(live_b_directory);
        stored_b.load();
        require(std::any_of(stored_b.records().begin(), stored_b.records().end(), [&](const auto& record) {
            return record.endpoint.host == "127.0.0.1" && record.endpoint.port == live_a_port;
        }), "Node B did not persist Node A in peers.dat");

        const std::uint16_t live_b_port = live_node_b.bound_port();
        live_node_b.request_stop();
        live_node_b.wait();

        bincoin::PersistentTestnetNode rediscovered_b(
            live_b_directory, "127.0.0.1", live_b_port,
            std::vector<bincoin::PeerEndpoint>{}, local_test_policy());
        rediscovered_b.start();
        {
            bincoin::TestnetChain chain_a(live_a_directory);
            chain_a.load();
            live_wallet_a.load();
            chain_a.generate(1, live_wallet_a.locking_script());
            require(chain_a.tip().height == 104, "Peer-store restart source height mismatch");
        }
        wait_for_condition([&] {
            bincoin::TestnetChain chain_b(live_b_directory);
            chain_b.load();
            return chain_b.tip().height == 104;
        }, std::chrono::seconds(12), "Node B did not reconnect from peers.dat without manual peers");
        require(rediscovered_b.stats().outbound_connections >= 1,
                "Node B did not automatically select a saved outbound peer");

        rediscovered_b.request_stop();
        live_node_c.request_stop();
        live_node_a->request_stop();
        rediscovered_b.wait();
        live_node_c.wait();
        live_node_a->wait();
        live_node_a.reset();
        std::filesystem::remove_all(live_root);

        std::cout << "All BinaryCoin Testnet Alpha v0.1.4 consensus, storage, HD wallet, peer, headers-first, live-relay and reorganization tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
