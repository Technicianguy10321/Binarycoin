#include "merkle.hpp"

#include <stdexcept>

namespace bincoin {

Hash256 merkle_root_raw(const std::vector<Transaction>& transactions) {
    if (transactions.empty()) throw std::invalid_argument("Cannot build a Merkle root from zero transactions");

    std::vector<Hash256> level;
    level.reserve(transactions.size());
    for (const Transaction& transaction : transactions) level.push_back(transaction.raw_hash());

    while (level.size() > 1) {
        if (level.size() % 2 != 0) level.push_back(level.back());

        std::vector<Hash256> next;
        next.reserve(level.size() / 2);
        for (std::size_t index = 0; index < level.size(); index += 2) {
            std::vector<std::uint8_t> joined;
            joined.reserve(64);
            joined.insert(joined.end(), level[index].begin(), level[index].end());
            joined.insert(joined.end(), level[index + 1].begin(), level[index + 1].end());
            next.push_back(sha256d(joined));
        }
        level = std::move(next);
    }
    return level.front();
}

std::string merkle_root_hex(const std::vector<Transaction>& transactions) {
    return hash_to_display_hex(merkle_root_raw(transactions));
}

} // namespace bincoin
