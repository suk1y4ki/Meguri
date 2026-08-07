#include "probe_cache.h"

#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"
#include "settings.h"  // narrow / widen / settings_file_path

namespace fs = std::filesystem;
using nlohmann::json;

namespace meguri::app {

namespace {

std::wstring cache_file_path() { return storage_file_path(L"probe_cache.json"); }

}  // namespace

void ProbeCache::load() {
    entries_.clear();
    const std::wstring path = cache_file_path();
    if (path.empty()) return;
    std::ifstream file(fs::path(path), std::ios::binary);
    if (!file) return;
    json j = json::parse(file, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;

    for (auto& [key, value] : j.items()) {
        // [size, mtime, w, h, duration_sec, frames]
        if (!value.is_array() || value.size() < 6) continue;
        Entry entry;
        entry.file_size = value[0].get<uint64_t>();
        entry.modified_time = value[1].get<int64_t>();
        entry.width = value[2].get<int>();
        entry.height = value[3].get<int>();
        entry.duration_sec = value[4].get<double>();
        entry.frame_count = value[5].get<int>();
        entries_[widen(key)] = entry;
    }
}

bool ProbeCache::save() const {
    const std::wstring path = cache_file_path();
    if (path.empty()) return false;
    json j = json::object();
    for (const auto& [key, e] : entries_) {
        j[narrow(key)] = json::array(
            {e.file_size, e.modified_time, e.width, e.height, e.duration_sec, e.frame_count});
    }
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream file(fs::path(path), std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << j.dump();
    return static_cast<bool>(file);
}

void ProbeCache::apply(std::vector<core::MediaItem>* items) const {
    for (auto& item : *items) {
        const auto it = entries_.find(item.path);
        if (it == entries_.end()) continue;
        const Entry& e = it->second;
        // サイズか更新日時が変わっていたら別ファイルとみなして使わない
        if (e.file_size != item.file_size || e.modified_time != item.modified_time) continue;
        item.width = e.width;
        item.height = e.height;
        item.duration_sec = e.duration_sec;
        item.frame_count = e.frame_count;
    }
}

bool ProbeCache::update(const std::vector<core::MediaItem>& items) {
    bool changed = false;
    for (const auto& item : items) {
        if (item.width <= 0 || item.height <= 0) continue;
        Entry entry;
        entry.file_size = item.file_size;
        entry.modified_time = item.modified_time;
        entry.width = item.width;
        entry.height = item.height;
        entry.duration_sec = item.duration_sec;
        entry.frame_count = item.frame_count;
        auto it = entries_.find(item.path);
        if (it != entries_.end() && it->second.file_size == entry.file_size &&
            it->second.modified_time == entry.modified_time &&
            it->second.width == entry.width && it->second.height == entry.height) {
            continue;  // 変更なし
        }
        entries_[item.path] = entry;
        changed = true;
    }
    return changed;
}

}  // namespace meguri::app
