// Meguri GUI エントリポイント。
#define WIN32_LEAN_AND_MEAN
#include <objbase.h>
#include <shellapi.h>
#include <windows.h>

#include <string>

#include "main_window.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmd_line, int show_command) {
    // 混在 DPI 環境向け (Per-Monitor V2)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // UI スレッドは STA (シェルダイアログ・IFileOperation 用)。エンジンのワーカーは MTA
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    {
        // 引数にフォルダを渡すと起動時にそれを開く (E2E やショートカット用)。
        // 注意: CommandLineToArgvW は空文字列を渡すと EXE 自身のパスを返す仕様が
        // あるため、引数が本当にあるときだけ解析する (無いと last_folder 復元が壊れる)
        std::wstring initial_folder;
        if (cmd_line && cmd_line[0] != L'\0') {
            int argc = 0;
            if (LPWSTR* argv = CommandLineToArgvW(cmd_line, &argc); argv) {
                if (argc >= 1 && argv[0][0] != L'\0') initial_folder = argv[0];
                LocalFree(argv);
            }
        }

        meguri::MainWindow window;
        if (!window.create(instance, show_command, initial_folder)) {
            CoUninitialize();
            return 1;
        }

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    return 0;
}
