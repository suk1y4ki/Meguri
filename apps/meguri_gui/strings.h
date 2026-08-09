// UI 文字列テーブル (日英)。既定はシステム言語、メニューで切替。
#pragma once

namespace meguri {

enum class UiLanguage {
    Auto,  // システムの言語設定に従う
    Japanese,
    English,
};

// GUI の表示文字列 ID
enum class Str {
    AppTitle,
    OpenFolder,
    FilterWebp,
    FilterMp4,
    FilterWmv,
    FilterAvi,
    FilterPng,
    FilterJpeg,
    Recursive,
    StatusNoFolder,      // フォルダを開いてください
    StatusFmt,           // %d 件 (WEBP %d / MP4 %d / WMV %d / AVI %d / PNG %d / JPG %d)  選択 %d
    StatusLoadingFmt,    // 読み込み中... %d/%d
    StatusDeletedFmt,    // %d 件をゴミ箱へ移動しました (Ctrl+Z で元に戻す)
    StatusRestoredFmt,   // %d 件を復元しました
    StatusDeleteFailed,  // 削除に失敗しました
    ConfirmDeleteTitle,
    ConfirmDeleteFmt,  // %d 件をゴミ箱へ移動しますか?
    MenuOptions,
    MenuLanguage,
    MenuLangAuto,
    MenuLangJa,
    MenuLangEn,
    MenuTheme,
    ThemeDark,
    ThemeLight,
    MenuMode,
    ModeStandard,
    ModeMassive,
    ModePlayAll,
    MenuSort,
    SortName,
    SortModified,
    SortSize,
    SortDescending,
    MenuConfirmDelete,
    MenuDebugOverlay,
    MenuSeekbar,
    MenuShowFilenames,
    MenuCopyComfyMetadata,
    MenuIntroOffset,
    MenuGridAudio,
    MenuGpuMemory,
    GpuMemoryFmt,  // VRAM 予算の %d%% を使う
    MenuStorage,
    StoragePortable,
    StorageAppData,
    StorageMoveFailed,
    MenuRowHeight,
    RowHeightSmall,
    RowHeightMedium,
    RowHeightLarge,
    RowHeightXLarge,
    RowHeightStatusFmt,  // タイル高さ: %d px (Ctrl+ホイールで調整)
    StatusNoSelection,
    StatusCopiedFilesFmt,
    StatusCopiedComfyMetadata,
    StatusCopiedFilesNoMetadata,
    StatusCopyFailed,
    FolderPickTitle,
    LoadFailedTile,  // 読込失敗
};

// 言語を設定する (Auto はシステムの UI 言語から日本語/英語に解決)
void set_ui_language(UiLanguage language);
UiLanguage current_ui_language();

// 設定ファイル用の名前 ("auto" | "ja" | "en")
const char* ui_language_name(UiLanguage language);
UiLanguage ui_language_from_name(const char* name);

// 現在の言語の文字列を返す
const wchar_t* tr(Str id);

}  // namespace meguri
