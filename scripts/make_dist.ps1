# 配布用ビルド: Release ビルド -> 全テスト -> dist/Meguri へ最小構成で集約する。
# 使い方 (Developer PowerShell か通常の PowerShell どちらでも可):
#   powershell -ExecutionPolicy Bypass -File scripts\make_dist.ps1
param(
    [string]$DistDir = "dist\Meguri"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

# VS 開発環境が未ロードなら読み込む (cl / ninja を PATH に通す)
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

cmake --preset windows-msvc-release
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
cmake --build --preset build-release
if ($LASTEXITCODE -ne 0) { throw "build failed" }
ctest --preset test-release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "tests failed" }

if (Test-Path $DistDir) { Remove-Item $DistDir -Recurse -Force }
cmake --install build\windows-msvc-release --prefix $DistDir
if ($LASTEXITCODE -ne 0) { throw "install failed" }

Write-Host ""
Write-Host "=== 配布フォルダ: $DistDir ==="
Get-ChildItem $DistDir -Recurse | Select-Object FullName, Length
