#include "mnemonic.hpp"

#include "hash.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace bincoin {
namespace {

constexpr std::array<std::string_view, 2048> WORDS{
#include "bip39_words.inc"
};

const std::unordered_map<std::string_view, std::uint16_t>& word_index() {
    static const auto indexes = [] {
        std::unordered_map<std::string_view, std::uint16_t> result;
        result.reserve(WORDS.size());
        for (std::size_t index = 0; index < WORDS.size(); ++index) {
            result.emplace(WORDS[index], static_cast<std::uint16_t>(index));
        }
        return result;
    }();
    return indexes;
}

std::string normalize_spaces(const std::string& input) {
    std::istringstream stream(input);
    std::ostringstream output;
    std::string word;
    bool first = true;
    while (stream >> word) {
        if (!first) output << ' ';
        output << word;
        first = false;
    }
    return output.str();
}

} // namespace

Entropy256 generate_entropy256() {
    Entropy256 entropy{};
    if (RAND_bytes(entropy.data(), static_cast<int>(entropy.size())) != 1) {
        throw std::runtime_error("Secure mnemonic entropy generation failed");
    }
    return entropy;
}

std::vector<std::string> mnemonic_words(const std::string& phrase) {
    std::vector<std::string> result;
    std::istringstream stream(normalize_spaces(phrase));
    std::string word;
    while (stream >> word) {
        std::transform(word.begin(), word.end(), word.begin(), [](const unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        result.push_back(word);
    }
    return result;
}

std::string entropy_to_mnemonic(const Entropy256& entropy) {
    const auto checksum = sha256(entropy);
    std::array<std::uint8_t, 33> data{};
    std::copy(entropy.begin(), entropy.end(), data.begin());
    data.back() = checksum.front();

    std::ostringstream phrase;
    for (std::size_t word_number = 0; word_number < 24; ++word_number) {
        std::uint16_t index = 0;
        for (std::size_t bit = 0; bit < 11; ++bit) {
            const std::size_t absolute = word_number * 11 + bit;
            const std::uint8_t byte = data[absolute / 8];
            const std::uint8_t value = static_cast<std::uint8_t>((byte >> (7 - (absolute % 8))) & 1U);
            index = static_cast<std::uint16_t>((index << 1U) | value);
        }
        if (word_number != 0) phrase << ' ';
        phrase << WORDS[index];
    }
    return phrase.str();
}

Entropy256 mnemonic_to_entropy(const std::string& phrase) {
    const auto words = mnemonic_words(phrase);
    if (words.size() != 24) throw std::invalid_argument("HD import requires exactly 24 words");

    std::array<std::uint8_t, 33> data{};
    std::size_t bit_position = 0;
    for (const auto& word : words) {
        const auto found = word_index().find(word);
        if (found == word_index().end()) throw std::invalid_argument("Import phrase contains an unknown word: " + word);
        const std::uint16_t index = found->second;
        for (int bit = 10; bit >= 0; --bit) {
            if (((index >> bit) & 1U) != 0U) {
                data[bit_position / 8] = static_cast<std::uint8_t>(
                    data[bit_position / 8] | (1U << (7 - (bit_position % 8))));
            }
            ++bit_position;
        }
    }

    Entropy256 entropy{};
    std::copy_n(data.begin(), entropy.size(), entropy.begin());
    const auto checksum = sha256(entropy);
    if (data.back() != checksum.front()) throw std::invalid_argument("Import phrase checksum is invalid");
    return entropy;
}

Seed512 mnemonic_to_seed(const std::string& phrase, const std::string& passphrase) {
    const std::string normalized = normalize_spaces(phrase);
    const std::string salt = "mnemonic" + passphrase;
    Seed512 seed{};
    if (PKCS5_PBKDF2_HMAC(
            normalized.c_str(), static_cast<int>(normalized.size()),
            reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size()),
            2048, EVP_sha512(), static_cast<int>(seed.size()), seed.data()) != 1) {
        throw std::runtime_error("PBKDF2-HMAC-SHA512 failed");
    }
    return seed;
}

} // namespace bincoin
