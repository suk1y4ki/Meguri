#include "main_window.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>

#include "dark_mode.h"
#include "io/mp4_decoder.h"  // set_gpu_memory_percent
#include "io/recycle.h"
#include "io/scanner.h"
#include "strings.h"

using Microsoft::WRL::ComPtr;

namespace meguri {

namespace {

constexpr wchar_t kClassName[] = L"MeguriMainWindow";

// コントロール / メニュー ID
enum : int {
    IDC_BTN_OPEN = 100,
    IDC_CHK_WEBP,
    IDC_CHK_MP4,
    IDC_CHK_PNG,
    IDC_CHK_JPEG,
    IDC_CHK_RECURSIVE,

    IDM_LANG_AUTO = 200,
    IDM_LANG_JA,
    IDM_LANG_EN,
    IDM_THEME_DARK = 210,
    IDM_THEME_LIGHT,
    IDM_MODE_STANDARD = 220,
    IDM_MODE_MASSIVE,
    IDM_MODE_PLAYALL,
    IDM_SORT_NAME = 230,
    IDM_SORT_MODIFIED,
    IDM_SORT_SIZE,
    IDM_SORT_DESC,
    IDM_CONFIRM_DELETE = 240,
    IDM_DEBUG_OVERLAY = 245,
    IDM_SEEKBAR = 246,
    IDM_INTRO_OFFSET = 247,
    IDM_GRID_AUDIO = 248,
    IDM_SHOW_FILENAMES = 249,
    IDM_ROWH_SMALL = 250,  // 小 / 中 / 大 / 特大 (連番)
    IDM_ROWH_MEDIUM,
    IDM_ROWH_LARGE,
    IDM_ROWH_XLARGE,
    IDM_GPUMEM_25 = 260,  // 25 / 50 / 75 / 100% (連番)
    IDM_GPUMEM_50,
    IDM_GPUMEM_75,
    IDM_GPUMEM_100,
    IDM_STORAGE_PORTABLE = 270,
    IDM_STORAGE_APPDATA,
};

constexpr UINT_PTR kStatusTimer = 2;

int toolbar_height(UINT dpi) { return MulDiv(36, dpi, 96); }
int statusbar_height(UINT dpi) { return MulDiv(26, dpi, 96); }

COLORREF chrome_color(bool dark) { return dark ? RGB(32, 32, 38) : RGB(243, 243, 245); }
COLORREF chrome_text_color(bool dark) { return dark ? RGB(230, 230, 235) : RGB(30, 30, 35); }

}  // namespace

bool MainWindow::create(HINSTANCE instance, int show_command,
                        const std::wstring& initial_folder) {
    instance_ = instance;
    settings_ = app::load_settings();
    probe_cache_.load();
    set_ui_language(ui_language_from_name(settings_.language.c_str()));
    init_dark_mode_support(settings_.theme == "dark");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // WM_ERASEBKGND で塗る
    // app.rc のアイコン (ID=1) をタイトルバー / タスクバーに出す
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    RegisterClassExW(&wc);

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1280, h = 800;
    if (settings_.window_width > 200 && settings_.window_height > 150) {
        x = settings_.window_x;
        y = settings_.window_y;
        w = settings_.window_width;
        h = settings_.window_height;
    }

    hwnd_ = CreateWindowExW(0, kClassName, tr(Str::AppTitle), WS_OVERLAPPEDWINDOW, x, y, w, h,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    ShowWindow(hwnd_, settings_.window_maximized ? SW_SHOWMAXIMIZED : show_command);
    UpdateWindow(hwnd_);

    // 引数のフォルダ > 前回のフォルダ の順で復元
    const std::wstring& folder =
        !initial_folder.empty() ? initial_folder : settings_.last_folder;
    if (!folder.empty()) {
        DWORD attr = GetFileAttributesW(folder.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            open_folder(folder);
        }
    }
    return true;
}

LRESULT CALLBACK MainWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    MainWindow* self;
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        self->on_create();
        return 0;
    }
    self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);
    return self->handle_message(msg, wparam, lparam);
}

