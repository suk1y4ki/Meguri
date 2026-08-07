#include "scheduler.h"

#include <algorithm>
#include <cmath>

namespace meguri::core {

ScheduleParams params_for_mode(PerformanceMode mode) {
    ScheduleParams p;
    switch (mode) {
        case PerformanceMode::Standard:
            p.preload_screens = 1.0;
            p.max_active = 96;
            break;
        case PerformanceMode::Massive:
            p.preload_screens = 0.25;
            p.max_active = 48;
            break;
        case PerformanceMode::PlayAll:
            p.preload_screens = 1000000.0;  // 実質全域
            p.max_active = 0;               // 無制限
            break;
    }
    return p;
}

ScheduleResult compute_schedule(const Layout& layout, const ScheduleParams& params) {
    ScheduleResult result;
    if (layout.tiles.empty()) return result;

    const double view_top = params.scroll_y;
    const double view_bottom = params.scroll_y + params.viewport_height;
    const double preload = params.preload_screens * params.viewport_height;
    const double active_top = view_top - preload;
    const double active_bottom = view_bottom + preload;

    // 距離付き候補: 可視は距離 0、それ以外は可視帯からの距離
    struct Candidate {
        int index;
        double distance;
    };
    std::vector<Candidate> candidates;

    for (int i = 0; i < static_cast<int>(layout.tiles.size()); ++i) {
        const TileRect& t = layout.tiles[i];
        const double top = t.y;
        const double bottom = t.y + t.height;
        if (bottom <= active_top || top >= active_bottom) continue;

        const bool visible = bottom > view_top && top < view_bottom;
        if (visible) {
            result.visible.push_back(i);
            // 可視は距離 -1 として必ず先頭に並べる (境界上の非可視 0 距離と区別)
            candidates.push_back({i, -1.0});
        } else {
            const double dist = top >= view_bottom ? top - view_bottom : view_top - bottom;
            candidates.push_back({i, dist});
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                         return a.distance < b.distance;
                     });

    // max_active は先読み分の抑制であって、可視タイルは常に全数アクティブにする
    // (巨大ウィンドウで可視タイルが上限を超えると下の方が永久に読み込まれないため)
    size_t limit = candidates.size();
    if (params.max_active > 0) {
        limit = std::min(limit, static_cast<size_t>(params.max_active));
        limit = std::max(limit, result.visible.size());
    }
    result.active.reserve(limit);
    for (size_t i = 0; i < limit; ++i) result.active.push_back(candidates[i].index);

    return result;
}

}  // namespace meguri::core
