// Direct2D 描画のグリッドビュー (専用子ウィンドウ)。
// レイアウト・スクロール・選択・タイル描画と、再生エンジンの駆動を担当する。
// Direct2D の HwndRenderTarget はこの子ウィンドウに閉じ込める。
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/layout.h"
#include "core/scheduler.h"
#include "core/selection.h"
#include "io/audio_player.h"
#include "playback_engine.h"

struct ID3D11Texture2D;

namespace meguri {

class GridView {
public:
    GridView() = default;
    ~GridView();
    GridView(const GridView&) = delete;
    GridView& operator=(const GridView&) = delete;

    bool create(HWND parent, HINSTANCE instance, PlaybackEngine* engine);
    HWND hwnd() const { return hwnd_; }

    // 表示順 (engine の tile index 列) を差し替える。選択はクリアされる。
    void set_display_order(std::vector<int> order);
    const std::vector<int>& display_order() const { return display_order_; }

    void set_theme(bool dark);
    bool theme_dark() const { return dark_; }

    // デバッグオーバーレイ (各タイルの解像度 / DXVA・SW / open・decode 時間など)
    void set_debug_overlay(bool enabled) { debug_overlay_ = enabled; }

    // ズーム中のシークバー表示
    void set_show_seekbar(bool enabled) { show_seekbar_ = enabled; }

    // ズーム中の音声 (音量 0〜100、ミュート)。変更は on_audio_changed で通知
    void set_audio_state(int volume, bool muted);
    std::function<void(int volume, bool muted)> on_audio_changed;

    // 実験的: 一覧ビューで表示中の動画の音を同時再生 (最大 kGridAudioMax 件)
    void set_grid_audio(bool enabled);
    void set_row_height(double height);  // DPI スケール適用済みの値を渡す
    void set_schedule_params(core::PerformanceMode mode);
    void move_to(const RECT& rc);

    // 選択 (display index -> engine index に解決して返す)
    std::vector<int> selected_engine_indices() const;
    size_t selection_count() const { return selection_.size(); }
    void clear_selection();

    // main_window が設定するコールバック
    std::function<void()> on_selection_changed;
    std::function<void()> on_delete_requested;
    std::function<void()> on_undo_requested;
    std::function<void(int)> on_row_height_wheel;  // Ctrl+ホイール (ノッチ数, 正=拡大)

    // 現在のビューを PNG に保存する (WIC レンダーターゲット使用。
    // セッションロック中など DWM 合成が見えない状況でも E2E 検証できる)
    bool capture_to_png(const std::wstring& path);

    // E2E テスト用: display index を単独選択する
    void test_select(int display_index);
    // E2E テスト用: 音声再生位置 (ms、無ければ -1)。ズーム音声を優先し、
    // 一覧音声のみのときは最初に見つかった再生中スロットの位置を返す
    int64_t zoom_audio_position() {
        if (audio_.is_open()) return audio_.position_ms();
        for (auto& slot : grid_slots_) {
            if (slot.engine >= 0 && slot.player.is_open()) return slot.player.position_ms();
        }
        return -1;
    }
    // E2E テスト用: 一覧音声で再生中のスロット数
    int grid_audio_count() const {
        int n = 0;
        for (const auto& slot : grid_slots_) {
            if (slot.engine >= 0) ++n;
        }
        return n;
    }
    // E2E テスト用: ホバー状態を直接設定 (実マウス位置の影響を受けずに検証する)
    void test_set_hover(int display_index) {
        hover_index_ = display_index;
        mouse_tracking_ = true;  // TrackMouseEvent の再登録 (即 LEAVE) を防ぐ
    }

    // ズーム表示 (1 つをウィンドウ全体に拡大)。ダブルクリック / Esc / ←→ で操作
    bool zoomed() const { return zoom_display_index_ >= 0; }
    void enter_zoom(int display_index);
    void exit_zoom();
    void zoom_step(int delta);  // ±1 で前後のアイテムへ

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT msg, WPARAM wparam, LPARAM lparam);

    void ensure_render_target();
    void render();
    void rebuild_layout();
    void update_scroll_info();
    void scroll_by(double delta);
    void scroll_to(double y);
    int hit_test(POINT client) const;  // display index (-1 = なし)
    void handle_click(POINT client, bool ctrl, bool shift);
    void handle_key(WPARAM key);
    void sweep_bitmaps();

    HWND hwnd_ = nullptr;
    PlaybackEngine* engine_ = nullptr;

    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> render_target_;  // 従来経路
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> zoom_label_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> debug_format_;
    std::unordered_map<int, Microsoft::WRL::ComPtr<ID2D1Bitmap>> bitmaps_;  // engine index

    // ---- ゼロコピー経路 (共有 D3D デバイス上の D2D1.1 + スワップチェーン)。
    //      初期化に失敗した環境では従来の HwndRenderTarget 経路で動く ----
    bool zero_copy_ = false;
    bool zero_copy_tried_ = false;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2d_ctx_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_bitmap_;
    // GPU フレームテクスチャの D2D ラッパー (engine index -> ping-pong 2 面)
    struct GpuTileBitmaps {
        void* texture[2] = {nullptr, nullptr};  // 再作成検出用 (参照は bitmap が保持)
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap[2];
    };
    std::unordered_map<int, GpuTileBitmaps> gpu_bitmaps_;