void MainWindow::on_create() {
    dpi_ = GetDpiForWindow(hwnd_);

    // ボタン類はオーナードローで統一 (DarkMode_Explorer テーマは環境により
    // プッシュボタンに効かないことがあるため自前で描く)
    btn_open_ = CreateWindowExW(0, L"BUTTON", tr(Str::OpenFolder),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10,
                                10, hwnd_, reinterpret_cast<HMENU>(IDC_BTN_OPEN), instance_,
                                nullptr);
    // フィルタボタンはオーナードロー (BS_PUSHLIKE は DarkMode テーマが効かないため)。
    // チェック状態は settings_.show_webp / show_mp4 が持つ
    chk_webp_ = CreateWindowExW(0, L"BUTTON", tr(Str::FilterWebp),
                                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
                                reinterpret_cast<HMENU>(IDC_CHK_WEBP), instance_, nullptr);
    chk_mp4_ = CreateWindowExW(0, L"BUTTON", tr(Str::FilterMp4),
                               WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(IDC_CHK_MP4), instance_, nullptr);
    chk_png_ = CreateWindowExW(0, L"BUTTON", tr(Str::FilterPng),
                               WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(IDC_CHK_PNG), instance_, nullptr);
    chk_jpeg_ = CreateWindowExW(0, L"BUTTON", tr(Str::FilterJpeg),
                                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
                                reinterpret_cast<HMENU>(IDC_CHK_JPEG), instance_, nullptr);
    chk_recursive_ = CreateWindowExW(0, L"BUTTON", tr(Str::Recursive),
                                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 10, 10,
                                     hwnd_, reinterpret_cast<HMENU>(IDC_CHK_RECURSIVE),
                                     instance_, nullptr);
    Button_SetCheck(chk_recursive_, settings_.recursive ? BST_CHECKED : BST_UNCHECKED);

    grid_.create(hwnd_, instance_, &engine_);
    grid_.set_schedule_params(settings_.performance_mode);
    grid_.set_debug_overlay(settings_.debug_overlay);
    grid_.set_show_seekbar(settings_.show_seekbar);
    grid_.set_audio_state(settings_.audio_volume, settings_.audio_muted);
    grid_.set_grid_audio(settings_.grid_audio);
    grid_.on_audio_changed = [this](int volume, bool muted) {
        settings_.audio_volume = volume;
        settings_.audio_muted = muted;
    };
    engine_.set_intro_offset(settings_.intro_offset);
    io::set_gpu_memory_percent(settings_.gpu_memory_percent);
    grid_.on_selection_changed = [this] { update_status(); };
    grid_.on_delete_requested = [this] { delete_selection(); };
    grid_.on_undo_requested = [this] { undo_delete(); };
    grid_.on_row_height_wheel = [this](int notches) {
        // 1 ノッチで約 10% 拡縮。80〜800px にクランプ
        double height = settings_.target_row_height;
        for (int i = 0; i < notches; ++i) height *= 1.1;
        for (int i = 0; i > notches; --i) height /= 1.1;
        settings_.target_row_height = std::clamp(height, 80.0, 800.0);
        grid_.set_row_height(scaled_row_height());
        engine_.set_decode_limit(static_cast<int>(scaled_row_height() * 2.5));
        wchar_t text[128];
        swprintf(text, 128, tr(Str::RowHeightStatusFmt),
                 static_cast<int>(settings_.target_row_height));
        set_status(text);
    };

    update_dpi(dpi_);
    rebuild_menu();
    apply_theme();
    set_status(tr(Str::StatusNoFolder));

    DragAcceptFiles(hwnd_, TRUE);
    SetTimer(hwnd_, kStatusTimer, 500, nullptr);  // プローブ進捗などの定期更新
}

