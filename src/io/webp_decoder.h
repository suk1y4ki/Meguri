// アニメーション WEBP デコーダ (vendored libwebp / WebPAnimDecoder)。
#pragma once

#include <memory>

#include "core/decoder.h"

namespace meguri::io {

class WebpDecoder : public core::IMediaDecoder {
public:
    WebpDecoder();
    ~WebpDecoder() override;

    bool open(const std::wstring& path) override;
    const core::MediaInfo& info() const override;
    bool next_frame(core::VideoFrame& out) override;
    void rewind() override;
    void seek_ms(int64_t position_ms) override;
    const std::string& error_message() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace meguri::io
