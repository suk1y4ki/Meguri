// タイル選択モデル。インデックスは「フィルタ・ソート後の表示順」で扱う。
#pragma once

#include <set>
#include <vector>

namespace meguri::core {

class Selection {
public:
    // クリック操作。ctrl: トグル追加、shift: アンカーからの範囲選択。
    void click(int index, bool ctrl, bool shift);
    void select_all(int count);
    void clear();

    // 削除などで表示リストが変わったとき、残存アイテムの旧→新対応で選択を再構成する。
    // old_to_new[旧index] = 新index (削除されたものは -1)。
    void remap(const std::vector<int>& old_to_new);

    bool contains(int index) const { return selected_.count(index) > 0; }
    bool empty() const { return selected_.empty(); }
    size_t size() const { return selected_.size(); }
    std::vector<int> items() const;  // 昇順
    int anchor() const { return anchor_; }

private:
    std::set<int> selected_;
    int anchor_ = -1;
};

}  // namespace meguri::core
