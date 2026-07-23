#include "script.hpp"

#include "key.hpp"
#include "serialize.hpp"

#include <stdexcept>

namespace bincoin {

std::vector<std::uint8_t> make_p2pk_script(const std::span<const std::uint8_t> compressed_public_key) {
    if (!valid_compressed_public_key(compressed_public_key)) {
        throw std::invalid_argument("P2PK requires a valid compressed secp256k1 public key");
    }
    std::vector<std::uint8_t> script;
    script.reserve(35);
    script.push_back(33U);
    script.insert(script.end(), compressed_public_key.begin(), compressed_public_key.end());
    script.push_back(OP_CHECKSIG);
    return script;
}

bool is_p2pk_script(const std::span<const std::uint8_t> script_pubkey) {
    return script_pubkey.size() == 35 && script_pubkey[0] == 33U &&
           script_pubkey[34] == OP_CHECKSIG &&
           valid_compressed_public_key(script_pubkey.subspan(1, 33));
}

std::vector<std::uint8_t> extract_p2pk_public_key(const std::span<const std::uint8_t> script_pubkey) {
    if (!is_p2pk_script(script_pubkey)) throw std::invalid_argument("Not a supported P2PK script");
    return {script_pubkey.begin() + 1, script_pubkey.begin() + 34};
}

Hash256 signature_hash_all(
    const Transaction& transaction,
    const std::size_t input_index,
    const std::span<const std::uint8_t> previous_script_pubkey
) {
    if (input_index >= transaction.inputs.size()) throw std::out_of_range("Input index out of range");
    Transaction copy = transaction;
    for (TxInput& input : copy.inputs) input.script_sig.clear();
    copy.inputs[input_index].script_sig.assign(previous_script_pubkey.begin(), previous_script_pubkey.end());
    auto bytes = copy.serialize();
    append_u32_le(bytes, SIGHASH_ALL);
    return sha256d(bytes);
}

void sign_p2pk_input(
    Transaction& transaction,
    const std::size_t input_index,
    const std::span<const std::uint8_t> previous_script_pubkey,
    const Secp256k1Key& key
) {
    const auto expected_public_key = extract_p2pk_public_key(previous_script_pubkey);
    if (expected_public_key != key.compressed_public_key()) {
        throw std::invalid_argument("Wallet key does not match spent P2PK output");
    }
    auto signature = key.sign_der(signature_hash_all(transaction, input_index, previous_script_pubkey));
    signature.push_back(static_cast<std::uint8_t>(SIGHASH_ALL));
    transaction.inputs[input_index].script_sig = push_script_data(signature);
}

bool verify_p2pk_input(
    const Transaction& transaction,
    const std::size_t input_index,
    const std::span<const std::uint8_t> previous_script_pubkey
) {
    if (input_index >= transaction.inputs.size() || !is_p2pk_script(previous_script_pubkey)) return false;
    const auto& script_sig = transaction.inputs[input_index].script_sig;
    if (script_sig.size() < 2 || script_sig[0] != script_sig.size() - 1) return false;
    const std::span<const std::uint8_t> pushed(script_sig.data() + 1, script_sig.size() - 1);
    if (pushed.empty() || pushed.back() != static_cast<std::uint8_t>(SIGHASH_ALL)) return false;
    const auto public_key = extract_p2pk_public_key(previous_script_pubkey);
    return verify_ecdsa_der(
        signature_hash_all(transaction, input_index, previous_script_pubkey),
        pushed.first(pushed.size() - 1),
        public_key
    );
}

} // namespace bincoin
