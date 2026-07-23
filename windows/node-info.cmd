@echo off
setlocal
cd /d "%~dp0"
echo === Blockchain ===
binarycoin-cli.exe -testnet getblockchaininfo
echo.
echo === Network ===
binarycoin-cli.exe -testnet getnetworkinfo
echo.
echo === Peers ===
binarycoin-cli.exe -testnet getpeerinfo
pause
