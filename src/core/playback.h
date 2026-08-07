// 再生タイミングの純ロジック。経過時間からループ内の表示フレームを決める。
#pragma once

#include <cstdint>
#include <vector>

namespace meguri::core {

// durations_ms: 各フレームの表示時間 (ms)。0 以下は 10ms 扱い (壊れた WEBP 対策)。
// elapsed_ms: 再生開始からの経過時間。ループを考慮して表示すべきフレーム index を返す。
// 空なら 0 を返す。
int frame_index_for_time(const std::vector<int>& durations_ms, int64_t elapsed_ms);

// 総再生時間 (ms)。0 以下のフレームは 10ms として合算。
int64_t total_duration_ms(const std::vector<int>& durations_ms);

}  // namespace meguri::core
