// ダークモード適用ヘルパー (uxtheme の非公開 API + DWM)。
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace meguri {

// アプリ起動時に 1 回呼ぶ (SetPreferredAppMode ordinal 135)
void init_dark_mode_support(bool dark);

// トップレベルウィンドウのタイトルバーをダークにする (DWMWA=20)
void apply_dark_titlebar(HWND hwnd, bool dark);

// ボタン等の子コントロールにダークテーマを当てる
void apply_dark_control_theme(HWND control, bool dark);

// テーマを無効化する (チェックボックスを WM_CTLCOLOR* で自前配色するため)
void disable_control_theme(HWND control);

// テーマ変更を全子コントロールへ反映させる (SetWindowTheme 後に呼ぶ)
void broadcast_theme_changed(HWND parent);

}  // namespace meguri
