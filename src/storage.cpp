#include "storage.hpp"

#include "hash.hpp"
#include "platform.hpp"
#include "serialize.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bincoin {
namespace {

constexpr std::array<std::uint8_t, 4> BLOCK_RECORD_MAGIC{'B', 'C', 'R', '9'};
constexpr std::uint32_t BLOCK_RECORD_VERSION = 1;
constexpr std::size_t BLOCK_RECORD_HEADER_SIZE = 4 + 4 + 8 + 32;
constexpr std::uint64_t MAX_BLOCK_RECORD_SIZE = 16ULL * 1024ULL * 1024ULL;
constexpr const char* MANIFEST_MAGIC = "BINARYCOIN_CHAINSTATE_MANIFEST_V1";
constexpr const char* INDEX_MAGIC = "BINARYCOIN_BLOCK_INDEX_V1";
constexpr const char* UTXO_MAGIC = "BINARYCOIN_UTXO_SNAPSHOT_V1";

class FileLock {
public:
    explicit FileLock(const std::filesystem::path& path) {
#ifdef _WIN32
        while (true) {
            handle_ = ::CreateFileW(
                path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) break;

            const DWORD error = ::GetLastError();
            if (error != ERROR_SHARING_VIOLATION &&
                error != ERROR_LOCK_VIOLATION) {
                throw std::runtime_error(
                    "Unable to lock chainstate. Windows error=" +
                    std::to_string(error));
            }
            ::Sleep(5);
        }
#else
        descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (descriptor_ < 0) throw std::runtime_error("Unable to open chainstate lock: " + std::string(std::strerror(errno)));
        if (::flock(descriptor_, LOCK_EX) != 0) {
            const std::string message = std::strerror(errno);
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("Unable to lock chainstate: " + message);
        }
#endif
    }

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    ~FileLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
#else
        if (descriptor_ >= 0) {
            (void)::flock(descriptor_, LOCK_UN);
            (void)::close(descriptor_);
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int descriptor_{-1};
#endif
};

struct IndexEntry {
    std::uint64_t height{0};
    std::uint64_t offset{0};
    std::uint64_t payload_size{0};
    std::string hash;
};

struct Manifest {
    std::uint64_t generation{0};
    std::uint64_t height{0};
    std::string bestblock;
    std::string index_file;
    std::string index_sha256;
    std::string utxo_file;
    std::string utxo_sha256;
    std::uint64_t block_file_size{0};
};

void throw_system_error(const std::string& action) {
    throw std::runtime_error(action + ": " + std::string(std::strerror(errno)));
}


int open_read_file(const std::filesystem::path& path) {
#ifdef _WIN32
    return ::_wopen(path.c_str(), _O_RDONLY | _O_BINARY);
#else
    return ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
#endif
}

int open_temporary_file(const std::filesystem::path& path) {
#ifdef _WIN32
    return ::_wopen(path.c_str(), _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY,
                    _S_IREAD | _S_IWRITE);
#else
    return ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
#endif
}

int open_append_file(const std::filesystem::path& path) {
#ifdef _WIN32
    return ::_wopen(path.c_str(), _O_CREAT | _O_RDWR | _O_APPEND | _O_BINARY,
                    _S_IREAD | _S_IWRITE);
#else
    return ::open(path.c_str(), O_CREAT | O_RDWR | O_APPEND | O_CLOEXEC, 0600);
#endif
}

int close_file(const int descriptor) noexcept {
#ifdef _WIN32
    return ::_close(descriptor);
#else
    return ::close(descriptor);
#endif
}

int sync_file(const int descriptor) {
#ifdef _WIN32
    return ::_commit(descriptor);
#else
    return ::fsync(descriptor);
#endif
}

std::int64_t seek_file_end(const int descriptor) {
#ifdef _WIN32
    return static_cast<std::int64_t>(::_lseeki64(descriptor, 0, SEEK_END));
#else
    return static_cast<std::int64_t>(::lseek(descriptor, 0, SEEK_END));
#endif
}

std::ptrdiff_t write_file_chunk(const int descriptor, const void* data, const std::size_t size) {
#ifdef _WIN32
    const unsigned int chunk = static_cast<unsigned int>(std::min<std::size_t>(size, 0x7fffffffU));
    return static_cast<std::ptrdiff_t>(::_write(descriptor, data, chunk));
#else
    return static_cast<std::ptrdiff_t>(::write(descriptor, data, size));
#endif
}

std::ptrdiff_t read_file_at(
    const int descriptor,
    const std::uint64_t offset,
    void* data,
    const std::size_t size
) {
#ifdef _WIN32
    if (::_lseeki64(descriptor, static_cast<__int64>(offset), SEEK_SET) < 0) return -1;
    const unsigned int chunk = static_cast<unsigned int>(std::min<std::size_t>(size, 0x7fffffffU));
    return static_cast<std::ptrdiff_t>(::_read(descriptor, data, chunk));
#else
    return static_cast<std::ptrdiff_t>(::pread(
        descriptor, data, size, static_cast<off_t>(offset)));
#endif
}

void remove_file_noexcept(const std::filesystem::path& path) noexcept {
    std::error_code error;
    std::filesystem::remove(path, error);
}

void write_all(const int descriptor, std::span<const std::uint8_t> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::ptrdiff_t written = write_file_chunk(
            descriptor,
            bytes.data() + static_cast<std::ptrdiff_t>(offset),
            bytes.size() - offset
        );
        if (written < 0) {
            if (errno == EINTR) continue;
            throw_system_error("Unable to write durable chainstate file");
        }
        if (written == 0) throw std::runtime_error("Short write while storing chainstate");
        offset += static_cast<std::size_t>(written);
    }
}

void pread_all(const int descriptor, const std::uint64_t offset, std::span<std::uint8_t> bytes) {
    std::size_t completed = 0;
    while (completed < bytes.size()) {
        const std::ptrdiff_t count = read_file_at(
            descriptor,
            offset + completed,
            bytes.data() + static_cast<std::ptrdiff_t>(completed),
            bytes.size() - completed
        );
        if (count < 0) {
            if (errno == EINTR) continue;
            throw_system_error("Unable to read block record");
        }
        if (count == 0) throw std::runtime_error("Truncated block record");
        completed += static_cast<std::size_t>(count);
    }
}

void fsync_directory(const std::filesystem::path& directory) {
#ifdef _WIN32
    // MoveFileExW(..., MOVEFILE_WRITE_THROUGH) provides the durable replacement barrier.
    (void)directory;
#else
    const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) throw_system_error("Unable to open directory for fsync");
    if (sync_file(descriptor) != 0) {
        const std::string message = std::strerror(errno);
        (void)close_file(descriptor);
        throw std::runtime_error("Unable to fsync directory: " + message);
    }
    (void)close_file(descriptor);
#endif
}

