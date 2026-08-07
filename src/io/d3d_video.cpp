#include "d3d_video.h"

#include <d3d11.h>
#include <dxgi1_4.h>
#include <mfapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "mfplat.lib")

using Microsoft::WRL::ComPtr;

namespace meguri::io {

namespace {

// ---- 共有デバイス ----
std::once_flag g_device_once;
ComPtr<ID3D11Device> g_device;
ComPtr<IMFDXGIDeviceManager> g_manager;
ComPtr<IDXGIAdapter3> g_adapter3;

// ---- GPU デコーダ枠 ----
std::atomic<int> g_max_gpu_readers{32};       // 静的上限 (VRAM 予算ベース)
std::atomic<int> g_dynamic_gpu_cap{1 << 20};  // 動的上限 (実行時失敗から学習)
std::atomic<int> g_gpu_reader_count{0};
std::atomic<int> g_gpu_memory_percent{50};
std::atomic<uint64_t> g_vram_budget_mb{0};
std::atomic<uint64_t> g_last_gpu_failure_ms{0};
std::atomic<uint64_t> g_last_cap_raise_ms{0};
std::atomic<uint64_t> g_last_cap_drop_ms{0};

// ---- 共有 VideoProcessor (NV12 -> BGRA 変換用、mutex で直列化) ----
std::mutex g_vp_mutex;
ComPtr<ID3D11VideoDevice> g_video_device;
ComPtr<ID3D11VideoContext> g_video_context;
ComPtr<ID3D11VideoProcessorEnumerator> g_vp_enum;
ComPtr<ID3D11VideoProcessor> g_vp;

void recompute_gpu_cap() {
    const uint64_t budget = g_vram_budget_mb.load();
    if (budget == 0) return;
    int cap = static_cast<int>(budget * g_gpu_memory_percent.load() / 100 / 128);
    if (cap < 8) cap = 8;
    if (cap > 256) cap = 256;
    g_max_gpu_readers = cap;
}

int effective_gpu_cap() {
    return g_max_gpu_readers.load() < g_dynamic_gpu_cap.load() ? g_max_gpu_readers.load()
                                                               : g_dynamic_gpu_cap.load();
}

// 失敗が収まって時間が経ったら動的上限を少しずつ戻す (3 秒ごとに +2)
void maybe_raise_dynamic_cap() {
    const uint64_t now = GetTickCount64();
    if (g_dynamic_gpu_cap.load() >= g_max_gpu_readers.load()) return;
    if (now - g_last_gpu_failure_ms.load() < 10000) return;  // 直近 10 秒失敗なし
    uint64_t last_raise = g_last_cap_raise_ms.load();
    if (now - last_raise < 3000) return;
    if (g_last_cap_raise_ms.compare_exchange_strong(last_raise, now)) {
        g_dynamic_gpu_cap += 2;
    }
}

void create_shared_device() {
    // BGRA_SUPPORT: Direct2D 連携 (ゼロコピー描画) に必須
    const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    ComPtr<ID3D11Device> device;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr,
                                   nullptr);
    if (FAILED(hr)) return;
    // 複数スレッド (MF ワーカー + UI) から共有されるため必須
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

    UINT token = 0;
    ComPtr<IMFDXGIDeviceManager> manager;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &manager))) return;
    if (FAILED(manager->ResetDevice(device.Get(), token))) return;

    // VRAM 予算を取得して静的上限を決める
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(device.As(&dxgi_device)) && SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
        uint64_t budget_mb = 0;
        ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(adapter.As(&adapter3))) {
            g_adapter3 = adapter3;
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                                                         &info))) {
                budget_mb = info.Budget / (1024 * 1024);
            }
        }
        if (budget_mb == 0) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                budget_mb = desc.DedicatedVideoMemory / (1024 * 1024);
            }
        }
        g_vram_budget_mb = budget_mb;
        recompute_gpu_cap();
    }

    g_device = device;
    g_manager = manager;
}

