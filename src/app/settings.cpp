#include "settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace meguri::app {

namespace {

constexpr const wchar_t* kStorageFiles[] = {L"settings.json", L"probe_cache.json"};

std::wstring exe_directory() {
    wchar_t buf[MAX_PATH * 2] = L"";
    if (GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf)) == 0) return L"";
    return fs::path(buf).parent_path().wstring();
}

std::wstring appdata_directory() {
    wchar_t* appdata = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&appdata, &len, L"APPDATA") != 0 || !appdata) return L"";
    std::wstring dir = std::wstring(appdata) + L"\\Meguri";
    free(appdata);
    return dir;
}

std::wstring storage_dir(StorageLocation location) {
    return location == StorageLocation::Portable ? exe_directory() : appdata_directory();
}

StorageLocation g_storage = StorageLocation::Portable;
bool g_storage_detected = false;

// EXE 側優先で保存先を検出する
void detect_storage_location() {
    if (g_storage_detected) return;
    g_storage_detected = true;
    std::error_code ec;
    const std::wstring exe_dir = exe_directory();
    if (!exe_dir.empty() && fs::exists(fs::path(exe_dir) / L"settings.json", ec)) {
        g_storage = StorageLocation::Portable;
        return;
    }
    const std::wstring appdata = appdata_directory();
    if (!appdata.empty() && fs::exists(fs::path(appdata) / L"settings.json", ec)) {
        g_storage = StorageLocation::AppData;
        return;
    }
    g_storage = StorageLocation::Portable;  // どちらも無ければ既定 (ポータブル)
}

}  // namespace

StorageLocation active_storage_location() {
    detect_storage_location();
    return g_storage;
}

std::wstring storage_file_path(const wchar_t* filename) {
    detect_storage_location();
    const std::wstring dir = storage_dir(g_storage);
    if (dir.empty()) return L"";
    return (fs::path(dir) / filename).wstring();
}

bool set_storage_location(StorageLocation target) {
    detect_storage_location();
    if (target == g_storage) return true;

    const std::wstring src_dir = storage_dir(g_storage);
    const std::wstring dst_dir = storage_dir(target);
    if (dst_dir.empty()) return false;

    std::error_code ec;
    fs::create_directories(dst_dir, ec);
    // 書き込み可否の確認 (ポータブル先が読み取り専用の場合など)
    {
        const fs::path probe = fs::path(dst_dir) / L".meguri_write_test";
        std::ofstream test(probe, std::ios::binary | std::ios::trunc);
        if (!test) return false;
        test.close();
        fs::remove(probe, ec);
    }

    // 設定とキャッシュを即時移動する (別ボリューム等で rename 不可ならコピー + 削除)
    for (const wchar_t* name : kStorageFiles) {
        const fs::path src = fs::path(src_dir) / name;
        const fs::path dst = fs::path(dst_dir) / name;
        if (!fs::exists(src, ec)) continue;
        fs::rename(src, dst, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            if (!ec) fs::remove(src, ec);
        }
    }
    g_storage = target;
    return true;
}

