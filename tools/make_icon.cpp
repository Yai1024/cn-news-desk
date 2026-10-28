// ============================================================================
//  make_icon.cpp —— 生成安装包与快捷方式用的 app.ico
//
//  用法：make_icon.exe <输出.ico>
//  生成 16/32/48/64/256 五种尺寸；大图用 PNG 压缩，Windows Vista+ 支持。
// ============================================================================

#define _WIN32_WINNT 0x0601
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace Gdiplus;

static bool getEncoder(const WCHAR* mime, CLSID& clsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<BYTE> buf(size);
    ImageCodecInfo* info = (ImageCodecInfo*)buf.data();
    GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(info[i].MimeType, mime) == 0) {
            clsid = info[i].Clsid;
            return true;
        }
    }
    return false;
}

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

static bool renderPng(int size, const CLSID& pngClsid, std::vector<BYTE>& out) {
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    if (bmp.GetLastStatus() != Ok) return false;

    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(0, 0, 0, 0));

    float inset = size * 0.055f;
    RectF r(inset, inset, (REAL)size - inset * 2, (REAL)size - inset * 2);
    GraphicsPath path;
    addRoundRect(path, r, size * 0.225f);

    LinearGradientBrush fill(r, Color(255, 210, 66, 55), Color(255, 158, 30, 24),
                             LinearGradientModeForwardDiagonal);
    g.FillPath(&fill, &path);

    Pen edge(Color(55, 255, 255, 255), std::max(1.0f, size * 0.018f));
    g.DrawPath(&edge, &path);

    FontFamily* fam = new FontFamily(L"Microsoft YaHei UI");
    if (fam->GetLastStatus() != Ok) {
        delete fam;
        fam = new FontFamily(L"Microsoft YaHei");
    }
    if (fam->GetLastStatus() == Ok) {
        Font font(fam, size * 0.56f, FontStyleBold, UnitPixel);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush white(Color(255, 255, 255, 255));
        g.DrawString(L"解", -1, &font, RectF(0, size * 0.015f, (REAL)size, (REAL)size), &sf, &white);
    }
    delete fam;

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) != S_OK) return false;
    Status st = bmp.Save(stream, &pngClsid, nullptr);
    if (st != Ok) {
        stream->Release();
        return false;
    }
    STATSTG stat{};
    stream->Stat(&stat, STATFLAG_NONAME);
    ULARGE_INTEGER sz;
    sz.QuadPart = stat.cbSize.QuadPart;
    out.resize((size_t)sz.QuadPart);
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    stream->Read(out.data(), (ULONG)out.size(), &read);
    stream->Release();
    return read == out.size();
}

static void put16(FILE* f, unsigned v) {
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
}

static void put32(FILE* f, unsigned v) {
    for (int i = 0; i < 4; ++i) fputc((int)((v >> (8 * i)) & 0xFF), f);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: make_icon.exe <output.ico>\n");
        return 2;
    }

    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) {
        std::fprintf(stderr, "GDI+ startup failed\n");
        return 1;
    }

    CLSID png;
    if (!getEncoder(L"image/png", png)) {
        GdiplusShutdown(token);
        std::fprintf(stderr, "PNG encoder not found\n");
        return 1;
    }

    const int sizes[] = {16, 32, 48, 64, 256};
    struct Entry {
        int size;
        std::vector<BYTE> png;
    };
    std::vector<Entry> entries;

    for (int size : sizes) {
        Entry e;
        e.size = size;
        if (!renderPng(size, png, e.png)) {
            GdiplusShutdown(token);
            std::fprintf(stderr, "render failed at %d\n", size);
            return 1;
        }
        entries.push_back(std::move(e));
    }

    FILE* f = std::fopen(argv[1], "wb");
    if (!f) {
        GdiplusShutdown(token);
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }

    put16(f, 0);  // reserved
    put16(f, 1);  // type: icon
    put16(f, (unsigned)entries.size());

    unsigned offset = 6 + 16 * (unsigned)entries.size();
    for (const Entry& e : entries) {
        fputc(e.size >= 256 ? 0 : e.size, f);  // width
        fputc(e.size >= 256 ? 0 : e.size, f);  // height
        fputc(0, f);                           // palette
        fputc(0, f);                           // reserved
        put16(f, 1);                           // planes
        put16(f, 32);                          // bpp
        put32(f, (unsigned)e.png.size());
        put32(f, offset);
        offset += (unsigned)e.png.size();
    }
    for (const Entry& e : entries) fwrite(e.png.data(), 1, e.png.size(), f);
    std::fclose(f);

    GdiplusShutdown(token);
    std::printf("icon: %s (%u sizes)\n", argv[1], (unsigned)entries.size());
    return 0;
}
