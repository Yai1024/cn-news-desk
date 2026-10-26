// ============================================================================
//  agent_host.exe —— 政策解读 Agent（独立进程）
//
//  职责：
//    1. 读取本地新闻稿（output/），按日期组装"材料包"；
//    2. 载入 skill/ 下的解读方法论（SKILL.md + references/*.md）作为系统提示；
//    3. 调用 OpenAI 兼容接口（默认 DeepSeek），流式把解读结果写到 stdout；
//    4. 由 news_desk.exe 以子进程方式拉起，通过管道逐字回显。
//
//  说明：本程序自身不做任何政策判断，判断由模型依据 skill 完成。
//        密钥只从 agent.ini 或环境变量读取，不写日志、不回显。
// ============================================================================

#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <io.h>

namespace fs = std::filesystem;
using std::string;
using std::wstring;
using std::vector;

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

static string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return string();
    std::ostringstream ss;
    ss << f.rdbuf();
    string s = ss.str();
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
    return s;
}

// 按 UTF-8 码点截断，避免把汉字切成半个
static string utf8Cut(const string& s, size_t maxChars) {
    size_t chars = 0, i = 0;
    while (i < s.size() && chars < maxChars) {
        unsigned char c = (unsigned char)s[i];
        size_t step = 1;
        if ((c & 0x80) == 0) step = 1;
        else if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        if (i + step > s.size()) break;
        i += step;
        ++chars;
    }
    return s.substr(0, i);
}

static size_t utf8Len(const string& s) {
    size_t chars = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t step = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        i += step;
        ++chars;
    }
    return chars;
}

static bool isSpaceCh(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static string trim(const string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isSpaceCh(s[a])) ++a;
    while (b > a && isSpaceCh(s[b - 1])) --b;
    return s.substr(a, b - a);
}

static vector<string> splitLines(const string& s) {
    vector<string> out;
    string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ----------------------------------------------------------------------------
// 配置
// ----------------------------------------------------------------------------

struct Config {
    string base_url = "https://api.deepseek.com";
    string model = "deepseek-chat";
    string api_key;
    string extra_json;      // 追加到请求体的原始 JSON 片段（联网等厂商特有参数）
    string proxy;           // host:port，留空表示不用代理
    string proxy_mode = "auto";  // auto=先直连再代理 / direct / always
    int retries = 3;        // 传输层失败的重试轮数
    int max_tokens = 8000;
    double temperature = 1.0;
    int timeout_sec = 300;
    int days = 7;
    bool extend = true;
    int max_input_chars = 60000;
    wstring skill_dir = L"skill";
};

static void loadIni(const fs::path& path, Config& cfg) {
    std::ifstream f(path);
    if (!f) return;
    string line;
    while (std::getline(f, line)) {
        string s = trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';' || s[0] == '[') continue;
        size_t eq = s.find('=');
        if (eq == string::npos) continue;
        string k = trim(s.substr(0, eq)), v = trim(s.substr(eq + 1));
        if (k == "base_url") cfg.base_url = v;
        else if (k == "model") cfg.model = v;
        else if (k == "api_key") cfg.api_key = v;
        else if (k == "extra_json") cfg.extra_json = v;
        else if (k == "proxy") cfg.proxy = v;
        else if (k == "proxy_mode") cfg.proxy_mode = v;
        else if (k == "retries") cfg.retries = atoi(v.c_str());
        else if (k == "max_tokens") cfg.max_tokens = atoi(v.c_str());
        else if (k == "temperature") cfg.temperature = atof(v.c_str());
        else if (k == "timeout_seconds") cfg.timeout_sec = atoi(v.c_str());
        else if (k == "days") cfg.days = atoi(v.c_str());
        else if (k == "extend_history") cfg.extend = (v == "1" || v == "true");
        else if (k == "max_input_chars") cfg.max_input_chars = atoi(v.c_str());
        else if (k == "skill_dir") cfg.skill_dir = utf8ToWide(v);
    }
    const char* env = getenv("DEEPSEEK_API_KEY");
    if (env && *env) cfg.api_key = env;
}

// ----------------------------------------------------------------------------
// 技能包
// ----------------------------------------------------------------------------