    // 現在の描画ターゲット (ゼロコピー時は DeviceContext、従来時は HwndRenderTarget)
    ID2D1RenderTarget* rt() {
        return zero_copy_ ? static_cast<ID2D1RenderTarget*>(d2d_ctx_.Get())
                          : static_cast<ID2D1RenderTarget*>(render_target_.Get());
    }

    bool init_zero_copy();
    bool create_swapchain_target();
    void release_zero_copy();
    // GPU フレームの D2D ラッパーを取得 (テクスチャが変わっていれば作り直す)
    ID2D1Bitmap1* gpu_bitmap_for(int engine_index, int slot, ID3D11Texture2D* texture);

    std::vector<int> display_order_;
    core::Layout layout_;
    core::Selection selection_;
    core::PerformanceMode mode_ = core::PerformanceMode::Standard;
    std::vector<int> last_active_;  // engine index (set_active の差分判定)

    double scroll_y_ = 0.0;
    double row_height_ = 180.0;
    bool dark_ = true;
    bool layout_dirty_ = true;
    uint64_t seen_probe_version_ = 0;
    int hover_index_ = -1;  // display index
    int zoom_display_index_ = -1;    // -1 = ズームなし
    int zoom_fullres_engine_ = -1;   // 原寸デコードを要求中の engine index

    // ズーム中のシークバー
    bool show_seekbar_ = true;
    bool seek_dragging_ = false;
    int64_t last_seek_issue_ms_ = 0;      // ドラッグ中のシーク発行スロットル
    D2D1_RECT_F seek_hit_rect_{};         // クリック判定領域 (render で毎回更新)
    void draw_seekbar(double view_w, double view_h);
    bool handle_seek_input(POINT pt, bool is_down);  // true = シークバーが消費した

    // ズーム中の再生/一時停止
    bool zoom_paused_ = false;
    D2D1_RECT_F play_hit_rect_{};
    void toggle_zoom_pause();
    void reset_zoom_pause();  // 対象切替時に再生状態へ戻す

    // UI スケール (DPI / 96)。ズームのコントロール類に使う
    float ui_scale() const;

    // ズーム中の音声再生 (MFPlay、映像クロックへ同期)
    io::AudioPlayer audio_;
    int audio_volume_ = 60;     // 0〜100
    bool audio_muted_ = false;
    bool volume_dragging_ = false;
    int64_t audio_video_pts_ = -1;        // 直前フレームの pts (ループ検出用)
    int64_t audio_drift_check_ms_ = 0;    // 定期ドリフト補正
    D2D1_RECT_F mute_hit_rect_{};
    D2D1_RECT_F volume_hit_rect_{};
    void start_zoom_audio();              // 現在のズーム対象で音声を開く
    void stop_zoom_audio();
    void sync_zoom_audio(int64_t now_ms); // render から毎ティック呼ぶ
    void apply_audio_state();
    // 指定タイルの映像クロックへ音声を追従させる共通処理。
    // video_pts / drift_check は呼び出し側 (ズーム or 一覧スロット) が保持する
    void sync_player_to_tile(io::AudioPlayer& player, int engine_index,
                             int64_t& video_pts, int64_t& drift_check, int64_t now_ms);

    // 実験的: 一覧音声 (表示中の動画を画面順に最大 kGridAudioMax 件、同時再生)
    static constexpr int kGridAudioMax = 10;
    struct GridAudioSlot {
        io::AudioPlayer player;
        int engine = -1;         // 再生中の engine index (-1 = 空き)
        int64_t video_pts = -1;  // 映像クロック同期用 (ズームの audio_video_pts_ 相当)
        int64_t drift_check = 0;
    };
    bool grid_audio_ = false;
    std::array<GridAudioSlot, kGridAudioMax> grid_slots_;
    std::unordered_set<int> grid_audio_skip_;  // 音声なし等で open に失敗した engine index
    bool mouse_tracking_ = false;              // WM_MOUSELEAVE の TrackMouseEvent 状態
    // render (非ズーム時) から呼ぶ。visible_list は表示中タイル (画面順)
    void update_grid_audio(const std::vector<int>& visible_list, int64_t now_ms);
    void stop_grid_audio();
    int64_t start_tick_ = 0;
    int64_t last_sweep_ms_ = 0;

    // デバッグオーバーレイ
    bool debug_overlay_ = false;
    int render_fps_ = 0;          // 直近 1 秒の描画フレーム数
    int fps_counter_ = 0;
    int64_t fps_window_start_ = 0;
    uint64_t vram_usage_mb_ = 0;  // 1 秒ごとに更新するキャッシュ
    uint64_t vram_budget_mb_ = 0;

    void draw_debug_overlay(const std::vector<int>& visible_list,
                            const std::vector<int>& active_list, double view_w);
    void set_zoom_fullres(int engine_index);  // -1 で解除
    // ズーム中の表示対象と先読み対象 (display index) を作る
    void build_zoom_schedule(std::vector<int>* visible, std::vector<int>* active) const;
};

}  // namespace meguri
