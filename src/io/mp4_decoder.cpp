#include "mp4_decoder.h"

#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace meguri::io {

namespace {
std::once_flag g_mf_once;
std::atomic<bool> g_mf_ok{false};

// MF_SOURCE_READER_* 列挙値は負値なので DWORD 引数へ渡す際は明示キャストしておく
constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kMediaSource = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);
}  // namespace

bool ensure_media_foundation() {
    std::call_once(g_mf_once, [] {
        // MFStartup はプロセス全体で 1 回。COM はスレッドごとに呼び出し側が初期化する。
        g_mf_ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    });
    return g_mf_ok;
}

void shutdown_media_foundation() {
    if (g_mf_ok.exchange(false)) MFShutdown();
}

struct Mp4Decoder::Impl {
    ComPtr<IMFSourceReader> reader;
    core::MediaInfo info;
    std::string error;
    int stride = 0;                        // CPU モードの行バイト数 (負なら上下反転)
    int64_t default_frame_100ns = 333333;  // 30fps 相当のフォールバック
    int max_output_dim = 0;                // 0 = 原寸
    bool allow_hardware = true;            // false なら常にソフトウェアで開く
    bool want_gpu_output = false;          // GPU テクスチャモードの要求
    bool gpu_output = false;               // GPU テクスチャモードで動作中
    bool using_gpu = false;                // GPU 枠のカウント管理用

    bool configure_rgb32(const std::wstring& path, bool use_gpu);
    bool configure_nv12(const std::wstring& path);
    void read_common_metadata(IMFMediaType* current);
    bool read_next_sample(ComPtr<IMFSample>* out_sample, int64_t* pts_ms, int* duration_ms);

    ~Impl() {
        if (using_gpu) release_gpu_slot();
    }
};

Mp4Decoder::Mp4Decoder() : impl_(std::make_unique<Impl>()) {}
Mp4Decoder::~Mp4Decoder() = default;

void Mp4Decoder::set_max_output_dimension(int max_dim) {
    impl_->max_output_dim = max_dim > 0 ? max_dim : 0;
}

void Mp4Decoder::set_allow_hardware(bool allow) { impl_->allow_hardware = allow; }

void Mp4Decoder::set_gpu_output(bool enabled) { impl_->want_gpu_output = enabled; }

bool Mp4Decoder::gpu_frames_active() const { return impl_->gpu_output; }

// 総再生時間・フレームレートなど共通メタデータを埋める
void Mp4Decoder::Impl::read_common_metadata(IMFMediaType* current) {
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(reader->GetPresentationAttribute(kMediaSource, MF_PD_DURATION, &var)) &&
        var.vt == VT_UI8) {
        info.duration_sec = static_cast<double>(var.uhVal.QuadPart) / 10000000.0;
    }
    PropVariantClear(&var);

    UINT32 fps_num = 0, fps_den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(current, MF_MT_FRAME_RATE, &fps_num, &fps_den)) &&
        fps_num > 0 && fps_den > 0) {
        const double fps = static_cast<double>(fps_num) / fps_den;
        info.frame_count = static_cast<int>(info.duration_sec * fps + 0.5);
        default_frame_100ns = static_cast<int64_t>(10000000.0 / fps);
    }
}

