#include "grid_view.h"

#include <d3d11.h>
#include <wincodec.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "io/d3d_video.h"
#include "io/mp4_decoder.h"
#include "strings.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

// メモ: d2d1.h も winuser.h の DrawText マクロの影響下でコンパイルされるため、
// UNICODE ビルドでは ID2D1RenderTarget のメソッド実名は DrawTextW になる

using Microsoft::WRL::ComPtr;

namespace meguri {

namespace {

constexpr wchar_t kClassName[] = L"MeguriGridView";
constexpr UINT_PTR kRenderTimer = 1;
constexpr UINT kRenderIntervalMs = 15;  // 約 60fps
constexpr double kGap = 4.0;

D2D1_COLOR_F bg_color(bool dark) {
    return dark ? D2D1::ColorF(0.086f, 0.086f, 0.102f) : D2D1::ColorF(0.94f, 0.94f, 0.95f);
}
D2D1_COLOR_F placeholder_color(bool dark) {
    return dark ? D2D1::ColorF(0.16f, 0.16f, 0.19f) : D2D1::ColorF(0.85f, 0.85f, 0.87f);
}
D2D1_COLOR_F accent_color() { return D2D1::ColorF(0.30f, 0.62f, 1.0f); }

}  // namespace

GridView::~GridView() {
    if (hwnd_) KillTimer(hwnd_, kRenderTimer);
}

bool GridView::create(HWND parent, HINSTANCE instance, PlaybackEngine* engine) {
    engine_ = engine;
    start_tick_ = static_cast<int64_t>(GetTickCount64());

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;  // ダブルクリックでズーム
    RegisterClassW(&wc);  // 二重登録は無視される

    hwnd_ = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
                            0, 0, 100, 100, parent, nullptr, instance, this);
    if (!hwnd_) return false;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 __uuidof(ID2D1Factory1), nullptr,
                                 reinterpret_cast<void**>(d2d_factory_.GetAddressOf()))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf()))))
        return false;
    dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f,
                                      L"", text_format_.GetAddressOf());
    if (text_format_) {
        text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    // ズーム UI は DPI に合わせて大きめに (ラベル・ミュートアイコンに使う)
    const float zoom_font = 16.0f * (GetDpiForWindow(hwnd_) / 96.0f);
    dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      zoom_font, L"", zoom_label_format_.GetAddressOf());
    if (zoom_label_format_) {
        zoom_label_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        zoom_label_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        zoom_label_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    dwrite_factory_->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f,
                                      L"", debug_format_.GetAddressOf());
    if (debug_format_) {
        debug_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        debug_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        debug_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    SetTimer(hwnd_, kRenderTimer, kRenderIntervalMs, nullptr);
    return true;
}

LRESULT CALLBACK GridView::wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    GridView* self;
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<GridView*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<GridView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);
    return self->handle_message(msg, wparam, lparam);
}

LRESULT GridView::handle_message(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_TIMER:
            if (wparam == kRenderTimer) {
                render();
                return 0;
            }
            break;
        case WM_PAINT: {
            ValidateRect(hwnd_, nullptr);
            render();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (zero_copy_ && swapchain_) {
                // バックバッファ参照を外してからリサイズし、ターゲットを作り直す
                d2d_ctx_->SetTarget(nullptr);
                target_bitmap_.Reset();
                if (FAILED(swapchain_->ResizeBuffers(0, std::max<UINT>(LOWORD(lparam), 8),
                                                     std::max<UINT>(HIWORD(lparam), 8),
                                                     DXGI_FORMAT_UNKNOWN, 0)) ||
                    !create_swapchain_target()) {
                    release_zero_copy();  // 従来経路へフォールバック
                    zero_copy_tried_ = false;  // 次の render で再試行
                }
            } else if (render_target_) {
                // Resize 失敗でエラーステートに入ると以後の描画が全て失敗するため、
                // 失敗したら作り直す (次の render で再生成される)
                if (FAILED(render_target_->Resize(
                        D2D1::SizeU(LOWORD(lparam), HIWORD(lparam))))) {
                    render_target_.Reset();
                    brush_.Reset();
                    bitmaps_.clear();
                }
            }
            layout_dirty_ = true;
            return 0;
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            if (!zoomed() && (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL)) {
                // Ctrl+ホイールでタイルの大きさを調整
                if (on_row_height_wheel) on_row_height_wheel(delta / 120);
                return 0;
            }
            if (zoomed()) {
                zoom_step(delta < 0 ? 1 : -1);  // ズーム中は前後送り
            } else {
                scroll_by(-delta / 120.0 * row_height_ * 0.9);
            }
            return 0;
        }
        case WM_VSCROLL: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd_, SB_VERT, &si);
            switch (LOWORD(wparam)) {
                case SB_LINEUP: scroll_by(-60); break;
                case SB_LINEDOWN: scroll_by(60); break;
                case SB_PAGEUP: scroll_by(-static_cast<double>(si.nPage)); break;
                case SB_PAGEDOWN: scroll_by(static_cast<double>(si.nPage)); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: scroll_to(si.nTrackPos); break;
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (zoomed()) {
                handle_seek_input(pt, true);  // シークバー以外のクリックは何もしない
                return 0;
            }
            handle_click(pt, (wparam & MK_CONTROL) != 0, (wparam & MK_SHIFT) != 0);
            return 0;
        }
        case WM_LBUTTONUP:
            if (seek_dragging_ || volume_dragging_) {
                seek_dragging_ = false;
                volume_dragging_ = false;
                ReleaseCapture();
            }
            return 0;
        case WM_LBUTTONDBLCLK: {
            SetFocus(hwnd_);
            const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (zoomed()) {
                if (handle_seek_input(pt, true)) return 0;  // バー上の連打はシーク扱い
                exit_zoom();
                return 0;
            }
            const int index = hit_test(pt);
            if (index >= 0) enter_zoom(index);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (seek_dragging_ || volume_dragging_) {
                handle_seek_input(pt, false);
                return 0;
            }
            hover_index_ = hit_test(pt);
            if (!mouse_tracking_) {
                // マウスが離れたらホバー (と一覧ホバー音声) を確実に解除する
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&tme);
                mouse_tracking_ = true;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_index_ = -1;
            mouse_tracking_ = false;
            return 0;
        case WM_KEYDOWN:
            handle_key(wparam);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_DESTROY:
            KillTimer(hwnd_, kRenderTimer);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void GridView::set_display_order(std::vector<int> order) {
    display_order_ = std::move(order);
    selection_.clear();
    layout_dirty_ = true;
    // engine index が別ライブラリを指し得るため、キャッシュは全部作り直す。
    // last_active_ を空にすると次の render で set_active が必ず走り、
    // エンジン側の内部リストとの差分で不要なデコーダが閉じられる
    bitmaps_.clear();
    gpu_bitmaps_.clear();
    last_active_.clear();
    stop_grid_audio();  // engine index が変わるため一覧音声も仕切り直す

    // ズーム中は同じ表示位置を維持する (削除後は次のアイテムがその位置に来る)
    if (zoomed()) {
        const int count = static_cast<int>(display_order_.size());
        if (count == 0) {
            zoom_display_index_ = -1;
            set_zoom_fullres(-1);
            stop_zoom_audio();
        } else {
            zoom_display_index_ = std::min(zoom_display_index_, count - 1);
            selection_.click(zoom_display_index_, false, false);
            const int engine_index = display_order_[zoom_display_index_];
            const bool same_target = zoom_fullres_engine_ == engine_index;
            if (!same_target) reset_zoom_pause();
            set_zoom_fullres(engine_index);
            if (!same_target) start_zoom_audio();  // 削除で対象が変わった場合など
        }
    }
    if (on_selection_changed) on_selection_changed();
    // スクロール位置は範囲内にクランプされる (update_scroll_info)
}

void GridView::set_theme(bool dark) {
    dark_ = dark;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GridView::set_row_height(double height) {
    // 表示位置がなるべくずれないようスクロールも比例させる
    if (row_height_ > 0 && height > 0) scroll_y_ *= height / row_height_;
    row_height_ = height;
    layout_dirty_ = true;
}

void GridView::set_schedule_params(core::PerformanceMode mode) { mode_ = mode; }

void GridView::move_to(const RECT& rc) {
    MoveWindow(hwnd_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

std::vector<int> GridView::selected_engine_indices() const {
    std::vector<int> result;
    for (int display_index : selection_.items()) {
        if (display_index >= 0 && display_index < static_cast<int>(display_order_.size())) {
            result.push_back(display_order_[display_index]);
        }
    }
    return result;
}

void GridView::clear_selection() {
    selection_.clear();
    if (on_selection_changed) on_selection_changed();
}

void GridView::test_select(int display_index) {
    selection_.click(display_index, false, false);
    if (on_selection_changed) on_selection_changed();
}

bool GridView::init_zero_copy() {
    ID3D11Device* device = io::shared_d3d_device();
    if (!device) return false;

    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) return false;
    if (FAILED(d2d_factory_->CreateDevice(dxgi_device.Get(), &d2d_device_))) return false;
    if (FAILED(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_ctx_))) {
        return false;
    }
    d2d_ctx_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);  // 自前レイアウトはピクセル座標

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = std::max<UINT>(rc.right - rc.left, 8);
    desc.Height = std::max<UINT>(rc.bottom - rc.top, 8);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    if (FAILED(factory->CreateSwapChainForHwnd(device, hwnd_, &desc, nullptr, nullptr,
                                               &swapchain_))) {
        return false;
    }
    // Alt+Enter 等の DXGI 側フルスクリーン遷移は使わない
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    if (!create_swapchain_target()) return false;
    if (FAILED(d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                               brush_.ReleaseAndGetAddressOf()))) {
        return false;
    }
    return true;
}

