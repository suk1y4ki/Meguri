// プローブ結果 (解像度・再生時間) の永続キャッシュ。
// %APPDATA%\Meguri\probe_cache.json に保存し、2 回目以降のフォルダオープンで
// 全件プローブを省略する。ファイルサイズ + 更新日時が変わったら無効。
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/media_item.h"

namespace meguri::app {

class ProbeCache {
public:
    // 起動時に一度読み込む。壊れた JSON は空として扱う
    void load();
    bool save() const;

    // 走査結果にキャッシュ値を適用する (一致したものは width 等が埋まり、
    // エンジン側でプローブ済み扱いになる)
    void apply(std::vector<core::MediaItem>* items) const;

    // プローブ済みアイテムの値を取り込む (width > 0 のもののみ)。
    // 変更があったときだけ true
    bool update(const std::vector<core::MediaItem>& items);

    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        uint64_t file_size = 0;
        int64_t modified_time = 0;
        int width = 0;
        int height = 0;
        double duration_sec = 0.0;
        int frame_count = 0;
    };
    std::unordered_map<std::wstring, Entry> entries_;
};

}  // namespace meguri::app
