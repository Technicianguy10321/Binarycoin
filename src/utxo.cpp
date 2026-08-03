#include "utxo.hpp"

#include "script.hpp"

#include <set>
#include <stdexcept>

namespace bincoin {

bool OutPointLess::operator()(const OutPoint& left, const OutPoint& right) const noexcept {
    if (left.txid != right.txid) return left.txid < right.txid;
    return left.index < right.index;
}

Amount sum_outputs(const Transaction& transaction) {
    Amount total = 0;
    for (const TxOutput& output : transaction.outputs) {
        if (!money_range(output.value) || total > MAX_MONEY - output.value) {
            throw std::runtime_error("Transaction output total outside money range");
        }
        total += output.value;
    }
    return total;
}

Amount apply_transaction(
    const Transaction& transaction,
    const std::uint64_t spend_height,
    UtxoSet& utxos
) {
    validate_transaction_structure(transaction);
    if (transaction.is_coinbase()) throw std::runtime_error("Coinbase cannot be applied as a regular transaction");

    std::set<OutPoint, OutPointLess> seen_inputs;
    Amount input_total = 0;
    for (std::size_t index = 0; index < transaction.inputs.size(); ++index) {
        const OutPoint& outpoint = transaction.inputs[index].previous_output;
        if (!seen_inputs.insert(outpoint).second) throw std::runtime_error("Duplicate transaction input");
        const auto coin_it = utxos.find(outpoint);
        if (coin_it == utxos.end()) throw std::runtime_error("Missing or already spent transaction input");
        const Coin& coin = coin_it->second;
        if (coin.coinbase && spend_height < coin.height + COINBASE_MATURITY) {
            throw std::runtime_error("Attempt to spend immature coinbase output");
        }
        if (!verify_p2pk_input(transaction, index, coin.output.script_pubkey)) {
            throw std::runtime_error("Transaction signature validation failed");
        }
        if (input_total > MAX_MONEY - coin.output.value) throw std::runtime_error("Transaction input total overflow");
        input_total += coin.output.value;
    }

    const Amount output_total = sum_outputs(transaction);
    if (input_total < output_total) throw std::runtime_error("Transaction spends more than its inputs");
    const Amount fee = input_total - output_total;

    for (const TxInput& input : transaction.inputs) utxos.erase(input.previous_output);
    const std::string transaction_id = transaction.txid();
    for (std::size_t index = 0; index < transaction.outputs.size(); ++index) {
        OutPoint outpoint{.txid = transaction_id, .index = static_cast<std::uint32_t>(index)};
        if (utxos.contains(outpoint)) throw std::runtime_error("Duplicate transaction output identifier");
        utxos.emplace(std::move(outpoint), Coin{
            .output = transaction.outputs[index],
            .height = spend_height,
            .coinbase = false,
        });
    }
    return fee;
}

UtxoSet build_utxo_set(const std::vector<StoredBlock>& blocks) {
    UtxoSet utxos;
    for (std::size_t block_index = 1; block_index < blocks.size(); ++block_index) {
        const StoredBlock& stored = blocks[block_index];
        if (stored.block.transactions.empty() || !stored.block.transactions.front().is_coinbase()) {
            throw std::runtime_error("Block is missing coinbase transaction");
        }
        for (std::size_t tx_index = 1; tx_index < stored.block.transactions.size(); ++tx_index) {
            (void)apply_transaction(stored.block.transactions[tx_index], stored.height, utxos);
        }
        const Transaction& coinbase = stored.block.transactions.front();
        const std::string txid = coinbase.txid();
        for (std::size_t output_index = 0; output_index < coinbase.outputs.size(); ++output_index) {
            OutPoint outpoint{.txid = txid, .index = static_cast<std::uint32_t>(output_index)};
            if (utxos.contains(outpoint)) throw std::runtime_error("Duplicate coinbase output identifier");
            utxos.emplace(std::move(outpoint), Coin{
                .output = coinbase.outputs[output_index],
                .height = stored.height,
                .coinbase = true,
            });
        }
    }
    return utxos;
}

} // namespace bincoin
