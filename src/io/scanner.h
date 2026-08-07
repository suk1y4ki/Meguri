// フォルダ走査。対応拡張子 (.webp / .mp4) のファイルを列挙して MediaItem を作る。
#pragma once

#include <string>
#include <vector>

#include "core/media_item.h"

namespace meguri::io {

struct ScanOptions {
    bool recursive = true;  // サブフォルダも走査する (既定オン)
};

// folder 以下を走査して MediaItem (メタデータ未プローブ) を返す。
// 読めないエントリは黙ってスキップする。パスの辞書順で返す。
std::vector<core::MediaItem> scan_folder(const std::wstring& folder, const ScanOptions& options);

// 拡張子判定 (小文字比較)。走査以外 (D&D 受け入れ等) でも使う。
bool is_supported_media_path(const std::wstring& path);

// 拡張子から種別を判定する (大文字小文字を無視)。非対応拡張子は false。
bool classify_media_path(const std::wstring& path, core::MediaType* out_type);

}  // namespace meguri::io
