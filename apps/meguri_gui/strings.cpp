#include "strings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

namespace meguri {

namespace {

UiLanguage g_language = UiLanguage::Auto;

bool resolve_japanese() {
    if (g_language == UiLanguage::Japanese) return true;
    if (g_language == UiLanguage::English) return false;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE;
}

struct Entry {
    const wchar_t* ja;
    const wchar_t* en;
};

const Entry& entry_for(Str id) {
    // Str と同順に並べる
    static const Entry table[] = {
        /* AppTitle */ {L"Meguri", L"Meguri"},
        /* OpenFolder */ {L"フォルダを開く...", L"Open Folder..."},
        /* FilterWebp */ {L"WEBP", L"WEBP"},
        /* FilterMp4 */ {L"MP4", L"MP4"},
        /* FilterWmv */ {L"WMV", L"WMV"},
        /* FilterAvi */ {L"AVI", L"AVI"},
        /* FilterPng */ {L"PNG", L"PNG"},
        /* FilterJpeg */ {L"JPG", L"JPG"},
        /* Recursive */ {L"サブフォルダ", L"Subfolders"},
        /* StatusNoFolder */
        {L"フォルダを開くかドロップしてください。クリックで選択 / Del でゴミ箱へ / Ctrl+Z で復元",
         L"Open or drop a folder. Click to select, Del to recycle, Ctrl+Z to restore"},
        /* StatusFmt */
        {L"%d 件 (WEBP %d / MP4 %d / WMV %d / AVI %d / PNG %d / JPG %d)   選択 %d",
         L"%d items (WEBP %d / MP4 %d / WMV %d / AVI %d / PNG %d / JPG %d)   %d selected"},
        /* StatusLoadingFmt */
        {L"ファイル情報を取得中... %d/%d (次回からはキャッシュで即時)",
         L"Reading file info... %d/%d (cached for next time)"},
        /* StatusDeletedFmt */
        {L"%d 件をゴミ箱へ移動しました (Ctrl+Z で元に戻す)", L"Moved %d item(s) to Recycle Bin (Ctrl+Z to undo)"},
        /* StatusRestoredFmt */ {L"%d 件を復元しました", L"Restored %d item(s)"},
        /* StatusDeleteFailed */ {L"削除に失敗しました", L"Failed to delete"},
        /* ConfirmDeleteTitle */ {L"削除の確認", L"Confirm Delete"},
        /* ConfirmDeleteFmt */
        {L"%d 件をゴミ箱へ移動しますか?", L"Move %d item(s) to the Recycle Bin?"},
        /* MenuOptions */ {L"オプション(&O)", L"&Options"},
        /* MenuLanguage */ {L"言語 (Language)", L"Language"},
        /* MenuLangAuto */ {L"自動 (システム設定)", L"Auto (System)"},
        /* MenuLangJa */ {L"日本語", L"Japanese"},
        /* MenuLangEn */ {L"英語 (English)", L"English"},
        /* MenuTheme */ {L"テーマ", L"Theme"},
        /* ThemeDark */ {L"ダーク", L"Dark"},
        /* ThemeLight */ {L"ライト", L"Light"},
        /* MenuMode */ {L"パフォーマンスモード", L"Performance mode"},
        /* ModeStandard */ {L"標準 (可視 + 先読み)", L"Standard (visible + preload)"},
        /* ModeMassive */ {L"大量 (可視のみ)", L"Massive (visible only)"},
        /* ModePlayAll */ {L"全再生 (全ファイル)", L"Play all (every file)"},
        /* MenuSort */ {L"並び順", L"Sort"},
        /* SortName */ {L"名前", L"Name"},
        /* SortModified */ {L"更新日時", L"Modified time"},
        /* SortSize */ {L"ファイルサイズ", L"File size"},
        /* SortDescending */ {L"降順", L"Descending"},
        /* MenuConfirmDelete */ {L"削除時に確認する", L"Confirm before delete"},
        /* MenuDebugOverlay */
        {L"デバッグ情報を表示 (計測値)", L"Show debug overlay (metrics)"},
        /* MenuSeekbar */
        {L"拡大表示でシークバーを表示", L"Show seek bar in zoom view"},
        /* MenuShowFilenames */
        {L"選択ファイル名をステータスバーに表示", L"Show selected file name in status bar"},
        /* MenuIntroOffset */
        {L"イントロを飛ばす (5 分以上は 3:00 から)", L"Skip intro (5min+ starts at 3:00)"},
        /* MenuGridAudio */
        {L"実験的: 一覧で表示中の動画の音を再生 (最大 10)",
         L"Experimental: play audio for visible videos (up to 10)"},
        /* MenuGpuMemory */ {L"GPU メモリ使用率", L"GPU memory usage"},
        /* GpuMemoryFmt */ {L"VRAM 予算の %d%%", L"%d%% of VRAM budget"},
        /* MenuStorage */ {L"設定の保存先", L"Settings location"},
        /* StoragePortable */
        {L"EXE と同じフォルダ (ポータブル)", L"Next to the EXE (portable)"},
        /* StorageAppData */ {L"AppData (%APPDATA%\\Meguri)", L"AppData (%APPDATA%\\Meguri)"},
        /* StorageMoveFailed */
        {L"保存先を変更できませんでした (書き込み権限を確認してください)",
         L"Could not change the settings location (check write permission)"},
        /* MenuRowHeight */ {L"タイルの大きさ", L"Tile size"},
        /* RowHeightSmall */ {L"小", L"Small"},
        /* RowHeightMedium */ {L"中", L"Medium"},
        /* RowHeightLarge */ {L"大", L"Large"},
        /* RowHeightXLarge */ {L"特大", L"Extra large"},
        /* RowHeightStatusFmt */
        {L"タイル高さ: %d px (Ctrl+ホイールで調整)", L"Tile height: %d px (Ctrl+wheel to adjust)"},
        /* FolderPickTitle */ {L"表示するフォルダを選択", L"Select a folder to view"},
        /* LoadFailedTile */ {L"読込失敗", L"Load failed"},
    };
    return table[static_cast<int>(id)];
}

}  // namespace

void set_ui_language(UiLanguage language) { g_language = language; }
UiLanguage current_ui_language() { return g_language; }

const char* ui_language_name(UiLanguage language) {
    switch (language) {
        case UiLanguage::Japanese: return "ja";
        case UiLanguage::English: return "en";
        case UiLanguage::Auto: break;
    }
    return "auto";
}

UiLanguage ui_language_from_name(const char* name) {
    if (name && std::strcmp(name, "ja") == 0) return UiLanguage::Japanese;
    if (name && std::strcmp(name, "en") == 0) return UiLanguage::English;
    return UiLanguage::Auto;
}

const wchar_t* tr(Str id) {
    const Entry& e = entry_for(id);
    return resolve_japanese() ? e.ja : e.en;
}

}  // namespace meguri
