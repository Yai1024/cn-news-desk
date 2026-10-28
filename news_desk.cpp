// ============================================================================
//  news_desk.exe —— 政策解读台（Win32 自绘主界面）
//
//  · 「获取新闻稿」：后台运行同目录的 运行.bat（即 fetch_news.exe），
//    以隐藏窗口方式执行，实时把输出回显到窗口，完成后自动盘点 output/。
//  · 「开始解读」：以子进程方式拉起 agent_host.exe（独立封装的解读 Agent），
//    通过管道逐字流式回显它调用 API 得到的解读结果。
//  · 「停止」：连同子进程树一并结束。
//
//  界面为纯 Win32 + GDI+ 自绘（GDI+ 为系统组件，非第三方框架）：
//  编辑室浅色配色、圆角卡片、自绘按钮与细滚动条，无任何外部资源依赖。
//  本程序不含任何解读逻辑；解读完全由 agent_host + skill/ 完成。
// ============================================================================

#define _WIN32_WINNT 0x0601
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

using namespace Gdiplus;
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
#define ID_SCROLL 1020

#define WM_APP_CHUNK (WM_APP + 1)
#define WM_APP_STATUS (WM_APP + 2)
#define WM_APP_DONE (WM_APP + 3)

static HWND g_hwnd = nullptr;
static HWND g_btnFetch = nullptr, g_btnRun = nullptr, g_btnStop = nullptr, g_btnOpen = nullptr;
static HWND g_txt = nullptr, g_sb = nullptr;
static HFONT g_fontEdit = nullptr;
static HBRUSH g_brWhite = nullptr;
static std::atomic<bool> g_running(false);
static HANDLE g_job = nullptr;
static bool g_lastWasFetch = false;
static int g_dpi = 96;

static Font* g_fTitle = nullptr;
static Font* g_fSub = nullptr;
static Font* g_fBtn = nullptr;
static Font* g_fBtnBold = nullptr;
static Font* g_fCard = nullptr;
static Font* g_fPill = nullptr;
static Font* g_fSmall = nullptr;
static FontFamily* g_family = nullptr;

// ----------------------------------------------------------------------------
// 配色（编辑室浅色）
// ----------------------------------------------------------------------------

namespace C {
static const Color BG(255, 250, 248, 245);           // #FAF8F5 纸感底色
static const Color CARD(255, 255, 255, 255);
static const Color BORDER(255, 231, 226, 218);       // #E7E2DA
static const Color INK(255, 27, 27, 27);             // #1B1B1B
static const Color MUTED(255, 122, 115, 107);        // #7A736B
static const Color FAINT(255, 168, 161, 152);
static const Color ACCENT(255, 200, 50, 43);         // #C8322B 党政红
static const Color ACCENT_HOVER(255, 176, 42, 36);
static const Color ACCENT_DOWN(255, 153, 36, 31);
static const Color ACCENT_SOFT(255, 252, 243, 241);
static const Color GHOST_HOVER(255, 243, 239, 234);
static const Color DISABLED_BG(255, 241, 238, 234);
static const Color DISABLED_FG(255, 183, 176, 168);
static const Color PILL_BG(255, 241, 235, 228);
static const Color OK(255, 46, 125, 91);             // #2E7D5B
static const Color THUMB(255, 222, 216, 207);
static const Color THUMB_HOVER(255, 200, 193, 183);
}  // namespace C

static int px(int v) { return MulDiv(v, g_dpi, 96); }

static int mouseY(LPARAM l) { return (int)(short)HIWORD(l); }

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
// GDI+ 绘图辅助
// ----------------------------------------------------------------------------

