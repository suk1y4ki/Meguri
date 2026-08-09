# ズーム往復・最大化後も再生と新規読み込みが継続することを検証する E2E。
# (Debug ビルドのテストフックとアプリ内蔵キャプチャを使用)
param(
    [string]$Folder = "tmp\e2e-heavy"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$cli = "build\vs2022\apps\meguri_cli\Debug\Meguri_CLI.exe"
if (-not (Test-Path $Folder)) {
    if (-not (Test-Path $cli)) {
        throw "Debug CLI not found. Run: cmake --build --preset build-debug"
    }
    & $cli gensample $Folder --webp 12 --mp4 12 | Out-Null
}
New-Item -ItemType Directory -Force tmp\e2e-output | Out-Null

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MgZoomTest {
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, ref COPYDATASTRUCT lp);
    [StructLayout(LayoutKind.Sequential)]
    public struct COPYDATASTRUCT { public IntPtr dwData; public int cbData; public IntPtr lpData; }
}
"@

function Send-Hook([IntPtr]$hwnd, [int]$code, [int]$arg = [int]::MinValue) {
    $cds = New-Object MgZoomTest+COPYDATASTRUCT
    $cds.dwData = [IntPtr]$code
    if ($arg -ne [int]::MinValue) {
        $mem = [Runtime.InteropServices.Marshal]::AllocHGlobal(4)
        [Runtime.InteropServices.Marshal]::WriteInt32($mem, $arg)
        $cds.cbData = 4; $cds.lpData = $mem
        $r = [MgZoomTest]::SendMessageW($hwnd, 0x004A, [IntPtr]::Zero, [ref]$cds)
        [Runtime.InteropServices.Marshal]::FreeHGlobal($mem)
        return $r
    }
    $cds.cbData = 0; $cds.lpData = [IntPtr]::Zero
    return [MgZoomTest]::SendMessageW($hwnd, 0x004A, [IntPtr]::Zero, [ref]$cds)
}

function Get-CaptureHash([string]$path) {
    powershell -ExecutionPolicy Bypass -File scripts\capture_app.ps1 -Out $path | Out-Null
    return (Get-FileHash $path -Algorithm MD5).Hash
}

function Assert-Playing([IntPtr]$hwnd, [string]$label) {
    $h1 = Get-CaptureHash "tmp\e2e-output\zr_a.png"
    Start-Sleep -Milliseconds 700
    $h2 = Get-CaptureHash "tmp\e2e-output\zr_b.png"
    if ($h1 -eq $h2) { throw "$label : 画面が変化していない (再生停止)" }
    Write-Host "$label : 再生継続 OK"
}

Get-Process Meguri -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Start-Process "build\vs2022\apps\meguri_gui\Debug\Meguri.exe" -ArgumentList "`"$root\$Folder`""
Start-Sleep -Seconds 5
$proc = Get-Process Meguri | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$hwnd = $proc.MainWindowHandle

Assert-Playing $hwnd "1. 起動直後"

# 2. ズーム往復
Send-Hook $hwnd 6 2 | Out-Null
Start-Sleep -Seconds 2
Assert-Playing $hwnd "2. ズーム中"
Send-Hook $hwnd 8 | Out-Null
Start-Sleep -Seconds 2
Assert-Playing $hwnd "3. ズーム復帰後"

# 3. 最大化 (SC_MAXIMIZE) → 新規タイルの読み込みと再生
[MgZoomTest]::SendMessageW($hwnd, 0x0112, [IntPtr]0xF030, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 4
Assert-Playing $hwnd "4. 最大化後"

# 4. 末尾までスクロール → 新規タイルが読み込まれる
$grid = 0
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MgFind { [DllImport("user32.dll")] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, [MarshalAs(UnmanagedType.LPWStr)] string c, IntPtr t); }
"@
$grid = [MgFind]::FindWindowExW($hwnd, [IntPtr]::Zero, "MeguriGridView", [IntPtr]::Zero)
[MgZoomTest]::SendMessageW($grid, 0x0100, [IntPtr]0x23, [IntPtr]::Zero) | Out-Null  # VK_END
Start-Sleep -Seconds 3
Assert-Playing $hwnd "5. 末尾スクロール後"

# 5. 元のサイズへ戻す (SC_RESTORE)
[MgZoomTest]::SendMessageW($hwnd, 0x0112, [IntPtr]0xF120, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 2
Assert-Playing $hwnd "6. リストア後"

Get-Process Meguri -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Write-Host "E2E zoom/resize test PASSED"
