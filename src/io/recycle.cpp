#include "recycle.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;

namespace meguri::io {

namespace {

std::wstring to_lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

}  // namespace

bool send_to_recycle_bin(const std::vector<std::wstring>& paths, std::wstring* error) {
    if (paths.empty()) return true;

    ComPtr<IFileOperation> op;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    if (FAILED(hr)) {
        if (error) *error = L"IFileOperation の生成に失敗しました";
        return false;
    }
    op->SetOperationFlags(FOFX_RECYCLEONDELETE | FOF_ALLOWUNDO | FOF_NOCONFIRMATION |
                          FOF_SILENT | FOF_NOERRORUI);

    int queued = 0;
    for (const auto& path : paths) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            if (SUCCEEDED(op->DeleteItem(item.Get(), nullptr))) ++queued;
        }
    }
    if (queued == 0) {
        if (error) *error = L"削除対象を開けませんでした";
        return false;
    }
    hr = op->PerformOperations();
    if (FAILED(hr)) {
        if (error) *error = L"ゴミ箱への移動に失敗しました";
        return false;
    }
    BOOL aborted = FALSE;
    op->GetAnyOperationsAborted(&aborted);
    if (aborted) {
        if (error) *error = L"一部の削除が中断されました";
        return false;
    }
    return true;
}

int restore_from_recycle_bin(const std::vector<std::wstring>& original_paths) {
    if (original_paths.empty()) return 0;

    // 検索用に小文字化したフルパス集合を作る
    std::vector<std::wstring> targets;
    targets.reserve(original_paths.size());
    for (const auto& p : original_paths) targets.push_back(to_lower(p));

    ComPtr<IShellFolder> desktop;
    if (FAILED(SHGetDesktopFolder(&desktop))) return 0;

    PIDLIST_ABSOLUTE bin_pidl = nullptr;
    if (FAILED(SHGetSpecialFolderLocation(nullptr, CSIDL_BITBUCKET, &bin_pidl))) return 0;

    ComPtr<IShellFolder2> bin;
    HRESULT hr = desktop->BindToObject(bin_pidl, nullptr, IID_PPV_ARGS(&bin));
    CoTaskMemFree(bin_pidl);
    if (FAILED(hr)) return 0;

    ComPtr<IEnumIDList> enum_ids;
    if (FAILED(bin->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &enum_ids)) ||
        !enum_ids) {
        return 0;
    }

    int restored = 0;
    PITEMID_CHILD child = nullptr;
    while (enum_ids->Next(1, &child, nullptr) == S_OK) {
        // 列 0: 名前, 列 1: 元の場所 (日本語/英語どちらの Windows でも列順は同じ)
        auto details_text = [&](int column) -> std::wstring {
            SHELLDETAILS details{};
            if (FAILED(bin->GetDetailsOf(child, column, &details))) return L"";
            wchar_t buf[MAX_PATH * 2] = L"";
            if (FAILED(StrRetToBufW(&details.str, child, buf, ARRAYSIZE(buf)))) return L"";
            return buf;
        };
        const std::wstring name = details_text(0);
        std::wstring location = details_text(1);
        if (!location.empty() && location.back() != L'\\') location += L'\\';
        const std::wstring full = to_lower(location + name);

        const bool match =
            std::find(targets.begin(), targets.end(), full) != targets.end();
        if (match) {
            // コンテキストメニューの undelete 動詞で復元する
            ComPtr<IContextMenu> menu;
            LPCITEMIDLIST items[] = {child};
            if (SUCCEEDED(bin->GetUIObjectOf(nullptr, 1, items, IID_IContextMenu, nullptr,
                                             reinterpret_cast<void**>(menu.GetAddressOf())))) {
                CMINVOKECOMMANDINFO info{};
                info.cbSize = sizeof(info);
                info.lpVerb = "undelete";
                info.nShow = SW_SHOWNORMAL;
                if (SUCCEEDED(menu->InvokeCommand(&info))) ++restored;
            }
        }
        CoTaskMemFree(child);
        child = nullptr;
    }
    return restored;
}

}  // namespace meguri::io
