#include "ban_store.hpp"

#include "platform.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace bincoin {
namespace {

constexpr const char* FILE_MAGIC = "BINARYCOIN_BANLIST_V1";

std::int64_t now_seconds() {
    return static_cast<std::int64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

bool valid_host(const std::string& host) {
    return !host.empty() && host.size() <= 255 && host.find_first_of("\t\r\n") == std::string::npos;
}

std::string sanitize_reason(std::string reason) {
    for (char& character : reason) {
        if (character == '\t' || character == '\r' || character == '\n') character = ' ';
    }
    if (reason.size() > 160) reason.resize(160);
    return reason;
}

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
        throw std::runtime_error(std::string("Invalid banlist ") + field);
    }
    return value;
}

} // namespace

BanStore::BanStore(std::filesystem::path data_directory)
    : data_directory_(std::move(data_directory)), path_(data_directory_ / "banlist-v1.tsv") {}

void BanStore::load() {
    records_.clear();
    if (!std::filesystem::exists(path_)) return;
    std::ifstream input(path_);
    if (!input) throw std::runtime_error("Unable to open banlist-v1.tsv");
    std::string line;
    if (!std::getline(input, line) || line != FILE_MAGIC) {
        throw std::runtime_error("Invalid banlist-v1.tsv header");
    }
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split(line, '\t');
        if (fields.size() != 4) throw std::runtime_error("Malformed banlist-v1.tsv record");
        BanRecord record{
            .host = fields[0],
            .banned_until = parse_number<std::int64_t>(fields[1], "expiry"),
            .score = parse_number<std::uint32_t>(fields[2], "score"),
            .reason = fields[3],
        };
        if (!valid_host(record.host)) throw std::runtime_error("Invalid banlist host");
        records_.push_back(std::move(record));
    }
    prune_expired();
}

void BanStore::save() const {
    std::filesystem::create_directories(data_directory_);
    const std::filesystem::path temporary = path_.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("Unable to create temporary banlist");
    output << FILE_MAGIC << '\n';
    for (const BanRecord& record : records_) {
        output << record.host << '\t' << record.banned_until << '\t'
               << record.score << '\t' << sanitize_reason(record.reason) << '\n';
    }
    output.flush();
    if (!output) throw std::runtime_error("Unable to flush banlist");
    replace_file_atomically(temporary, path_);
}

void BanStore::prune_expired(std::int64_t now) {
    if (now == 0) now = now_seconds();
    records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const BanRecord& record) {
        return record.banned_until <= now;
    }), records_.end());
}

void BanStore::ban(const std::string& host, const std::int64_t duration_seconds,
                   const std::uint32_t score, std::string reason) {
    if (!valid_host(host) || duration_seconds <= 0) return;
    const std::int64_t expiry = now_seconds() + duration_seconds;
    const auto iterator = std::find_if(records_.begin(), records_.end(), [&](const BanRecord& record) {
        return record.host == host;
    });
    if (iterator == records_.end()) {
        records_.push_back(BanRecord{
            .host = host,
            .banned_until = expiry,
            .score = score,
            .reason = sanitize_reason(std::move(reason)),
        });
    } else {
        iterator->banned_until = std::max(iterator->banned_until, expiry);
        iterator->score = std::max(iterator->score, score);
        iterator->reason = sanitize_reason(std::move(reason));
    }
    save();
}

void BanStore::clear() {
    records_.clear();
    save();
}

bool BanStore::is_banned(const std::string& host, std::int64_t now) const {
    if (now == 0) now = now_seconds();
    return std::any_of(records_.begin(), records_.end(), [&](const BanRecord& record) {
        return record.host == host && record.banned_until > now;
    });
}

const std::vector<BanRecord>& BanStore::records() const noexcept { return records_; }
const std::filesystem::path& BanStore::path() const noexcept { return path_; }

} // namespace bincoin
