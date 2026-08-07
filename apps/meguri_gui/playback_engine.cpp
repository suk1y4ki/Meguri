#include "playback_engine.h"

#include <d3d11.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>

#include "io/d3d_video.h"
#include "io/mp4_decoder.h"
#include "io/probe.h"

namespace meguri {

namespace {

// straight alpha -> premultiplied (Direct2D の PREMULTIPLIED ビットマップ用)
void premultiply_bgra(std::vector<uint8_t>& bgra) {
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
        const uint32_t a = bgra[i + 3];
        if (a == 255) continue;
        bgra[i + 0] = static_cast<uint8_t>(bgra[i + 0] * a / 255);
        bgra[i + 1] = static_cast<uint8_t>(bgra[i + 1] * a / 255);
        bgra[i + 2] = static_cast<uint8_t>(bgra[i + 2] * a / 255);
    }
}

// ゼロコピー用のタイル出力テクスチャ (BGRA、共有 VP の変換先 = D2D の描画元)
Microsoft::WRL::ComPtr<ID3D11Texture2D> create_tile_texture(int width, int height) {
    ID3D11Device* device = io::shared_d3d_device();
    if (!device) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) return nullptr;
    return texture;
}

}  // namespace

PlaybackEngine::PlaybackEngine() {
    unsigned n = std::thread::hardware_concurrency();
    // デコードは CPU バウンド。UI と描画のために少し残す
    unsigned worker_count = n > 2 ? n - 2 : 1;
    if (worker_count > 16) worker_count = 16;
    for (unsigned i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // MF 用
            worker_loop();
            CoUninitialize();
        });
    }
}

PlaybackEngine::~PlaybackEngine() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stopping_ = true;
        high_queue_.clear();
        low_queue_.clear();
    }
    queue_cv_.notify_all();
    for (auto& w : workers_) w.join();
}

void PlaybackEngine::set_library(std::vector<core::MediaItem> items) {
    // 旧タイルのデコーダ解放を積んでから差し替え (shared_ptr がジョブ完了まで生かす)
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        low_queue_.clear();  // 旧プローブは不要
        for (auto& t : tiles_) {
            t->active = false;
            high_queue_.push_back({JobKind::Close, t});
        }
    }
    queue_cv_.notify_all();

    tiles_.clear();
    active_indices_.clear();
    probed_count_ = 0;
    for (auto& item : items) {
        auto t = std::make_shared<Tile>();
        t->item = std::move(item);
        // 走査段階でサイズが分かっていれば (再オープン時など) プローブ不要
        t->probed = t->item.width > 0 && t->item.height > 0;
        if (t->probed) ++probed_count_;
        tiles_.push_back(std::move(t));
    }

    // プローブジョブ (低優先度)。表示順に積む
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (auto& t : tiles_) {
            if (!t->probed) low_queue_.push_back({JobKind::Probe, t});
        }
    }
    queue_cv_.notify_all();
}

void PlaybackEngine::set_active(const std::vector<int>& active_indices) {
    // 非アクティブ化されたものはデコーダを閉じてメモリを返す
    for (int old_index : active_indices_) {
        if (old_index < 0 || old_index >= tile_count()) continue;
        const bool still_active =
            std::find(active_indices.begin(), active_indices.end(), old_index) !=
            active_indices.end();
        if (!still_active) {
            auto& t = tiles_[old_index];
            t->active = false;
            std::lock_guard<std::mutex> lock(queue_mutex_);
            high_queue_.push_back({JobKind::Close, t});
        }
    }
    for (int index : active_indices) {
        if (index < 0 || index >= tile_count()) continue;
        auto& t = tiles_[index];
        if (!t->active.exchange(true)) {
            t->next_due_ms = 0;  // すぐ最初のフレームを要求する
        }
    }

    // 可視付近の未プローブを最優先でプローブする (レイアウト確定を早める)。
    // 全件プローブは低優先度キューが背景で続ける (二重実行は probe_started で防ぐ)
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (auto it = active_indices.rbegin(); it != active_indices.rend(); ++it) {
            if (*it < 0 || *it >= tile_count()) continue;
            auto& t = tiles_[*it];
            if (!t->probed.load() && !t->probe_started.load()) {
                high_queue_.push_front({JobKind::Probe, t});
            }
        }
    }

    active_indices_ = active_indices;
    queue_cv_.notify_all();
}

