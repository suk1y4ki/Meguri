// Meguri CLI: コアと I/O の検証・ベンチ・サンプル生成のフロントエンド。
// GUI より先にここで E2E (生成 → 走査 → デコード → 出力確認) を回す。
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "app/settings.h"  // narrow()
#include "io/probe.h"
#include "io/sample_gen.h"
#include "io/scanner.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace fs = std::filesystem;
using namespace meguri;

namespace {

void print_usage() {
    std::wprintf(
        L"Meguri CLI\n"
        L"usage:\n"
        L"  meguri_cli scan <folder> [--no-recursive]      フォルダを走査して一覧表示\n"
        L"  meguri_cli info <file|folder>                  メタデータをプローブして表示\n"
        L"  meguri_cli decode <file> [--out <dir>] [--max <n>]  フレームを PNG で出力\n"
        L"  meguri_cli bench <folder> [--threads <n>] [--limit <px>]  全ファイルをフルデコードして計測\n"
        L"                                                 (--limit はデコード長辺の上限。0=原寸)\n"
        L"  meguri_cli gensample <folder> [--webp <n>] [--mp4 <n>] [--large]  サンプル生成\n"
        L"                                                 (--large は 720p/1080p の MP4 を生成)\n");
}

const wchar_t* type_name(core::MediaType type) {
    switch (type) {
        case core::MediaType::Mp4: return L"MP4";
        case core::MediaType::Wmv: return L"WMV";
        case core::MediaType::Avi: return L"AVI";
        case core::MediaType::Png: return L"PNG";
        case core::MediaType::Jpeg: return L"JPG";
        case core::MediaType::Webp: break;
    }
    return L"WEBP";
}

// ---- scan ----

int cmd_scan(const std::vector<std::wstring>& args) {
    if (args.empty()) {
        print_usage();
        return 1;
    }
    io::ScanOptions options;
    for (const auto& a : args) {
        if (a == L"--no-recursive") options.recursive = false;
    }
    const auto items = io::scan_folder(args[0], options);
    for (const auto& item : items) {
        std::wprintf(L"%-4s %10llu  %s\n", type_name(item.type),
                     static_cast<unsigned long long>(item.file_size), item.path.c_str());
    }
    std::wprintf(L"%zu file(s)\n", items.size());
    return 0;
}

// ---- info ----

int cmd_info(const std::vector<std::wstring>& args) {
    if (args.empty()) {
        print_usage();
        return 1;
    }
    std::vector<core::MediaItem> items;
    if (fs::is_directory(args[0])) {
        items = io::scan_folder(args[0], {});
    } else {
        core::MediaItem item;
        item.path = args[0];
        io::classify_media_path(args[0], &item.type);  // 非対応拡張子は Webp のままプローブ失敗
        items.push_back(item);
    }
    int failed = 0;
    for (auto& item : items) {
        if (io::probe_media_item(item)) {
            std::wprintf(L"%-4s %5dx%-5d %6.2fs %4d frames  %s\n", type_name(item.type),
                         item.width, item.height, item.duration_sec, item.frame_count,
                         item.path.c_str());
        } else {
            ++failed;
            std::wprintf(L"FAIL %s\n", item.path.c_str());
        }
    }
    std::wprintf(L"%zu file(s), %d failed\n", items.size(), failed);
    return failed == 0 ? 0 : 1;
}

// ---- decode ----

int cmd_decode(const std::vector<std::wstring>& args) {
    if (args.empty()) {
        print_usage();
        return 1;
    }
    const std::wstring& path = args[0];
    std::wstring out_dir = L".";
    int max_frames = 8;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == L"--out" && i + 1 < args.size()) out_dir = args[++i];
        if (args[i] == L"--max" && i + 1 < args.size()) max_frames = _wtoi(args[++i].c_str());
    }

    core::MediaItem item;
    item.path = path;
    io::classify_media_path(path, &item.type);
    auto decoder = io::create_decoder(item.type);
    if (!decoder->open(path)) {
        std::printf("open failed: %s\n", decoder->error_message().c_str());
        return 1;
    }
    const core::MediaInfo& info = decoder->info();
    std::wprintf(L"%dx%d %.2fs %d frames\n", info.width, info.height, info.duration_sec,
                 info.frame_count);

    std::error_code ec;
    fs::create_directories(out_dir, ec);
    const std::string stem = app::narrow(fs::path(path).stem().wstring());

    core::VideoFrame frame;
    int index = 0;
    while (index < max_frames && decoder->next_frame(frame)) {
        // MP4 (RGB32) はアルファ不定なので不透明にしてから出力する
        if (!info.has_alpha) {
            for (size_t i = 3; i < frame.bgra.size(); i += 4) frame.bgra[i] = 255;
        }
        // stb は RGBA なので BGRA から入れ替え
        std::vector<uint8_t> rgba(frame.bgra.size());
        for (size_t i = 0; i + 3 < frame.bgra.size(); i += 4) {
            rgba[i + 0] = frame.bgra[i + 2];
            rgba[i + 1] = frame.bgra[i + 1];
            rgba[i + 2] = frame.bgra[i + 0];
            rgba[i + 3] = frame.bgra[i + 3];
        }
        char name[512];
        std::snprintf(name, sizeof(name), "%s\\%s_f%03d_t%lld.png",
                      app::narrow(out_dir).c_str(), stem.c_str(), index,
                      static_cast<long long>(frame.pts_ms));
        if (!stbi_write_png(name, frame.width, frame.height, 4, rgba.data(),
                            frame.width * 4)) {
            std::printf("png write failed: %s\n", name);
            return 1;
        }
        std::printf("wrote %s (dur=%dms)\n", name, frame.duration_ms);
        ++index;
    }
    std::printf("%d frame(s) dumped\n", index);
    return index > 0 ? 0 : 1;
}

