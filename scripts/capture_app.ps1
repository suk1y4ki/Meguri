# Meguri の描画内容をアプリ内蔵キャプチャ (WM_COPYDATA) で PNG 保存する。
# DWM 合成に依存しないため、セッションロック中でも取得できる (E2E 用)。
# 使い方: powershell -ExecutionPolicy Bypass -File scripts\capture_app.ps1 -Out shot.png
param(
    [string]$Out = "capture.png"
)
$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MeguriCapture {
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, ref COPYDATASTRUCT lp);
    [StructLayout(LayoutKind.Sequential)]
    public struct COPYDATASTRUCT { public IntPtr dwData; public int cbData; public IntPtr lpData; }
}
"@

$proc = Get-Process Meguri -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { throw "Meguri window not found" }
$hwnd = $proc.MainWindowHandle

$fullPath = [System.IO.Path]::GetFullPath($Out)
$bytes = [System.Text.Encoding]::Unicode.GetBytes($fullPath + [char]0)
$mem = [Runtime.InteropServices.Marshal]::AllocHGlobal($bytes.Length)
try {
    [Runtime.InteropServices.Marshal]::Copy($bytes, 0, $mem, $bytes.Length)
    $cds = New-Object MeguriCapture+COPYDATASTRUCT
    $cds.dwData = [IntPtr]1
    $cds.cbData = $bytes.Length
    $cds.lpData = $mem
    $result = [MeguriCapture]::SendMessageW($hwnd, 0x004A, [IntPtr]::Zero, [ref]$cds)  # WM_COPYDATA
    if ($result -eq [IntPtr]::Zero) { throw "capture failed (app returned 0)" }
    Write-Host "saved: $fullPath"
} finally {
    [Runtime.InteropServices.Marshal]::FreeHGlobal($mem)
}
