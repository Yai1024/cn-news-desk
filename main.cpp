// ============================================================================
//  人民日报 & 新闻联播 每日内容抓取器
//  按《人民日报_新闻联播_每日获取方案.txt》实现：
//    新闻联播：优先 DuckBurnIncense/xin-wen-lian-bo 的当日 Markdown，
//              失败后回退央视网日页 + 每条新闻的 #content_area。
//    人民日报：解析 paper.people.com.cn 电子版 node_*.html，
//              逐篇抓取 <h1> 与 #ozoom 正文。
//  输出：output/新闻联播_YYYYMMDD.txt、output/人民日报_YYYYMMDD.txt
//  说明：内容版权归原媒体所有，仅供个人阅读、学习研究，请勿转载或商用。
//  许可：程序代码 MIT License；抓取内容不授权。完整许可范围、第三方来源与
//        变动记录见 LICENSE.md。
// ============================================================================

#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

using std::string;
using std::wstring;

static const string SEP(78, '=');
static const string SEP2(78, '-');

// ----------------------------------------------------------------------------
// 基础工具：编码 / 文件 / 时间
// ----------------------------------------------------------------------------

static const size_t NPOS = string::npos;

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

static bool isSpaceCh(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static string trim(const string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isSpaceCh(s[a])) ++a;
    while (b > a && isSpaceCh(s[b - 1])) --b;
    return s.substr(a, b - a);
}

static string lowerAscii(const string& s) {
    string r = s;
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return r;
}

static size_t findCI(const string& hay, const string& needle, size_t from = 0) {
    if (needle.empty()) return from;
    if (from >= hay.size()) return NPOS;
    auto it = std::search(hay.begin() + from, hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
                          });
    return it == hay.end() ? NPOS : (size_t)(it - hay.begin());
}

