#include "key.hpp"

#include "serialize.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>

#ifdef BINARYCOIN_USE_LIBSECP256K1
#include <secp256k1.h>
#else
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>
#endif

#include <openssl/rand.h>

namespace bincoin {
namespace {

#ifdef BINARYCOIN_USE_LIBSECP256K1

struct ContextDeleter {
    void operator()(secp256k1_context* context) const noexcept { secp256k1_context_destroy(context); }
};
using ContextPtr = std::unique_ptr<secp256k1_context, ContextDeleter>;

secp256k1_context* context() {
    static ContextPtr ctx(secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY));
    if (!ctx) throw std::runtime_error("Unable to create libsecp256k1 context");
    return ctx.get();
}

bool backend_valid_secret(const std::span<const std::uint8_t> secret) {
    return secret.size() == 32 && secp256k1_ec_seckey_verify(context(), secret.data()) == 1;
}

#else

template <typename T, void (*Deleter)(T*)>
using OsslPtr = std::unique_ptr<T, decltype(Deleter)>;
using EcKeyPtr = OsslPtr<EC_KEY, EC_KEY_free>;
using BnPtr = OsslPtr<BIGNUM, BN_free>;
using EcPointPtr = OsslPtr<EC_POINT, EC_POINT_free>;
using EcdsaSigPtr = OsslPtr<ECDSA_SIG, ECDSA_SIG_free>;
using BnCtxPtr = OsslPtr<BN_CTX, BN_CTX_free>;

EcKeyPtr new_key() {
    EcKeyPtr key(EC_KEY_new_by_curve_name(NID_secp256k1), EC_KEY_free);
    if (!key) throw std::runtime_error("Unable to create secp256k1 key context");
    EC_KEY_set_conv_form(key.get(), POINT_CONVERSION_COMPRESSED);
    return key;
}

bool backend_valid_secret(const std::span<const std::uint8_t> secret) {
    if (secret.size() != 32) return false;
    auto key = new_key();
    const EC_GROUP* group = EC_KEY_get0_group(key.get());
    BnPtr value(BN_bin2bn(secret.data(), static_cast<int>(secret.size()), nullptr), BN_free);
    BnPtr order(BN_new(), BN_free);
    if (!value || !order || EC_GROUP_get_order(group, order.get(), nullptr) != 1) {
        throw std::runtime_error("Unable to inspect secp256k1 group order");
    }
    return !BN_is_zero(value.get()) && !BN_is_negative(value.get()) && BN_cmp(value.get(), order.get()) < 0;
}

EcKeyPtr key_from_secret(const std::array<std::uint8_t, 32>& secret) {
    if (!backend_valid_secret(secret)) throw std::invalid_argument("Invalid secp256k1 secret key");
    auto key = new_key();
    const EC_GROUP* group = EC_KEY_get0_group(key.get());
    BnPtr private_value(BN_bin2bn(secret.data(), static_cast<int>(secret.size()), nullptr), BN_free);
    EcPointPtr public_point(EC_POINT_new(group), EC_POINT_free);
    BnCtxPtr context_value(BN_CTX_new(), BN_CTX_free);
    if (!private_value || !public_point || !context_value) throw std::runtime_error("OpenSSL allocation failed");
    if (EC_POINT_mul(group, public_point.get(), private_value.get(), nullptr, nullptr, context_value.get()) != 1 ||
        EC_KEY_set_private_key(key.get(), private_value.get()) != 1 ||
        EC_KEY_set_public_key(key.get(), public_point.get()) != 1 ||
        EC_KEY_check_key(key.get()) != 1) {
        throw std::runtime_error("Unable to construct secp256k1 key pair");
    }
    return key;
}

bool is_low_s(const ECDSA_SIG* signature, const EC_GROUP* group) {
    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(signature, &r, &s);
    (void)r;
    BnPtr order(BN_new(), BN_free);
    BnPtr half(BN_new(), BN_free);
    if (!order || !half || EC_GROUP_get_order(group, order.get(), nullptr) != 1 ||
        BN_rshift1(half.get(), order.get()) != 1) {
        throw std::runtime_error("Unable to calculate secp256k1 half order");
    }
    return BN_cmp(s, half.get()) <= 0;
}

void normalize_low_s(ECDSA_SIG* signature, const EC_GROUP* group) {
    const BIGNUM* old_r = nullptr;
    const BIGNUM* old_s = nullptr;
    ECDSA_SIG_get0(signature, &old_r, &old_s);
    BnPtr order(BN_new(), BN_free);
    BnPtr half(BN_new(), BN_free);
    if (!order || !half || EC_GROUP_get_order(group, order.get(), nullptr) != 1 ||
        BN_rshift1(half.get(), order.get()) != 1) {
        throw std::runtime_error("Unable to calculate secp256k1 order");
    }
    if (BN_cmp(old_s, half.get()) <= 0) return;
    BIGNUM* new_r = BN_dup(old_r);
    BIGNUM* new_s = BN_new();
    if (!new_r || !new_s || BN_sub(new_s, order.get(), old_s) != 1) {
        BN_free(new_r);
        BN_free(new_s);
        throw std::runtime_error("Unable to normalize ECDSA signature");
    }
    if (ECDSA_SIG_set0(signature, new_r, new_s) != 1) {
        BN_free(new_r);
        BN_free(new_s);
        throw std::runtime_error("Unable to replace ECDSA signature values");
    }
}

EcKeyPtr key_from_public(const std::span<const std::uint8_t> public_key) {
    auto key = new_key();
    const EC_GROUP* group = EC_KEY_get0_group(key.get());
    EcPointPtr point(EC_POINT_new(group), EC_POINT_free);
    BnCtxPtr context_value(BN_CTX_new(), BN_CTX_free);
    if (!point || !context_value ||
        EC_POINT_oct2point(group, point.get(), public_key.data(), public_key.size(), context_value.get()) != 1 ||
        EC_KEY_set_public_key(key.get(), point.get()) != 1 || EC_KEY_check_key(key.get()) != 1) {
        throw std::invalid_argument("Invalid secp256k1 public key");
    }
    return key;
}

#endif

} // namespace

