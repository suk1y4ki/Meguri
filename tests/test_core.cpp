// core 層 (純ロジック) のテスト。
#include "test_main.h"

#include "core/item_filter.h"
#include "core/layout.h"
#include "core/playback.h"
#include "core/scheduler.h"
#include "core/selection.h"

using namespace meguri::core;

// ---- layout ----

TEST_CASE(layout_empty) {
    LayoutParams params;
    const Layout layout = compute_justified_layout({}, params);
    CHECK(layout.tiles.empty());
    CHECK_NEAR(layout.total_height, 0.0, 1e-9);
}

TEST_CASE(layout_single_row_fills_width) {
    LayoutParams params;
    params.viewport_width = 1000.0;
    params.target_row_height = 100.0;
    params.gap = 0.0;
    // 1:1 x 12 個 → 各行が幅いっぱいにジャスティファイされる
    const Layout layout = compute_justified_layout(std::vector<double>(12, 1.0), params);
    CHECK_EQ(static_cast<int>(layout.tiles.size()), 12);
    // 先頭行の右端が viewport 幅とほぼ一致
    double first_row_y = layout.tiles[0].y;
    double right = 0.0;
    for (const auto& t : layout.tiles) {
        if (t.y == first_row_y) right = t.x + t.width;
    }
    CHECK_NEAR(right, 1000.0, 0.5);
}

TEST_CASE(layout_rows_advance_downward) {
    LayoutParams params;
    params.viewport_width = 500.0;
    params.target_row_height = 100.0;
    params.gap = 4.0;
    const Layout layout = compute_justified_layout(std::vector<double>(10, 1.5), params);
    CHECK_EQ(static_cast<int>(layout.tiles.size()), 10);
    // y は単調非減少、行が変わると増える
    double prev_y = -1.0;
    for (const auto& t : layout.tiles) {
        CHECK(t.y >= prev_y - 1e-9);
        prev_y = t.y;
    }
    CHECK(layout.total_height > 200.0);  // 複数行になっている
}

TEST_CASE(layout_last_row_not_stretched) {
    LayoutParams params;
    params.viewport_width = 1000.0;
    params.target_row_height = 100.0;
    params.gap = 0.0;
    // 最終行に 1 個だけ残る → 拡大されず基準行高のまま
    const Layout layout = compute_justified_layout(std::vector<double>(1, 1.0), params);
    CHECK_EQ(static_cast<int>(layout.tiles.size()), 1);
    CHECK_NEAR(layout.tiles[0].height, 100.0, 1e-6);
    CHECK_NEAR(layout.tiles[0].width, 100.0, 1e-6);
}

TEST_CASE(layout_degenerate_aspect_handled) {
    LayoutParams params;
    const Layout layout = compute_justified_layout({0.0, -5.0, 100.0}, params);
    CHECK_EQ(static_cast<int>(layout.tiles.size()), 3);
    for (const auto& t : layout.tiles) {
        CHECK(t.width > 0.0);
        CHECK(t.height > 0.0);
    }
}

// ---- scheduler ----

namespace {
Layout make_grid_layout(int count, double tile_h = 100.0) {
    // 1 列 x count 行の縦一列レイアウトを手組み (スケジューラ単体テスト用)
    Layout layout;
    for (int i = 0; i < count; ++i) {
        TileRect t;
        t.item_index = i;
        t.x = 0;
        t.y = i * tile_h;
        t.width = 100;
        t.height = tile_h;
        layout.tiles.push_back(t);
    }
    layout.total_height = count * tile_h;
    return layout;
}
}  // namespace

TEST_CASE(scheduler_visible_range) {
    const Layout layout = make_grid_layout(100);
    ScheduleParams params;
    params.scroll_y = 250.0;
    params.viewport_height = 300.0;
    params.preload_screens = 0.0;
    params.max_active = 0;
    const ScheduleResult r = compute_schedule(layout, params);
    // 250..550 に重なるのは index 2..5
    CHECK_EQ(static_cast<int>(r.visible.size()), 4);
    CHECK_EQ(r.visible.front(), 2);
    CHECK_EQ(r.visible.back(), 5);
    CHECK_EQ(static_cast<int>(r.active.size()), 4);
}

TEST_CASE(scheduler_preload_and_cap) {
    const Layout layout = make_grid_layout(100);
    ScheduleParams params;
    params.scroll_y = 5000.0;
    params.viewport_height = 300.0;
    params.preload_screens = 1.0;  // 上下 300px
    params.max_active = 6;
    const ScheduleResult r = compute_schedule(layout, params);
    CHECK_EQ(static_cast<int>(r.active.size()), 6);
    // 可視 (50..53) が必ず active に含まれる
    for (int v : r.visible) {
        bool found = false;
        for (int a : r.active) {
            if (a == v) found = true;
        }
        CHECK(found);
    }
}

TEST_CASE(scheduler_visible_never_capped) {
    // 可視タイルが max_active を超えても全可視がアクティブになる
    const Layout layout = make_grid_layout(100);
    ScheduleParams params;
    params.scroll_y = 0.0;
    params.viewport_height = 2000.0;  // 20 タイル可視
    params.preload_screens = 0.25;
    params.max_active = 8;  // 可視より小さい上限
    const ScheduleResult r = compute_schedule(layout, params);
    CHECK_EQ(static_cast<int>(r.visible.size()), 20);
    CHECK(r.active.size() >= r.visible.size());
    for (int v : r.visible) {
        bool found = false;
        for (int a : r.active) {
            if (a == v) found = true;
        }
        CHECK(found);
    }
}

