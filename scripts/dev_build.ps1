# Development build: configure, build, and optionally run tests.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\dev_build.ps1 [-Release] [-NoTest]
param(
    [switch]$Release,
    [switch]$NoTest
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$config = if ($Release) { "Release" } else { "Debug" }
$buildPreset = if ($Release) { "build-release" } else { "build-debug" }
$testPreset = if ($Release) { "test-release" } else { "test-debug" }

cmake --preset vs2022
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

cmake --build --preset $buildPreset
if ($LASTEXITCODE -ne 0) { throw "$config build failed" }

if (-not $NoTest) {
    ctest --preset $testPreset --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "$config tests failed" }
}
