# 指定タイトルのウィンドウをキャプチャして PNG 保存する (E2E 目視確認用)。
# 使い方: powershell -ExecutionPolicy Bypass -File scripts\capture_window.ps1 -TitlePrefix "Meguri" -Out shot.png
param(
    [string]$TitlePrefix = "Meguri",
    [string]$Out = "capture.png"
)
$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class Win32Capture {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public static IntPtr Found = IntPtr.Zero;
    public static string Prefix = "";
    public static bool Callback(IntPtr hWnd, IntPtr lParam) {
        if (!IsWindowVisible(hWnd)) return true;
        var sb = new StringBuilder(512);
        GetWindowText(hWnd, sb, 512);
        if (sb.ToString().StartsWith(Prefix)) { Found = hWnd; return false; }
        return true;
    }
}
"@ -ReferencedAssemblies System.Drawing

# DPI 仮想化で物理ウィンドウが切り抜かれるのを防ぐ (混在 DPI 環境で必須)
[Win32Capture]::SetProcessDPIAware() | Out-Null
[Win32Capture]::Prefix = $TitlePrefix
[Win32Capture]::EnumWindows([Win32Capture+EnumWindowsProc]{ param($h, $l) [Win32Capture]::Callback($h, $l) }, [IntPtr]::Zero) | Out-Null
$hwnd = [Win32Capture]::Found
if ($hwnd -eq [IntPtr]::Zero) { throw "window not found: $TitlePrefix" }

$rect = New-Object Win32Capture+RECT
[Win32Capture]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
if ($w -le 0 -or $h -le 0) { throw "empty window rect" }

Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap $w, $h
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $gfx.GetHdc()
# PW_RENDERFULLCONTENT (2) で DirectX 描画も含めて取得
[Win32Capture]::PrintWindow($hwnd, $hdc, 2) | Out-Null
$gfx.ReleaseHdc($hdc)
$gfx.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "saved: $Out ($w x $h)"
