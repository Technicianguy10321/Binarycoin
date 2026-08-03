@echo off
setlocal
cd /d "%~dp0"
binarycoind.exe -testnet -daemonwait
if errorlevel 1 (
  echo.
  echo BinaryCoin failed to start. Run show-logs.ps1 for details.
  pause
  exit /b 1
)
echo BinaryCoin is running in the background.
echo Use node-info.cmd to check the node.