std::vector<std::uint8_t> string_bytes(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string sha256_hex(std::span<const std::uint8_t> bytes) {
    return bytes_to_hex(sha256(bytes));
}

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Unable to open chainstate file: " + path.string());
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length < 0) throw std::runtime_error("Unable to determine chainstate file size");
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input) throw std::runtime_error("Unable to read complete chainstate file: " + path.string());
    return bytes;
}

void atomic_write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::filesystem::create_directories(path.parent_path());
    const std::filesystem::path temporary = path.string() + ".tmp";
    const int descriptor = open_temporary_file(temporary);
    if (descriptor < 0) throw_system_error("Unable to create temporary chainstate file");
    bool closed = false;
    try {
        write_all(descriptor, bytes);
        if (sync_file(descriptor) != 0) throw_system_error("Unable to sync temporary chainstate file");
        if (close_file(descriptor) != 0) throw_system_error("Unable to close temporary chainstate file");
        closed = true;
    } catch (...) {
        if (!closed) (void)close_file(descriptor);
        remove_file_noexcept(temporary);
        throw;
    }
    replace_file_atomically(temporary, path);
    fsync_directory(path.parent_path());
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
Integer parse_integer(const std::string& text, const char* field) {
    Integer value{};
    const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (error != std::errc{} || pointer != text.data() + text.size()) {
        throw std::runtime_error(std::string("Invalid ") + field + ": " + text);
    }
    return value;
}