void ensure_device() { std::call_once(g_device_once, [] { create_shared_device(); }); }

// g_vp_mutex を保持した状態で呼ぶこと
bool ensure_video_processor(UINT src_w, UINT src_h, UINT dst_w, UINT dst_h) {
    if (!g_device) return false;
    if (!g_video_device) {
        if (FAILED(g_device.As(&g_video_device))) return false;
        ComPtr<ID3D11DeviceContext> context;
        g_device->GetImmediateContext(&context);
        if (FAILED(context.As(&g_video_context))) return false;
    }
    if (!g_vp) {
        // ContentDesc のサイズはヒント扱いで、実際の Blt は任意サイズの
        // 入出力 RECT を受け付ける。プロセスで 1 個だけ作って使い回す
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputWidth = src_w;
        desc.InputHeight = src_h;
        desc.OutputWidth = dst_w;
        desc.OutputHeight = dst_h;
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(g_video_device->CreateVideoProcessorEnumerator(&desc, &g_vp_enum))) {
            return false;
        }
        if (FAILED(g_video_device->CreateVideoProcessor(g_vp_enum.Get(), 0, &g_vp))) {
            g_vp_enum.Reset();
            return false;
        }
    }
    return true;
}

}  // namespace

ID3D11Device* shared_d3d_device() {
    ensure_device();
    return g_device.Get();
}

IMFDXGIDeviceManager* dxgi_device_manager() {
    ensure_device();
    return g_manager.Get();
}

bool try_acquire_gpu_slot() {
    maybe_raise_dynamic_cap();
    if (g_gpu_reader_count.fetch_add(1) < effective_gpu_cap()) return true;
    --g_gpu_reader_count;
    return false;
}

void release_gpu_slot() { --g_gpu_reader_count; }

void note_gpu_runtime_failure() {
    // 失敗はバーストで数十件届くため、降下は 2 秒に 1 回まで
    // (1 件ごとに下げると、失敗したデコーダが閉じて同時数が減っていく中で
    //  上限がスパイラル降下し、全タイルが SW に落ちてしまう)
    const uint64_t now = GetTickCount64();
    g_last_gpu_failure_ms = now;

    uint64_t last_drop = g_last_cap_drop_ms.load();
    if (now - last_drop < 2000) return;
    if (!g_last_cap_drop_ms.compare_exchange_strong(last_drop, now)) return;

    const int used = g_gpu_reader_count.load();
    int target = used - 4;
    if (target < 16) target = 16;
    int current = g_dynamic_gpu_cap.load();
    while (target < current && !g_dynamic_gpu_cap.compare_exchange_weak(current, target)) {
    }
}

void gpu_decoder_stats(int* used, int* cap) {
    if (used) *used = g_gpu_reader_count.load();
    if (cap) *cap = effective_gpu_cap();
}

void set_gpu_memory_percent(int percent) {
    if (percent < 10) percent = 10;
    if (percent > 100) percent = 100;
    g_gpu_memory_percent = percent;
    recompute_gpu_cap();
}

bool gpu_memory_stats(uint64_t* usage_mb, uint64_t* budget_mb) {
    if (budget_mb) *budget_mb = g_vram_budget_mb.load();
    if (usage_mb) *usage_mb = 0;
    if (!g_adapter3) return g_vram_budget_mb.load() > 0;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        return false;
    }
    if (usage_mb) *usage_mb = info.CurrentUsage / (1024 * 1024);
    if (budget_mb) *budget_mb = info.Budget / (1024 * 1024);
    return true;
}

