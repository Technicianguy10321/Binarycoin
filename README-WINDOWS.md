# BinaryCoin Testnet Alpha v0.1.5 — Windows x64

## For normal users

1. Extract the Windows ZIP completely.
2. Double-click `start-binarycoin.cmd`.
3. Windows Defender Firewall may ask about network access. Allow **Private networks** if you want inbound LAN peers. Outbound seed connections work without opening a router port.
4. v0.1.5 automatically advertises the Windows LAN listening address to connected peers and dynamically dials addresses learned through the seed. Do not add a private `-addnode` during the discovery test.
4. Double-click `node-info.cmd` to see chain and peer status.
5. Run `stop-binarycoin.cmd` before shutting down or moving the data directory.

The node connects to the compiled testnet seed:

```text
binarycoin-testnet.ezgateway.net:26001
```

The Windows data directory is:

```text
%APPDATA%\BinaryCoin\testnet
```

RPC listens only on `127.0.0.1:25001` by default and authenticates with the per-user `.cookie` file.

## Console commands

Run these from Command Prompt inside the extracted folder:

```bat
binarycoind.exe -testnet
binarycoind.exe -testnet -daemonwait
binarycoin-cli.exe -testnet getblockchaininfo
binarycoin-cli.exe -testnet getpeerinfo
binarycoin-cli.exe -testnet getnewaddress
binarycoin-cli.exe -testnet stop
```

Foreground mode shows logs in the terminal. Daemon mode writes them to:

```text
%APPDATA%\BinaryCoin\testnet\debug.log
```

## Build the Windows package from source

Requirements:

- Windows 10 or 11 x64
- Visual Studio 2022 Build Tools with **Desktop development with C++**
- CMake 3.25 or newer
- Git
- PowerShell 5.1 or newer

From PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\build-windows.ps1
```

The script bootstraps vcpkg, builds static OpenSSL and libsecp256k1 dependencies, compiles both executables, runs the tests, and creates:

```text
dist\BinaryCoin-Testnet-Alpha-v0.1.5-win64.zip
```
