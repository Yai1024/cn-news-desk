// ============================================================================
//  政策解读台 安装程序（Win32 + GDI+，无第三方运行时依赖）
//
//  · 单文件安装包：内置全部程序、skill 方法论与卸载程序
//  · 向导页里直接填写 DeepSeek API 密钥，安装时写入本机 agent.ini
//  · 默认装到 %LOCALAPPDATA%\Programs\政策解读台，不需要管理员权限
//  · 自动创建桌面 / 开始菜单快捷方式，并登记到「应用和功能」
//  · 支持静默安装：
//      政策解读台-安装程序.exe /S [/D=目录] [/KEY=sk-xxx]
//                              [/BASE=https://api.deepseek.com] [/MODEL=deepseek-chat]
//                              [/NODESKTOP]
// ============================================================================

#define _WIN32_WINNT 0x0A00
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <commctrl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace Gdiplus;
using std::string;
using std::wstring;

#define APP_NAME L"政策解读台"
#define APP_VERSION L"1.0.0"
#define APP_PUBLISHER L"Yai1024"
#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PolicyDesk"

#define IDI_APP 1
#define IDR_PAYLOAD 2

#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)
#define WM_APP_ESCAPE (WM_APP + 3)

enum Page { PAGE_WELCOME, PAGE_KEY, PAGE_INSTALL, PAGE_DONE };

enum : UINT {
    ID_EDIT_PATH = 3001,
    ID_EDIT_KEY,
    ID_EDIT_BASE,
    ID_EDIT_MODEL,
    ID_CHK_DESKTOP = 3010,
    ID_CHK_SHOWKEY,
    ID_CHK_ADVANCED,
    ID_CHK_RUN,
    ID_BTN_BROWSE = 3020,
    ID_BTN_PRIMARY,
    ID_BTN_SECONDARY,
    ID_BTN_LINK
};

static const wchar_t* kDefaultBase = L"https://api.deepseek.com";
static const wchar_t* kDefaultModel = L"deepseek-chat";

static HINSTANCE g_hInst = nullptr;
static HWND g_hwnd = nullptr;
static int g_dpi = 96;
static ULONG_PTR g_gdiToken = 0;

static Page g_page = PAGE_WELCOME;
static wstring g_dir;
static wstring g_key;
static wstring g_base = kDefaultBase;
static wstring g_model = kDefaultModel;
static bool g_desktop = true;
static bool g_showKey = false;
static bool g_advanced = false;
static bool g_run = true;
static bool g_installing = false;
static bool g_installOk = false;
static int g_step = 0;
static int g_progress = 0;
static wstring g_status;

static HWND g_editPath = nullptr, g_editKey = nullptr, g_editBase = nullptr, g_editModel = nullptr;
static HWND g_chkDesktop = nullptr, g_chkShow = nullptr, g_chkAdvanced = nullptr, g_chkRun = nullptr;
static HWND g_btnBrowse = nullptr, g_btnPrimary = nullptr, g_btnSecondary = nullptr, g_btnLink = nullptr;

static FontFamily* g_family = nullptr;
static Font *g_h1 = nullptr, *g_body = nullptr, *g_small = nullptr;
static Font *g_label = nullptr, *g_btn = nullptr, *g_logo = nullptr;
static HFONT g_hfEdit = nullptr, g_hfCheck = nullptr;
static HBRUSH g_brushBg = nullptr, g_brushWhite = nullptr;
static std::map<UINT, bool> g_hover;

static HANDLE g_logHandle = INVALID_HANDLE_VALUE;
static wstring g_logPath;
static CRITICAL_SECTION g_logCs;
static bool g_logCsReady = false;

static void updatePage();
static void layoutControls();
static LRESULT CALLBACK EditSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK BtnSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

// ----------------------------------------------------------------------------
// 基础工具
// ----------------------------------------------------------------------------

namespace C {
static const Color BG(255, 250, 248, 245);
static const Color CARD(255, 255, 255, 255);
static const Color BORDER(255, 231, 226, 218);
static const Color INK(255, 27, 27, 27);
static const Color MUTED(255, 122, 115, 107);
static const Color FAINT(255, 168, 161, 152);
static const Color ACCENT(255, 200, 50, 43);
static const Color ACCENT_HOVER(255, 176, 42, 36);
static const Color ACCENT_DOWN(255, 153, 36, 31);
static const Color ACCENT_SOFT(255, 252, 243, 241);
static const Color GHOST_HOVER(255, 243, 239, 234);
static const Color DISABLED_BG(255, 241, 238, 234);
static const Color DISABLED_FG(255, 183, 176, 168);
static const Color OK(255, 46, 125, 91);
static const Color HEAD1(255, 150, 36, 31);
static const Color HEAD2(255, 200, 50, 43);
static const Color ROW_DONE(255, 239, 234, 227);
}  // namespace C

static int px(int v) { return MulDiv(v, g_dpi, 96); }

static wstring trim(const wstring& in) {
    size_t a = 0, b = in.size();
    while (a < b && iswspace(in[a])) ++a;
    while (b > a && iswspace(in[b - 1])) --b;
    return in.substr(a, b - a);
}

static string trimAscii(const string& in) {
    size_t a = 0, b = in.size();
    while (a < b && (in[a] == ' ' || in[a] == '\t' || in[a] == '\r' || in[a] == '\n')) ++a;
    while (b > a && (in[b - 1] == ' ' || in[b - 1] == '\t' || in[b - 1] == '\r' ||
                     in[b - 1] == '\n'))
        --b;
    return in.substr(a, b - a);
}

static string lowerAscii(string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static string wideToUtf8(const wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static wstring utf8ToWide(const string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), &w[0], n);
    return w;
}

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

static wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    if (SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p) == S_OK) {
        wstring s = p;
        CoTaskMemFree(p);
        return s;
    }
    return {};
}

static wstring windowsDir() {
    wchar_t buf[MAX_PATH] = {};
    UINT n = GetWindowsDirectoryW(buf, MAX_PATH);
    return n ? wstring(buf, n) : wstring();
}

static bool fileExists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool dirExists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool ensureDir(const wstring& path, wstring& err) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) {
        if (a & FILE_ATTRIBUTE_DIRECTORY) return true;
        err = L"路径已存在但不是文件夹：\n" + path;
        return false;
    }
    size_t pos = path.find_last_of(L"\\/");
    if (pos != wstring::npos && pos > 2) {
        if (!ensureDir(path.substr(0, pos), err)) return false;
    }
    if (!CreateDirectoryW(path.c_str(), nullptr)) {
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) {
            err = L"无法创建文件夹：\n" + path + L"\n\n" + sysError(e);
            return false;
        }
    }
    return true;
}

static wstring normalizeDir(const wstring& in) {
    wstring s = trim(in);
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    if (s.empty()) return s;
    wchar_t buf[32768] = {};
    DWORD n = GetFullPathNameW(s.c_str(), 32768, buf, nullptr);
    wstring r = (n > 0 && n < 32768) ? wstring(buf, n) : s;
    while (r.size() > 3 && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    return r;
}

static bool isAbsolutePath(const wstring& p) {
    return (p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) ||
           (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\');
}

static wstring expandEnv(const wstring& p) {
    wchar_t buf[32768] = {};
    DWORD n = ExpandEnvironmentStringsW(p.c_str(), buf, 32768);
    if (n == 0 || n >= 32768) return p;
    return wstring(buf, n - 1);
}

static bool dirHasEntries(const wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool any = false;
    do {
        if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
            any = true;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}

static string readFileUtf8(const wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(h);
        return {};
    }
    string s((size_t)sz.QuadPart, '\0');
    DWORD total = 0;
    while (total < s.size()) {
        DWORD got = 0;
        DWORD want = (DWORD)std::min<size_t>(s.size() - total, 1 << 20);
        if (!ReadFile(h, &s[total], want, &got, nullptr) || got == 0) break;
        total += got;
    }
    CloseHandle(h);
    s.resize(total);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
    return s;
}

static bool writeFileBytes(const wstring& path, const string& data, wstring& err) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        err = L"无法写入文件：\n" + path + L"\n\n" + sysError(e);
        return false;
    }
    DWORD total = 0;
    while (total < data.size()) {
        DWORD wrote = 0;
        DWORD want = (DWORD)std::min<size_t>(data.size() - total, 1 << 20);
        if (!WriteFile(h, data.data() + total, want, &wrote, nullptr) || wrote == 0) {
            DWORD e = GetLastError();
            CloseHandle(h);
            err = L"写入文件失败：\n" + path + L"\n\n" + sysError(e);
            return false;
        }
        total += wrote;
    }
    CloseHandle(h);
    return true;
}

