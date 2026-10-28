// ============================================================================
//  pack_payload.cpp —— 把安装包要释放的文件打成一个二进制载荷
//
//  用法：pack_payload.exe <基准目录> <清单文件> <输出文件>
//
//  清单每行一条：源路径|目标路径
//    · 源路径可以是文件或目录；目录会递归展开
//    · 目标路径使用 / 分隔，相对安装目录
//    · 空行和 # 开头的行忽略
//
//  载荷格式（小端）：
//    "NDPKG001" (8 字节)
//    uint64 全部文件字节数
//    uint32 文件数
//    每项：uint16 路径长度 + 路径(UTF-8) + uint32 大小 + 内容
// ============================================================================

#define _WIN32_WINNT 0x0601
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::string trim(const std::string& in) {
    size_t a = 0, b = in.size();
    while (a < b && (in[a] == ' ' || in[a] == '\t' || in[a] == '\r' || in[a] == '\n')) ++a;
    while (b > a && (in[b - 1] == ' ' || in[b - 1] == '\t' || in[b - 1] == '\r' ||
                     in[b - 1] == '\n'))
        --b;
    return in.substr(a, b - a);
}

static std::string lowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static bool readAllBytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.assign((size_t)sz.QuadPart, '\0');
    DWORD total = 0;
    while (total < out.size()) {
        DWORD got = 0;
        DWORD want = (DWORD)std::min<size_t>(out.size() - total, 1 << 20);
        if (!ReadFile(h, &out[total], want, &got, nullptr) || got == 0) break;
        total += got;
    }
    CloseHandle(h);
    out.resize(total);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return total == sz.QuadPart;
}

static bool safeDest(const std::string& dest) {
    if (dest.empty() || dest[0] == '/' || dest[0] == '\\' || dest.find(':') != std::string::npos)
        return false;
    std::string low = lowerAscii(dest);
    if (low == "agent.ini" || low.rfind("output/", 0) == 0) return false;
    size_t start = 0;
    while (start <= dest.size()) {
        size_t p = dest.find('/', start);
        std::string seg = (p == std::string::npos) ? dest.substr(start) : dest.substr(start, p - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return true;
}

static void put16(std::string& b, uint16_t v) {
    b.push_back((char)(v & 0xFF));
    b.push_back((char)((v >> 8) & 0xFF));
}

static void put32(std::string& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((char)((v >> (8 * i)) & 0xFF));
}

static void put64(std::string& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back((char)((v >> (8 * i)) & 0xFF));
}

static bool appendFile(std::string& out, const fs::path& src, std::string& err) {
    HANDLE h = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "无法读取：" + wideToUtf8(src.wstring());
        return false;
    }
    std::vector<char> buf(1 << 20);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr)) {
            err = "读取失败：" + wideToUtf8(src.wstring());
            CloseHandle(h);
            return false;
        }
        if (got == 0) break;
        out.append(buf.data(), got);
    }
    CloseHandle(h);
    return true;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: pack_payload.exe <baseDir> <manifest> <output>\n");
        return 2;
    }

    fs::path base = fs::u8path(argv[1]);
    fs::path manifest = fs::u8path(argv[2]);
    fs::path output = fs::u8path(argv[3]);

    std::string text;
    if (!readAllBytes(manifest.wstring(), text)) {
        std::fprintf(stderr, "cannot read manifest: %s\n", argv[2]);
        return 1;
    }

    std::map<std::string, fs::path> files;  // 目标(UTF-8) -> 源
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line =
            trim((nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos));
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        if (line.empty() || line[0] == '#') continue;

        size_t bar = line.find('|');
        if (bar == std::string::npos) {
            std::fprintf(stderr, "bad manifest line: %s\n", line.c_str());
            return 1;
        }
        std::string srcS = trim(line.substr(0, bar));
        std::string dstS = trim(line.substr(bar + 1));
        if (srcS.empty() || dstS.empty()) {
            std::fprintf(stderr, "bad manifest line: %s\n", line.c_str());
            return 1;
        }
        dstS = [&] {
            std::string d;
            for (char c : dstS) d.push_back(c == '\\' ? '/' : c);
            return d;
        }();

        fs::path src = base / fs::u8path(srcS);
        std::error_code ec;
        if (!fs::exists(src, ec)) {
            std::fprintf(stderr, "missing source: %s\n", srcS.c_str());
            return 1;
        }

        if (fs::is_directory(src, ec)) {
            for (fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec),
                 end;
                 it != end; it.increment(ec)) {
                if (ec) {
                    std::fprintf(stderr, "walk failed: %s\n", srcS.c_str());
                    return 1;
                }
                std::error_code fec;
                if (!it->is_regular_file(fec)) continue;
                fs::path rel = it->path().lexically_relative(src);
                std::string dest = dstS + "/" + rel.generic_u8string();
                if (!safeDest(dest)) {
                    std::fprintf(stderr, "unsafe destination: %s\n", dest.c_str());
                    return 1;
                }
                files[dest] = it->path();
            }
        } else if (fs::is_regular_file(src, ec)) {
            if (!safeDest(dstS)) {
                std::fprintf(stderr, "unsafe destination: %s\n", dstS.c_str());
                return 1;
            }
            files[dstS] = src;
        } else {
            std::fprintf(stderr, "not a file or directory: %s\n", srcS.c_str());
            return 1;
        }
    }

    if (files.empty()) {
        std::fprintf(stderr, "manifest produced no files\n");
        return 1;
    }

    uint64_t total = 0;
    for (const auto& kv : files) {
        std::error_code ec;
        uint64_t sz = fs::file_size(kv.second, ec);
        if (ec || sz > 0xFFFFFFFFull) {
            std::fprintf(stderr, "bad file size: %s\n", wideToUtf8(kv.second.wstring()).c_str());
            return 1;
        }
        total += sz;
    }

    std::string out;
    out.reserve((size_t)total + 1024);
    out.append("NDPKG001", 8);
    put64(out, total);
    put32(out, (uint32_t)files.size());

    uint64_t written = 0;
    for (const auto& kv : files) {
        uint64_t sz = fs::file_size(kv.second);
        if (kv.first.size() > 0xFFFF) {
            std::fprintf(stderr, "path too long\n");
            return 1;
        }
        put16(out, (uint16_t)kv.first.size());
        out.append(kv.first);
        put32(out, (uint32_t)sz);
        std::string err;
        if (!appendFile(out, kv.second, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        written += sz;
    }

    HANDLE h = CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "cannot write output\n");
        return 1;
    }
    DWORD totalWritten = 0;
    while (totalWritten < out.size()) {
        DWORD w = 0;
        DWORD want = (DWORD)std::min<size_t>(out.size() - totalWritten, 1 << 20);
        if (!WriteFile(h, out.data() + totalWritten, want, &w, nullptr) || w == 0) {
            CloseHandle(h);
            std::fprintf(stderr, "write failed\n");
            return 1;
        }
        totalWritten += w;
    }
    CloseHandle(h);

    std::printf("payload: %u files, %llu bytes, %llu bytes written\n", (unsigned)files.size(),
                (unsigned long long)total, (unsigned long long)written);
    return 0;
}