static void makeRound(GraphicsPath& p, const RectF& r, float rad) {
    if (rad <= 0.5f) { p.AddRectangle(r); return; }
    float d = rad * 2.0f;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    p.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

static void fillRound(Graphics& g, const RectF& r, float rad, const Color& c) {
    GraphicsPath p;
    makeRound(p, r, rad);
    SolidBrush b(c);
    g.FillPath(&b, &p);
}

static void strokeRound(Graphics& g, const RectF& r, float rad, const Color& c, float w) {
    GraphicsPath p;
    makeRound(p, r, rad);
    Pen pen(c, w);
    g.DrawPath(&pen, &p);
}

static void drawText(Graphics& g, const wstring& s, Font* f, const Color& c, const RectF& r,
                     StringAlignment ha = StringAlignmentNear,
                     StringAlignment va = StringAlignmentCenter) {
    if (!f || s.empty()) return;
    StringFormat sf;
    sf.SetAlignment(ha);
    sf.SetLineAlignment(va);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush b(c);
    g.DrawString(s.c_str(), (INT)s.size(), f, r, &sf, &b);
}

static RectF rectToF(const RECT& r) {
    return RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top));
}

// ----------------------------------------------------------------------------
// 布局
// ----------------------------------------------------------------------------

struct Layout {
    int cliW = 0, cliH = 0;
    int pad = 0, headerH = 0, sideW = 0, gapX = 0, cardHeaderH = 0;
    RECT card{};
    RECT edit{};
    RECT scroll{};
    RECT buttons[4]{};
};

static Layout g_L;

static void computeLayout(int w, int h) {
    g_L.cliW = w;
    g_L.cliH = h;
    g_L.pad = px(24);
    g_L.headerH = px(72);
    g_L.sideW = px(196);
    g_L.gapX = px(24);
    g_L.cardHeaderH = px(52);

    int cardX = g_L.pad + g_L.sideW + g_L.gapX;
    int cardY = g_L.headerH + g_L.pad;
    int cardR = w - g_L.pad;
    int cardB = h - g_L.pad;
    if (cardR < cardX + px(200)) cardR = cardX + px(200);
    if (cardB < cardY + px(160)) cardB = cardY + px(160);
    g_L.card = {cardX, cardY, cardR, cardB};

    int bodyTop = cardY + g_L.cardHeaderH;
    g_L.edit = {cardX + 1, bodyTop, cardR - 1 - px(20), cardB - px(12)};

    int sbW = px(8);
    int sbX = cardR - 1 - px(16);
    g_L.scroll = {sbX, bodyTop + px(10), sbX + sbW, cardB - px(12) - px(10)};

    int bx = g_L.pad;
    int bw = g_L.sideW;
    int y = g_L.headerH + g_L.pad;
    int bh = px(44);
    g_L.buttons[0] = {bx, y, bx + bw, y + bh};
    y += bh + px(10);
    g_L.buttons[1] = {bx, y, bx + bw, y + bh};
    y += bh + px(16);
    g_L.buttons[2] = {bx, y, bx + bw, y + px(40)};
    y += px(40) + px(8);
    g_L.buttons[3] = {bx, y, bx + bw, y + px(38)};
}

// ----------------------------------------------------------------------------
// 按钮（自绘）
// ----------------------------------------------------------------------------

struct BtnState {
    WNDPROC oldProc = nullptr;
    Color bg, bgHover, bgDown, border, fg;
    bool hasBorder = false;
    bool bold = false;
    bool hover = false;
    int radius = 8;
};

static BtnState g_stFetch, g_stRun, g_stStop, g_stOpen;

static void initButtonState(BtnState& st, const Color& bg, const Color& bgHover, const Color& bgDown,
                            const Color& border, const Color& fg, bool bold, int radius) {
    st.bg = bg;
    st.bgHover = bgHover;
    st.bgDown = bgDown;
    st.border = border;
    st.fg = fg;
    st.bold = bold;
    st.hasBorder = (border.GetA() != 0);
    st.radius = radius;
}

