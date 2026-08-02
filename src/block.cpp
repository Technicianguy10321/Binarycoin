#include "block.hpp"

#include "amount.hpp"
#include "merkle.hpp"
#include "pow.hpp"
#include "serialize.hpp"
#include "script.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace bincoin {

std::vector<std::uint8_t> BlockHeader::serialize() const {
    std::vector<std::uint8_t> output;
    output.reserve(80);

    append_i32_le(output, version);
    const auto previous_wire = display_hex_to_wire_bytes(previous_hash);
    output.insert(output.end(), previous_wire.begin(), previous_wire.end());
    const auto merkle_wire = display_hex_to_wire_bytes(merkle_root);
    output.insert(output.end(), merkle_wire.begin(), merkle_wire.end());
    append_u32_le(output, time);
    append_u32_le(output, bits);
    append_u32_le(output, nonce);

    if (output.size() != 80) throw std::runtime_error("Serialized block header is not 80 bytes");
    return output;
}

Hash256 BlockHeader::raw_hash() const {
    return sha256d(serialize());
}

std::string BlockHeader::hash_hex() const {
    return hash_to_display_hex(raw_hash());
}

BlockHeader BlockHeader::deserialize(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != 80) throw std::runtime_error("Serialized block header must be exactly 80 bytes");
    std::size_t offset = 0;
    BlockHeader header;
    header.version = read_i32_le(bytes, offset);

    const auto previous_vector = read_bytes(bytes, offset, 32);
    std::array<std::uint8_t, 32> previous_wire{};
    std::copy(previous_vector.begin(), previous_vector.end(), previous_wire.begin());
    header.previous_hash = wire_bytes_to_display_hex(previous_wire);

    const auto merkle_vector = read_bytes(bytes, offset, 32);
    std::array<std::uint8_t, 32> merkle_wire{};
    std::copy(merkle_vector.begin(), merkle_vector.end(), merkle_wire.begin());
    header.merkle_root = wire_bytes_to_display_hex(merkle_wire);

    header.time = read_u32_le(bytes, offset);
    header.bits = read_u32_le(bytes, offset);
    header.nonce = read_u32_le(bytes, offset);
    if (offset != bytes.size()) throw std::runtime_error("Trailing block header bytes");
    return header;
}

Block testnet_genesis_block() {
    static constexpr std::array<std::uint8_t, 33> genesis_public_key{
        0x02, 0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac, 0x55, 0xa0,
        0x62, 0x95, 0xce, 0x87, 0x0b, 0x07, 0x02, 0x9b, 0xfc, 0xdb, 0x2d,
        0xce, 0x28, 0xd9, 0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98
    };

    Block block;
    block.transactions.push_back(make_testnet_coinbase(
        0, INITIAL_SUBSIDY, make_p2pk_script(genesis_public_key)));
    block.header = BlockHeader{
        .version = 1,
        .previous_hash = std::string(64, '0'),
        .merkle_root = TESTNET_GENESIS_MERKLE,
        .time = 1784699218U,
        .bits = TESTNET_GENESIS_BITS,
        .nonce = 180004U,
    };
    if (block.transactions.front().txid() != TESTNET_GENESIS_TXID ||
        merkle_root_hex(block.transactions) != TESTNET_GENESIS_MERKLE ||
        block.header.hash_hex() != TESTNET_GENESIS_HASH) {
        throw std::runtime_error("Locked BinaryCoin testnet genesis constants are inconsistent");
    }
    return block;
}

BlockHeader testnet_genesis_header() {
    return testnet_genesis_block().header;
}

Block mine_testnet_block(
    const std::uint64_t height,
    const std::string& previous_hash,
    const std::uint32_t previous_time,
    const std::uint32_t bits,
    const std::span<const std::uint8_t> coinbase_script_pubkey,
    const std::vector<Transaction>& transactions,
    const Amount transaction_fees
) {
    if (!money_range(transaction_fees)) throw std::invalid_argument("Transaction fees outside money range");
    const Amount subsidy = block_subsidy(height);
    if (subsidy > MAX_MONEY - transaction_fees) throw std::runtime_error("Coinbase reward overflow");

    Block block;
    block.transactions.push_back(make_testnet_coinbase(
        height, subsidy + transaction_fees, coinbase_script_pubkey));
    block.transactions.insert(block.transactions.end(), transactions.begin(), transactions.end());

    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const auto now_u32 = now < 0 ? 0U : static_cast<std::uint32_t>(now);
    block.header = BlockHeader{
        .version = 1,
        .previous_hash = previous_hash,
        .merkle_root = merkle_root_hex(block.transactions),
        .time = std::max(previous_time + 1U, now_u32),
        .bits = bits,
        .nonce = 0,
    };

    for (;;) {
        if (check_proof_of_work(block.header.raw_hash(), block.header.bits)) return block;
        if (block.header.nonce == std::numeric_limits<std::uint32_t>::max()) {
            if (block.header.time == std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("Exhausted time and nonce space while mining");
            }
            ++block.header.time;
            block.header.nonce = 0;
        } else {
            ++block.header.nonce;
        }
    }
}

} // namespace bincoin