Secp256k1Key::Secp256k1Key(std::array<std::uint8_t, 32> secret) : secret_(secret) {
    if (!valid_secret_key(secret_)) throw std::invalid_argument("Invalid secp256k1 secret key");
}

Secp256k1Key Secp256k1Key::generate() {
    std::array<std::uint8_t, 32> secret{};
    do {
        if (RAND_bytes(secret.data(), static_cast<int>(secret.size())) != 1) {
            throw std::runtime_error("Secure random generation failed");
        }
    } while (!valid_secret_key(secret));
    return Secp256k1Key(secret);
}

Secp256k1Key Secp256k1Key::from_secret_hex(const std::string& secret_hex_value) {
    const auto bytes = hex_to_bytes(secret_hex_value);
    if (bytes.size() != 32) throw std::invalid_argument("Secret key must be 32 bytes");
    std::array<std::uint8_t, 32> secret{};
    std::copy(bytes.begin(), bytes.end(), secret.begin());
    return Secp256k1Key(secret);
}

Secp256k1Key Secp256k1Key::from_secret_bytes(const std::array<std::uint8_t, 32>& secret) {
    return Secp256k1Key(secret);
}

std::string Secp256k1Key::secret_hex() const { return bytes_to_hex(secret_); }
const std::array<std::uint8_t, 32>& Secp256k1Key::secret_bytes() const noexcept { return secret_; }

std::vector<std::uint8_t> Secp256k1Key::compressed_public_key() const {
#ifdef BINARYCOIN_USE_LIBSECP256K1
    secp256k1_pubkey public_key{};
    if (secp256k1_ec_pubkey_create(context(), &public_key, secret_.data()) != 1) {
        throw std::runtime_error("libsecp256k1 public-key creation failed");
    }
    std::array<std::uint8_t, 33> output{};
    std::size_t length = output.size();
    if (secp256k1_ec_pubkey_serialize(
            context(), output.data(), &length, &public_key, SECP256K1_EC_COMPRESSED) != 1 ||
        length != output.size()) {
        throw std::runtime_error("libsecp256k1 public-key serialization failed");
    }
    return {output.begin(), output.end()};
#else
    auto key = key_from_secret(secret_);
    const EC_GROUP* group = EC_KEY_get0_group(key.get());
    const EC_POINT* point = EC_KEY_get0_public_key(key.get());
    BnCtxPtr context_value(BN_CTX_new(), BN_CTX_free);
    if (!context_value) throw std::runtime_error("OpenSSL allocation failed");
    const std::size_t length = EC_POINT_point2oct(
        group, point, POINT_CONVERSION_COMPRESSED, nullptr, 0, context_value.get());
    if (length != 33) throw std::runtime_error("Unexpected compressed public-key length");
    std::vector<std::uint8_t> output(length);
    if (EC_POINT_point2oct(
            group, point, POINT_CONVERSION_COMPRESSED,
            output.data(), output.size(), context_value.get()) != output.size()) {
        throw std::runtime_error("Unable to serialize compressed public key");
    }
    return output;
#endif
}

std::string Secp256k1Key::compressed_public_key_hex() const { return bytes_to_hex(compressed_public_key()); }

std::vector<std::uint8_t> Secp256k1Key::sign_der(const Hash256& message_hash) const {
#ifdef BINARYCOIN_USE_LIBSECP256K1
    secp256k1_ecdsa_signature signature{};
    if (secp256k1_ecdsa_sign(context(), &signature, message_hash.data(), secret_.data(), nullptr, nullptr) != 1) {
        throw std::runtime_error("libsecp256k1 signing failed");
    }
    secp256k1_ecdsa_signature normalized{};
    (void)secp256k1_ecdsa_signature_normalize(context(), &normalized, &signature);
    std::array<std::uint8_t, 72> output{};
    std::size_t length = output.size();
    if (secp256k1_ecdsa_signature_serialize_der(context(), output.data(), &length, &normalized) != 1) {
        throw std::runtime_error("libsecp256k1 DER serialization failed");
    }
    return {output.begin(), output.begin() + static_cast<std::ptrdiff_t>(length)};
#else
    auto key = key_from_secret(secret_);
    EcdsaSigPtr signature(
        ECDSA_do_sign(message_hash.data(), static_cast<int>(message_hash.size()), key.get()), ECDSA_SIG_free);
    if (!signature) throw std::runtime_error("ECDSA signing failed");
    normalize_low_s(signature.get(), EC_KEY_get0_group(key.get()));
    const int length = i2d_ECDSA_SIG(signature.get(), nullptr);
    if (length <= 0) throw std::runtime_error("Unable to measure DER signature");
    std::vector<std::uint8_t> output(static_cast<std::size_t>(length));
    unsigned char* pointer = output.data();
    if (i2d_ECDSA_SIG(signature.get(), &pointer) != length) {
        throw std::runtime_error("Unable to serialize DER signature");
    }
    return output;
#endif
}

