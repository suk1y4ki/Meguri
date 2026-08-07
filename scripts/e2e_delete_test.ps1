# 削除 → ゴミ箱 → Ctrl+Z 復元の E2E テスト (Debug ビルドのテストフックを使用)。
# 使い方: powershell -ExecutionPolicy Bypass -File scripts\e2e_delete_test.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$cli = "build\windows-msvc-debug\apps\meguri_cli\Meguri_CLI.exe"
$gui = "build\windows-msvc-debug\apps\meguri_gui\Meguri.exe"
$testDir = Join-Path $root "samples\deltest"

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MgHook {
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, ref COPYDATASTRUCT lp);
    [StructLayout(LayoutKind.Sequential)]
    public struct COPYDATASTRUCT { public IntPtr dwData; public int cbData; public IntPtr lpData; }
}
"@

function Send-Hook([IntPtr]$hwnd, [int]$code, [int]$arg = -1) {
    $cds = New-Object MgHook+COPYDATASTRUCT
    $cds.dwData = [IntPtr]$code
    if ($arg -ge 0) {
        $mem = [Runtime.InteropServices.Marshal]::AllocHGlobal(4)
        [Runtime.InteropServices.Marshal]::WriteInt32($mem, $arg)
        $cds.cbData = 4
        $cds.lpData = $mem
        $r = [MgHook]::SendMessageW($hwnd, 0x004A, [IntPtr]::Zero, [ref]$cds)
        [Runtime.InteropServices.Marshal]::FreeHGlobal($mem)
        return $r
    }
    $cds.cbData = 0
    $cds.lpData = [IntPtr]::Zero
    return [MgHook]::SendMessageW($hwnd, 0x004A, [IntPtr]::Zero, [ref]$cds)
}

# 1. テスト用フォルダを生成
Get-Process Meguri -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
if (Test-Path $testDir) { Remove-Item $testDir -Recurse -Force -Confirm:$false }
& $cli gensample $testDir --webp 3 --mp4 3 | Out-Null
$before = (Get-ChildItem $testDir -File).Count
Write-Host "files before: $before"

# 2. GUI 起動
Start-Process $gui -ArgumentList "`"$testDir`""
Start-Sleep -Seconds 4
$proc = Get-Process Meguri | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { throw "GUI not running" }
$hwnd = $proc.MainWindowHandle

# 3. 先頭タイルを選択 → 削除
if ((Send-Hook $hwnd 5 0) -eq [IntPtr]::Zero) { throw "select hook failed" }
if ((Send-Hook $hwnd 3) -eq [IntPtr]::Zero) { throw "delete hook failed" }
Start-Sleep -Seconds 1
$afterDelete = (Get-ChildItem $testDir -File).Count
Write-Host "files after delete: $afterDelete"
if ($afterDelete -ne $before - 1) { throw "delete did not remove exactly 1 file" }

# 4. Ctrl+Z 相当の復元
if ((Send-Hook $hwnd 4) -eq [IntPtr]::Zero) { throw "undo hook failed" }
Start-Sleep -Seconds 2
$afterUndo = (Get-ChildItem $testDir -File).Count
Write-Host "files after undo: $afterUndo"
if ($afterUndo -ne $before) { throw "undo did not restore the file" }

# 5. スクリーンショットも残す
powershell -ExecutionPolicy Bypass -File scripts\capture_app.ps1 -Out samples\output\e2e_delete.png | Out-Null

Get-Process Meguri -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Write-Host "E2E delete/undo test PASSED"
