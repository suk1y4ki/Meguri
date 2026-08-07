// ジャスティファイド (行詰め) レイアウト。
// 左上から右へ、行ごとに高さを揃えてアスペクト比の異なるタイルを敷き詰める。
#pragma once

#include <vector>

namespace meguri::core {

struct LayoutParams {
    double viewport_width = 1280.0;
    double target_row_height = 180.0;  // 基準行高。行の確定時に拡縮される
    double gap = 4.0;                  // タイル間・行間の隙間
    double max_stretch = 1.35;  // 行確定時に許容する拡大率上限 (超える場合は左詰めのまま)
};

struct TileRect {
    int item_index = 0;  // 表示順リスト内のインデックス
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct Layout {
    std::vector<TileRect> tiles;  // item_index 昇順 = 上から下への行順
    double total_height = 0.0;
};

// aspect_ratios: 表示順に並んだ各アイテムの幅/高さ比 (0 以下は 1.0 扱い)。
Layout compute_justified_layout(const std::vector<double>& aspect_ratios,
                                const LayoutParams& params);

}  // namespace meguri::core