void PlaybackEngine::tick(int64_t now_ms) {
    last_tick_ms_ = now_ms;

    // GPU 枠が回復したら SW 稼働中のタイルを順次 GPU へ戻す (3 秒ごとに 2 枚まで)
    if (now_ms - last_upgrade_check_ms_ >= 3000) {
        last_upgrade_check_ms_ = now_ms;
        maybe_upgrade_software_tiles();
    }

    bool queued = false;
    for (int index : active_indices_) {
        if (index < 0 || index >= tile_count()) continue;
        auto& t = tiles_[index];
        if (t->failed) continue;
        // 一時停止中はデコードしない (ただしシーク要求はプレビューのため 1 回通す)
        if (t->paused.load() && t->seek_target_ms.load() < 0) continue;
        if (now_ms < t->next_due_ms) continue;
        if (t->job_pending.exchange(true)) continue;  // 既にジョブあり
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            high_queue_.push_back({JobKind::Decode, tiles_[index]});
        }
        queued = true;
    }
    if (queued) queue_cv_.notify_all();
}

void PlaybackEngine::frame_consumed(int index, int64_t now_ms, int duration_ms) {
    if (index < 0 || index >= tile_count()) return;
    if (duration_ms < 5) duration_ms = 5;

    // ボトルネック緩和: デコードが重いタイルは自分のフレームレートを落とし、
    // ワーカープールを占有して他のタイルを遅くしないようにする
    // (1 タイルがワーカー 1 本の 40% を超えない周期に制限)
    const int min_period_ms = tiles_[index]->decode_us_ema.load() * 25 / 10000;  // 2.5 倍
    if (duration_ms < min_period_ms) duration_ms = min_period_ms;
    // 「消費した時刻 + 表示時間」ではなく「予定時刻 + 表示時間」で次の期限を刻む。
    // 前者だとジョブ投入→デコード→描画反映のレイテンシが毎フレーム再生周期に
    // 加算され、再生速度が 3 割ほど遅くなる (33ms の動画が実測 20fps になっていた)。
    // 予定時刻ベースなら遅延は「一定の表示遅れ」に留まり、レートは正確になる
    auto& t = tiles_[index];
    const int64_t scheduled = t->next_due_ms.load();
    int64_t next = scheduled + duration_ms;
    if (scheduled <= 0 || next < now_ms - 500) {
        // 初回、またはデコードが追いつかず大きく遅れたときは現在時刻へリシンク
        next = now_ms + duration_ms;
    }
    t->next_due_ms = next;
}

PlaybackEngine::Tile* PlaybackEngine::tile(int index) {
    if (index < 0 || index >= tile_count()) return nullptr;
    return tiles_[index].get();
}

core::MediaItem PlaybackEngine::item_snapshot(int index) {
    if (index < 0 || index >= tile_count()) return {};
    Tile& t = *tiles_[index];
    std::lock_guard<std::mutex> lock(t.item_mutex);
    return t.item;
}

void PlaybackEngine::maybe_upgrade_software_tiles() {
    int used = 0, cap = 0;
    io::gpu_decoder_stats(&used, &cap);
    int headroom = cap - used - 2;  // 上限ぎりぎりまで詰めない (再失敗を避ける)
    if (headroom <= 0) return;

    int upgraded = 0;
    for (int index : active_indices_) {
        if (index < 0 || index >= tile_count()) continue;
        auto& t = tiles_[index];
        if (t->item.type != core::MediaType::Mp4) continue;
        if (t->hardware || t->failed || t->job_pending.load()) continue;
        if (t->latest_version == 0) continue;  // まだ一度も再生できていないものは対象外

        // GPU で開き直す (失敗すれば従来どおり SW へフォールバックする)
        t->prefer_software = false;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            high_queue_.push_back({JobKind::Reopen, t});
        }
        t->next_due_ms = 0;
        queue_cv_.notify_all();
        if (++upgraded >= 2 || --headroom <= 0) break;
    }
}