std::string generation_name(const char* prefix, const std::uint64_t generation) {
    std::ostringstream output;
    output << prefix << std::setw(20) << std::setfill('0') << generation << ".tsv";
    return output.str();
}

std::string make_index_text(const std::vector<IndexEntry>& entries) {
    std::ostringstream output;
    output << INDEX_MAGIC << '\n';
    for (const IndexEntry& entry : entries) {
        output << entry.height << '\t' << entry.offset << '\t' << entry.payload_size << '\t' << entry.hash << '\n';
    }
    return output.str();
}

std::vector<IndexEntry> parse_index_text(const std::string& text) {
    std::istringstream input(text);
    std::string line;
    if (!std::getline(input, line) || line != INDEX_MAGIC) throw std::runtime_error("Invalid block-index header");
    std::vector<IndexEntry> entries;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split(line, '\t');
        if (fields.size() != 4) throw std::runtime_error("Malformed block-index record");
        IndexEntry entry;
        entry.height = parse_integer<std::uint64_t>(fields[0], "block-index height");
        entry.offset = parse_integer<std::uint64_t>(fields[1], "block-index offset");
        entry.payload_size = parse_integer<std::uint64_t>(fields[2], "block-index payload size");
        entry.hash = fields[3];
        if (entry.payload_size > MAX_BLOCK_RECORD_SIZE) throw std::runtime_error("Indexed block payload exceeds limit");
        if (entry.height != entries.size()) throw std::runtime_error("Block index is not contiguous");
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) throw std::runtime_error("Block index contains no genesis record");
    return entries;
}

std::string make_utxo_text(
    const UtxoSet& utxos,
    const std::uint64_t height,
    const std::string& bestblock
) {
    std::ostringstream output;
    output << UTXO_MAGIC << '\n'
           << "height=" << height << '\n'
           << "bestblock=" << bestblock << '\n';
    for (const auto& [outpoint, coin] : utxos) {
        output << outpoint.txid << '\t'
               << outpoint.index << '\t'
               << coin.output.value << '\t'
               << coin.height << '\t'
               << (coin.coinbase ? 1 : 0) << '\t'
               << bytes_to_hex(coin.output.script_pubkey) << '\n';
    }
    return output.str();
}

UtxoSet parse_utxo_text(
    const std::string& text,
    const std::uint64_t expected_height,
    const std::string& expected_bestblock
) {
    std::istringstream input(text);
    std::string line;
    if (!std::getline(input, line) || line != UTXO_MAGIC) throw std::runtime_error("Invalid UTXO snapshot header");
    if (!std::getline(input, line) || !line.starts_with("height=")) throw std::runtime_error("UTXO snapshot height missing");
    const std::uint64_t height = parse_integer<std::uint64_t>(line.substr(7), "UTXO snapshot height");
    if (!std::getline(input, line) || !line.starts_with("bestblock=")) throw std::runtime_error("UTXO best block missing");
    const std::string bestblock = line.substr(10);
    if (height != expected_height || bestblock != expected_bestblock) {
        throw std::runtime_error("UTXO snapshot does not match committed active chain");
    }

    UtxoSet utxos;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split(line, '\t');
        if (fields.size() != 6) throw std::runtime_error("Malformed UTXO snapshot record");
        OutPoint outpoint{
            .txid = fields[0],
            .index = parse_integer<std::uint32_t>(fields[1], "UTXO output index"),
        };
        Coin coin{
            .output = TxOutput{
                .value = parse_integer<Amount>(fields[2], "UTXO value"),
                .script_pubkey = hex_to_bytes(fields[5]),
            },
            .height = parse_integer<std::uint64_t>(fields[3], "UTXO height"),
            .coinbase = fields[4] == "1",
        };
        if (!money_range(coin.output.value)) throw std::runtime_error("UTXO value outside money range");
        if (fields[4] != "0" && fields[4] != "1") throw std::runtime_error("Invalid UTXO coinbase flag");
        if (!utxos.emplace(std::move(outpoint), std::move(coin)).second) {
            throw std::runtime_error("Duplicate UTXO snapshot outpoint");
        }
    }
    return utxos;
}