bool GridView::create_swapchain_target() {
    ComPtr<IDXGISurface> surface;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    if (FAILED(d2d_ctx_->CreateBitmapFromDxgiSurface(surface.Get(), &props,
                                                     target_bitmap_.ReleaseAndGetAddressOf()))) {
        return false;
    }
    d2d_ctx_->SetTarget(target_bitmap_.Get());
    return true;
}

void GridView::release_zero_copy() {
    if (d2d_ctx_) d2d_ctx_->SetTarget(nullptr);
    target_bitmap_.Reset();
    gpu_bitmaps_.clear();
    bitmaps_.clear();
    brush_.Reset();
    swapchain_.Reset();
    d2d_ctx_.Reset();
    d2d_device_.Reset();
    zero_copy_ = false;
    if (engine_) engine_->set_zero_copy(false);
}

ID2D1Bitmap1* GridView::gpu_bitmap_for(int engine_index, int slot, ID3D11Texture2D* texture) {
    GpuTileBitmaps& entry = gpu_bitmaps_[engine_index];
    if (entry.texture[slot] == texture && entry.bitmap[slot]) {
        return entry.bitmap[slot].Get();
    }
    entry.texture[slot] = nullptr;
    entry.bitmap[slot].Reset();
    ComPtr<IDXGISurface> surface;
    if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&surface)))) return nullptr;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    if (FAILED(d2d_ctx_->CreateBitmapFromDxgiSurface(surface.Get(), &props,
                                                     entry.bitmap[slot].GetAddressOf()))) {
        return nullptr;
    }
    entry.texture[slot] = texture;
    return entry.bitmap[slot].Get();
}

void GridView::ensure_render_target() {
    // まずゼロコピー経路を試す (共有 D3D デバイス上の D2D)。
    // 失敗した環境・MEGURI_NO_ZEROCOPY 指定時は従来の HwndRenderTarget
    if (!zero_copy_tried_) {
        zero_copy_tried_ = true;
        wchar_t disable[8];
        if (GetEnvironmentVariableW(L"MEGURI_NO_ZEROCOPY", disable, 8) == 0) {
            if (init_zero_copy()) {
                zero_copy_ = true;
                if (engine_) engine_->set_zero_copy(true);
            } else {
                release_zero_copy();
            }
        }
    }
    if (zero_copy_) {
        if (!target_bitmap_) {
            if (!create_swapchain_target()) release_zero_copy();
        }
        return;
    }

    if (render_target_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
    if (FAILED(d2d_factory_->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f),  // DPI スケールは自前レイアウトで行う (ピクセル座標描画)
            D2D1::HwndRenderTargetProperties(hwnd_, size),
            render_target_.GetAddressOf()))) {
        return;
    }
    rt()->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                          brush_.ReleaseAndGetAddressOf());
    bitmaps_.clear();  // RT 依存リソースを作り直す
    // 各タイルが保持中の最新フレームを再アップロードさせる
    // (RT 再生成後にプレースホルダへ戻ってしまわないように)
    if (engine_) {
        for (int i = 0; i < engine_->tile_count(); ++i) {
            if (auto* t = engine_->tile(i)) t->shown_version = 0;
        }
    }
}

void GridView::rebuild_layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    core::LayoutParams params;
    params.viewport_width = static_cast<double>(rc.right - rc.left);
    params.target_row_height = row_height_;
    params.gap = kGap;

    std::vector<double> aspects;
    aspects.reserve(display_order_.size());
    for (int engine_index : display_order_) {
        auto* t = engine_->tile(engine_index);
        aspects.push_back(t ? core::aspect_ratio(t->item) : 1.0);
    }
    layout_ = core::compute_justified_layout(aspects, params);
    layout_dirty_ = false;
    update_scroll_info();
}

void GridView::update_scroll_info() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int view_h = rc.bottom - rc.top;
    const double max_scroll = std::max(0.0, layout_.total_height - view_h);
    scroll_y_ = std::clamp(scroll_y_, 0.0, max_scroll);

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = static_cast<int>(layout_.total_height);
    si.nPage = static_cast<UINT>(view_h);
    si.nPos = static_cast<int>(scroll_y_);
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void GridView::scroll_by(double delta) { scroll_to(scroll_y_ + delta); }

void GridView::scroll_to(double y) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const double max_scroll = std::max(0.0, layout_.total_height - (rc.bottom - rc.top));
    scroll_y_ = std::clamp(y, 0.0, max_scroll);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    si.nPos = static_cast<int>(scroll_y_);
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

int GridView::hit_test(POINT client) const {
    const double x = client.x;
    const double y = client.y + scroll_y_;
    for (const auto& tile : layout_.tiles) {
        if (x >= tile.x && x < tile.x + tile.width && y >= tile.y &&
            y < tile.y + tile.height) {
            return tile.item_index;
        }
    }
    return -1;
}

