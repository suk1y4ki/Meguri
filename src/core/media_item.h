// メディアアイテムの基本型。core は標準 C++ のみに依存する。
#pragma once

#include <cstdint>
#include <string>

namespace meguri::core {

enum class MediaType {
    Webp,
    Mp4,
    Wmv,
    Avi,
    Png,   // 静止画。1 フレームだけの動画として扱う
    Jpeg,  // 静止画 (.jpg / .jpeg)
};

inline bool is_media_foundation_video(MediaType type) {
    return type == MediaType::Mp4 || type == MediaType::Wmv || type == MediaType::Avi;
}

// 走査済みメディアファイル 1 件分のメタデータ。
// width/height/duration はデコーダによるプローブ後に埋まる (未プローブ時は 0)。
struct MediaItem {
    std::wstring path;  // 絶対パス
    MediaType type = MediaType::Webp;
    int width = 0;
    int height = 0;
    double duration_sec = 0.0;
    int frame_count = 0;        // 不明なら 0
    uint64_t file_size = 0;     // バイト
    int64_t modified_time = 0;  // ソート用の単調キー (io 層が epoch 秒で埋める)
};

// アスペクト比 (幅/高さ)。未プローブや不正値は 1.0 とみなす。
inline double aspect_ratio(const MediaItem& item) {
    if (item.width <= 0 || item.height <= 0) return 1.0;
    return static_cast<double>(item.width) / static_cast<double>(item.height);
}

}  // namespace meguri::core