static wstring joinPath(const wstring& a, const wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    wchar_t last = a.back();
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

static wstring exeDir() {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    if (n == 0) return L".";
    wstring p(buf.data(), n);
    size_t pos = p.find_last_of(L"\\/");
    return pos == wstring::npos ? wstring(L".") : p.substr(0, pos);
}

static bool isAbsolutePath(const wstring& p) {
    if (p.size() >= 2 && p[1] == L':') return true;
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') return true;
    return !p.empty() && (p[0] == L'\\' || p[0] == L'/');
}

static bool ensureDir(const wstring& path) {
    if (path.empty()) return true;
    wstring cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        wchar_t c = path[i];
        if ((c == L'\\' || c == L'/') && cur.size() > 1) {
            if (cur.size() == 3 && cur[1] == L':') continue;
            CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    CreateDirectoryW(path.c_str(), nullptr);
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool writeFileUtf8(const wstring& path, const string& utf8, bool withBom) {
    // 先写临时文件再改名，避免中途退出留下“看起来已抓完”的半截文件
    wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    if (withBom) {
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        fwrite(bom, 1, 3, f);
    }
    size_t n = fwrite(utf8.data(), 1, utf8.size(), f);
    bool ok = (n == utf8.size()) && (fflush(f) == 0);
    fclose(f);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

static long long fileSize(const wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return -1;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return -1;
    return ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

static string toCrlf(const string& s) {
    string r;
    r.reserve(s.size() + s.size() / 20);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') continue;
        if (s[i] == '\n') r += "\r\n";
        else r.push_back(s[i]);
    }
    return r;
}

static string normalizeLf(const string& s) {
    string r;
    r.reserve(s.size());
    for (char c : s)
        if (c != '\r') r.push_back(c);
    return r;
}

// 日期（Howard Hinnant 的公历算法）
struct Date {
    int y = 1970;
    int m = 1;
    int d = 1;
};

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static Date civilFromDays(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)yoe + (int)era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    Date r;
    r.y = y;
    r.m = (int)m;
    r.d = (int)d;
    return r;
}

static Date addDays(const Date& dt, int delta) {
    return civilFromDays(daysFromCivil(dt.y, (unsigned)dt.m, (unsigned)dt.d) + delta);
}

static bool sameDate(const Date& a, const Date& b) {
    return a.y == b.y && a.m == b.m && a.d == b.d;
}

static string pad2(int v) {
    char b[8];
    sprintf(b, "%02d", v);
    return string(b);
}

static string ymd(const Date& d) {
    char b[16];
    sprintf(b, "%04d%02d%02d", d.y, d.m, d.d);
    return string(b);
}

static string ym(const Date& d) {
    char b[16];
    sprintf(b, "%04d%02d", d.y, d.m);
    return string(b);
}

static string dateCn(const Date& d) {
    char b[40];
    sprintf(b, "%04d年%02d月%02d日", d.y, d.m, d.d);
    return string(b);
}

static string weekdayCn(const Date& d) {
    static const char* names[] = {"日", "一", "二", "三", "四", "五", "六"};
    int64_t z = daysFromCivil(d.y, (unsigned)d.m, (unsigned)d.d);
    int wd = (int)(((z + 4) % 7 + 7) % 7);  // 1970-01-01 是星期四
    return string("星期") + names[wd];
}

static string nowStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[40];
    sprintf(b, "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
            st.wMinute, st.wSecond);
    return string(b);
}

static string clockStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[16];
    sprintf(b, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return string(b);
}

static string humanSize(long long n) {
    char b[32];
    if (n >= 1024 * 1024)
        sprintf(b, "%.1f MB", (double)n / 1024.0 / 1024.0);
    else if (n >= 1024)
        sprintf(b, "%.1f KB", (double)n / 1024.0);
    else
        sprintf(b, "%lld B", n);
    return string(b);
}

static Date today() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    Date d;
    d.y = st.wYear;
    d.m = st.wMonth;
    d.d = st.wDay;
    return d;
}

// ----------------------------------------------------------------------------
// 日志
// ----------------------------------------------------------------------------
struct Logger {
    FILE* file = nullptr;

    void open(const wstring& path) {
        bool needBom = fileSize(path) <= 0;
        file = _wfopen(path.c_str(), L"ab");
        if (file && needBom) {
            const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
            fwrite(bom, 1, 3, file);
            fflush(file);
        }
    }

    void line(const string& s) {
        string msg = "[" + clockStamp() + "] " + s + "\n";
        fwrite(msg.data(), 1, msg.size(), stdout);
        fflush(stdout);
        if (file) {
            fwrite(msg.data(), 1, msg.size(), file);
            fflush(file);
        }
    }

    void raw(const string& s) {
        fwrite(s.data(), 1, s.size(), stdout);
        fflush(stdout);
        if (file) {
            fwrite(s.data(), 1, s.size(), file);
            fflush(file);
        }
    }

    void close() {
        if (file) {
            fclose(file);
            file = nullptr;
        }
    }
};

// ----------------------------------------------------------------------------
// 极简 HTML 文本处理
// ----------------------------------------------------------------------------
static void appendUtf8(string& out, unsigned cp) {
    if (cp <= 0x7F) {
        out.push_back((char)cp);
    } else if (cp <= 0x7FF) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

static string decodeEntity(const string& e) {
    if (e.empty()) return "&;";
    if (e[0] == '#') {
        unsigned cp = 0;
        bool ok = false;
        if (e.size() > 1 && (e[1] == 'x' || e[1] == 'X')) {
            for (size_t i = 2; i < e.size(); ++i) {
                char c = e[i];
                int v;
                if (c >= '0' && c <= '9') v = c - '0';
                else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
                else { ok = false; break; }
                cp = cp * 16 + (unsigned)v;
                ok = true;
            }
        } else {
            for (size_t i = 1; i < e.size(); ++i) {
                char c = e[i];
                if (c < '0' || c > '9') { ok = false; break; }
                cp = cp * 10 + (unsigned)(c - '0');
                ok = true;
            }
        }
        if (ok && cp > 0 && cp <= 0x10FFFF) {
            string s;
            appendUtf8(s, cp);
            return s;
        }
        return "&" + e + ";";
    }

    static const std::map<string, string> named = {
        {"amp", "&"},       {"lt", "<"},        {"gt", ">"},
        {"quot", "\""},     {"apos", "'"},      {"nbsp", " "},
        {"ensp", " "},      {"emsp", " "},      {"thinsp", " "},
        {"ldquo", "“"},     {"rdquo", "”"},     {"lsquo", "‘"},
        {"rsquo", "’"},     {"hellip", "…"},    {"mdash", "—"},
        {"ndash", "–"},     {"middot", "·"},    {"times", "×"},
        {"divide", "÷"},    {"copy", "©"},      {"reg", "®"},
        {"trade", "™"},     {"deg", "°"},       {"plusmn", "±"},
        {"laquo", "«"},     {"raquo", "»"},     {"bull", "•"},
        {"zwnj", ""},       {"zwj", ""},
    };
    auto it = named.find(lowerAscii(e));
    if (it != named.end()) return it->second;
    return "&" + e + ";";
}

static bool isBlockTag(const string& name) {
    static const std::set<string> tags = {
        "p", "div", "br", "li", "ul", "ol", "tr", "td", "th", "table", "thead", "tbody",
        "h1", "h2", "h3", "h4", "h5", "h6", "section", "article", "header", "footer",
        "blockquote", "pre", "hr", "figure", "figcaption", "p",
    };
    return tags.count(name) > 0;
}

static string htmlToText(const string& html) {
    string out;
    out.reserve(html.size() / 2);
    auto addNewline = [&]() {
        if (!out.empty() && out.back() != '\n') out.push_back('\n');
    };

    for (size_t i = 0; i < html.size();) {
        unsigned char c = (unsigned char)html[i];
        if (c == '<') {
            size_t gt = html.find('>', i);
            if (gt == NPOS) break;
            string inner = trim(html.substr(i + 1, gt - i - 1));
            bool closing = !inner.empty() && inner[0] == '/';
            string name = lowerAscii(inner);
            if (closing) name = name.substr(1);
            size_t sp = name.find_first_of(" \t\r\n/");
            if (sp != NPOS) name = name.substr(0, sp);
            if (isBlockTag(name)) addNewline();
            i = gt + 1;
        } else if (c == '&') {
            size_t semi = html.find(';', i);
            if (semi != NPOS && semi - i <= 12) {
                out += decodeEntity(html.substr(i + 1, semi - i - 1));
                i = semi + 1;
            } else {
                out.push_back('&');
                ++i;
            }
        } else {
            if (c == '\r') { ++i; continue; }
            if (c == '\t' || c == '\n') c = ' ';
            if (c == ' ') {
                if (out.empty() || out.back() == ' ' || out.back() == '\n') { ++i; continue; }
            }
            out.push_back((char)c);
            ++i;
        }
    }

    string result;
    size_t pos = 0;
    while (pos <= out.size()) {
        size_t nl = out.find('\n', pos);
        string line = trim(out.substr(pos, nl == NPOS ? NPOS : nl - pos));
        if (!line.empty()) {
            if (!result.empty()) result.push_back('\n');
            result += line;
        }
        if (nl == NPOS) break;
        pos = nl + 1;
    }
    return result;
}

static string removeBetweenCI(string h, const string& startTag, const string& endTag) {
    size_t pos = 0;
    while ((pos = findCI(h, startTag, pos)) != NPOS) {
        size_t e = findCI(h, endTag, pos);
        if (e == NPOS) {
            h.erase(pos);
            break;
        }
        h.erase(pos, e + endTag.size() - pos);
    }
    return h;
}

static string removeComments(string h) {
    size_t pos = 0;
    while ((pos = h.find("<!--", pos)) != NPOS) {
        size_t e = h.find("-->", pos + 4);
        if (e == NPOS) {
            h.erase(pos);
            break;
        }
        h.erase(pos, e + 3 - pos);
    }
    return h;
}

static string normalizeCharset(const string& raw) {
    string head = raw.substr(0, std::min<size_t>(raw.size(), 4096));
    string low = lowerAscii(head);
    bool gb = low.find("gb2312") != NPOS || low.find("gbk") != NPOS ||
              low.find("gb18030") != NPOS;
    if (gb) {
        int n = MultiByteToWideChar(936, 0, raw.data(), (int)raw.size(), nullptr, 0);
        if (n > 0) {
            wstring w((size_t)n, L'\0');
            MultiByteToWideChar(936, 0, raw.data(), (int)raw.size(), &w[0], n);
            return wideToUtf8(w);
        }
    }
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB &&
        (unsigned char)raw[2] == 0xBF)
        return raw.substr(3);
    return raw;
}

static string cleanHtml(const string& raw) {
    string h = normalizeCharset(raw);
    h = removeBetweenCI(h, "<script", "</script>");
    h = removeBetweenCI(h, "<style", "</style>");
    h = removeComments(h);
    return h;
}

// 找到包含 marker 的元素（marker 形如 id="ozoom"），按同名标签配对，返回内部 HTML。
static string extractElement(const string& html, const string& marker, size_t searchFrom = 0) {
    size_t p = searchFrom;
    while ((p = findCI(html, marker, p)) != NPOS) {
        size_t lt = html.rfind('<', p);
        size_t gt = html.find('>', p);
        if (lt == NPOS || gt == NPOS || p < lt || p > gt) {
            p += marker.size();
            continue;
        }
        size_t ns = lt + 1;
        while (ns < gt && isSpaceCh(html[ns])) ++ns;
        if (ns < gt && html[ns] == '/') { p = gt + 1; continue; }
        size_t ne = ns;
        while (ne < gt && !isSpaceCh(html[ne]) && html[ne] != '/' && html[ne] != '>') ++ne;
        string tag = lowerAscii(html.substr(ns, ne - ns));
        if (tag.empty()) { p = gt + 1; continue; }
        if (gt > lt && html[gt - 1] == '/') return string();

        int depth = 1;
        size_t i = gt + 1;
        while (i < html.size() && depth > 0) {
            size_t lo = findCI(html, "<" + tag, i);
            size_t lc = findCI(html, "</" + tag, i);
            if (lo == NPOS && lc == NPOS) return string();
            size_t l;
            bool closing = false;
            if (lo == NPOS) { l = lc; closing = true; }
            else if (lc == NPOS) { l = lo; }
            else if (lo < lc) { l = lo; }
            else { l = lc; closing = true; }

            // 开标签名从 l+1 开始，闭标签名从 l+2 开始（多一个 '/'）
            size_t after = l + (closing ? 2 : 1) + tag.size();
            bool boundary = after >= html.size() || isSpaceCh(html[after]) ||
                            html[after] == '>' || html[after] == '/';
            size_t g = html.find('>', l);
            if (g == NPOS) return string();
            if (!boundary) { i = l + 1; continue; }

            if (closing) {
                --depth;
            } else {
                bool selfClose = g > l && html[g - 1] == '/';
                if (!selfClose) ++depth;
            }
            if (depth == 0) return html.substr(gt + 1, l - (gt + 1));
            i = g + 1;
        }
        p = gt + 1;
    }
    return string();
}

static string extractFirstTag(const string& html, const string& tag) {
    size_t i = 0;
    while ((i = findCI(html, "<" + tag, i)) != NPOS) {
        size_t after = i + 1 + tag.size();
        bool boundary = after < html.size() &&
                        (isSpaceCh(html[after]) || html[after] == '>' || html[after] == '/');
        if (!boundary) { i = after; continue; }
        size_t gt = html.find('>', i);
        if (gt == NPOS) return string();
        if (gt > i && html[gt - 1] == '/') return string();
        size_t close = findCI(html, "</" + tag, i);
        if (close == NPOS) return string();
        return html.substr(gt + 1, close - gt - 1);
    }
    return string();
}

// ----------------------------------------------------------------------------
// HTTP (WinHTTP)
// ----------------------------------------------------------------------------
struct HttpResult {
    int status = 0;
    string body;
    string error;
};

static const wchar_t* kUserAgent =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    L"Chrome/126.0.0.0 Safari/537.36";

static HttpResult httpGet(const string& url, bool useProxy, const wstring& proxy,
                          int recvSec) {
    HttpResult r;
    wstring wurl = utf8ToWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;

    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
        r.error = "URL 解析失败";
        return r;
    }

    wstring host(uc.lpszHostName, uc.dwHostNameLength);
    wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength && uc.lpszExtraInfo)
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    DWORD accessType = useProxy ? WINHTTP_ACCESS_TYPE_NAMED_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY;
    HINTERNET hs = WinHttpOpen(kUserAgent, accessType,
                               useProxy ? proxy.c_str() : WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) {
        char b[64];
        sprintf(b, "WinHttpOpen 失败 (%lu)", GetLastError());
        r.error = b;
        return r;
    }

    WinHttpSetTimeouts(hs, 15000, 15000, 30000, recvSec * 1000);
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hs, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    HINTERNET hc = WinHttpConnect(hs, host.c_str(), uc.nPort, 0);
    if (!hc) {
        char b[64];
        sprintf(b, "连接失败 (%lu)", GetLastError());
        r.error = b;
        WinHttpCloseHandle(hs);
        return r;
    }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hr = WinHttpOpenRequest(hc, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hr) {
        char b[64];
        sprintf(b, "创建请求失败 (%lu)", GetLastError());
        r.error = b;
        WinHttpCloseHandle(hc);
        WinHttpCloseHandle(hs);
        return r;
    }

    WinHttpAddRequestHeaders(
        hr,
        L"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
        L"Accept-Language: zh-CN,zh;q=0.9,en;q=0.6\r\n",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    BOOL ok = WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                                 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hr, nullptr);
    if (!ok) {
        char b[64];
        sprintf(b, "请求失败 (%lu)", GetLastError());
        r.error = b;
        WinHttpCloseHandle(hr);
        WinHttpCloseHandle(hc);
        WinHttpCloseHandle(hs);
        return r;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)status;

    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hr, &avail) && avail > 0) {
        string buf(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(hr, &buf[0], avail, &got)) {
            char b[64];
            sprintf(b, "读取响应失败 (%lu)", GetLastError());
            r.error = b;
            break;
        }
        if (got == 0) break;
        r.body.append(buf.data(), got);
    }

    WinHttpCloseHandle(hr);
    WinHttpCloseHandle(hc);
    WinHttpCloseHandle(hs);
    return r;
}