static bool loadSkill(const wstring& dir, string& out) {
    fs::path base(dir);
    string skill = readFile(base / "SKILL.md");
    if (skill.empty()) return false;
    out = "# 解读方法论（skill）\n\n" + skill;
    vector<fs::path> refs;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(base / "references", ec)) {
        if (e.is_regular_file() && e.path().extension() == ".md") refs.push_back(e.path());
    }
    std::sort(refs.begin(), refs.end());
    for (auto& p : refs) {
        out += "\n\n# 参考：" + wideToUtf8(p.stem().wstring()) + "\n\n" + readFile(p);
    }
    return true;
}

// ----------------------------------------------------------------------------
// 材料抽取
// ----------------------------------------------------------------------------

struct NewsFile {
    string date;        // YYYY-MM-DD
    bool cctv = false;
    string text;
    string name;
};

static string dateFromName(const string& name) {
    // 从 新闻联播_20261025.txt 这类文件名里取 YYYYMMDD
    for (size_t i = 0; i + 8 <= name.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < 8; ++j)
            if (!isdigit((unsigned char)name[i + j])) { ok = false; break; }
        if (!ok) continue;
        if (i > 0 && isdigit((unsigned char)name[i - 1])) continue;
        if (i + 8 < name.size() && isdigit((unsigned char)name[i + 8])) continue;
        string d = name.substr(i, 8);
        if (d.compare(0, 2, "20") != 0) continue;
        return d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2);
    }
    return string();
}

static bool isTitleOnly(const string& text) {
    return text.find("本条没有文字稿") != string::npos && utf8Len(text) < 3000;
}

// 联播：摘要条目 + 重点条目正文
static string extractCctv(const NewsFile& f, const string& keywordHint, int bodyPerItem,
                          int maxBodyItems, bool withBody) {
    string out = "### 新闻联播 " + f.date + "\n";
    string text = f.text;

    // 摘要块
    size_t sp = text.find("## 新闻摘要");
    size_t dp = text.find("## 详细新闻");
    string summary = sp == string::npos ? string()
                    : text.substr(sp, (dp == string::npos ? text.size() : dp) - sp);

    vector<string> titles;
    if (!summary.empty()) {
        for (auto& raw : splitLines(summary)) {
            string s = trim(raw);
            if (s.empty() || s.rfind("##", 0) == 0) continue;
            if (s.find("本期节目主要内容") != string::npos) continue;
            if (s.rfind("（《新闻联播》", 0) == 0) continue;
            size_t i = 0;
            while (i < s.size() && isdigit((unsigned char)s[i])) ++i;
            size_t skip = 0;
            if (i > 0) {
                if (i < s.size() && s[i] == '.') skip = 1;
                else if (s.compare(i, 3, "、") == 0) skip = 3;
                else if (s.compare(i, 3, "．") == 0) skip = 3;
            }
            if (skip > 0) {
                string t = trim(s.substr(i + skip));
                if (!t.empty()) titles.push_back(t);
            } else if (!titles.empty() && s.rfind("（", 0) != 0 && s.rfind("(", 0) != 0) {
                titles.back() += s;  // 标题折行，接回去
            }
        }
    }
    if (titles.empty()) {
        // 仅标题格式：【1】xxx（】为 3 字节 UTF-8）
        for (auto& raw : splitLines(text)) {
            string s = trim(raw);
            if (s.rfind("【", 0) != 0) continue;
            size_t close = s.find("】");
            if (close == string::npos) continue;
            string t = trim(s.substr(close + 3));
            if (!t.empty()) titles.push_back(t);
        }
    }

    out += "条目数：" + std::to_string(titles.size());
    if (isTitleOnly(text)) out += "（仅标题，无正文）";
    out += "\n";
    for (size_t i = 0; i < titles.size(); ++i)
        out += std::to_string(i + 1) + ". " + utf8Cut(titles[i], 90) + "\n";

    if (!withBody || isTitleOnly(text)) return out;

    // 详细正文：按关键词挑重点条目
    vector<string> hints;
    {
        std::stringstream ss(keywordHint);
        string k;
        while (std::getline(ss, k, '|'))
            if (!trim(k).empty()) hints.push_back(trim(k));
    }
    vector<std::pair<string, string>> sections;  // title, body
    size_t pos = 0;
    while (true) {
        size_t h = text.find("\n### ", pos);
        if (h == string::npos) break;
        size_t tStart = h + 5;
        size_t tEnd = text.find('\n', tStart);
        if (tEnd == string::npos) break;
        string title = trim(text.substr(tStart, tEnd - tStart));
        size_t next = text.find("\n### ", tEnd);
        string body = text.substr(tEnd, (next == string::npos ? text.size() : next) - tEnd);
        // 去标签
        string plain;
        bool inTag = false;
        for (char c : body) {
            if (c == '<') { inTag = true; continue; }
            if (c == '>') { inTag = false; continue; }
            if (!inTag) plain.push_back(c);
        }
        plain = trim(plain);
        if (plain.find("查看原文") == 0) plain.clear();
        size_t p2 = plain.find("查看原文");
        if (p2 != string::npos) plain = trim(plain.substr(0, p2));
        sections.push_back({title, plain});
        pos = (next == string::npos) ? text.size() : next;
        if (next == string::npos) break;
    }

    int used = 0;
    if (!sections.empty()) out += "\n重点正文：\n";
    for (auto& sec : sections) {
        if (used >= maxBodyItems) break;
        bool hit = hints.empty();
        for (auto& k : hints)
            if (sec.first.find(k) != string::npos || sec.second.find(k) != string::npos) { hit = true; break; }
        if (!hit) continue;
        out += "【" + sec.first + "】" + utf8Cut(sec.second, (size_t)bodyPerItem) + "\n";
        ++used;
    }
    return out;
}