void GridView::handle_click(POINT client, bool ctrl, bool shift) {
    const int index = hit_test(client);
    selection_.click(index, ctrl, shift);
    if (on_selection_changed) on_selection_changed();
}

void GridView::handle_key(WPARAM key) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const double page = rc.bottom - rc.top;

    if (zoomed()) {
        switch (key) {
            case VK_LEFT: zoom_step(-1); break;
            case VK_RIGHT: zoom_step(1); break;
            case VK_SPACE: toggle_zoom_pause(); break;
            case VK_ESCAPE: exit_zoom(); break;
            case VK_DELETE:
                // ズーム中の Del は表示中の 1 件を削除し、次のアイテムが表示される
                if (on_delete_requested) on_delete_requested();
                break;
            case 'Z':
                if (ctrl && on_undo_requested) on_undo_requested();
                break;
        }
        return;
    }

    switch (key) {
        case VK_DELETE:
            if (on_delete_requested) on_delete_requested();
            break;
        case 'Z':
            if (ctrl && on_undo_requested) on_undo_requested();
            break;
        case 'A':
            if (ctrl) {
                selection_.select_all(static_cast<int>(display_order_.size()));
                if (on_selection_changed) on_selection_changed();
            }
            break;
        case VK_ESCAPE:
            clear_selection();
            break;
        case VK_RETURN:
            // 単独選択中に Enter でもズームへ入れる
            if (selection_.size() == 1) enter_zoom(selection_.items().front());
            break;
        case VK_PRIOR: scroll_by(-page); break;
        case VK_NEXT: scroll_by(page); break;
        case VK_HOME: scroll_to(0); break;
        case VK_END: scroll_to(layout_.total_height); break;
        case VK_UP: scroll_by(-60); break;
        case VK_DOWN: scroll_by(60); break;
    }
}

float GridView::ui_scale() const {
    const UINT dpi = GetDpiForWindow(hwnd_);
    return dpi > 0 ? dpi / 96.0f : 1.0f;
}

void GridView::toggle_zoom_pause() {
    if (!zoomed() || zoom_display_index_ >= static_cast<int>(display_order_.size())) return;
    zoom_paused_ = !zoom_paused_;
    engine_->set_tile_paused(display_order_[zoom_display_index_], zoom_paused_);
    if (audio_.is_open()) {
        if (zoom_paused_) {
            audio_.pause();
        } else {
            // 再開時は映像位置へ合わせてから鳴らす
            auto* t = engine_->tile(display_order_[zoom_display_index_]);
            if (t) {
                std::lock_guard<std::mutex> lock(t->frame_mutex);
                audio_.seek_ms(t->latest.pts_ms);
            }
            audio_.play();
        }
    }
}

void GridView::reset_zoom_pause() {
    if (zoom_paused_ && zoom_fullres_engine_ >= 0) {
        engine_->set_tile_paused(zoom_fullres_engine_, false);
    }
    zoom_paused_ = false;
}

void GridView::set_audio_state(int volume, bool muted) {
    audio_volume_ = std::clamp(volume, 0, 100);
    audio_muted_ = muted;
    apply_audio_state();
}

void GridView::apply_audio_state() {
    if (audio_.is_open()) {
        audio_.set_volume(audio_volume_ / 100.0f);
        audio_.set_mute(audio_muted_);
    }
    for (auto& slot : grid_slots_) {
        if (slot.engine >= 0 && slot.player.is_open()) {
            slot.player.set_volume(audio_volume_ / 100.0f);
            slot.player.set_mute(audio_muted_);
        }
    }
}

void GridView::start_zoom_audio() {
    stop_zoom_audio();
    if (zoom_display_index_ < 0 ||
        zoom_display_index_ >= static_cast<int>(display_order_.size())) {
        return;
    }
    auto* t = engine_->tile(display_order_[zoom_display_index_]);
    if (!t || !core::is_media_foundation_video(t->item.type)) return;
    if (!audio_.open(t->item.path)) return;                  // 音声ストリーム無し等
    apply_audio_state();
    int64_t pts = 0;
    {
        std::lock_guard<std::mutex> lock(t->frame_mutex);
        pts = t->latest.pts_ms;
    }
    audio_.seek_ms(pts);
    audio_.play();
    audio_video_pts_ = pts;
    audio_drift_check_ms_ = 0;
}

void GridView::stop_zoom_audio() {
    audio_.close();
    audio_video_pts_ = -1;
}

// 映像クロック (フレーム pts) に音声を追従させる。
// ループ・シークで pts が跳んだら合わせ直し、通常時も定期的にドリフトを補正する
void GridView::sync_player_to_tile(io::AudioPlayer& player, int engine_index,
                                   int64_t& video_pts, int64_t& drift_check,
                                   int64_t now_ms) {
    if (!player.is_open()) return;
    auto* t = engine_->tile(engine_index);
    if (!t) return;
    int64_t pts = 0;
    {
        std::lock_guard<std::mutex> lock(t->frame_mutex);
        pts = t->latest.pts_ms;
    }
    const bool jumped = video_pts >= 0 &&
                        (pts < video_pts - 1000 || pts > video_pts + 2000);
    bool need_seek = jumped;
    if (!jumped && now_ms - drift_check >= 5000) {
        drift_check = now_ms;
        const int64_t audio_pos = player.position_ms();
        if (audio_pos >= 0 && (audio_pos < pts - 300 || audio_pos > pts + 300)) {
            need_seek = true;
        }
    }
    if (need_seek) {
        player.seek_ms(pts);
        player.play();  // 終端で停止していても再開する (ループ)
    }
    video_pts = pts;
}

void GridView::sync_zoom_audio(int64_t now_ms) {
    if (!zoomed() || zoom_paused_ ||
        zoom_display_index_ >= static_cast<int>(display_order_.size())) {
        return;
    }
    sync_player_to_tile(audio_, display_order_[zoom_display_index_], audio_video_pts_,
                        audio_drift_check_ms_, now_ms);
}

void GridView::set_grid_audio(bool enabled) {
    grid_audio_ = enabled;
    if (!enabled) stop_grid_audio();
}

void GridView::stop_grid_audio() {
    for (auto& slot : grid_slots_) {
        if (slot.engine >= 0) {
            slot.player.close();
            slot.engine = -1;
            slot.video_pts = -1;
        }
    }
    grid_audio_skip_.clear();  // engine index の意味が変わる場合があるので覚え直す
}

