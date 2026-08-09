// メインウィンドウ。ツールバー / グリッド / ステータスバーの配置と、
// フォルダ読み込み・フィルタ・削除 (ゴミ箱) ・復元・設定の永続化を担当する。
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

#include "app/probe_cache.h"
#include "app/settings.h"
#include "core/item_filter.h"
#include "grid_view.h"
#include "playback_engine.h"

namespace meguri {

class MainWindow {
public:
    MainWindow() = default;
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // initial_folder が空でなければ設定より優先して開く
    bool create(HINSTANCE instance, int show_command,
                const std::wstring& initial_folder = std::wstring());
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT msg, WPARAM wparam, LPARAM lparam);

    void on_create();
    void on_command(int id);
    void layout_children();
    void update_dpi(UINT dpi);

    void open_folder_dialog();
    void open_folder(const std::wstring& folder);
    void rebuild_display_order();

    void delete_selection();
    void undo_delete();

    void rebuild_menu();
    void apply_language();
    void apply_theme();
    void update_status();
    void set_status(const std::wstring& text);
    void save_settings_now();

    double scaled_row_height() const;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND btn_open_ = nullptr;
    HWND chk_webp_ = nullptr;
    HWND chk_mp4_ = nullptr;
    HWND chk_wmv_ = nullptr;
    HWND chk_avi_ = nullptr;
    HWND chk_png_ = nullptr;
    HWND chk_jpeg_ = nullptr;
    HWND chk_recursive_ = nullptr;
    HFONT ui_font_ = nullptr;
    UINT dpi_ = 96;

    app::Settings settings_;
    PlaybackEngine engine_;
    GridView grid_;

    std::vector<core::MediaItem> library_;  // engine の tile と同一順
    std::vector<bool> deleted_;             // ゴミ箱送りにしたもの (表示から除外)
    std::wstring current_folder_;

    // Ctrl+Z 用: 直近の削除バッチ (engine index と元パス)
    struct DeleteBatch {
        std::vector<int> engine_indices;
        std::vector<std::wstring> paths;
    };
    std::vector<DeleteBatch> undo_stack_;

    std::wstring status_text_;
    bool probing_ = false;

    // プローブ結果の永続キャッシュ (2 回目以降のオープンを即時化)
    app::ProbeCache probe_cache_;
    void store_probe_results();  // プローブ完了時にキャッシュへ取り込んで保存
};

}  // namespace meguri
