#include "scanner.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace meguri::io {

namespace {

std::wstring lower_extension(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return ext;
}

bool classify(const fs::path& p, core::MediaType* out_type) {
    const std::wstring ext = lower_extension(p);
    if (ext == L".webp") {
        *out_type = core::MediaType::Webp;
        return true;
    }
    if (ext == L".mp4") {
        *out_type = core::MediaType::Mp4;
        return true;
    }
    if (ext == L".png") {
        *out_type = core::MediaType::Png;
        return true;
    }
    if (ext == L".jpg" || ext == L".jpeg") {
        *out_type = core::MediaType::Jpeg;
        return true;
    }
    return false;
}

void append_entry(const fs::directory_entry& entry, std::vector<core::MediaItem>* out) {
    std::error_code ec;
    if (!entry.is_regular_file(ec) || ec) return;
    core::MediaType type;
    if (!classify(entry.path(), &type)) return;

    core::MediaItem item;
    item.path = entry.path().wstring();
    item.type = type;
    item.file_size = entry.file_size(ec);
    if (ec) item.file_size = 0;
    const auto mtime = entry.last_write_time(ec);
    if (!ec) {
        item.modified_time =
            std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
    }
    out->push_back(std::move(item));
}

}  // namespace

bool is_supported_media_path(const std::wstring& path) {
    core::MediaType ignored;
    return classify(fs::path(path), &ignored);
}

bool classify_media_path(const std::wstring& path, core::MediaType* out_type) {
    return classify(fs::path(path), out_type);
}

std::vector<core::MediaItem> scan_folder(const std::wstring& folder, const ScanOptions& options) {
    std::vector<core::MediaItem> items;
    std::error_code ec;
    const auto dir_options = fs::directory_options::skip_permission_denied;
    if (options.recursive) {
        for (fs::recursive_directory_iterator it(folder, dir_options, ec), end;
             !ec && it != end; it.increment(ec)) {
            append_entry(*it, &items);
        }
    } else {
        for (fs::directory_iterator it(folder, dir_options, ec), end; !ec && it != end;
             it.increment(ec)) {
            append_entry(*it, &items);
        }
    }
    std::sort(items.begin(), items.end(),
              [](const core::MediaItem& a, const core::MediaItem& b) { return a.path < b.path; });
    return items;
}

}  // namespace meguri::io