static LRESULT CALLBACK ButtonProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    BtnState* st = (BtnState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_MOUSEMOVE:
            if (st && !st->hover) {
                TRACKMOUSEEVENT tme;
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = h;
                tme.dwHoverTime = 0;
                TrackMouseEvent(&tme);
                st->hover = true;
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (st) {
                st->hover = false;
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT && IsWindowEnabled(h)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            InvalidateRect(h, nullptr, FALSE);
            break;
    }
    return CallWindowProcW(st ? st->oldProc : DefWindowProcW, h, m, w, l);
}

static void drawButton(LPDRAWITEMSTRUCT dis) {
    BtnState* st = (BtnState*)GetWindowLongPtrW(dis->hwndItem, GWLP_USERDATA);
    if (!st) return;

    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    Color bg = disabled ? C::DISABLED_BG : pressed ? st->bgDown : (st->hover ? st->bgHover : st->bg);
    Color fg = disabled ? C::DISABLED_FG : st->fg;

    wchar_t buf[128] = {0};
    GetWindowTextW(dis->hwndItem, buf, 128);

    Graphics g(dis->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    RectF r = rectToF(dis->rcItem);
    // 底色先铺满，避免自绘残留
    SolidBrush clear(C::BG);
    g.FillRectangle(&clear, r);

    RectF inner(r.X + 0.5f, r.Y + 0.5f, r.Width - 1.0f, r.Height - 1.0f);
    float rad = (float)px(st->radius);
    fillRound(g, inner, rad, bg);
    if (st->hasBorder && !disabled) strokeRound(g, inner, rad, st->border, 1.0f);

    drawText(g, buf, st->bold ? g_fBtnBold : g_fBtn, fg, inner,
             StringAlignmentCenter, StringAlignmentCenter);
}

// ----------------------------------------------------------------------------
// 滚动条（自绘，贴在卡片右侧）
// ----------------------------------------------------------------------------

struct SbState {
    WNDPROC oldProc = nullptr;
    bool hover = false;
    bool dragging = false;
    int dragY = 0;
    int dragFirst = 0;
};

static SbState g_sbState;

static int editLineHeight() {
    if (!g_txt) return 16;
    HDC dc = GetDC(g_txt);
    HFONT f = (HFONT)SendMessageW(g_txt, WM_GETFONT, 0, 0);
    HGDIOBJ old = f ? SelectObject(dc, f) : nullptr;
    TEXTMETRICW tm;
    ZeroMemory(&tm, sizeof(tm));
    GetTextMetricsW(dc, &tm);
    if (old) SelectObject(dc, old);
    ReleaseDC(g_txt, dc);
    return tm.tmHeight > 0 ? tm.tmHeight : px(16);
}

static bool sbMetrics(int& total, int& first, int& vis, int& maxFirst) {
    if (!g_txt) return false;
    total = (int)SendMessageW(g_txt, EM_GETLINECOUNT, 0, 0);
    first = (int)SendMessageW(g_txt, EM_GETFIRSTVISIBLELINE, 0, 0);
    int lineH = editLineHeight();
    int h = g_L.edit.bottom - g_L.edit.top;
    vis = lineH > 0 ? h / lineH : 1;
    if (vis < 1) vis = 1;
    maxFirst = total - vis;
    if (maxFirst < 0) maxFirst = 0;
    return maxFirst > 0;
}

static void sbPaint(HWND h) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);

    RECT rc;
    GetClientRect(h, &rc);
    int trackH = rc.bottom - rc.top;

    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    SolidBrush bg(C::CARD);
    g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

    int total = 0, first = 0, vis = 0, maxFirst = 0;
    if (trackH > 0 && sbMetrics(total, first, vis, maxFirst)) {
        int thumbH = (int)((double)trackH * vis / (double)total);
        int minH = px(28);
        if (thumbH < minH) thumbH = minH;
        if (thumbH > trackH) thumbH = trackH;
        int span = trackH - thumbH;
        int ty = span > 0 ? (int)((double)span * first / (double)maxFirst) : 0;
        if (ty < 0) ty = 0;
        if (ty > span) ty = span;

        float w = (float)(rc.right - rc.left);
        RectF tr(0.0f, (REAL)ty, w, (REAL)thumbH);
        fillRound(g, tr, w / 2.0f, g_sbState.hover ? C::THUMB_HOVER : C::THUMB);
    }

    EndPaint(h, &ps);
}

