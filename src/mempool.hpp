#pragma once

#include "transaction.hpp"
#include "utxo.hpp"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace bincoin {

class TestnetMempool {
public:
    explicit TestnetMempool(std::filesystem::path data_directory);

    void load();
    void save() const;
    void clear();
    void add(const Transaction& transaction, const UtxoSet& chain_utxos, std::uint64_t spend_height);
    void revalidate(
        const UtxoSet& chain_utxos,
        std::uint64_t spend_height,
        const std::set<std::string>& confirmed_txids
    );
    [[nodiscard]] UtxoSet effective_utxos(const UtxoSet& chain_utxos, std::uint64_t spend_height) const;
    [[nodiscard]] Amount total_fees(const UtxoSet& chain_utxos, std::uint64_t spend_height) const;
    [[nodiscard]] const std::vector<Transaction>& transactions() const noexcept;

private:
    std::filesystem::path path_;
    std::vector<Transaction> transactions_;
};

} // namespace bincoin