LRESULT MainWindow::handle_message(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_COMMAND:
            on_command(LOWORD(wparam));
            return 0;
        case WM_SIZE:
            layout_children();
            return 0;
        case WM_DPICHANGED: {
            update_dpi(HIWORD(wparam));
            const RECT* rc = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, rc->left, rc->top, rc->right - rc->left,
                         rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_ERASEBKGND: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            RECT rc;
            GetClientRect(hwnd_, &rc);
            HBRUSH brush = CreateSolidBrush(chrome_color(settings_.theme == "dark"));
            FillRect(dc, &rc, brush);
            DeleteObject(brush);
            return 1;
        }
        case WM_DRAWITEM: {
            // ボタンのオーナードロー (開く = アクションボタン、WEBP / MP4 = トグル)
            const DRAWITEMSTRUCT* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (dis->CtlID != IDC_CHK_WEBP && dis->CtlID != IDC_CHK_MP4 &&
                dis->CtlID != IDC_CHK_PNG && dis->CtlID != IDC_CHK_JPEG &&
                dis->CtlID != IDC_BTN_OPEN) {
                break;
            }
            const bool dark = settings_.theme == "dark";
            // アクションボタンは「非チェックのトグル」と同じ配色 (文字だけ明るめ)
            const bool is_toggle = dis->CtlID != IDC_BTN_OPEN;
            const bool checked =
                is_toggle && (dis->CtlID == IDC_CHK_WEBP   ? settings_.show_webp
                              : dis->CtlID == IDC_CHK_MP4  ? settings_.show_mp4
                              : dis->CtlID == IDC_CHK_PNG  ? settings_.show_png
                                                           : settings_.show_jpeg);
            const bool pressed = (dis->itemState & ODS_SELECTED) != 0;

            // チェック時はアクセント色、非チェックはくすんだパネル色
            COLORREF fill = checked ? RGB(45, 100, 175) : (dark ? RGB(52, 52, 60) : RGB(225, 225, 228));
            if (pressed) fill = checked ? RGB(35, 80, 145) : (dark ? RGB(70, 70, 80) : RGB(205, 205, 210));
            const COLORREF border = checked ? RGB(90, 150, 220)
                                            : (dark ? RGB(90, 90, 100) : RGB(160, 160, 165));
            const COLORREF text = (checked || !is_toggle)
                                      ? (dark || checked ? RGB(235, 235, 240) : RGB(40, 40, 45))
                                      : (dark ? RGB(200, 200, 205) : RGB(60, 60, 65));

            HBRUSH fill_brush = CreateSolidBrush(fill);
            FillRect(dis->hDC, &dis->rcItem, fill_brush);
            DeleteObject(fill_brush);
            HBRUSH border_brush = CreateSolidBrush(border);
            FrameRect(dis->hDC, &dis->rcItem, border_brush);
            DeleteObject(border_brush);

            SetTextColor(dis->hDC, text);
            SetBkMode(dis->hDC, TRANSPARENT);
            if (ui_font_) SelectObject(dis->hDC, ui_font_);
            wchar_t label[64] = L"";
            GetWindowTextW(dis->hwndItem, label, 64);
            RECT text_rc = dis->rcItem;
            DrawTextW(dis->hDC, label, -1, &text_rc,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            return TRUE;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            // チェックボックス (テーマ無効) の文字色と背景
            HDC dc = reinterpret_cast<HDC>(wparam);
            const bool dark = settings_.theme == "dark";
            SetTextColor(dc, chrome_text_color(dark));
            SetBkColor(dc, chrome_color(dark));
            static HBRUSH dark_brush = CreateSolidBrush(chrome_color(true));
            static HBRUSH light_brush = CreateSolidBrush(chrome_color(false));
            return reinterpret_cast<LRESULT>(dark ? dark_brush : light_brush);
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            // ステータスバー描画
            RECT rc;
            GetClientRect(hwnd_, &rc);
            RECT status_rc = rc;
            status_rc.top = rc.bottom - statusbar_height(dpi_);
            const bool dark = settings_.theme == "dark";
            HBRUSH brush = CreateSolidBrush(chrome_color(dark));
            FillRect(dc, &status_rc, brush);
            DeleteObject(brush);
            SetTextColor(dc, chrome_text_color(dark));
            SetBkMode(dc, TRANSPARENT);
            if (ui_font_) SelectObject(dc, ui_font_);
            status_rc.left += MulDiv(10, dpi_, 96);
            DrawTextW(dc, status_text_.c_str(), -1, &status_rc,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wparam == kStatusTimer) {
                // プローブ進捗・件数・選択数の定期更新 (常時。軽い処理なので無条件)
                update_status();
                return 0;
            }
            break;
        case WM_COPYDATA: {
            // E2E 用: dwData=1 でグリッドを PNG 保存 (lpData = 保存先ワイド文字パス)
            const COPYDATASTRUCT* cds = reinterpret_cast<COPYDATASTRUCT*>(lparam);
            if (cds && cds->dwData == 1 && cds->lpData && cds->cbData >= sizeof(wchar_t)) {
                std::wstring path(static_cast<const wchar_t*>(cds->lpData),
                                  cds->cbData / sizeof(wchar_t));
                while (!path.empty() && path.back() == L'\0') path.pop_back();
                return grid_.capture_to_png(path) ? 1 : 0;
            }
#ifdef _DEBUG
            // Debug ビルド限定の E2E テストフック
            if (cds && cds->dwData == 2) {  // テーマ・設定状態の診断
                return (settings_.theme == "dark" ? 1 : 0) + (grid_.theme_dark() ? 2 : 0) +
                       (!settings_.last_folder.empty() ? 4 : 0) +
                       (app::active_storage_location() == app::StorageLocation::Portable ? 8
                                                                                         : 0);
            }
            if (cds && cds->dwData == 3) {  // 選択削除
                delete_selection();
                return 1;
            }
            if (cds && cds->dwData == 4) {  // 直前の削除を復元
                undo_delete();
                return 1;
            }
            if (cds && cds->dwData == 5 && cds->lpData && cds->cbData >= sizeof(int)) {
                grid_.test_select(*static_cast<const int*>(cds->lpData));  // 単独選択
                return 1;
            }
            if (cds && cds->dwData == 6 && cds->lpData && cds->cbData >= sizeof(int)) {
                grid_.enter_zoom(*static_cast<const int*>(cds->lpData));  // ズーム開始
                return grid_.zoomed() ? 1 : 0;
            }
            if (cds && cds->dwData == 7 && cds->lpData && cds->cbData >= sizeof(int)) {
                grid_.zoom_step(*static_cast<const int*>(cds->lpData));  // ズーム中の前後送り
                return 1;
            }
            if (cds && cds->dwData == 8) {  // ズーム終了
                grid_.exit_zoom();
                return 1;
            }
            if (cds && cds->dwData == 10) {  // ズーム中の音声位置 (ms、無ければ -1→0)
                const int64_t pos = grid_.zoom_audio_position();
                return pos >= 0 ? static_cast<LRESULT>(pos) : -1;
            }
            if (cds && cds->dwData == 12 && cds->lpData && cds->cbData >= sizeof(int)) {
                grid_.test_set_hover(*static_cast<const int*>(cds->lpData));  // ホバー設定
                return 1;
            }
            if (cds && cds->dwData == 13) {  // 一覧音声で再生中のスロット数
                return grid_.grid_audio_count();
            }
#endif
            if (cds && cds->dwData == 9 && cds->lpData && cds->cbData >= sizeof(wchar_t)) {
                // エンジン状態のダンプ (読み取り専用の診断なので Release でも有効)
                std::wstring path(static_cast<const wchar_t*>(cds->lpData),
                                  cds->cbData / sizeof(wchar_t));
                while (!path.empty() && path.back() == L'\0') path.pop_back();
                return engine_.dump_state(path) ? 1 : 0;
            }
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wparam);
            wchar_t path[MAX_PATH];
            if (DragQueryFileW(drop, 0, path, MAX_PATH)) {
                std::wstring folder = path;
                const DWORD attr = GetFileAttributesW(path);
                if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                    const size_t pos = folder.find_last_of(L'\\');
                    if (pos != std::wstring::npos) folder = folder.substr(0, pos);
                }
                open_folder(folder);
            }
            DragFinish(drop);
            return 0;
        }
        case WM_CLOSE:
            save_settings_now();
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kStatusTimer);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void MainWindow::on_command(int id) {
    switch (id) {
        case IDC_BTN_OPEN:
            open_folder_dialog();
            return;
        case IDC_CHK_WEBP:
            settings_.show_webp = !settings_.show_webp;
            InvalidateRect(chk_webp_, nullptr, TRUE);
            rebuild_display_order();
            return;
        case IDC_CHK_MP4:
            settings_.show_mp4 = !settings_.show_mp4;
            InvalidateRect(chk_mp4_, nullptr, TRUE);
            rebuild_display_order();
            return;
        case IDC_CHK_PNG:
            settings_.show_png = !settings_.show_png;
            InvalidateRect(chk_png_, nullptr, TRUE);
            rebuild_display_order();
            return;
        case IDC_CHK_JPEG:
            settings_.show_jpeg = !settings_.show_jpeg;
            InvalidateRect(chk_jpeg_, nullptr, TRUE);
            rebuild_display_order();
            return;
        case IDC_CHK_RECURSIVE:
            settings_.recursive = Button_GetCheck(chk_recursive_) == BST_CHECKED;
            if (!current_folder_.empty()) open_folder(current_folder_);
            return;
        case IDM_LANG_AUTO:
        case IDM_LANG_JA:
        case IDM_LANG_EN:
            settings_.language = id == IDM_LANG_JA   ? "ja"
                                 : id == IDM_LANG_EN ? "en"
                                                     : "auto";
            set_ui_language(ui_language_from_name(settings_.language.c_str()));
            apply_language();
            return;
        case IDM_THEME_DARK:
        case IDM_THEME_LIGHT:
            settings_.theme = id == IDM_THEME_DARK ? "dark" : "light";
            apply_theme();
            return;
        case IDM_MODE_STANDARD:
        case IDM_MODE_MASSIVE:
        case IDM_MODE_PLAYALL:
            settings_.performance_mode = id == IDM_MODE_MASSIVE ? core::PerformanceMode::Massive
                                         : id == IDM_MODE_PLAYALL
                                             ? core::PerformanceMode::PlayAll
                                             : core::PerformanceMode::Standard;
            grid_.set_schedule_params(settings_.performance_mode);
            rebuild_menu();
            return;
        case IDM_SORT_NAME:
        case IDM_SORT_MODIFIED:
        case IDM_SORT_SIZE:
            settings_.sort_key = id == IDM_SORT_MODIFIED ? core::SortKey::ModifiedTime
                                 : id == IDM_SORT_SIZE   ? core::SortKey::FileSize
                                                         : core::SortKey::Name;
            rebuild_display_order();
            rebuild_menu();
            return;
        case IDM_SORT_DESC:
            settings_.sort_descending = !settings_.sort_descending;
            rebuild_display_order();
            rebuild_menu();
            return;
        case IDM_CONFIRM_DELETE:
            settings_.confirm_delete = !settings_.confirm_delete;
            rebuild_menu();
            return;
        case IDM_DEBUG_OVERLAY:
            settings_.debug_overlay = !settings_.debug_overlay;
            grid_.set_debug_overlay(settings_.debug_overlay);
            rebuild_menu();
            return;
        case IDM_SEEKBAR:
            settings_.show_seekbar = !settings_.show_seekbar;
            grid_.set_show_seekbar(settings_.show_seekbar);
            rebuild_menu();
            return;
        case IDM_SHOW_FILENAMES:
            settings_.show_filenames = !settings_.show_filenames;
            update_status();
            rebuild_menu();
            return;
        case IDM_INTRO_OFFSET:
            settings_.intro_offset = !settings_.intro_offset;
            engine_.set_intro_offset(settings_.intro_offset);
            rebuild_menu();
            return;
        case IDM_GRID_AUDIO:
            settings_.grid_audio = !settings_.grid_audio;
            grid_.set_grid_audio(settings_.grid_audio);
            rebuild_menu();
            return;
        case IDM_STORAGE_PORTABLE:
        case IDM_STORAGE_APPDATA: {
            const app::StorageLocation target = id == IDM_STORAGE_PORTABLE
                                                    ? app::StorageLocation::Portable
                                                    : app::StorageLocation::AppData;
            if (app::set_storage_location(target)) {
                // 移動後の場所にファイルを確定させる (無ければ新規作成)
                app::save_settings(settings_);
                probe_cache_.save();
            } else {
                set_status(tr(Str::StorageMoveFailed));
            }
            rebuild_menu();
            return;
        }
        case IDM_GPUMEM_25:
        case IDM_GPUMEM_50:
        case IDM_GPUMEM_75:
        case IDM_GPUMEM_100:
            settings_.gpu_memory_percent = (id - IDM_GPUMEM_25 + 1) * 25;
            io::set_gpu_memory_percent(settings_.gpu_memory_percent);
            rebuild_menu();
            return;
        case IDM_ROWH_SMALL:
        case IDM_ROWH_MEDIUM:
        case IDM_ROWH_LARGE:
        case IDM_ROWH_XLARGE:
            settings_.target_row_height = id == IDM_ROWH_SMALL     ? 120.0
                                          : id == IDM_ROWH_LARGE   ? 260.0
                                          : id == IDM_ROWH_XLARGE  ? 400.0
                                                                   : 180.0;
            grid_.set_row_height(scaled_row_height());
            engine_.set_decode_limit(static_cast<int>(scaled_row_height() * 2.5));
            rebuild_menu();
            return;
    }
}