static string iniGet(const string& content, const string& key) {
    string s;
    for (char c : content) {
        if (c != '\r') s.push_back(c);
    }
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t nl = s.find('\n', pos);
        string line = trimAscii((nl == string::npos) ? s.substr(pos) : s.substr(pos, nl - pos));
        pos = (nl == string::npos) ? s.size() + 1 : nl + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == string::npos) continue;
        if (lowerAscii(trimAscii(line.substr(0, eq))) == lowerAscii(key))
            return trimAscii(line.substr(eq + 1));
    }
    return {};
}

static string iniSet(string content, const string& section, const string& key, const string& value) {
    string norm;
    norm.reserve(content.size());
    for (char c : content) {
        if (c != '\r') norm.push_back(c);
    }

    std::vector<string> lines;
    size_t pos = 0;
    while (pos <= norm.size()) {
        size_t nl = norm.find('\n', pos);
        if (nl == string::npos) {
            lines.push_back(norm.substr(pos));
            break;
        }
        lines.push_back(norm.substr(pos, nl - pos));
        pos = nl + 1;
    }
    while (!lines.empty() && trimAscii(lines.back()).empty()) lines.pop_back();

    string clean;
    for (char c : value)
        if (c != '\r' && c != '\n') clean.push_back(c);

    string secTag = "[" + lowerAscii(trimAscii(section)) + "]";
    int secIdx = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        string t = trimAscii(lines[i]);
        if (!t.empty() && t[0] == '[' && lowerAscii(t) == secTag) {
            secIdx = (int)i;
            break;
        }
    }

    if (secIdx >= 0) {
        size_t end = lines.size();
        for (size_t i = (size_t)secIdx + 1; i < lines.size(); ++i) {
            string t = trimAscii(lines[i]);
            if (!t.empty() && t[0] == '[') {
                end = i;
                break;
            }
        }
        int found = -1;
        for (size_t i = (size_t)secIdx + 1; i < end; ++i) {
            string t = trimAscii(lines[i]);
            if (t.empty() || t[0] == '#' || t[0] == ';') continue;
            size_t eq = t.find('=');
            if (eq == string::npos) continue;
            if (lowerAscii(trimAscii(t.substr(0, eq))) == lowerAscii(key)) {
                found = (int)i;
                break;
            }
        }
        string line = key + "=" + clean;
        if (found >= 0)
            lines[found] = line;
        else
            lines.insert(lines.begin() + secIdx + 1, line);
    } else {
        if (!lines.empty() && !trimAscii(lines.back()).empty()) lines.push_back("");
        lines.push_back("[" + section + "]");
        lines.push_back(key + "=" + clean);
    }

    string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out += "\r\n";
        out += lines[i];
    }
    if (!out.empty()) out += "\r\n";
    return out;
}

// ----------------------------------------------------------------------------
// 日志
// ----------------------------------------------------------------------------

static void logOpen() {
    InitializeCriticalSection(&g_logCs);
    g_logCsReady = true;
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    g_logPath = wstring(tmp) + L"政策解读台-安装日志.txt";
    g_logHandle = CreateFileW(g_logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logHandle != INVALID_HANDLE_VALUE) {
        const BYTE bom[3] = {0xEF, 0xBB, 0xBF};
        DWORD w = 0;
        WriteFile(g_logHandle, bom, 3, &w, nullptr);
    }
}

static void logMsg(const wstring& s) {
    if (g_logHandle == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t head[64];
    swprintf(head, 64, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    string utf8 = wideToUtf8(wstring(head) + s + L"\r\n");
    if (g_logCsReady) EnterCriticalSection(&g_logCs);
    DWORD w = 0;
    WriteFile(g_logHandle, utf8.data(), (DWORD)utf8.size(), &w, nullptr);
    if (g_logCsReady) LeaveCriticalSection(&g_logCs);
}

static void logClose() {
    if (g_logHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logHandle);
        g_logHandle = INVALID_HANDLE_VALUE;
    }
    if (g_logCsReady) {
        DeleteCriticalSection(&g_logCs);
        g_logCsReady = false;
    }
}

// ----------------------------------------------------------------------------
// 注册表
// ----------------------------------------------------------------------------

static bool regSetStr(HKEY root, const wstring& sub, const wstring& name, const wstring& value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(k, name.c_str(), 0, REG_SZ, (const BYTE*)value.c_str(),
                            (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static bool regSetDword(HKEY root, const wstring& sub, const wstring& name, DWORD value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(k, name.c_str(), 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static wstring regGetStr(HKEY root, const wstring& sub, const wstring& name) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return {};
    wchar_t buf[32768] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    LONG r = RegQueryValueExW(k, name.c_str(), nullptr, &type, (BYTE*)buf, &size);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_SZ || size < sizeof(wchar_t)) return {};
    return wstring(buf);
}

// ----------------------------------------------------------------------------
// 安装参数与载荷
// ----------------------------------------------------------------------------

struct InstallInput {
    wstring dir;
    wstring key;
    wstring baseUrl;
    wstring model;
    bool setKey = false;
    bool setBase = false;
    bool setModel = false;
    bool desktop = true;
};

struct PayloadEntry {
    string path;
    uint32_t size = 0;
    const BYTE* data = nullptr;
};

static bool loadPayload(std::vector<PayloadEntry>& out, uint64_t& total, wstring& err) {
    HRSRC res = FindResourceW(g_hInst, MAKEINTRESOURCEW(IDR_PAYLOAD), RT_RCDATA);
    if (!res) {
        err = L"安装包内部数据缺失，请重新下载安装包。";
        return false;
    }
    HGLOBAL hg = LoadResource(g_hInst, res);
    DWORD rawSize = SizeofResource(g_hInst, res);
    const BYTE* p = hg ? (const BYTE*)LockResource(hg) : nullptr;
    if (!p || rawSize < 20) {
        err = L"安装包内部数据已损坏。";
        return false;
    }
    if (memcmp(p, "NDPKG001", 8) != 0) {
        err = L"安装包内部数据格式不正确。";
        return false;
    }

    uint64_t totalBytes = 0;
    uint32_t count = 0;
    memcpy(&totalBytes, p + 8, 8);
    memcpy(&count, p + 16, 4);
    if (count == 0 || count > 10000) {
        err = L"安装包内文件数量异常。";
        return false;
    }

    size_t off = 20;
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 2 > rawSize) {
            err = L"安装包内部数据被截断。";
            return false;
        }
        uint16_t pathLen = 0;
        memcpy(&pathLen, p + off, 2);
        off += 2;
        if (pathLen == 0 || pathLen > 4096 || off + pathLen + 4 > rawSize) {
            err = L"安装包内部路径异常。";
            return false;
        }
        string path((const char*)p + off, pathLen);
        off += pathLen;
        uint32_t size = 0;
        memcpy(&size, p + off, 4);
        off += 4;
        if (off + size > rawSize) {
            err = L"安装包内部数据被截断。";
            return false;
        }
        out.push_back({path, size, p + off});
        off += size;
    }
    total = totalBytes;
    return true;
}

static bool safeRel(const string& utf8, wstring& rel, wstring& err) {
    wstring w = utf8ToWide(utf8);
    if (w.empty()) {
        err = L"安装包内部路径为空。";
        return false;
    }
    for (wchar_t& c : w)
        if (c == L'/') c = L'\\';
    if (w[0] == L'\\' || w.find(L':') != wstring::npos) {
        err = L"安装包内部路径不合法：" + w;
        return false;
    }
    size_t start = 0;
    while (start <= w.size()) {
        size_t p = w.find(L'\\', start);
        wstring seg = (p == wstring::npos) ? w.substr(start) : w.substr(start, p - start);
        if (seg.empty() || seg == L"." || seg == L"..") {
            err = L"安装包内部路径不合法：" + w;
            return false;
        }
        if (p == wstring::npos) break;
        start = p + 1;
    }
    rel = w;
    return true;
}

static bool extractPayload(const wstring& dir,
                           const std::function<void(int, int, const wstring&)>& progress,
                           wstring& err) {
    std::vector<PayloadEntry> entries;
    uint64_t total = 0;
    if (!loadPayload(entries, total, err)) return false;

    logMsg(L"载荷 " + std::to_wstring(entries.size()) + L" 个文件，" + std::to_wstring(total) +
           L" 字节");
    uint64_t done = 0;
    int lastPct = -1;
    for (const PayloadEntry& e : entries) {
        wstring rel;
        if (!safeRel(e.path, rel, err)) return false;
        wstring full = dir + L"\\" + rel;

        size_t slash = full.find_last_of(L"\\/");
        if (slash != wstring::npos) {
            if (!ensureDir(full.substr(0, slash), err)) return false;
        }

        SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
        HANDLE h = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            err = L"无法写入文件：\n" + full + L"\n\n" + sysError(e) +
                  L"\n\n如果政策解读台正在运行，请先关闭它再安装。";
            return false;
        }
        const BYTE* ptr = e.data;
        size_t left = e.size;
        while (left > 0) {
            DWORD wrote = 0;
            DWORD want = (DWORD)std::min<size_t>(left, 1 << 20);
            if (!WriteFile(h, ptr, want, &wrote, nullptr) || wrote == 0) {
                DWORD we = GetLastError();
                CloseHandle(h);
                err = L"写入文件失败：\n" + full + L"\n\n" + sysError(we);
                return false;
            }
            ptr += wrote;
            left -= wrote;
        }
        CloseHandle(h);
        done += e.size;

        int pct = total ? (int)(5 + (80.0 * (double)done / (double)total)) : 85;
        if (pct != lastPct) {
            lastPct = pct;
            if (progress) progress(1, pct, L"正在释放程序文件…");
        }
    }
    if (progress) progress(1, 85, L"正在释放程序文件…");
    return true;
}

static bool writeConfig(const InstallInput& in, wstring& err) {
    wstring path = in.dir + L"\\agent.ini";
    string content = readFileUtf8(path);
    bool existing = !content.empty();
    if (content.empty()) content = readFileUtf8(in.dir + L"\\agent.ini.example");
    if (content.empty()) {
        content =
            "[api]\r\nbase_url=https://api.deepseek.com\r\nmodel=deepseek-chat\r\napi_key=\r\n"
            "extra_json=\r\nmax_tokens=8000\r\ntemperature=0.8\r\ntimeout_seconds=300\r\n"
            "retries=3\r\nproxy=\r\nproxy_mode=auto\r\n\r\n[agent]\r\ndays=7\r\n"
            "extend_history=1\r\nmax_input_chars=60000\r\nskill_dir=skill\r\n";
    }
    if (in.setKey) content = iniSet(content, "api", "api_key", wideToUtf8(in.key));
    if (in.setBase) content = iniSet(content, "api", "base_url", wideToUtf8(in.baseUrl));
    if (in.setModel) content = iniSet(content, "api", "model", wideToUtf8(in.model));
    if (!writeFileBytes(path, content, err)) return false;
    logMsg(existing ? L"已更新现有 agent.ini" : L"已生成 agent.ini");
    return true;
}

// ----------------------------------------------------------------------------
// 快捷方式与卸载登记
// ----------------------------------------------------------------------------

static bool createShortcut(const wstring& lnk, const wstring& target, const wstring& args,
                           const wstring& workDir, const wstring& desc, const wstring& icon,
                           wstring& err) {
    size_t slash = lnk.find_last_of(L"\\/");
    if (slash != wstring::npos) ensureDir(lnk.substr(0, slash), err);
    DeleteFileW(lnk.c_str());

    IShellLinkW* sl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                  (void**)&sl);
    if (FAILED(hr) || !sl) {
        wchar_t buf[64];
        swprintf(buf, 64, L"0x%08X", (unsigned)hr);
        err = L"无法创建快捷方式（" + wstring(buf) + L"）：\n" + lnk;
        return false;
    }
    sl->SetPath(target.c_str());
    sl->SetArguments(args.c_str());
    sl->SetWorkingDirectory(workDir.c_str());
    sl->SetDescription(desc.c_str());
    if (!icon.empty()) sl->SetIconLocation(icon.c_str(), 0);

    IPersistFile* pf = nullptr;
    hr = sl->QueryInterface(IID_IPersistFile, (void**)&pf);
    if (SUCCEEDED(hr) && pf) {
        hr = pf->Save(lnk.c_str(), TRUE);
        pf->Release();
    }
    sl->Release();
    if (FAILED(hr)) {
        wchar_t buf[64];
        swprintf(buf, 64, L"0x%08X", (unsigned)hr);
        err = L"无法保存快捷方式（" + wstring(buf) + L"）：\n" + lnk;
        return false;
    }
    return true;
}

