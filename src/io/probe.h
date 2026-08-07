// デコーダ生成とメタデータプローブ。
#pragma once

#include <memory>

#include "core/decoder.h"
#include "core/media_item.h"

namespace meguri::io {

// type に応じたデコーダを作る (open は呼び出し側)。
std::unique_ptr<core::IMediaDecoder> create_decoder(core::MediaType type);

// item.path を開いて width/height/duration/frame_count を埋める。失敗時 false。
bool probe_media_item(core::MediaItem& item);

}  // namespace meguri::io