void MainWindow::layout_children() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int tb = toolbar_height(dpi_);
    const int sb = statusbar_height(dpi_);
    const int pad = MulDiv(4, dpi_, 96);
    const int btn_h = tb - pad * 2;

    int x = pad;
    auto place = [&](HWND ctl, int width) {
        MoveWindow(ctl, x, pad, width, btn_h, TRUE);
        x += width + pad;
    };
    place(btn_open_, MulDiv(130, dpi_, 96));
    place(chk_webp_, MulDiv(70, dpi_, 96));
    place(chk_mp4_, MulDiv(70, dpi_, 96));
    place(chk_png_, MulDiv(70, dpi_, 96));
    place(chk_jpeg_, MulDiv(70, dpi_, 96));
    place(chk_recursive_, MulDiv(130, dpi_, 96));

    RECT grid_rc = rc;
    grid_rc.top = tb;
    grid_rc.bottom = rc.bottom - sb;
    grid_.move_to(grid_rc);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::update_dpi(UINT dpi) {
    dpi_ = dpi;
    if (ui_font_) DeleteObject(ui_font_);
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(12, dpi_, 96);
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    ui_font_ = CreateFontIndirectW(&lf);
    for (HWND ctl : {btn_open_, chk_webp_, chk_mp4_, chk_png_, chk_jpeg_, chk_recursive_}) {
        SendMessageW(ctl, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    }
    grid_.set_row_height(scaled_row_height());
    // タイル表示に必要な解像度までデコードを縮める (ズーム時は原寸に切替わる)。
    // 2.5 倍はジャスティファイの引き伸ばし + 横長タイルの幅を見込んだ余裕
    engine_.set_decode_limit(static_cast<int>(scaled_row_height() * 2.5));
    layout_children();
}

double MainWindow::scaled_row_height() const {
    return settings_.target_row_height * dpi_ / 96.0;
}

void MainWindow::open_folder_dialog() {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&dialog)))) {
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    dialog->SetTitle(tr(Str::FolderPickTitle));
    if (FAILED(dialog->Show(hwnd_))) return;  // キャンセル

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    std::wstring folder = path;
    CoTaskMemFree(path);
    open_folder(folder);
}