static void createShortcuts(const InstallInput& in) {
    wstring exe = in.dir + L"\\news_desk.exe";
    wstring uninst = in.dir + L"\\uninstall.exe";
    wstring err;

    wstring desktop = knownFolder(FOLDERID_Desktop);
    if (!desktop.empty()) {
        wstring lnk = desktop + L"\\" + APP_NAME + L".lnk";
        DeleteFileW(lnk.c_str());
        if (in.desktop &&
            !createShortcut(lnk, exe, L"", in.dir, APP_NAME, exe, err))
            logMsg(L"桌面快捷方式失败：" + err);
    }

    wstring programs = knownFolder(FOLDERID_Programs);
    if (programs.empty()) {
        logMsg(L"找不到开始菜单目录，跳过快捷方式。");
        return;
    }
    wstring folder = programs + L"\\" + APP_NAME;
    if (!ensureDir(folder, err)) {
        logMsg(L"开始菜单文件夹失败：" + err);
        return;
    }

    if (!createShortcut(folder + L"\\" + APP_NAME + L".lnk", exe, L"", in.dir, APP_NAME, exe, err))
        logMsg(L"开始菜单快捷方式失败：" + err);
    if (!createShortcut(folder + L"\\使用说明.lnk", in.dir + L"\\使用说明.txt", L"", in.dir,
                        L"使用说明", L"", err))
        logMsg(L"使用说明快捷方式失败：" + err);

    wstring notepad = windowsDir() + L"\\notepad.exe";
    if (!createShortcut(folder + L"\\修改 API 密钥.lnk", notepad,
                        L"\"" + in.dir + L"\\agent.ini\"", in.dir, L"修改 API 密钥", notepad, err))
        logMsg(L"密钥快捷方式失败：" + err);
    if (!createShortcut(folder + L"\\卸载政策解读台.lnk", uninst, L"", in.dir,
                        L"卸载政策解读台", uninst, err))
        logMsg(L"卸载快捷方式失败：" + err);
}

static void registerUninstall(const wstring& dir, uint64_t totalBytes) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t date[32];
    swprintf(date, 32, L"%04d%02d%02d", st.wYear, st.wMonth, st.wDay);

    wstring quoted = L"\"" + dir + L"\\uninstall.exe\"";
    regSetStr(HKEY_CURRENT_USER, UNINSTALL_KEY, L"DisplayName", APP_NAME);
    regSetStr(HKEY_CURRENT_USER, UNINSTALL_KEY, L"DisplayVersion", APP_VERSION);
    regSetStr(HKEY_CURRENT_USER, UNINSTALL_KEY, L"Publisher", APP_PUBLISHER);
    regSetStr(HKEY_CURRENT_USER, UNINSTALL_KEY, L"DisplayIcon", dir + L"\\news_desk.exe,0");
    regSetStr(HKEY_CURRENT_USER, UNINSTALL_KEY, L"InstallLocation", dir);
    regSetStr(HKEY_CURRENT_USER, UNINSTALL_KEY, L"UninstallString", quoted);
    regSetStr(HKEY_CURRENT_USER, UNINSTALL_KEY, L"QuietUninstallString", quoted + L" /S");
    regSetStr(HKEY_CURRENT_USER, UNINSTALL_KEY, L"InstallDate", date);
    regSetDword(HKEY_CURRENT_USER, UNINSTALL_KEY, L"NoModify", 1);
    regSetDword(HKEY_CURRENT_USER, UNINSTALL_KEY, L"NoRepair", 1);
    regSetDword(HKEY_CURRENT_USER, UNINSTALL_KEY, L"EstimatedSize",
                (DWORD)std::min<uint64_t>(totalBytes / 1024 + 1, 0xFFFFFFFFull));
}

static wstring defaultInstallDir() {
    wstring loc = regGetStr(HKEY_CURRENT_USER, UNINSTALL_KEY, L"InstallLocation");
    if (!loc.empty() && dirExists(loc)) return normalizeDir(loc);
    wstring lad = knownFolder(FOLDERID_LocalAppData);
    if (!lad.empty()) return lad + L"\\Programs\\" APP_NAME;
    return L"C:\\" APP_NAME;
}

// ----------------------------------------------------------------------------
// 安装主流程
// ----------------------------------------------------------------------------

static bool doInstall(const InstallInput& in,
                      const std::function<void(int, int, const wstring&)>& progress, wstring& err) {
    logMsg(L"开始安装到 " + in.dir);

    if (progress) progress(0, 0, L"正在创建安装目录…");
    if (!ensureDir(in.dir, err)) return false;

    wstring probe = in.dir + L"\\.pd_write_test";
    HANDLE test = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (test == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        err = L"没有写入权限：\n" + in.dir + L"\n\n" + sysError(e) +
              L"\n\n请换一个位置；推荐使用默认的当前用户目录。";
        return false;
    }
    CloseHandle(test);
    DeleteFileW(probe.c_str());
    logMsg(L"目录可写");

    if (!extractPayload(in.dir, progress, err)) return false;

    if (progress) progress(2, 86, L"正在写入 API 配置…");
    if (!writeConfig(in, err)) return false;

    if (progress) progress(2, 90, L"正在准备稿件目录…");
    if (!ensureDir(in.dir + L"\\output", err)) return false;
    if (!ensureDir(in.dir + L"\\output\\logs", err)) return false;

    if (progress) progress(3, 93, L"正在创建快捷方式…");
    createShortcuts(in);

    if (progress) progress(4, 97, L"正在登记卸载信息…");
    uint64_t total = 0;
    {
        std::vector<PayloadEntry> entries;
        wstring tmp;
        if (loadPayload(entries, total, tmp)) {
        } else {
            total = 0;
        }
    }
    registerUninstall(in.dir, total);

    if (progress) progress(5, 100, L"安装完成");
    logMsg(L"安装成功");
    return true;
}

