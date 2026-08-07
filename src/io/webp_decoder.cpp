#include "webp_decoder.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "webp/decode.h"
#include "webp/demux.h"

namespace meguri::io {

struct WebpDecoder::Impl {
    std::vector<uint8_t> file_data;
    WebPAnimDecoder* anim = nullptr;
    core::MediaInfo info;
    std::string error;
    int64_t prev_timestamp_ms = 0;  // 直前フレームの表示開始時刻
    int default_frame_ms = 100;     // 単一フレーム時などのフォールバック

    ~Impl() {
        if (anim) WebPAnimDecoderDelete(anim);
    }
};

WebpDecoder::WebpDecoder() : impl_(std::make_unique<Impl>()) {}
WebpDecoder::~WebpDecoder() = default;

bool WebpDecoder::open(const std::wstring& path) {
    Impl& d = *impl_;
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!file) {
        d.error = "cannot open file";
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        d.error = "empty file";
        return false;
    }
    file.seekg(0);
    d.file_data.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(d.file_data.data()), size)) {
        d.error = "read failed";
        return false;
    }

    WebPData data{d.file_data.data(), d.file_data.size()};

    // メタデータ (アルファ・フレーム数・総再生時間) は demuxer から取る
    WebPDemuxer* demux = WebPDemux(&data);
    if (!demux) {
        d.error = "not a valid webp";
        return false;
    }
    d.info.width = static_cast<int>(WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH));
    d.info.height = static_cast<int>(WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT));
    d.info.frame_count = static_cast<int>(WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT));
    const uint32_t flags = WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS);
    d.info.has_alpha = (flags & ALPHA_FLAG) != 0;
    d.info.loops = true;  // ループ回数指定があってもツールの用途上ループ再生する

    int64_t total_ms = 0;
    WebPIterator iter;
    if (WebPDemuxGetFrame(demux, 1, &iter)) {
        do {
            total_ms += iter.duration > 0 ? iter.duration : d.default_frame_ms;
        } while (WebPDemuxNextFrame(&iter));
        WebPDemuxReleaseIterator(&iter);
    }
    d.info.duration_sec = static_cast<double>(total_ms) / 1000.0;
    WebPDemuxDelete(demux);

    WebPAnimDecoderOptions options;
    WebPAnimDecoderOptionsInit(&options);
    options.color_mode = MODE_BGRA;  // Direct2D / PNG 出力と揃える
    options.use_threads = 0;         // 並列化はファイル単位で行う (タイルごとに 1 スレッド)
    d.anim = WebPAnimDecoderNew(&data, &options);
    if (!d.anim) {
        d.error = "WebPAnimDecoderNew failed";
        return false;
    }
    d.prev_timestamp_ms = 0;
    return true;
}

const core::MediaInfo& WebpDecoder::info() const { return impl_->info; }

bool WebpDecoder::next_frame(core::VideoFrame& out) {
    Impl& d = *impl_;
    if (!d.anim) return false;
    if (!WebPAnimDecoderHasMoreFrames(d.anim)) return false;

    uint8_t* buf = nullptr;
    int timestamp_ms = 0;  // このフレームの表示「終了」時刻
    if (!WebPAnimDecoderGetNext(d.anim, &buf, &timestamp_ms)) {
        d.error = "decode failed";
        return false;
    }
    out.width = d.info.width;
    out.height = d.info.height;
    const size_t bytes = static_cast<size_t>(d.info.width) * d.info.height * 4;
    out.bgra.resize(bytes);
    std::memcpy(out.bgra.data(), buf, bytes);

    out.pts_ms = d.prev_timestamp_ms;
    int duration = static_cast<int>(timestamp_ms - d.prev_timestamp_ms);
    if (duration <= 0) duration = d.default_frame_ms;
    out.duration_ms = duration;
    d.prev_timestamp_ms = timestamp_ms;
    return true;
}

void WebpDecoder::rewind() {
    Impl& d = *impl_;
    if (!d.anim) return;
    WebPAnimDecoderReset(d.anim);
    d.prev_timestamp_ms = 0;
}

void WebpDecoder::seek_ms(int64_t position_ms) {
    // WebP はランダムアクセスできないため、先頭から目的時刻までデコードスキップする
    // (フレームのコピーはしない)。短いアニメーション用途なのでコストは小さい
    Impl& d = *impl_;
    if (!d.anim) return;
    rewind();
    if (position_ms <= 0) return;
    uint8_t* buf = nullptr;
    int timestamp_ms = 0;
    while (WebPAnimDecoderHasMoreFrames(d.anim)) {
        if (!WebPAnimDecoderGetNext(d.anim, &buf, &timestamp_ms)) break;
        d.prev_timestamp_ms = timestamp_ms;
        if (timestamp_ms >= position_ms) break;
    }
}

const std::string& WebpDecoder::error_message() const { return impl_->error; }

}  // namespace meguri::io