// 人民日报：版面 + 标题（全部），重点版面正文
static string extractRmrb(const NewsFile& f, int bodyPerArticle, int maxBody, int maxTitles) {
    string out = "### 人民日报 " + f.date + "\n";
    auto lines = splitLines(f.text);

    struct Art { string title, body; int page = 0; };
    vector<Art> arts;
    int page = 0;
    string pageName;
    Art cur;
    bool inArt = false;

    auto flush = [&]() {
        if (inArt && !cur.title.empty()) arts.push_back(cur);
        inArt = false;
        cur = Art();
    };

    for (auto& raw : lines) {
        string s = trim(raw);
        if (s.empty()) continue;
        if (s.rfind("第", 0) == 0 && s.find("版") != string::npos) {
            // 注意：这些字都是 3 字节 UTF-8，不能用 +1 切
            size_t bpos = s.find("版");
            if (bpos < 3) continue;
            flush();
            page = atoi(s.substr(3, bpos - 3).c_str());
            string rest = s.substr(bpos + 3);
            if (rest.rfind("：", 0) == 0) rest = rest.substr(3);
            else if (rest.rfind(":", 0) == 0) rest = rest.substr(1);
            pageName = trim(rest);
            out += "\n第" + std::to_string(page) + "版：" + pageName + "\n";
            continue;
        }
        if (s.rfind("【", 0) == 0) {
            size_t close = s.find("】");
            if (close != string::npos) {
                string t = trim(s.substr(close + 3));  // 】占 3 字节
                if (!t.empty()) {
                    flush();
                    cur.title = t;
                    cur.page = page;
                    inArt = true;
                    out += "· " + utf8Cut(cur.title, 60) + "\n";
                }
                continue;
            }
        }
        if (inArt) {
            if (s.rfind("链接：", 0) == 0) continue;
            if (s.find("版权") != string::npos && s.size() < 80) continue;
            cur.body += s;
        }
    }
    flush();

    if (maxBody <= 0) return out;
    out += "\n重点正文：\n";
    int used = 0;
    for (auto& a : arts) {
        if (used >= maxBody) break;
        bool important = a.page <= 2 || a.page == 5 || a.page == 9;
        if (!important) continue;
        if (utf8Len(a.body) < 200) continue;
        out += "【第" + std::to_string(a.page) + "版·" + a.title + "】" +
               utf8Cut(a.body, (size_t)bodyPerArticle) + "\n";
        ++used;
    }
    (void)maxTitles;
    return out;
}

// ----------------------------------------------------------------------------
// 材料包组装
// ----------------------------------------------------------------------------

struct Bundle {
    string text;
    int files = 0;
    int days = 0;
    string newest, oldest;
    vector<string> missing;
    bool truncated = false;
};