// ----------------------------------------------------------------------------
// 静默安装
// ----------------------------------------------------------------------------

static std::vector<wstring> commandArgs() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<wstring> out;
    if (argv) {
        for (int i = 0; i < argc; ++i) out.push_back(argv[i]);
        LocalFree(argv);
    }
    return out;
}

static bool ieq(const wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }

static bool optValue(const wstring& arg, const wchar_t* prefix, wstring& out) {
    size_t n = wcslen(prefix);
    if (_wcsnicmp(arg.c_str(), prefix, n) != 0) return false;
    out = arg.substr(n);
    return true;
}

static int runSilent(const std::vector<wstring>& args) {
    InstallInput in;
    in.dir = defaultInstallDir();

    for (const wstring& a : args) {
        wstring v;
        if (optValue(a, L"/D=", v) || optValue(a, L"--dir=", v))
            in.dir = trim(v);
        else if (optValue(a, L"/KEY=", v) || optValue(a, L"--key=", v)) {
            in.key = trim(v);
            in.setKey = true;
        } else if (optValue(a, L"/BASE=", v) || optValue(a, L"--base=", v)) {
            in.baseUrl = trim(v);
            in.setBase = true;
        } else if (optValue(a, L"/MODEL=", v) || optValue(a, L"--model=", v)) {
            in.model = trim(v);
            in.setModel = true;
        } else if (ieq(a, L"/NODESKTOP") || ieq(a, L"--no-desktop")) {
            in.desktop = false;
        }
    }

    in.dir = normalizeDir(expandEnv(in.dir));
    if (in.dir.empty()) in.dir = defaultInstallDir();
    if (!isAbsolutePath(in.dir)) {
        wchar_t buf[32768] = {};
        DWORD n = GetFullPathNameW(in.dir.c_str(), 32768, buf, nullptr);
        if (n > 0 && n < 32768) in.dir = wstring(buf, n);
    }

    logMsg(L"静默安装：" + in.dir);
    wstring err;
    bool ok = false;
    try {
        ok = doInstall(in, nullptr, err);
    } catch (...) {
        err = L"安装过程出现异常。";
    }
    logMsg(ok ? L"静默安装成功" : (L"静默安装失败：" + err));
    return ok ? 0 : 1;
}

// ----------------------------------------------------------------------------
// 自绘
// ----------------------------------------------------------------------------

static void addRoundRect(GraphicsPath& p, const RectF& r, float rad) {
    float d = rad * 2;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

static void fillRound(Graphics& g, const RectF& r, float rad, const Color& fill,
                      const Color& border = Color(0, 0, 0, 0), float borderW = 1.0f) {
    GraphicsPath p;
    addRoundRect(p, r, rad);
    if (fill.GetA() > 0) {
        SolidBrush b(fill);
        g.FillPath(&b, &p);
    }
    if (border.GetA() > 0) {
        Pen pen(border, borderW);
        g.DrawPath(&pen, &p);
    }
}

static void drawText(Graphics& g, const wstring& s, Font* f, const Color& c, const RectF& r,
                     StringAlignment halign = StringAlignmentNear,
                     StringAlignment valign = StringAlignmentNear, bool wrap = false,
                     StringTrimming trimMode = StringTrimmingEllipsisCharacter) {
    if (!f) return;
    StringFormat fmt;
    fmt.SetAlignment(halign);
    fmt.SetLineAlignment(valign);
    fmt.SetTrimming(trimMode);
    fmt.SetFormatFlags(wrap ? 0 : StringFormatFlagsNoWrap);
    SolidBrush b(c);
    g.DrawString(s.c_str(), (INT)s.size(), f, r, &fmt, &b);
}

static void checkMark(Graphics& g, float cx, float cy, float r, const Color& c, float width) {
    Pen pen(c, width);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, cx - r * 0.45f, cy + r * 0.02f, cx - r * 0.08f, cy + r * 0.36f);
    g.DrawLine(&pen, cx - r * 0.08f, cy + r * 0.36f, cx + r * 0.48f, cy - r * 0.35f);
}

static void paintHeader(Graphics& g, int W) {
    LinearGradientBrush bg(RectF(0, 0, (REAL)W, (REAL)px(92)), C::HEAD1, C::HEAD2,
                           LinearGradientModeHorizontal);
    g.FillRectangle(&bg, 0, 0, W, px(92));

    RectF logo((REAL)px(44), (REAL)px(23), (REAL)px(44), (REAL)px(44));
    fillRound(g, logo, (float)px(11), Color(255, 255, 255, 255));
    drawText(g, L"解", g_logo, C::ACCENT, logo, StringAlignmentCenter, StringAlignmentCenter);

    drawText(g, APP_NAME, g_h1, Color(255, 255, 255, 255),
             RectF((REAL)px(102), (REAL)px(18), (REAL)px(330), (REAL)px(34)));
    drawText(g, L"一键安装程序 · v" APP_VERSION, g_small, Color(215, 255, 255, 255),
             RectF((REAL)px(104), (REAL)px(52), (REAL)px(300), (REAL)px(20)));

    int active = (g_page == PAGE_WELCOME) ? 0 : (g_page == PAGE_KEY ? 1 : 2);
    const wchar_t* labels[3] = {L"安装位置", L"API 密钥", L"完成"};
    float cx[3] = {(float)(W - px(284)), (float)(W - px(188)), (float)(W - px(92))};
    float cy = (float)px(32), r = (float)px(12);

    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            Pen line(i <= active ? Color(170, 255, 255, 255) : Color(80, 255, 255, 255), 1.0f);
            g.DrawLine(&line, cx[i - 1] + r + px(6), cy, cx[i] - r - px(6), cy);
        }
        if (i < active) {
            SolidBrush b(Color(255, 255, 255, 255));
            g.FillEllipse(&b, cx[i] - r, cy - r, r * 2, r * 2);
            checkMark(g, cx[i], cy, r * 0.72f, C::ACCENT, (float)px(2));
        } else if (i == active) {
            SolidBrush b(Color(255, 255, 255, 255));
            g.FillEllipse(&b, cx[i] - r, cy - r, r * 2, r * 2);
            drawText(g, std::to_wstring(i + 1), g_small, C::ACCENT,
                     RectF(cx[i] - r, cy - r, r * 2, r * 2), StringAlignmentCenter,
                     StringAlignmentCenter);
        } else {
            SolidBrush b(Color(40, 255, 255, 255));
            g.FillEllipse(&b, cx[i] - r, cy - r, r * 2, r * 2);
            Pen pen(Color(125, 255, 255, 255), 1.0f);
            g.DrawEllipse(&pen, cx[i] - r, cy - r, r * 2, r * 2);
            drawText(g, std::to_wstring(i + 1), g_small, Color(185, 255, 255, 255),
                     RectF(cx[i] - r, cy - r, r * 2, r * 2), StringAlignmentCenter,
                     StringAlignmentCenter);
        }
        drawText(g, labels[i], g_small,
                 i == active ? Color(255, 255, 255, 255) : Color(170, 255, 255, 255),
                 RectF(cx[i] - px(50), (REAL)px(50), (REAL)px(100), (REAL)px(18)),
                 StringAlignmentCenter);
    }
}

static void paintWelcome(Graphics& g, int W) {
    drawText(g, L"欢迎使用政策解读台", g_h1, C::INK,
             RectF((REAL)px(48), (REAL)px(120), (REAL)(W - px(96)), (REAL)px(40)));
    drawText(g, L"自动获取《新闻联播》《人民日报》，用 AI 生成政策解读。", g_body, C::INK,
             RectF((REAL)px(48), (REAL)px(168), (REAL)(W - px(96)), (REAL)px(22)));
    drawText(g, L"安装包已包含全部运行文件，不需要 Python、Node 或编译器。", g_body, C::MUTED,
             RectF((REAL)px(48), (REAL)px(194), (REAL)(W - px(96)), (REAL)px(22)));

    RectF card((REAL)px(48), (REAL)px(236), (REAL)(W - px(96)), (REAL)px(190));
    fillRound(g, card, (float)px(10), C::CARD, C::BORDER);
    drawText(g, L"安装位置", g_label, C::INK,
             RectF(card.X + px(24), card.Y + px(16), card.Width - px(48), (REAL)px(20)));
    drawText(g, L"建议使用默认位置；安装到 C:\\Program Files 需要管理员权限。", g_small, C::FAINT,
             RectF(card.X + px(24), card.Y + px(88), card.Width - px(48), (REAL)px(20)));
    drawText(g, L"约 10 MB 磁盘空间。", g_small, C::FAINT,
             RectF(card.X + px(24), card.Y + px(150), card.Width - px(48), (REAL)px(20)));

    float by = card.Y + px(222);
    const wchar_t* bullets[3] = {L"• 主界面：一键获取稿件、发起解读、查看结果",
                                 L"• 内置解读 Agent 与方法论，不依赖外部运行环境",
                                 L"• 首次解读前，请准备 DeepSeek API 密钥"};
    for (int i = 0; i < 3; ++i) {
        drawText(g, bullets[i], g_body, i == 2 ? C::MUTED : C::INK,
                 RectF((REAL)px(48), by + px(24) * i, (REAL)(W - px(96)), (REAL)px(22)));
    }
}