std::string make_manifest_text(const Manifest& manifest) {
    std::ostringstream output;
    output << MANIFEST_MAGIC << '\n'
           << "generation=" << manifest.generation << '\n'
           << "height=" << manifest.height << '\n'
           << "bestblock=" << manifest.bestblock << '\n'
           << "index_file=" << manifest.index_file << '\n'
           << "index_sha256=" << manifest.index_sha256 << '\n'
           << "utxo_file=" << manifest.utxo_file << '\n'
           << "utxo_sha256=" << manifest.utxo_sha256 << '\n'
           << "block_file_size=" << manifest.block_file_size << '\n';
    return output.str();
}

Manifest parse_manifest_text(const std::string& text) {
    std::istringstream input(text);
    std::string line;
    if (!std::getline(input, line) || line != MANIFEST_MAGIC) throw std::runtime_error("Invalid chainstate manifest header");
    std::map<std::string, std::string> fields;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) throw std::runtime_error("Malformed chainstate manifest field");
        fields.emplace(line.substr(0, separator), line.substr(separator + 1));
    }
    const auto get = [&](const char* name) -> const std::string& {
        const auto iterator = fields.find(name);
        if (iterator == fields.end()) throw std::runtime_error(std::string("Missing chainstate manifest field: ") + name);
        return iterator->second;
    };
    return Manifest{
        .generation = parse_integer<std::uint64_t>(get("generation"), "manifest generation"),
        .height = parse_integer<std::uint64_t>(get("height"), "manifest height"),
        .bestblock = get("bestblock"),
        .index_file = get("index_file"),
        .index_sha256 = get("index_sha256"),
        .utxo_file = get("utxo_file"),
        .utxo_sha256 = get("utxo_sha256"),
        .block_file_size = parse_integer<std::uint64_t>(get("block_file_size"), "manifest block file size"),
    };
}

