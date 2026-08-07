#include "sample_gen.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "mp4_decoder.h"  // ensure_media_foundation
#include "webp/encode.h"
#include "webp/mux.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace meguri::io {

namespace {

// 動きが分かるテストパターンを BGRA で描く: 色付き背景 + 跳ねるボール + 走査バー
void render_test_frame(std::vector<uint8_t>& bgra, int w, int h, int frame, int total,
                       int seed) {
    bgra.resize(static_cast<size_t>(w) * h * 4);
    const double t = total > 0 ? static_cast<double>(frame) / total : 0.0;

    const uint8_t bg_b = static_cast<uint8_t>(40 + (seed * 53) % 120);
    const uint8_t bg_g = static_cast<uint8_t>(40 + (seed * 97) % 120);
    const uint8_t bg_r = static_cast<uint8_t>(40 + (seed * 31) % 120);

    // ボール軌道: 横は往復、縦は放物線ぽく
    const double phase = t * 2.0 * 3.14159265358979;
    const double cx = w * (0.5 + 0.38 * std::sin(phase + seed));
    const double cy = h * (0.5 + 0.38 * std::cos(phase * 2.0 + seed * 0.7));
    const double radius = (w < h ? w : h) * 0.18;
    const int bar_x = static_cast<int>(t * w) % (w > 0 ? w : 1);

    for (int y = 0; y < h; ++y) {
        uint8_t* row = bgra.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            uint8_t b = bg_b, g = bg_g, r = bg_r;
            if (x >= bar_x && x < bar_x + w / 16 + 2) {
                b = static_cast<uint8_t>(b + 60);
                g = static_cast<uint8_t>(g + 60);
                r = static_cast<uint8_t>(r + 60);
            }
            const double dx = x - cx;
            const double dy = y - cy;
            if (dx * dx + dy * dy < radius * radius) {
                b = static_cast<uint8_t>(255 - (seed * 37) % 128);
                g = static_cast<uint8_t>(80 + (seed * 71) % 160);
                r = static_cast<uint8_t>(255 - (seed * 13) % 200);
            }
            // 左上に白マーカー (上下反転の検出用)
            if (x < w / 10 && y < h / 10) {
                b = g = r = 255;
            }
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 255;
        }
    }
}

}  // namespace

bool generate_sample_webp(const std::wstring& path, const SampleSpec& spec,
                          std::string* error) {
    WebPAnimEncoderOptions enc_options;
    WebPAnimEncoderOptionsInit(&enc_options);
    enc_options.anim_params.loop_count = 0;  // 無限ループ

    WebPAnimEncoder* enc = WebPAnimEncoderNew(spec.width, spec.height, &enc_options);
    if (!enc) {
        if (error) *error = "WebPAnimEncoderNew failed";
        return false;
    }

    WebPConfig config;
    WebPConfigInit(&config);
    config.quality = 75.0f;

    std::vector<uint8_t> frame;
    bool ok = true;
    for (int i = 0; i < spec.frame_count && ok; ++i) {
        render_test_frame(frame, spec.width, spec.height, i, spec.frame_count, spec.seed);
        WebPPicture pic;
        WebPPictureInit(&pic);
        pic.width = spec.width;
        pic.height = spec.height;
        pic.use_argb = 1;
        if (!WebPPictureImportBGRA(&pic, frame.data(), spec.width * 4)) {
            ok = false;
        } else {
            ok = WebPAnimEncoderAdd(enc, &pic, i * spec.frame_ms, &config) != 0;
        }
        WebPPictureFree(&pic);
    }
    if (ok) {
        // 最終フレームの表示終了時刻を確定させる
        ok = WebPAnimEncoderAdd(enc, nullptr, spec.frame_count * spec.frame_ms, nullptr) != 0;
    }

    WebPData data;
    WebPDataInit(&data);
    if (ok) ok = WebPAnimEncoderAssemble(enc, &data) != 0;
    WebPAnimEncoderDelete(enc);
    if (!ok) {
        WebPDataClear(&data);
        if (error) *error = "webp encode failed";
        return false;
    }

    std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char*>(data.bytes),
                             static_cast<std::streamsize>(data.size))) {
        WebPDataClear(&data);
        if (error) *error = "write failed";
        return false;
    }
    WebPDataClear(&data);
    return true;
}