// 実験的: 一覧ビューで表示中の動画の音を同時に再生する (画面順に最大
// kGridAudioMax 件、ホバー中のタイルは最優先で確保)。
// open は同期呼び出しで 1 件数十 ms かかり得るため 1 ティック 2 件までとし、
// 失敗した (音声なし等の) ファイルは grid_audio_skip_ に覚えて再試行しない
void GridView::update_grid_audio(const std::vector<int>& visible_list, int64_t now_ms) {
    if (!grid_audio_ || zoomed()) return;

    // 再生対象の engine index を決める
    std::vector<int> targets;
    targets.reserve(kGridAudioMax);
    auto add_target = [&](int display_index) {
        if (static_cast<int>(targets.size()) >= kGridAudioMax) return;
        if (display_index < 0 ||
            display_index >= static_cast<int>(display_order_.size())) {
            return;
        }
        const int engine_index = display_order_[display_index];
        if (grid_audio_skip_.count(engine_index)) return;
        if (std::find(targets.begin(), targets.end(), engine_index) != targets.end()) return;
        auto* t = engine_->tile(engine_index);
        if (!t || !core::is_media_foundation_video(t->item.type)) return;
        targets.push_back(engine_index);
    };
    add_target(hover_index_);
    for (int display_index : visible_list) add_target(display_index);

    // 対象から外れたスロットを閉じる
    for (auto& slot : grid_slots_) {
        if (slot.engine >= 0 &&
            std::find(targets.begin(), targets.end(), slot.engine) == targets.end()) {
            slot.player.close();
            slot.engine = -1;
            slot.video_pts = -1;
        }
    }

    // 新しい対象を空きスロットで開く
    int opened = 0;
    for (int engine_index : targets) {
        const bool playing = std::any_of(
            grid_slots_.begin(), grid_slots_.end(),
            [engine_index](const GridAudioSlot& s) { return s.engine == engine_index; });
        if (playing) continue;
        if (opened >= 2) break;  // 残りは次のティックで開く
        auto free_it = std::find_if(grid_slots_.begin(), grid_slots_.end(),
                                    [](const GridAudioSlot& s) { return s.engine < 0; });
        if (free_it == grid_slots_.end()) break;
        ++opened;
        auto* t = engine_->tile(engine_index);
        if (!t || !free_it->player.open(t->item.path)) {
            grid_audio_skip_.insert(engine_index);
            continue;
        }
        free_it->player.set_volume(audio_volume_ / 100.0f);
        free_it->player.set_mute(audio_muted_);
        int64_t pts = 0;
        {
            std::lock_guard<std::mutex> lock(t->frame_mutex);
            pts = t->latest.pts_ms;
        }
        free_it->player.seek_ms(pts);
        free_it->player.play();
        free_it->engine = engine_index;
        free_it->video_pts = pts;
        free_it->drift_check = now_ms;
    }

    // 各スロットを映像クロックへ同期
    for (auto& slot : grid_slots_) {
        if (slot.engine >= 0) {
            sync_player_to_tile(slot.player, slot.engine, slot.video_pts,
                                slot.drift_check, now_ms);
        }
    }
}

void GridView::set_zoom_fullres(int engine_index) {
    if (zoom_fullres_engine_ == engine_index) return;
    if (zoom_fullres_engine_ >= 0) engine_->request_full_resolution(zoom_fullres_engine_, false);
    if (engine_index >= 0) engine_->request_full_resolution(engine_index, true);
    zoom_fullres_engine_ = engine_index;
}

void GridView::enter_zoom(int display_index) {
    if (display_index < 0 || display_index >= static_cast<int>(display_order_.size())) return;
    zoom_display_index_ = display_index;
    selection_.click(display_index, false, false);  // Del が「表示中の 1 件」に効くように
    if (on_selection_changed) on_selection_changed();
    reset_zoom_pause();
    set_zoom_fullres(display_order_[display_index]);
    stop_grid_audio();  // ズーム中は対象 1 件の音声だけを鳴らす
    start_zoom_audio();
}

void GridView::exit_zoom() {
    if (!zoomed()) return;
    stop_zoom_audio();
    reset_zoom_pause();
    const int index = zoom_display_index_;
    zoom_display_index_ = -1;
    set_zoom_fullres(-1);
    // 戻ったときにそのタイルが見えるようにスクロールを合わせる
    if (index >= 0 && index < static_cast<int>(layout_.tiles.size())) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        const core::TileRect& tile = layout_.tiles[index];
        if (tile.y < scroll_y_ || tile.y + tile.height > scroll_y_ + (rc.bottom - rc.top)) {
            scroll_to(tile.y - (rc.bottom - rc.top - tile.height) / 2);
        }
    }
}

void GridView::zoom_step(int delta) {
    if (!zoomed() || display_order_.empty()) return;
    const int count = static_cast<int>(display_order_.size());
    const int next = std::clamp(zoom_display_index_ + delta, 0, count - 1);
    if (next == zoom_display_index_) return;
    zoom_display_index_ = next;
    selection_.click(next, false, false);
    if (on_selection_changed) on_selection_changed();
    reset_zoom_pause();
    set_zoom_fullres(display_order_[next]);
    start_zoom_audio();
}

void GridView::build_zoom_schedule(std::vector<int>* visible, std::vector<int>* active) const {
    const int count = static_cast<int>(display_order_.size());
    if (zoom_display_index_ < 0 || zoom_display_index_ >= count) return;
    visible->push_back(zoom_display_index_);
    active->push_back(zoom_display_index_);
    // 前後 2 件を先読みしておく (キー送りで待たされないように)
    for (int d = 1; d <= 2; ++d) {
        if (zoom_display_index_ + d < count) active->push_back(zoom_display_index_ + d);
        if (zoom_display_index_ - d >= 0) active->push_back(zoom_display_index_ - d);
    }
}

