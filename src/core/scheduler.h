// 仮想化スケジューラ。スクロール位置から「描画すべきタイル」と
// 「デコード・再生をアクティブにすべきタイル」を決める。
#pragma once

#include <vector>

#include "layout.h"

namespace meguri::core {

// パフォーマンスモード。ユーザーが切替可能 (設定に保存)。
enum class PerformanceMode {
    Standard,  // 標準: 可視 + 前後 1 画面を先読み
    Massive,   // 大量: 可視のみ再生 (先読みなし・同時数を強めに制限)
    PlayAll,   // 全再生: 全アイテムをアクティブ化 (小規模フォルダ向け)
};

struct ScheduleParams {
    double scroll_y = 0.0;
    double viewport_height = 720.0;
    double preload_screens = 1.0;  // 可視範囲の上下に先読みする画面数
    int max_active = 0;            // 同時アクティブ上限 (0 = 無制限)
};

// モードごとの既定パラメータを埋める (scroll_y / viewport_height は呼び出し側で設定)
ScheduleParams params_for_mode(PerformanceMode mode);

struct ScheduleResult {
    std::vector<int> visible;  // 可視タイル (tiles 配列のインデックス, 上から順)
    std::vector<int> active;   // デコード・再生対象 (可視を優先し距離が近い順)
};

ScheduleResult compute_schedule(const Layout& layout, const ScheduleParams& params);

}  // namespace meguri::core
