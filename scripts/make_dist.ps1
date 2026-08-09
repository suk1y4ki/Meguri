# Distribution build: Release build, tests, then install minimal files to dist/Meguri.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\make_dist.ps1
param(
    [string]$DistDir = "dist\Meguri"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

cmake --preset vs2022
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

cmake --build --preset build-release
if ($LASTEXITCODE -ne 0) { throw "release build failed" }

ctest --preset test-release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "tests failed" }

if (Test-Path $DistDir) { Remove-Item $DistDir -Recurse -Force }
cmake --install build\vs2022 --config Release --prefix $DistDir
if ($LASTEXITCODE -ne 0) { throw "install failed" }

Write-Host ""
Write-Host "=== Distribution folder: $DistDir ==="
Get-ChildItem $DistDir -Recurse | Select-Object FullName, Length