static void sbScrollToFirst(int target) {
    if (!g_txt) return;
    int total = 0, first = 0, vis = 0, maxFirst = 0;
    if (!sbMetrics(total, first, vis, maxFirst)) return;
    if (target < 0) target = 0;
    if (target > maxFirst) target = maxFirst;
    SendMessageW(g_txt, EM_LINESCROLL, 0, (LPARAM)(target - first));
    InvalidateRect(g_sb, nullptr, FALSE);
}

static void sbScrollBy(int lines) {
    if (!g_txt) return;
    int first = (int)SendMessageW(g_txt, EM_GETFIRSTVISIBLELINE, 0, 0);
    sbScrollToFirst(first + lines);
}

static LRESULT CALLBACK SbProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            sbPaint(h);
            return 0;
        case WM_MOUSEMOVE: {
            if (!g_sbState.hover) {
                TRACKMOUSEEVENT tme;
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = h;
                tme.dwHoverTime = 0;
                TrackMouseEvent(&tme);
                g_sbState.hover = true;
                InvalidateRect(h, nullptr, FALSE);
            }
            if (g_sbState.dragging) {
                RECT rc;
                GetClientRect(h, &rc);
                int trackH = rc.bottom - rc.top;
                int total = 0, first = 0, vis = 0, maxFirst = 0;
                if (sbMetrics(total, first, vis, maxFirst) && trackH > 0) {
                    int thumbH = (int)((double)trackH * vis / (double)total);
                    int minH = px(28);
                    if (thumbH < minH) thumbH = minH;
                    int span = trackH - thumbH;
                    if (span > 0) {
                        int dy = mouseY(l) - g_sbState.dragY;
                        int target = g_sbState.dragFirst + (int)((double)dy * maxFirst / (double)span);
                        sbScrollToFirst(target);
                    }
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_sbState.hover = false;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            SetCapture(h);
            int total = 0, first = 0, vis = 0, maxFirst = 0;
            if (!sbMetrics(total, first, vis, maxFirst)) return 0;
            RECT rc;
            GetClientRect(h, &rc);
            int trackH = rc.bottom - rc.top;
            int thumbH = (int)((double)trackH * vis / (double)total);
            int minH = px(28);
            if (thumbH < minH) thumbH = minH;
            int span = trackH - thumbH;
            int ty = span > 0 ? (int)((double)span * first / (double)maxFirst) : 0;
            int y = mouseY(l);
            if (y >= ty && y <= ty + thumbH) {
                g_sbState.dragging = true;
                g_sbState.dragY = y;
                g_sbState.dragFirst = first;
            } else {
                // 点击空白处翻页
                sbScrollBy(y < ty ? -(vis - 1) : (vis - 1));
            }
            return 0;
        }
        case WM_LBUTTONUP:
            g_sbState.dragging = false;
            ReleaseCapture();
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(w);
            sbScrollBy(-(delta / WHEEL_DELTA) * 3);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
    }
    return DefWindowProcW(h, m, w, l);
}

// ----------------------------------------------------------------------------
// 输出区（只读编辑框，自绘滚动条代为滚动）
// ----------------------------------------------------------------------------

static WNDPROC g_editOldProc = nullptr;

