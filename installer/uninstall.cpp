// ============================================================================
//  政策解读台 卸载程序
//
//  · 默认保留 output\ 稿件与 agent.ini（方便以后重装）
//  · 可选择连同稿件和密钥一起删除
//  · 支持静默卸载：uninstall.exe /S [/PURGE]
// ============================================================================

#define _WIN32_WINNT 0x0A00
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>

#include <string>
#include <vector>

using std::wstring;

#define APP_NAME L"政策解读台"
#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PolicyDesk"

static wstring g_dir;
static wstring g_self;
static std::vector<wstring> g_errors;

static wstring sysError(DWORD e) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, e, 0, (LPWSTR)&buf, 0, nullptr);
    wstring s = (n && buf) ? wstring(buf, n) : wstring();
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' '))
        s.pop_back();
    return s;
}

static bool fileExists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool dirExists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    if (SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p) == S_OK) {
        wstring s = p;
        CoTaskMemFree(p);
        return s;
    }
    return {};
}

static void deleteFile(const wstring& path) {
    if (!fileExists(path)) return;
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!DeleteFileW(path.c_str())) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND)
            g_errors.push_back(L"无法删除：" + path + L"（" + sysError(e) + L"）");
    }
}

static void removeDir(const wstring& dir) {
    if (!dirExists(dir)) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    RemoveDirectoryW(p.c_str());
                } else {
                    removeDir(p);
                }
            } else {
                deleteFile(p);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryW(dir.c_str())) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND && e != ERROR_DIR_NOT_EMPTY)
            g_errors.push_back(L"无法删除文件夹：" + dir + L"（" + sysError(e) + L"）");
    }
}

static void removeShortcuts() {
    wstring desktop = knownFolder(FOLDERID_Desktop);
    if (!desktop.empty()) deleteFile(desktop + L"\\" + APP_NAME + L".lnk");

    wstring programs = knownFolder(FOLDERID_Programs);
    if (!programs.empty()) removeDir(programs + L"\\" + APP_NAME);
}

static void removeRegistry() {
    RegDeleteTreeW(HKEY_CURRENT_USER, UNINSTALL_KEY);
}

static bool closeRunningApp(bool silent) {
    HWND w = FindWindowW(L"NewsDeskWindow", nullptr);
    if (!w) return true;
    if (!silent) {
        if (MessageBoxW(nullptr,
                        L"政策解读台正在运行。\n\n是否关闭它并继续卸载？",
                        APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return false;
    }
    PostMessageW(w, WM_CLOSE, 0, 0);
    for (int i = 0; i < 25; ++i) {
        Sleep(100);
        if (!IsWindow(w)) break;
    }
    if (IsWindow(w)) {
        g_errors.push_back(L"政策解读台仍在运行，部分文件可能无法删除。");
        return false;
    }
    return true;
}

static void scheduleSelfDelete(bool purge) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    wstring cmd = L"\"" + wstring(sys) + L"\\cmd.exe\" /c ping 127.0.0.1 -n 3 >nul & del /f /q \"" +
                  g_self + L"\"";
    if (purge) cmd += L" & rmdir /s /q \"" + g_dir + L"\"";

    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, temp,
                       &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    (void)hInst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_self = self;
    size_t slash = g_self.find_last_of(L"\\/");
    g_dir = (slash == wstring::npos) ? wstring(L".") : g_self.substr(0, slash);

    std::vector<wstring> args;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 0; i < argc; ++i) args.push_back(argv[i]);
        LocalFree(argv);
    }
    bool silent = false, purge = false;
    for (const wstring& a : args) {
        if (_wcsicmp(a.c_str(), L"/S") == 0 || _wcsicmp(a.c_str(), L"--silent") == 0) silent = true;
        if (_wcsicmp(a.c_str(), L"/PURGE") == 0 || _wcsicmp(a.c_str(), L"--purge") == 0) purge = true;
    }

    if (!silent) {
        TASKDIALOG_BUTTON buttons[2] = {
            {100, L"卸载（保留稿件与密钥）"},
            {101, L"卸载并删除全部数据"},
        };
        TASKDIALOGCONFIG cfg;
        ZeroMemory(&cfg, sizeof(cfg));
        cfg.cbSize = sizeof(cfg);
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS;
        cfg.pszWindowTitle = APP_NAME L" 卸载";
        cfg.pszMainIcon = TD_INFORMATION_ICON;
        cfg.pszMainInstruction = L"要卸载政策解读台吗？";
        cfg.pszContent =
            L"将删除程序文件、快捷方式和卸载登记。\n\n"
            L"• 保留稿件：output 文件夹与 agent.ini 里的 API 密钥会保留。\n"
            L"• 全部删除：连同已下载的稿件和密钥一起删除，无法恢复。";
        cfg.pButtons = buttons;
        cfg.cButtons = 2;
        cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        cfg.nDefaultButton = 100;

        int chosen = 0;
        HRESULT hr = TaskDialogIndirect(&cfg, &chosen, nullptr, nullptr);
        if (FAILED(hr)) {
            int r = MessageBoxW(nullptr,
                                L"要卸载政策解读台吗？\n\n"
                                L"是：保留 output 稿件与 agent.ini\n"
                                L"否：连同稿件和密钥一起删除\n"
                                L"取消：退出",
                                APP_NAME, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDYES)
                chosen = 100;
            else if (r == IDNO)
                chosen = 101;
            else
                chosen = 0;
        }
        if (chosen != 100 && chosen != 101) {
            CoUninitialize();
            return 0;
        }
        purge = (chosen == 101);
    }

    if (!closeRunningApp(silent)) {
        if (!silent && !g_errors.empty()) {
            MessageBoxW(nullptr, g_errors.front().c_str(), APP_NAME, MB_ICONERROR | MB_OK);
        }
        CoUninitialize();
        return silent ? 1 : (g_errors.empty() ? 0 : 1);
    }

    removeShortcuts();
    const wchar_t* files[] = {L"news_desk.exe", L"agent_host.exe", L"fetch_news.exe",
                              L"运行.bat", L"config.ini", L"agent.ini.example",
                              L"README.md", L"LICENSE.md", L"使用说明.txt"};
    for (const wchar_t* f : files) deleteFile(g_dir + L"\\" + f);
    removeDir(g_dir + L"\\skill");
    deleteFile(g_dir + L"\\.pd_write_test");

    if (purge) {
        removeDir(g_dir + L"\\output");
        deleteFile(g_dir + L"\\agent.ini");
    }

    removeRegistry();
    scheduleSelfDelete(purge);

    if (!silent) {
        wstring msg = purge ? L"已卸载政策解读台，并删除全部数据。" :
                              L"已卸载政策解读台。\n\n已保留：\n" + g_dir +
                                  L"\\output\n" + g_dir + L"\\agent.ini";
        if (!g_errors.empty()) {
            msg += L"\n\n以下项目未能删除（可能正在被占用）：\n";
            for (size_t i = 0; i < g_errors.size() && i < 6; ++i) msg += L"• " + g_errors[i] + L"\n";
            if (g_errors.size() > 6) msg += L"……\n";
        }
        MessageBoxW(nullptr, msg.c_str(), APP_NAME,
                    (g_errors.empty() ? MB_ICONINFORMATION : MB_ICONWARNING) | MB_OK);
    }

    CoUninitialize();
    return g_errors.empty() ? 0 : 1;
}