void GridView::sweep_bitmaps() {
    // アクティブ集合に無いタイルのビットマップを解放する
    for (auto it = bitmaps_.begin(); it != bitmaps_.end();) {
        const bool active =
            std::find(last_active_.begin(), last_active_.end(), it->first) != last_active_.end();
        if (!active) {
            it = bitmaps_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = gpu_bitmaps_.begin(); it != gpu_bitmaps_.end();) {
        const bool active =
            std::find(last_active_.begin(), last_active_.end(), it->first) != last_active_.end();
        if (!active) {
            it = gpu_bitmaps_.erase(it);
        } else {
            ++it;
        }
    }
}

void GridView::render() {
    if (!engine_) return;
    ensure_render_target();
    if (!rt()) return;

    const int64_t now = static_cast<int64_t>(GetTickCount64()) - start_tick_;

    // プローブ完了でアスペクト比が変わったらレイアウトし直す
    const uint64_t pv = engine_->probe_version();
    if (pv != seen_probe_version_) {
        seen_probe_version_ = pv;
        layout_dirty_ = true;
    }
    if (layout_dirty_) rebuild_layout();

    RECT rc;
    GetClientRect(hwnd_, &rc);
    const double view_h = rc.bottom - rc.top;

    // スケジュール決定 → エンジンへ反映 (ズーム中は対象 + 前後の先読みのみ)
    std::vector<int> visible_list;
    std::vector<int> active_list;
    if (zoomed()) {
        build_zoom_schedule(&visible_list, &active_list);
    } else {
        core::ScheduleParams params = core::params_for_mode(mode_);
        params.scroll_y = scroll_y_;
        params.viewport_height = view_h;
        core::ScheduleResult schedule = core::compute_schedule(layout_, params);
        visible_list = std::move(schedule.visible);
        active_list = std::move(schedule.active);
    }

    std::vector<int> active_engine;
    active_engine.reserve(active_list.size());
    for (int display_index : active_list) {
        active_engine.push_back(display_order_[display_index]);
    }
    if (active_engine != last_active_) {
        engine_->set_active(active_engine);
        last_active_ = std::move(active_engine);
        sweep_bitmaps();
    }

    // 非可視のアクティブタイル (先読み分) もフレームを「消費」して次の期限を設定する。
    // これが無いと期限 0 のまま毎ティック全速でデコードし続けてしまう。
    // 可視タイルはビットマップ転送を伴うため描画ループ側で消費する (ここでは除外)
    const std::unordered_set<int> visible_set(visible_list.begin(), visible_list.end());
    for (int display_index : active_list) {
        if (visible_set.count(display_index)) continue;
        const int engine_index = display_order_[display_index];
        auto* t = engine_->tile(engine_index);
        if (!t) continue;
        std::lock_guard<std::mutex> lock(t->frame_mutex);
        if (t->latest_version > t->shown_version) {
            t->shown_version = t->latest_version;
            engine_->frame_consumed(engine_index, now, t->latest.duration_ms);
        }
    }

    engine_->tick(now);

    // ---- 描画 ----
    rt()->BeginDraw();
    rt()->Clear(bg_color(dark_));

    const double view_w = rc.right - rc.left;
    for (int display_index : visible_list) {
        const core::TileRect& tile = layout_.tiles[display_index];
        const int engine_index = display_order_[display_index];
        auto* t = engine_->tile(engine_index);
        if (!t) continue;

        // ズーム中はクライアント全面へ (アスペクト維持は後段のフィット処理が行う)
        const D2D1_RECT_F dest =
            zoomed() ? D2D1::RectF(0.0f, 0.0f, static_cast<float>(view_w),
                                   static_cast<float>(view_h))
                     : D2D1::RectF(static_cast<float>(tile.x),
                                   static_cast<float>(tile.y - scroll_y_),
                                   static_cast<float>(tile.x + tile.width),
                                   static_cast<float>(tile.y + tile.height - scroll_y_));

        // 新フレームの取り込みと描画元の決定。
        // GPU フレーム (ゼロコピー) はテクスチャに書き込み済みなのでラッパー取得のみ、
        // CPU フレームはビットマップへ転送する
        ID2D1Bitmap* draw_bitmap = nullptr;
        {
            std::lock_guard<std::mutex> lock(t->frame_mutex);
            const bool is_gpu = zero_copy_ && t->gpu_front >= 0;
            if (!is_gpu && !t->latest.bgra.empty()) {
                // 先読み中に消費済みのフレームでも、ビットマップが無ければ
                // 手元の最新フレームから作る (これが無いと画面に入った瞬間に
                // 次のデコードまでプレースホルダが見えてちらつく)
                auto& bitmap = bitmaps_[engine_index];
                const D2D1_SIZE_U frame_size =
                    D2D1::SizeU(t->latest.width, t->latest.height);
                const bool bitmap_stale = !bitmap ||
                                          bitmap->GetPixelSize().width != frame_size.width ||
                                          bitmap->GetPixelSize().height != frame_size.height;
                if (t->latest_version > t->shown_version || bitmap_stale) {
                    if (bitmap_stale) {
                        bitmap.Reset();
                        rt()->CreateBitmap(
                            frame_size,
                            D2D1::BitmapProperties(D2D1::PixelFormat(
                                DXGI_FORMAT_B8G8R8A8_UNORM,
                                t->has_alpha ? D2D1_ALPHA_MODE_PREMULTIPLIED
                                             : D2D1_ALPHA_MODE_IGNORE)),
                            bitmap.GetAddressOf());
                    }
                    if (!bitmap) continue;
                    bitmap->CopyFromMemory(nullptr, t->latest.bgra.data(),
                                           static_cast<UINT32>(t->latest.width) * 4);
                }
            }
            if (t->latest_version > t->shown_version &&
                (is_gpu || !t->latest.bgra.empty())) {
                t->shown_version = t->latest_version;
                engine_->frame_consumed(engine_index, now, t->latest.duration_ms);
            }
            if (is_gpu) {
                draw_bitmap = gpu_bitmap_for(engine_index, t->gpu_front,
                                             t->gpu_tex[t->gpu_front].Get());
            }
        }
        if (!draw_bitmap) {
            auto bitmap_it = bitmaps_.find(engine_index);
            if (bitmap_it != bitmaps_.end()) draw_bitmap = bitmap_it->second.Get();
        }

        if (draw_bitmap) {
            // アスペクト比を保ってタイル内にフィット (プローブ前の仮レイアウト対策)
            const D2D1_SIZE_U bs = draw_bitmap->GetPixelSize();
            D2D1_RECT_F fitted = dest;
            const double dw = dest.right - dest.left, dh = dest.bottom - dest.top;
            const double baspect = bs.height > 0 ? static_cast<double>(bs.width) / bs.height : 1.0;
            const double taspect = dh > 0 ? dw / dh : 1.0;
            if (std::abs(baspect - taspect) > 0.01) {
                if (baspect > taspect) {
                    const double fit_h = dw / baspect;
                    const double pad = (dh - fit_h) / 2;
                    fitted.top += static_cast<float>(pad);
                    fitted.bottom -= static_cast<float>(pad);
                } else {
                    const double fit_w = dh * baspect;
                    const double pad = (dw - fit_w) / 2;
                    fitted.left += static_cast<float>(pad);
                    fitted.right -= static_cast<float>(pad);
                }
            }
            rt()->DrawBitmap(draw_bitmap, fitted, 1.0f,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            // プレースホルダ
            brush_->SetColor(placeholder_color(dark_));
            rt()->FillRectangle(dest, brush_.Get());
            if (text_format_) {
                const wchar_t* label;
                std::wstring name;
                if (t->failed) {
                    label = tr(Str::LoadFailedTile);
                } else {
                    const size_t pos = t->item.path.find_last_of(L'\\');
                    name = pos == std::wstring::npos ? t->item.path
                                                     : t->item.path.substr(pos + 1);
                    label = name.c_str();
                }
                brush_->SetColor(dark_ ? D2D1::ColorF(0.6f, 0.6f, 0.65f)
                                       : D2D1::ColorF(0.35f, 0.35f, 0.4f));
                rt()->DrawTextW(label, static_cast<UINT32>(wcslen(label)),
                                          text_format_.Get(), dest, brush_.Get());
            }
        }

        // 選択・ホバーの枠 (ズーム中は不要)
        if (!zoomed()) {
            if (selection_.contains(display_index)) {
                brush_->SetColor(accent_color());
                rt()->DrawRectangle(dest, brush_.Get(), 3.0f);
                brush_->SetColor(D2D1::ColorF(0.30f, 0.62f, 1.0f, 0.18f));
                rt()->FillRectangle(dest, brush_.Get());
            } else if (display_index == hover_index_) {
                brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, dark_ ? 0.35f : 0.6f));
                rt()->DrawRectangle(dest, brush_.Get(), 1.5f);
            }
        }
    }

    // デバッグオーバーレイ (タイルごとの計測値 + 全体統計)
    if (debug_overlay_) {
        ++fps_counter_;
        if (now - fps_window_start_ >= 1000) {
            render_fps_ = fps_counter_;
            fps_counter_ = 0;
            fps_window_start_ = now;
            io::gpu_memory_stats(&vram_usage_mb_, &vram_budget_mb_);  // 1 秒ごとに更新
        }
        draw_debug_overlay(visible_list, active_list, view_w);
    }

    // 音声: ズーム中は対象へ同期、一覧では (実験的機能が有効なら) 表示中の動画
    if (zoomed()) {
        sync_zoom_audio(now);
    } else {
        update_grid_audio(visible_list, now);
    }

    // ズーム中のシークバー (薄いトラック + 進捗 + ノブ。メニューで非表示可)
    if (zoomed() && show_seekbar_) {
        draw_seekbar(view_w, view_h);
    } else {
        seek_hit_rect_ = D2D1_RECT_F{};
        mute_hit_rect_ = D2D1_RECT_F{};
        volume_hit_rect_ = D2D1_RECT_F{};
        play_hit_rect_ = D2D1_RECT_F{};
    }

    // ズーム中はファイル名と位置 (n / 総数) を左下にオーバーレイ表示
    if (zoomed() && zoom_label_format_ &&
        zoom_display_index_ < static_cast<int>(display_order_.size())) {
        auto* t = engine_->tile(display_order_[zoom_display_index_]);
        if (t) {
            const size_t pos = t->item.path.find_last_of(L'\\');
            const std::wstring name =
                pos == std::wstring::npos ? t->item.path : t->item.path.substr(pos + 1);
            wchar_t label[512];
            swprintf(label, 512, L"%s    %d / %d", name.c_str(), zoom_display_index_ + 1,
                     static_cast<int>(display_order_.size()));
            const float ui = ui_scale();
            const float bar_h = 40.0f * ui;
            const D2D1_RECT_F bar = D2D1::RectF(0.0f, static_cast<float>(view_h) - bar_h,
                                                static_cast<float>(view_w),
                                                static_cast<float>(view_h));
            brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f));
            rt()->FillRectangle(bar, brush_.Get());
            brush_->SetColor(D2D1::ColorF(0.92f, 0.92f, 0.95f));
            D2D1_RECT_F text_rc = bar;
            text_rc.left += 16.0f * ui;
            rt()->DrawTextW(label, static_cast<UINT32>(wcslen(label)),
                                      zoom_label_format_.Get(), text_rc, brush_.Get());
        }
    }

    const HRESULT hr = rt()->EndDraw();
    if (zero_copy_) {
        HRESULT present_hr = S_OK;
        // vsync 待ち (Present(1)) は UI スレッドが共有デバイスを長時間拘束し、
        // ワーカーのデコード/変換を巻き添えにするため待たない (描画は 15ms タイマー駆動)
        if (SUCCEEDED(hr)) present_hr = swapchain_->Present(0, 0);
        if (FAILED(hr) || FAILED(present_hr)) {
            // デバイスロスト等。作り直して再試行 (ダメなら従来経路へ落ちる)
            release_zero_copy();
            zero_copy_tried_ = false;
        }
    } else if (FAILED(hr)) {
        // RECREATE_TARGET に限らず、失敗したら丸ごと作り直して自己回復する
        render_target_.Reset();
        brush_.Reset();
        bitmaps_.clear();
    }
}