static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(w);
            sbScrollBy(-(delta / WHEEL_DELTA) * 3);
            return 0;
        }
        case WM_KEYDOWN:
            if (w == VK_PRIOR || w == VK_NEXT || w == VK_HOME || w == VK_END) {
                LRESULT r = CallWindowProcW(g_editOldProc, h, m, w, l);
                InvalidateRect(g_sb, nullptr, FALSE);
                return r;
            }
            break;
        case WM_VSCROLL:
        case WM_SETFOCUS:
            InvalidateRect(g_sb, nullptr, FALSE);
            break;
    }
    return CallWindowProcW(g_editOldProc, h, m, w, l);
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
    InvalidateRect(g_sb, nullptr, FALSE);
}

enum class StatusKind { Idle, Busy, Ok, Error };
static wstring g_statusText = L"就绪";
static StatusKind g_statusKind = StatusKind::Idle;

static void setStatus(const wstring& s, StatusKind k = StatusKind::Idle) {
    g_statusText = s;
    g_statusKind = k;
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
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
    if (n == 0) swprintf(buf, 256, L"还没有稿件，先获取新闻稿");
    else swprintf(buf, 256, L"%d 份稿件 · 最新 %ls", n, newest.c_str());
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
// 字体 / 控件
// ----------------------------------------------------------------------------

static void createFonts() {
    FontFamily tmp(L"Microsoft YaHei UI");
    if (tmp.GetLastStatus() == Ok) g_family = new FontFamily(L"Microsoft YaHei UI");
    else g_family = new FontFamily(L"Microsoft YaHei");

    g_fTitle = new Font(g_family, (REAL)px(21), FontStyleBold, UnitPixel);
    g_fSub = new Font(g_family, (REAL)px(13), FontStyleRegular, UnitPixel);
    g_fBtn = new Font(g_family, (REAL)px(15), FontStyleRegular, UnitPixel);
    g_fBtnBold = new Font(g_family, (REAL)px(15), FontStyleBold, UnitPixel);
    g_fCard = new Font(g_family, (REAL)px(15), FontStyleBold, UnitPixel);
    g_fPill = new Font(g_family, (REAL)px(12), FontStyleRegular, UnitPixel);
    g_fSmall = new Font(g_family, (REAL)px(12), FontStyleRegular, UnitPixel);
}

static void destroyFonts() {
    delete g_fSmall; g_fSmall = nullptr;
    delete g_fPill; g_fPill = nullptr;
    delete g_fCard; g_fCard = nullptr;
    delete g_fBtnBold; g_fBtnBold = nullptr;
    delete g_fBtn; g_fBtn = nullptr;
    delete g_fSub; g_fSub = nullptr;
    delete g_fTitle; g_fTitle = nullptr;
    delete g_family; g_family = nullptr;
}

static void applyEditFormatRect() {
    if (!g_txt) return;
    RECT rc;
    GetClientRect(g_txt, &rc);
    RECT fmt;
    fmt.left = px(20);    // 与卡片标题「输出」左对齐
    fmt.top = px(14);
    fmt.right = rc.right - px(14);
    fmt.bottom = rc.bottom - px(10);
    if (fmt.right < fmt.left + px(20)) fmt.right = fmt.left + px(20);
    if (fmt.bottom < fmt.top + px(20)) fmt.bottom = fmt.top + px(20);
    SendMessageW(g_txt, EM_SETRECT, 0, (LPARAM)&fmt);
}

static HWND makeButton(HWND parent, HINSTANCE hInst, int id, const wchar_t* text, RECT r, BtnState& st) {
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             r.left, r.top, r.right - r.left, r.bottom - r.top,
                             parent, (HMENU)(INT_PTR)id, hInst, nullptr);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);
    st.oldProc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)ButtonProc);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_fontEdit, TRUE);
    return h;
}

