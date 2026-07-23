# BinaryCoin Testnet Alpha v0.1.3

BinaryCoin v0.1.3 is the first source release designed for both Linux and native Windows x64.

## Added

- Native Winsock networking for P2P and JSON-RPC.
- Native Windows `binarycoind.exe` and `binarycoin-cli.exe` build targets.
- Windows data directory at `%APPDATA%\BinaryCoin\testnet`.
- Windows-compatible data-directory locking, process IDs, console shutdown handling and background daemon launch.
- Current-user-only Windows ACL protection for the RPC `.cookie` file.
- Atomic file replacement on Windows for wallet, peer, ban, mempool, branch and chainstate files.
- One-command `build-windows.ps1` build and packaging script using Visual Studio and vcpkg.
- GitHub Actions Windows build/test/package workflow.
- Portable Windows helper scripts for starting, stopping, status and live logs.

## Compatibility

- Testnet magic, genesis, P2P protocol version, DNS seed, ports, chain data and wallet format are unchanged from v0.1.2.
- Linux and Raspberry Pi builds continue to use the same commands and data directory.
- RPC remains bound to `127.0.0.1:25001` by default.
- P2P remains on TCP port `26001`.

## Security defaults

- Production builds require libsecp256k1.
- Windows release packages use static dependencies and the static MSVC runtime.
- RPC is local-only unless explicitly reconfigured.
- Cookie authentication is generated automatically and restricted to the current user.

## Verification status

The source is built and tested on Linux as part of release preparation. The included Windows workflow is the authoritative native Windows build/test path and must pass before distributing its generated ZIP as a verified Windows binary release.