bool convert_nv12_to_bgra(ID3D11Texture2D* src, unsigned subresource, int src_width,
                          int src_height, ID3D11Texture2D* dst) {
    if (!src || !dst) return false;
    D3D11_TEXTURE2D_DESC src_desc{}, dst_desc{};
    src->GetDesc(&src_desc);
    dst->GetDesc(&dst_desc);
    // 表示サイズが指定されていればそれをソース矩形にする (パディング行の除外)
    if (src_width <= 0 || src_width > static_cast<int>(src_desc.Width)) {
        src_width = static_cast<int>(src_desc.Width);
    }
    if (src_height <= 0 || src_height > static_cast<int>(src_desc.Height)) {
        src_height = static_cast<int>(src_desc.Height);
    }

    std::lock_guard<std::mutex> lock(g_vp_mutex);
    if (!ensure_video_processor(src_desc.Width, src_desc.Height, dst_desc.Width,
                                dst_desc.Height)) {
        return false;
    }

    ComPtr<ID3D11VideoProcessorInputView> input_view;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc{};
    in_desc.FourCC = 0;  // テクスチャのフォーマットに従う (NV12)
    in_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    in_desc.Texture2D.MipSlice = 0;
    in_desc.Texture2D.ArraySlice = subresource;
    if (FAILED(g_video_device->CreateVideoProcessorInputView(src, g_vp_enum.Get(), &in_desc,
                                                             &input_view))) {
        return false;
    }

    ComPtr<ID3D11VideoProcessorOutputView> output_view;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_desc{};
    out_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    out_desc.Texture2D.MipSlice = 0;
    if (FAILED(g_video_device->CreateVideoProcessorOutputView(dst, g_vp_enum.Get(), &out_desc,
                                                              &output_view))) {
        return false;
    }

    // 色空間: HD (720 行以上) は BT.709、SD は BT.601。いずれも limited range
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
    in_cs.YCbCr_Matrix = src_height >= 720 ? 1 : 0;  // 1 = BT.709, 0 = BT.601
    in_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    g_video_context->VideoProcessorSetStreamColorSpace(g_vp.Get(), 0, &in_cs);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{};
    out_cs.RGB_Range = 0;  // 0-255
    g_video_context->VideoProcessorSetOutputColorSpace(g_vp.Get(), &out_cs);

    RECT src_rect{0, 0, static_cast<LONG>(src_width), static_cast<LONG>(src_height)};
    RECT dst_rect{0, 0, static_cast<LONG>(dst_desc.Width), static_cast<LONG>(dst_desc.Height)};
    g_video_context->VideoProcessorSetStreamSourceRect(g_vp.Get(), 0, TRUE, &src_rect);
    g_video_context->VideoProcessorSetStreamDestRect(g_vp.Get(), 0, TRUE, &dst_rect);
    g_video_context->VideoProcessorSetOutputTargetRect(g_vp.Get(), TRUE, &dst_rect);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();
    return SUCCEEDED(
        g_video_context->VideoProcessorBlt(g_vp.Get(), output_view.Get(), 0, 1, &stream));
}

bool read_texture_bgra(ID3D11Texture2D* texture, unsigned char* out, size_t out_size,
                       int* width, int* height) {
    if (!texture || !g_device) return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (width) *width = static_cast<int>(desc.Width);
    if (height) *height = static_cast<int>(desc.Height);
    const size_t needed = static_cast<size_t>(desc.Width) * desc.Height * 4;
    if (!out) return true;  // サイズ問い合わせのみ
    if (out_size < needed) return false;

    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g_device->CreateTexture2D(&staging_desc, nullptr, &staging))) return false;

    ComPtr<ID3D11DeviceContext> context;
    g_device->GetImmediateContext(&context);
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const size_t row_bytes = static_cast<size_t>(desc.Width) * 4;
    for (UINT y = 0; y < desc.Height; ++y) {
        memcpy(out + static_cast<size_t>(y) * row_bytes,
               static_cast<const unsigned char*>(mapped.pData) +
                   static_cast<size_t>(y) * mapped.RowPitch,
               row_bytes);
    }
    context->Unmap(staging.Get(), 0);
    return true;
}

}  // namespace meguri::io