static void paintKey(Graphics& g, int W) {
    drawText(g, L"填写 API 密钥", g_h1, C::INK,
             RectF((REAL)px(48), (REAL)px(120), (REAL)(W - px(96)), (REAL)px(40)));
    drawText(g, L"解读功能通过 DeepSeek 的 OpenAI 兼容接口生成报告。", g_body, C::INK,
             RectF((REAL)px(48), (REAL)px(168), (REAL)(W - px(96)), (REAL)px(22)));
    drawText(g, L"密钥只写入本机 agent.ini，不会上传到任何其他位置。", g_body, C::MUTED,
             RectF((REAL)px(48), (REAL)px(194), (REAL)(W - px(96)), (REAL)px(22)));
    drawText(g, L"API 密钥", g_label, C::INK,
             RectF((REAL)px(48), (REAL)px(232), (REAL)px(200), (REAL)px(20)));

    Pen line(C::BORDER, 1.0f);
    g.DrawLine(&line, (REAL)px(48), (REAL)px(360), (REAL)(W - px(48)), (REAL)px(360));

    if (g_advanced) {
        drawText(g, L"接口地址（base_url）", g_small, C::MUTED,
                 RectF((REAL)px(48), (REAL)px(402), (REAL)px(300), (REAL)px(18)));
        int modelX = W - px(48) - px(196);
        drawText(g, L"模型（model）", g_small, C::MUTED,
                 RectF((REAL)modelX, (REAL)px(402), (REAL)px(196), (REAL)px(18)));
        drawText(g, L"仅支持 OpenAI 兼容的 Chat Completions 端点；DeepSeek 官方端点已实测。",
                 g_small, C::FAINT,
                 RectF((REAL)px(48), (REAL)px(472), (REAL)(W - px(96)), (REAL)px(40)),
                 StringAlignmentNear, StringAlignmentNear, true);
    } else {
        drawText(g,
                 L"可以跳过：安装后从开始菜单打开「修改 API 密钥」补填。未填密钥时点"
                 L"「开始解读」只会提示，不会发请求。",
                 g_small, C::FAINT,
                 RectF((REAL)px(48), (REAL)px(408), (REAL)(W - px(96)), (REAL)px(56)),
                 StringAlignmentNear, StringAlignmentNear, true);
    }
}

static void paintInstall(Graphics& g, int W) {
    drawText(g, L"正在安装", g_h1, C::INK,
             RectF((REAL)px(48), (REAL)px(120), (REAL)(W - px(96)), (REAL)px(40)));
    drawText(g, g_status.empty() ? wstring(L"准备安装…") : g_status, g_body, C::INK,
             RectF((REAL)px(48), (REAL)px(168), (REAL)(W - px(140)), (REAL)px(22)));
    drawText(g, std::to_wstring(g_progress) + L"%", g_small, C::MUTED,
             RectF((REAL)(W - px(140)), (REAL)px(168), (REAL)px(92), (REAL)px(20)),
             StringAlignmentFar);

    RectF track((REAL)px(48), (REAL)px(204), (REAL)(W - px(96)), (REAL)px(10));
    fillRound(g, track, (float)px(5), Color(255, 237, 231, 223));
    if (g_progress > 0) {
        float fw = track.Width * g_progress / 100.0f;
        if (fw < px(6)) fw = (float)px(6);
        fillRound(g, RectF(track.X, track.Y, fw, track.Height),
                  std::min((float)px(5), fw / 2), C::ACCENT);
    }

    RectF card((REAL)px(48), (REAL)px(252), (REAL)(W - px(96)), (REAL)px(252));
    fillRound(g, card, (float)px(10), C::CARD, C::BORDER);

    const wchar_t* names[5] = {L"创建安装目录", L"释放程序文件", L"写入 API 配置",
                               L"创建快捷方式", L"注册卸载信息"};
    for (int i = 0; i < 5; ++i) {
        float rowY = card.Y + px(18) + px(44) * i;
        float cx = card.X + px(30);
        float cy = rowY + px(15);
        float r = (float)px(10);
        bool done = i < g_step;
        bool active = i == g_step && g_installing;

        if (done) {
            SolidBrush b(C::OK);
            g.FillEllipse(&b, cx - r, cy - r, r * 2, r * 2);
            checkMark(g, cx, cy, r * 0.72f, Color(255, 255, 255, 255), (float)px(2));
        } else if (active) {
            SolidBrush b(C::ACCENT);
            g.FillEllipse(&b, cx - r, cy - r, r * 2, r * 2);
            drawText(g, std::to_wstring(i + 1), g_small, Color(255, 255, 255, 255),
                     RectF(cx - r, cy - r, r * 2, r * 2), StringAlignmentCenter,
                     StringAlignmentCenter);
        } else {
            SolidBrush b(C::ROW_DONE);
            g.FillEllipse(&b, cx - r, cy - r, r * 2, r * 2);
            drawText(g, std::to_wstring(i + 1), g_small, C::FAINT,
                     RectF(cx - r, cy - r, r * 2, r * 2), StringAlignmentCenter,
                     StringAlignmentCenter);
        }

        drawText(g, names[i], g_body, done ? C::MUTED : C::INK,
                 RectF(card.X + px(52), rowY, card.Width - px(140), (REAL)px(28)));
        if (done) {
            drawText(g, L"完成", g_small, C::OK,
                     RectF(card.X + card.Width - px(92), rowY, (REAL)px(68), (REAL)px(28)),
                     StringAlignmentFar);
        } else if (active) {
            drawText(g, L"进行中", g_small, C::ACCENT,
                     RectF(card.X + card.Width - px(92), rowY, (REAL)px(68), (REAL)px(28)),
                     StringAlignmentFar);
        }
        if (i < 4) {
            Pen sep(Color(255, 243, 239, 233), 1.0f);
            g.DrawLine(&sep, card.X + px(52), rowY + px(36), card.X + card.Width - px(20),
                       rowY + px(36));
        }
    }

    drawText(g, L"安装完成后会自动创建桌面与开始菜单快捷方式。", g_small, C::FAINT,
             RectF((REAL)px(48), (REAL)px(518), (REAL)(W - px(96)), (REAL)px(20)),
             StringAlignmentCenter);
}

static void paintDone(Graphics& g, int W) {
    float cx = W / 2.0f;
    float cy = (float)px(176);
    float r = (float)px(34);
    SolidBrush green(C::OK);
    g.FillEllipse(&green, cx - r, cy - r, r * 2, r * 2);
    checkMark(g, cx, cy, r * 0.9f, Color(255, 255, 255, 255), (float)px(4));

    drawText(g, L"安装完成", g_h1, C::INK,
             RectF(cx - px(240), (REAL)px(220), (REAL)px(480), (REAL)px(42)),
             StringAlignmentCenter);
    drawText(g, L"政策解读台已安装到：", g_small, C::MUTED,
             RectF(cx - px(300), (REAL)px(266), (REAL)px(600), (REAL)px(20)),
             StringAlignmentCenter);
    drawText(g, g_dir, g_body, C::INK,
             RectF(cx - px(320), (REAL)px(286), (REAL)px(640), (REAL)px(24)),
             StringAlignmentCenter, StringAlignmentNear, false, StringTrimmingEllipsisPath);

    if (g_key.empty()) {
        drawText(g, L"尚未填写 API 密钥：可从开始菜单打开「修改 API 密钥」补填。", g_small,
                 C::ACCENT,
                 RectF(cx - px(320), (REAL)px(320), (REAL)px(640), (REAL)px(22)),
                 StringAlignmentCenter);
    } else {
        drawText(g, L"API 密钥已写入 agent.ini，可以直接开始解读。", g_small, C::OK,
                 RectF(cx - px(320), (REAL)px(320), (REAL)px(640), (REAL)px(22)),
                 StringAlignmentCenter);
    }
    drawText(g, L"也可以稍后从桌面快捷方式启动。", g_small, C::FAINT,
             RectF(cx - px(300), (REAL)px(398), (REAL)px(600), (REAL)px(20)),
             StringAlignmentCenter);
}

