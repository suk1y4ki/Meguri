// プロセス共有の D3D11 デバイスと GPU 動画基盤。
// - MF デコーダへ渡す IMFDXGIDeviceManager (DXVA デコード用)
// - GPU デコーダ同時数の管理 (静的上限 + 実行時失敗から学習する動的上限)
// - NV12 デコードテクスチャ → BGRA テクスチャの GPU 内変換
//   (プロセス共有の ID3D11VideoProcessor 1 個で行う。ストリームごとの
//    Video Processor MFT を持たないのが Chrome 型ゼロコピー構成の要)
#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11Texture2D;
struct IMFDXGIDeviceManager;

namespace meguri::io {

// 共有 D3D11 デバイス (BGRA + VIDEO 対応、マルチスレッド保護済み)。失敗時 nullptr。
// D2D 側もこのデバイス上に作ることでテクスチャをゼロコピーで描画できる
ID3D11Device* shared_d3d_device();

// MF SourceReader へ渡すデバイスマネージャ。失敗時 nullptr (ソフトウェアデコードへ)
IMFDXGIDeviceManager* dxgi_device_manager();

// ---- GPU デコーダ同時数の管理 ----
// 枠の取得/返却。取得できなければソフトウェアで開く
bool try_acquire_gpu_slot();
void release_gpu_slot();

// GPU デコーダの実行時失敗 (ReadSample の E_OUTOFMEMORY 等) の通知。
// 動的上限を下げて以後の open をソフトウェアへ流す (静穏で徐々に復帰)
void note_gpu_runtime_failure();

// デバッグ表示用: 現在の使用数と実効上限
void gpu_decoder_stats(int* used, int* cap);

// GPU デコーダに使ってよい VRAM 予算の割合 (10〜100%)
void set_gpu_memory_percent(int percent);

// VRAM の現在使用量と予算 (MB)。取得できない環境では false
bool gpu_memory_stats(uint64_t* usage_mb, uint64_t* budget_mb);

// ---- NV12 → BGRA の GPU 内変換 (共有 VideoProcessor、スレッド安全) ----
// src の subresource (デコーダのテクスチャ配列スライス) を dst 全面へ
// 変換・拡縮して書き込む。dst は shared_d3d_device 上の BGRA RENDER_TARGET。
// src_width/src_height には「表示サイズ」を渡すこと。デコードテクスチャは
// コーデックのアライメント (16 の倍数など) でパディングされており、
// テクスチャ寸法をそのまま使うと下端・右端にゴミ行が混入する (0 = テクスチャ寸法)。
// 色空間は表示高さから BT.601 / BT.709 (limited range) を推定する
bool convert_nv12_to_bgra(ID3D11Texture2D* src, unsigned subresource, int src_width,
                          int src_height, ID3D11Texture2D* dst);

// BGRA テクスチャを CPU へ読み戻す (キャプチャ用。ステージング経由なので低速)
bool read_texture_bgra(ID3D11Texture2D* texture, unsigned char* out, size_t out_size,
                       int* width, int* height);

}  // namespace meguri::io