void GridView::draw_seekbar(double view_w, double view_h) {
    if (zoom_display_index_ < 0 ||
        zoom_display_index_ >= static_cast<int>(display_order_.size())) {
        return;
    }
    const int engine_index = display_order_[zoom_display_index_];
    auto* t = engine_->tile(engine_index);
    if (!t) return;
    const double duration_ms = t->item.duration_sec * 1000.0;
    if (duration_ms < 100.0) {  // 静止画・極短は出さない
        seek_hit_rect_ = D2D1_RECT_F{};
        return;
    }
    int64_t pts_ms = 0;
    {
        std::lock_guard<std::mutex> lock(t->frame_mutex);
        pts_ms = t->latest.pts_ms;
    }
    double ratio = pts_ms / duration_ms;
    ratio = std::clamp(ratio, 0.0, 1.0);

    // DPI に応じて全体を拡大。下部ラベルバーの少し上に
    // [▶/⏸] [シークバー .....] [🔊] [音量 ---] を並べる
    const float ui = ui_scale();
    const float margin = 28.0f * ui;
    const float label_h = 40.0f * ui;
    const float track_h = 6.0f * ui;
    const float y = static_cast<float>(view_h) - label_h - 26.0f * ui;
    const float center_y = y + track_h / 2;
    const float hit_pad = 12.0f * ui;
    const bool has_audio = audio_.is_open();

    // 再生/一時停止ボタン (左端)
    const float play_size = 22.0f * ui;
    const float play_x = margin;
    play_hit_rect_ = D2D1::RectF(play_x - 8.0f * ui, center_y - play_size,
                                 play_x + play_size + 8.0f * ui, center_y + play_size);
    if (zoom_paused_) {
        // ▶ (三角形)
        brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
        ComPtr<ID2D1PathGeometry> tri;
        if (SUCCEEDED(d2d_factory_->CreatePathGeometry(&tri))) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(tri->Open(&sink))) {
                sink->BeginFigure(D2D1::Point2F(play_x, center_y - play_size * 0.62f),
                                  D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(play_x + play_size, center_y));
                sink->AddLine(D2D1::Point2F(play_x, center_y + play_size * 0.62f));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                rt()->FillGeometry(tri.Get(), brush_.Get());
            }
        }
    } else {
        // ⏸ (2 本バー)
        brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.9f));
        const float bar_w = play_size * 0.32f;
        const float bar_h2 = play_size * 0.62f;
        rt()->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(play_x, center_y - bar_h2, play_x + bar_w,
                                          center_y + bar_h2),
                              2.0f * ui, 2.0f * ui),
            brush_.Get());
        rt()->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(play_x + play_size - bar_w, center_y - bar_h2,
                                          play_x + play_size, center_y + bar_h2),
                              2.0f * ui, 2.0f * ui),
            brush_.Get());
    }

    // 右側コントロール幅 (音声があるときのみ)
    const float icon_w = 34.0f * ui;
    const float vol_w = 130.0f * ui;
    const float controls_w = has_audio ? icon_w + vol_w + 28.0f * ui : 0.0f;

    const float left = play_x + play_size + 20.0f * ui;
    const float right = static_cast<float>(view_w) - margin - controls_w;

    // クリック判定は太めに取る
    seek_hit_rect_ =
        D2D1::RectF(left - hit_pad, y - hit_pad, right + hit_pad, y + track_h + hit_pad);

    // トラック (薄い白) / 進捗 (アクセント) / ノブ (白丸)
    auto rounded = [&](float x0, float x1, float yy, const D2D1_COLOR_F& color) {
        if (x1 <= x0) return;
        brush_->SetColor(color);
        D2D1_ROUNDED_RECT rr{D2D1::RectF(x0, yy, x1, yy + track_h), track_h / 2, track_h / 2};
        rt()->FillRoundedRectangle(rr, brush_.Get());
    };
    rounded(left, right, y, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f));
    const float knob_x = left + static_cast<float>((right - left) * ratio);
    rounded(left, knob_x, y, D2D1::ColorF(0.30f, 0.62f, 1.0f, 0.9f));
    brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, seek_dragging_ ? 1.0f : 0.9f));
    const float knob_r = (seek_dragging_ ? 12.0f : 9.0f) * ui;
    rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob_x, center_y), knob_r, knob_r),
                      brush_.Get());

    if (!has_audio) {
        mute_hit_rect_ = D2D1_RECT_F{};
        volume_hit_rect_ = D2D1_RECT_F{};
        return;
    }

    // ミュートボタン (スピーカー絵文字) + 音量ミニスライダー
    const float icon_x = right + 22.0f * ui;
    mute_hit_rect_ = D2D1::RectF(icon_x - 8.0f * ui, center_y - 20.0f * ui,
                                 icon_x + icon_w, center_y + 20.0f * ui);
    if (zoom_label_format_) {
        brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, audio_muted_ ? 0.55f : 0.95f));
        const wchar_t* glyph = audio_muted_ ? L"\U0001F507" : L"\U0001F50A";  // 🔇 / 🔊
        rt()->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), zoom_label_format_.Get(),
                        mute_hit_rect_, brush_.Get());
    }

    const float vol_left = icon_x + icon_w + 6.0f * ui;
    const float vol_right = static_cast<float>(view_w) - margin;
    volume_hit_rect_ =
        D2D1::RectF(vol_left - hit_pad, y - hit_pad, vol_right + hit_pad, y + track_h + hit_pad);
    rounded(vol_left, vol_right, y, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f));
    const float effective = audio_muted_ ? 0.0f : audio_volume_ / 100.0f;
    const float vol_x = vol_left + (vol_right - vol_left) * effective;
    rounded(vol_left, vol_x, y, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.75f));
    brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
    rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(vol_x, center_y), 7.0f * ui, 7.0f * ui),
                      brush_.Get());
}

