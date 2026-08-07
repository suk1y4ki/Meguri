#include "image_decoder.h"

#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <vector>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace meguri::io {

namespace {
// 静止画の「表示時間」。実質再デコードしない長さにする (ループ時も 1 時間ごと)
constexpr int kStillFrameMs = 3600 * 1000;
}  // namespace

struct ImageDecoder::Impl {
    core::MediaInfo info;
    std::string error;
    std::vector<uint8_t> bgra;  // straight alpha (プリマルチはエンジン側)
    int max_output_dim = 0;
    bool delivered = false;
};

ImageDecoder::ImageDecoder() : impl_(std::make_unique<Impl>()) {}
ImageDecoder::~ImageDecoder() = default;

void ImageDecoder::set_max_output_dimension(int max_dim) {
    impl_->max_output_dim = max_dim > 0 ? max_dim : 0;
}

bool ImageDecoder::open(const std::wstring& path) {
    Impl& d = *impl_;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        d.error = "WIC factory failed";
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand, &decoder))) {
        d.error = "not a decodable image";
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        d.error = "GetFrame failed";
        return false;
    }
    UINT native_w = 0, native_h = 0;
    if (FAILED(frame->GetSize(&native_w, &native_h)) || native_w == 0 || native_h == 0) {
        d.error = "no size";
        return false;
    }

    // タイル表示に必要なサイズまで縮小 (ズーム時は原寸で開き直される)
    UINT out_w = native_w, out_h = native_h;
    const UINT longer = native_w > native_h ? native_w : native_h;
    if (d.max_output_dim > 0 && longer > static_cast<UINT>(d.max_output_dim)) {
        const double scale = static_cast<double>(d.max_output_dim) / longer;
        out_w = static_cast<UINT>(native_w * scale);
        out_h = static_cast<UINT>(native_h * scale);
        if (out_w < 1) out_w = 1;
        if (out_h < 1) out_h = 1;
    }

    ComPtr<IWICBitmapSource> source = frame;
    if (out_w != native_w || out_h != native_h) {
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame.Get(), out_w, out_h,
                                         WICBitmapInterpolationModeFant))) {
            source = scaler;
        } else {
            out_w = native_w;
            out_h = native_h;
        }
    }

    ComPtr<IWICBitmapSource> converted;
    if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, source.Get(),
                                      &converted))) {
        d.error = "convert failed";
        return false;
    }

    d.bgra.resize(static_cast<size_t>(out_w) * out_h * 4);
    if (FAILED(converted->CopyPixels(nullptr, out_w * 4,
                                     static_cast<UINT>(d.bgra.size()), d.bgra.data()))) {
        d.error = "CopyPixels failed";
        d.bgra.clear();
        return false;
    }

    d.info.width = static_cast<int>(out_w);
    d.info.height = static_cast<int>(out_h);
    d.info.frame_count = 1;
    d.info.duration_sec = 0.0;
    d.info.has_alpha = true;  // PNG の透過に対応 (JPEG は全ピクセル a=255 で実質無視)
    d.info.loops = false;
    d.delivered = false;
    return true;
}

const core::MediaInfo& ImageDecoder::info() const { return impl_->info; }

bool ImageDecoder::next_frame(core::VideoFrame& out) {
    Impl& d = *impl_;
    if (d.delivered || d.bgra.empty()) return false;
    out.width = d.info.width;
    out.height = d.info.height;
    out.bgra = d.bgra;  // ループ用に元データは保持する
    out.pts_ms = 0;
    out.duration_ms = kStillFrameMs;
    d.delivered = true;
    return true;
}

void ImageDecoder::rewind() { impl_->delivered = false; }

const std::string& ImageDecoder::error_message() const { return impl_->error; }

}  // namespace meguri::io
