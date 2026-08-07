#include "item_filter.h"

#include <algorithm>

namespace meguri::core {

std::vector<int> build_display_order(const std::vector<MediaItem>& items,
                                     const FilterOptions& filter, const SortOptions& sort) {
    std::vector<int> order;
    order.reserve(items.size());
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const MediaType t = items[i].type;
        if (t == MediaType::Webp && !filter.show_webp) continue;
        if (t == MediaType::Mp4 && !filter.show_mp4) continue;
        if (t == MediaType::Png && !filter.show_png) continue;
        if (t == MediaType::Jpeg && !filter.show_jpeg) continue;
        order.push_back(i);
    }

    auto less = [&](int a, int b) {
        const MediaItem& ia = items[a];
        const MediaItem& ib = items[b];
        switch (sort.key) {
            case SortKey::ModifiedTime:
                if (ia.modified_time != ib.modified_time)
                    return ia.modified_time < ib.modified_time;
                break;
            case SortKey::FileSize:
                if (ia.file_size != ib.file_size) return ia.file_size < ib.file_size;
                break;
            case SortKey::Name:
                break;
        }
        return ia.path < ib.path;  // 同値はパスで安定化
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return sort.descending ? less(b, a) : less(a, b);
    });
    return order;
}

}  // namespace meguri::core