static HttpResult getWithRetry(const string& url, bool useProxy, const wstring& proxy,
                               int attempts, int recvSec) {
    HttpResult last;
    for (int i = 0; i < attempts; ++i) {
        last = httpGet(url, useProxy, proxy, recvSec);
        if (last.status == 200) return last;
        bool retryable = last.status == 0 || last.status == 429 || last.status >= 500;
        if (!retryable || i + 1 >= attempts) break;
        Sleep(800 * (i + 1));
    }
    return last;
}

// ----------------------------------------------------------------------------
// 选项 / 配置
// ----------------------------------------------------------------------------
struct Options {
    Date target;
    bool hasDate = false;
    wstring outDir;
    int days = 7;  // 向前补抓的天数（含目标日）
    bool strict = false;
    int delayMs = 1000;
    int pageFrom = 1;
    int pageTo = 999;
    bool pagesSpecified = false;  // 用 --pages 调试时，文件名加“_部分”，不阻塞正式归档
    bool onlyRmrb = false;
    bool onlyXwlb = false;
    bool force = false;
    bool help = false;
    string proxy;  // 留空表示不使用代理；由 config.ini 提供本机代理
    bool githubProxy = true;
    string xwlbSource = "auto";  // auto | github | govopen | cctv
};

enum class DateResult { Saved, Skipped, Missing, Failed, Partial };

struct WindowSummary {
    int saved = 0;
    int skipped = 0;
    int missing = 0;
    int failed = 0;
    int partial = 0;
    bool anyAvailable() const { return saved + skipped + partial > 0; }
};

static bool parseDateString(const string& s, Date& out) {
    int y = 0, m = 0, d = 0;
    if (s.size() == 8 &&
        std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        y = std::stoi(s.substr(0, 4));
        m = std::stoi(s.substr(4, 2));
        d = std::stoi(s.substr(6, 2));
    } else if (s.size() == 10 && s[4] == '-' && s[7] == '-' &&
               std::all_of(s.begin(), s.end(), [](char c) { return c == '-' || (c >= '0' && c <= '9'); })) {
        y = std::stoi(s.substr(0, 4));
        m = std::stoi(s.substr(5, 2));
        d = std::stoi(s.substr(8, 2));
    } else {
        return false;
    }
    if (m < 1 || m > 12 || d < 1 || d > 31) return false;
    Date check = civilFromDays(daysFromCivil(y, (unsigned)m, (unsigned)d));
    if (check.y != y || check.m != m || check.d != d) return false;
    out = check;
    return true;
}

