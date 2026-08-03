# BinaryCoin Testnet Alpha v0.1.4

BinaryCoin v0.1.4 is a scheduled consensus hard fork that activates dynamic
proof-of-work difficulty at block **30**. This release is not assigned a BCIP
number.

## Consensus hard fork

- Activation height: **30**
- Pre-fork difficulty: fixed `0x1f00ffff`
- Target block interval: **60 seconds**
- Retarget interval: **20 blocks**
- First measurement window: blocks **10 through 29**
- First expected timespan: **1,140 seconds** (19 timestamp gaps)
- Adjustment clamp: no more than **4x harder** or **4x easier** per retarget
- Easiest permitted target: `0x1f00ffff`
- Later retarget heights: **50, 70, 90, 110, ...**

Between retarget boundaries, blocks inherit the previous block's difficulty.
At a retarget boundary, the previous target is multiplied by the clamped
actual-timespan/expected-timespan ratio and encoded back into compact `bits`.

## Compatibility

- Blocks 0 through 29 remain compatible with v0.1.3.
- v0.1.3 nodes enforce fixed difficulty forever and can reject the v0.1.4
  chain once a post-fork block uses retargeted difficulty.
- P2P protocol remains version 5 because the wire message format is unchanged.
- Testnet genesis, magic bytes, ports, DNS seed, address format, chain storage,
  wallet format and RPC interface are unchanged.

## Build and reliability fixes carried forward

- Native Windows x64 MSVC production build and separate fast-test build.
- Windows-safe atomic file replacement and blocking chainstate lock behavior.
- Stoppable Windows P2P and RPC listener loops.
- Reliable outbound reconnect behavior on Windows.
- Linux, Raspberry Pi and Windows builds continue to require libsecp256k1 for
  production use.

## Upgrade rule

Upgrade all mining and seed nodes before mining block 30. Keep a backup of the
testnet data directory and wallet recovery phrase before replacing binaries.