bool PlaybackEngine::dump_state(const std::wstring& path) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"w, ccs=UTF-8") != 0 || !file) return false;
    fwprintf(file, L"probed_count=%d probe_version=%llu tile_count=%d\n", probed_count_.load(),
             static_cast<unsigned long long>(probe_version_.load()), tile_count());
    fwprintf(file,
             L"index\tactive\tfailed\tfail_count\ttotal_fail\thw\topen_ms\tdec_us\tframes\tpts_ms"
             L"\terror\tpath\n");
    for (int i = 0; i < tile_count(); ++i) {
        Tile& t = *tiles_[i];
        std::string error;
        {
            std::lock_guard<std::mutex> lock(t.item_mutex);
            error = t.last_error;
        }
        int64_t pts_ms = -1;
        {
            std::lock_guard<std::mutex> lock(t.frame_mutex);
            if (t.latest_version > 0) pts_ms = t.latest.pts_ms;
        }
        fwprintf(file, L"%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%lld\t%hs\t%s\n", i,
                 t.active.load() ? 1 : 0, t.failed.load() ? 1 : 0, t.fail_count.load(),
                 t.total_failures.load(), t.hardware.load() ? 1 : 0, t.open_ms.load(),
                 t.decode_us_ema.load(), t.frames_decoded.load(),
                 static_cast<long long>(pts_ms), error.c_str(), t.item.path.c_str());
    }
    fclose(file);
    return true;
}

void PlaybackEngine::request_seek(int index, int64_t position_ms) {
    if (index < 0 || index >= tile_count()) return;
    auto& t = tiles_[index];
    t->seek_target_ms = position_ms;
    t->next_due_ms = 0;  // すぐ反映
}

void PlaybackEngine::set_tile_paused(int index, bool paused) {
    if (index < 0 || index >= tile_count()) return;
    auto& t = tiles_[index];
    t->paused = paused;
    if (!paused) t->next_due_ms = 0;  // 再開時はすぐ次のフレームへ
}

void PlaybackEngine::request_full_resolution(int index, bool on) {
    if (index < 0 || index >= tile_count()) return;
    auto& t = tiles_[index];
    if (t->full_resolution.exchange(on) == on) return;  // 変化なし
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        high_queue_.push_back({JobKind::Reopen, t});
    }
    t->next_due_ms = 0;  // すぐ新解像度のフレームを要求
    queue_cv_.notify_all();
}

void PlaybackEngine::worker_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return stopping_ || !high_queue_.empty() || !low_queue_.empty();
            });
            if (stopping_) return;
            if (!high_queue_.empty()) {
                job = std::move(high_queue_.front());
                high_queue_.pop_front();
            } else {
                job = std::move(low_queue_.front());
                low_queue_.pop_front();
            }
        }
        Tile& t = *job.tile;
        switch (job.kind) {
            case JobKind::Decode:
                if (t.active) run_decode(t);
                t.job_pending = false;
                break;
            case JobKind::Close: {
                std::lock_guard<std::mutex> lock(t.item_mutex);
                if (!t.active) {  // 再アクティブ化されていたら残す
                    t.decoder.reset();
                    t.decoder_opened = false;
                    // 失敗状態は「そのときの GPU 混雑」に依存するため、
                    // 画面から外れたらリセットして再訪時にやり直す
                    t.failed = false;
                    t.fail_count = 0;
                    t.prefer_software = false;
                    t.paused = false;
                    std::lock_guard<std::mutex> flock(t.frame_mutex);
                    t.latest = core::VideoFrame{};  // フレームメモリも返す
                    t.gpu_tex[0].Reset();           // GPU テクスチャも返す
                    t.gpu_tex[1].Reset();
                    t.gpu_front = -1;
                }
                break;
            }
            case JobKind::Reopen: {
                // 解像度切替 (ズーム開始/終了)。CPU 側の直近フレームは残して
                // 映像の穴を防ぐ。GPU テクスチャはサイズが変わるため作り直す
                std::lock_guard<std::mutex> lock(t.item_mutex);
                t.decoder.reset();
                t.decoder_opened = false;
                std::lock_guard<std::mutex> flock(t.frame_mutex);
                t.gpu_tex[0].Reset();
                t.gpu_tex[1].Reset();
                t.gpu_front = -1;
                break;
            }
            case JobKind::Probe:
                run_probe(t);
                break;
        }
    }
}

