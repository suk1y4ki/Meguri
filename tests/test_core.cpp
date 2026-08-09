// core 層 (純ロジック) のテスト。
#include "test_main.h"

#include "core/item_filter.h"
#include "core/layout.h"
#include "core/playback.h"
#include "core/scheduler.h"
#include "core/selection.h"
#include "io/comfy_metadata.h"

#include <array>
#include <filesystem>
#include <fstream>

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

// ---- ComfyUI metadata ----

namespace {
void append_u32_be(std::vector<unsigned char>* out, uint32_t value) {
    out->push_back(static_cast<unsigned char>((value >> 24) & 0xff));
    out->push_back(static_cast<unsigned char>((value >> 16) & 0xff));
    out->push_back(static_cast<unsigned char>((value >> 8) & 0xff));
    out->push_back(static_cast<unsigned char>(value & 0xff));
}

void append_u16_le(std::vector<unsigned char>* out, uint16_t value) {
    out->push_back(static_cast<unsigned char>(value & 0xff));
    out->push_back(static_cast<unsigned char>((value >> 8) & 0xff));
}

void append_u32_le(std::vector<unsigned char>* out, uint32_t value) {
    out->push_back(static_cast<unsigned char>(value & 0xff));
    out->push_back(static_cast<unsigned char>((value >> 8) & 0xff));
    out->push_back(static_cast<unsigned char>((value >> 16) & 0xff));
    out->push_back(static_cast<unsigned char>((value >> 24) & 0xff));
}

void append_chunk(std::vector<unsigned char>* out, const char* type,
                  const std::vector<unsigned char>& data) {
    append_u32_be(out, static_cast<uint32_t>(data.size()));
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<unsigned char>(type[i]));
    out->insert(out->end(), data.begin(), data.end());
    append_u32_be(out, 0);  // CRC is not validated by the metadata scanner.
}

void append_box(std::vector<unsigned char>* out, const std::array<unsigned char, 4>& type,
                const std::vector<unsigned char>& payload) {
    append_u32_be(out, static_cast<uint32_t>(payload.size() + 8));
    out->insert(out->end(), type.begin(), type.end());
    out->insert(out->end(), payload.begin(), payload.end());
}

std::vector<unsigned char> make_box(const std::array<unsigned char, 4>& type,
                                    const std::vector<unsigned char>& payload) {
    std::vector<unsigned char> box;
    append_box(&box, type, payload);
    return box;
}

std::filesystem::path write_test_png(const char* name, const std::vector<unsigned char>& bytes) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return path;
}

std::vector<unsigned char> text_chunk_data(const std::string& key, const std::string& text) {
    std::vector<unsigned char> data(key.begin(), key.end());
    data.push_back(0);
    data.insert(data.end(), text.begin(), text.end());
    return data;
}

std::vector<unsigned char> data_box_payload(const std::string& text) {
    std::vector<unsigned char> data = {0, 0, 0, 1, 0, 0, 0, 0};
    data.insert(data.end(), text.begin(), text.end());
    return data;
}

std::vector<unsigned char> metadata_keys_payload(const std::vector<std::string>& keys) {
    std::vector<unsigned char> data = {0, 0, 0, 0};  // version/flags
    append_u32_be(&data, static_cast<uint32_t>(keys.size()));
    for (const auto& key : keys) {
        append_u32_be(&data, static_cast<uint32_t>(key.size() + 8));
        data.insert(data.end(), {'m', 'd', 't', 'a'});
        data.insert(data.end(), key.begin(), key.end());
    }
    return data;
}

std::vector<unsigned char> indexed_metadata_item(uint32_t index, const std::string& text) {
    const auto data = make_box({'d', 'a', 't', 'a'}, data_box_payload(text));
    return make_box({static_cast<unsigned char>((index >> 24) & 0xff),
                     static_cast<unsigned char>((index >> 16) & 0xff),
                     static_cast<unsigned char>((index >> 8) & 0xff),
                     static_cast<unsigned char>(index & 0xff)},
                    data);
}

std::vector<unsigned char> webp_exif_payload(const std::string& text) {
    std::vector<unsigned char> tiff = {'I', 'I'};
    append_u16_le(&tiff, 42);
    append_u32_le(&tiff, 8);
    append_u16_le(&tiff, 1);
    append_u16_le(&tiff, 270);
    append_u16_le(&tiff, 2);
    append_u32_le(&tiff, static_cast<uint32_t>(text.size() + 1));
    append_u32_le(&tiff, 8 + 2 + 12 + 4);
    append_u32_le(&tiff, 0);
    tiff.insert(tiff.end(), text.begin(), text.end());
    tiff.push_back(0);

    std::vector<unsigned char> payload = {'E', 'x', 'i', 'f', 0, 0};
    payload.insert(payload.end(), tiff.begin(), tiff.end());
    return payload;
}

void append_riff_chunk(std::vector<unsigned char>* out, const char* type,
                       const std::vector<unsigned char>& data) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<unsigned char>(type[i]));
    append_u32_le(out, static_cast<uint32_t>(data.size()));
    out->insert(out->end(), data.begin(), data.end());
    if (data.size() % 2 == 1) out->push_back(0);
}
}  // namespace

