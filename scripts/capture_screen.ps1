# 指定タイトルのウィンドウ領域をスクリーンから直接コピーして PNG 保存する。
# (PrintWindow で DirectX 内容が取れない場合の代替。ウィンドウが手前にある必要あり)
param(
    [string]$TitlePrefix = "Meguri",
    [string]$Out = "capture.png"
)
$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class Win32Screen {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
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
"@

[Win32Screen]::SetProcessDPIAware() | Out-Null
[Win32Screen]::Prefix = $TitlePrefix
[Win32Screen]::EnumWindows([Win32Screen+EnumWindowsProc]{ param($h, $l) [Win32Screen]::Callback($h, $l) }, [IntPtr]::Zero) | Out-Null
$hwnd = [Win32Screen]::Found
if ($hwnd -eq [IntPtr]::Zero) { throw "window not found: $TitlePrefix" }
[Win32Screen]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 400

$rect = New-Object Win32Screen+RECT
[Win32Screen]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
if ($w -le 0 -or $h -le 0) { throw "empty window rect" }

Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap $w, $h
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size $w, $h))
$gfx.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "saved: $Out ($w x $h)"
