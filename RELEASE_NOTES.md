# BinaryCoin Testnet Alpha v0.1.5

BinaryCoin v0.1.5 adds automatic peer discovery while remaining wire-compatible
with protocol version 5 and the v0.1.4 testnet chain.

## Peer discovery

- Nodes advertise the local listening address used for each live P2P connection.
- Seed nodes store advertised addresses in `peers.dat` and return them through
  the existing `getaddr` / `addrv2` protocol.
- Connected nodes refresh address data every 10 seconds.
- The outbound manager watches newly learned addresses and dials them without a
  restart or `-addnode`.
- Failed endpoints use bounded retry backoff rather than being retried every
  second forever.
- Self endpoints, unspecified addresses, multicast addresses and link-local
  addresses are not dialed or gossiped.

This allows two nodes that first meet through the BinaryCoin seed to establish a
direct connection automatically when the advertised addresses are mutually
reachable, such as Windows and Linux devices on the same LAN.

## Compatibility

- Consensus rules and the v0.1.4 difficulty activation remain unchanged.
- Genesis block and best-chain format remain unchanged.
- P2P protocol version remains 5.
- v0.1.4 peers can still exchange blocks and transactions, but only v0.1.5
  nodes dynamically dial newly learned addresses.

## Build fixes

- Windows vcpkg detection now supports the exported
  `libsecp256k1::secp256k1` target.
- Added a cross-platform three-node peer-discovery integration test.
