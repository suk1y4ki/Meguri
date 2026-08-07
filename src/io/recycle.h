// ゴミ箱操作。削除 (復元可能) と、直前削除の復元。
#pragma once

#include <string>
#include <vector>

namespace meguri::io {

// paths をゴミ箱へ送る。確認ダイアログは出さない。
// 失敗したときは false を返し error に理由を入れる (成功パスもあるため部分成功に注意)。
// COM 初期化済みスレッドから呼ぶこと (GUI スレッドなど)。
bool send_to_recycle_bin(const std::vector<std::wstring>& paths, std::wstring* error);

// ゴミ箱を列挙して、元パスが original_paths に一致する項目を復元する。
// 復元できた件数を返す (Ctrl+Z 用)。
int restore_from_recycle_bin(const std::vector<std::wstring>& original_paths);

}  // namespace meguri::io
