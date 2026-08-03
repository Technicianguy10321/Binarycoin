#pragma once

#include "block.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bincoin {

class SideBranchStore {
public:
    explicit SideBranchStore(std::filesystem::path data_directory);

    void load();
    void save() const;
    void put(const StoredBlock& block);
    void put_many(const std::vector<StoredBlock>& blocks);
    void remove_active(const std::vector<StoredBlock>& active_blocks);
    [[nodiscard]] std::optional<StoredBlock> find(const std::string& hash) const;
    [[nodiscard]] std::vector<StoredBlock> all() const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path data_directory_;
    std::filesystem::path path_;
    std::vector<StoredBlock> blocks_;
};

} // namespace bincoin