static void createControls(HWND hwnd) {
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);

    initButtonState(g_stFetch, C::ACCENT, C::ACCENT_HOVER, C::ACCENT_DOWN, Color(0, 0, 0, 0),
                    Color(255, 255, 255, 255), true, 8);
    initButtonState(g_stRun, C::CARD, C::ACCENT_SOFT, Color(255, 247, 236, 233), C::ACCENT,
                    C::ACCENT, true, 8);
    initButtonState(g_stStop, C::CARD, C::GHOST_HOVER, Color(255, 236, 231, 225), C::BORDER,
                    C::MUTED, false, 8);
    initButtonState(g_stOpen, Color(0, 0, 0, 0), C::GHOST_HOVER, Color(255, 236, 231, 225),
                    Color(0, 0, 0, 0), C::MUTED, false, 8);

    g_btnFetch = makeButton(hwnd, hInst, ID_BTN_FETCH, L"获取新闻稿", g_L.buttons[0], g_stFetch);
    g_btnRun = makeButton(hwnd, hInst, ID_BTN_RUN, L"开始解读", g_L.buttons[1], g_stRun);
    g_btnStop = makeButton(hwnd, hInst, ID_BTN_STOP, L"停止", g_L.buttons[2], g_stStop);
    g_btnOpen = makeButton(hwnd, hInst, ID_BTN_OPEN, L"打开稿件目录", g_L.buttons[3], g_stOpen);

    g_txt = CreateWindowExW(0, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                                ES_AUTOVSCROLL | ES_NOHIDESEL,
                            g_L.edit.left, g_L.edit.top,
                            g_L.edit.right - g_L.edit.left, g_L.edit.bottom - g_L.edit.top,
                            hwnd, (HMENU)ID_TXT, hInst, nullptr);
    SendMessageW(g_txt, EM_SETLIMITTEXT, 0x7FFFFFF0, 0);
    SendMessageW(g_txt, WM_SETFONT, (WPARAM)g_fontEdit, TRUE);
    g_editOldProc = (WNDPROC)SetWindowLongPtrW(g_txt, GWLP_WNDPROC, (LONG_PTR)EditProc);

    g_sb = CreateWindowExW(0, L"DeskScroll", L"",
                           WS_CHILD | WS_VISIBLE,
                           g_L.scroll.left, g_L.scroll.top,
                           g_L.scroll.right - g_L.scroll.left, g_L.scroll.bottom - g_L.scroll.top,
                           hwnd, (HMENU)ID_SCROLL, hInst, nullptr);

    applyEditFormatRect();

    wstring hint;
    hint += L"政策解读台 · 《新闻联播》《人民日报》\r\n\r\n";
    hint += L"① 点「获取新闻稿」——后台运行 运行.bat，自动补抓最近 7 天缺失的日期（首次约 15–20 分钟）。\r\n";
    hint += L"② 点「开始解读」——调用 agent_host.exe，按 skill/ 里的方法论解读，结果逐字流式显示。\r\n";
    hint += L"③ 解读需要 agent.ini 里配置 api_key；未配置时程序会提示。\r\n\r\n";

    string key = readIniKey(joinPath(exeDir(), L"agent.ini"), "api_key");
    hint += key.empty() ? L"当前状态：未配置 API 密钥（agent.ini 的 api_key= 为空）。\r\n"
                        : L"当前状态：API 密钥已配置。\r\n";
    hint += L"\r\n" + scanSummary() + L"\r\n";
    SetWindowTextW(g_txt, hint.c_str());
}

// ----------------------------------------------------------------------------
// 自绘主界面
// ----------------------------------------------------------------------------