static std::map<string, string> loadConfig(const wstring& path) {
    std::map<string, string> kv;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return kv;
    string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    data = normalizeCharset(data);
    size_t pos = 0;
    while (pos <= data.size()) {
        size_t nl = data.find('\n', pos);
        string line = trim(data.substr(pos, nl == NPOS ? NPOS : nl - pos));
        if (nl == NPOS) pos = data.size() + 1;
        else pos = nl + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == NPOS) continue;
        string k = trim(line.substr(0, eq));
        string v = trim(line.substr(eq + 1));
        if (!k.empty()) kv[k] = v;
    }
    return kv;
}

static string normalizeProxy(string p) {
    p = trim(p);
    if (p.rfind("http://", 0) == 0) p = p.substr(7);
    else if (p.rfind("https://", 0) == 0) p = p.substr(8);
    while (!p.empty() && (p.back() == '/' || p.back() == ' ')) p.pop_back();
    return p;
}

static void printHelp() {
    printf("人民日报 / 新闻联播 每日内容抓取器\n\n");
    printf("用法：fetch_news.exe [选项]\n\n");
    printf("  --date YYYY-MM-DD    指定日期（默认今天），也接受 YYYYMMDD\n");
    printf("  --out DIR            输出目录（默认程序目录下 output）\n");
    printf("  --days N             扫描最近 N 天，只补缺失日期（默认 7）\n");
    printf("  --strict             只抓目标日期（等价于 --days 1）\n");
    printf("  --delay-ms N         请求间隔毫秒（默认 1000）\n");
    printf("  --pages A-B          人民日报只抓指定版面（调试用，文件名会加 _部分）\n");
    printf("  --only rmrb|xwlb     只抓其中一家\n");
    printf("  --force              已存在文件也重新抓取\n");
    printf("  --proxy HOST:PORT    访问 GitHub 的代理（默认读 config.ini；留空则直连）\n");
    printf("  --no-github-proxy    访问 GitHub 不走代理\n");
    printf("  --xwlb-source auto|github|govopen|cctv  新闻联播来源（默认 auto）\n");
    printf("                       auto = GitHub -> GovOpenData -> 央视网，逐级回退\n");
    printf("  --help               显示本帮助\n\n");
    printf("输出文件名自带日期：output\\新闻联播_YYYYMMDD.txt、output\\人民日报_YYYYMMDD.txt\n");
}

// ----------------------------------------------------------------------------
// 新闻联播
// ----------------------------------------------------------------------------
struct XwlbContent {
    string text;
    string sourceName;
    string sourceUrl;
    int items = 0;
    bool fromGithub = false;
    bool complete = true;  // 央视路径个别条目失败时为 false
    int failed = 0;
};

static string stripVideoPrefix(string s) {
    s = trim(s);
    // "[视频]" 是 1+6+1 个字节，不能用 substr(6)
    if (s.rfind("[视频]", 0) == 0) s = trim(s.substr(8));
    return s;
}

static std::vector<string> extractCctvLinks(const string& html, const Date& d) {
    std::vector<string> links;
    std::set<string> seen;
    std::regex re(R"((?:https?:)?//tv\.cctv\.com/(\d{4})/(\d{2})/(\d{2})/VIDE[A-Za-z0-9]+\.shtml)");
    auto begin = std::sregex_iterator(html.begin(), html.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::smatch m = *it;
        if (std::stoi(m[1].str()) != d.y || std::stoi(m[2].str()) != d.m ||
            std::stoi(m[3].str()) != d.d)
            continue;
        string url = m[0].str();
        if (url.rfind("//", 0) == 0) url = "https:" + url;
        else if (url.rfind("http://", 0) == 0)
            url = "https://" + url.substr(7);
        if (seen.insert(url).second) links.push_back(url);
    }
    return links;
}

// 本次运行中 GitHub 直连是否已确认不可用：确认后后续日期直接走代理，
// 避免为每一天都等一次超时（无代理机器则直接改用其它来源）。
static bool gGithubDirectDown = false;

static bool fetchXwlbGithub(const Date& d, const Options& opt, Logger& log, XwlbContent& c) {
    string url = "https://raw.githubusercontent.com/DuckBurnIncense/xin-wen-lian-bo/master/news/" +
                 ymd(d) + ".md";
    log.line("  尝试 GitHub 源：" + url);

    bool hasProxy = opt.githubProxy && !opt.proxy.empty();
    HttpResult r;

    // 先直连；无代理的机器同样可以工作
    if (!gGithubDirectDown) {
        r = getWithRetry(url, false, wstring(), 1, 25);
        if (r.status == 0 || r.status == 403 || r.status >= 500) {
            gGithubDirectDown = true;
            log.line("  GitHub 直连不可用，本次运行后续日期不再重试直连");
        }
    }

    bool networkFailure = (r.status == 0 || r.status == 403 || r.status >= 500);
    if (r.status != 200 && hasProxy && networkFailure) {
        log.line("  尝试代理 " + opt.proxy);
        HttpResult r2 = getWithRetry(url, true, utf8ToWide(opt.proxy), 2, 30);
        if (r2.status == 200) r = r2;
    }

    if (r.status != 200) {
        log.line("  GitHub 源不可用（HTTP " + std::to_string(r.status) +
                 (r.error.empty() ? "" : "，" + r.error) + "）");
        return false;
    }
    if (r.body.size() < 200 || r.body.find("新闻联播") == NPOS) {
        log.line("  GitHub 返回内容异常，忽略");
        return false;
    }

    c.text = normalizeLf(r.body);
    c.sourceName = "GitHub：DuckBurnIncense/xin-wen-lian-bo";
    c.sourceUrl = url;
    c.fromGithub = true;
    int count = 0;
    size_t pos = 0;
    while ((pos = c.text.find("\n### ", pos)) != NPOS) {
        ++count;
        pos += 5;
    }
    c.items = std::max(count, 1);
    return true;
}