// CPU (RGB32) モード。use_gpu 時はリーダー内の Video Processor で変換・縮小する
bool Mp4Decoder::Impl::configure_rgb32(const std::wstring& path, bool use_gpu) {
    reader.Reset();
    info = core::MediaInfo{};

    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 3))) {
        error = "MFCreateAttributes failed";
        return false;
    }
    // YUV -> RGB32 変換 (と縮小) をリーダー内の Video Processor に任せる
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (use_gpu) {
        if (IMFDXGIDeviceManager* manager = dxgi_device_manager()) {
            attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager);
        }
    }

    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attrs.Get(), &reader);
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MFCreateSourceReaderFromURL failed hr=0x%08X",
                 static_cast<unsigned>(hr));
        error = buf;
        return false;
    }

    // 映像ストリームのみ選択 (音声はデコードしない = 一覧再生は常時ミュート)
    reader->SetStreamSelection(kAllStreams, FALSE);
    reader->SetStreamSelection(kVideoStream, TRUE);

    // 原寸を調べ、タイル表示に必要なサイズまで縮小した出力を要求する
    UINT32 native_w = 0, native_h = 0;
    {
        ComPtr<IMFMediaType> native;
        if (SUCCEEDED(reader->GetNativeMediaType(kVideoStream, 0, &native))) {
            MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &native_w, &native_h);
        }
    }
    UINT32 out_w = 0, out_h = 0;  // 0 = サイズ指定なし (原寸)
    if (max_output_dim > 0 && native_w > 0 && native_h > 0) {
        const UINT32 longer = native_w > native_h ? native_w : native_h;
        if (longer > static_cast<UINT32>(max_output_dim)) {
            const double scale = static_cast<double>(max_output_dim) / longer;
            out_w = (static_cast<UINT32>(native_w * scale) + 1) & ~1u;  // 偶数へ
            out_h = (static_cast<UINT32>(native_h * scale) + 1) & ~1u;
            if (out_w < 2) out_w = 2;
            if (out_h < 2) out_h = 2;
        }
    }

    auto set_output_type = [&](UINT32 w, UINT32 h) -> HRESULT {
        ComPtr<IMFMediaType> out_type;
        HRESULT r = MFCreateMediaType(&out_type);
        if (FAILED(r)) return r;
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);  // メモリ上は B,G,R,X
        if (w > 0 && h > 0) MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, w, h);
        return reader->SetCurrentMediaType(kVideoStream, nullptr, out_type.Get());
    };
    hr = set_output_type(out_w, out_h);
    if (FAILED(hr) && out_w > 0) {
        // 縮小指定を受け付けない環境では原寸でリトライ
        hr = set_output_type(0, 0);
    }
    if (FAILED(hr)) {
        error = "SetCurrentMediaType(RGB32) failed";
        return false;
    }

    ComPtr<IMFMediaType> current;
    hr = reader->GetCurrentMediaType(kVideoStream, &current);
    if (FAILED(hr)) {
        error = "GetCurrentMediaType failed";
        return false;
    }
    UINT32 w = 0, h = 0;
    if (FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 ||
        h == 0) {
        error = "no frame size";
        return false;
    }
    info.width = static_cast<int>(w);
    info.height = static_cast<int>(h);
    info.has_alpha = false;  // RGB32 の X は不定なので不透明として扱う
    info.loops = true;

    UINT32 stride_u = 0;
    if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride_u))) {
        stride = static_cast<int>(static_cast<INT32>(stride_u));
    } else {
        LONG computed = 0;
        if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &computed))) {
            stride = static_cast<int>(computed);
        } else {
            stride = static_cast<int>(w) * 4;
        }
    }

    read_common_metadata(current.Get());
    return true;
}

// GPU テクスチャ (NV12) モード。ストリームごとの Video Processor を持たず、
// デコーダのネイティブ出力をそのまま受け取る
bool Mp4Decoder::Impl::configure_nv12(const std::wstring& path) {
    reader.Reset();
    info = core::MediaInfo{};

    IMFDXGIDeviceManager* manager = dxgi_device_manager();
    if (!manager) {
        error = "no d3d device";
        return false;
    }
    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 2))) {
        error = "MFCreateAttributes failed";
        return false;
    }
    attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager);

    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attrs.Get(), &reader);
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MFCreateSourceReaderFromURL(NV12) failed hr=0x%08X",
                 static_cast<unsigned>(hr));
        error = buf;
        return false;
    }

    reader->SetStreamSelection(kAllStreams, FALSE);
    reader->SetStreamSelection(kVideoStream, TRUE);

    ComPtr<IMFMediaType> out_type;
    if (FAILED(MFCreateMediaType(&out_type))) {
        error = "MFCreateMediaType failed";
        return false;
    }
    out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    hr = reader->SetCurrentMediaType(kVideoStream, nullptr, out_type.Get());
    if (FAILED(hr)) {
        error = "SetCurrentMediaType(NV12) failed";
        return false;
    }

    ComPtr<IMFMediaType> current;
    if (FAILED(reader->GetCurrentMediaType(kVideoStream, &current))) {
        error = "GetCurrentMediaType failed";
        return false;
    }
    UINT32 w = 0, h = 0;
    if (FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 ||
        h == 0) {
        error = "no frame size";
        return false;
    }
    info.width = static_cast<int>(w);
    info.height = static_cast<int>(h);
    info.has_alpha = false;
    info.loops = true;
    info.hardware = true;

    read_common_metadata(current.Get());
    return true;
}

bool Mp4Decoder::open(const std::wstring& path) {
    if (!ensure_media_foundation()) {
        impl_->error = "Media Foundation unavailable";
        return false;
    }

    // GPU テクスチャモード: NV12 のまま受け取り、共有 VP で変換する (ゼロコピー)
    if (impl_->want_gpu_output && impl_->allow_hardware && try_acquire_gpu_slot()) {
        if (impl_->configure_nv12(path)) {
            impl_->using_gpu = true;
            impl_->gpu_output = true;
            return true;
        }
        release_gpu_slot();
    }
    impl_->gpu_output = false;

    // CPU (RGB32) モード。まず GPU、ダメならソフトウェア
    if (impl_->allow_hardware && !impl_->want_gpu_output) {
        if (try_acquire_gpu_slot()) {
            if (impl_->configure_rgb32(path, true)) {
                impl_->using_gpu = true;
                impl_->info.hardware = true;
                return true;
            }
            release_gpu_slot();
        }
    }
    return impl_->configure_rgb32(path, false);
}

const core::MediaInfo& Mp4Decoder::info() const { return impl_->info; }