static void paintScene(Graphics& g, int w, int h) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // 底色
    SolidBrush bg(C::BG);
    g.FillRectangle(&bg, 0, 0, w, h);

    // ---- 顶部标题区 ----
    int pad = g_L.pad;
    float barX = (float)pad;
    float barY = (float)px(16);
    RectF bar(barX, barY, (REAL)px(4), (REAL)px(30));
    fillRound(g, bar, (REAL)px(2), C::ACCENT);

    drawText(g, L"政策解读台", g_fTitle, C::INK,
             RectF(barX + px(14), barY - px(3), (REAL)px(320), (REAL)px(30)),
             StringAlignmentNear, StringAlignmentCenter);

    drawText(g, L"《新闻联播》《人民日报》每日抓取与政策解读", g_fSub, C::MUTED,
             RectF(barX + px(14), barY + px(27), (REAL)px(460), (REAL)px(20)),
             StringAlignmentNear, StringAlignmentCenter);

    // ---- 状态胶囊（右上） ----
    {
        RectF bounds;
        g.MeasureString(g_statusText.c_str(), (INT)g_statusText.size(), g_fPill, PointF(0, 0), &bounds);
        float pillH = (float)px(30);
        float pillW = px(14) * 2 + px(7) + px(8) + bounds.Width;
        float pillX = (float)w - pad - pillW;
        float pillY = (barY + px(30) / 2.0f) - pillH / 2.0f;
        RectF pill(pillX, pillY, pillW, pillH);
        fillRound(g, pill, pillH / 2.0f, C::PILL_BG);

        Color dot = C::FAINT;
        if (g_statusKind == StatusKind::Busy) dot = C::ACCENT;
        else if (g_statusKind == StatusKind::Ok) dot = C::OK;
        else if (g_statusKind == StatusKind::Error) dot = C::ACCENT;
        SolidBrush db(dot);
        float dy = pillY + pillH / 2.0f - px(7) / 2.0f;
        g.FillEllipse(&db, pillX + px(14), dy, (REAL)px(7), (REAL)px(7));

        drawText(g, g_statusText, g_fPill, C::MUTED,
                 RectF(pillX + px(14) + px(7) + px(8), pillY, pillW - px(14) * 2 - px(7) - px(8), pillH),
                 StringAlignmentNear, StringAlignmentCenter);
    }

    // 标题区与内容区的分隔线
    Pen line(C::BORDER, 1.0f);
    g.DrawLine(&line, (REAL)0, (REAL)(g_L.headerH - 1), (REAL)w, (REAL)(g_L.headerH - 1));

    // ---- 右侧输出卡片 ----
    RectF card = rectToF(g_L.card);
    RectF cardIn(card.X + 0.5f, card.Y + 0.5f, card.Width - 1.0f, card.Height - 1.0f);
    fillRound(g, cardIn, (REAL)px(10), C::CARD);
    strokeRound(g, cardIn, (REAL)px(10), C::BORDER, 1.0f);

    // 卡片标题栏
    drawText(g, L"输出", g_fCard, C::INK,
             RectF(card.X + px(20), card.Y, card.Width - px(40), (REAL)g_L.cardHeaderH),
             StringAlignmentNear, StringAlignmentCenter);
    drawText(g, L"output\\", g_fSmall, C::FAINT,
             RectF(card.X + px(20), card.Y, card.Width - px(40), (REAL)g_L.cardHeaderH),
             StringAlignmentFar, StringAlignmentCenter);

    // 卡片内分隔线（不压到圆角）
    float sepY = card.Y + g_L.cardHeaderH - 1;
    g.DrawLine(&line, card.X + px(20), sepY, card.X + card.Width - px(20), sepY);

    // ---- 左栏底部提示 ----
    {
        float sideBottom = (float)(h - pad);
        float lh = (float)px(18);
        float tipY = sideBottom - lh * 2;
        drawText(g, L"首次使用请先获取新闻稿；", g_fSmall, C::FAINT,
                 RectF((REAL)pad, tipY, (REAL)g_L.sideW, lh), StringAlignmentNear, StringAlignmentNear);
        drawText(g, L"解读前在 agent.ini 填好密钥。", g_fSmall, C::FAINT,
                 RectF((REAL)pad, tipY + lh, (REAL)g_L.sideW, lh), StringAlignmentNear, StringAlignmentNear);
    }
}

