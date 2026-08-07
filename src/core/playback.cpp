#include "playback.h"

namespace meguri::core {

namespace {
constexpr int kMinFrameMs = 10;
inline int sanitize(int ms) { return ms <= 0 ? kMinFrameMs : ms; }
}  // namespace

int64_t total_duration_ms(const std::vector<int>& durations_ms) {
    int64_t total = 0;
    for (int d : durations_ms) total += sanitize(d);
    return total;
}

int frame_index_for_time(const std::vector<int>& durations_ms, int64_t elapsed_ms) {
    if (durations_ms.empty()) return 0;
    const int64_t total = total_duration_ms(durations_ms);
    if (total <= 0) return 0;
    int64_t t = elapsed_ms % total;
    if (t < 0) t += total;
    for (int i = 0; i < static_cast<int>(durations_ms.size()); ++i) {
        t -= sanitize(durations_ms[i]);
        if (t < 0) return i;
    }
    return static_cast<int>(durations_ms.size()) - 1;
}

}  // namespace meguri::core
