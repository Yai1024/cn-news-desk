// ============================================================================
//  news_desk.exe —— 政策解读台（Win32 主界面）
//
//  · 「获取新闻稿」：后台运行同目录的 运行.bat（即 fetch_news.exe），
//    以隐藏窗口方式执行，实时把输出回显到窗口，完成后自动盘点 output/。
//  · 「开始解读」：以子进程方式拉起 agent_host.exe（独立封装的解读 Agent），
//    通过管道逐字流式回显它调用 API 得到的解读结果。
//  · 「停止」：连同子进程树一并结束。
//
//  本程序不含任何解读逻辑；解读完全由 agent_host + skill/ 完成。
// ============================================================================

#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

using std::string;
using std::wstring;

// ----------------------------------------------------------------------------
// 控件与消息
// ----------------------------------------------------------------------------

#define ID_BTN_FETCH 1001
#define ID_BTN_RUN 1002
#define ID_BTN_STOP 1003
#define ID_BTN_OPEN 1004
#define ID_TXT 1010
#define ID_STATUS 1011

#define WM_APP_CHUNK (WM_APP + 1)
#define WM_APP_STATUS (WM_APP + 2)
#define WM_APP_DONE (WM_APP + 3)

static HWND g_hwnd = nullptr;
static HWND g_btnFetch = nullptr, g_btnRun = nullptr, g_btnStop = nullptr, g_btnOpen = nullptr;
static HWND g_txt = nullptr, g_status = nullptr;
static HFONT g_fontUi = nullptr, g_fontText = nullptr;
static std::atomic<bool> g_running(false);
static HANDLE g_job = nullptr;
static bool g_lastWasFetch = false;

// ----------------------------------------------------------------------------
// 编码
// ----------------------------------------------------------------------------