static bool fetchXwlbCctv(const Date& d, const Options& opt, Logger& log, XwlbContent& c) {
    string dayUrl = "https://tv.cctv.com/lm/xwlb/day/" + ymd(d) + ".shtml";
    log.line("  回退央视网日页：" + dayUrl);

    HttpResult day = getWithRetry(dayUrl, false, wstring(), 2, 30);
    if (day.status != 200) {
        log.line("  央视日页不可用（HTTP " + std::to_string(day.status) + "）");
        return false;
    }

    string dayHtml = cleanHtml(day.body);
    std::vector<string> links = extractCctvLinks(dayHtml, d);
    if (links.empty()) {
        log.line("  央视日页中没有 " + ymd(d) + " 的新闻链接");
        return false;
    }
    log.line("  日页解析到 " + std::to_string(links.size()) + " 条新闻，开始逐条抓取");

    string body;
    int items = 0;
    int failed = 0;
    for (size_t i = 0; i < links.size(); ++i) {
        if (i > 0) Sleep((DWORD)opt.delayMs);
        HttpResult r = getWithRetry(links[i], false, wstring(), 2, 30);
        if (r.status != 200) {
            ++failed;
            log.line("  [" + std::to_string(i + 1) + "/" + std::to_string(links.size()) +
                     "] 抓取失败 HTTP " + std::to_string(r.status));
            continue;
        }
        string html = cleanHtml(r.body);
        string title = stripVideoPrefix(htmlToText(extractElement(html, "class=\"tit\"")));
        if (title.empty() || title == "视频简介")
            title = stripVideoPrefix(htmlToText(extractFirstTag(html, "title")));

        string bodyHtml = extractElement(html, "id=\"content_area\"");
        string text = htmlToText(bodyHtml);
        if (text.empty()) text = htmlToText(extractElement(html, "class=\"brief\""));

        if (title.empty() && text.empty()) {
            ++failed;
            continue;
        }
        if (text.empty()) ++failed;  // 只有标题没有正文，不算完整
        if (title.empty()) title = "（无标题）";
        ++items;

        body += "【" + std::to_string(items) + "】" + title + "\n";
        body += "链接：" + links[i] + "\n";
        body += SEP2 + "\n";
        body += text.empty() ? "（本条没有文字稿，请访问原链接查看）" : text;
        body += "\n\n";

        log.line("  [" + std::to_string(i + 1) + "/" + std::to_string(links.size()) + "] " +
                 title);
    }

    if (items == 0) {
        log.line("  所有新闻页都未取到正文");
        return false;
    }
    if (failed > 0)
        log.line("  有 " + std::to_string(failed) + " 条抓取失败");

    c.text = body;
    c.sourceName = "央视网《新闻联播》";
    c.sourceUrl = dayUrl;
    c.items = items;
    c.complete = (failed == 0);
    c.failed = failed;
    return true;
}

// 无代理环境下可用的全文来源（第三方，作为 GitHub 的回退）
static int buildGovOpenText(const string& html, string& out) {
    string content = extractElement(html, "class=\"article-content\"");
    if (content.empty()) content = extractElement(html, "class=\"content-body\"");
    if (content.empty()) return 0;

    string text;
    int n = 0;
    size_t pos = 0;
    while (true) {
        size_t h = findCI(content, "class=\"content-heading\"", pos);
        if (h == NPOS) break;
        size_t lt = content.rfind('<', h);
        size_t gt = content.find('>', h);
        if (lt == NPOS || gt == NPOS) break;
        size_t close = findCI(content, "</h2", gt);
        if (close == NPOS) break;

        string title = trim(htmlToText(content.substr(gt + 1, close - gt - 1)));
        size_t next = findCI(content, "class=\"content-heading\"", close + 5);
        size_t end = (next == NPOS) ? content.size() : content.rfind('<', next);
        string bodyText = trim(htmlToText(content.substr(close + 5, end - (close + 5))));

        if (!title.empty() || !bodyText.empty()) {
            ++n;
            text += "【" + std::to_string(n) + "】" +
                    (title.empty() ? string("（无标题）") : title) + "\n";
            text += SEP2 + "\n";
            text += bodyText.empty() ? "（本条没有文字稿，请访问原链接查看）" : bodyText;
            text += "\n\n";
        }
        pos = close + 5;
        if (next == NPOS) break;
    }

    if (n == 0) {
        string whole = trim(htmlToText(content));
        if (whole.size() < 300) return 0;
        out = whole;
        return 1;
    }
    out = text;
    return n;
}

static bool fetchXwlbGovOpen(const Date& d, const Options& opt, Logger& log,
                             XwlbContent& c) {
    (void)opt;
    string url = "https://cn.govopendata.com/xinwenlianbo/" + ymd(d) + "/";
    log.line("  尝试 GovOpenData 文字版：" + url);

    HttpResult r = getWithRetry(url, false, wstring(), 2, 30);
    if (r.status != 200) {
        log.line("  GovOpenData 不可用（HTTP " + std::to_string(r.status) + "）");
        return false;
    }
    string html = cleanHtml(r.body);
    if (html.find("content-heading") == NPOS && html.find("article-content") == NPOS) {
        log.line("  GovOpenData 页面没有正文结构");
        return false;
    }

    string text;
    int items = buildGovOpenText(html, text);
    if (items == 0 || text.size() < 300) {
        log.line("  GovOpenData 正文为空或过短");
        return false;
    }

    c.text = text;
    c.sourceName = "cn.govopendata.com（第三方文字版）";
    c.sourceUrl = url;
    c.items = items;
    return true;
}

static bool fetchXwlbForDate(const Date& d, const Options& opt, Logger& log,
                             XwlbContent& c) {
    bool wantGithub = (opt.xwlbSource == "auto" || opt.xwlbSource == "github");
    bool wantGovOpen = (opt.xwlbSource == "auto" || opt.xwlbSource == "govopen");
    bool wantCctv = (opt.xwlbSource == "auto" || opt.xwlbSource == "cctv");

    if (wantGithub && fetchXwlbGithub(d, opt, log, c)) return true;
    if (wantGovOpen && fetchXwlbGovOpen(d, opt, log, c)) return true;
    if (wantCctv && fetchXwlbCctv(d, opt, log, c)) return true;
    return false;
}

// 抓取并保存某一期新闻联播；不会跨日期回退（补抓由外层窗口负责）
static DateResult saveXwlbDate(const Date& d, const Options& opt, const wstring& outDir,
                               Logger& log) {
    wstring basePath = joinPath(outDir, L"新闻联播_" + utf8ToWide(ymd(d)));
    wstring fullPath = basePath + L".txt";
    long long existing = fileSize(fullPath);
    if (!opt.force && existing > 500) {
        log.line("  已存在，跳过：" + wideToUtf8(fullPath) + "（" + humanSize(existing) + "）");
        return DateResult::Skipped;
    }

    XwlbContent c;
    if (!fetchXwlbForDate(d, opt, log, c)) {
        log.line("  " + dateCn(d) + " 暂无可用内容（可能尚未更新，下次运行会自动补）");
        return DateResult::Missing;
    }

    bool partial = !c.complete;
    wstring path = partial ? (basePath + L"_部分.txt") : fullPath;
    if (partial)
        log.line("  有 " + std::to_string(c.failed) +
                 " 条抓取失败，先保存为部分文件，下次运行会重试");

    string out;
    out += SEP + "\n";
    out += "新闻联播 文字稿\n";
    out += "日期：" + dateCn(d) + "（" + weekdayCn(d) + "）\n";
    if (partial) out += "说明：有 " + std::to_string(c.failed) +
                        " 条抓取失败，本文件可能不完整，下次运行会重试。\n";
    out += "来源：" + c.sourceName + "\n";
    out += "链接：" + c.sourceUrl + "\n";
    out += "抓取时间：" + nowStamp() + "\n";
    out += "条数：" + std::to_string(c.items) + "\n";
    out += "版权：内容版权归中央广播电视总台所有，仅供个人学习研究，请勿转载或商用。\n";
    out += SEP + "\n\n";
    out += c.text;
    if (!out.empty() && out.back() != '\n') out.push_back('\n');

    if (!writeFileUtf8(path, toCrlf(out), true)) {
        log.line("  写入失败：" + wideToUtf8(path));
        return DateResult::Failed;
    }
    long long sz = fileSize(path);
    log.line("  已保存：" + wideToUtf8(path) + "（" + std::to_string(c.items) + " 条，" +
             humanSize(sz) + "）");
    if (!partial) DeleteFileW((basePath + L"_部分.txt").c_str());
    return partial ? DateResult::Partial : DateResult::Saved;
}

