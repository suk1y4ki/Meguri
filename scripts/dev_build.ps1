# 開発用ビルド: VS 開発環境を読み込んで configure + build + test を行う。
# 使い方: powershell -ExecutionPolicy Bypass -File scripts\dev_build.ps1 [-Release] [-NoTest]
param(
    [switch]$Release,
    [switch]$NoTest
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = & $vswhere -latest -products * -property installationPath
    $devShell = Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1"
    try {
        & $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    } catch {
        Write-Warning "Launch-VsDevShell.ps1 failed. Falling back to VsDevCmd.bat."
    }
    if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
        $vsDevCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
        $envLines = & cmd /s /c "`"$vsDevCmd`" -arch=amd64 -host_arch=amd64 >nul && set"
        foreach ($line in $envLines) {
            $idx = $line.IndexOf("=")
            if ($idx -gt 0) {
                [Environment]::SetEnvironmentVariable($line.Substring(0, $idx), $line.Substring($idx + 1), "Process")
            }
        }
    }
    Set-Location $root
}

$preset = if ($Release) { "windows-msvc-release" } else { "windows-msvc-debug" }
$buildPreset = if ($Release) { "build-release" } else { "build-debug" }
$testPreset = if ($Release) { "test-release" } else { "test-debug" }

cmake --preset $preset
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
cmake --build --preset $buildPreset
if ($LASTEXITCODE -ne 0) { throw "build failed" }
if (-not $NoTest) {
    ctest --preset $testPreset --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "tests failed" }
}
