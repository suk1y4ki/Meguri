// デコーダの抽象インターフェース。実装は io 層 (libwebp / Media Foundation)。
// core はフレームの器と契約だけを定義し、コーデックの詳細を知らない。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "media_item.h"

namespace meguri::core {

// BGRA (プリマルチプライドではない straight alpha) の 1 フレーム。
// Direct2D へは GUI 側でプリマルチプライ変換して渡す。
struct VideoFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;  // width * height * 4
    int64_t pts_ms = 0;         // 先頭からの表示時刻
    int duration_ms = 0;        // この フレームの表示時間 (不明なら 0)
};

struct MediaInfo {
    int width = 0;
    int height = 0;
    double duration_sec = 0.0;
    int frame_count = 0;  // 不明なら 0 (MP4 は概算)
    bool has_alpha = false;
    bool loops = true;
    bool hardware = false;  // ハードウェア (GPU) デコードで開いたか (デバッグ表示用)
};

class IMediaDecoder {
public:
    virtual ~IMediaDecoder() = default;

    // open の前に呼ぶと、長辺が max_dim 以下になるよう縮小した出力を要求する
    // (0 = 原寸)。対応しないデコーダは無視してよい。
    // 小さいタイルに原寸フレームをデコードする無駄を省くための性能ヒント。
    virtual void set_max_output_dimension(int max_dim) { (void)max_dim; }

    // open の前に呼ぶと、ハードウェア (GPU) デコードの使用可否を指定できる。
    // GPU デコーダが実行時に失敗した後のリトライで、確実に動くソフトウェアへ
    // 切り替えるために使う。対応しないデコーダは無視してよい。
    virtual void set_allow_hardware(bool allow) { (void)allow; }

    // 成否を返す。失敗理由は error_message() に入る。
    virtual bool open(const std::wstring& path) = 0;
    virtual const MediaInfo& info() const = 0;

    // 次のフレームをデコードする。終端に達したら false (rewind して続行可能)。
    virtual bool next_frame(VideoFrame& out) = 0;
    virtual void rewind() = 0;

    // 指定時刻へシークする (次の next_frame がその付近から返る)。
    // 対応しないデコーダは無視してよい (静止画など)
    virtual void seek_ms(int64_t position_ms) { (void)position_ms; }

    virtual const std::string& error_message() const = 0;
};

}  // namespace meguri::core