static Bundle buildBundle(const fs::path& dir, int days, bool extend, int budget) {
    Bundle b;
    vector<NewsFile> all;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        string name = wideToUtf8(e.path().filename().wstring());
        if (name.find("_部分") != string::npos) continue;
        bool cctv = name.find("新闻联播") != string::npos;
        bool rmrb = name.find("人民日报") != string::npos;
        if (!cctv && !rmrb) continue;
        string d = dateFromName(name);
        if (d.empty()) continue;
        NewsFile nf;
        nf.date = d;
        nf.cctv = cctv;
        nf.name = name;
        nf.text = readFile(e.path());
        if (nf.text.empty()) continue;
        all.push_back(std::move(nf));
    }
    std::sort(all.begin(), all.end(), [](const NewsFile& a, const NewsFile& b) {
        if (a.date != b.date) return a.date > b.date;
        return a.cctv && !b.cctv;
    });

    // 取最近 days 个自然日窗口
    vector<string> recent;
    {
        // 由今天往前推 days-1 天
        SYSTEMTIME st;
        GetLocalTime(&st);
        FILETIME ft;
        SystemTimeToFileTime(&st, &ft);
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        for (int i = 0; i < days; ++i) {
            ULARGE_INTEGER x = u;
            x.QuadPart -= (ULONGLONG)i * 864000000000ULL;
            FILETIME f2;
            f2.dwLowDateTime = x.LowPart;
            f2.dwHighDateTime = x.HighPart;
            SYSTEMTIME s2;
            FileTimeToSystemTime(&f2, &s2);
            char buf[16];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d", s2.wYear, s2.wMonth, s2.wDay);
            recent.push_back(buf);
        }
        std::reverse(recent.begin(), recent.end());
    }

    vector<NewsFile> inWindow, older;
    for (auto& n : all) {
        if (std::find(recent.begin(), recent.end(), n.date) != recent.end()) inWindow.push_back(n);
        else older.push_back(n);
    }

    // 缺日检测
    for (auto& d : recent) {
        bool hasC = false, hasR = false;
        for (auto& n : inWindow) {
            if (n.date == d) (n.cctv ? hasC : hasR) = true;
        }
        if (!hasC || !hasR) b.missing.push_back(d + (hasC ? "" : " 缺联播") + (hasR ? "" : " 缺人民日报"));
    }

    b.files = (int)inWindow.size();
    b.days = days;
    if (!all.empty()) { b.newest = all.front().date; b.oldest = all.back().date; }

    string head;
    head += "【材料清单】\n";
    head += "窗口：" + (recent.empty() ? string("?") : recent.front()) + " 至 " +
            (recent.empty() ? string("?") : recent.back()) + "（" + std::to_string(days) + " 天）\n";
    head += "命中文件：" + std::to_string(inWindow.size()) + " 份\n";
    if (!b.missing.empty()) {
        head += "缺口：\n";
        for (auto& m : b.missing) head += "  - " + m + "\n";
    }
    head += "说明：缺口属材料状态，不得据此判断官方表述变化。\n\n";
    b.text = head;

    size_t used = utf8Len(head);
    auto append = [&](const string& s) {
        if (used >= (size_t)budget) { b.truncated = true; return; }
        string cut = utf8Cut(s, (size_t)budget - used);
        if (utf8Len(cut) < utf8Len(s)) b.truncated = true;
        b.text += cut;
        used += utf8Len(cut);
    };

    const char* cctvHints =
        "会议|全会|中央|国务院|政治局|部署|指示|政策|改革|规划|巡视|党建|从严|经济|金融|"
        "科技|产业|就业|安全|发展|开放|民生|教育|能源|乡村|区域";

    // 1) 联播摘要（全部日期）
    append("【近七日·新闻联播】\n");
    for (auto& n : inWindow) {
        if (!n.cctv) continue;
        append(extractCctv(n, cctvHints, 700, 5, false));
        append("\n");
    }

    // 2) 人民日报版面与标题
    append("【近七日·人民日报】\n");
    for (auto& n : inWindow) {
        if (n.cctv) continue;
        append(extractRmrb(n, 500, 0, 200));
        append("\n");
    }

    // 3) 联播重点正文
    append("【新闻联播·重点正文】\n");
    for (auto& n : inWindow) {
        if (!n.cctv) continue;
        append(extractCctv(n, cctvHints, 700, 5, true));
        append("\n");
    }

    // 4) 人民日报重点版面正文
    append("【人民日报·重点正文】\n");
    for (auto& n : inWindow) {
        if (n.cctv) continue;
        append(extractRmrb(n, 800, 3, 0));
        append("\n");
    }

    // 5) 更早材料（仅标题，追踪长期变化）
    if (extend && !older.empty()) {
        append("【更早材料·仅标题（用于长期变化追踪）】\n");
        int cnt = 0;
        for (auto& n : older) {
            if (cnt++ >= 30) { b.truncated = true; break; }
            if (n.cctv) append(extractCctv(n, "", 0, 0, false));
            else append(extractRmrb(n, 0, 0, 0));
        }
    }

    return b;
}