std::string narrow(const std::wstring& text) {
    std::string out;
    out.reserve(text.size() * 3);
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t cp = text[i];
        // サロゲートペア結合
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size()) {
            const uint32_t lo = text[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::wstring widen(const std::string& text) {
    std::wstring out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            ++i;
            continue;  // 不正バイトはスキップ
        }
        if (i + extra >= text.size()) break;
        bool valid = true;
        for (int k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(text[i + k]);
            if ((cc & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += extra + 1;
        if (!valid) continue;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
    }
    return out;
}

namespace {

const char* sort_key_name(core::SortKey key) {
    switch (key) {
        case core::SortKey::ModifiedTime: return "modified";
        case core::SortKey::FileSize: return "size";
        case core::SortKey::Name: break;
    }
    return "name";
}

core::SortKey sort_key_from_name(const std::string& name) {
    if (name == "modified") return core::SortKey::ModifiedTime;
    if (name == "size") return core::SortKey::FileSize;
    return core::SortKey::Name;
}

const char* mode_name(core::PerformanceMode mode) {
    switch (mode) {
        case core::PerformanceMode::Massive: return "massive";
        case core::PerformanceMode::PlayAll: return "play_all";
        case core::PerformanceMode::Standard: break;
    }
    return "standard";
}

core::PerformanceMode mode_from_name(const std::string& name) {
    if (name == "massive") return core::PerformanceMode::Massive;
    if (name == "play_all") return core::PerformanceMode::PlayAll;
    return core::PerformanceMode::Standard;
}

}  // namespace

std::wstring settings_file_path() { return storage_file_path(L"settings.json"); }

Settings load_settings() {
    Settings s;
    const std::wstring path = settings_file_path();
    if (path.empty()) return s;
    std::ifstream file(fs::path(path), std::ios::binary);
    if (!file) return s;

    json j = json::parse(file, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return s;

    // キー欠落は既定値のまま (前方互換)
    s.language = j.value("language", s.language);
    s.theme = j.value("theme", s.theme);
    s.target_row_height = j.value("target_row_height", s.target_row_height);
    s.last_folder = widen(j.value("last_folder", std::string()));
    s.recursive = j.value("recursive", s.recursive);
    s.show_webp = j.value("show_webp", s.show_webp);
    s.show_mp4 = j.value("show_mp4", s.show_mp4);
    s.show_wmv = j.value("show_wmv", s.show_wmv);
    s.show_avi = j.value("show_avi", s.show_avi);
    s.show_png = j.value("show_png", s.show_png);
    s.show_jpeg = j.value("show_jpeg", s.show_jpeg);
    s.sort_key = sort_key_from_name(j.value("sort_key", std::string("name")));
    s.sort_descending = j.value("sort_descending", s.sort_descending);
    s.performance_mode = mode_from_name(j.value("performance_mode", std::string("standard")));
    s.gpu_memory_percent = j.value("gpu_memory_percent", s.gpu_memory_percent);
    s.confirm_delete = j.value("confirm_delete", s.confirm_delete);
    s.debug_overlay = j.value("debug_overlay", s.debug_overlay);
    s.show_seekbar = j.value("show_seekbar", s.show_seekbar);
    s.show_filenames = j.value("show_filenames", s.show_filenames);
    s.audio_volume = j.value("audio_volume", s.audio_volume);
    s.audio_muted = j.value("audio_muted", s.audio_muted);
    s.grid_audio = j.value("grid_audio", s.grid_audio);
    s.intro_offset = j.value("intro_offset", s.intro_offset);
    s.window_x = j.value("window_x", s.window_x);
    s.window_y = j.value("window_y", s.window_y);
    s.window_width = j.value("window_width", s.window_width);
    s.window_height = j.value("window_height", s.window_height);
    s.window_maximized = j.value("window_maximized", s.window_maximized);
    return s;
}

bool save_settings(const Settings& s) {
    std::wstring path = settings_file_path();
    if (path.empty()) return false;

    json j;
    j["language"] = s.language;
    j["theme"] = s.theme;
    j["target_row_height"] = s.target_row_height;
    j["last_folder"] = narrow(s.last_folder);
    j["recursive"] = s.recursive;
    j["show_webp"] = s.show_webp;
    j["show_mp4"] = s.show_mp4;
    j["show_wmv"] = s.show_wmv;
    j["show_avi"] = s.show_avi;
    j["show_png"] = s.show_png;
    j["show_jpeg"] = s.show_jpeg;
    j["sort_key"] = sort_key_name(s.sort_key);
    j["sort_descending"] = s.sort_descending;
    j["performance_mode"] = mode_name(s.performance_mode);
    j["gpu_memory_percent"] = s.gpu_memory_percent;
    j["confirm_delete"] = s.confirm_delete;
    j["debug_overlay"] = s.debug_overlay;
    j["show_seekbar"] = s.show_seekbar;
    j["show_filenames"] = s.show_filenames;
    j["audio_volume"] = s.audio_volume;
    j["audio_muted"] = s.audio_muted;
    j["grid_audio"] = s.grid_audio;
    j["intro_offset"] = s.intro_offset;
    j["window_x"] = s.window_x;
    j["window_y"] = s.window_y;
    j["window_width"] = s.window_width;
    j["window_height"] = s.window_height;
    j["window_maximized"] = s.window_maximized;

    auto write_to = [&](const std::wstring& target) -> bool {
        std::error_code ec;
        fs::create_directories(fs::path(target).parent_path(), ec);
        std::ofstream file(fs::path(target), std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file << j.dump(2);
        return static_cast<bool>(file);
    };
    if (write_to(path)) return true;

    // ポータブル保存先 (EXE フォルダ) に書き込めない場合は AppData へ自動退避
    if (active_storage_location() == StorageLocation::Portable) {
        g_storage = StorageLocation::AppData;
        path = settings_file_path();
        if (!path.empty() && write_to(path)) return true;
    }
    return false;
}

}  // namespace meguri::app
