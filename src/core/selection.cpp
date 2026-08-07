#include "selection.h"

#include <algorithm>

namespace meguri::core {

void Selection::click(int index, bool ctrl, bool shift) {
    if (index < 0) {
        if (!ctrl && !shift) clear();
        return;
    }
    if (shift && anchor_ >= 0) {
        const int lo = std::min(anchor_, index);
        const int hi = std::max(anchor_, index);
        if (!ctrl) selected_.clear();
        for (int i = lo; i <= hi; ++i) selected_.insert(i);
        // shift 選択ではアンカーは維持する (Explorer と同じ挙動)
        return;
    }
    if (ctrl) {
        if (selected_.count(index)) {
            selected_.erase(index);
        } else {
            selected_.insert(index);
        }
        anchor_ = index;
        return;
    }
    selected_.clear();
    selected_.insert(index);
    anchor_ = index;
}

void Selection::select_all(int count) {
    selected_.clear();
    for (int i = 0; i < count; ++i) selected_.insert(i);
    if (count > 0 && anchor_ < 0) anchor_ = 0;
}

void Selection::clear() {
    selected_.clear();
    anchor_ = -1;
}

void Selection::remap(const std::vector<int>& old_to_new) {
    std::set<int> next;
    for (int old_index : selected_) {
        if (old_index >= 0 && old_index < static_cast<int>(old_to_new.size())) {
            const int now = old_to_new[old_index];
            if (now >= 0) next.insert(now);
        }
    }
    selected_ = std::move(next);
    if (anchor_ >= 0 && anchor_ < static_cast<int>(old_to_new.size())) {
        anchor_ = old_to_new[anchor_];
    } else {
        anchor_ = -1;
    }
}

std::vector<int> Selection::items() const {
    return std::vector<int>(selected_.begin(), selected_.end());
}

}  // namespace meguri::core
