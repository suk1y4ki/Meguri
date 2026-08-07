#include "layout.h"

#include <algorithm>

namespace meguri::core {

namespace {

double sanitize_aspect(double a) {
    if (a <= 0.0) return 1.0;
    // 極端な比率はレイアウトを壊すのでクランプ (1:8 〜 8:1)
    return std::clamp(a, 0.125, 8.0);
}

}  // namespace

Layout compute_justified_layout(const std::vector<double>& aspect_ratios,
                                const LayoutParams& params) {
    Layout layout;
    const int count = static_cast<int>(aspect_ratios.size());
    if (count == 0) return layout;

    const double viewport = std::max(params.viewport_width, 64.0);
    const double row_h = std::max(params.target_row_height, 16.0);
    const double gap = std::max(params.gap, 0.0);

    layout.tiles.reserve(count);

    double y = gap;
    int row_start = 0;
    double row_aspect_sum = 0.0;  // 行内アイテムのアスペクト比合計

    auto flush_row = [&](int row_end, bool is_last_row) {
        const int n = row_end - row_start;
        if (n <= 0) return;
        const double gaps_total = gap * (n + 1);
        const double avail = std::max(viewport - gaps_total, 32.0);
        // 基準行高での自然幅に対する拡縮率
        const double natural_width = row_aspect_sum * row_h;
        double scale = avail / natural_width;
        // 最終行や引き伸ばし過多の行は基準行高のまま左詰めにする
        if (scale > params.max_stretch || (is_last_row && scale > 1.0)) scale = 1.0;
        const double h = row_h * scale;

        double x = gap;
        for (int i = row_start; i < row_end; ++i) {
            const double a = sanitize_aspect(aspect_ratios[i]);
            const double w = a * h;
            TileRect tile;
            tile.item_index = i;
            tile.x = x;
            tile.y = y;
            tile.width = w;
            tile.height = h;
            layout.tiles.push_back(tile);
            x += w + gap;
        }
        y += h + gap;
    };

    for (int i = 0; i < count; ++i) {
        const double a = sanitize_aspect(aspect_ratios[i]);
        const double next_sum = row_aspect_sum + a;
        const int n_after = i - row_start + 1;
        const double natural_width = next_sum * row_h + gap * (n_after + 1);
        if (natural_width >= viewport && i > row_start) {
            // このアイテムを足すと溢れる → 現アイテムを含めた方が誤差が小さいか判定
            const double over = natural_width - viewport;
            const double under = viewport - (row_aspect_sum * row_h + gap * (n_after));
            if (over <= under) {
                row_aspect_sum = next_sum;
                flush_row(i + 1, false);
                row_start = i + 1;
                row_aspect_sum = 0.0;
            } else {
                flush_row(i, false);
                row_start = i;
                row_aspect_sum = a;
            }
        } else if (natural_width >= viewport) {
            // 1 アイテムだけで溢れる (巨大アスペクト or 狭幅ビューポート)
            row_aspect_sum = a;
            flush_row(i + 1, false);
            row_start = i + 1;
            row_aspect_sum = 0.0;
        } else {
            row_aspect_sum = next_sum;
        }
    }
    flush_row(count, true);

    layout.total_height = y;
    return layout;
}

}  // namespace meguri::core