void MainWindow::open_folder(const std::wstring& folder) {
    current_folder_ = folder;
    settings_.last_folder = folder;

    io::ScanOptions options;
    options.recursive = settings_.recursive;
    library_ = io::scan_folder(folder, options);
    probe_cache_.apply(&library_);  // キャッシュ一致分はプローブ不要になる
    deleted_.assign(library_.size(), false);
    undo_stack_.clear();

    engine_.set_library(library_);  // コピーを渡す (tile が自分の item を持つ)
    probing_ = true;
    rebuild_display_order();

    std::wstring title = std::wstring(tr(Str::AppTitle)) + L" — " + folder;
    SetWindowTextW(hwnd_, title.c_str());
    update_status();
}

void MainWindow::rebuild_display_order() {
    core::FilterOptions filter;
    filter.show_webp = settings_.show_webp;
    filter.show_mp4 = settings_.show_mp4;
    filter.show_png = settings_.show_png;
    filter.show_jpeg = settings_.show_jpeg;
    core::SortOptions sort;
    sort.key = settings_.sort_key;
    sort.descending = settings_.sort_descending;

    std::vector<int> order = core::build_display_order(library_, filter, sort);
    order.erase(std::remove_if(order.begin(), order.end(),
                               [this](int i) { return deleted_[i]; }),
                order.end());
    grid_.set_display_order(std::move(order));
    update_status();
}

