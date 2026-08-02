# Third-party notices

BinaryCoin implements BIP32-style hierarchical private-key derivation, a
BIP39-compatible 24-word English mnemonic format, and Bech32 checksums using its
own C++ code and published specification test vectors. The embedded 2,048-word
English list is sourced from the Bitcoin BIPs repository.

OpenSSL is used for SHA-256, HMAC-SHA512, PBKDF2-HMAC-SHA512, and secure random
number generation.

The public Testnet Alpha build requires system `libsecp256k1` for secret-key
validation, public-key generation, ECDSA signing and verification, and BIP32
private-key tweaks.

Boost Multiprecision headers are used for proof-of-work target and chainwork
arithmetic.