static string wideToUtf8(const wstring& w) {
    if (w.empty()) return string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return string();
    string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static wstring utf8ToWide(const string& s) {
    if (s.empty()) return wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return wstring();
    wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static wstring cp936ToWide(const string& s) {
    if (s.empty()) return wstring();
    int n = MultiByteToWideChar(936, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return wstring();
    wstring w((size_t)n, L'\0');
    MultiByteToWideChar(936, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// 判断一段字节是否是合法 UTF-8（允许末尾不完整，由调用方先切齐）
static bool isValidUtf8(const string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t step = 0;
        if (c < 0x80) step = 1;
        else if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        else return false;
        if (i + step > s.size()) return true;  // 末尾截断，按合法处理
        for (size_t k = 1; k < step; ++k)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += step;
    }
    return true;
}

// 保留末尾可能不完整的 UTF-8 序列，返回可安全解码的前缀长度
static size_t utf8SafeCut(const string& s) {
    if (s.empty()) return 0;
    size_t i = s.size();
    size_t back = 0;
    while (i > 0 && back < 4) {
        unsigned char c = (unsigned char)s[i - 1];
        if ((c & 0xC0) == 0x80) { --i; ++back; continue; }  // 续字节
        size_t need = ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
        size_t have = s.size() - i + 1;
        if (need > have) return i - 1;  // 该字符未收全
        return s.size();
    }
    return s.size();
}

// ----------------------------------------------------------------------------
// 路径 / 配置
// ----------------------------------------------------------------------------

static wstring exeDir() {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    if (n == 0) return L".";
    wstring p(buf.data(), n);
    size_t pos = p.find_last_of(L"\\/");
    return pos == wstring::npos ? wstring(L".") : p.substr(0, pos);
}

static wstring joinPath(const wstring& a, const wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    wchar_t last = a.back();
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

static string readIniKey(const wstring& file, const string& key) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return string();
    char buf[8192];
    DWORD read = 0;
    string all;
    while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0) all.append(buf, read);
    CloseHandle(h);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        string line = all.substr(pos, nl == string::npos ? string::npos : nl - pos);
        pos = nl == string::npos ? all.size() : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t b = 0;
        while (b < line.size() && line[b] == ' ') ++b;
        line = line.substr(b);
        if (line.rfind(key + "=", 0) == 0) {
            string v = line.substr(key.size() + 1);
            while (!v.empty() && v.back() == ' ') v.pop_back();
            return v;
        }
    }
    return string();
}

// ----------------------------------------------------------------------------
// 界面辅助
// ----------------------------------------------------------------------------

static void appendText(const wstring& w) {
    if (w.empty() || !g_txt) return;
    int len = GetWindowTextLengthW(g_txt);
    SendMessageW(g_txt, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_txt, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
    SendMessageW(g_txt, EM_SCROLLCARET, 0, 0);
}

static void setStatus(const wstring& s) {
    if (g_status) SetWindowTextW(g_status, s.c_str());
}

static void setBusy(bool busy, bool fetching) {
    EnableWindow(g_btnFetch, !busy);
    EnableWindow(g_btnRun, !busy);
    EnableWindow(g_btnStop, busy);
    g_running = busy;
    g_lastWasFetch = fetching;
}

// 盘点 output 目录
static wstring scanSummary() {
    wstring outDir = joinPath(exeDir(), L"output");
    wstring pat = joinPath(outDir, L"*_*.txt");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    int n = 0;
    wstring newest;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            ++n;
            if (newest.empty() || wcscmp(fd.cFileName, newest.c_str()) > 0) newest = fd.cFileName;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    wchar_t buf[256];
    if (n == 0) swprintf(buf, 256, L"就绪：output\\ 里还没有稿件，先点「获取新闻稿」");
    else swprintf(buf, 256, L"就绪：%d 份稿件，最新 %ls", n, newest.c_str());
    return buf;
}

// ----------------------------------------------------------------------------
// 子进程
// ----------------------------------------------------------------------------

struct Job {
    HWND hwnd = nullptr;
    wstring cmd;
    bool fetch = false;
};

static wstring quoteArg(const wstring& s) { return L"\"" + s + L"\""; }

static DWORD WINAPI Worker(LPVOID param) {
    Job* job = (Job*)param;

    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rPipe = nullptr, wPipe = nullptr;
    if (!CreatePipe(&rPipe, &wPipe, &sa, 0)) {
        PostMessageW(job->hwnd, WM_APP_STATUS, 0, (LPARAM)new wstring(L"创建管道失败"));
        PostMessageW(job->hwnd, WM_APP_DONE, 1, 0);
        delete job;
        return 1;
    }
    SetHandleInformation(rPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wPipe;
    si.hStdError = wPipe;
    si.hStdInput = (hNul != INVALID_HANDLE_VALUE) ? hNul : nullptr;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> cmdBuf(job->cmd.begin(), job->cmd.end());
    cmdBuf.push_back(L'\0');

    wstring cwd = exeDir();
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                             nullptr, cwd.c_str(), &si, &pi);
    CloseHandle(wPipe);

    if (!ok) {
        DWORD e = GetLastError();
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        CloseHandle(rPipe);
        wchar_t buf[128];
        swprintf(buf, 128, L"启动失败（错误码 %lu）", e);
        PostMessageW(job->hwnd, WM_APP_STATUS, 0, (LPARAM)new wstring(buf));
        PostMessageW(job->hwnd, WM_APP_DONE, 1, 0);
        delete job;
        return 1;
    }

    // 放进 Job Object，停止时连子进程树一起结束
    HANDLE jobObj = CreateJobObjectW(nullptr, nullptr);
    if (jobObj) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
        ZeroMemory(&info, sizeof(info));
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(jobObj, JobObjectExtendedLimitInformation, &info, sizeof(info));
        AssignProcessToJobObject(jobObj, pi.hProcess);
    }
    // 交给停止按钮使用
    g_job = jobObj;

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    WaitForSingleObject(pi.hProcess, 200);  // 让子进程先跑起来

    string pending;
    char buf[4096];
    while (true) {
        DWORD read = 0;
        if (!ReadFile(rPipe, buf, sizeof(buf), &read, nullptr) || read == 0) break;
        pending.append(buf, read);
        size_t cut = utf8SafeCut(pending);
        if (cut == 0) continue;
        string chunk = pending.substr(0, cut);
        pending.erase(0, cut);
        wstring w = isValidUtf8(chunk) ? utf8ToWide(chunk) : cp936ToWide(chunk);
        PostMessageW(job->hwnd, WM_APP_CHUNK, 0, (LPARAM)new wstring(w));
    }
    if (!pending.empty()) {
        wstring w = isValidUtf8(pending) ? utf8ToWide(pending) : cp936ToWide(pending);
        PostMessageW(job->hwnd, WM_APP_CHUNK, 0, (LPARAM)new wstring(w));
    }

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    DWORD waitRes = WaitForSingleObject(pi.hProcess, 5000);
    if (waitRes == WAIT_TIMEOUT) code = 1;

    CloseHandle(pi.hProcess);
    CloseHandle(rPipe);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    if (jobObj) {
        CloseHandle(jobObj);
        g_job = nullptr;
    }

    PostMessageW(job->hwnd, WM_APP_DONE, (WPARAM)code, 0);
    delete job;
    return 0;
}

static void startJob(const wstring& cmd, bool fetch) {
    if (g_running) return;
    setBusy(true, fetch);
    EnableWindow(g_btnStop, TRUE);
    Job* job = new Job();
    job->hwnd = g_hwnd;
    job->cmd = cmd;
    job->fetch = fetch;
    // 先建 Job 占位，真正句柄在 Worker 里替换
    g_job = nullptr;
    HANDLE th = CreateThread(nullptr, 0, Worker, job, 0, nullptr);
    if (th) CloseHandle(th);
}

// ----------------------------------------------------------------------------
// 命令拼装
// ----------------------------------------------------------------------------

static wstring fetchCommand() {
    wstring bat = joinPath(exeDir(), L"运行.bat");
    // < nul 让 bat 末尾的 pause 立即返回；/c 后面整体再加一层引号
    return L"cmd.exe /c \"\"" + bat + L"\"\" < nul";
}

static wstring interpretCommand() {
    wstring exe = joinPath(exeDir(), L"agent_host.exe");
    wstring out = joinPath(exeDir(), L"output");
    return quoteArg(exe) + L" --input-dir " + quoteArg(out);
}

// ----------------------------------------------------------------------------
// 窗口过程
// ----------------------------------------------------------------------------

static void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int pad = 10, btnH = 32, btnW = 122, gap = 8, statusH = 24;
    int x = pad, y = pad;
    MoveWindow(g_btnFetch, x, y, btnW, btnH, TRUE); x += btnW + gap;
    MoveWindow(g_btnRun, x, y, btnW, btnH, TRUE); x += btnW + gap;
    MoveWindow(g_btnStop, x, y, 72, btnH, TRUE); x += 72 + gap;
    MoveWindow(g_btnOpen, x, y, btnW, btnH, TRUE);

    int top = y + btnH + pad;
    int editH = rc.bottom - top - statusH - pad;
    if (editH < 80) editH = 80;
    MoveWindow(g_txt, pad, top, rc.right - 2 * pad, editH, TRUE);
    MoveWindow(g_status, pad, rc.bottom - statusH - 4, rc.right - 2 * pad, statusH, TRUE);
}

static void createControls(HWND hwnd) {
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    g_btnFetch = CreateWindowW(L"BUTTON", L"获取新闻稿", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               0, 0, 0, 0, hwnd, (HMENU)ID_BTN_FETCH, hInst, nullptr);
    g_btnRun = CreateWindowW(L"BUTTON", L"开始解读", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             0, 0, 0, 0, hwnd, (HMENU)ID_BTN_RUN, hInst, nullptr);
    g_btnStop = CreateWindowW(L"BUTTON", L"停止", WS_CHILD | BS_PUSHBUTTON,
                              0, 0, 0, 0, hwnd, (HMENU)ID_BTN_STOP, hInst, nullptr);
    g_btnOpen = CreateWindowW(L"BUTTON", L"打开稿件目录", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              0, 0, 0, 0, hwnd, (HMENU)ID_BTN_OPEN, hInst, nullptr);
    g_txt = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL,
                            0, 0, 0, 0, hwnd, (HMENU)ID_TXT, hInst, nullptr);
    g_status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                             0, 0, 0, 0, hwnd, (HMENU)ID_STATUS, hInst, nullptr);

    g_fontUi = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_fontText = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH, L"Microsoft YaHei UI");

    SendMessageW(g_btnFetch, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
    SendMessageW(g_btnRun, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
    SendMessageW(g_btnStop, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
    SendMessageW(g_btnOpen, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
    SendMessageW(g_txt, WM_SETFONT, (WPARAM)g_fontText, TRUE);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_fontUi, TRUE);

    SendMessageW(g_txt, EM_SETLIMITTEXT, 0x7FFFFFF0, 0);

    wstring hint;
    hint += L"政策解读台 · 《新闻联播》《人民日报》\r\n\r\n";
    hint += L"1. 点「获取新闻稿」：后台运行 运行.bat，自动补抓最近 7 天缺失的日期（首次约 15–20 分钟）。\r\n";
    hint += L"2. 点「开始解读」：调用 agent_host.exe，按 skill/ 里的方法论解读并流式显示。\r\n";
    hint += L"3. 解读需要 agent.ini 里配置 api_key；未配置时程序会提示。\r\n\r\n";

    string key = readIniKey(joinPath(exeDir(), L"agent.ini"), "api_key");
    hint += key.empty() ? L"当前状态：未配置 API 密钥（agent.ini 的 api_key= 为空）。\r\n"
                        : L"当前状态：API 密钥已配置。\r\n";
    hint += L"\r\n" + scanSummary() + L"\r\n";
    SetWindowTextW(g_txt, hint.c_str());
}

static void onFetch() {
    appendText(L"\r\n=== 获取新闻稿 ===\r\n");
    setStatus(L"正在后台抓取（首次较慢，可随时点「停止」）…");
    startJob(fetchCommand(), true);
}

static void onInterpret() {
    if (g_running) return;
    appendText(L"\r\n=== 开始解读 ===\r\n");
    setStatus(L"解读中…（模型输出会逐字出现）");
    startJob(interpretCommand(), false);
}

static void onStop() {
    if (g_job) {
        CloseHandle(g_job);  // KILL_ON_JOB_CLOSE：连同子进程树一起结束
        g_job = nullptr;
    }
    setStatus(L"已请求停止…");
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            createControls(hwnd);
            layout(hwnd);
            setStatus(scanSummary());
            EnableWindow(g_btnStop, FALSE);
            return 0;

        case WM_SIZE:
            layout(hwnd);
            return 0;

        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == ID_BTN_FETCH) onFetch();
            else if (id == ID_BTN_RUN) onInterpret();
            else if (id == ID_BTN_STOP) onStop();
            else if (id == ID_BTN_OPEN) {
                wstring dir = joinPath(exeDir(), L"output");
                ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        }

        case WM_APP_CHUNK: {
            wstring* p = (wstring*)lp;
            appendText(*p);
            delete p;
            return 0;
        }

        case WM_APP_STATUS: {
            wstring* p = (wstring*)lp;
            setStatus(*p);
            delete p;
            return 0;
        }

        case WM_APP_DONE: {
            int code = (int)wp;
            bool wasFetch = g_lastWasFetch;
            setBusy(false, false);
            EnableWindow(g_btnStop, FALSE);
            if (wasFetch) {
                appendText(code == 0
                               ? wstring(L"\r\n[抓取结束]\r\n")
                               : wstring(L"\r\n[抓取进程已退出，退出码 ") + std::to_wstring(code) +
                                     L"；请查看上方输出]\r\n");
                setStatus(scanSummary());
            } else {
                if (code != 0) {
                    appendText(L"\r\n[解读未完成，退出码 " + std::to_wstring(code) + L"；原因见上方]\r\n");
                    setStatus(L"解读失败（检查 agent.ini 的 api_key / base_url / model）");
                } else {
                    setStatus(L"解读完成");
                }
            }
            return 0;
        }

        case WM_DESTROY:
            if (g_job) { CloseHandle(g_job); g_job = nullptr; }
            if (g_fontUi) DeleteObject(g_fontUi);
            if (g_fontText) DeleteObject(g_fontText);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    SetProcessDPIAware();

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"NewsDeskWindow";
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"政策解读台 — 新闻联播 / 人民日报",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 720,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