std::optional<std::uint64_t> generation_from_index_filename(const std::string& filename) {
    constexpr std::string_view prefix = "block-index-v1-";
    constexpr std::string_view suffix = ".tsv";
    if (!filename.starts_with(prefix) || !filename.ends_with(suffix)) return std::nullopt;
    const std::string number = filename.substr(prefix.size(), filename.size() - prefix.size() - suffix.size());
    try {
        return parse_integer<std::uint64_t>(number, "index generation");
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::uint64_t> available_generations(const std::filesystem::path& directory) {
    std::vector<std::uint64_t> generations;
    if (!std::filesystem::exists(directory)) return generations;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        const auto generation = generation_from_index_filename(entry.path().filename().string());
        if (!generation) continue;
        const auto utxo = directory / generation_name("utxo-v1-", *generation);
        if (std::filesystem::exists(utxo)) generations.push_back(*generation);
    }
    std::sort(generations.begin(), generations.end(), std::greater<>());
    generations.erase(std::unique(generations.begin(), generations.end()), generations.end());
    return generations;
}


void cleanup_old_generations(const std::filesystem::path& directory, const std::uint64_t current) {
    for (const std::uint64_t generation : available_generations(directory)) {
        if (generation == current || (current > 0 && generation + 1 == current)) continue;
        std::error_code error;
        std::filesystem::remove(directory / generation_name("block-index-v1-", generation), error);
        error.clear();
        std::filesystem::remove(directory / generation_name("utxo-v1-", generation), error);
    }
}

std::uint64_t file_size_or_zero(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

std::vector<std::uint8_t> make_record_bytes(const StoredBlock& block) {
    const auto payload = serialize_stored_block(block);
    if (payload.size() > MAX_BLOCK_RECORD_SIZE) throw std::runtime_error("Block record exceeds durable storage limit");
    const Hash256 checksum = sha256d(payload);
    std::vector<std::uint8_t> record;
    record.reserve(BLOCK_RECORD_HEADER_SIZE + payload.size());
    record.insert(record.end(), BLOCK_RECORD_MAGIC.begin(), BLOCK_RECORD_MAGIC.end());
    append_u32_le(record, BLOCK_RECORD_VERSION);
    append_u64_le(record, payload.size());
    record.insert(record.end(), checksum.begin(), checksum.end());
    record.insert(record.end(), payload.begin(), payload.end());
    return record;
}

StoredBlock read_record_at(
    const int descriptor,
    const std::uint64_t offset,
    const std::optional<std::uint64_t> expected_payload_size = std::nullopt
) {
    std::array<std::uint8_t, BLOCK_RECORD_HEADER_SIZE> header{};
    pread_all(descriptor, offset, header);
    if (!std::equal(BLOCK_RECORD_MAGIC.begin(), BLOCK_RECORD_MAGIC.end(), header.begin())) {
        throw std::runtime_error("Invalid block-record magic");
    }
    std::size_t header_offset = 4;
    const std::uint32_t version = read_u32_le(header, header_offset);
    if (version != BLOCK_RECORD_VERSION) throw std::runtime_error("Unsupported block-record version");
    const std::uint64_t payload_size = read_u64_le(header, header_offset);
    if (payload_size > MAX_BLOCK_RECORD_SIZE) throw std::runtime_error("Block-record payload exceeds limit");
    if (expected_payload_size && payload_size != *expected_payload_size) {
        throw std::runtime_error("Block-index payload size mismatch");
    }
    const auto checksum_vector = read_bytes(header, header_offset, 32);
    std::array<std::uint8_t, 32> checksum{};
    std::copy(checksum_vector.begin(), checksum_vector.end(), checksum.begin());
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(payload_size));
    pread_all(descriptor, offset + BLOCK_RECORD_HEADER_SIZE, payload);
    if (sha256d(payload) != checksum) throw std::runtime_error("Block-record checksum mismatch");
    return deserialize_stored_block(payload);
}

std::vector<StoredBlock> load_indexed_chain(
    const std::filesystem::path& blocks_path,
    const std::vector<IndexEntry>& entries
) {
    const int descriptor = open_read_file(blocks_path);
    if (descriptor < 0) throw_system_error("Unable to open block data file");
    std::vector<StoredBlock> blocks;
    try {
        blocks.reserve(entries.size());
        for (const IndexEntry& entry : entries) {
            StoredBlock block = read_record_at(descriptor, entry.offset, entry.payload_size);
            if (block.height != entry.height || block.block.header.hash_hex() != entry.hash) {
                throw std::runtime_error("Block index points to the wrong block record");
            }
            blocks.push_back(std::move(block));
        }
        (void)close_file(descriptor);
    } catch (...) {
        (void)close_file(descriptor);
        throw;
    }
    return blocks;
}

std::map<std::string, IndexEntry> scan_catalog(const std::filesystem::path& blocks_path) {
    std::map<std::string, IndexEntry> catalog;
    if (!std::filesystem::exists(blocks_path)) return catalog;
    const std::uint64_t file_size = file_size_or_zero(blocks_path);
    const int descriptor = open_read_file(blocks_path);
    if (descriptor < 0) throw_system_error("Unable to scan block data file");
    try {
        std::uint64_t offset = 0;
        while (offset < file_size) {
            if (file_size - offset < BLOCK_RECORD_HEADER_SIZE) break; // interrupted tail is ignored
            std::array<std::uint8_t, BLOCK_RECORD_HEADER_SIZE> header{};
            pread_all(descriptor, offset, header);
            if (!std::equal(BLOCK_RECORD_MAGIC.begin(), BLOCK_RECORD_MAGIC.end(), header.begin())) break;
            std::size_t header_offset = 4;
            const std::uint32_t version = read_u32_le(header, header_offset);
            if (version != BLOCK_RECORD_VERSION) break;
            const std::uint64_t payload_size = read_u64_le(header, header_offset);
            if (payload_size > MAX_BLOCK_RECORD_SIZE || BLOCK_RECORD_HEADER_SIZE + payload_size > file_size - offset) break;
            try {
                StoredBlock block = read_record_at(descriptor, offset, payload_size);
                catalog.emplace(block.block.header.hash_hex(), IndexEntry{
                    .height = block.height,
                    .offset = offset,
                    .payload_size = payload_size,
                    .hash = block.block.header.hash_hex(),
                });
            } catch (...) {
                // The length-prefixed record is damaged, but later records may still be usable.
            }
            offset += BLOCK_RECORD_HEADER_SIZE + payload_size;
        }
        (void)close_file(descriptor);
    } catch (...) {
        (void)close_file(descriptor);
        throw;
    }
    return catalog;
}

Manifest manifest_for_generation(
    const std::filesystem::path& chainstate_directory,
    const std::filesystem::path& blocks_path,
    const std::uint64_t generation,
    const std::vector<StoredBlock>& blocks,
    const UtxoSet& utxos
) {
    const std::string index_file = generation_name("block-index-v1-", generation);
    const std::string utxo_file = generation_name("utxo-v1-", generation);
    const auto catalog = scan_catalog(blocks_path);
    std::vector<IndexEntry> entries;
    entries.reserve(blocks.size());
    for (const StoredBlock& block : blocks) {
        const auto iterator = catalog.find(block.block.header.hash_hex());
        if (iterator == catalog.end()) throw std::runtime_error("Committed block is missing from append-only block store");
        IndexEntry entry = iterator->second;
        entry.height = block.height;
        entries.push_back(std::move(entry));
    }

    const std::string index_text = make_index_text(entries);
    const std::string utxo_text = make_utxo_text(utxos, blocks.back().height, blocks.back().block.header.hash_hex());
    const auto index_bytes = string_bytes(index_text);
    const auto utxo_bytes = string_bytes(utxo_text);
    atomic_write(chainstate_directory / index_file, index_bytes);
    atomic_write(chainstate_directory / utxo_file, utxo_bytes);

    return Manifest{
        .generation = generation,
        .height = blocks.back().height,
        .bestblock = blocks.back().block.header.hash_hex(),
        .index_file = index_file,
        .index_sha256 = sha256_hex(index_bytes),
        .utxo_file = utxo_file,
        .utxo_sha256 = sha256_hex(utxo_bytes),
        .block_file_size = file_size_or_zero(blocks_path),
    };
}

std::pair<std::vector<StoredBlock>, UtxoSet> load_manifest_generation(
    const std::filesystem::path& chainstate_directory,
    const std::filesystem::path& blocks_path,
    const Manifest& manifest,
    const bool verify_hashes
) {
    const auto index_bytes = read_file_bytes(chainstate_directory / manifest.index_file);
    const auto utxo_bytes = read_file_bytes(chainstate_directory / manifest.utxo_file);
    if (verify_hashes) {
        if (sha256_hex(index_bytes) != manifest.index_sha256) throw std::runtime_error("Block-index checksum mismatch");
        if (sha256_hex(utxo_bytes) != manifest.utxo_sha256) throw std::runtime_error("UTXO snapshot checksum mismatch");
    }
    const std::string index_text(index_bytes.begin(), index_bytes.end());
    const std::string utxo_text(utxo_bytes.begin(), utxo_bytes.end());
    const auto entries = parse_index_text(index_text);
    auto blocks = load_indexed_chain(blocks_path, entries);
    if (blocks.back().height != manifest.height || blocks.back().block.header.hash_hex() != manifest.bestblock) {
        throw std::runtime_error("Chainstate manifest tip does not match block index");
    }
    auto utxos = parse_utxo_text(utxo_text, manifest.height, manifest.bestblock);
    return {std::move(blocks), std::move(utxos)};
}

} // namespace

