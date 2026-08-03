#pragma once

#include "amount.hpp"
#include "block.hpp"
#include "utxo.hpp"
#include "storage.hpp"

#include <boost/multiprecision/cpp_int.hpp>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace bincoin {

std::uint32_t expected_testnet_bits(
    std::span<const StoredBlock> previous_blocks,
    std::uint64_t next_height
);

struct ChainActivationResult {
    bool activated{false};
    std::uint64_t old_height{0};
    std::uint64_t new_height{0};
    std::uint64_t fork_height{0};
    std::size_t disconnected_blocks{0};
    std::size_t connected_blocks{0};
    boost::multiprecision::cpp_int old_chainwork{0};
    boost::multiprecision::cpp_int candidate_chainwork{0};
    std::vector<Transaction> disconnected_transactions;
};

class TestnetChain {
public:
    explicit TestnetChain(std::filesystem::path data_directory);

    void initialize();
    void load();
    void save();
    void verify() const;
    void reindex();
    void append_blocks(const std::vector<StoredBlock>& incoming);
    [[nodiscard]] ChainActivationResult activate_if_more_work(
        const std::vector<StoredBlock>& candidate
    );
    void generate(
        std::uint64_t count,
        std::span<const std::uint8_t> coinbase_script_pubkey,
        const std::vector<Transaction>& pending_transactions = {}
    );

    [[nodiscard]] const std::vector<StoredBlock>& blocks() const noexcept;
    [[nodiscard]] const StoredBlock& tip() const;
    [[nodiscard]] const std::filesystem::path& data_directory() const noexcept;
    [[nodiscard]] Amount issued_supply() const;
    [[nodiscard]] UtxoSet utxo_set() const;
    [[nodiscard]] std::set<std::string> confirmed_txids() const;
    [[nodiscard]] boost::multiprecision::cpp_int chainwork() const;
    [[nodiscard]] std::string chainwork_hex() const;
    [[nodiscard]] ChainStorageStats storage_stats() const;

    [[nodiscard]] static boost::multiprecision::cpp_int calculate_chainwork(
        const std::vector<StoredBlock>& blocks
    );
    static void verify_blocks(const std::vector<StoredBlock>& blocks);

private:
    std::filesystem::path data_directory_;
    std::filesystem::path legacy_v3_path_;
    std::filesystem::path legacy_v2_path_;
    std::filesystem::path legacy_v1_path_;
    DurableChainStore storage_;
    std::vector<StoredBlock> blocks_;
    UtxoSet utxos_;

    void load_legacy_v3();
};

} // namespace bincoin