static void paintFooter(Graphics& g, int W, int H) {
    int fy = H - px(74);
    SolidBrush white(Color(255, 255, 255, 255));
    g.FillRectangle(&white, 0, fy, W, px(74));
    Pen line(C::BORDER, 1.0f);
    g.DrawLine(&line, (REAL)0, (REAL)fy, (REAL)W, (REAL)fy);

    wstring note;
    if (g_page == PAGE_WELCOME)
        note = L"安装到当前用户目录，无需管理员权限";
    else if (g_page == PAGE_KEY)
        note = L"密钥只保存在本机 agent.ini，不会上传";
    else if (g_page == PAGE_INSTALL)
        note = L"正在写入文件，请勿关闭窗口";
    else
        note = L"感谢使用 · MIT License";
    drawText(g, note, g_small, C::FAINT,
             RectF((REAL)px(48), (REAL)(fy + px(27)), (REAL)(W - px(380)), (REAL)px(20)));
}

static void paintAll(Graphics& g, int W, int H) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    SolidBrush bg(C::BG);
    g.FillRectangle(&bg, 0, 0, W, H);

    paintHeader(g, W);
    switch (g_page) {
        case PAGE_WELCOME:
            paintWelcome(g, W);
            break;
        case PAGE_KEY:
            paintKey(g, W);
            break;
        case PAGE_INSTALL:
            paintInstall(g, W);
            break;
        case PAGE_DONE:
            paintDone(g, W);
            break;
    }
    paintFooter(g, W, H);
}

// ----------------------------------------------------------------------------
// 字体与控件
// ----------------------------------------------------------------------------

static void destroyFonts() {
    delete g_logo;
    g_logo = nullptr;
    delete g_btn;
    g_btn = nullptr;
    delete g_label;
    g_label = nullptr;
    delete g_small;
    g_small = nullptr;
    delete g_body;
    g_body = nullptr;
    delete g_h1;
    g_h1 = nullptr;
    delete g_family;
    g_family = nullptr;
}

static void createFonts() {
    destroyFonts();
    g_family = new FontFamily(L"Microsoft YaHei UI");
    if (g_family->GetLastStatus() != Ok) {
        delete g_family;
        g_family = new FontFamily(L"Microsoft YaHei");
    }
    g_h1 = new Font(g_family, (REAL)px(27), FontStyleBold, UnitPixel);
    g_body = new Font(g_family, (REAL)px(14), FontStyleRegular, UnitPixel);
    g_small = new Font(g_family, (REAL)px(12), FontStyleRegular, UnitPixel);
    g_label = new Font(g_family, (REAL)px(13), FontStyleBold, UnitPixel);
    g_btn = new Font(g_family, (REAL)px(15), FontStyleBold, UnitPixel);
    g_logo = new Font(g_family, (REAL)px(23), FontStyleBold, UnitPixel);
}

static void createUiFonts() {
    if (g_hfEdit) DeleteObject(g_hfEdit);
    if (g_hfCheck) DeleteObject(g_hfCheck);
    g_hfEdit = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_hfCheck = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH, L"Microsoft YaHei UI");
}

static void applyControlFonts() {
    HWND edits[] = {g_editPath, g_editKey, g_editBase, g_editModel};
    for (HWND h : edits)
        if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g_hfEdit, TRUE);
    HWND checks[] = {g_chkDesktop, g_chkShow, g_chkAdvanced, g_chkRun};
    for (HWND h : checks)
        if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g_hfCheck, TRUE);
}

static wstring getText(HWND h) {
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return {};
    wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}

static HWND makeEdit(UINT id, DWORD extraStyle) {
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extraStyle, 0,
                             0, 10, 10, g_hwnd, (HMENU)(UINT_PTR)id, g_hInst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_hfEdit, TRUE);
    SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(8), px(8)));
    SetWindowSubclass(h, EditSubclass, id, 0);
    return h;
}

static HWND makeCheck(UINT id, const wchar_t* text) {
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10, 10,
                             g_hwnd, (HMENU)(UINT_PTR)id, g_hInst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_hfCheck, TRUE);
    return h;
}

static HWND makeButton(UINT id, const wchar_t* text) {
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                             g_hwnd, (HMENU)(UINT_PTR)id, g_hInst, nullptr);
    SetWindowSubclass(h, BtnSubclass, id, 0);
    return h;
}

static void loadExistingConfig() {
    g_dir = defaultInstallDir();
    string cfg = readFileUtf8(g_dir + L"\\agent.ini");
    if (!cfg.empty()) {
        string k = iniGet(cfg, "api_key");
        string b = iniGet(cfg, "base_url");
        string m = iniGet(cfg, "model");
        if (!k.empty()) g_key = utf8ToWide(k);
        if (!b.empty()) g_base = utf8ToWide(b);
        if (!m.empty()) g_model = utf8ToWide(m);
    }
    if (g_base.empty()) g_base = kDefaultBase;
    if (g_model.empty()) g_model = kDefaultModel;
    g_advanced = (g_base != kDefaultBase || g_model != kDefaultModel);

    SetWindowTextW(g_editPath, g_dir.c_str());
    SetWindowTextW(g_editKey, g_key.c_str());
    SetWindowTextW(g_editBase, g_base.c_str());
    SetWindowTextW(g_editModel, g_model.c_str());
}

static void createUi() {
    createFonts();
    createUiFonts();
    g_brushBg = CreateSolidBrush(RGB(250, 248, 245));
    g_brushWhite = CreateSolidBrush(RGB(255, 255, 255));

    g_editPath = makeEdit(ID_EDIT_PATH, 0);
    g_editKey = makeEdit(ID_EDIT_KEY, ES_PASSWORD);
    g_editBase = makeEdit(ID_EDIT_BASE, 0);
    g_editModel = makeEdit(ID_EDIT_MODEL, 0);

    g_chkDesktop = makeCheck(ID_CHK_DESKTOP, L"创建桌面快捷方式（推荐）");
    g_chkShow = makeCheck(ID_CHK_SHOWKEY, L"显示");
    g_chkAdvanced = makeCheck(ID_CHK_ADVANCED, L"使用其他 OpenAI 兼容端点（高级）");
    g_chkRun = makeCheck(ID_CHK_RUN, L"立即运行政策解读台");

    g_btnBrowse = makeButton(ID_BTN_BROWSE, L"浏览…");
    g_btnPrimary = makeButton(ID_BTN_PRIMARY, L"下一步");
    g_btnSecondary = makeButton(ID_BTN_SECONDARY, L"取消");
    g_btnLink = makeButton(ID_BTN_LINK, L"还没有密钥？打开 platform.deepseek.com 获取 →");

    SendMessageW(g_editKey, EM_SETPASSWORDCHAR, (WPARAM)L'\u25CF', 0);
    SendMessageW(g_editKey, EM_SETCUEBANNER, TRUE, (LPARAM)L"sk-...");
    SendMessageW(g_editBase, EM_SETCUEBANNER, TRUE, (LPARAM)L"https://api.deepseek.com");
    SendMessageW(g_editModel, EM_SETCUEBANNER, TRUE, (LPARAM)L"deepseek-chat");

    SendMessageW(g_chkDesktop, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(g_chkRun, BM_SETCHECK, BST_CHECKED, 0);

    loadExistingConfig();
    if (g_advanced) SendMessageW(g_chkAdvanced, BM_SETCHECK, BST_CHECKED, 0);
    applyControlFonts();
    g_page = PAGE_WELCOME;
    updatePage();
}

static void layoutControls() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    int cardX = px(48);
    int cardY = px(236);
    int cardW = W - px(96);
    int editY = cardY + px(44);
    int browseW = px(100);
    int pathW = cardW - px(48) - browseW - px(12);
    MoveWindow(g_editPath, cardX + px(24), editY, pathW, px(38), TRUE);
    MoveWindow(g_btnBrowse, cardX + cardW - px(24) - browseW, editY, browseW, px(38), TRUE);
    MoveWindow(g_chkDesktop, cardX + px(24), cardY + px(118), cardW - px(48), px(24), TRUE);

    int keyX = px(48);
    int keyY = px(256);
    int keyH = px(38);
    int showW = px(112);
    int keyW = W - px(96) - showW - px(12);
    MoveWindow(g_editKey, keyX, keyY, keyW, keyH, TRUE);
    MoveWindow(g_chkShow, keyX + keyW + px(12), keyY + px(6), showW, px(24), TRUE);
    MoveWindow(g_btnLink, px(48), px(304), px(470), px(26), TRUE);
    MoveWindow(g_chkAdvanced, px(48), px(370), px(400), px(24), TRUE);

    int modelW = px(196);
    int baseW = W - px(96) - modelW - px(16);
    MoveWindow(g_editBase, px(48), px(424), baseW, px(36), TRUE);
    MoveWindow(g_editModel, W - px(48) - modelW, px(424), modelW, px(36), TRUE);

    int runW = px(300);
    MoveWindow(g_chkRun, (W - runW) / 2, px(360), runW, px(24), TRUE);

    int bh = px(40);
    int by = H - px(57);
    int pw = px(150);
    int primaryX = W - px(48) - pw;
    MoveWindow(g_btnPrimary, primaryX, by, pw, bh, TRUE);
    int sw = (g_page == PAGE_DONE) ? px(164) : px(130);
    MoveWindow(g_btnSecondary, primaryX - px(12) - sw, by, sw, bh, TRUE);
}

