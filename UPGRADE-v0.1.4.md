# Upgrading to BinaryCoin Testnet Alpha v0.1.4

v0.1.4 activates dynamic proof-of-work difficulty at block 30.

## Before upgrading

1. Stop `binarycoind` cleanly.
2. Back up the testnet data directory and the wallet recovery phrase.
3. Do not delete the existing chain or wallet when replacing binaries.

Linux data directory:

```text
~/.binarycoin/testnet
```

Windows data directory:

```text
%APPDATA%\BinaryCoin\testnet
```

## Activation

- Blocks 0-29: original fixed difficulty
- Block 30: first retarget
- Later retargets: 50, 70, 90, ...

All seed nodes and miners should run v0.1.4 before block 30.

## Verify the upgraded node

```bash
binarycoin-cli -testnet getnetworkinfo
binarycoin-cli -testnet getblockchaininfo
binarycoin-cli -testnet getmininginfo
```

`getmininginfo` reports `nextblockbits`, `difficultyforkheight`,
`retargetinterval`, `targetspacing`, and `nextretargetheight`.
