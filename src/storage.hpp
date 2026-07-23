#pragma once

#include "block.hpp"
#include "utxo.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace bincoin {

struct ChainStorageStats {
    std::uint64_t generation{0};
    std::uint64_t active_height{0};
    std::uint64_t block_records{0};
    std::uint64_t block_file_bytes{0};
    bool recovered{false};
    bool migrated_legacy{false};
};

class DurableChainStore {
public:
    explicit DurableChainStore(std::filesystem::path data_directory);

    [[nodiscard]] bool exists() const;
    [[nodiscard]] std::vector<StoredBlock> load(UtxoSet& utxos);
    void commit(const std::vector<StoredBlock>& blocks, const UtxoSet& utxos);
    [[nodiscard]] std::vector<StoredBlock> scan_all_blocks() const;
    [[nodiscard]] ChainStorageStats stats() const;
    void mark_migrated_legacy() noexcept;

    [[nodiscard]] const std::filesystem::path& blocks_path() const noexcept;
    [[nodiscard]] const std::filesystem::path& chainstate_directory() const noexcept;

private:
    std::filesystem::path data_directory_;
    std::filesystem::path blocks_directory_;
    std::filesystem::path chainstate_directory_;
    std::filesystem::path blocks_path_;
    std::filesystem::path manifest_path_;
    std::filesystem::path lock_path_;
    ChainStorageStats stats_;
};

[[nodiscard]] std::vector<std::uint8_t> serialize_stored_block(const StoredBlock& stored);
[[nodiscard]] StoredBlock deserialize_stored_block(std::span<const std::uint8_t> bytes);

} // namespace bincoin