static void setChecked(HWND h, bool on) {
    SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void updatePage() {
    bool welcome = g_page == PAGE_WELCOME;
    bool key = g_page == PAGE_KEY;
    bool done = g_page == PAGE_DONE;

    ShowWindow(g_editPath, welcome ? SW_SHOW : SW_HIDE);
    ShowWindow(g_btnBrowse, welcome ? SW_SHOW : SW_HIDE);
    ShowWindow(g_chkDesktop, welcome ? SW_SHOW : SW_HIDE);

    ShowWindow(g_editKey, key ? SW_SHOW : SW_HIDE);
    ShowWindow(g_chkShow, key ? SW_SHOW : SW_HIDE);
    ShowWindow(g_btnLink, key ? SW_SHOW : SW_HIDE);
    ShowWindow(g_chkAdvanced, key ? SW_SHOW : SW_HIDE);
    ShowWindow(g_editBase, key && g_advanced ? SW_SHOW : SW_HIDE);
    ShowWindow(g_editModel, key && g_advanced ? SW_SHOW : SW_HIDE);

    ShowWindow(g_chkRun, done ? SW_SHOW : SW_HIDE);

    bool buttons = g_page != PAGE_INSTALL;
    ShowWindow(g_btnPrimary, buttons ? SW_SHOW : SW_HIDE);
    ShowWindow(g_btnSecondary, buttons ? SW_SHOW : SW_HIDE);

    if (welcome) {
        SetWindowTextW(g_btnPrimary, L"下一步");
        SetWindowTextW(g_btnSecondary, L"取消");
    } else if (key) {
        SetWindowTextW(g_btnPrimary, L"开始安装");
        SetWindowTextW(g_btnSecondary, L"上一步");
    } else if (done) {
        SetWindowTextW(g_btnPrimary, L"完成");
        SetWindowTextW(g_btnSecondary, L"打开安装文件夹");
    }

    setChecked(g_chkDesktop, g_desktop);
    setChecked(g_chkShow, g_showKey);
    setChecked(g_chkAdvanced, g_advanced);
    setChecked(g_chkRun, g_run);

    layoutControls();
    InvalidateRect(g_hwnd, nullptr, FALSE);

    if (welcome)
        SetFocus(g_editPath);
    else if (key) {
        SetFocus(g_editKey);
        if (!g_key.empty()) SendMessageW(g_editKey, EM_SETSEL, 0, -1);
    } else if (done)
        SetFocus(g_btnPrimary);
}

// ----------------------------------------------------------------------------
// 按钮子类化与绘制
// ----------------------------------------------------------------------------

static LRESULT CALLBACK EditSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                     DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(ID_BTN_PRIMARY, BN_CLICKED), 0);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        PostMessageW(GetParent(h), WM_APP_ESCAPE, 0, 0);
        return 0;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static LRESULT CALLBACK BtnSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                    DWORD_PTR) {
    if (msg == WM_MOUSEMOVE) {
        if (!g_hover[id]) {
            g_hover[id] = true;
            InvalidateRect(h, nullptr, TRUE);
        }
        TRACKMOUSEEVENT tme;
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = h;
        tme.dwHoverTime = 0;
        TrackMouseEvent(&tme);
    } else if (msg == WM_MOUSELEAVE) {
        if (g_hover[id]) {
            g_hover[id] = false;
            InvalidateRect(h, nullptr, TRUE);
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static int buttonKind(UINT id) {
    if (id == ID_BTN_PRIMARY) return 0;
    if (id == ID_BTN_LINK) return 2;
    return 1;
}

static void drawOwnerButton(DRAWITEMSTRUCT* d) {
    int kind = buttonKind(d->CtlID);
    bool disabled = (d->itemState & ODS_DISABLED) != 0;
    bool pressed = (d->itemState & ODS_SELECTED) != 0;
    bool hover = g_hover[d->CtlID];

    RECT r = d->rcItem;
    Graphics g(d->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    SolidBrush clear(kind == 2 ? C::BG : Color(255, 255, 255, 255));
    g.FillRectangle(&clear, (INT)r.left, (INT)r.top, (INT)(r.right - r.left),
                    (INT)(r.bottom - r.top));

    RectF box((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top));
    wchar_t text[256] = {};
    GetWindowTextW(d->hwndItem, text, 256);

    Color fill, border, fg;
    if (kind == 0) {
        fill = disabled ? C::DISABLED_BG : (pressed ? C::ACCENT_DOWN : (hover ? C::ACCENT_HOVER : C::ACCENT));
        border = fill;
        fg = disabled ? C::DISABLED_FG : Color(255, 255, 255, 255);
        fillRound(g, RectF(box.X + 0.5f, box.Y + 0.5f, box.Width - 1, box.Height - 1),
                  (float)px(7), fill, border);
        drawText(g, text, g_btn, fg, box, StringAlignmentCenter, StringAlignmentCenter);
    } else if (kind == 1) {
        fill = disabled ? C::DISABLED_BG : (pressed ? Color(255, 235, 230, 223)
                                                    : (hover ? C::GHOST_HOVER : C::CARD));
        border = C::BORDER;
        fg = disabled ? C::DISABLED_FG : C::INK;
        fillRound(g, RectF(box.X + 0.5f, box.Y + 0.5f, box.Width - 1, box.Height - 1),
                  (float)px(7), fill, border);
        drawText(g, text, g_body, fg, box, StringAlignmentCenter, StringAlignmentCenter);
    } else {
        fg = hover ? C::ACCENT_HOVER : C::ACCENT;
        drawText(g, text, g_body, fg,
                 RectF(box.X + px(2), box.Y, box.Width - px(4), box.Height),
                 StringAlignmentNear, StringAlignmentCenter);
    }
}

// ----------------------------------------------------------------------------
// 交互
// ----------------------------------------------------------------------------

static void message(const wstring& text, UINT flags = MB_OK | MB_ICONINFORMATION) {
    MessageBoxW(g_hwnd, text.c_str(), APP_NAME, flags);
}

static void browseFolder() {
    IFileDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFileDialog, (void**)&dlg);
    if (FAILED(hr) || !dlg) {
        message(L"无法打开文件夹选择窗口。");
        return;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                SetWindowTextW(g_editPath, p);
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
}

static bool checkWritable(const wstring& dir, wstring& err) {
    if (!ensureDir(dir, err)) return false;
    wstring probe = dir + L"\\.pd_write_test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        err = L"该位置无法写入：\n" + dir + L"\n\n" + sysError(e) +
              L"\n\n请换一个位置；推荐使用默认的当前用户目录。";
        return false;
    }
    CloseHandle(h);
    DeleteFileW(probe.c_str());
    return true;
}

static void handleWelcomeNext() {
    wstring p = trim(getText(g_editPath));
    if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"') p = p.substr(1, p.size() - 2);
    p = trim(expandEnv(p));
    if (p.empty()) {
        message(L"请填写安装位置。");
        return;
    }
    if (!isAbsolutePath(p)) {
        message(L"请填写完整路径，例如：\nC:\\Users\\你的用户名\\AppData\\Local\\Programs\\政策解读台");
        return;
    }
    p = normalizeDir(p);
    wstring err;
    if (!checkWritable(p, err)) {
        message(err, MB_OK | MB_ICONERROR);
        return;
    }
    if (!fileExists(p + L"\\news_desk.exe") && dirHasEntries(p)) {
        if (MessageBoxW(g_hwnd,
                        L"所选目录不是空的。继续安装会覆盖同名文件，其他文件保持不变。\n\n仍要"
                        L"安装到这个目录吗？",
                        APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }
    g_dir = p;
    g_page = PAGE_KEY;
    updatePage();
}

static DWORD WINAPI InstallThreadProc(LPVOID param) {
    InstallInput* input = (InstallInput*)param;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wstring err;
    bool ok = false;
    try {
        ok = doInstall(*input,
                       [](int step, int pct, const wstring& text) {
                           if (g_hwnd)
                               PostMessageW(g_hwnd, WM_APP_PROGRESS, MAKEWPARAM(step, pct),
                                            (LPARAM)new wstring(text));
                       },
                       err);
    } catch (...) {
        err = L"安装过程出现异常。";
        ok = false;
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    delete input;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, ok ? 1 : 0, (LPARAM)new wstring(err));
    return 0;
}

static void startInstall() {
    auto* in = new InstallInput;
    in->dir = g_dir;
    in->key = g_key;
    in->setKey = true;
    in->baseUrl = g_advanced ? g_base : wstring(kDefaultBase);
    in->model = g_advanced ? g_model : wstring(kDefaultModel);
    in->setBase = true;
    in->setModel = true;
    in->desktop = g_desktop;

    g_installing = true;
    g_installOk = false;
    g_step = 0;
    g_progress = 0;
    g_status = L"准备安装…";
    g_page = PAGE_INSTALL;
    updatePage();

    HANDLE th = CreateThread(nullptr, 0, InstallThreadProc, in, 0, nullptr);
    if (th) {
        CloseHandle(th);
    } else {
        delete in;
        g_installing = false;
        g_page = PAGE_KEY;
        updatePage();
        message(L"无法启动安装线程。", MB_OK | MB_ICONERROR);
    }
}

static void handleKeyNext() {
    wstring key = trim(getText(g_editKey));
    wstring base = trim(getText(g_editBase));
    wstring model = trim(getText(g_editModel));

    if (g_advanced) {
        if (base.empty()) {
            message(L"请填写接口地址（base_url）。");
            SetFocus(g_editBase);
            return;
        }
        if (_wcsnicmp(base.c_str(), L"http://", 7) != 0 &&
            _wcsnicmp(base.c_str(), L"https://", 8) != 0) {
            message(L"接口地址需要以 http:// 或 https:// 开头。");
            SetFocus(g_editBase);
            return;
        }
        if (model.empty()) {
            message(L"请填写模型名称（model）。");
            SetFocus(g_editModel);
            return;
        }
    } else {
        base = kDefaultBase;
        model = kDefaultModel;
    }

    if (key.empty()) {
        if (MessageBoxW(g_hwnd,
                        L"还没有填写 API 密钥。\n\n安装后仍可从开始菜单打开「修改 API 密钥」"
                        L"补填；未填密钥时「开始解读」只会提示，不会发请求。\n\n仍要继续安装吗？",
                        APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }

    g_key = key;
    g_base = base;
    g_model = model;
    g_installing = false;
    startInstall();
}

static void finishInstall() {
    if (g_run && g_installOk) {
        wstring exe = g_dir + L"\\news_desk.exe";
        ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, g_dir.c_str(), SW_SHOWNORMAL);
    }
    DestroyWindow(g_hwnd);
}

static void handlePrimary() {
    if (g_page == PAGE_WELCOME) {
        handleWelcomeNext();
    } else if (g_page == PAGE_KEY) {
        handleKeyNext();
    } else if (g_page == PAGE_DONE) {
        finishInstall();
    }
}

static void handleSecondary() {
    if (g_page == PAGE_WELCOME) {
        DestroyWindow(g_hwnd);
    } else if (g_page == PAGE_KEY) {
        g_page = PAGE_WELCOME;
        updatePage();
    } else if (g_page == PAGE_DONE) {
        ShellExecuteW(nullptr, L"open", g_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ----------------------------------------------------------------------------
// 窗口过程
// ----------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            createUi();
            return 0;

        case WM_SIZE:
            layoutControls();
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (rc.right <= 0 || rc.bottom <= 0) {
                EndPaint(hwnd, &ps);
                return 0;
            }
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            {
                Graphics g(mem);
                paintAll(g, rc.right, rc.bottom);
            }
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, RGB(27, 27, 27));
            SetBkColor(hdc, RGB(255, 255, 255));
            return (LRESULT)g_brushWhite;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wp;
            if (GetDlgCtrlID((HWND)lp) == ID_CHK_DESKTOP) {
                SetTextColor(hdc, RGB(27, 27, 27));
                SetBkColor(hdc, RGB(255, 255, 255));
                return (LRESULT)g_brushWhite;
            }
            SetTextColor(hdc, RGB(27, 27, 27));
            SetBkColor(hdc, RGB(250, 248, 245));
            return (LRESULT)g_brushBg;
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)lp;
            if (d->CtlType == ODT_BUTTON) {
                drawOwnerButton(d);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND: {
            UINT id = LOWORD(wp);
            UINT code = HIWORD(wp);
            if (id == ID_BTN_BROWSE) {
                browseFolder();
            } else if (id == ID_BTN_PRIMARY) {
                handlePrimary();
            } else if (id == ID_BTN_SECONDARY) {
                handleSecondary();
            } else if (id == ID_BTN_LINK) {
                ShellExecuteW(nullptr, L"open", L"https://platform.deepseek.com/api_keys", nullptr,
                              nullptr, SW_SHOWNORMAL);
            } else if (id == ID_CHK_SHOWKEY && code == BN_CLICKED) {
                g_showKey = SendMessageW(g_chkShow, BM_GETCHECK, 0, 0) == BST_CHECKED;
                SendMessageW(g_editKey, EM_SETPASSWORDCHAR,
                             g_showKey ? 0 : (WPARAM)L'\u25CF', 0);
                InvalidateRect(g_editKey, nullptr, TRUE);
            } else if (id == ID_CHK_ADVANCED && code == BN_CLICKED) {
                g_advanced = SendMessageW(g_chkAdvanced, BM_GETCHECK, 0, 0) == BST_CHECKED;
                ShowWindow(g_editBase, g_advanced ? SW_SHOW : SW_HIDE);
                ShowWindow(g_editModel, g_advanced ? SW_SHOW : SW_HIDE);
                layoutControls();
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == ID_CHK_DESKTOP && code == BN_CLICKED) {
                g_desktop = SendMessageW(g_chkDesktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
            } else if (id == ID_CHK_RUN && code == BN_CLICKED) {
                g_run = SendMessageW(g_chkRun, BM_GETCHECK, 0, 0) == BST_CHECKED;
            }
            return 0;
        }

        case WM_APP_PROGRESS: {
            g_step = LOWORD(wp);
            g_progress = HIWORD(wp);
            g_status = *(wstring*)lp;
            delete (wstring*)lp;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_APP_DONE: {
            bool ok = wp != 0;
            wstring err = *(wstring*)lp;
            delete (wstring*)lp;
            g_installing = false;
            g_installOk = ok;
            if (ok) {
                g_step = 5;
                g_progress = 100;
                g_page = PAGE_DONE;
                updatePage();
            } else {
                if (err.empty()) err = L"安装失败。";
                message(err + L"\n\n日志：" + g_logPath, MB_OK | MB_ICONERROR);
                g_page = PAGE_KEY;
                updatePage();
            }
            return 0;
        }

        case WM_APP_ESCAPE:
            if (g_page == PAGE_KEY) {
                g_page = PAGE_WELCOME;
                updatePage();
            } else if (g_page != PAGE_INSTALL) {
                DestroyWindow(hwnd);
            }
            return 0;

        case WM_KEYDOWN:
            if (wp == VK_RETURN) {
                handlePrimary();
                return 0;
            }
            if (wp == VK_ESCAPE) {
                SendMessageW(hwnd, WM_APP_ESCAPE, 0, 0);
                return 0;
            }
            break;

        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            createFonts();
            createUiFonts();
            applyControlFonts();
            RECT* nr = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, nr->left, nr->top, nr->right - nr->left,
                         nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layoutControls();
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_CLOSE:
            if (g_installing) {
                message(L"正在安装，请稍候再关闭。");
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ----------------------------------------------------------------------------
// 入口
// ----------------------------------------------------------------------------

static int runGui(HINSTANCE hInst, int nCmdShow) {
    GdiplusStartupInput gsi;
    if (GdiplusStartup(&g_gdiToken, &gsi, nullptr) != Ok) return 1;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    HDC screen = GetDC(nullptr);
    if (screen) {
        g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"PolicyDeskSetupWindow";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) {
        GdiplusShutdown(g_gdiToken);
        return 1;
    }

    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    DWORD exStyle = WS_EX_CONTROLPARENT;
    RECT rc = {0, 0, px(780), px(620)};
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);

    HWND hwnd = CreateWindowExW(exStyle, wc.lpszClassName, APP_NAME L" 安装程序", style,
                                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                                rc.bottom - rc.top, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        GdiplusShutdown(g_gdiToken);
        return 1;
    }

    RECT wr;
    GetWindowRect(hwnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd, nullptr, (sw - w) / 2, (sh - h) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    destroyFonts();
    if (g_hfEdit) DeleteObject(g_hfEdit);
    if (g_hfCheck) DeleteObject(g_hfCheck);
    if (g_brushBg) DeleteObject(g_brushBg);
    if (g_brushWhite) DeleteObject(g_brushWhite);
    GdiplusShutdown(g_gdiToken);
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    g_hInst = hInst;
    logOpen();

    std::vector<wstring> args = commandArgs();
    bool silent = false;
    for (const wstring& a : args) {
        if (ieq(a, L"/S") || ieq(a, L"--silent")) silent = true;
    }

    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int rc = 0;
    if (silent) {
        rc = runSilent(args);
    } else {
        rc = runGui(hInst, nCmdShow);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    logClose();
    return rc;
}