void MainWindow::delete_selection() {
    std::vector<int> engine_indices = grid_.selected_engine_indices();
    if (engine_indices.empty()) return;

    if (settings_.confirm_delete) {
        wchar_t text[256];
        swprintf(text, 256, tr(Str::ConfirmDeleteFmt),
                 static_cast<int>(engine_indices.size()));
        if (MessageBoxW(hwnd_, text, tr(Str::ConfirmDeleteTitle),
                        MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
            return;
        }
    }

    DeleteBatch batch;
    for (int i : engine_indices) {
        if (i >= 0 && i < static_cast<int>(library_.size())) {
            batch.engine_indices.push_back(i);
            batch.paths.push_back(library_[i].path);
        }
    }

    std::wstring error;
    if (!io::send_to_recycle_bin(batch.paths, &error)) {
        set_status(std::wstring(tr(Str::StatusDeleteFailed)) + L": " + error);
        return;
    }
    for (int i : batch.engine_indices) deleted_[i] = true;
    const int count = static_cast<int>(batch.paths.size());
    undo_stack_.push_back(std::move(batch));
    grid_.clear_selection();
    rebuild_display_order();

    wchar_t text[256];
    swprintf(text, 256, tr(Str::StatusDeletedFmt), count);
    set_status(text);
}

void MainWindow::undo_delete() {
    if (undo_stack_.empty()) return;
    DeleteBatch batch = std::move(undo_stack_.back());
    undo_stack_.pop_back();

    const int restored = io::restore_from_recycle_bin(batch.paths);
    if (restored > 0) {
        for (int i : batch.engine_indices) deleted_[i] = false;
        rebuild_display_order();
    }
    wchar_t text[256];
    swprintf(text, 256, tr(Str::StatusRestoredFmt), restored);
    set_status(text);
}

void MainWindow::rebuild_menu() {
    HMENU bar = CreateMenu();
    HMENU options = CreatePopupMenu();

    HMENU lang = CreatePopupMenu();
    AppendMenuW(lang, MF_STRING, IDM_LANG_AUTO, tr(Str::MenuLangAuto));
    AppendMenuW(lang, MF_STRING, IDM_LANG_JA, tr(Str::MenuLangJa));
    AppendMenuW(lang, MF_STRING, IDM_LANG_EN, tr(Str::MenuLangEn));
    const UINT lang_checked = settings_.language == "ja"   ? IDM_LANG_JA
                              : settings_.language == "en" ? IDM_LANG_EN
                                                           : IDM_LANG_AUTO;
    CheckMenuRadioItem(lang, IDM_LANG_AUTO, IDM_LANG_EN, lang_checked, MF_BYCOMMAND);

    HMENU theme = CreatePopupMenu();
    AppendMenuW(theme, MF_STRING, IDM_THEME_DARK, tr(Str::ThemeDark));
    AppendMenuW(theme, MF_STRING, IDM_THEME_LIGHT, tr(Str::ThemeLight));
    CheckMenuRadioItem(theme, IDM_THEME_DARK, IDM_THEME_LIGHT,
                       settings_.theme == "dark" ? IDM_THEME_DARK : IDM_THEME_LIGHT,
                       MF_BYCOMMAND);

    HMENU mode = CreatePopupMenu();
    AppendMenuW(mode, MF_STRING, IDM_MODE_STANDARD, tr(Str::ModeStandard));
    AppendMenuW(mode, MF_STRING, IDM_MODE_MASSIVE, tr(Str::ModeMassive));
    AppendMenuW(mode, MF_STRING, IDM_MODE_PLAYALL, tr(Str::ModePlayAll));
    const UINT mode_checked =
        settings_.performance_mode == core::PerformanceMode::Massive   ? IDM_MODE_MASSIVE
        : settings_.performance_mode == core::PerformanceMode::PlayAll ? IDM_MODE_PLAYALL
                                                                       : IDM_MODE_STANDARD;
    CheckMenuRadioItem(mode, IDM_MODE_STANDARD, IDM_MODE_PLAYALL, mode_checked, MF_BYCOMMAND);

    HMENU sort = CreatePopupMenu();
    AppendMenuW(sort, MF_STRING, IDM_SORT_NAME, tr(Str::SortName));
    AppendMenuW(sort, MF_STRING, IDM_SORT_MODIFIED, tr(Str::SortModified));
    AppendMenuW(sort, MF_STRING, IDM_SORT_SIZE, tr(Str::SortSize));
    AppendMenuW(sort, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sort, MF_STRING | (settings_.sort_descending ? MF_CHECKED : 0), IDM_SORT_DESC,
                tr(Str::SortDescending));
    const UINT sort_checked = settings_.sort_key == core::SortKey::ModifiedTime
                                  ? IDM_SORT_MODIFIED
                              : settings_.sort_key == core::SortKey::FileSize ? IDM_SORT_SIZE
                                                                              : IDM_SORT_NAME;
    CheckMenuRadioItem(sort, IDM_SORT_NAME, IDM_SORT_SIZE, sort_checked, MF_BYCOMMAND);

    HMENU gpumem = CreatePopupMenu();
    for (int i = 0; i < 4; ++i) {
        wchar_t label[64];
        swprintf(label, 64, tr(Str::GpuMemoryFmt), (i + 1) * 25);
        AppendMenuW(gpumem, MF_STRING, IDM_GPUMEM_25 + i, label);
    }
    int gpumem_checked = IDM_GPUMEM_50;
    if (settings_.gpu_memory_percent <= 25) gpumem_checked = IDM_GPUMEM_25;
    else if (settings_.gpu_memory_percent <= 50) gpumem_checked = IDM_GPUMEM_50;
    else if (settings_.gpu_memory_percent <= 75) gpumem_checked = IDM_GPUMEM_75;
    else gpumem_checked = IDM_GPUMEM_100;
    CheckMenuRadioItem(gpumem, IDM_GPUMEM_25, IDM_GPUMEM_100, gpumem_checked, MF_BYCOMMAND);

    HMENU rowh = CreatePopupMenu();
    AppendMenuW(rowh, MF_STRING, IDM_ROWH_SMALL, tr(Str::RowHeightSmall));
    AppendMenuW(rowh, MF_STRING, IDM_ROWH_MEDIUM, tr(Str::RowHeightMedium));
    AppendMenuW(rowh, MF_STRING, IDM_ROWH_LARGE, tr(Str::RowHeightLarge));
    AppendMenuW(rowh, MF_STRING, IDM_ROWH_XLARGE, tr(Str::RowHeightXLarge));
    const UINT rowh_checked = settings_.target_row_height <= 140.0   ? IDM_ROWH_SMALL
                              : settings_.target_row_height <= 215.0 ? IDM_ROWH_MEDIUM
                              : settings_.target_row_height <= 320.0 ? IDM_ROWH_LARGE
                                                                     : IDM_ROWH_XLARGE;
    CheckMenuRadioItem(rowh, IDM_ROWH_SMALL, IDM_ROWH_XLARGE, rowh_checked, MF_BYCOMMAND);

    AppendMenuW(options, MF_POPUP, reinterpret_cast<UINT_PTR>(lang), tr(Str::MenuLanguage));
    AppendMenuW(options, MF_POPUP, reinterpret_cast<UINT_PTR>(theme), tr(Str::MenuTheme));
    AppendMenuW(options, MF_POPUP, reinterpret_cast<UINT_PTR>(mode), tr(Str::MenuMode));
    AppendMenuW(options, MF_POPUP, reinterpret_cast<UINT_PTR>(gpumem), tr(Str::MenuGpuMemory));

    HMENU storage = CreatePopupMenu();
    AppendMenuW(storage, MF_STRING, IDM_STORAGE_PORTABLE, tr(Str::StoragePortable));
    AppendMenuW(storage, MF_STRING, IDM_STORAGE_APPDATA, tr(Str::StorageAppData));
    CheckMenuRadioItem(storage, IDM_STORAGE_PORTABLE, IDM_STORAGE_APPDATA,
                       app::active_storage_location() == app::StorageLocation::Portable
                           ? IDM_STORAGE_PORTABLE
                           : IDM_STORAGE_APPDATA,
                       MF_BYCOMMAND);
    AppendMenuW(options, MF_POPUP, reinterpret_cast<UINT_PTR>(storage), tr(Str::MenuStorage));
    AppendMenuW(options, MF_POPUP, reinterpret_cast<UINT_PTR>(sort), tr(Str::MenuSort));
    AppendMenuW(options, MF_POPUP, reinterpret_cast<UINT_PTR>(rowh), tr(Str::MenuRowHeight));
    AppendMenuW(options, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(options, MF_STRING | (settings_.confirm_delete ? MF_CHECKED : 0),
                IDM_CONFIRM_DELETE, tr(Str::MenuConfirmDelete));
    AppendMenuW(options, MF_STRING | (settings_.show_seekbar ? MF_CHECKED : 0), IDM_SEEKBAR,
                tr(Str::MenuSeekbar));
    AppendMenuW(options, MF_STRING | (settings_.show_filenames ? MF_CHECKED : 0),
                IDM_SHOW_FILENAMES, tr(Str::MenuShowFilenames));
    AppendMenuW(options, MF_STRING | (settings_.intro_offset ? MF_CHECKED : 0),
                IDM_INTRO_OFFSET, tr(Str::MenuIntroOffset));
    AppendMenuW(options, MF_STRING | (settings_.grid_audio ? MF_CHECKED : 0), IDM_GRID_AUDIO,
                tr(Str::MenuGridAudio));
    AppendMenuW(options, MF_STRING | (settings_.debug_overlay ? MF_CHECKED : 0),
                IDM_DEBUG_OVERLAY, tr(Str::MenuDebugOverlay));

    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(options), tr(Str::MenuOptions));

    HMENU old = GetMenu(hwnd_);
    SetMenu(hwnd_, bar);
    if (old) DestroyMenu(old);
}

void MainWindow::apply_language() {
    SetWindowTextW(btn_open_, tr(Str::OpenFolder));
    SetWindowTextW(chk_webp_, tr(Str::FilterWebp));
    SetWindowTextW(chk_mp4_, tr(Str::FilterMp4));
    SetWindowTextW(chk_png_, tr(Str::FilterPng));
    SetWindowTextW(chk_jpeg_, tr(Str::FilterJpeg));
    SetWindowTextW(chk_recursive_, tr(Str::Recursive));
    if (current_folder_.empty()) {
        SetWindowTextW(hwnd_, tr(Str::AppTitle));
        set_status(tr(Str::StatusNoFolder));
    } else {
        std::wstring title = std::wstring(tr(Str::AppTitle)) + L" — " + current_folder_;
        SetWindowTextW(hwnd_, title.c_str());
        update_status();
    }
    rebuild_menu();
}

void MainWindow::apply_theme() {
    const bool dark = settings_.theme == "dark";
    init_dark_mode_support(dark);  // ForceDark + FlushMenuThemes + Refresh (毎回呼び直す)
    apply_dark_titlebar(hwnd_, dark);
    // ボタン類 (開く / WEBP / MP4) はオーナードローなのでテーマ不要 (WM_DRAWITEM が配色)
    apply_dark_control_theme(grid_.hwnd(), dark);  // スクロールバー
    // 通常チェックボックスはテーマを外して WM_CTLCOLORBTN で描く (白文字にするため)
    if (dark) {
        disable_control_theme(chk_recursive_);
    } else {
        apply_dark_control_theme(chk_recursive_, false);
    }
    broadcast_theme_changed(hwnd_);  // これが無いと DarkMode_* が反映されない
    grid_.set_theme(dark);
    InvalidateRect(hwnd_, nullptr, TRUE);
    RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
}

void MainWindow::update_status() {
    if (library_.empty()) {
        set_status(tr(Str::StatusNoFolder));
        probing_ = false;
        return;
    }
    const int total = engine_.tile_count();
    const int probed = engine_.probed_count();
    if (probing_ && probed < total) {
        wchar_t text[128];
        swprintf(text, 128, tr(Str::StatusLoadingFmt), probed, total);
        set_status(text);
        return;
    }
    if (probing_) {
        // プローブ完了 → 結果をキャッシュへ保存 (次回オープンを即時化)
        probing_ = false;
        store_probe_results();
    }

    int shown = 0, webp = 0, mp4 = 0, png = 0, jpeg = 0;
    for (int engine_index : grid_.display_order()) {
        if (engine_index < 0 || engine_index >= static_cast<int>(library_.size())) continue;
        ++shown;
        switch (library_[engine_index].type) {
            case core::MediaType::Webp: ++webp; break;
            case core::MediaType::Mp4: ++mp4; break;
            case core::MediaType::Png: ++png; break;
            case core::MediaType::Jpeg: ++jpeg; break;
        }
    }
    wchar_t text[192];
    swprintf(text, 192, tr(Str::StatusFmt), shown, webp, mp4, png, jpeg,
             static_cast<int>(grid_.selection_count()));
    std::wstring status = text;
    if (settings_.show_filenames && grid_.selection_count() > 0) {
        const std::vector<int> selected = grid_.selected_engine_indices();
        if (!selected.empty()) {
            const int first = selected.front();
            if (first >= 0 && first < static_cast<int>(library_.size())) {
                const std::wstring& path = library_[first].path;
                const size_t pos = path.find_last_of(L"\\/");
                const std::wstring name =
                    pos == std::wstring::npos ? path : path.substr(pos + 1);
                status += L"   ";
                status += name;
                if (selected.size() > 1) {
                    status += L" +";
                    status += std::to_wstring(selected.size() - 1);
                }
            }
        }
    }
    set_status(status);
}

void MainWindow::store_probe_results() {
    std::vector<core::MediaItem> items;
    items.reserve(engine_.tile_count());
    for (int i = 0; i < engine_.tile_count(); ++i) {
        items.push_back(engine_.item_snapshot(i));
    }
    if (probe_cache_.update(items)) probe_cache_.save();
}

void MainWindow::set_status(const std::wstring& text) {
    status_text_ = text;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    rc.top = rc.bottom - statusbar_height(dpi_);
    InvalidateRect(hwnd_, &rc, FALSE);
}

void MainWindow::save_settings_now() {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd_, &wp)) {
        settings_.window_maximized = wp.showCmd == SW_SHOWMAXIMIZED;
        settings_.window_x = wp.rcNormalPosition.left;
        settings_.window_y = wp.rcNormalPosition.top;
        settings_.window_width = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        settings_.window_height = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    }
    app::save_settings(settings_);
}

}  // namespace meguri
