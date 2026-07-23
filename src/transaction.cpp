#include "transaction.hpp"

#include "serialize.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace bincoin {
namespace {

constexpr std::size_t MAX_TRANSACTION_ITEMS = 100'000;
constexpr std::size_t MAX_SCRIPT_SIZE = 10'000;

std::size_t checked_size(const std::uint64_t value, const char* field) {
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds platform size");
    }
    return static_cast<std::size_t>(value);
}

} // namespace

std::vector<std::uint8_t> Transaction::serialize() const {
    validate_transaction_structure(*this);

    std::vector<std::uint8_t> output;
    append_i32_le(output, version);
    append_compact_size(output, inputs.size());

    for (const TxInput& input : inputs) {
        const auto previous_wire = display_hex_to_wire_bytes(input.previous_output.txid);
        output.insert(output.end(), previous_wire.begin(), previous_wire.end());
        append_u32_le(output, input.previous_output.index);
        append_compact_size(output, input.script_sig.size());
        output.insert(output.end(), input.script_sig.begin(), input.script_sig.end());
        append_u32_le(output, input.sequence);
    }

    append_compact_size(output, outputs.size());
    for (const TxOutput& tx_output : outputs) {
        append_i64_le(output, tx_output.value);
        append_compact_size(output, tx_output.script_pubkey.size());
        output.insert(output.end(), tx_output.script_pubkey.begin(), tx_output.script_pubkey.end());
    }

    append_u32_le(output, lock_time);
    return output;
}

Hash256 Transaction::raw_hash() const {
    const auto bytes = serialize();
    return sha256d(bytes);
}

std::string Transaction::txid() const {
    return hash_to_display_hex(raw_hash());
}

std::size_t Transaction::virtual_size() const {
    // BinaryCoin currently has no witness serialization, so vsize equals byte size.
    return serialize().size();
}

bool Transaction::is_coinbase() const {
    return inputs.size() == 1 &&
           inputs.front().previous_output.txid == std::string(64, '0') &&
           inputs.front().previous_output.index == 0xffffffffU;
}

Transaction Transaction::deserialize(const std::span<const std::uint8_t> bytes) {
    std::size_t offset = 0;
    Transaction transaction;
    transaction.version = read_i32_le(bytes, offset);

    const std::size_t input_count = checked_size(read_compact_size(bytes, offset), "Input count");
    if (input_count > MAX_TRANSACTION_ITEMS) throw std::runtime_error("Too many transaction inputs");
    transaction.inputs.reserve(input_count);

    for (std::size_t index = 0; index < input_count; ++index) {
        const auto previous_wire_vector = read_bytes(bytes, offset, 32);
        Hash256 previous_wire{};
        std::copy(previous_wire_vector.begin(), previous_wire_vector.end(), previous_wire.begin());

        TxInput input;
        input.previous_output.txid = wire_bytes_to_display_hex(previous_wire);
        input.previous_output.index = read_u32_le(bytes, offset);
        const std::size_t script_size = checked_size(read_compact_size(bytes, offset), "Input script size");
        if (script_size > MAX_SCRIPT_SIZE) throw std::runtime_error("Input script exceeds size limit");
        input.script_sig = read_bytes(bytes, offset, script_size);
        input.sequence = read_u32_le(bytes, offset);
        transaction.inputs.push_back(std::move(input));
    }

    const std::size_t output_count = checked_size(read_compact_size(bytes, offset), "Output count");
    if (output_count > MAX_TRANSACTION_ITEMS) throw std::runtime_error("Too many transaction outputs");
    transaction.outputs.reserve(output_count);

    for (std::size_t index = 0; index < output_count; ++index) {
        TxOutput tx_output;
        tx_output.value = read_i64_le(bytes, offset);
        const std::size_t script_size = checked_size(read_compact_size(bytes, offset), "Output script size");
        if (script_size > MAX_SCRIPT_SIZE) throw std::runtime_error("Output script exceeds size limit");
        tx_output.script_pubkey = read_bytes(bytes, offset, script_size);
        transaction.outputs.push_back(std::move(tx_output));
    }

    transaction.lock_time = read_u32_le(bytes, offset);
    if (offset != bytes.size()) throw std::runtime_error("Trailing data after transaction");
    validate_transaction_structure(transaction);
    return transaction;
}

