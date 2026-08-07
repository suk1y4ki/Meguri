// 静止画 (PNG / JPEG) デコーダ。WIC で読み込み「1 フレームだけの動画」として振る舞う。
// max_output_dimension が指定されていれば WIC のスケーラで縮小してから取り出す
// (巨大な写真をタイルサイズで保持するため)。
#pragma once

#include <memory>

#include "core/decoder.h"

namespace meguri::io {

class ImageDecoder : public core::IMediaDecoder {
public:
    ImageDecoder();
    ~ImageDecoder() override;

    void set_max_output_dimension(int max_dim) override;
    bool open(const std::wstring& path) override;
    const core::MediaInfo& info() const override;
    bool next_frame(core::VideoFrame& out) override;
    void rewind() override;
    const std::string& error_message() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace meguri::io
