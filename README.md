# BinaryCoin

[![Status](https://img.shields.io/badge/status-testnet%20alpha-orange)](#project-status)
[![Current release](https://img.shields.io/badge/release-v0.1.5-blue)](#release-history)
[![Protocol](https://img.shields.io/badge/P2P%20protocol-5-blueviolet)](#network-parameters)
[![Language](https://img.shields.io/badge/C%2B%2B-20-00599C)](#building-from-source)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

> **Experimental public testnet software.**
>
> BinaryCoin is not mainnet software, has not received an independent security
> audit, and must not be used for real funds. Testnet BIN has no monetary value.
> The chain, wallet format, network protocol, and consensus rules may change or
> be reset during development.

BinaryCoin is an independent proof-of-work blockchain project written in C++20.
It is built for learning, development, wallet experiments, peer-to-peer
networking, consensus testing, and running small cross-platform test networks.

The project includes:

- `binarycoind` — the BinaryCoin full node, wallet backend, miner, P2P server,
  and JSON-RPC server
- `binarycoin-cli` — the command-line RPC client
- a persistent blockchain and UTXO set
- an HD wallet with 24-word recovery phrases
- transaction creation, signing, validation, and relay
- DNS bootstrap and automatic peer discovery
- Windows x64 and Linux support
- a CMake test suite and GitHub Actions Windows build

BinaryCoin is inspired by some Bitcoin design ideas, but it is a separate
implementation. It is **not compatible with Bitcoin nodes, wallets, addresses,
blocks, mining pools, or transaction formats**.

---

## Contents

- [Project status](#project-status)
- [Features](#features)
- [Network parameters](#network-parameters)
- [Architecture](#architecture)
- [Quick start](#quick-start)
- [Wallet setup](#wallet-setup)
- [Running a node](#running-a-node)
- [Mining](#mining)
- [Transactions](#transactions)
- [Peer discovery](#peer-discovery)
- [RPC commands](#rpc-commands)
- [Building from source](#building-from-source)
- [Testing](#testing)
- [Data directories](#data-directories)
- [Security warning](#security-warning)
- [Known limitations](#known-limitations)
- [Troubleshooting](#troubleshooting)
- [Roadmap](#roadmap)
- [Release history](#release-history)
- [Contributing](#contributing)
- [License](#license)

---

## Project status

| Item | Status |
|---|---|
| Current software | BinaryCoin Testnet Alpha v0.1.5 |
| Network | Experimental public testnet |
| Mainnet | Not available |
| Real-value usage | Not supported |
| P2P protocol | Version 5 |
| Consensus | SHA-256d proof of work |
| Linux ARM64 | Supported |
| Linux x86-64 | Supported from source |
| Windows x64 | Supported |
| Wallet encryption | Not implemented |
| External miner interface | Not implemented |
| Independent security audit | Not completed |

v0.1.5 is suitable for controlled public testnet experiments. It is not
production-ready cryptocurrency software.

---

## Features

### Blockchain and consensus

- SHA-256d proof of work
- 80-byte block headers
- persistent append-only block storage
- checksummed block records
- persistent block index and UTXO snapshots
- accumulated-chainwork best-chain selection
- competing branch storage
- live reorganizations when a competing branch has strictly greater work
- coinbase rewards and transaction fees
- 100-block coinbase maturity
- 50 BIN initial block subsidy
- subsidy halving every 210,000 blocks
- maximum monetary range of 21,000,000 BIN
- 60-second target block spacing
- dynamic difficulty beginning at block 30
- difficulty retarget every 20 blocks
- retarget adjustment clamped between 0.25× and 4× per interval

### Wallet and transactions

- HD wallet backend
- 24-word recovery phrase
- BIP32-style deterministic key derivation
- compressed secp256k1 public keys
- P2PK locking and spending scripts
- `tbin1` Bech32-PK testnet addresses
- transaction signing and verification
- wallet balance and immature-balance reporting
- UTXO selection
- fee estimation
- local mempool
- double-spend checks
- transaction relay between peers

### Networking

- BinaryCoin P2P protocol version 5
- DNS-seed bootstrap
- persistent peer database
- `getaddr` and `addrv2` address exchange
- automatic dialing of newly discovered reachable peers
- manual `-addnode` support for diagnostics
- headers-first synchronization
- missing-block download
- block inventory and relay
- transaction inventory and relay
- inbound and outbound connection limits
- self-connection and duplicate-peer rejection
- malformed-message handling
- rate limiting
- persistent peer bans
- ping/pong timeout handling

### Node and developer tooling

- native `binarycoind` daemon
- native `binarycoin-cli` client
- authenticated localhost JSON-RPC
- per-user RPC cookie
- foreground and background operation
- category-based logging
- chain verification and reindex commands
- Linux and Windows builds
- CMake and CTest
- GitHub Actions Windows workflow
- production `libsecp256k1` backend
- test-only OpenSSL secp256k1 fallback

---

## Network parameters

| Parameter | Value |
|---|---|
| Network name | BinaryCoin Testnet Alpha |
| Coin symbol | BIN |
| Smallest unit | 0.00000001 BIN |
| Address HRP | `tbin` |
| Address appearance | `tbin1...` |
| P2P port | `26001/TCP` |
| RPC port | `25001/TCP` |
| Default RPC bind | `127.0.0.1` |
| DNS seed | `binarycoin-testnet.ezgateway.net` |
| Message start | `TBIT` |
| P2P protocol version | `5` |
| Target block spacing | 60 seconds |
| Difficulty activation | Block 30 |
| Retarget interval | 20 blocks |
| Easiest target bits | `1f00ffff` |
| Initial subsidy | 50 BIN |
| Halving interval | 210,000 blocks |
| Coinbase maturity | 100 blocks |
| Maximum monetary range | 21,000,000 BIN |

A target spacing of 60 seconds is an **average**, not a countdown. Proof-of-work
is random: one block may take seconds while another may take several minutes.
Difficulty attempts to move the long-term average toward one block per minute.

### Genesis block

```text
Hash:
00007e7bc2b6d593d9bacb54cd32bacaf9a99ff86adc9ac6fbe36134fff1c372

Merkle root:
2cf10845810fce4b828950111a45d66e33d8649db1265f088cf4607789ae1442

Genesis transaction:
2cf10845810fce4b828950111a45d66e33d8649db1265f088cf4607789ae1442

Bits:
1f00ffff
```

The genesis reward is not added to the spendable UTXO set.

---

## Architecture

A normal BinaryCoin node contains several connected parts:

```text
                        ┌─────────────────────┐
                        │   binarycoin-cli    │
                        └──────────┬──────────┘
                                   │ JSON-RPC
                                   │ 127.0.0.1:25001
                        ┌──────────▼──────────┐
                        │    binarycoind      │
                        ├─────────────────────┤
                        │ RPC command layer   │
                        │ Wallet + keys       │
                        │ Mempool             │
                        │ Chain validation    │
                        │ UTXO set            │
                        │ Block storage       │
                        │ P2P networking      │
                        │ Peer discovery      │
                        └──────────┬──────────┘
                                   │ P2P protocol v5
                                   │ TCP 26001
                   ┌───────────────┼────────────────┐
                   ▼               ▼                ▼
               Seed node        Linux peer      Windows peer
```

### Main source areas

| Area | Important files |
|---|---|
| Node startup | `src/main.cpp`, `src/node_app.cpp` |
| RPC | `src/rpc.cpp`, `src/rpc_commands.cpp` |
| Chain and consensus | `src/chain.cpp`, `src/block.cpp`, `src/pow.cpp` |
| Transactions | `src/transaction.cpp`, `src/script.cpp`, `src/fees.cpp` |
| Wallet | `src/wallet_manager.cpp`, `src/hd_wallet_backend.cpp` |
| Keys and addresses | `src/key.cpp`, `src/hdkey.cpp`, `src/mnemonic.cpp`, `src/bech32.cpp` |
| Storage | `src/storage.cpp`, `src/utxo.cpp`, `src/branch_store.cpp` |
| P2P networking | `src/net.cpp`, `src/bootstrap.cpp` |
| Peer management | `src/peer_store.cpp`, `src/ban_store.cpp` |
| Tests | `tests/` |

---

# Quick start

## Windows x64

### Normal user

1. Download the official Windows ZIP and `SHA256SUMS.txt`.
2. Verify the ZIP checksum.
3. Extract the ZIP completely.
4. Open PowerShell inside the extracted folder.
5. Create a wallet before starting the node.
6. Start the node.
7. Use a second PowerShell window for CLI commands.

Verify the ZIP:

```powershell
Get-FileHash .\BinaryCoin-Testnet-Alpha-v0.1.5-win64.zip -Algorithm SHA256
```

Create the wallet:

```powershell
.\binarycoind.exe -testnet wallet-create
```

Start the node:

```powershell
.\binarycoind.exe -testnet -printtoconsole
```

In a second PowerShell window:

```powershell
.\binarycoin-cli.exe -testnet getblockchaininfo
.\binarycoin-cli.exe -testnet getnetworkinfo
.\binarycoin-cli.exe -testnet getpeerinfo
.\binarycoin-cli.exe -testnet getwalletinfo
```

Stop cleanly:

```powershell
.\binarycoin-cli.exe -testnet stop
```

The release ZIP may also include:

```text
start-binarycoin.cmd
stop-binarycoin.cmd
node-info.cmd
show-logs.ps1
```

See [`README-WINDOWS.md`](README-WINDOWS.md) for the dedicated Windows guide.

## Linux and Raspberry Pi

Install build dependencies on Debian, Ubuntu, or Raspberry Pi OS:

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  pkg-config \
  libssl-dev \
  libsecp256k1-dev
```

Clone and compile:

```bash
git clone https://github.com/Technicianguy10321/Binarycoin.git
cd Binarycoin

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Create a wallet:

```bash
./build/binarycoind -testnet wallet-create
```

Start the node:

```bash
./build/binarycoind -testnet -printtoconsole
```

In another terminal:

```bash
./build/binarycoin-cli -testnet getblockchaininfo
./build/binarycoin-cli -testnet getnetworkinfo
./build/binarycoin-cli -testnet getpeerinfo
./build/binarycoin-cli -testnet getwalletinfo
```

Stop cleanly:

```bash
./build/binarycoin-cli -testnet stop
```

---

## Wallet setup

### Create a new wallet

The daemon must not already be using the wallet file.

Linux:

```bash
./build/binarycoind -testnet wallet-create
```

Windows:

```powershell
.\binarycoind.exe -testnet wallet-create
```

The command prints:

- wallet path
- wallet backend
- cryptographic backend
- first receiving address
- a 24-word import phrase

Store the phrase privately. Never paste it into chat, an issue report, Discord,
a screenshot, a release archive, or a public repository.

`wallet-create` is a direct `binarycoind` command. It is **not** an RPC method.
This is incorrect:

```bash
./build/binarycoin-cli -testnet wallet-create
```

It will return:

```text
Method not found: wallet-create
```

### Import a wallet

Linux:

```bash
./build/binarycoind -testnet wallet-import
```

Windows:

```powershell
.\binarycoind.exe -testnet wallet-import
```

Enter the complete 24-word BinaryCoin import phrase when prompted.

### Query the loaded wallet

```bash
./build/binarycoin-cli -testnet getwalletinfo
./build/binarycoin-cli -testnet getbalance
./build/binarycoin-cli -testnet getnewaddress
./build/binarycoin-cli -testnet listunspent
```

---

## Running a node

### Foreground mode

```bash
./build/binarycoind -testnet -printtoconsole
```

Foreground mode is best for development because logs remain visible.

### Background mode

```bash
./build/binarycoind -testnet -daemonwait
```

Check it:

```bash
./build/binarycoin-cli -testnet uptime
./build/binarycoin-cli -testnet getnetworkinfo
```

Stop it:

```bash
./build/binarycoin-cli -testnet stop
```

### Debug logging

```bash
./build/binarycoind \
  -testnet \
  -printtoconsole \
  -debug=net \
  -debug=rpc \
  -debug=addrman \
  -debug=validation \
  -debug=wallet
```

Available documented categories include:

```text
net
rpc
addrman
validation
wallet
```

### Public node

BinaryCoin listens on P2P port `26001/TCP`.

To accept peers from the internet:

1. allow TCP `26001` through the host firewall;
2. forward TCP `26001` from the router to the node;
3. keep the node running;
4. do **not** expose RPC port `25001`.

A seed-only node may use:

```bash
./build/binarycoind \
  -testnet \
  -printtoconsole \
  --max-outbound 0
```

A normal client should keep outbound peers enabled.

### Useful server options

```text
-daemon
-daemonwait
-datadir=<directory>
-bind=<address>
-port=<port>
-rpcbind=<address>
-rpcport=<port>
-debug=<category>
-printtoconsole
-addnode=<host:port>
--max-inbound <number>
--max-outbound <number>
--ban-seconds <number>
```

RPC should remain bound to `127.0.0.1`.

---

## Mining

### Mine to the loaded wallet

```bash
./build/binarycoin-cli -testnet generate 1
```

Mine a small batch:

```bash
./build/binarycoin-cli -testnet generate 5
```

### Mine to a specified address

```bash
./build/binarycoin-cli -testnet generatetoaddress 1 "tbin1..."
```

### Mining information

```bash
./build/binarycoin-cli -testnet getmininginfo
```

Important fields include:

```text
blocks
difficulty
difficultyforkheight
nextblockbits
nextblockheight
nextretargetheight
retargetinterval
targetspacing
```

### Important v0.1.5 mining behavior

The current `generate` implementation is synchronous. A large request such as:

```bash
./build/binarycoin-cli -testnet generate 100
```

can keep the node's RPC data lock busy for a long time. Other CLI requests may
appear to hang until mining finishes.

The CLI process can also be interrupted while the daemon is still completing
the request. This can make the node look frozen even when the blockchain is not
corrupted.

For v0.1.5, use small batches:

```bash
./build/binarycoin-cli -testnet generate 1
./build/binarycoin-cli -testnet generate 5
./build/binarycoin-cli -testnet generate 10
```

The current built-in miner may use only part of a multi-core CPU. Background
multithreaded mining and external miner interfaces are planned, not currently
implemented.

### Coinbase maturity

A mined reward becomes spendable after 100 blocks. Before maturity it appears
as an immature wallet balance.

---

## Transactions

Generate a receiving address:

```bash
./build/binarycoin-cli -testnet getnewaddress
```

Validate an address:

```bash
./build/binarycoin-cli -testnet validateaddress "tbin1..."
```

Send BIN:

```bash
./build/binarycoin-cli -testnet sendtoaddress "tbin1..." 1.25000000
```

Check the mempool:

```bash
./build/binarycoin-cli -testnet getrawmempool
./build/binarycoin-cli -testnet getmempoolinfo
```

Mine a block to confirm pending transactions:

```bash
./build/binarycoin-cli -testnet generate 1
```

All BIN on this network is testnet currency and has no monetary value.

---

## Peer discovery

BinaryCoin v0.1.5 introduces automatic peer discovery.

A normal startup flow is:

```text
1. Resolve the compiled DNS seed
2. Connect to a reachable seed or saved peer
3. Complete the BinaryCoin protocol handshake
4. Advertise a reachable listening address
5. Exchange addresses through getaddr / addrv2
6. Save learned peers in peers.dat
7. Dynamically dial newly discovered peers
```

The compiled bootstrap endpoint is:

```text
binarycoin-testnet.ezgateway.net:26001
```

No private-LAN `-addnode` should be needed when discovered addresses are
mutually reachable.

Inspect peers:

```bash
./build/binarycoin-cli -testnet getpeerinfo
```

Inspect general network status:

```bash
./build/binarycoin-cli -testnet getnetworkinfo
```

A healthy peer should show protocol version `5` and a subversion similar to:

```text
/BinaryCoinTestnetAlpha:0.1.5/
```

Automatic discovery does not provide NAT traversal or hole punching. Nodes
behind separate routers generally need a reachable public endpoint to accept
inbound internet connections.

---

## RPC commands

Run:

```bash
./build/binarycoin-cli -testnet help
```

### Blockchain

```text
getbestblockhash
getblock "blockhash" ( verbosity )
getblockchaininfo
getblockcount
getblockhash height
```

### Control

```text
help ( "command" )
logging ( [include,...] [exclude,...] )
stop
uptime
```

### Generating

```text
generate nblocks
generatetoaddress nblocks "address"
getmininginfo
```

### Mempool

```text
getmempoolinfo
getrawmempool
estimatesmartfee conf_target ( "economical"|"conservative" )
```

### Network

```text
getconnectioncount
getnetworkinfo
getpeerinfo
```

### Wallet

```text
getbalance
getnewaddress
getwalletinfo
listunspent
sendtoaddress "address" amount
validateaddress "address"
```

Some accepted optional arguments are placeholders or only partially implemented
in v0.1.5. Check actual behavior before building software that depends on them.

---

## Building from source

### Requirements

- CMake 3.20 or newer
- C++20 compiler
- OpenSSL
- `libsecp256k1`
- Git
- optional: Ninja

Production builds require `libsecp256k1`. The OpenSSL signing fallback is for
tests only.

### Debian, Ubuntu, Raspberry Pi OS

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  pkg-config \
  libssl-dev \
  libsecp256k1-dev
```

Build:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBINARYCOIN_REQUIRE_LIBSECP256K1=ON

cmake --build build -j"$(nproc)"
```

Verify:

```bash
./build/binarycoind --version
./build/binarycoin-cli --version
```

### Clean rebuild

```bash
rm -rf build

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBINARYCOIN_REQUIRE_LIBSECP256K1=ON

cmake --build build -j"$(nproc)"
```

### Windows source build

Requirements:

- Windows 10 or Windows 11 x64
- Visual Studio 2022 Build Tools
- Desktop development with C++
- CMake 3.25 or newer
- Git
- PowerShell 5.1 or newer

Run:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\build-windows.ps1
```

The script bootstraps vcpkg, builds dependencies, compiles the executables, runs
tests, and creates a portable ZIP under `dist\`.

---

## Testing

### Standard test build

```bash
cmake -S . -B build-tests \
  -DCMAKE_BUILD_TYPE=Release \
  -DBINARYCOIN_FAST_TESTS=ON \
  -DBUILD_TESTING=ON \
  -DBINARYCOIN_REQUIRE_LIBSECP256K1=OFF

cmake --build build-tests -j"$(nproc)"
ctest --test-dir build-tests --output-on-failure
```

The source tree includes tests for:

- chain and wallet behavior
- RPC behavior
- DNS bootstrap
- automatic peer discovery
- network hardening
- synchronization and reorganization scenarios

The fast-test proof-of-work target must never be used for a production public
node build.

---

## Data directories

### Linux

```text
~/.binarycoin/testnet
```

### Windows

```text
%APPDATA%\BinaryCoin\testnet
```

Important files may include:

```text
wallet-v2.dat
peers.dat
debug.log
.cookie
```

The directory also contains blockchain, UTXO, mempool, branch, index, and
storage data. Internal filenames and formats may change during alpha releases.

### Custom data directory

```bash
./build/binarycoind -testnet -datadir=/path/to/binarycoin-data
```

Use the same `-datadir` option with `binarycoin-cli` when needed.

### Backup

Stop the daemon cleanly before copying the whole data directory:

```bash
./build/binarycoin-cli -testnet stop
cp -a "$HOME/.binarycoin/testnet" "$HOME/binarycoin-testnet-backup"
```

The 24-word recovery phrase is the critical wallet recovery material. A data
directory backup also preserves chain and peer state.

---

# Security warning

## Experimental and unaudited

BinaryCoin Testnet Alpha has not received an independent security audit. Bugs
may cause:

- wallet loss
- blockchain resets
- synchronization failures
- crashes
- reorganizations
- incompatible upgrades
- corrupted test data
- loss of unconfirmed transactions

Never use BinaryCoin for real funds.

## Wallet secrets are stored without encryption

The current `wallet-v2.dat` stores HD-wallet recovery entropy in plaintext.
Operating-system file permissions restrict ordinary access, but they do not
protect the wallet from:

- malware running under the same account
- administrator or root access
- physical disk access
- copied or cloud-synchronized backups
- accidental uploads
- compromised development machines
- another process reading the data directory

Anyone who obtains the wallet file may be able to recover the private keys and
spend the wallet's testnet coins.

Never:

- share `wallet-v2.dat`
- publish the data directory
- commit wallet files to Git
- include a wallet in a release archive
- post recovery words or private keys
- keep secrets visible in screenshots
- reuse a BinaryCoin phrase in another cryptocurrency wallet

Wallet encryption, passphrase protection, wallet locking, and safer secret
handling are planned for a future release. These protections do not exist until
they are implemented and tested.

## RPC security

RPC defaults to:

```text
127.0.0.1:25001
```

Do not change it to a public bind address. Do not forward TCP port `25001`
through a router.

Public nodes should expose only:

```text
TCP 26001 — BinaryCoin P2P
```

## Network privacy

BinaryCoin P2P traffic is not encrypted. Peers can learn network addresses, and
v0.1.5 distributes reachable addresses through peer discovery. BinaryCoin does
not provide anonymity.

## Low-hashrate security

A small testnet can be controlled by one miner. A majority miner may reorganize
recent blocks, delay transactions, or reverse their own payments.

A majority miner cannot create signatures for keys they do not possess or make
other nodes accept blocks that violate consensus rules.

Do not treat BinaryCoin testnet confirmations as financially secure.

---

## Known limitations

The following limitations apply to v0.1.5:

- the wallet file is not encrypted;
- `generate` is synchronous and may block other RPC calls;
- a large mining batch is saved after the complete batch finishes;
- mining does not have a clean background cancellation interface;
- built-in mining may not use all CPU cores;
- `getblocktemplate` is not implemented;
- `submitblock` is not implemented;
- `setgenerate` is not implemented;
- `getnetworkhashps` is not implemented;
- some RPC statistics are placeholders;
- some optional RPC parameters are accepted but ignored;
- block timestamps are required to increase, but stronger median-time-past and
  maximum-future-time rules are not yet implemented;
- P2P traffic is unencrypted;
- peer discovery does not perform NAT traversal;
- the network currently depends heavily on a small number of reachable nodes;
- no mainnet is available;
- chain resets remain possible during alpha development.

These limitations are part of the reason BinaryCoin must remain a testnet alpha.

---

## Troubleshooting

### `wallet-create` says method not found

Do not run it through `binarycoin-cli`.

Correct:

```bash
./build/binarycoind -testnet wallet-create
```

Incorrect:

```bash
./build/binarycoin-cli -testnet wallet-create
```

### No wallet is loaded

Stop the daemon, create or import the wallet, then restart:

```bash
./build/binarycoin-cli -testnet stop
./build/binarycoind -testnet wallet-create
./build/binarycoind -testnet -printtoconsole
```

### CLI hangs after a large `generate` request

The daemon may still be mining synchronously. Check the process:

```bash
pgrep -af binarycoind
ps -o pid,stat,%cpu,etime,cmd -C binarycoind
```

Use small mining batches after restarting.

Try a normal shutdown first:

```bash
kill -TERM "$(pgrep -xo binarycoind)"
```

Use `kill -KILL` only when the normal signal fails. A hard kill prevents a clean
shutdown.

### Node exits immediately

Check whether another node is already running:

```bash
pgrep -af binarycoind
```

Read the log:

```bash
tail -n 100 "$HOME/.binarycoin/testnet/debug.log"
```

### No peers

Check DNS:

```bash
getent hosts binarycoin-testnet.ezgateway.net
```

Check TCP reachability:

```bash
nc -vz binarycoin-testnet.ezgateway.net 26001
```

Inspect peers:

```bash
./build/binarycoin-cli -testnet getpeerinfo
```

### Port already in use

```bash
ss -ltnp | grep -E ':26001|:25001'
```

Expected defaults:

```text
0.0.0.0:26001    P2P
127.0.0.1:25001  RPC
```

### Reindex after storage trouble

Back up the data directory first, stop the daemon, then run:

```bash
./build/binarycoind -testnet reindex
```

Do not delete the wallet or whole data directory as the first troubleshooting
step.

### Check logs

Linux:

```bash
tail -f "$HOME/.binarycoin/testnet/debug.log"
```

Windows:

```powershell
Get-Content "$env:APPDATA\BinaryCoin\testnet\debug.log" -Wait
```

---

## Roadmap

Planned work for later releases includes:

### Mining and RPC

- asynchronous background mining
- configurable CPU worker threads
- safe mining cancellation
- save each accepted block without waiting for an entire large batch
- `setgenerate`
- `getblocktemplate`
- `submitblock`
- `getnetworkhashps`
- accurate local and network hash statistics
- external miner compatibility
- possible Stratum support after template submission is stable

### Wallet security

- encrypted wallet file
- passphrase-based key protection
- explicit wallet lock and unlock
- safer secret-memory handling
- improved backup and recovery workflow
- independent wallet-format review

### Consensus hardening

- median-time-past validation
- maximum-future-time validation
- explicit consensus block size or block weight rule
- improved timestamp and retarget tests
- long-running multi-node reorganization tests

### Networking and observability

- accurate peer byte counters
- accurate send and receive timestamps
- improved synchronization progress
- more seed nodes
- better network diagnostics
- improved peer-address privacy controls

Roadmap items are plans, not promises, and are not implemented until merged,
tested, and included in a release.

---

## Release history

### v0.1.5 — automatic peer discovery

- advertises reachable local listening addresses;
- exchanges addresses through `getaddr` and `addrv2`;
- dynamically dials newly learned peers;
- stores peer addresses persistently;
- adds bounded retry backoff;
- filters invalid, self, multicast, and link-local endpoints;
- fixes Windows vcpkg `libsecp256k1::secp256k1` detection;
- adds a cross-platform three-node discovery test;
- remains compatible with protocol version 5 and the v0.1.4 chain.

### v0.1.4 — dynamic difficulty

- activates dynamic proof-of-work difficulty at block 30;
- targets 60 seconds per block;
- retargets every 20 blocks;
- clamps each retarget to a maximum 4× increase or 4× decrease;
- requires upgraded miners and nodes after the activation height.

### v0.1.3 — Windows port

- adds native Windows x64 support;
- adds Windows packaging and helper scripts;
- adds vcpkg-based dependency builds;
- provides portable `binarycoind.exe` and `binarycoin-cli.exe`.

Older alpha versions should not be used for the current public testnet unless
their compatibility is explicitly documented.

---

## Release packages

A normal v0.1.5 release may contain:

```text
BinaryCoin-Testnet-Alpha-v0.1.5-win64.zip
BinaryCoin-Testnet-Alpha-v0.1.5-linux-arm64.tar.gz
BinaryCoin-Testnet-Alpha-v0.1.5-source.tar.gz
SHA256SUMS.txt
```

Verify release hashes before execution.

Linux:

```bash
sha256sum -c SHA256SUMS.txt
```

Windows:

```powershell
Get-FileHash .\BinaryCoin-Testnet-Alpha-v0.1.5-win64.zip -Algorithm SHA256
```

Unsigned experimental binaries may trigger operating-system warnings. Do not
disable security software globally.

---

## Contributing

Contributions, bug reports, test results, and documentation improvements are
welcome.

Before submitting code:

```bash
cmake -S . -B build-tests \
  -DCMAKE_BUILD_TYPE=Release \
  -DBINARYCOIN_FAST_TESTS=ON \
  -DBUILD_TESTING=ON \
  -DBINARYCOIN_REQUIRE_LIBSECP256K1=OFF

cmake --build build-tests -j"$(nproc)"
ctest --test-dir build-tests --output-on-failure
```

Bug reports should include:

- operating system and architecture;
- BinaryCoin version;
- exact command;
- expected behavior;
- actual behavior;
- relevant `debug.log` lines;
- whether the issue reproduces with a clean test data directory.

Never attach:

- recovery phrases;
- private keys;
- `wallet-v2.dat`;
- RPC cookies;
- private personal data.

Large consensus changes should include tests and clearly state whether they
require a hard fork, soft fork, data migration, or network reset.

---

## Responsible disclosure

Do not publish a working exploit, wallet-stealing method, or network-crashing
payload in a public issue before maintainers have had a reasonable opportunity
to investigate.

A security report should explain:

- affected version;
- affected component;
- reproduction conditions;
- expected impact;
- whether private keys, consensus, storage, RPC, or networking are affected;
- a suggested fix when available.

Never include real private keys or recovery phrases in a report.

---

## License

BinaryCoin is released under the MIT License. See [`LICENSE`](LICENSE).

Third-party components and notices are documented in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

---

## Final warning

```text
BinaryCoin Testnet Alpha is experimental software.
Testnet BIN has no monetary value.
Do not use it for real funds.
Do not share wallet files, private keys, or recovery phrases.
Keep RPC port 25001 restricted to localhost.
Expect bugs, incompatible upgrades, and possible chain resets.
```
