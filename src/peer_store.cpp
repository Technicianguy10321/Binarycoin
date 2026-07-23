#include "peer_store.hpp"

#include "platform.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace bincoin {
namespace {

constexpr const char* FILE_MAGIC = "BINARYCOIN_PEERS_V1";

std::vector<std::string> split(const std::string& text, const char separator) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t end = text.find(separator, begin);
        fields.push_back(text.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return fields;
}

template <typename Integer>
Integer parse_number(const std::string& text, const char* field) {
    Integer value{};
    const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (error != std::errc{} || pointer != text.data() + text.size()) {
        throw std::runtime_error(std::string("Invalid peers.dat ") + field);
    }
    return value;
}

std::int64_t now_seconds() {
    return static_cast<std::int64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

bool valid_host(const std::string& host) {
    return !host.empty() && host.size() <= 255 && host.find('\t') == std::string::npos &&
           host.find('\n') == std::string::npos && host.find('\r') == std::string::npos;
}

bool endpoint_equal(const PeerEndpoint& left, const PeerEndpoint& right) {
    return left.host == right.host && left.port == right.port;
}

} // namespace

PeerStore::PeerStore(std::filesystem::path data_directory)
    : data_directory_(std::move(data_directory)), path_(data_directory_ / "peers.dat") {}

void PeerStore::load() {
    records_.clear();
    if (!std::filesystem::exists(path_)) return;
    std::ifstream input(path_);
    if (!input) throw std::runtime_error("Unable to open peers.dat");
    std::string line;
    if (!std::getline(input, line) || line != FILE_MAGIC) {
        throw std::runtime_error("Invalid peers.dat header");
    }
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split(line, '\t');
        if (fields.size() != 7) throw std::runtime_error("Malformed peers.dat record");
        PeerRecord record;
        record.endpoint.host = fields[0];
        record.endpoint.port = parse_number<std::uint16_t>(fields[1], "port");
        record.services = parse_number<std::uint64_t>(fields[2], "services");
        record.last_seen = parse_number<std::int64_t>(fields[3], "last_seen");
        record.last_success = parse_number<std::int64_t>(fields[4], "last_success");
        record.failures = parse_number<std::uint32_t>(fields[5], "failures");
        record.manual = fields[6] == "1";
        if (!valid_host(record.endpoint.host) || record.endpoint.port == 0) {
            throw std::runtime_error("Invalid peers.dat endpoint");
        }
        add_or_update(record);
    }
}

void PeerStore::save() const {
    std::filesystem::create_directories(data_directory_);
    const std::filesystem::path temporary = path_.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("Unable to create temporary peers.dat");
    output << FILE_MAGIC << '\n';
    for (const PeerRecord& record : records_) {
        output << record.endpoint.host << '\t' << record.endpoint.port << '\t'
               << record.services << '\t' << record.last_seen << '\t'
               << record.last_success << '\t' << record.failures << '\t'
               << (record.manual ? 1 : 0) << '\n';
    }
    output.flush();
    if (!output) throw std::runtime_error("Unable to flush peers.dat");
    replace_file_atomically(temporary, path_);
}

bool PeerStore::add_if_missing(const PeerRecord& incoming) {
    if (!valid_host(incoming.endpoint.host) || incoming.endpoint.port == 0) return false;
    const auto iterator = std::find_if(records_.begin(), records_.end(), [&](const PeerRecord& record) {
        return endpoint_equal(record.endpoint, incoming.endpoint);
    });
    if (iterator != records_.end()) return false;
    records_.push_back(incoming);
    return true;
}

void PeerStore::add_or_update(const PeerRecord& incoming) {
    if (!valid_host(incoming.endpoint.host) || incoming.endpoint.port == 0) return;
    const auto iterator = std::find_if(records_.begin(), records_.end(), [&](const PeerRecord& record) {
        return endpoint_equal(record.endpoint, incoming.endpoint);
    });
    if (iterator == records_.end()) {
        records_.push_back(incoming);
        return;
    }
    iterator->services |= incoming.services;
    iterator->last_seen = std::max(iterator->last_seen, incoming.last_seen);
    iterator->last_success = std::max(iterator->last_success, incoming.last_success);
    iterator->failures = std::min(iterator->failures, incoming.failures);
    iterator->manual = iterator->manual || incoming.manual;
}

void PeerStore::mark_success(const PeerEndpoint& endpoint, const std::uint64_t services) {
    const std::int64_t now = now_seconds();
    PeerRecord record{
        .endpoint = endpoint,
        .services = services,
        .last_seen = now,
        .last_success = now,
        .failures = 0,
        .manual = false,
    };
    const auto iterator = std::find_if(records_.begin(), records_.end(), [&](const PeerRecord& stored) {
        return endpoint_equal(stored.endpoint, endpoint);
    });
    if (iterator == records_.end()) records_.push_back(record);
    else {
        iterator->services |= services;
        iterator->last_seen = now;
        iterator->last_success = now;
        iterator->failures = 0;
    }
    save();
}

void PeerStore::mark_failure(const PeerEndpoint& endpoint) {
    const auto iterator = std::find_if(records_.begin(), records_.end(), [&](const PeerRecord& stored) {
        return endpoint_equal(stored.endpoint, endpoint);
    });
    if (iterator == records_.end()) return;
    if (iterator->failures < 1'000'000) ++iterator->failures;
    save();
}

const std::vector<PeerRecord>& PeerStore::records() const noexcept { return records_; }

std::vector<PeerEndpoint> PeerStore::select_outbound(
    const std::size_t maximum,
    const std::vector<PeerEndpoint>& excluded
) const {
    std::vector<PeerRecord> candidates = records_;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const PeerRecord& record) {
        if (record.failures >= 10) return true;
        return std::any_of(excluded.begin(), excluded.end(), [&](const PeerEndpoint& endpoint) {
            return endpoint_equal(record.endpoint, endpoint);
        });
    }), candidates.end());
    std::stable_sort(candidates.begin(), candidates.end(), [](const PeerRecord& left, const PeerRecord& right) {
        if (left.manual != right.manual) return left.manual > right.manual;
        if (left.last_success != right.last_success) return left.last_success > right.last_success;
        if (left.failures != right.failures) return left.failures < right.failures;
        return left.last_seen > right.last_seen;
    });
    std::vector<PeerEndpoint> result;
    for (const PeerRecord& candidate : candidates) {
        if (result.size() >= maximum) break;
        result.push_back(candidate.endpoint);
    }
    return result;
}

const std::filesystem::path& PeerStore::path() const noexcept { return path_; }

} // namespace bincoin
