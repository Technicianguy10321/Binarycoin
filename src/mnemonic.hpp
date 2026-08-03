#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bincoin {

using Entropy256 = std::array<std::uint8_t, 32>;
using Seed512 = std::array<std::uint8_t, 64>;

[[nodiscard]] Entropy256 generate_entropy256();
[[nodiscard]] std::string entropy_to_mnemonic(const Entropy256& entropy);
[[nodiscard]] Entropy256 mnemonic_to_entropy(const std::string& phrase);
[[nodiscard]] Seed512 mnemonic_to_seed(const std::string& phrase, const std::string& passphrase = "");
[[nodiscard]] std::vector<std::string> mnemonic_words(const std::string& phrase);

} // namespace bincoin
