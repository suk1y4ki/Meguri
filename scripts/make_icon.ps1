# assets\icon.png (正方形推奨・透過可) からマルチサイズの assets\app.ico を生成する。
# 使い方: powershell -ExecutionPolicy Bypass -File scripts\make_icon.ps1
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$src = Join-Path $root "assets\icon.png"
$out = Join-Path $root "assets\app.ico"
if (-not (Test-Path $src)) { throw "assets\icon.png がありません。元画像を置いてください。" }

# アルファ無し画像 (市松模様などの「透過風」背景が描き込まれたもの) は、
# 外周からのフラッドフィルで明るい背景だけを透過にする。
# 塗りつぶしは暗い輪郭線で止まるため、図柄内部の白 (紙) は残る。
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class IconBg {
    public static Bitmap RemoveLightBackground(Image source, int threshold) {
        var bmp = new Bitmap(source.Width, source.Height, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp)) g.DrawImage(source, 0, 0, source.Width, source.Height);
        var rect = new Rectangle(0, 0, bmp.Width, bmp.Height);
        var data = bmp.LockBits(rect, ImageLockMode.ReadWrite, PixelFormat.Format32bppArgb);
        int w = bmp.Width, h = bmp.Height, stride = data.Stride;
        var px = new byte[stride * h];
        Marshal.Copy(data.Scan0, px, 0, px.Length);
        var visited = new bool[w * h];
        var queue = new Queue<int>();
        Func<int, bool> isLight = i => {
            int o = (i / w) * stride + (i % w) * 4;
            return px[o] >= threshold && px[o + 1] >= threshold && px[o + 2] >= threshold;
        };
        Action<int> seed = i => {
            if (!visited[i] && isLight(i)) { visited[i] = true; queue.Enqueue(i); }
        };
        for (int x = 0; x < w; x++) { seed(x); seed((h - 1) * w + x); }
        for (int y = 0; y < h; y++) { seed(y * w); seed(y * w + w - 1); }
        while (queue.Count > 0) {
            int i = queue.Dequeue();
            int o = (i / w) * stride + (i % w) * 4;
            px[o + 3] = 0;  // 透過
            int x = i % w, y = i / w;
            if (x > 0) seed(i - 1);
            if (x + 1 < w) seed(i + 1);
            if (y > 0) seed(i - w);
            if (y + 1 < h) seed(i + w);
        }
        Marshal.Copy(px, 0, data.Scan0, px.Length);
        bmp.UnlockBits(data);
        return bmp;
    }
}
"@

$loaded = [System.Drawing.Image]::FromFile($src)
$hasAlpha = [System.Drawing.Image]::IsAlphaPixelFormat($loaded.PixelFormat)
if (-not $hasAlpha) {
    Write-Host "アルファ無し画像のため、外周の明るい背景を透過化します"
    $source = [IconBg]::RemoveLightBackground($loaded, 235)
    $loaded.Dispose()
} else {
    $source = $loaded
}
$sizes = 256, 128, 64, 48, 32, 24, 16
$blobs = @()
foreach ($size in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.DrawImage($source, 0, 0, $size, $size)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $blobs += , @{ size = $size; bytes = $ms.ToArray() }
    $ms.Dispose()
}
$source.Dispose()

# ICO コンテナ (PNG 圧縮エントリ、Windows Vista 以降対応)
$fs = [System.IO.File]::Create($out)
$w = New-Object System.IO.BinaryWriter($fs)
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$blobs.Count)
$offset = 6 + 16 * $blobs.Count
foreach ($blob in $blobs) {
    $dim = if ($blob.size -ge 256) { 0 } else { $blob.size }
    $w.Write([byte]$dim); $w.Write([byte]$dim)       # 幅, 高さ (0 = 256)
    $w.Write([byte]0); $w.Write([byte]0)             # パレット数, 予約
    $w.Write([uint16]1); $w.Write([uint16]32)        # プレーン, ビット深度
    $w.Write([uint32]$blob.bytes.Length)             # データサイズ
    $w.Write([uint32]$offset)                        # データ位置
    $offset += $blob.bytes.Length
}
foreach ($blob in $blobs) { $w.Write($blob.bytes) }
$w.Close()
"created: $out ($($blobs.Count) sizes)"