bool generate_sample_mp4(const std::wstring& path, const SampleSpec& spec,
                         std::string* error) {
    if (!ensure_media_foundation()) {
        if (error) *error = "Media Foundation unavailable";
        return false;
    }

    // H.264 は偶数サイズが安全
    const int w = spec.width & ~1;
    const int h = spec.height & ~1;
    int fps = spec.frame_ms > 0 ? (1000 + spec.frame_ms / 2) / spec.frame_ms : 30;
    if (fps < 1) fps = 1;
    const LONGLONG frame_100ns = 10000000LL / fps;

    ComPtr<IMFSinkWriter> writer;
    HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &writer);
    if (FAILED(hr)) {
        if (error) *error = "MFCreateSinkWriterFromURL failed";
        return false;
    }

    ComPtr<IMFMediaType> out_type;
    MFCreateMediaType(&out_type);
    out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out_type->SetUINT32(MF_MT_AVG_BITRATE, 2000000);
    out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD stream_index = 0;
    hr = writer->AddStream(out_type.Get(), &stream_index);
    if (FAILED(hr)) {
        if (error) *error = "AddStream(H264) failed";
        return false;
    }

    ComPtr<IMFMediaType> in_type;
    MFCreateMediaType(&in_type);
    in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    // RGB32 の既定はボトムアップ。stride 属性はエンコーダに無視されることがあるため
    // 属性に頼らず、サンプル作成時に行を上下反転して渡す (E2E で実測して確認済み)
    MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(in_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = writer->SetInputMediaType(stream_index, in_type.Get(), nullptr);
    if (FAILED(hr)) {
        if (error) *error = "SetInputMediaType(RGB32) failed";
        return false;
    }

    // 音声トラック (AAC、サイン波)。音声再生機能の E2E 用
    constexpr UINT32 kSampleRate = 44100;
    constexpr UINT32 kChannels = 2;
    DWORD audio_stream = 0;
    bool has_audio = false;
    {
        ComPtr<IMFMediaType> audio_out;
        MFCreateMediaType(&audio_out);
        audio_out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audio_out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        audio_out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
        audio_out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
        audio_out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audio_out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 12000);
        ComPtr<IMFMediaType> audio_in;
        MFCreateMediaType(&audio_in);
        audio_in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audio_in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        audio_in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
        audio_in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
        audio_in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audio_in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
        audio_in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kSampleRate * kChannels * 2);
        if (SUCCEEDED(writer->AddStream(audio_out.Get(), &audio_stream)) &&
            SUCCEEDED(writer->SetInputMediaType(audio_stream, audio_in.Get(), nullptr))) {
            has_audio = true;
        }
    }

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        if (error) *error = "BeginWriting failed";
        return false;
    }

    const DWORD frame_bytes = static_cast<DWORD>(w) * h * 4;
    std::vector<uint8_t> frame;
    for (int i = 0; i < spec.frame_count; ++i) {
        render_test_frame(frame, w, h, i, spec.frame_count, spec.seed);

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateMemoryBuffer(frame_bytes, &buffer))) {
            if (error) *error = "MFCreateMemoryBuffer failed";
            return false;
        }
        BYTE* dst = nullptr;
        buffer->Lock(&dst, nullptr, nullptr);
        const size_t row_bytes = static_cast<size_t>(w) * 4;
        for (int y = 0; y < h; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * row_bytes,
                        frame.data() + static_cast<size_t>(h - 1 - y) * row_bytes, row_bytes);
        }
        buffer->Unlock();
        buffer->SetCurrentLength(frame_bytes);

        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(i * frame_100ns);
        sample->SetSampleDuration(frame_100ns);
        hr = writer->WriteSample(stream_index, sample.Get());
        if (FAILED(hr)) {
            if (error) *error = "WriteSample failed";
            return false;
        }
    }

    // 音声: 動画長ぶんのサイン波 (seed で周波数を変える) を 100ms 単位で書き込む
    if (has_audio) {
        const double freq = 220.0 + (spec.seed % 12) * 55.0;
        const double total_sec =
            static_cast<double>(spec.frame_count) * frame_100ns / 10000000.0;
        const UINT32 chunk_frames = kSampleRate / 10;  // 100ms
        const DWORD chunk_bytes = chunk_frames * kChannels * 2;
        int64_t written_frames = 0;
        const int64_t total_frames = static_cast<int64_t>(total_sec * kSampleRate);
        while (written_frames < total_frames) {
            const UINT32 n = static_cast<UINT32>(
                std::min<int64_t>(chunk_frames, total_frames - written_frames));
            ComPtr<IMFMediaBuffer> abuf;
            if (FAILED(MFCreateMemoryBuffer(chunk_bytes, &abuf))) break;
            BYTE* dst = nullptr;
            abuf->Lock(&dst, nullptr, nullptr);
            int16_t* pcm = reinterpret_cast<int16_t*>(dst);
            for (UINT32 s = 0; s < n; ++s) {
                const double t = static_cast<double>(written_frames + s) / kSampleRate;
                const int16_t v = static_cast<int16_t>(
                    6000.0 * std::sin(2.0 * 3.14159265358979 * freq * t));
                for (UINT32 c = 0; c < kChannels; ++c) pcm[s * kChannels + c] = v;
            }
            abuf->Unlock();
            abuf->SetCurrentLength(n * kChannels * 2);
            ComPtr<IMFSample> asample;
            MFCreateSample(&asample);
            asample->AddBuffer(abuf.Get());
            asample->SetSampleTime(written_frames * 10000000LL / kSampleRate);
            asample->SetSampleDuration(static_cast<LONGLONG>(n) * 10000000LL / kSampleRate);
            if (FAILED(writer->WriteSample(audio_stream, asample.Get()))) break;
            written_frames += n;
        }
    }

    hr = writer->Finalize();
    if (FAILED(hr)) {
        if (error) *error = "Finalize failed";
        return false;
    }
    return true;
}

}  // namespace meguri::io
