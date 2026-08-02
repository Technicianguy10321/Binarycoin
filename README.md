# BinaryCoin Testnet Alpha v0.1.4

BinaryCoin is an experimental Bitcoin-style testnet implementation with a persistent blockchain, HD wallet, P2P synchronization, DNS-seed bootstrap, JSON-RPC, `binarycoind`, and `binarycoin-cli`.

## Network

```text
Network:       BinaryCoin Testnet Alpha
P2P port:      26001/TCP
RPC port:      25001/TCP, localhost only by default
Address HRP:   tbin
DNS seed:      binarycoin-testnet.ezgateway.net
```

The testnet genesis and on-disk formats remain compatible with v0.1.2.

## v0.1.4 difficulty hard fork

Dynamic proof-of-work difficulty activates at block **30**. The target block
time is 60 seconds and difficulty retargets every 20 blocks, with each
adjustment clamped to between one quarter and four times the previous target.
The easiest allowed target remains `0x1f00ffff`.

Upgrade every miner and seed node before block 30. v0.1.3 nodes keep enforcing
fixed difficulty and may remain on the pre-fork chain.

## Linux / Raspberry Pi build

Dependencies: CMake, a C++20 compiler, OpenSSL, and libsecp256k1.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Run in the foreground with live console logs:

```bash
./build/binarycoind -testnet
```

Run in the background:

```bash
./build/binarycoind -testnet -daemonwait
./build/binarycoin-cli -testnet getnetworkinfo
./build/binarycoin-cli -testnet getpeerinfo
./build/binarycoin-cli -testnet stop
```

Linux data directory:

```text
~/.binarycoin/testnet
```

## Native Windows x64

See [`README-WINDOWS.md`](README-WINDOWS.md). The short build command is:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\build-windows.ps1
```

This builds and tests native `binarycoind.exe` and `binarycoin-cli.exe`, then produces a portable ZIP under `dist\`.

Windows data directory:

```text
%APPDATA%\BinaryCoin\testnet
```

## Common RPC commands

```text
getblockchaininfo
getnetworkinfo
getpeerinfo
getconnectioncount
getwalletinfo
getbalance
getnewaddress
listunspent
sendtoaddress
generate
getrawmempool
logging
uptime
stop
```

## Logging

Foreground mode prints logs and writes `debug.log`. Daemon mode writes only to `debug.log`.

```bash
binarycoind -testnet -debug=net -debug=rpc
```

## Warning

BinaryCoin Testnet Alpha is experimental software. Testnet coins have no monetary value. Back up wallet recovery words before testing wallet operations.