bool verify_ecdsa_der(
    const Hash256& message_hash,
    const std::span<const std::uint8_t> der_signature,
    const std::span<const std::uint8_t> public_key
) {
#ifdef BINARYCOIN_USE_LIBSECP256K1
    secp256k1_pubkey parsed_public{};
    secp256k1_ecdsa_signature parsed_signature{};
    if (secp256k1_ec_pubkey_parse(context(), &parsed_public, public_key.data(), public_key.size()) != 1 ||
        secp256k1_ecdsa_signature_parse_der(
            context(), &parsed_signature, der_signature.data(), der_signature.size()) != 1) return false;
    secp256k1_ecdsa_signature normalized{};
    if (secp256k1_ecdsa_signature_normalize(context(), &normalized, &parsed_signature) != 0) return false;
    return secp256k1_ecdsa_verify(context(), &parsed_signature, message_hash.data(), &parsed_public) == 1;
#else
    try {
        auto key = key_from_public(public_key);
        const unsigned char* pointer = der_signature.data();
        EcdsaSigPtr signature(
            d2i_ECDSA_SIG(nullptr, &pointer, static_cast<long>(der_signature.size())), ECDSA_SIG_free);
        if (!signature || pointer != der_signature.data() + der_signature.size()) return false;
        if (!is_low_s(signature.get(), EC_KEY_get0_group(key.get()))) return false;
        return ECDSA_do_verify(
            message_hash.data(), static_cast<int>(message_hash.size()), signature.get(), key.get()) == 1;
    } catch (...) {
        return false;
    }
#endif
}

bool valid_secret_key(const std::span<const std::uint8_t> secret) {
    try { return backend_valid_secret(secret); } catch (...) { return false; }
}

std::array<std::uint8_t, 32> add_secret_tweak(
    const std::span<const std::uint8_t> secret,
    const std::span<const std::uint8_t> tweak
) {
    if (secret.size() != 32 || tweak.size() != 32 || !valid_secret_key(secret)) {
        throw std::invalid_argument("Invalid secret or tweak length");
    }
    std::array<std::uint8_t, 32> result{};
    std::copy(secret.begin(), secret.end(), result.begin());
#ifdef BINARYCOIN_USE_LIBSECP256K1
    if (secp256k1_ec_seckey_tweak_add(context(), result.data(), tweak.data()) != 1) {
        throw std::runtime_error("BIP32 child tweak produced an invalid key");
    }
#else
    auto key = new_key();
    const EC_GROUP* group = EC_KEY_get0_group(key.get());
    BnPtr parent(BN_bin2bn(secret.data(), 32, nullptr), BN_free);
    BnPtr tweak_bn(BN_bin2bn(tweak.data(), 32, nullptr), BN_free);
    BnPtr order(BN_new(), BN_free);
    BnPtr child(BN_new(), BN_free);
    BnCtxPtr context_value(BN_CTX_new(), BN_CTX_free);
    if (!parent || !tweak_bn || !order || !child || !context_value ||
        EC_GROUP_get_order(group, order.get(), context_value.get()) != 1 ||
        BN_cmp(tweak_bn.get(), order.get()) >= 0 ||
        BN_mod_add(child.get(), parent.get(), tweak_bn.get(), order.get(), context_value.get()) != 1 ||
        BN_is_zero(child.get()) || BN_bn2binpad(child.get(), result.data(), 32) != 32) {
        throw std::runtime_error("BIP32 child tweak produced an invalid key");
    }
#endif
    return result;
}

bool valid_compressed_public_key(const std::span<const std::uint8_t> public_key) {
    if (public_key.size() != 33 || (public_key.front() != 0x02U && public_key.front() != 0x03U)) return false;
#ifdef BINARYCOIN_USE_LIBSECP256K1
    secp256k1_pubkey parsed{};
    return secp256k1_ec_pubkey_parse(context(), &parsed, public_key.data(), public_key.size()) == 1;
#else
    try { (void)key_from_public(public_key); return true; } catch (...) { return false; }
#endif
}

const char* crypto_backend_name() noexcept {
#ifdef BINARYCOIN_USE_LIBSECP256K1
    return "libsecp256k1";
#else
    return "openssl-fallback";
#endif
}

} // namespace bincoin