static WindowSummary runXwlbWindow(const Options& opt, const wstring& outDir, Logger& log) {
    WindowSummary sum;
    int days = std::max(1, opt.days);
    log.line("扫描最近 " + std::to_string(days) + " 天（" + dateCn(addDays(opt.target, -(days - 1))) +
             " 至 " + dateCn(opt.target) + "），只补缺失日期");

    int todo = 0;
    for (int back = 0; back < days; ++back) {
        Date d = addDays(opt.target, -back);
        wstring p = joinPath(outDir, L"新闻联播_" + utf8ToWide(ymd(d)) + L".txt");
        if (opt.force || fileSize(p) <= 500) ++todo;
    }
    if (todo > 0) log.line("  本次预计抓取 " + std::to_string(todo) + " 天");

    // 从新到旧，先保证最近的日期拿到
    for (int back = 0; back < days; ++back) {
        Date d = addDays(opt.target, -back);
        log.line("—— 新闻联播 " + dateCn(d) + " ——");
        switch (saveXwlbDate(d, opt, outDir, log)) {
            case DateResult::Saved: ++sum.saved; break;
            case DateResult::Partial: ++sum.partial; break;
            case DateResult::Skipped: ++sum.skipped; break;
            case DateResult::Missing: ++sum.missing; break;
            case DateResult::Failed: ++sum.failed; break;
        }
    }

    log.line("新闻联播汇总：新增 " + std::to_string(sum.saved) + " 天，部分 " +
             std::to_string(sum.partial) + " 天，已存在 " + std::to_string(sum.skipped) +
             " 天，暂无 " + std::to_string(sum.missing) + " 天，失败 " +
             std::to_string(sum.failed) + " 天");
    return sum;
}

// ----------------------------------------------------------------------------
// 人民日报
// ----------------------------------------------------------------------------
struct RmrbPage {
    int no = 1;
    string name;
    std::vector<string> urls;
};

static int detectPageCount(const string& html) {
    std::regex re(R"(node_(\d{2,3})\.html)");
    int maxNo = 1;
    auto begin = std::sregex_iterator(html.begin(), html.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        int no = std::stoi((*it)[1].str());
        if (no > maxNo && no < 100) maxNo = no;
    }
    return maxNo;
}

static std::vector<string> extractArticleUrls(const string& html, const Date& d) {
    std::vector<string> urls;
    std::set<string> seen;
    std::regex re(R"(content/(\d{6})/(\d{2})/content_(\d+)\.html)");
    auto begin = std::sregex_iterator(html.begin(), html.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::smatch m = *it;
        if (m[1].str() != ym(d) || std::stoi(m[2].str()) != d.d) continue;
        string url = "https://paper.people.com.cn/rmrb/pc/content/" + m[1].str() + "/" +
                     m[2].str() + "/content_" + m[3].str() + ".html";
        if (seen.insert(url).second) urls.push_back(url);
    }
    return urls;
}

static bool fetchRmrbForDate(const Date& d, const Options& opt, Logger& log,
                             string& outText, int& pageCount, int& articleCount,
                             bool& complete) {
    string layoutUrl =
        "https://paper.people.com.cn/rmrb/pc/layout/" + ym(d) + "/" + pad2(d.d) + "/node_01.html";
    log.line("  版面首页：" + layoutUrl);

    HttpResult first = getWithRetry(layoutUrl, false, wstring(), 2, 30);
    if (first.status != 200) {
        log.line("  版面首页不可用（HTTP " + std::to_string(first.status) + "）");
        return false;
    }
    string firstHtml = cleanHtml(first.body);
    int totalPages = detectPageCount(firstHtml);
    log.line("  检测到 " + std::to_string(totalPages) + " 个版面");

    int from = std::max(1, opt.pageFrom);
    int to = std::min(totalPages, opt.pageTo);
    if (to < from) {
        log.line("  --pages 范围与版面数不匹配");
        return false;
    }

    std::set<string> seenArticles;
    string body;
    int pagesDone = 0;
    int articlesDone = 0;
    int failed = 0;
    int emptyBodies = 0;

    for (int no = from; no <= to; ++no) {
        string html = firstHtml;
        if (no != 1) {
            Sleep((DWORD)opt.delayMs);
            string url = "https://paper.people.com.cn/rmrb/pc/layout/" + ym(d) + "/" +
                         pad2(d.d) + "/node_" + pad2(no) + ".html";
            HttpResult pr = getWithRetry(url, false, wstring(), 2, 30);
            if (pr.status != 200) {
                log.line("  第" + pad2(no) + "版抓取失败 HTTP " + std::to_string(pr.status));
                ++failed;
                continue;
            }
            html = cleanHtml(pr.body);
        }

        string pageName =
            trim(htmlToText(extractElement(html, "class=\"left ban\"")));
        if (pageName.empty()) pageName = "第" + pad2(no) + "版";

        std::vector<string> urls = extractArticleUrls(html, d);
        // 去重：同一篇文章可能出现在相邻版面或导航里
        std::vector<string> fresh;
        for (const string& u : urls)
            if (seenArticles.insert(u).second) fresh.push_back(u);
        if (fresh.empty()) continue;

        pageCount = std::max(pageCount, no);
        body += pageName + "\n";
        body += SEP + "\n\n";

        int idx = 0;
        for (const string& au : fresh) {
            if (articlesDone + failed > 0) Sleep((DWORD)opt.delayMs);
            HttpResult ar = getWithRetry(au, false, wstring(), 2, 30);
            if (ar.status != 200) {
                ++failed;
                log.line("  [" + pageName + "] 文章抓取失败 HTTP " +
                         std::to_string(ar.status) + "：" + au);
                continue;
            }
            string ah = cleanHtml(ar.body);
            string title = trim(htmlToText(extractFirstTag(ah, "h1")));
            if (title.empty())
                title = trim(htmlToText(extractFirstTag(ah, "title")));
            if (title.empty()) title = "（无标题）";

            // 版面责编、整版广告不是新闻正文，跳过
            if (title.find("责编") != NPOS || title == "广告") {
                log.line("  [" + pageName + "] 跳过非正文条目：" + title);
                continue;
            }

            string bodyHtml = extractElement(ah, "id=\"ozoom\"");
            if (bodyHtml.empty()) bodyHtml = extractElement(ah, "id=\"rwb_zw\"");
            if (bodyHtml.empty()) bodyHtml = extractElement(ah, "class=\"rm_txt_con\"");
            if (bodyHtml.empty()) bodyHtml = extractElement(ah, "class=\"show_text\"");
            string text = htmlToText(bodyHtml);
            if (text.empty()) {
                ++emptyBodies;
                text = "（正文为空，请访问原链接查看）";
            }

            ++idx;
            ++articlesDone;
            body += "【" + std::to_string(idx) + "】" + title + "\n";
            body += "链接：" + au + "\n\n";
            body += text + "\n\n";
            body += SEP2 + "\n";

            if (idx % 5 == 0 || idx == (int)fresh.size())
                log.line("  [" + pageName + "] " + std::to_string(idx) + "/" +
                         std::to_string(fresh.size()) + " " + title);
        }
        body += "\n";
        ++pagesDone;
    }

    pageCount = pagesDone;
    if (articlesDone == 0) {
        log.line("  没有取到任何文章");
        return false;
    }
    if (failed > 0) log.line("  有 " + std::to_string(failed) + " 个请求失败");
    log.line("  共完成 " + std::to_string(pagesDone) + " 个版面、" +
             std::to_string(articlesDone) + " 篇文章");

    outText = body;
    articleCount = articlesDone;
    // 少量图片稿可能没有文字属正常；大面积为空则视为抓取不完整
    complete = (failed == 0 && emptyBodies * 4 <= articlesDone);
    return true;
}

