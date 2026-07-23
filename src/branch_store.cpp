#include "branch_store.hpp"

#include "platform.hpp"
#include "net.hpp"
#include "serialize.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace bincoin {
namespace {
constexpr const char* FILE_MAGIC = "BINARYCOIN_SIDE_BRANCH_V1";
}

SideBranchStore::SideBranchStore(std::filesystem::path data_directory)
    : data_directory_(std::move(data_directory)), path_(data_directory_ / "sidebranches-v1.dat") {}

void SideBranchStore::load() {
    blocks_.clear();
    if (!std::filesystem::exists(path_)) return;
    std::ifstream input(path_);
    if (!input) throw std::runtime_error("Unable to open side-branch store");
    std::string header;
    if (!std::getline(input, header) || header != FILE_MAGIC) {
        throw std::runtime_error("Invalid side-branch store header");
    }
    std::string hex;
    while (std::getline(input, hex)) {
        if (hex.empty()) continue;
        const auto decoded = deserialize_stored_blocks(hex_to_bytes(hex));
        if (decoded.size() != 1) throw std::runtime_error("Invalid side-branch record");
        put(decoded.front());
    }
}

void SideBranchStore::save() const {
    std::filesystem::create_directories(data_directory_);
    const std::filesystem::path temporary = path_.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("Unable to create side-branch store");
    output << FILE_MAGIC << '\n';
    for (const StoredBlock& block : blocks_) {
        output << bytes_to_hex(serialize_stored_blocks({block}, 0, 1)) << '\n';
    }
    output.flush();
    if (!output) throw std::runtime_error("Unable to flush side-branch store");
    replace_file_atomically(temporary, path_);
}

void SideBranchStore::put(const StoredBlock& block) {
    const std::string hash = block.block.header.hash_hex();
    const auto iterator = std::find_if(blocks_.begin(), blocks_.end(), [&](const StoredBlock& stored) {
        return stored.block.header.hash_hex() == hash;
    });
    if (iterator == blocks_.end()) blocks_.push_back(block);
}

void SideBranchStore::put_many(const std::vector<StoredBlock>& blocks) {
    for (const StoredBlock& block : blocks) put(block);
    save();
}

void SideBranchStore::remove_active(const std::vector<StoredBlock>& active_blocks) {
    std::vector<std::string> active_hashes;
    active_hashes.reserve(active_blocks.size());
    for (const StoredBlock& block : active_blocks) {
        active_hashes.push_back(block.block.header.hash_hex());
    }
    blocks_.erase(std::remove_if(blocks_.begin(), blocks_.end(), [&](const StoredBlock& stored) {
        const std::string hash = stored.block.header.hash_hex();
        return std::find(active_hashes.begin(), active_hashes.end(), hash) != active_hashes.end();
    }), blocks_.end());
    save();
}

std::optional<StoredBlock> SideBranchStore::find(const std::string& hash) const {
    const auto iterator = std::find_if(blocks_.begin(), blocks_.end(), [&](const StoredBlock& stored) {
        return stored.block.header.hash_hex() == hash;
    });
    if (iterator == blocks_.end()) return std::nullopt;
    return *iterator;
}

std::vector<StoredBlock> SideBranchStore::all() const { return blocks_; }
std::size_t SideBranchStore::size() const noexcept { return blocks_.size(); }
const std::filesystem::path& SideBranchStore::path() const noexcept { return path_; }

} // namespace bincoin