void PlaybackEngine::run_decode(Tile& t) {
    std::lock_guard<std::mutex> lock(t.item_mutex);

    // 一時的な失敗 (多数の GPU デコーダを同時に開いたときのリソース枯渇等) は
    // デコーダを作り直してバックオフ付きでリトライする。連続で失敗し続けた
    // ものだけを永久失敗にする
    constexpr int kMaxFailures = 5;
    auto note_failure = [&] {
        if (t.decoder) t.last_error = t.decoder->error_message();
        // HW デコーダの実行時失敗 (ReadSample の E_OUTOFMEMORY 等) は同時デコーダが
        // 増えすぎてドライバ内部リソースが尽きたときに起こる。open は成功して
        // しまうため、(1) このタイルのリトライはソフトウェアで開き、
        // (2) 全体の動的上限を下げて以後の失敗の連鎖を止める
        if (t.hardware) {
            t.prefer_software = true;
            io::note_gpu_runtime_failure();
        }
        t.decoder.reset();
        t.decoder_opened = false;
        t.hardware = false;
        ++t.total_failures;
        const int failures = ++t.fail_count;
        if (failures >= kMaxFailures) {
            t.failed = true;
        } else {
            t.next_due_ms = last_tick_ms_.load() + 500ll * failures;
        }
    };

    using Clock = std::chrono::steady_clock;
    if (!t.decoder) {
        t.decoder = io::create_decoder(t.item.type);
        // タイル表示なら縮小デコード、ズーム表示なら原寸
        t.decoder->set_max_output_dimension(t.full_resolution ? 0 : decode_limit_.load());
        if (t.prefer_software) t.decoder->set_allow_hardware(false);
        // ゼロコピー描画時は MP4 を GPU テクスチャモードで開く
        auto* mp4 = dynamic_cast<io::Mp4Decoder*>(t.decoder.get());
        if (mp4 && zero_copy_.load() && !t.prefer_software) mp4->set_gpu_output(true);

        const auto open_start = Clock::now();
        t.decoder_opened = t.decoder->open(t.item.path);
        t.open_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - open_start)
                .count());
        if (!t.decoder_opened) {
            note_failure();
            return;
        }
        t.has_alpha = t.decoder->info().has_alpha;
        t.hardware = t.decoder->info().hardware;

        // イントロオフセット: 5 分以上の動画はロゴ画面等を飛ばして 3:00 から
        t.start_offset_ms =
            intro_offset_.load() && t.decoder->info().duration_sec >= 300.0 ? 180000 : 0;
        if (t.start_offset_ms > 0) t.decoder->seek_ms(t.start_offset_ms);

        // GPU テクスチャモードなら出力テクスチャ (ping-pong) を用意する
        if (mp4 && mp4->gpu_frames_active()) {
            const core::MediaInfo& info = t.decoder->info();
            int out_w = info.width, out_h = info.height;
            const int limit = t.full_resolution ? 0 : decode_limit_.load();
            const int longer = out_w > out_h ? out_w : out_h;
            if (limit > 0 && longer > limit) {
                const double scale = static_cast<double>(limit) / longer;
                out_w = static_cast<int>(out_w * scale);
                out_h = static_cast<int>(out_h * scale);
                if (out_w < 2) out_w = 2;
                if (out_h < 2) out_h = 2;
            }
            auto tex0 = create_tile_texture(out_w, out_h);
            auto tex1 = create_tile_texture(out_w, out_h);
            std::lock_guard<std::mutex> flock(t.frame_mutex);
            t.gpu_tex[0] = tex0;
            t.gpu_tex[1] = tex1;
            t.gpu_front = -1;
            t.gpu_back = 0;
        }
    }

    auto* mp4 = dynamic_cast<io::Mp4Decoder*>(t.decoder.get());
    const bool gpu_frames = mp4 && mp4->gpu_frames_active() && t.gpu_tex[0] && t.gpu_tex[1];

    // シーク要求 (ズーム中のシークバー) があれば反映
    const int64_t seek_to = t.seek_target_ms.exchange(-1);
    if (seek_to >= 0) t.decoder->seek_ms(seek_to);

    const auto decode_start = Clock::now();
    core::VideoFrame frame;
    io::GpuFrameInfo gpu_info;
    // 終端 → イントロオフセット位置 (無ければ先頭) からループ
    auto restart = [&] {
        if (t.start_offset_ms > 0) {
            t.decoder->seek_ms(t.start_offset_ms);
        } else {
            t.decoder->rewind();
        }
    };
    if (gpu_frames) {
        ID3D11Texture2D* target = t.gpu_tex[t.gpu_back].Get();
        if (!mp4->next_frame_to_texture(target, &gpu_info)) {
            restart();
            if (!mp4->next_frame_to_texture(target, &gpu_info)) {
                note_failure();
                return;
            }
        }
    } else {
        if (!t.decoder->next_frame(frame)) {
            restart();
            if (!t.decoder->next_frame(frame)) {
                // 巻き戻しても取れない (デコーダが壊れた等)。作り直してリトライ
                note_failure();
                return;
            }
        }
    }
    t.fail_count = 0;
    ++t.frames_decoded;
    const int decode_us = static_cast<int>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - decode_start)
            .count());
    const int ema = t.decode_us_ema.load();
    t.decode_us_ema = ema > 0 ? (ema * 7 + decode_us) / 8 : decode_us;

    if (gpu_frames) {
        D3D11_TEXTURE2D_DESC desc{};
        t.gpu_tex[t.gpu_back]->GetDesc(&desc);
        std::lock_guard<std::mutex> flock(t.frame_mutex);
        t.gpu_front = t.gpu_back;
        t.gpu_back ^= 1;
        t.latest.bgra.clear();  // GPU フレームの目印 (bgra 空 + gpu_front >= 0)
        t.latest.width = static_cast<int>(desc.Width);
        t.latest.height = static_cast<int>(desc.Height);
        t.latest.pts_ms = gpu_info.pts_ms;
        t.latest.duration_ms = gpu_info.duration_ms;
        ++t.latest_version;
        return;
    }

    if (t.has_alpha) premultiply_bgra(frame.bgra);
    {
        std::lock_guard<std::mutex> flock(t.frame_mutex);
        t.latest = std::move(frame);
        t.gpu_front = -1;
        ++t.latest_version;
    }
}

void PlaybackEngine::run_probe(Tile& t) {
    if (t.probed) return;
    if (t.probe_started.exchange(true)) return;  // 優先/通常キューの二重実行防止
    core::MediaItem copy;
    {
        std::lock_guard<std::mutex> lock(t.item_mutex);
        copy = t.item;
    }
    const bool ok = io::probe_media_item(copy);
    {
        std::lock_guard<std::mutex> lock(t.item_mutex);
        if (ok) {
            t.item.width = copy.width;
            t.item.height = copy.height;
            t.item.duration_sec = copy.duration_sec;
            t.item.frame_count = copy.frame_count;
        } else {
            t.failed = true;
        }
    }
    t.probed = true;
    ++probed_count_;
    ++probe_version_;
}

}  // namespace meguri
