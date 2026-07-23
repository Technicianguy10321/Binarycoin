#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bincoin {

struct BanRecord {
    std::string host;
    std::int64_t banned_until{0};
    std::uint32_t score{0};
    std::string reason;
};

class BanStore {
public:
    explicit BanStore(std::filesystem::path data_directory);

    void load();
    void save() const;
    void prune_expired(std::int64_t now = 0);
    void ban(const std::string& host, std::int64_t duration_seconds,
             std::uint32_t score, std::string reason);
    void clear();

    [[nodiscard]] bool is_banned(const std::string& host, std::int64_t now = 0) const;
    [[nodiscard]] const std::vector<BanRecord>& records() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path data_directory_;
    std::filesystem::path path_;
    std::vector<BanRecord> records_;
};

} // namespace bincoin
