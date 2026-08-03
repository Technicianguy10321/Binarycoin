$ErrorActionPreference = 'Stop'
$Log = Join-Path $env:APPDATA 'BinaryCoin\testnet\debug.log'
if (-not (Test-Path $Log)) {
    Write-Host "No log exists yet: $Log"
    Write-Host 'Start BinaryCoin first with start-binarycoin.cmd.'
    exit 1
}
Get-Content -Path $Log -Tail 100 -Wait
