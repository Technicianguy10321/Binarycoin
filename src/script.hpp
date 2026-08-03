#pragma once

#include "hash.hpp"
#include "transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bincoin {

inline constexpr std::uint8_t OP_CHECKSIG = 0xacU;
inline constexpr std::uint32_t SIGHASH_ALL = 1U;

[[nodiscard]] std::vector<std::uint8_t> make_p2pk_script(std::span<const std::uint8_t> compressed_public_key);
[[nodiscard]] bool is_p2pk_script(std::span<const std::uint8_t> script_pubkey);
[[nodiscard]] std::vector<std::uint8_t> extract_p2pk_public_key(std::span<const std::uint8_t> script_pubkey);
[[nodiscard]] Hash256 signature_hash_all(
    const Transaction& transaction,
    std::size_t input_index,
    std::span<const std::uint8_t> previous_script_pubkey
);
void sign_p2pk_input(
    Transaction& transaction,
    std::size_t input_index,
    std::span<const std::uint8_t> previous_script_pubkey,
    const class Secp256k1Key& key
);
[[nodiscard]] bool verify_p2pk_input(
    const Transaction& transaction,
    std::size_t input_index,
    std::span<const std::uint8_t> previous_script_pubkey
);

} // namespace bincoin