std::vector<std::uint8_t> encode_script_number(const std::int64_t value) {
    if (value == 0) return {};

    const bool negative = value < 0;
    std::uint64_t absolute = negative
        ? static_cast<std::uint64_t>(-(value + 1)) + 1U
        : static_cast<std::uint64_t>(value);

    std::vector<std::uint8_t> result;
    while (absolute != 0) {
        result.push_back(static_cast<std::uint8_t>(absolute & 0xffU));
        absolute >>= 8U;
    }

    if ((result.back() & 0x80U) != 0) {
        result.push_back(negative ? 0x80U : 0x00U);
    } else if (negative) {
        result.back() |= 0x80U;
    }
    return result;
}

std::vector<std::uint8_t> push_script_data(const std::span<const std::uint8_t> data) {
    if (data.size() > 75) {
        throw std::invalid_argument("This milestone supports direct script pushes of at most 75 bytes");
    }
    std::vector<std::uint8_t> output;
    output.reserve(data.size() + 1);
    output.push_back(static_cast<std::uint8_t>(data.size()));
    output.insert(output.end(), data.begin(), data.end());
    return output;
}

Transaction make_testnet_coinbase(
    const std::uint64_t height,
    const Amount reward,
    const std::span<const std::uint8_t> script_pubkey
) {
    if (!money_range(reward)) throw std::invalid_argument("Coinbase reward outside money range");
    if (script_pubkey.empty() || script_pubkey.size() > MAX_SCRIPT_SIZE) {
        throw std::invalid_argument("Coinbase locking script has invalid size");
    }

    const auto height_number = encode_script_number(static_cast<std::int64_t>(height));
    const auto pushed_height = push_script_data(height_number);
    static constexpr char marker[] = "July 22 2026, BinaryCoin testnet alpha begins.";
    const auto* marker_begin = reinterpret_cast<const std::uint8_t*>(marker);
    const std::span<const std::uint8_t> marker_span(marker_begin, sizeof(marker) - 1);
    const auto pushed_marker = push_script_data(marker_span);

    TxInput input;
    input.previous_output.txid = std::string(64, '0');
    input.previous_output.index = 0xffffffffU;
    input.script_sig.reserve(pushed_height.size() + pushed_marker.size());
    input.script_sig.insert(input.script_sig.end(), pushed_height.begin(), pushed_height.end());
    input.script_sig.insert(input.script_sig.end(), pushed_marker.begin(), pushed_marker.end());
    input.sequence = 0xffffffffU;

    TxOutput output;
    output.value = reward;
    output.script_pubkey.assign(script_pubkey.begin(), script_pubkey.end());

    Transaction transaction;
    transaction.version = 1;
    transaction.inputs = {std::move(input)};
    transaction.outputs = {std::move(output)};
    transaction.lock_time = 0;
    return transaction;
}

void validate_transaction_structure(const Transaction& transaction) {
    if (transaction.inputs.empty()) throw std::runtime_error("Transaction has no inputs");
    if (transaction.outputs.empty()) throw std::runtime_error("Transaction has no outputs");
    if (transaction.inputs.size() > MAX_TRANSACTION_ITEMS || transaction.outputs.size() > MAX_TRANSACTION_ITEMS) {
        throw std::runtime_error("Transaction item count exceeds limit");
    }

    Amount total_output = 0;
    for (const TxInput& input : transaction.inputs) {
        if (input.script_sig.size() > MAX_SCRIPT_SIZE) throw std::runtime_error("Input script exceeds size limit");
        (void)display_hex_to_wire_bytes(input.previous_output.txid);
    }
    for (const TxOutput& output : transaction.outputs) {
        if (!money_range(output.value)) throw std::runtime_error("Transaction output outside money range");
        if (output.script_pubkey.size() > MAX_SCRIPT_SIZE) throw std::runtime_error("Output script exceeds size limit");
        if (total_output > MAX_MONEY - output.value) throw std::runtime_error("Transaction output total overflow");
        total_output += output.value;
    }
}

} // namespace bincoin