bool GridView::handle_seek_input(POINT pt, bool is_down) {
    if (!zoomed() || !show_seekbar_) return false;
    const float x = static_cast<float>(pt.x);
    const float y = static_cast<float>(pt.y);
    auto inside = [&](const D2D1_RECT_F& r) {
        return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    };

    // 音量スライダーのドラッグ
    auto update_volume = [&] {
        const float left = volume_hit_rect_.left + 6.0f;
        const float right = volume_hit_rect_.right - 6.0f;
        float ratio = (x - left) / std::max(right - left, 1.0f);
        ratio = std::clamp(ratio, 0.0f, 1.0f);
        audio_volume_ = static_cast<int>(ratio * 100.0f + 0.5f);
        if (audio_volume_ > 0) audio_muted_ = false;  // 音量を触ったらミュート解除
        apply_audio_state();
        if (on_audio_changed) on_audio_changed(audio_volume_, audio_muted_);
    };

    if (is_down) {
        if (inside(play_hit_rect_)) {
            toggle_zoom_pause();
            return true;
        }
        if (audio_.is_open() && inside(mute_hit_rect_)) {
            audio_muted_ = !audio_muted_;
            apply_audio_state();
            if (on_audio_changed) on_audio_changed(audio_volume_, audio_muted_);
            return true;
        }
        if (audio_.is_open() && inside(volume_hit_rect_)) {
            volume_dragging_ = true;
            SetCapture(hwnd_);
            update_volume();
            return true;
        }
        if (!inside(seek_hit_rect_)) return false;
        seek_dragging_ = true;
        SetCapture(hwnd_);
    } else if (volume_dragging_) {
        update_volume();
        return true;
    } else if (!seek_dragging_) {
        return false;
    }

    if (zoom_display_index_ < 0 ||
        zoom_display_index_ >= static_cast<int>(display_order_.size())) {
        return true;
    }
    const int engine_index = display_order_[zoom_display_index_];
    auto* t = engine_->tile(engine_index);
    if (!t) return true;
    const double duration_ms = t->item.duration_sec * 1000.0;
    if (duration_ms < 100.0) return true;

    // ドラッグ中は 80ms ごとに発行 (デコーダへの過剰なシークを避ける)
    const int64_t now = static_cast<int64_t>(GetTickCount64());
    if (is_down || now - last_seek_issue_ms_ >= 80) {
        last_seek_issue_ms_ = now;
        const float left = seek_hit_rect_.left + 6.0f;
        const float right = seek_hit_rect_.right - 6.0f;
        double ratio = (x - left) / std::max(right - left, 1.0f);
        ratio = std::clamp(ratio, 0.0, 1.0);
        const int64_t target = static_cast<int64_t>(duration_ms * ratio);
        engine_->request_seek(engine_index, target);
        if (audio_.is_open()) {
            audio_.seek_ms(target);
            audio_.play();
            audio_video_pts_ = target;  // 直後の映像 pts ジャンプを再シーク扱いにしない
        }
    }
    return true;
}

void GridView::draw_debug_overlay(const std::vector<int>& visible_list,
                                  const std::vector<int>& active_list, double view_w) {
    if (!debug_format_ || !brush_) return;

    // 可視タイルの平均デコード時間 (重いファイルを相対判定するため)
    double avg_decode_us = 0.0;
    {
        int64_t sum = 0;
        int count = 0;
        for (int display_index : visible_list) {
            auto* t = engine_->tile(display_order_[display_index]);
            if (!t) continue;
            const int ema = t->decode_us_ema.load();
            if (ema > 0) {
                sum += ema;
                ++count;
            }
        }
        if (count > 0) avg_decode_us = static_cast<double>(sum) / count;
    }

    // ---- タイルごとの計測値 ----
    for (int display_index : visible_list) {
        const core::TileRect& tile = layout_.tiles[display_index];
        const int engine_index = display_order_[display_index];
        auto* t = engine_->tile(engine_index);
        if (!t) continue;

        int out_w = 0, out_h = 0;
        {
            std::lock_guard<std::mutex> lock(t->frame_mutex);
            out_w = t->latest.width;
            out_h = t->latest.height;
        }
        const double size_mb = static_cast<double>(t->item.file_size) / (1024.0 * 1024.0);
        const double dec_ms = t->decode_us_ema.load() / 1000.0;
        const int open_ms = t->open_ms.load();
        // Media Foundation video 以外は CPU デコードが正規の経路 (橙判定の対象外)
        const bool cpu_native = !core::is_media_foundation_video(t->item.type);
        const wchar_t* codec_label =
            t->item.type == core::MediaType::Webp   ? L"WEBP(CPU)"
            : t->item.type == core::MediaType::Png  ? L"PNG(CPU)"
            : t->item.type == core::MediaType::Jpeg ? L"JPG(CPU)"
            : t->item.type == core::MediaType::Wmv  ? (t->hardware ? L"WMV(GPU)" : L"WMV(CPU)")
            : t->item.type == core::MediaType::Avi  ? (t->hardware ? L"AVI(GPU)" : L"AVI(CPU)")
                                                    : (t->hardware ? L"GPU" : L"CPU");

        wchar_t text[256];
        swprintf(text, 256, L"%dx%d→%dx%d %s\nopen %dms dec %.1fms %.1fMB%s", t->item.width,
                 t->item.height, out_w, out_h, codec_label, open_ms < 0 ? 0 : open_ms, dec_ms,
                 size_mb, t->failed ? L" FAILED" : (t->fail_count > 0 ? L" retry" : L""));

        const float x = static_cast<float>(tile.x);
        const float y = static_cast<float>(tile.y - scroll_y_);
        const float w = static_cast<float>(std::min(tile.width, 280.0));
        const D2D1_RECT_F bg = D2D1::RectF(x, y, x + w, y + 34.0f);
        brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f));
        rt()->FillRectangle(bg, brush_.Get());

        // 文字色で重さを可視化:
        //   赤 = 失敗 or 平均の 3 倍超 (かつ 3ms 超)
        //   黄 = 平均の 1.5 倍超 (かつ 1.5ms 超)
        //   橙 = ソフトウェアデコード (MP4 のみ)
        //   緑 = 正常 (GPU / WEBP)
        const int ema = t->decode_us_ema.load();
        D2D1_COLOR_F text_color = D2D1::ColorF(0.65f, 1.0f, 0.65f);
        if (!t->hardware && !cpu_native) text_color = D2D1::ColorF(1.0f, 0.75f, 0.4f);
        if (avg_decode_us > 0 && ema > avg_decode_us * 1.5 && ema > 1500) {
            text_color = D2D1::ColorF(1.0f, 0.95f, 0.3f);
        }
        if ((avg_decode_us > 0 && ema > avg_decode_us * 3.0 && ema > 3000) || t->failed) {
            text_color = D2D1::ColorF(1.0f, 0.35f, 0.3f);
        }
        brush_->SetColor(text_color);
        D2D1_RECT_F text_rc = bg;
        text_rc.left += 4.0f;
        rt()->DrawTextW(text, static_cast<UINT32>(wcslen(text)), debug_format_.Get(),
                                  text_rc, brush_.Get());
    }

    // ---- 全体統計 (右上) ----
    // アクティブな MP4 デコーダの GPU / CPU 内訳
    int dec_gpu = 0, dec_cpu = 0;
    for (int display_index : active_list) {
        auto* t = engine_->tile(display_order_[display_index]);
        if (!t || !core::is_media_foundation_video(t->item.type)) continue;
        if (t->decode_us_ema.load() == 0) continue;  // まだ動いていないものは除外
        if (t->hardware) {
            ++dec_gpu;
        } else {
            ++dec_cpu;
        }
    }
    int gpu_used = 0, gpu_cap = 0;
    io::gpu_decoder_stats(&gpu_used, &gpu_cap);
    wchar_t global[256];
    swprintf(global, 256,
             L"render %d fps   visible %zu   active %zu   dec GPU:%d CPU:%d   "
             L"枠 %d/%d   VRAM %.1f/%.1fGB",
             render_fps_, visible_list.size(), active_list.size(), dec_gpu, dec_cpu,
             gpu_used, gpu_cap, vram_usage_mb_ / 1024.0, vram_budget_mb_ / 1024.0);
    const float gw = 720.0f;
    const D2D1_RECT_F gbg =
        D2D1::RectF(static_cast<float>(view_w) - gw, 0.0f, static_cast<float>(view_w), 22.0f);
    brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.7f));
    rt()->FillRectangle(gbg, brush_.Get());
    brush_->SetColor(D2D1::ColorF(0.75f, 0.9f, 1.0f));
    D2D1_RECT_F gtext = gbg;
    gtext.left += 6.0f;
    gtext.top += 3.0f;
    rt()->DrawTextW(global, static_cast<UINT32>(wcslen(global)), debug_format_.Get(),
                              gtext, brush_.Get());
}

