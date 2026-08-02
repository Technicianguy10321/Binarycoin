#pragma once

#include "amount.hpp"
#include "block.hpp"
#include "transaction.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace bincoin {

struct OutPointLess {
    bool operator()(const OutPoint& left, const OutPoint& right) const noexcept;
};

struct Coin {
    TxOutput output;
    std::uint64_t height{0};
    bool coinbase{false};
};

using UtxoSet = std::map<OutPoint, Coin, OutPointLess>;

[[nodiscard]] Amount apply_transaction(
    const Transaction& transaction,
    std::uint64_t spend_height,
    UtxoSet& utxos
);
[[nodiscard]] UtxoSet build_utxo_set(const std::vector<StoredBlock>& blocks);
[[nodiscard]] Amount sum_outputs(const Transaction& transaction);

} // namespace bincoin
