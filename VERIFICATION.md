# BinaryCoin v0.1.5 verification

## Consensus checks

The test suite must verify:

1. blocks below height 30 use the original fixed difficulty;
2. block 30 calculates difficulty from blocks 10 through 29;
3. the adjustment is clamped to at most 4x per retarget;
4. non-retarget blocks inherit the previous block's `bits`;
5. headers-first synchronization and complete-chain validation enforce the same rule.

## Linux / Raspberry Pi

Production build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build -j2
```

Fast test build:

```bash
cmake -S . -B build-tests \
  -DCMAKE_BUILD_TYPE=Release \
  -DBINARYCOIN_FAST_TESTS=ON \
  -DBUILD_TESTING=ON
cmake --build build-tests -j2
ctest --test-dir build-tests --output-on-failure
```

A production build must print:

```text
BinaryCoin secp256k1 backend: libsecp256k1
```

## Native Windows x64

GitHub Actions and `build-windows.ps1` create separate production and fast-test
build directories. The production binaries must not contain
`BINARYCOIN_FAST_TESTS`.

The Windows build must:

1. compile `binarycoind.exe` and `binarycoin-cli.exe` with MSVC;
2. pass all Windows-enabled CTest tests;
3. create `dist\BinaryCoin-Testnet-Alpha-v0.1.5-win64.zip`;
4. create the matching SHA-256 file.

## End-to-end fork test

Run upgraded nodes on two machines, synchronize to height 29, mine block 30 on
one machine, and verify the other upgraded node accepts the same block and
reports the same best block hash and chainwork.
