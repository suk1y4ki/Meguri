// MP4 (H.264/HEVC) デコーダ。Windows 標準の Media Foundation IMFSourceReader を使う。
// ファイル全体を読み込まずストリーミングでデコードする。
//
// 2 つの出力モードがある:
// - CPU モード (既定): BGRA バッファを返す (core::IMediaDecoder::next_frame)。
//   GPU デコード時はリーダー内の Video Processor で RGB32 へ変換して読み戻す
// - GPU テクスチャモード (set_gpu_output(true)): NV12 のデコードテクスチャを
//   共有 VideoProcessor で呼び出し側の BGRA テクスチャへ GPU 内変換する。
//   CPU への読み戻しが発生しない (ゼロコピー描画用)
#pragma once

#include <memory>

#include "core/decoder.h"
#include "d3d_video.h"

struct ID3D11Texture2D;

namespace meguri::io {

// プロセスで一度だけ Media Foundation を初期化する (デコーダ生成前に呼ぶ)。
// 各スレッドの CoInitializeEx は呼び出し側 (スレッド起動側) の責務。
bool ensure_media_foundation();
void shutdown_media_foundation();

// GPU テクスチャモードのフレームメタデータ
struct GpuFrameInfo {
    int64_t pts_ms = 0;
    int duration_ms = 0;
};

class Mp4Decoder : public core::IMediaDecoder {
public:
    Mp4Decoder();
    ~Mp4Decoder() override;

    void set_max_output_dimension(int max_dim) override;
    void set_allow_hardware(bool allow) override;

    // open の前に呼ぶ。GPU テクスチャモードを要求する (HW で開けた場合のみ有効)
    void set_gpu_output(bool enabled);
    // open 後: GPU テクスチャモードで動いているか
    bool gpu_frames_active() const;

    bool open(const std::wstring& path) override;
    const core::MediaInfo& info() const override;
    bool next_frame(core::VideoFrame& out) override;

    // GPU テクスチャモード時のフレーム取得。デコード結果を target (共有デバイス上の
    // BGRA RENDER_TARGET テクスチャ) へ変換・拡縮して書き込む。終端で false
    bool next_frame_to_texture(ID3D11Texture2D* target, GpuFrameInfo* out);

    void rewind() override;
    void seek_ms(int64_t position_ms) override;
    const std::string& error_message() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace meguri::io
