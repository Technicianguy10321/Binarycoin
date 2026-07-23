@echo off
setlocal
cd /d "%~dp0"
binarycoin-cli.exe -testnet stop
if errorlevel 1 (
  echo BinaryCoin may not be running. Check %%APPDATA%%\BinaryCoin\testnet\debug.log
  exit /b 1
)
