#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bincoin {

struct PeerEndpoint {
    std::string host;
    std::uint16_t port{26001};

    [[nodiscard]] bool operator==(const PeerEndpoint&) const = default;
};

struct PeerRecord {
    PeerEndpoint endpoint;
    std::uint64_t services{1};
    std::int64_t last_seen{0};
    std::int64_t last_success{0};
    std::uint32_t failures{0};
    bool manual{false};
};

class PeerStore {
public:
    explicit PeerStore(std::filesystem::path data_directory);

    void load();
    void save() const;
    void add_or_update(const PeerRecord& record);
    bool add_if_missing(const PeerRecord& record);
    void mark_success(const PeerEndpoint& endpoint, std::uint64_t services = 1);
    void mark_failure(const PeerEndpoint& endpoint);

    [[nodiscard]] const std::vector<PeerRecord>& records() const noexcept;
    [[nodiscard]] std::vector<PeerEndpoint> select_outbound(
        std::size_t maximum,
        const std::vector<PeerEndpoint>& excluded = {}
    ) const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path data_directory_;
    std::filesystem::path path_;
    std::vector<PeerRecord> records_;
};

} // namespace bincoin