std::vector<std::uint8_t> serialize_stored_block(const StoredBlock& stored) {
    std::vector<std::uint8_t> output;
    append_u64_le(output, stored.height);
    const auto header = stored.block.header.serialize();
    output.insert(output.end(), header.begin(), header.end());
    append_compact_size(output, stored.block.transactions.size());
    for (const Transaction& transaction : stored.block.transactions) {
        const auto transaction_bytes = transaction.serialize();
        append_compact_size(output, transaction_bytes.size());
        output.insert(output.end(), transaction_bytes.begin(), transaction_bytes.end());
    }
    return output;
}

StoredBlock deserialize_stored_block(const std::span<const std::uint8_t> bytes) {
    std::size_t offset = 0;
    StoredBlock stored;
    stored.height = read_u64_le(bytes, offset);
    stored.block.header = BlockHeader::deserialize(read_bytes(bytes, offset, 80));
    const std::uint64_t transaction_count = read_compact_size(bytes, offset);
    if (transaction_count > 100'000) throw std::runtime_error("Stored block transaction count exceeds limit");
    stored.block.transactions.reserve(static_cast<std::size_t>(transaction_count));
    for (std::uint64_t index = 0; index < transaction_count; ++index) {
        const std::uint64_t transaction_size = read_compact_size(bytes, offset);
        if (transaction_size > 4'000'000 || transaction_size > bytes.size() - offset) {
            throw std::runtime_error("Stored transaction payload exceeds limit");
        }
        stored.block.transactions.push_back(Transaction::deserialize(
            read_bytes(bytes, offset, static_cast<std::size_t>(transaction_size))
        ));
    }
    if (offset != bytes.size()) throw std::runtime_error("Trailing bytes in stored block record");
    return stored;
}

DurableChainStore::DurableChainStore(std::filesystem::path data_directory)
    : data_directory_(std::move(data_directory)),
      blocks_directory_(data_directory_ / "blocks"),
      chainstate_directory_(data_directory_ / "chainstate"),
      blocks_path_(blocks_directory_ / "blk00000.dat"),
      manifest_path_(chainstate_directory_ / "manifest-v1"),
      lock_path_(chainstate_directory_ / "LOCK") {}

bool DurableChainStore::exists() const {
    return std::filesystem::exists(manifest_path_) ||
           (!available_generations(chainstate_directory_).empty() && std::filesystem::exists(blocks_path_));
}

std::vector<StoredBlock> DurableChainStore::load(UtxoSet& utxos) {
    std::filesystem::create_directories(blocks_directory_);
    std::filesystem::create_directories(chainstate_directory_);
    FileLock lock(lock_path_);
    stats_.recovered = false;

    if (std::filesystem::exists(manifest_path_)) {
        try {
            const auto manifest_bytes = read_file_bytes(manifest_path_);
            const Manifest manifest = parse_manifest_text(std::string(manifest_bytes.begin(), manifest_bytes.end()));
            auto [blocks, loaded_utxos] = load_manifest_generation(
                chainstate_directory_, blocks_path_, manifest, true);
            stats_.generation = manifest.generation;
            stats_.active_height = manifest.height;
            stats_.block_file_bytes = file_size_or_zero(blocks_path_);
            stats_.block_records = scan_catalog(blocks_path_).size();
            utxos = std::move(loaded_utxos);
            return blocks;
        } catch (const std::exception&) {
            stats_.recovered = true;
        }
    } else {
        stats_.recovered = true;
    }

    for (const std::uint64_t generation : available_generations(chainstate_directory_)) {
        try {
            Manifest candidate{
                .generation = generation,
                .height = 0,
                .bestblock = {},
                .index_file = generation_name("block-index-v1-", generation),
                .index_sha256 = {},
                .utxo_file = generation_name("utxo-v1-", generation),
                .utxo_sha256 = {},
                .block_file_size = 0,
            };
            const auto index_bytes = read_file_bytes(chainstate_directory_ / candidate.index_file);
            const auto entries = parse_index_text(std::string(index_bytes.begin(), index_bytes.end()));
            auto blocks = load_indexed_chain(blocks_path_, entries);
            if (blocks.empty()) continue;
            candidate.height = blocks.back().height;
            candidate.bestblock = blocks.back().block.header.hash_hex();
            // A recovery scan rebuilds UTXOs from validated blocks in TestnetChain::load.
            utxos.clear();
            stats_.generation = generation;
            stats_.active_height = candidate.height;
            stats_.block_file_bytes = file_size_or_zero(blocks_path_);
            stats_.block_records = scan_catalog(blocks_path_).size();
            return blocks;
        } catch (const std::exception&) {
            continue;
        }
    }
    throw std::runtime_error("No valid durable chainstate generation was found; run reindex");
}

void DurableChainStore::commit(const std::vector<StoredBlock>& blocks, const UtxoSet& utxos) {
    if (blocks.empty()) throw std::runtime_error("Cannot commit an empty active chain");
    std::filesystem::create_directories(blocks_directory_);
    std::filesystem::create_directories(chainstate_directory_);
    FileLock lock(lock_path_);

    auto catalog = scan_catalog(blocks_path_);
    const int descriptor = open_append_file(blocks_path_);
    if (descriptor < 0) throw_system_error("Unable to open append-only block store");
    try {
        const std::int64_t end_offset = seek_file_end(descriptor);
        if (end_offset < 0) throw_system_error("Unable to seek append-only block store");
        std::uint64_t offset = static_cast<std::uint64_t>(end_offset);
        bool appended = false;
        for (const StoredBlock& block : blocks) {
            const std::string hash = block.block.header.hash_hex();
            if (catalog.contains(hash)) continue;
            const auto record = make_record_bytes(block);
            write_all(descriptor, record);
            catalog.emplace(hash, IndexEntry{
                .height = block.height,
                .offset = offset,
                .payload_size = record.size() - BLOCK_RECORD_HEADER_SIZE,
                .hash = hash,
            });
            offset += record.size();
            appended = true;
        }
        if (appended && sync_file(descriptor) != 0) throw_system_error("Unable to fsync append-only block store");
        if (close_file(descriptor) != 0) throw_system_error("Unable to close append-only block store");
    } catch (...) {
        (void)close_file(descriptor);
        throw;
    }
    fsync_directory(blocks_directory_);

    std::uint64_t generation = 1;
    if (std::filesystem::exists(manifest_path_)) {
        try {
            const auto bytes = read_file_bytes(manifest_path_);
            generation = parse_manifest_text(std::string(bytes.begin(), bytes.end())).generation + 1;
        } catch (...) {
            const auto generations = available_generations(chainstate_directory_);
            if (!generations.empty()) generation = generations.front() + 1;
        }
    } else {
        const auto generations = available_generations(chainstate_directory_);
        if (!generations.empty()) generation = generations.front() + 1;
    }

    const Manifest manifest = manifest_for_generation(
        chainstate_directory_, blocks_path_, generation, blocks, utxos);
    const std::string manifest_text = make_manifest_text(manifest);
    atomic_write(manifest_path_, string_bytes(manifest_text)); // commit point: manifest is always last
    if (stats_.migrated_legacy) {
        const std::string marker = "migrated from chain-v3.tsv\n";
        atomic_write(chainstate_directory_ / "migrated-from-chain-v3", string_bytes(marker));
    }
    cleanup_old_generations(chainstate_directory_, generation);

    stats_.generation = generation;
    stats_.active_height = blocks.back().height;
    stats_.block_file_bytes = manifest.block_file_size;
    stats_.block_records = catalog.size();
}

std::vector<StoredBlock> DurableChainStore::scan_all_blocks() const {
    std::vector<StoredBlock> blocks;
    const auto catalog = scan_catalog(blocks_path_);
    if (catalog.empty()) return blocks;
    const int descriptor = open_read_file(blocks_path_);
    if (descriptor < 0) throw_system_error("Unable to open append-only block store");
    try {
        blocks.reserve(catalog.size());
        for (const auto& [hash, entry] : catalog) {
            (void)hash;
            blocks.push_back(read_record_at(descriptor, entry.offset, entry.payload_size));
        }
        (void)close_file(descriptor);
    } catch (...) {
        (void)close_file(descriptor);
        throw;
    }
    return blocks;
}

ChainStorageStats DurableChainStore::stats() const {
    ChainStorageStats result = stats_;
    result.migrated_legacy = result.migrated_legacy ||
        std::filesystem::exists(chainstate_directory_ / "migrated-from-chain-v3");
    result.block_file_bytes = file_size_or_zero(blocks_path_);
    try {
        result.block_records = scan_catalog(blocks_path_).size();
    } catch (...) {
        // Status should preserve the last known values when the store is damaged.
    }
    return result;
}

void DurableChainStore::mark_migrated_legacy() noexcept { stats_.migrated_legacy = true; }

const std::filesystem::path& DurableChainStore::blocks_path() const noexcept { return blocks_path_; }
const std::filesystem::path& DurableChainStore::chainstate_directory() const noexcept { return chainstate_directory_; }

} // namespace bincoin
