#include "probe.h"

#include "image_decoder.h"
#include "mp4_decoder.h"
#include "webp_decoder.h"

namespace meguri::io {

std::unique_ptr<core::IMediaDecoder> create_decoder(core::MediaType type) {
    if (core::is_media_foundation_video(type)) return std::make_unique<Mp4Decoder>();
    if (type == core::MediaType::Png || type == core::MediaType::Jpeg) {
        return std::make_unique<ImageDecoder>();
    }
    return std::make_unique<WebpDecoder>();
}

bool probe_media_item(core::MediaItem& item) {
    auto decoder = create_decoder(item.type);
    // メタデータを読むだけなので GPU (DXVA) 枠は使わない
    // (起動直後の全件プローブが GPU セッションを浪費しないように)
    decoder->set_allow_hardware(false);
    if (!decoder->open(item.path)) return false;
    const core::MediaInfo& info = decoder->info();
    item.width = info.width;
    item.height = info.height;
    item.duration_sec = info.duration_sec;
    item.frame_count = info.frame_count;
    return true;
}

}  // namespace meguri::io
