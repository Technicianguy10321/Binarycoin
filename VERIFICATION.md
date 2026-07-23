# BinaryCoin v0.1.3 verification

## Linux / Raspberry Pi

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

A production build must print:

```text
BinaryCoin secp256k1 backend: libsecp256k1
```

## Native Windows x64

From PowerShell on Windows 10 or 11 with Visual Studio 2022 C++ Build Tools, CMake and Git:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\build-windows.ps1
```

The script must:

1. build `binarycoind.exe` and `binarycoin-cli.exe`;
2. pass all Windows-enabled CTest tests;
3. create `dist\BinaryCoin-Testnet-Alpha-v0.1.3-win64.zip`;
4. create its SHA-256 file.

## End-to-end Windows seed test

Use a fresh Windows data directory and a network outside the seed node's home LAN:

```bat
binarycoind.exe -testnet -daemonwait
binarycoin-cli.exe -testnet getnetworkinfo
binarycoin-cli.exe -testnet getpeerinfo
binarycoin-cli.exe -testnet getblockchaininfo
binarycoin-cli.exe -testnet stop
```

`getpeerinfo` should show an outbound peer reached through the compiled seed `binarycoin-testnet.ezgateway.net:26001` without manually supplying the Pi address.