// ---- bench ----

int cmd_bench(const std::vector<std::wstring>& args) {
    if (args.empty()) {
        print_usage();
        return 1;
    }
    int threads = static_cast<int>(std::thread::hardware_concurrency());
    int limit = 0;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == L"--threads" && i + 1 < args.size()) threads = _wtoi(args[++i].c_str());
        if (args[i] == L"--limit" && i + 1 < args.size()) limit = _wtoi(args[++i].c_str());
    }
    if (threads < 1) threads = 1;

    const auto items = io::scan_folder(args[0], {});
    if (items.empty()) {
        std::wprintf(L"no media files\n");
        return 1;
    }

    std::atomic<size_t> next_index{0};
    std::atomic<int64_t> total_frames{0};
    std::atomic<int> failures{0};
    const auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            // MF はスレッドごとに COM 初期化が必要
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            for (;;) {
                const size_t i = next_index.fetch_add(1);
                if (i >= items.size()) break;
                auto decoder = io::create_decoder(items[i].type);
                decoder->set_max_output_dimension(limit);
                if (!decoder->open(items[i].path)) {
                    ++failures;
                    continue;
                }
                core::VideoFrame frame;
                int64_t frames = 0;
                while (decoder->next_frame(frame)) ++frames;
                total_frames += frames;
            }
            CoUninitialize();
        });
    }
    for (auto& th : pool) th.join();

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double sec =
        std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
    std::wprintf(L"files: %zu  threads: %d  frames: %lld  failures: %d\n", items.size(),
                 threads, static_cast<long long>(total_frames.load()), failures.load());
    std::wprintf(L"time: %.3fs  (%.1f frames/s)\n", sec,
                 sec > 0 ? total_frames.load() / sec : 0.0);
    return failures == 0 ? 0 : 1;
}

// ---- gensample ----

