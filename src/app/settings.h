// アプリ設定。既定では EXE と同じフォルダの settings.json に保存する (ポータブル)。
// メニューから %APPDATA%\Meguri へ切替可能。読み込みは EXE 側優先。
// キー欠落は既定値で補完する (前方互換)。
#pragma once

#include <string>

#include "core/item_filter.h"
#include "core/scheduler.h"

namespace meguri::app {

// 設定・キャッシュの保存先
enum class StorageLocation {
    Portable,  // EXE と同じフォルダ (既定)
    AppData,   // %APPDATA%\Meguri
};

// 現在の保存先。初回アクセス時に検出する (EXE 側に settings.json があれば Portable、
// 無ければ AppData 側にあれば AppData、どちらも無ければ Portable)
StorageLocation active_storage_location();

// 保存先を切り替える。settings.json / probe_cache.json を即時移動する。
// 移動先に書き込めない場合は false (現状維持)
bool set_storage_location(StorageLocation target);

// 現在の保存先ディレクトリ内のファイルパスを返す
std::wstring storage_file_path(const wchar_t* filename);

struct Settings {
    // 表示
    std::string language = "auto";  // "auto" | "ja" | "en"
    std::string theme = "dark";     // "dark" | "light"
    double target_row_height = 180.0;

    // 走査・フィルタ
    std::wstring last_folder;
    bool recursive = true;
    bool show_webp = true;
    bool show_mp4 = true;
    bool show_wmv = false;
    bool show_avi = false;
    bool show_png = true;
    bool show_jpeg = true;
    core::SortKey sort_key = core::SortKey::Name;
    bool sort_descending = false;

    // 再生
    core::PerformanceMode performance_mode = core::PerformanceMode::Standard;
    int gpu_memory_percent = 50;  // GPU デコーダに使う VRAM 予算の割合 (25/50/75/100)

    // 削除
    bool confirm_delete = false;  // 既定は確認なし (ゴミ箱行きなので復元可能)

    // デバッグオーバーレイ (タイルごとの計測値表示)
    bool debug_overlay = false;

    // ズーム中のシークバー表示
    bool show_seekbar = true;

    // 選択中ファイル名をステータスバーに表示
    bool show_filenames = false;

    // Ctrl+C で ComfyUI メタデータを優先コピーする
    bool copy_comfy_metadata = true;

    // ズーム中の音声 (単体再生時のみ音を出す)
    int audio_volume = 60;     // 0〜100
    bool audio_muted = false;

    // 実験的: 一覧ビューでホバー中のタイルの音を再生
    bool grid_audio = false;

    // イントロオフセット: 5 分以上の動画を 3:00 から再生
    bool intro_offset = true;

    // ウィンドウ配置 (0 なら既定)
    int window_x = 0;
    int window_y = 0;
    int window_width = 0;
    int window_height = 0;
    bool window_maximized = false;
};

// 設定ファイルのフルパス (現在の保存先に従う)。取得不可なら空。
std::wstring settings_file_path();

// 読み込み失敗 (初回起動・壊れた JSON) は既定値を返す。
Settings load_settings();

// 保存。ポータブル保存先に書き込めない場合は AppData へ自動退避して保存する
bool save_settings(const Settings& settings);

// UTF-8 <-> UTF-16 変換 (設定や JSON まわりで共用)
std::string narrow(const std::wstring& text);
std::wstring widen(const std::string& text);

}  // namespace meguri::app