static DateResult saveRmrbDate(const Date& d, const Options& opt, const wstring& outDir,
                               Logger& log) {
    wstring basePath = joinPath(outDir, L"人民日报_" + utf8ToWide(ymd(d)));
    wstring fullPath = basePath + L".txt";
    long long existing = fileSize(fullPath);
    if (!opt.force && existing > 500) {
        log.line("  已存在，跳过：" + wideToUtf8(fullPath) + "（" + humanSize(existing) + "）");
        return DateResult::Skipped;
    }

    string body;
    int pageCount = 0;
    int articleCount = 0;
    bool complete = true;
    if (!fetchRmrbForDate(d, opt, log, body, pageCount, articleCount, complete)) {
        log.line("  " + dateCn(d) + " 暂无可用版面（可能尚未出版，下次运行会自动补）");
        return DateResult::Missing;
    }

    bool partial = opt.pagesSpecified || !complete;
    wstring path = partial ? (basePath + L"_部分.txt") : fullPath;
    if (!complete) log.line("  有请求失败，先保存为部分文件，下次运行会重试");

    string out;
    out += SEP + "\n";
    out += "人民日报 文字版\n";
    out += "日期：" + dateCn(d) + "（" + weekdayCn(d) + "）\n";
    out += "版面：" + std::to_string(pageCount) + " 个\n";
    out += "文章：" + std::to_string(articleCount) + " 篇\n";
    if (opt.pagesSpecified) out += "说明：本文件由 --pages 调试参数生成，只含部分版面。\n";
    if (!complete) out += "说明：抓取过程中有请求失败，本文件可能不完整，下次运行会重试。\n";
    out += "来源：人民网 人民日报电子版\n";
    out += "链接：https://paper.people.com.cn/rmrb/pc/layout/" + ym(d) + "/" +
           pad2(d.d) + "/node_01.html\n";
    out += "抓取时间：" + nowStamp() + "\n";
    out += "版权：内容仅供个人阅读、学习研究使用；未经授权，不得转载、复制、发行或镜像。\n";
    out += SEP + "\n\n";
    out += body;
    if (!out.empty() && out.back() != '\n') out.push_back('\n');

    if (!writeFileUtf8(path, toCrlf(out), true)) {
        log.line("  写入失败：" + wideToUtf8(path));
        return DateResult::Failed;
    }
    long long sz = fileSize(path);
    log.line("  已保存：" + wideToUtf8(path) + "（" + std::to_string(articleCount) + " 篇，" +
             humanSize(sz) + "）");
    if (!partial) DeleteFileW((basePath + L"_部分.txt").c_str());
    return partial ? DateResult::Partial : DateResult::Saved;
}

static WindowSummary runRmrbWindow(const Options& opt, const wstring& outDir, Logger& log) {
    WindowSummary sum;
    int days = std::max(1, opt.days);
    log.line("扫描最近 " + std::to_string(days) + " 天（" + dateCn(addDays(opt.target, -(days - 1))) +
             " 至 " + dateCn(opt.target) + "），只补缺失日期");

    int todo = 0;
    for (int back = 0; back < days; ++back) {
        Date d = addDays(opt.target, -back);
        wstring p = joinPath(outDir, L"人民日报_" + utf8ToWide(ymd(d)) + L".txt");
        if (opt.force || fileSize(p) <= 500) ++todo;
    }
    if (todo > 1)
        log.line("  本次需要补 " + std::to_string(todo) +
                 " 天，每天约 2–3 分钟，请耐心等待");

    for (int back = 0; back < days; ++back) {
        Date d = addDays(opt.target, -back);
        log.line("—— 人民日报 " + dateCn(d) + " ——");
        switch (saveRmrbDate(d, opt, outDir, log)) {
            case DateResult::Saved: ++sum.saved; break;
            case DateResult::Partial: ++sum.partial; break;
            case DateResult::Skipped: ++sum.skipped; break;
            case DateResult::Missing: ++sum.missing; break;
            case DateResult::Failed: ++sum.failed; break;
        }
    }

    log.line("人民日报汇总：新增 " + std::to_string(sum.saved) + " 天，部分 " +
             std::to_string(sum.partial) + " 天，已存在 " + std::to_string(sum.skipped) +
             " 天，暂无 " + std::to_string(sum.missing) + " 天，失败 " +
             std::to_string(sum.failed) + " 天");
    return sum;
}