TEST_CASE(scheduler_play_all_mode) {
    const Layout layout = make_grid_layout(50);
    ScheduleParams params = params_for_mode(PerformanceMode::PlayAll);
    params.scroll_y = 0.0;
    params.viewport_height = 300.0;
    const ScheduleResult r = compute_schedule(layout, params);
    CHECK_EQ(static_cast<int>(r.active.size()), 50);  // 全アイテムがアクティブ
}

// ---- selection ----

TEST_CASE(selection_click_and_ctrl) {
    Selection sel;
    sel.click(3, false, false);
    CHECK(sel.contains(3));
    CHECK_EQ(static_cast<int>(sel.size()), 1);
    sel.click(5, true, false);  // Ctrl+クリックで追加
    CHECK(sel.contains(3));
    CHECK(sel.contains(5));
    sel.click(3, true, false);  // Ctrl+クリックで解除
    CHECK(!sel.contains(3));
    CHECK(sel.contains(5));
}

TEST_CASE(selection_shift_range) {
    Selection sel;
    sel.click(2, false, false);
    sel.click(6, false, true);  // Shift で 2..6
    CHECK_EQ(static_cast<int>(sel.size()), 5);
    CHECK(sel.contains(2));
    CHECK(sel.contains(6));
    // アンカー維持: さらに Shift+4 → 2..4
    sel.click(4, false, true);
    CHECK_EQ(static_cast<int>(sel.size()), 3);
    CHECK(!sel.contains(6));
}

TEST_CASE(selection_remap_after_delete) {
    Selection sel;
    sel.click(1, false, false);
    sel.click(3, true, false);
    // index 2 が削除された → 旧 [0,1,2,3,4] -> 新 [0,1,-1,2,3]
    sel.remap({0, 1, -1, 2, 3});
    CHECK(sel.contains(1));
    CHECK(sel.contains(2));  // 旧 3
    CHECK_EQ(static_cast<int>(sel.size()), 2);
}

// ---- playback ----

TEST_CASE(playback_frame_lookup) {
    const std::vector<int> durations = {100, 100, 300};  // 合計 500ms
    CHECK_EQ(frame_index_for_time(durations, 0), 0);
    CHECK_EQ(frame_index_for_time(durations, 99), 0);
    CHECK_EQ(frame_index_for_time(durations, 100), 1);
    CHECK_EQ(frame_index_for_time(durations, 250), 2);
    CHECK_EQ(frame_index_for_time(durations, 500), 0);   // ループ
    CHECK_EQ(frame_index_for_time(durations, 1100), 1);  // 2 周目
}

TEST_CASE(playback_zero_duration_sanitized) {
    const std::vector<int> durations = {0, 0};
    CHECK_EQ(total_duration_ms(durations), 20);  // 0 は 10ms 扱い
    CHECK_EQ(frame_index_for_time(durations, 15), 1);
}

// ---- filter / sort ----

namespace {
std::vector<MediaItem> make_items() {
    std::vector<MediaItem> items(4);
    items[0].path = L"c:\\a.webp";
    items[0].type = MediaType::Webp;
    items[0].file_size = 300;
    items[0].modified_time = 30;
    items[1].path = L"c:\\b.mp4";
    items[1].type = MediaType::Mp4;
    items[1].file_size = 100;
    items[1].modified_time = 40;
    items[2].path = L"c:\\c.webp";
    items[2].type = MediaType::Webp;
    items[2].file_size = 200;
    items[2].modified_time = 10;
    items[3].path = L"c:\\d.mp4";
    items[3].type = MediaType::Mp4;
    items[3].file_size = 400;
    items[3].modified_time = 20;
    return items;
}
}  // namespace

TEST_CASE(filter_by_type) {
    const auto items = make_items();
    FilterOptions filter;
    filter.show_mp4 = false;
    SortOptions sort;
    const auto order = build_display_order(items, filter, sort);
    CHECK_EQ(static_cast<int>(order.size()), 2);
    CHECK_EQ(order[0], 0);
    CHECK_EQ(order[1], 2);
}

TEST_CASE(filter_legacy_video_default_off) {
    std::vector<MediaItem> items(3);
    items[0].path = L"c:\\a.mp4";
    items[0].type = MediaType::Mp4;
    items[1].path = L"c:\\b.wmv";
    items[1].type = MediaType::Wmv;
    items[2].path = L"c:\\c.avi";
    items[2].type = MediaType::Avi;

    FilterOptions filter;
    SortOptions sort;
    const auto order = build_display_order(items, filter, sort);
    CHECK_EQ(static_cast<int>(order.size()), 1);
    CHECK_EQ(order[0], 0);
}

TEST_CASE(filter_legacy_video_enabled) {
    std::vector<MediaItem> items(3);
    items[0].path = L"c:\\a.mp4";
    items[0].type = MediaType::Mp4;
    items[1].path = L"c:\\b.wmv";
    items[1].type = MediaType::Wmv;
    items[2].path = L"c:\\c.avi";
    items[2].type = MediaType::Avi;

    FilterOptions filter;
    filter.show_wmv = true;
    filter.show_avi = true;
    SortOptions sort;
    const auto order = build_display_order(items, filter, sort);
    CHECK_EQ(static_cast<int>(order.size()), 3);
    CHECK_EQ(order[0], 0);
    CHECK_EQ(order[1], 1);
    CHECK_EQ(order[2], 2);
}

TEST_CASE(sort_by_size_descending) {
    const auto items = make_items();
    FilterOptions filter;
    SortOptions sort;
    sort.key = SortKey::FileSize;
    sort.descending = true;
    const auto order = build_display_order(items, filter, sort);
    CHECK_EQ(order[0], 3);  // 400
    CHECK_EQ(order[1], 0);  // 300
    CHECK_EQ(order[2], 2);  // 200
    CHECK_EQ(order[3], 1);  // 100
}

int main() { return meguri_test::run_all(); }