// ReadSample の共通部分 (リトライ・終端判定)
bool Mp4Decoder::Impl::read_next_sample(ComPtr<IMFSample>* out_sample, int64_t* pts_ms,
                                        int* duration_ms) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        DWORD flags = 0;
        LONGLONG pts_100ns = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr =
            reader->ReadSample(kVideoStream, 0, nullptr, &flags, &pts_100ns, &sample);
        if (FAILED(hr)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "ReadSample failed hr=0x%08X",
                     static_cast<unsigned>(hr));
            error = buf;
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return false;
        if (!sample) continue;  // ギャップ等。次のサンプルへ

        *pts_ms = pts_100ns / 10000;
        LONGLONG dur_100ns = 0;
        if (FAILED(sample->GetSampleDuration(&dur_100ns)) || dur_100ns <= 0) {
            dur_100ns = default_frame_100ns;
        }
        *duration_ms = static_cast<int>(dur_100ns / 10000);
        if (*duration_ms <= 0) *duration_ms = 1;
        *out_sample = std::move(sample);
        return true;
    }
    error = "no video sample";
    return false;
}

bool Mp4Decoder::next_frame(core::VideoFrame& out) {
    Impl& d = *impl_;
    if (!d.reader || d.gpu_output) return false;

    ComPtr<IMFSample> sample;
    int64_t pts_ms = 0;
    int duration_ms = 0;
    if (!d.read_next_sample(&sample, &pts_ms, &duration_ms)) return false;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
        d.error = "ConvertToContiguousBuffer failed";
        return false;
    }
    BYTE* src = nullptr;
    DWORD max_len = 0, cur_len = 0;
    if (FAILED(buffer->Lock(&src, &max_len, &cur_len))) {
        d.error = "buffer Lock failed";
        return false;
    }

    const int w = d.info.width;
    const int h = d.info.height;
    out.width = w;
    out.height = h;
    out.bgra.resize(static_cast<size_t>(w) * h * 4);

    // 実測 (E2E: 自前生成 MP4 + 実動画のフレームダンプ目視) では、
    // ADVANCED_VIDEO_PROCESSING 経由の RGB32 出力は MF_MT_DEFAULT_STRIDE が
    // 負 (ボトムアップ報告) でも実データはトップダウンで届く。
    // そのため stride の符号では反転せず、常にトップダウンとして扱う。
    const int abs_stride = d.stride >= 0 ? d.stride : -d.stride;
    const size_t row_bytes = static_cast<size_t>(w) * 4;
    if (static_cast<size_t>(abs_stride) * h <= cur_len &&
        static_cast<size_t>(abs_stride) >= row_bytes) {
        for (int y = 0; y < h; ++y) {
            const BYTE* src_line = src + static_cast<size_t>(y) * abs_stride;
            std::memcpy(out.bgra.data() + static_cast<size_t>(y) * row_bytes, src_line,
                        row_bytes);
        }
    }
    buffer->Unlock();

    out.pts_ms = pts_ms;
    out.duration_ms = duration_ms;
    return true;
}

bool Mp4Decoder::next_frame_to_texture(ID3D11Texture2D* target, GpuFrameInfo* out) {
    Impl& d = *impl_;
    if (!d.reader || !d.gpu_output || !target) return false;

    ComPtr<IMFSample> sample;
    int64_t pts_ms = 0;
    int duration_ms = 0;
    if (!d.read_next_sample(&sample, &pts_ms, &duration_ms)) return false;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) {
        d.error = "GetBufferByIndex failed";
        return false;
    }
    ComPtr<IMFDXGIBuffer> dxgi_buffer;
    if (FAILED(buffer.As(&dxgi_buffer))) {
        d.error = "not a dxgi buffer";  // D3D マネージャ無しで開いてしまった等
        return false;
    }
    ComPtr<ID3D11Texture2D> texture;
    UINT subresource = 0;
    if (FAILED(dxgi_buffer->GetResource(IID_PPV_ARGS(&texture))) ||
        FAILED(dxgi_buffer->GetSubresourceIndex(&subresource))) {
        d.error = "GetResource failed";
        return false;
    }

    // 表示サイズを渡す (デコードテクスチャはアライメントでパディングされている)
    if (!convert_nv12_to_bgra(texture.Get(), subresource, d.info.width, d.info.height,
                              target)) {
        d.error = "nv12 convert failed";
        return false;
    }
    if (out) {
        out->pts_ms = pts_ms;
        out->duration_ms = duration_ms;
    }
    return true;
}

void Mp4Decoder::rewind() { seek_ms(0); }

void Mp4Decoder::seek_ms(int64_t position_ms) {
    Impl& d = *impl_;
    if (!d.reader) return;
    if (position_ms < 0) position_ms = 0;
    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = position_ms * 10000;  // ms -> 100ns
    d.reader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);
}

const std::string& Mp4Decoder::error_message() const { return impl_->error; }

}  // namespace meguri::io
