// 再生エンジン。ワーカースレッドプールでアクティブなタイルのフレームを
// 逐次デコードし、UI スレッドが最新フレームを取り出して描画する。
//
// 設計:
// - タイルごとに「未処理ジョブは常に 1 つまで」。UI の tick() が表示期限の来た
//   タイルだけジョブ投入するため、見えていないタイルの CPU を食わない
// - デコーダはワーカースレッドのみが触る (item_mutex 保護)。UI は frame_mutex で
//   最新フレームのコピーだけを受け取る
// - メタデータ未取得のアイテムはプローブジョブ (低優先度) が width/height を埋める
#pragma once

#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/decoder.h"
#include "core/media_item.h"

struct ID3D11Texture2D;

namespace meguri {

class PlaybackEngine {
public:
    struct Tile {
        core::MediaItem item;

        // ---- ワーカーのみ (item_mutex 保護) ----
        std::mutex item_mutex;
        std::unique_ptr<core::IMediaDecoder> decoder;
        bool decoder_opened = false;

        // ---- 受け渡し (frame_mutex 保護) ----
        std::mutex frame_mutex;
        core::VideoFrame latest;        // 最新デコード済みフレーム (アルファはプリマルチ済み)
        uint64_t latest_version = 0;    // 更新のたび +1
        bool has_alpha = false;

        // GPU テクスチャ経路 (ゼロコピー): bgra が空で gpu_front >= 0 なら
        // gpu_tex[gpu_front] に BGRA 変換済みフレームが入っている
        Microsoft::WRL::ComPtr<ID3D11Texture2D> gpu_tex[2];  // ping-pong
        int gpu_front = -1;
        int gpu_back = 0;  // ワーカーのみ

        // ---- 状態フラグ ----
        std::atomic<bool> job_pending{false};
        std::atomic<bool> active{false};
        std::atomic<bool> failed{false};  // fail_count が上限に達したら立つ (永久失敗)
        std::atomic<bool> probed{false};
        std::atomic<bool> probe_started{false};  // 優先キューと通常キューの二重実行防止
        std::atomic<bool> full_resolution{false};  // ズーム表示中は原寸でデコード
        std::atomic<int> fail_count{0};  // 連続失敗回数。成功でリセット。一時的な
                                         // 失敗 (GPU リソース枯渇等) はリトライする
        std::atomic<bool> prefer_software{false};  // HW デコーダが実行時に失敗した後、
                                                   // リトライを SW で行うためのフラグ
        std::atomic<int64_t> seek_target_ms{-1};   // シーク要求 (-1 = なし)。ワーカーが消費
        int64_t start_offset_ms = 0;               // イントロオフセット (ワーカーのみ)
        std::atomic<bool> paused{false};           // 一時停止 (ズーム中の再生ボタン)

        // UI が期限設定、ワーカーが失敗バックオフで延長するため atomic
        std::atomic<int64_t> next_due_ms{0};
        uint64_t shown_version = 0;  // 描画済みバージョン (UI スレッドのみ)

        // ---- デバッグオーバーレイ用の計測値 ----
        std::atomic<int> open_ms{-1};        // 直近の open 所要時間 (-1 = 未計測)
        std::atomic<int> decode_us_ema{0};   // 1 フレームのデコード時間 (μs, 指数移動平均)
        std::atomic<bool> hardware{false};   // GPU (DXVA) でデコード中か
        std::string last_error;              // 直近の失敗理由 (item_mutex 保護)
        std::atomic<int> total_failures{0};  // 累計失敗回数 (診断用)
        std::atomic<int> frames_decoded{0};  // 累計デコードフレーム数 (実効 fps の診断用)
    };

    PlaybackEngine();
    ~PlaybackEngine();

    // ライブラリを差し替える (フォルダを開いたとき)。プローブジョブを積む。
    void set_library(std::vector<core::MediaItem> items);

    // 表示順 index (set_library の順) のアクティブ集合を差し替える。
    // 非アクティブ化されたタイルのデコーダは解放ジョブで閉じる。
    void set_active(const std::vector<int>& active_indices);

    // UI の描画ティックから呼ぶ。期限が来たアクティブタイルへデコードジョブを積む。
    void tick(int64_t now_ms);

    // 新フレームを消費したら UI が呼ぶ (次フレームの期限を設定)
    void frame_consumed(int index, int64_t now_ms, int duration_ms);

    // タイル表示に十分な最大デコード寸法 (長辺 px、0 = 原寸)。
    // 既に開いているデコーダには影響しない (次に開くときに適用)
    void set_decode_limit(int max_dim) { decode_limit_ = max_dim; }

    // ゼロコピー描画 (レンダラーが共有 D3D デバイス上にあるとき)。
    // MP4 の GPU デコードは NV12 テクスチャ → 共有 VP 変換になり CPU コピーが消える
    void set_zero_copy(bool enabled) { zero_copy_ = enabled; }

    // ズーム表示用: 原寸デコードへの切替。デコーダが開いていれば開き直す
    void request_full_resolution(int index, bool on);

    // シーク要求 (ズーム中のシークバー用)。次のデコードで反映される
    // (一時停止中でもシーク先の 1 フレームだけデコードされる)
    void request_seek(int index, int64_t position_ms);

    // 一時停止 (ズーム中の再生ボタン)。解除時はすぐ次のフレームを要求する
    void set_tile_paused(int index, bool paused);

    // イントロオフセット: 5 分以上の動画を 3:00 から再生する (新規オープンに適用)
    void set_intro_offset(bool enabled) { intro_offset_ = enabled; }

    Tile* tile(int index);
    int tile_count() const { return static_cast<int>(tiles_.size()); }

    // item のスナップショット (ワーカーと競合しない安全な読み出し。キャッシュ保存用)
    core::MediaItem item_snapshot(int index);

    // プローブ完了数 (レイアウト再計算のトリガー用)
    uint64_t probe_version() const { return probe_version_.load(); }
    int probed_count() const { return probed_count_.load(); }

    // 診断用: 全タイルの状態をテキストファイルへ書き出す
    bool dump_state(const std::wstring& path);

private:
    enum class JobKind { Decode, Close, Reopen, Probe };
    struct Job {
        JobKind kind;
        std::shared_ptr<Tile> tile;
    };

    void worker_loop();
    void run_decode(Tile& t);
    void run_probe(Tile& t);
    void enqueue_front(Job job);
    void enqueue_back(Job job);

    std::vector<std::shared_ptr<Tile>> tiles_;
    std::vector<int> active_indices_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Job> high_queue_;  // Decode / Close
    std::deque<Job> low_queue_;   // Probe
    bool stopping_ = false;

    std::vector<std::thread> workers_;
    std::atomic<uint64_t> probe_version_{0};
    std::atomic<int> probed_count_{0};
    std::atomic<int> decode_limit_{0};
    std::atomic<bool> zero_copy_{false};
    std::atomic<bool> intro_offset_{true};
    std::atomic<int64_t> last_tick_ms_{0};  // ワーカーの失敗バックオフ計算用
    int64_t last_upgrade_check_ms_ = 0;     // SW→GPU 昇格の周期チェック (UI スレッドのみ)

    // GPU 枠に空きがあれば、SW で動いているタイルを GPU で開き直す (tick から呼ぶ)
    void maybe_upgrade_software_tiles();
};

}  // namespace meguri
