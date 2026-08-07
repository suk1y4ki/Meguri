// フィルタ・ソートの純ロジック。MediaItem の配列から表示順インデックスを作る。
#pragma once

#include <vector>

#include "media_item.h"

namespace meguri::core {

struct FilterOptions {
    bool show_webp = true;
    bool show_mp4 = true;
    bool show_png = true;
    bool show_jpeg = true;
};

enum class SortKey {
    Name,
    ModifiedTime,
    FileSize,
};

struct SortOptions {
    SortKey key = SortKey::Name;
    bool descending = false;
};

// items から表示対象の元インデックス列を作る (フィルタ → ソート)。
std::vector<int> build_display_order(const std::vector<MediaItem>& items,
                                     const FilterOptions& filter, const SortOptions& sort);

}  // namespace meguri::core
