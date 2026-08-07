#include "dark_mode.h"

#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace meguri {

void init_dark_mode_support(bool dark) {
    // uxtheme の未公開 API (1809 以降で定番の方法)。
    // 135 = SetPreferredAppMode, 104 = RefreshImmersiveColorPolicyState,
    // 136 = FlushMenuThemes。テーマ切替のたびに呼び直す必要がある
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using VoidFn = void(WINAPI*)();
    auto set_mode = reinterpret_cast<SetPreferredAppModeFn>(
        GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    auto refresh = reinterpret_cast<VoidFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(104)));
    auto flush_menus = reinterpret_cast<VoidFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    if (set_mode) set_mode(dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
    if (flush_menus) flush_menus();
    if (refresh) refresh();
}

void apply_dark_titlebar(HWND hwnd, bool dark) {
    const BOOL value = dark ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE (=20, Windows 10 2004+)
    DwmSetWindowAttribute(hwnd, 20, &value, sizeof(value));
}

void apply_dark_control_theme(HWND control, bool dark) {
    SetWindowTheme(control, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

void disable_control_theme(HWND control) { SetWindowTheme(control, L" ", L" "); }

void broadcast_theme_changed(HWND parent) {
    EnumChildWindows(
        parent,
        [](HWND child, LPARAM) -> BOOL {
            SendMessageW(child, WM_THEMECHANGED, 0, 0);
            return TRUE;
        },
        0);
}

}  // namespace meguri