// ----------------------------------------------------------------------------
// JSON
// ----------------------------------------------------------------------------

static string jsonEscape(const string& s) {
    string o;
    o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o.push_back((char)c);
                }
        }
    }
    return o;
}

static bool jsonUnescapeToUtf8(const string& in, size_t& i, string& out) {
    // i 指向开头引号后的第一个字符，读到未转义引号为止
    while (i < in.size()) {
        char c = in[i];
        if (c == '"') return true;
        if (c == '\\') {
            if (i + 1 >= in.size()) return false;
            char e = in[i + 1];
            i += 2;
            switch (e) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (i + 4 > in.size()) return false;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = in[i + k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                    }
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= in.size() && in[i] == '\\' && in[i + 1] == 'u') {
                        unsigned lo = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = in[i + 2 + k];
                            lo <<= 4;
                            if (h >= '0' && h <= '9') lo |= (h - '0');
                            else if (h >= 'a' && h <= 'f') lo |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') lo |= (h - 'A' + 10);
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                    // UTF-8 编码
                    if (cp < 0x80) out.push_back((char)cp);
                    else if (cp < 0x800) {
                        out.push_back((char)(0xC0 | (cp >> 6)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        out.push_back((char)(0xE0 | (cp >> 12)));
                        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back((char)(0xF0 | (cp >> 18)));
                        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: out.push_back(e); break;
            }
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return false;
}

// 从一条 SSE data 负载里取 delta.content
static bool extractDelta(const string& json, string& out) {
    size_t k = json.find("\"content\":");
    if (k == string::npos) return false;
    size_t q = json.find('"', k + 10);
    if (q == string::npos) return false;
    size_t i = q + 1;
    if (!jsonUnescapeToUtf8(json, i, out)) return false;
    return true;
}

// 非流式：取 choices[0].message.content
static bool extractMessage(const string& json, string& out) {
    size_t m = json.find("\"message\"");
    if (m == string::npos) return false;
    size_t k = json.find("\"content\":", m);
    if (k == string::npos) return false;
    size_t q = json.find('"', k + 10);
    if (q == string::npos) return false;
    size_t i = q + 1;
    return jsonUnescapeToUtf8(json, i, out);
}

// ----------------------------------------------------------------------------
// HTTP
// ----------------------------------------------------------------------------

struct UrlParts {
    wstring host;
    wstring path;
    INTERNET_PORT port = 443;
    bool https = true;
    bool ok = false;
};

static UrlParts crackUrl(const string& url) {
    UrlParts r;
    wstring w = utf8ToWide(url);
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[2048] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(w.c_str(), (DWORD)w.size(), 0, &uc)) return r;
    r.host = host;
    r.path = path;
    r.port = uc.nPort;
    r.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    r.ok = true;
    return r;
}

static void outPrint(const string& s) {
    fwrite(s.data(), 1, s.size(), stdout);
    fflush(stdout);
}

static void errPrint(const string& s) {
    fwrite(s.data(), 1, s.size(), stderr);
    fflush(stderr);
}

// 把 base_url 补成完整的 chat/completions 端点
static string endpointOf(const string& base) {
    string b = base;
    while (!b.empty() && (b.back() == '/' || b.back() == ' ')) b.pop_back();
    if (b.find("chat/completions") != string::npos) return b;
    return b + "/chat/completions";
}

// WinHTTP 错误的可读解释
static string winHttpHint(DWORD e) {
    switch (e) {
        case 12152: return "服务器返回了无法识别的响应（边缘节点重置连接，多为瞬时故障或代理/证书干扰）";
        case 12029: return "无法连接服务器（网络不通或被拦截）";
        case 12030: return "连接被中断";
        case 12002: return "请求超时";
        case 12007: return "域名解析失败";
        case 12057: return "证书校验失败（可能被中间人代理干扰）";
        case 12175: return "TLS 安全通道建立失败（代理或证书问题）";
        case 12169: return "证书颁发机构不受信任";
        default: return "WinHTTP 错误";
    }
}

struct Attempt {
    int status = 0;      // HTTP 状态码；0 表示传输层失败
    string err;
    size_t printed = 0;  // 本次尝试已经输出的字节数（>0 时不可重试，否则内容会重复）
};

// 单次尝试；useProxy=true 时走 cfg.proxy
static Attempt callApiOnce(const Config& cfg, const string& body, bool useProxy) {
    Attempt a;
    UrlParts u = crackUrl(endpointOf(cfg.base_url));
    if (!u.ok) { a.err = "base_url 无法解析：" + cfg.base_url; return a; }

    wstring proxyW = utf8ToWide(cfg.proxy);
    HINTERNET hSession = useProxy
        ? WinHttpOpen(L"NewsDeskAgent/1.0", WINHTTP_ACCESS_TYPE_NAMED_PROXY, proxyW.c_str(),
                      WINHTTP_NO_PROXY_BYPASS, 0)
        : WinHttpOpen(L"NewsDeskAgent/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { a.err = "WinHttpOpen 失败"; return a; }
    WinHttpSetTimeouts(hSession, 20000, 20000, cfg.timeout_sec * 1000, cfg.timeout_sec * 1000);

    HINTERNET hConnect = WinHttpConnect(hSession, u.host.c_str(), u.port, 0);
    if (!hConnect) {
        a.err = "连接失败：" + wideToUtf8(u.host);
        WinHttpCloseHandle(hSession);
        return a;
    }

    DWORD flags = u.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", u.path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        a.err = "创建请求失败";
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return a;
    }

    wstring headers = L"Content-Type: application/json; charset=utf-8\r\n"
                      L"Accept: text/event-stream\r\n"
                      L"User-Agent: NewsDeskAgent/1.0\r\n";
    if (!cfg.api_key.empty()) headers += L"Authorization: Bearer " + utf8ToWide(cfg.api_key) + L"\r\n";

    BOOL ok = WinHttpSendRequest(hReq, headers.c_str(), (DWORD)-1L, (LPVOID)body.data(),
                                 (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!ok) {
        DWORD e = GetLastError();
        a.err = winHttpHint(e) + "（错误码 " + std::to_string(e) + "）";
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return a;
    }
    if (!WinHttpReceiveResponse(hReq, nullptr)) {
        DWORD e = GetLastError();
        a.err = winHttpHint(e) + "（错误码 " + std::to_string(e) + "）";
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return a;
    }

    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    a.status = (int)status;

    auto emit = [&](const string& s) {
        outPrint(s);
        a.printed += s.size();
    };

    string buf;
    auto drain = [&](bool final) {
        while (true) {
            size_t nl = buf.find('\n');
            if (nl == string::npos) break;
            string line = trim(buf.substr(0, nl));
            buf.erase(0, nl + 1);
            if (line.rfind("data:", 0) != 0) continue;
            string payload = trim(line.substr(5));
            if (payload == "[DONE]") return;
            string delta;
            if (extractDelta(payload, delta)) emit(delta);
        }
        if (final && !buf.empty()) {
            string line = trim(buf);
            if (line.rfind("data:", 0) == 0) {
                string payload = trim(line.substr(5));
                if (payload != "[DONE]") {
                    string delta;
                    if (extractDelta(payload, delta)) emit(delta);
                }
            }
        }
    };

    string all;
    while (true) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)) break;
        if (avail == 0) break;
        string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(hReq, &chunk[0], avail, &read) || read == 0) break;
        chunk.resize(read);
        all += chunk;
        buf += chunk;
        if (status == 200) drain(false);
    }
    if (status == 200) drain(true);

    if (status != 200) {
        a.err = "HTTP " + std::to_string(status) + "：" + utf8Cut(all, 600);
    } else if (all.find("\"error\"") != string::npos && all.find("data:") == string::npos) {
        a.err = "接口返回错误：" + utf8Cut(all, 600);
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return a;
}

// 带重试与代理回退的调用入口：瞬时故障重试，已开始输出就不再重试（避免重复内容）。
static int callApi(const Config& cfg, const string& body, string& err) {
    vector<bool> plan;
    bool hasProxy = !cfg.proxy.empty();
    if (cfg.proxy_mode == "always" && hasProxy) plan = {true};
    else if (cfg.proxy_mode == "direct" || !hasProxy) plan = {false};
    else plan = {false, true};  // auto：先直连，失败再走代理

    int rounds = cfg.retries > 0 ? cfg.retries : 1;
    int last = 0;
    for (int round = 0; round < rounds; ++round) {
        for (bool useProxy : plan) {
            Attempt a = callApiOnce(cfg, body, useProxy);
            last = a.status;
            if (a.status == 200) return 200;
            err = a.err;
            if (a.printed > 0) return a.status;  // 已经在写内容了，重试会造成重复

            bool transient = (a.status == 0) || a.status == 429 || a.status >= 500;
            if (!transient) return a.status;

            if (round + 1 < rounds || useProxy != plan.back()) {
                outPrint("\n（第 " + std::to_string(round + 1) + " 轮" +
                         (useProxy ? "·代理" : "·直连") + "失败：" + a.err + "；稍后重试）\n");
                Sleep(700 * (round + 1));
            }
        }
    }
    return last;
}

// ----------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------

static const char* kSystemTail =
    "\n\n# 本次任务要求\n\n"
    "1. 以【近七日】稿件为核心材料，输出近期政策变动、国家方向与背后隐含信号。\n"
    "2. 严格按 skill 的证据规则：结论挂标签，差分与解释分栏，不做总分。\n"
    "3. 若材料含【更早材料·仅标题】，仅用于判断长期变化趋势，不得当作完整文本做措辞差分。\n"
    "4. 若启用了联网检索，可检索关联外部新闻作为旁证；每一条外部信息须标注为“外部来源，待核”，"
    "且不得替代对本地原文的分析。未启用联网时，不要臆造外部事件。\n"
    "5. 缺口、仅标题、署名缺失等材料问题，按“无法取证”处理，不推断官方沉默。\n"
    "6. 输出使用中文 Markdown，结构依次为：一页结论 / 材料与可比性 / 结构信号 / 逐条差分 / "
    "缺位审查 / 措辞语法 / 受控联想（每条含观察-桥梁-待验证-证伪信号）/ 兑现跟踪 / 边界。\n"
    "7. 篇幅控制在 1500–2500 字，宁可少写，不可编造。\n";

static void usage() {
    printf(
        "agent_host.exe —— 政策解读 Agent（独立进程）\n\n"
        "参数：\n"
        "  --input-dir <dir>   新闻稿目录，默认 output\n"
        "  --days <n>          近几日窗口，默认取 agent.ini\n"
        "  --extend            允许读取更早材料（默认按 agent.ini）\n"
        "  --no-extend         禁用更早材料\n"
        "  --config <file>     配置文件，默认 agent.ini\n"
        "  --dry-run           只组装材料包并打印统计，不调用接口\n"
        "  --help              显示本帮助\n");
}

int main(int argc, char** argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    fs::path dir(exeDir());
    fs::path cfgPath = dir / L"agent.ini";
    fs::path inputDir = dir / L"output";
    fs::path dumpPath;
    bool dry = false;
    int daysOverride = -1;
    int extendOverride = -1;

    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        auto next = [&](const char* name) -> string {
            if (i + 1 >= argc) { errPrint(string("缺少参数值：") + name + "\n"); exit(2); }
            return argv[++i];
        };
        if (a == "--input-dir") inputDir = utf8ToWide(next("--input-dir"));
        else if (a == "--days") daysOverride = atoi(next("--days").c_str());
        else if (a == "--config") cfgPath = utf8ToWide(next("--config"));
        else if (a == "--dump-bundle") dumpPath = utf8ToWide(next("--dump-bundle"));
        else if (a == "--extend") extendOverride = 1;
        else if (a == "--no-extend") extendOverride = 0;
        else if (a == "--dry-run") dry = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { errPrint("未知参数：" + a + "\n"); usage(); return 2; }
    }
    if (!cfgPath.is_absolute()) cfgPath = dir / cfgPath;

    Config cfg;
    loadIni(cfgPath, cfg);
    if (daysOverride > 0) cfg.days = daysOverride;
    if (extendOverride >= 0) cfg.extend = (extendOverride == 1);

    string skill;
    fs::path sd(cfg.skill_dir);
    fs::path skillDir = sd.is_absolute() ? sd : (dir / sd);
    if (!loadSkill(skillDir.wstring(), skill)) {
        errPrint("找不到技能包：" + wideToUtf8(skillDir.wstring()) +
                 "\n请确认 skill\\SKILL.md 存在（可从 Cherry 的 skills 目录复制）。\n");
        return 3;
    }

    Bundle b = buildBundle(inputDir, cfg.days, cfg.extend, cfg.max_input_chars);

    if (!dumpPath.empty()) {
        std::ofstream f(dumpPath, std::ios::binary);
        f.write(b.text.data(), (std::streamsize)b.text.size());
    }

    if (dry) {
        outPrint("【dry-run】技能包 " + std::to_string(utf8Len(skill)) + " 字符；材料包 " +
                 std::to_string(utf8Len(b.text)) + " 字符（上限 " +
                 std::to_string(cfg.max_input_chars) + "）\n");
        outPrint("文件 " + std::to_string(b.files) + " 份；最新 " + b.newest + "；最早 " +
                 b.oldest + "\n");
        if (b.truncated) outPrint("注意：材料包已按预算截断。\n");
        outPrint("\n—— 材料包前 4000 字符 ——\n");
        outPrint(utf8Cut(b.text, 4000) + "\n");
        return 0;
    }

    if (cfg.api_key.empty()) {
        errPrint("未配置 API 密钥。\n请在 agent.ini 里填写 api_key=，或设置环境变量 DEEPSEEK_API_KEY。\n"
                 "（密钥只在本机使用，不会写入日志。）\n");
        return 4;
    }

    string sys = skill + kSystemTail;
    string userMsg;
    userMsg += "以下是本地新闻稿材料包，请据此做解读。\n\n" + b.text;
    if (b.truncated)
        userMsg += "\n\n（注：材料包因长度预算被截断，缺失部分按“无法取证”处理。）\n";

    string body =
        "{\"model\":\"" + jsonEscape(cfg.model) + "\","
        "\"stream\":true,"
        "\"max_tokens\":" + std::to_string(cfg.max_tokens) + ","
        "\"temperature\":" + std::to_string(cfg.temperature) + ",";
    if (!cfg.extra_json.empty()) body += cfg.extra_json + ",";
    body +=
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + jsonEscape(sys) + "\"},"
        "{\"role\":\"user\",\"content\":\"" + jsonEscape(userMsg) + "\"}"
        "]}";

    outPrint("## 解读中（模型 " + cfg.model + "，材料 " + std::to_string(b.files) +
             " 份，窗口 " + std::to_string(cfg.days) + " 天）\n\n");

    string err;
    int st = callApi(cfg, body, err);

    // 厂商特有参数不被支持（常见 400）时，去掉后重试一次
    if (st == 400 && !cfg.extra_json.empty()) {
        outPrint("\n（接口拒绝了额外参数，已去掉 extra_json 重试一次。若需要联网检索，"
                 "请改用支持该参数的端点。）\n\n");
        Config cfg2 = cfg;
        cfg2.extra_json.clear();
        string body2 =
            "{\"model\":\"" + jsonEscape(cfg2.model) + "\","
            "\"stream\":true,"
            "\"max_tokens\":" + std::to_string(cfg2.max_tokens) + ","
            "\"temperature\":" + std::to_string(cfg2.temperature) + ","
            "\"messages\":["
            "{\"role\":\"system\",\"content\":\"" + jsonEscape(sys) + "\"},"
            "{\"role\":\"user\",\"content\":\"" + jsonEscape(userMsg) + "\"}"
            "]}";
        st = callApi(cfg2, body2, err);
    }

    outPrint("\n");
    if (st != 200) {
        errPrint("调用失败：" + err + "\n");
        return 5;
    }
    outPrint("\n[完成]\n");
    return 0;
}
