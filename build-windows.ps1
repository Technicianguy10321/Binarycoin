[CmdletBinding()]
param(
    [switch]$SkipTests,
    [switch]$KeepBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

function Require-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command '$Name' was not found. Install Visual Studio C++ Build Tools, CMake, and Git."
    }
}

Require-Command cmake
Require-Command git

if (-not $env:VCPKG_ROOT) {
    $env:VCPKG_ROOT = 'C:\vcpkg'
}

if (-not (Test-Path (Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'))) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $env:VCPKG_ROOT) | Out-Null
    Write-Host 'Downloading vcpkg...'
    git clone --depth 1 https://github.com/microsoft/vcpkg.git $env:VCPKG_ROOT
    & (Join-Path $env:VCPKG_ROOT 'bootstrap-vcpkg.bat') -disableMetrics
}

$ReleaseBuild = Join-Path $Root 'build\windows-x64-release'
$TestBuild = Join-Path $Root 'build\windows-x64-tests'
if (-not $KeepBuild) {
    Remove-Item -Recurse -Force $ReleaseBuild -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $TestBuild -ErrorAction SilentlyContinue
}

Write-Host 'Configuring BinaryCoin production release...'
cmake --preset windows-x64-release

Write-Host 'Building production binarycoind.exe and binarycoin-cli.exe...'
cmake --build --preset windows-x64-release

if (-not $SkipTests) {
    Write-Host 'Configuring fast consensus tests...'
    cmake --preset windows-x64-tests

    Write-Host 'Building test binaries...'
    cmake --build --preset windows-x64-tests

    Write-Host 'Running Windows tests...'
    ctest --preset windows-x64-tests
}

$Stage = Join-Path $Root 'dist\BinaryCoin-Testnet-Alpha-v0.1.5-win64'
$Zip = Join-Path $Root 'dist\BinaryCoin-Testnet-Alpha-v0.1.5-win64.zip'
Remove-Item -Recurse -Force $Stage -ErrorAction SilentlyContinue
Remove-Item -Force $Zip -ErrorAction SilentlyContinue
Remove-Item -Force "$Zip.sha256" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $Stage | Out-Null

Copy-Item (Join-Path $ReleaseBuild 'binarycoind.exe') $Stage
Copy-Item (Join-Path $ReleaseBuild 'binarycoin-cli.exe') $Stage
Copy-Item (Join-Path $Root 'windows\start-binarycoin.cmd') $Stage
Copy-Item (Join-Path $Root 'windows\stop-binarycoin.cmd') $Stage
Copy-Item (Join-Path $Root 'windows\node-info.cmd') $Stage
Copy-Item (Join-Path $Root 'windows\show-logs.ps1') $Stage
Copy-Item (Join-Path $Root 'README-WINDOWS.md') $Stage
Copy-Item (Join-Path $Root 'LICENSE') $Stage

Compress-Archive -Path (Join-Path $Stage '*') -DestinationPath $Zip -CompressionLevel Optimal
$Hash = (Get-FileHash -Algorithm SHA256 $Zip).Hash.ToLowerInvariant()
Set-Content -Encoding ascii -NoNewline -Path "$Zip.sha256" -Value "$Hash  $(Split-Path -Leaf $Zip)`n"

Write-Host ''
Write-Host 'Windows package created:'
Write-Host "  $Zip"
Write-Host "SHA-256: $Hash"