static void onPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    {
        Graphics g(mem);
        paintScene(g, w, h);
    }

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    computeLayout(rc.right - rc.left, rc.bottom - rc.top);

    HWND btns[4] = {g_btnFetch, g_btnRun, g_btnStop, g_btnOpen};
    for (int i = 0; i < 4; ++i) {
        const RECT& r = g_L.buttons[i];
        MoveWindow(btns[i], r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    }

    MoveWindow(g_txt, g_L.edit.left, g_L.edit.top,
               g_L.edit.right - g_L.edit.left, g_L.edit.bottom - g_L.edit.top, TRUE);
    applyEditFormatRect();

    MoveWindow(g_sb, g_L.scroll.left, g_L.scroll.top,
               g_L.scroll.right - g_L.scroll.left, g_L.scroll.bottom - g_L.scroll.top, TRUE);
    InvalidateRect(g_sb, nullptr, FALSE);
}

static void onFetch() {
    appendText(L"\r\n=== 获取新闻稿 ===\r\n");
    setStatus(L"正在后台抓取…", StatusKind::Busy);
    startJob(fetchCommand(), true);
}

static void onInterpret() {
    if (g_running) return;
    appendText(L"\r\n=== 开始解读 ===\r\n");
    setStatus(L"解读中…", StatusKind::Busy);
    startJob(interpretCommand(), false);
}

static void onStop() {
    if (g_job) {
        CloseHandle(g_job);  // KILL_ON_JOB_CLOSE：连同子进程树一起结束
        g_job = nullptr;
    }
    setStatus(L"已请求停止…", StatusKind::Idle);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            RECT rc;
            GetClientRect(hwnd, &rc);
            computeLayout(rc.right - rc.left, rc.bottom - rc.top);
            createControls(hwnd);
            layout(hwnd);
            setStatus(scanSummary(), StatusKind::Idle);
            EnableWindow(g_btnStop, FALSE);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            onPaint(hwnd);
            return 0;

        case WM_SIZE:
            layout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = px(720);
            mmi->ptMinTrackSize.y = px(500);
            return 0;
        }

        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
            drawButton(dis);
            return TRUE;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, RGB(27, 27, 27));
            SetBkColor(hdc, RGB(255, 255, 255));
            return (LRESULT)g_brWhite;
        }

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
            setStatus(*p, StatusKind::Error);
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
                setStatus(scanSummary(), code == 0 ? StatusKind::Ok : StatusKind::Error);
            } else {
                if (code != 0) {
                    appendText(L"\r\n[解读未完成，退出码 " + std::to_wstring(code) + L"；原因见上方]\r\n");
                    setStatus(L"解读失败 · 检查 api_key", StatusKind::Error);
                } else {
                    setStatus(L"解读完成", StatusKind::Ok);
                }
            }
            return 0;
        }

        case WM_DESTROY:
            if (g_job) { CloseHandle(g_job); g_job = nullptr; }
            destroyFonts();
            if (g_fontEdit) { DeleteObject(g_fontEdit); g_fontEdit = nullptr; }
            if (g_brWhite) { DeleteObject(g_brWhite); g_brWhite = nullptr; }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    SetProcessDPIAware();
    {
        HDC dc = GetDC(nullptr);
        g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        if (g_dpi <= 0) g_dpi = 96;
    }

    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gsi, nullptr) != Ok) return 1;

    createFonts();
    g_fontEdit = CreateFontW(-px(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_brWhite = CreateSolidBrush(RGB(255, 255, 255));

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"NewsDeskWindow";
    if (!RegisterClassExW(&wc)) return 1;

    WNDCLASSEXW sc;
    ZeroMemory(&sc, sizeof(sc));
    sc.cbSize = sizeof(sc);
    sc.style = CS_HREDRAW | CS_VREDRAW;
    sc.lpfnWndProc = SbProc;
    sc.hInstance = hInst;
    sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    sc.hbrBackground = nullptr;
    sc.lpszClassName = L"DeskScroll";
    if (!RegisterClassExW(&sc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"政策解读台 — 新闻联播 / 人民日报",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                1020, 730, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(token);
    return 0;
}