// ----------------------------------------------------------------------------
// 入口
// ----------------------------------------------------------------------------
static void applyConfig(Options& opt, const std::map<string, string>& cfg) {
    auto get = [&](const string& k) -> string {
        auto it = cfg.find(k);
        return it == cfg.end() ? string() : it->second;
    };
    string v;
    if (!(v = get("proxy")).empty()) opt.proxy = v;
    if (get("github_proxy") == "0" || lowerAscii(get("github_proxy")) == "false")
        opt.githubProxy = false;
    if (!(v = get("catchup_days")).empty()) opt.days = std::stoi(v);
    else if (!(v = get("fallback_days")).empty()) opt.days = std::stoi(v);  // 旧配置兼容
    opt.days = std::max(1, std::min(31, opt.days));
    if (!(v = get("delay_ms")).empty()) opt.delayMs = std::max(0, std::stoi(v));
    if (!(v = get("output_dir")).empty()) opt.outDir = utf8ToWide(v);
    if (!(v = get("xwlb_source")).empty()) opt.xwlbSource = lowerAscii(v);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<string> args;
    for (int i = 1; i < argc; ++i) args.push_back(wideToUtf8(argvW[i]));
    if (argvW) LocalFree(argvW);

    Options opt;
    opt.target = today();
    wstring base = exeDir();
    opt.outDir = joinPath(base, L"output");

    // 先读配置，再由命令行覆盖
    auto cfg = loadConfig(joinPath(base, L"config.ini"));
    try {
        applyConfig(opt, cfg);
    } catch (const std::exception&) {
        printf("config.ini 中的数值项格式错误\n");
        return 2;
    }

    try {
        for (size_t i = 0; i < args.size(); ++i) {
            string a = args[i];
            string key = a, val;
            size_t eq = a.find('=');
            if (eq != NPOS) {
                key = a.substr(0, eq);
                val = a.substr(eq + 1);
            }
            auto needValue = [&](const string& k) -> string {
                (void)k;
                if (!val.empty() || eq != NPOS) return val;
                if (i + 1 < args.size()) return args[++i];
                return string();
            };

            if (key == "--help" || key == "-h") {
                opt.help = true;
            } else if (key == "--date") {
                string v = needValue(key);
                if (!parseDateString(v, opt.target)) {
                    printf("日期格式错误：%s（应为 YYYY-MM-DD 或 YYYYMMDD）\n", v.c_str());
                    return 2;
                }
                opt.hasDate = true;
            } else if (key == "--out") {
                opt.outDir = utf8ToWide(needValue(key));
            } else if (key == "--days" || key == "--fallback-days") {
                opt.days = std::max(1, std::min(31, std::stoi(needValue(key))));
            } else if (key == "--strict") {
                opt.strict = true;
            } else if (key == "--delay-ms") {
                opt.delayMs = std::max(0, std::stoi(needValue(key)));
            } else if (key == "--pages") {
                string v = needValue(key);
                opt.pagesSpecified = true;
                size_t dash = v.find('-');
                if (dash == NPOS) {
                    opt.pageFrom = opt.pageTo = std::max(1, std::stoi(v));
                } else {
                    opt.pageFrom = std::max(1, std::stoi(v.substr(0, dash)));
                    opt.pageTo = std::max(opt.pageFrom, std::stoi(v.substr(dash + 1)));
                }
            } else if (key == "--only") {
                string v = lowerAscii(needValue(key));
                if (v == "rmrb") opt.onlyRmrb = true;
                else if (v == "xwlb") opt.onlyXwlb = true;
                else {
                    printf("--only 只能是 rmrb 或 xwlb\n");
                    return 2;
                }
            } else if (key == "--force") {
                opt.force = true;
            } else if (key == "--proxy") {
                opt.proxy = needValue(key);
            } else if (key == "--no-github-proxy") {
                opt.githubProxy = false;
            } else if (key == "--xwlb-source") {
                string v = lowerAscii(needValue(key));
                if (v != "auto" && v != "github" && v != "govopen" && v != "cctv") {
                    printf("--xwlb-source 只能是 auto / github / govopen / cctv\n");
                    return 2;
                }
                opt.xwlbSource = v;
            } else {
                printf("未知参数：%s（--help 查看用法）\n", a.c_str());
                return 2;
            }
        }
    } catch (const std::exception& e) {
        printf("参数解析失败：%s\n", e.what());
        return 2;
    }

    opt.proxy = normalizeProxy(opt.proxy);
    if (opt.strict) opt.days = 1;
    if (opt.xwlbSource != "auto" && opt.xwlbSource != "github" &&
        opt.xwlbSource != "govopen" && opt.xwlbSource != "cctv")
        opt.xwlbSource = "auto";

    if (opt.help) {
        printHelp();
        return 0;
    }

    // 相对输出目录以程序所在目录为基准
    if (!isAbsolutePath(opt.outDir)) opt.outDir = joinPath(base, opt.outDir);
    wstring logDir = joinPath(opt.outDir, L"logs");
    if (!ensureDir(opt.outDir) || !ensureDir(logDir)) {
        printf("无法创建输出目录：%s\n", wideToUtf8(opt.outDir).c_str());
        return 2;
    }

    Logger log;
    log.open(joinPath(logDir, L"抓取日志_" + utf8ToWide(ymd(opt.target)) + L".log"));

    log.raw("\n" + SEP + "\n");
    log.raw("人民日报 / 新闻联播 每日内容抓取\n");
    log.raw("目标日期：" + dateCn(opt.target) + "（" + weekdayCn(opt.target) + "）\n");
    log.raw("补抓窗口：" + std::to_string(opt.days) + " 天\n");
    log.raw("输出目录：" + wideToUtf8(opt.outDir) + "\n");
    log.raw(SEP + "\n\n");

    bool xwlbOk = true, rmrbOk = true;
    WindowSummary xwlbSum, rmrbSum;

    // 只抓今天时，今天尚未发布属于正常“待更新”，不算失败
    auto evaluate = [&](const WindowSummary& s) {
        bool pendingToday = opt.days == 1 && sameDate(opt.target, today()) &&
                            s.missing == 1 && s.failed == 0;
        return s.failed == 0 && (s.anyAvailable() || pendingToday);
    };

    if (!opt.onlyRmrb) {
        log.line("[新闻联播] 开始");
        xwlbSum = runXwlbWindow(opt, opt.outDir, log);
        xwlbOk = evaluate(xwlbSum);
        log.line(string("[新闻联播] ") + (xwlbOk ? "完成" : "有错误"));
    }

    if (!opt.onlyXwlb) {
        log.line("[人民日报] 开始");
        rmrbSum = runRmrbWindow(opt, opt.outDir, log);
        rmrbOk = evaluate(rmrbSum);
        log.line(string("[人民日报] ") + (rmrbOk ? "完成" : "有错误"));
    }

    auto summaryLine = [](const char* name, bool ok, const WindowSummary& s) {
        string line = string(name) + "：" + (ok ? "完成" : "有错误");
        line += "（新增 " + std::to_string(s.saved) + " 天，部分 " +
                std::to_string(s.partial) + " 天，已存在 " + std::to_string(s.skipped) +
                " 天，暂无 " + std::to_string(s.missing) + " 天";
        if (s.failed) line += "，失败 " + std::to_string(s.failed) + " 天";
        line += "）\n";
        return line;
    };

    log.raw("\n" + SEP + "\n");
    if (!opt.onlyXwlb) log.raw(summaryLine("人民日报", rmrbOk, rmrbSum));
    if (!opt.onlyRmrb) log.raw(summaryLine("新闻联播", xwlbOk, xwlbSum));
    log.raw(SEP + "\n");

    log.close();

    bool allOk = true;
    if (!opt.onlyRmrb) allOk = allOk && xwlbOk;
    if (!opt.onlyXwlb) allOk = allOk && rmrbOk;
    bool anyOk = (!opt.onlyRmrb && xwlbOk) || (!opt.onlyXwlb && rmrbOk);
    if (allOk) return 0;
    return anyOk ? 1 : 2;
}
