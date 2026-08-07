// E2E 検証用のサンプル生成。アニメーション WEBP (libwebp) と MP4 (MF SinkWriter) を
// 自前で生成するので、外部ツールなしで動作確認できる。
#pragma once

#include <string>

namespace meguri::io {

struct SampleSpec {
    int width = 320;
    int height = 240;
    int frame_count = 30;
    int frame_ms = 66;  // WEBP のフレーム間隔 (MP4 は近い fps に丸める)
    int seed = 0;       // 色・動きのバリエーション
};

bool generate_sample_webp(const std::wstring& path, const SampleSpec& spec, std::string* error);
bool generate_sample_mp4(const std::wstring& path, const SampleSpec& spec, std::string* error);

}  // namespace meguri::io
