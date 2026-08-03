# Upgrading to BinaryCoin Testnet Alpha v0.1.5

v0.1.5 is a networking upgrade and does not restart the testnet chain.

1. Stop `binarycoind` cleanly.
2. Replace the v0.1.4 executables with v0.1.5.
3. Keep the existing testnet data directory, wallet, chain and `peers.dat`.
4. Start normally without private-LAN `-addnode` entries.
5. Enable `-debug=net -debug=addrman` during the first discovery test.

A seed-only node may continue to use `--max-outbound 0`. Normal client nodes
must not use that option, because it disables all outbound discovery.
