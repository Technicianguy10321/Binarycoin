#pragma once

#include "amount.hpp"
#include "hash.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bincoin {

struct OutPoint {
    std::string txid{std::string(64, '0')};
    std::uint32_t index{0xffffffffU};
};

struct TxInput {
    OutPoint previous_output;
    std::vector<std::uint8_t> script_sig;
    std::uint32_t sequence{0xffffffffU};
};

struct TxOutput {
    Amount value{0};
    std::vector<std::uint8_t> script_pubkey;
};

struct Transaction {
    std::int32_t version{1};
    std::vector<TxInput> inputs;
    std::vector<TxOutput> outputs;
    std::uint32_t lock_time{0};

    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
    [[nodiscard]] Hash256 raw_hash() const;
    [[nodiscard]] std::string txid() const;
    [[nodiscard]] std::size_t virtual_size() const;
    [[nodiscard]] bool is_coinbase() const;

    static Transaction deserialize(std::span<const std::uint8_t> bytes);
};

std::vector<std::uint8_t> encode_script_number(std::int64_t value);
std::vector<std::uint8_t> push_script_data(std::span<const std::uint8_t> data);
Transaction make_testnet_coinbase(
    std::uint64_t height,
    Amount reward,
    std::span<const std::uint8_t> script_pubkey
);
void validate_transaction_structure(const Transaction& transaction);

} // namespace bincoin