int cmd_gensample(const std::vector<std::wstring>& args) {
    if (args.empty()) {
        print_usage();
        return 1;
    }
    int webp_count = 12;
    int mp4_count = 12;
    int long_count = 0;
    bool large = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == L"--webp" && i + 1 < args.size()) webp_count = _wtoi(args[++i].c_str());
        if (args[i] == L"--mp4" && i + 1 < args.size()) mp4_count = _wtoi(args[++i].c_str());
        if (args[i] == L"--long" && i + 1 < args.size()) long_count = _wtoi(args[++i].c_str());
        if (args[i] == L"--large") large = true;
    }
    std::error_code ec;
    fs::create_directories(args[0], ec);

    // サイズ・比率のバリエーション (敷き詰めレイアウトの検証用)。
    // --large は実際の動画に近い解像度 (GPU デコード / 縮小デコードの計測用)
    const int small_sizes[][2] = {{320, 240}, {240, 320}, {480, 270}, {200, 200},
                                  {640, 360}, {160, 280}, {400, 300}, {512, 288}};
    const int large_sizes[][2] = {{1920, 1080}, {1280, 720}, {1080, 1920}, {1920, 1080},
                                  {1280, 720},  {720, 1280}, {1920, 1080}, {1280, 720}};
    const auto& sizes = large ? large_sizes : small_sizes;
    const int size_count = 8;

    int generated = 0;
    for (int i = 0; i < webp_count; ++i) {
        io::SampleSpec spec;
        spec.width = sizes[i % size_count][0];
        spec.height = sizes[i % size_count][1];
        spec.frame_count = 24 + (i % 3) * 12;
        spec.frame_ms = 50 + (i % 4) * 25;
        spec.seed = i;
        wchar_t name[64];
        swprintf(name, 64, L"\\sample_%02d.webp", i);
        std::string error;
        if (io::generate_sample_webp(args[0] + name, spec, &error)) {
            ++generated;
        } else {
            std::printf("webp gen failed: %s\n", error.c_str());
        }
    }
    for (int i = 0; i < mp4_count; ++i) {
        io::SampleSpec spec;
        spec.width = sizes[(i + 3) % size_count][0];
        spec.height = sizes[(i + 3) % size_count][1];
        spec.frame_count = 60 + (i % 3) * 30;
        spec.frame_ms = 33;
        spec.seed = i + 100;
        wchar_t name[64];
        swprintf(name, 64, L"\\sample_%02d.mp4", i);
        std::string error;
        if (io::generate_sample_mp4(args[0] + name, spec, &error)) {
            ++generated;
        } else {
            std::printf("mp4 gen failed: %s\n", error.c_str());
        }
    }
    // 長尺 MP4 (約 5.5 分 @30fps)。イントロオフセットの検証用
    for (int i = 0; i < long_count; ++i) {
        io::SampleSpec spec;
        spec.width = 320;
        spec.height = 180;
        spec.frame_count = 9900;
        spec.frame_ms = 33;
        spec.seed = i + 300;
        wchar_t name[64];
        swprintf(name, 64, L"\\sample_long_%02d.mp4", i);
        std::string error;
        if (io::generate_sample_mp4(args[0] + name, spec, &error)) {
            ++generated;
        } else {
            std::printf("long mp4 gen failed: %s\n", error.c_str());
        }
    }

    const int expected = webp_count + mp4_count + long_count;
    std::wprintf(L"generated %d/%d file(s) in %s\n", generated, expected, args[0].c_str());
    return generated == expected ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const std::wstring command = argv[1];
    std::vector<std::wstring> args(argv + 2, argv + argc);

    int result = 1;
    if (command == L"scan") {
        result = cmd_scan(args);
    } else if (command == L"info") {
        result = cmd_info(args);
    } else if (command == L"decode") {
        result = cmd_decode(args);
    } else if (command == L"bench") {
        result = cmd_bench(args);
    } else if (command == L"gensample") {
        result = cmd_gensample(args);
    } else {
        print_usage();
    }

    CoUninitialize();
    return result;
}