TEST_CASE(comfy_png_metadata_prefers_workflow) {
    std::vector<unsigned char> bytes = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    append_chunk(&bytes, "tEXt", text_chunk_data("prompt", "{\"api\":true}"));
    append_chunk(&bytes, "tEXt", text_chunk_data("workflow", "{\"version\":1}"));
    append_chunk(&bytes, "IEND", {});

    const auto path = write_test_png("meguri_comfy_workflow.png", bytes);
    const auto metadata = meguri::io::extract_comfy_metadata(path.wstring());
    CHECK(metadata.kind == meguri::io::ComfyMetadataKind::Workflow);
    CHECK(metadata.json == "{\"version\":1}");
    std::filesystem::remove(path);
}

TEST_CASE(comfy_png_metadata_uses_prompt_fallback) {
    std::vector<unsigned char> bytes = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    append_chunk(&bytes, "tEXt", text_chunk_data("prompt", "{\"api\":true}"));
    append_chunk(&bytes, "IEND", {});

    const auto path = write_test_png("meguri_comfy_prompt.png", bytes);
    const auto metadata = meguri::io::extract_comfy_metadata(path.wstring());
    CHECK(metadata.kind == meguri::io::ComfyMetadataKind::Prompt);
    CHECK(metadata.json == "{\"api\":true}");
    std::filesystem::remove(path);
}

TEST_CASE(comfy_mp4_mdta_metadata_prefers_workflow) {
    const auto keys = make_box({'k', 'e', 'y', 's'},
                               metadata_keys_payload({"prompt", "workflow"}));
    const auto prompt = indexed_metadata_item(1, "{\"api\":true}");
    const auto workflow = indexed_metadata_item(2, "{\"version\":1}");
    std::vector<unsigned char> ilst_payload = prompt;
    ilst_payload.insert(ilst_payload.end(), workflow.begin(), workflow.end());
    const auto ilst = make_box({'i', 'l', 's', 't'}, ilst_payload);

    std::vector<unsigned char> meta_payload = {0, 0, 0, 0};
    meta_payload.insert(meta_payload.end(), keys.begin(), keys.end());
    meta_payload.insert(meta_payload.end(), ilst.begin(), ilst.end());
    const auto meta = make_box({'m', 'e', 't', 'a'}, meta_payload);
    const auto udta = make_box({'u', 'd', 't', 'a'}, meta);
    const auto moov = make_box({'m', 'o', 'o', 'v'}, udta);

    std::vector<unsigned char> bytes;
    append_box(&bytes, {'f', 't', 'y', 'p'}, {'i', 's', 'o', 'm', 0, 0, 0, 0});
    bytes.insert(bytes.end(), moov.begin(), moov.end());

    const auto path = write_test_png("meguri_comfy_video_mdta.mp4", bytes);
    const auto metadata = meguri::io::extract_comfy_metadata(path.wstring());
    CHECK(metadata.kind == meguri::io::ComfyMetadataKind::Workflow);
    CHECK(metadata.json == "{\"version\":1}");
    std::filesystem::remove(path);
}

TEST_CASE(comfy_mp4_comment_metadata_legacy_fallback) {
    const std::string combined = "{\"prompt\":{\"api\":true},\"workflow\":{\"version\":1}}";
    const auto data = make_box({'d', 'a', 't', 'a'}, data_box_payload(combined));
    const auto comment = make_box({0xa9, 'c', 'm', 't'}, data);
    const auto ilst = make_box({'i', 'l', 's', 't'}, comment);
    std::vector<unsigned char> meta_payload = {0, 0, 0, 0};
    meta_payload.insert(meta_payload.end(), ilst.begin(), ilst.end());
    const auto meta = make_box({'m', 'e', 't', 'a'}, meta_payload);
    const auto udta = make_box({'u', 'd', 't', 'a'}, meta);
    const auto moov = make_box({'m', 'o', 'o', 'v'}, udta);

    std::vector<unsigned char> bytes;
    append_box(&bytes, {'f', 't', 'y', 'p'}, {'i', 's', 'o', 'm', 0, 0, 0, 0});
    bytes.insert(bytes.end(), moov.begin(), moov.end());

    const auto path = write_test_png("meguri_comfy_video.mp4", bytes);
    const auto metadata = meguri::io::extract_comfy_metadata(path.wstring());
    CHECK(metadata.kind == meguri::io::ComfyMetadataKind::Workflow);
    CHECK(metadata.json == "{\"version\":1}");
    std::filesystem::remove(path);
}

TEST_CASE(comfy_webp_exif_metadata_reads_workflow) {
    std::vector<unsigned char> bytes = {'R', 'I', 'F', 'F'};
    std::vector<unsigned char> body = {'W', 'E', 'B', 'P'};
    append_riff_chunk(&body, "EXIF", webp_exif_payload("workflow:{\"version\":1}"));
    append_u32_le(&bytes, static_cast<uint32_t>(body.size()));
    bytes.insert(bytes.end(), body.begin(), body.end());

    const auto path = write_test_png("meguri_comfy_webp.webp", bytes);
    const auto metadata = meguri::io::extract_comfy_metadata(path.wstring());
    CHECK(metadata.kind == meguri::io::ComfyMetadataKind::Workflow);
    CHECK(metadata.json == "{\"version\":1}");
    std::filesystem::remove(path);
}

int main() { return meguri_test::run_all(); }
