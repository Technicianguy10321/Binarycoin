#pragma once

#include "hash.hpp"
#include "transaction.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bincoin {

struct BlockHeader {
    std::int32_t version{1};
    std::string previous_hash;
    std::string merkle_root;
    std::uint32_t time{0};
    std::uint32_t bits{0};
    std::uint32_t nonce{0};

    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
    [[nodiscard]] Hash256 raw_hash() const;
    [[nodiscard]] std::string hash_hex() const;
    [[nodiscard]] static BlockHeader deserialize(std::span<const std::uint8_t> bytes);
};

struct Block {
    BlockHeader header;
    std::vector<Transaction> transactions;
};

struct StoredBlock {
    std::uint64_t height{0};
    Block block;
};

Block testnet_genesis_block();
BlockHeader testnet_genesis_header();
Block mine_testnet_block(
    std::uint64_t height,
    const std::string& previous_hash,
    std::uint32_t previous_time,
    std::uint32_t bits,
    std::span<const std::uint8_t> coinbase_script_pubkey,
    const std::vector<Transaction>& transactions,
    Amount transaction_fees
);

inline constexpr std::uint32_t TESTNET_GENESIS_BITS = 0x1f00ffffU;
#ifdef BINARYCOIN_FAST_TESTS
inline constexpr std::uint32_t TESTNET_BITS = 0x207fffffU;
#else
inline constexpr std::uint32_t TESTNET_BITS = 0x1f00ffffU;
#endif
inline constexpr const char* TESTNET_GENESIS_HASH =
    "00007e7bc2b6d593d9bacb54cd32bacaf9a99ff86adc9ac6fbe36134fff1c372";
inline constexpr const char* TESTNET_GENESIS_MERKLE =
    "2cf10845810fce4b828950111a45d66e33d8649db1265f088cf4607789ae1442";
inline constexpr const char* TESTNET_GENESIS_TXID =
    "2cf10845810fce4b828950111a45d66e33d8649db1265f088cf4607789ae1442";

} // namespace bincoin