bool GridView::capture_to_png(const std::wstring& path) {
    if (!engine_ || !d2d_factory_) return false;
    if (layout_dirty_) rebuild_layout();

    RECT rc;
    GetClientRect(hwnd_, &rc);
    const UINT width = rc.right - rc.left;
    const UINT height = rc.bottom - rc.top;
    if (width == 0 || height == 0) return false;

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        return false;
    }
    ComPtr<IWICBitmap> wic_bitmap;
    if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapCacheOnDemand, &wic_bitmap))) {
        return false;
    }
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(d2d_factory_->CreateWicBitmapRenderTarget(
            wic_bitmap.Get(),
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f),
            &rt))) {
        return false;
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);

    std::vector<int> visible_list;
    if (zoomed()) {
        std::vector<int> ignored;
        build_zoom_schedule(&visible_list, &ignored);
    } else {
        core::ScheduleParams params = core::params_for_mode(mode_);
        params.scroll_y = scroll_y_;
        params.viewport_height = height;
        visible_list = compute_schedule(layout_, params).visible;
    }

    rt->BeginDraw();
    rt->Clear(bg_color(dark_));
    for (int display_index : visible_list) {
        const core::TileRect& tile = layout_.tiles[display_index];
        const int engine_index = display_order_[display_index];
        auto* t = engine_->tile(engine_index);
        if (!t) continue;
        const D2D1_RECT_F dest =
            zoomed() ? D2D1::RectF(0.0f, 0.0f, static_cast<float>(width),
                                   static_cast<float>(height))
                     : D2D1::RectF(static_cast<float>(tile.x),
                                   static_cast<float>(tile.y - scroll_y_),
                                   static_cast<float>(tile.x + tile.width),
                                   static_cast<float>(tile.y + tile.height - scroll_y_));

        ComPtr<ID2D1Bitmap> frame_bitmap;
        {
            std::lock_guard<std::mutex> lock(t->frame_mutex);
            if (!t->latest.bgra.empty()) {
                rt->CreateBitmap(
                    D2D1::SizeU(t->latest.width, t->latest.height), t->latest.bgra.data(),
                    static_cast<UINT32>(t->latest.width) * 4,
                    D2D1::BitmapProperties(D2D1::PixelFormat(
                        DXGI_FORMAT_B8G8R8A8_UNORM,
                        t->has_alpha ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE)),
                    &frame_bitmap);
            } else if (t->gpu_front >= 0 && t->gpu_tex[t->gpu_front]) {
                // GPU フレーム (ゼロコピー) はテクスチャを読み戻して描く (キャプチャ専用)
                int gw = 0, gh = 0;
                io::read_texture_bgra(t->gpu_tex[t->gpu_front].Get(), nullptr, 0, &gw, &gh);
                if (gw > 0 && gh > 0) {
                    std::vector<uint8_t> pixels(static_cast<size_t>(gw) * gh * 4);
                    if (io::read_texture_bgra(t->gpu_tex[t->gpu_front].Get(), pixels.data(),
                                              pixels.size(), nullptr, nullptr)) {
                        rt->CreateBitmap(
                            D2D1::SizeU(gw, gh), pixels.data(), static_cast<UINT32>(gw) * 4,
                            D2D1::BitmapProperties(D2D1::PixelFormat(
                                DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
                            &frame_bitmap);
                    }
                }
            }
        }
        if (frame_bitmap) {
            // アスペクト維持でフィット (render と同じ扱い)
            const D2D1_SIZE_U bs = frame_bitmap->GetPixelSize();
            D2D1_RECT_F fitted = dest;
            const double dw = dest.right - dest.left, dh = dest.bottom - dest.top;
            const double baspect =
                bs.height > 0 ? static_cast<double>(bs.width) / bs.height : 1.0;
            const double taspect = dh > 0 ? dw / dh : 1.0;
            if (std::abs(baspect - taspect) > 0.01) {
                if (baspect > taspect) {
                    const double pad = (dh - dw / baspect) / 2;
                    fitted.top += static_cast<float>(pad);
                    fitted.bottom -= static_cast<float>(pad);
                } else {
                    const double pad = (dw - dh * baspect) / 2;
                    fitted.left += static_cast<float>(pad);
                    fitted.right -= static_cast<float>(pad);
                }
            }
            rt->DrawBitmap(frame_bitmap.Get(), fitted, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            brush->SetColor(placeholder_color(dark_));
            rt->FillRectangle(dest, brush.Get());
        }
        if (!zoomed() && selection_.contains(display_index)) {
            brush->SetColor(accent_color());
            rt->DrawRectangle(dest, brush.Get(), 3.0f);
        }
    }
    if (FAILED(rt->EndDraw())) return false;

    // PNG 保存 (WIC エンコーダ)
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        return false;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        return false;
    }
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) {
        return false;
    }
    if (FAILED(frame->WriteSource(wic_bitmap.Get(), nullptr))) return false;
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;
    return true;
}

}  // namespace meguri
