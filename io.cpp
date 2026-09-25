// ============================================================================
//  io.cpp  ——  持久化与输入输出
//
//    · .edraft 存档：UTF-8 文本格式，一行一个对象，图片以 base64 内嵌
//      （因此单个 .edraft 文件自包含，拷到别处不会丢图）
//    · 加载器按"文件头版本号 + 行内字段个数"双重判断，能读回历代旧格式
//    · 图片：GDI+ 解码；粘贴/插入的图统一转 PNG 存储以压缩体积
//    · 剪贴板：优先 PNG，其次 CF_DIBV5 / CF_DIB / CF_BITMAP，最后才是文字
// ============================================================================
#include "edraft.h"
#include <commdlg.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>

// ------------------------------------------------------------------ 编码转换
string W2U8(const wstring& w) {
    if (w.empty()) return string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
wstring U82W(const string& s) {
    if (s.empty()) return wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// -------------------------------------------------------------------- base64
static const char* B64TAB = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static string B64Enc(const vector<BYTE>& v) {
    string out;
    out.reserve(((v.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i < v.size()) {
        unsigned a = v[i++];
        unsigned b = i < v.size() ? v[i++] : 0;
        unsigned c = i < v.size() ? v[i++] : 0;
        unsigned t = (a << 16) | (b << 8) | c;
        out += B64TAB[(t >> 18) & 63]; out += B64TAB[(t >> 12) & 63];
        out += B64TAB[(t >> 6) & 63];  out += B64TAB[t & 63];
    }
    size_t pad = (3 - v.size() % 3) % 3;          // 按标准补 '='
    while (pad--) out[out.size() - 1 - pad] = '=';
    return out;
}
static int B64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static bool B64Dec(const string& s, vector<BYTE>& out) {
    out.clear();
    unsigned acc = 0; int bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\r' || c == '\n') break;
        int v = B64Val(c);
        if (v < 0) continue;
        acc = (acc << 6) | (unsigned)v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((BYTE)((acc >> bits) & 0xFF)); }
    }
    return !out.empty();
}

// -------------------------------------------------------------------- 文件 IO
// 用 Win32 API 而不是 fstream：避免 MinGW 下宽字符路径的移植问题
bool ReadAll(const wstring& path, string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, nullptr);
    data.assign((size_t)sz, '\0');
    DWORD got = 0;
    BOOL ok = ReadFile(h, sz ? &data[0] : nullptr, sz, &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    data.resize((size_t)got);
    return true;
}
bool WriteAll(const wstring& path, const string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr);
    CloseHandle(h);
    return ok && put == data.size();
}
bool PickFile(HWND hwnd, wstring& out, bool save, const wchar_t* filter, const wchar_t* defExt) {
    wchar_t buf[MAX_PATH] = { 0 };
    if (!out.empty()) wcsncpy_s(buf, MAX_PATH, out.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return false;
    out = buf;
    return true;
}
// 存档只允许 .edraft（大小写不限）
bool HasEdraftExt(const wstring& p) {
    if (p.size() < 7) return false;
    wstring e = p.substr(p.size() - 7);
    for (size_t i = 0; i < e.size(); ++i) e[i] = (wchar_t)towlower(e[i]);
    return e == L".edraft";
}
wstring EnsureEdraftExt(const wstring& p) {
    return HasEdraftExt(p) ? p : (p + L"." + FILE_EXT);
}

// -------------------------------------------------------------------- 保存
static string Num(double v) {
    std::ostringstream os;
    os << std::setprecision(12) << v;
    return os.str();
}

// 存档格式（EDRAFT3）一览：
//   RECT  id x y w h 颜色 线宽 线型
//   TEXT  id x y w h 字号 粗体 颜色 线宽 线型 <正文（\n 转义为 \\n）>
//   IMAGE id x y w h <base64(PNG)>
//   ARROW id 起点id 终点id 起点边 终点边 直线/折线 箭头样式 线型 颜色 线宽 x1 y1 x2 y2 折弯
bool SaveDoc(const wstring& pathIn) {
    wstring path = EnsureEdraftExt(pathIn);
    std::ostringstream os;
    os << W2U8(FILE_HDR) << '\n';

    for (const Obj& o : g_objs) {
        switch (o.type) {
        case OT::Rect:
            os << "RECT " << o.id << ' ' << Num(o.x) << ' ' << Num(o.y) << ' '
               << Num(o.w) << ' ' << Num(o.h) << ' ' << o.colorRGB << ' '
               << Num(o.strokeW) << ' ' << o.dashMode << '\n';
            break;
        case OT::Text: {
            string t = W2U8(o.text), esc;
            for (char c : t) {                       // 正文里的换行与反斜杠要转义
                if (c == '\\')      esc += "\\\\";
                else if (c == '\n') esc += "\\n";
                else if (c == '\r') esc += "\\r";
                else                esc += c;
            }
            os << "TEXT " << o.id << ' ' << Num(o.x) << ' ' << Num(o.y) << ' '
               << Num(o.w) << ' ' << Num(o.h) << ' ' << Num(o.fontSize) << ' '
               << (o.bold ? 1 : 0) << ' ' << o.colorRGB << ' ' << Num(o.strokeW)
               << ' ' << o.dashMode << ' ' << esc << '\n';
            break;
        }
        case OT::Image:
            os << "IMAGE " << o.id << ' ' << Num(o.x) << ' ' << Num(o.y) << ' '
               << Num(o.w) << ' ' << Num(o.h) << ' '
               << (o.imgBytes ? B64Enc(*o.imgBytes) : string()) << '\n';
            break;
        case OT::Arrow:
            os << "ARROW " << o.id << ' ' << o.fromId << ' ' << o.toId << ' '
               << o.fromSide << ' ' << o.toSide << ' ' << o.lineMode << ' '
               << o.headMode << ' ' << o.dashMode << ' ' << o.colorRGB << ' '
               << Num(o.strokeW) << ' '
               << Num(o.x) << ' ' << Num(o.y) << ' ' << Num(o.x2) << ' ' << Num(o.y2)
               << ' ' << Num(o.bend) << '\n';
            break;
        }
    }
    if (!WriteAll(path, os.str())) {
        MessageBoxW(g_hwnd, L"写入文件失败。", APP_NAME, MB_OK | MB_ICONERROR);
        return false;
    }
    g_file = path;
    g_dirty = false;
    UpdateTitle();
    return true;
}

static void Unesc(const string& in, wstring& out) {
    string s;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            char c = in[i + 1];
            if (c == 'n')       { s += '\n'; ++i; }
            else if (c == 'r')  { s += '\r'; ++i; }
            else if (c == '\\') { s += '\\'; ++i; }
            else s += in[i];
        } else s += in[i];
    }
    out = U82W(s);
}

// -------------------------------------------------------------------- 读取
// 兼容历代格式。判断依据优先级：文件头版本号 > 该行数字字段个数
//   fmtVer: 1=WBDK1  2=WBDK2  3=EDRAFT1  4=EDRAFT2  5=EDRAFT3
bool LoadDoc(const wstring& path) {
    if (!HasEdraftExt(path)) {
        MessageBoxW(g_hwnd,
            L"只支持 .edraft 文件。\n\n若是旧版 .wbdk 文件，请先把扩展名改成 .edraft 再打开。",
            APP_NAME, MB_OK | MB_ICONWARNING);
        return false;
    }
    string data;
    if (!ReadAll(path, data)) {
        MessageBoxW(g_hwnd, L"读取文件失败。", APP_NAME, MB_OK | MB_ICONERROR);
        return false;
    }

    vector<Obj> objs;
    int maxId = 0, fmtVer = 0;
    size_t pos = 0;
    string line;

    auto nextLine = [&]() -> bool {
        if (pos >= data.size()) return false;
        size_t e = data.find('\n', pos);
        if (e == string::npos) e = data.size();
        line = data.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };

    while (nextLine()) {
        if (line.empty()) continue;

        // 整行切词，并记录每个词的结束位置（正文含空格，需"第 k 个词之后的剩余"）
        vector<string> tk;
        vector<size_t> ends;
        {
            size_t p = 0;
            while (p < line.size()) {
                while (p < line.size() && isspace((unsigned char)line[p])) ++p;
                if (p >= line.size()) break;
                size_t s = p;
                while (p < line.size() && !isspace((unsigned char)line[p])) ++p;
                tk.push_back(line.substr(s, p - s));
                ends.push_back(p);
            }
        }
        if (tk.empty()) continue;

        auto numAt = [&](int i) -> double {
            return (i >= 0 && i < (int)tk.size()) ? atof(tk[i].c_str()) : 0.0;
        };
        auto intAt = [&](int i) -> int {
            return (i >= 0 && i < (int)tk.size()) ? atoi(tk[i].c_str()) : 0;
        };
        auto restAfter = [&](int k) -> string {          // k 为 1-based 词序
            if (k < 1 || k > (int)ends.size()) return string();
            size_t q = ends[k - 1];
            while (q < line.size() && isspace((unsigned char)line[q])) ++q;
            return line.substr(q);
        };

        const string& t = tk[0];
        if      (t == "WBDK1")   { fmtVer = 1; continue; }
        else if (t == "WBDK2")   { fmtVer = 2; continue; }
        else if (t == "EDRAFT1") { fmtVer = 3; continue; }
        else if (t == "EDRAFT2") { fmtVer = 4; continue; }
        else if (t == "EDRAFT3") { fmtVer = 5; continue; }

        Obj o;
        if (t == "RECT") {
            o.type = OT::Rect;
            o.id = intAt(1);
            o.x = numAt(2); o.y = numAt(3); o.w = numAt(4); o.h = numAt(5);
            int c = ((int)tk.size() >= 7) ? intAt(6) : 0;
            o.colorRGB = (c == 0) ? 0x4A82B4 : c;
            double sw = (fmtVer >= 4) ? numAt(7) : 10.0;
            o.strokeW = (sw <= 0) ? 10.0 : sw;
            o.dashMode = (fmtVer >= 5) ? intAt(8) : 0;
        } else if (t == "TEXT") {
            o.type = OT::Text;
            o.id = intAt(1);
            o.x = numAt(2); o.y = numAt(3); o.w = numAt(4); o.h = numAt(5);
            if (fmtVer >= 3) {
                o.fontSize = numAt(6);
                if (o.fontSize < 6) o.fontSize = 14;
                o.bold = (intAt(7) != 0);
                int c = intAt(8);
                o.colorRGB = (c == 0) ? 0x1A1A1A : c;
                double sw = (fmtVer >= 4) ? numAt(9) : 4.0;
                o.strokeW = (sw <= 0) ? 4.0 : sw;
                o.dashMode = (fmtVer >= 5) ? intAt(10) : 0;
                Unesc(restAfter(fmtVer >= 5 ? 11 : (fmtVer >= 4 ? 10 : 9)), o.text);
            } else {                                     // 最初版本：5 个数字 + 正文
                o.fontSize = 14; o.bold = false; o.colorRGB = 0x1A1A1A; o.strokeW = 4.0; o.dashMode = 0;
                Unesc(restAfter(6), o.text);
            }
        } else if (t == "IMAGE") {
            o.type = OT::Image;
            o.id = intAt(1);
            o.x = numAt(2); o.y = numAt(3); o.w = numAt(4); o.h = numAt(5);
            o.strokeW = 1.2;                             // 图片边框固定较细
            vector<BYTE> bytes;
            if (B64Dec(restAfter(6), bytes)) {
                o.imgBytes = std::make_shared<vector<BYTE>>(std::move(bytes));
                MakeBmp(o);
            }
        } else if (t == "ARROW") {
            o.type = OT::Arrow;
            o.id = intAt(1);
            int nf = (int)tk.size() - 1;                 // 数字字段个数
            if (fmtVer >= 5 || nf >= 15) {               // 15：含线型与折弯
                o.fromId = intAt(2);  o.toId = intAt(3);
                o.fromSide = intAt(4); o.toSide = intAt(5);
                o.lineMode = intAt(6); o.headMode = intAt(7);
                o.dashMode = intAt(8);
                int c = intAt(9); o.colorRGB = (c == 0) ? 0x282828 : c;
                double sw = numAt(10); o.strokeW = (sw <= 0) ? 6.0 : sw;
                o.x = numAt(11); o.y = numAt(12); o.x2 = numAt(13); o.y2 = numAt(14);
                o.bend = numAt(15);
            } else if (fmtVer == 4 || nf >= 13) {        // 13：含线宽
                o.fromId = intAt(2);  o.toId = intAt(3);
                o.fromSide = intAt(4); o.toSide = intAt(5);
                o.lineMode = intAt(6); o.headMode = intAt(7);
                int c = intAt(8); o.colorRGB = (c == 0) ? 0x282828 : c;
                double sw = numAt(9); o.strokeW = (sw <= 0) ? 6.0 : sw;
                o.x = numAt(10); o.y = numAt(11); o.x2 = numAt(12); o.y2 = numAt(13);
                o.bend = 0.5;
            } else if (fmtVer == 3 || nf >= 12) {        // 12：有样式，无线宽
                o.fromId = intAt(2);  o.toId = intAt(3);
                o.fromSide = intAt(4); o.toSide = intAt(5);
                o.lineMode = intAt(6); o.headMode = intAt(7);
                int c = intAt(8); o.colorRGB = (c == 0) ? 0x282828 : c;
                o.strokeW = 6.0; o.bend = 0.5;
                o.x = numAt(9); o.y = numAt(10); o.x2 = numAt(11); o.y2 = numAt(12);
            } else if (fmtVer == 2 || nf >= 9) {         // 9：有绑定边
                o.fromId = intAt(2);  o.toId = intAt(3);
                o.fromSide = intAt(4); o.toSide = intAt(5);
                o.colorRGB = 0x282828; o.strokeW = 6.0; o.bend = 0.5;
                o.x = numAt(6); o.y = numAt(7); o.x2 = numAt(8); o.y2 = numAt(9);
            } else {                                     // 7：最初版本
                o.fromId = intAt(2);  o.toId = intAt(3);
                o.colorRGB = 0x282828; o.strokeW = 6.0; o.bend = 0.5;
                o.x = numAt(4); o.y = numAt(5); o.x2 = numAt(6); o.y2 = numAt(7);
            }
        } else continue;

        maxId = std::max(maxId, o.id);
        objs.push_back(std::move(o));
    }

    g_objs = std::move(objs);
    g_nextId = maxId + 1;
    g_sels.clear();
    g_undo.clear(); g_redo.clear();
    g_file = path;
    g_dirty = false;
    UpdateTitle();
    UpdateUndoButtons();
    InvalidateRect(g_hwnd, nullptr, FALSE);
    return true;
}

// ------------------------------------------------------------------ PNG 编解码
int EncoderClsid(const WCHAR* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    Gdiplus::ImageCodecInfo* p = (Gdiplus::ImageCodecInfo*)malloc(size);
    if (!p) return -1;
    Gdiplus::GetImageEncoders(num, size, p);
    int r = -1;
    for (UINT j = 0; j < num; ++j)
        if (wcscmp(p[j].MimeType, mime) == 0) { *clsid = p[j].Clsid; r = (int)j; break; }
    free(p);
    return r;
}
// 把位图编码成 PNG 字节（用于把粘贴/插入的图紧凑地存进 .edraft）
static bool EncodePNG(Gdiplus::Bitmap* bmp, vector<BYTE>& out) {
    CLSID clsid;
    if (EncoderClsid(L"image/png", &clsid) < 0) return false;
    IStream* stm = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &stm) != S_OK || !stm) return false;
    bool ok = (bmp->Save(stm, &clsid, nullptr) == Gdiplus::Ok);
    if (ok) {
        HGLOBAL hg = nullptr;
        if (GetHGlobalFromStream(stm, &hg) == S_OK && hg) {
            SIZE_T sz = GlobalSize(hg);
            void* p = GlobalLock(hg);
            if (p) out.assign((BYTE*)p, (BYTE*)p + sz);
            else ok = false;
            if (p) GlobalUnlock(hg);
        } else ok = false;
    }
    stm->Release();
    return ok && !out.empty();
}

// ------------------------------------------------------------------ 插入图片
void InsertImage(HWND hwnd) {
    wstring path;
    const wchar_t* flt = L"图片文件\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0所有文件\0*.*\0";
    if (!PickFile(hwnd, path, false, flt, nullptr)) return;

    string raw;
    if (!ReadAll(path, raw)) {
        MessageBoxW(hwnd, L"无法读取该图片。", L"插入图片", MB_OK | MB_ICONERROR);
        return;
    }
    Obj o;
    o.type = OT::Image;
    o.strokeW = 1.2;
    o.imgBytes = std::make_shared<vector<BYTE>>(raw.begin(), raw.end());
    if (!MakeBmp(o)) {
        MessageBoxW(hwnd, L"GDI+ 无法解码该图片。", L"插入图片", MB_OK | MB_ICONERROR);
        return;
    }
    double iw = (double)o.bmp->GetWidth(), ih = (double)o.bmp->GetHeight();
    double k = (iw > 480.0) ? (480.0 / iw) : 1.0;         // 过大的图等比缩小
    o.w = iw * k; o.h = ih * k;

    RECT rc; GetClientRect(hwnd, &rc);
    o.x = S2X((rc.right - rc.left) / 2.0) - o.w / 2.0;
    o.y = S2Y(g_tbH + (rc.bottom - rc.top - g_tbH) / 2.0) - o.h / 2.0;

    SnapUndo();
    o.id = g_nextId++;
    g_objs.push_back(o);
    SetSel(o.id);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------------ 导出 PNG
// 按内容包围盒自动裁切，四周留 30 像素白边；网格不参与导出
void ExportPNG(HWND hwnd) {
    if (g_objs.empty()) {
        MessageBoxW(hwnd, L"画布为空，无需导出。", L"导出 PNG", MB_OK | MB_ICONINFORMATION);
        return;
    }
    wstring path = g_file;
    if (!path.empty()) {
        size_t d = path.find_last_of(L"\\/");
        path = (d == wstring::npos ? path : path.substr(0, d + 1)) + L"export.png";
    }
    if (!PickFile(hwnd, path, true, L"PNG 图片\0*.png\0", L"png")) return;

    double x0, y0, x1, y1;
    ContentBounds(x0, y0, x1, y1);
    double m = 30.0;
    int W = (int)ceil(x1 - x0 + 2 * m), H = (int)ceil(y1 - y0 + 2 * m);
    if (W <= 0 || H <= 0 || W > 20000 || H > 20000) {
        MessageBoxW(hwnd, L"内容尺寸过大，无法导出。", L"导出 PNG", MB_OK | MB_ICONERROR);
        return;
    }
    Gdiplus::Bitmap bmp(W, H, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&bmp);
        g.Clear(Gdiplus::Color(255, 255, 255, 255));
        double os = g_scale, opx = g_panX, opy = g_panY;   // 临时改成 1:1 铺满
        g_scale = 1.0; g_panX = -x0 + m; g_panY = -y0 + m;
        DrawScene(g);
        g_scale = os; g_panX = opx; g_panY = opy;
    }
    CLSID clsid;
    if (EncoderClsid(L"image/png", &clsid) < 0) {
        MessageBoxW(hwnd, L"找不到 PNG 编码器。", L"导出 PNG", MB_OK | MB_ICONERROR);
        return;
    }
    if (bmp.Save(path.c_str(), &clsid, nullptr) != Gdiplus::Ok)
        MessageBoxW(hwnd, L"导出失败。", L"导出 PNG", MB_OK | MB_ICONERROR);
    else
        MessageBoxW(hwnd, L"已导出 PNG。", L"导出 PNG", MB_OK | MB_ICONINFORMATION);
}

// ------------------------------------------------------------ 剪贴板粘贴
// 从 CF_DIB / CF_DIBV5 数据拼出一个内存 BMP 文件。
// 剪贴板给的只是 BITMAPINFO + 像素，缺文件头，GDI+ 无法直接解码，必须自己补。
static bool DibToBmpBytes(const BYTE* dib, SIZE_T dibSize, vector<BYTE>& out) {
    if (!dib || dibSize < sizeof(BITMAPINFOHEADER)) return false;
    const BITMAPINFOHEADER* bih = (const BITMAPINFOHEADER*)dib;
    if (bih->biSize < sizeof(BITMAPINFOHEADER)) return false;

    DWORD extra = 0;                                    // 头后的调色板或位域掩膜字节数
    if (bih->biBitCount <= 8) {
        DWORD n = bih->biClrUsed ? bih->biClrUsed : (1u << bih->biBitCount);
        extra = n * sizeof(RGBQUAD);
    } else if (bih->biCompression == BI_BITFIELDS) {
        extra = 3 * sizeof(DWORD);
    }
    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42;                                // 'BM'
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + bih->biSize + extra;
    bfh.bfSize = (DWORD)(sizeof(BITMAPFILEHEADER) + dibSize);

    out.clear();
    out.reserve(sizeof(BITMAPFILEHEADER) + dibSize);
    const BYTE* pb = (const BYTE*)&bfh;
    out.insert(out.end(), pb, pb + sizeof(BITMAPFILEHEADER));
    out.insert(out.end(), dib, dib + dibSize);
    return true;
}
static bool ClipText(wstring& text) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return false;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) return false;
    wchar_t* p = (wchar_t*)GlobalLock(h);
    if (!p) return false;
    text = p;
    GlobalUnlock(h);
    return !text.empty();
}
// 取剪贴板图片，输出可直接存档的 PNG 字节
static bool ClipImage(vector<BYTE>& bytesOut) {
    UINT fPNG = RegisterClipboardFormatW(L"PNG");
    auto grab = [](UINT fmt, vector<BYTE>& dst) -> bool {
        if (!IsClipboardFormatAvailable(fmt)) return false;
        HANDLE h = GetClipboardData(fmt);
        if (!h) return false;
        SIZE_T sz = GlobalSize(h);
        void* p = GlobalLock(h);
        if (!p || sz == 0) { if (p) GlobalUnlock(h); return false; }
        dst.assign((BYTE*)p, (BYTE*)p + sz);
        GlobalUnlock(h);
        return true;
    };
    vector<BYTE> raw;
    if (fPNG && grab(fPNG, raw)) { bytesOut = raw; return true; }   // 已是 PNG，直接用

    UINT dibFmt = 0;
    if (IsClipboardFormatAvailable(CF_DIBV5)) dibFmt = CF_DIBV5;
    else if (IsClipboardFormatAvailable(CF_DIB)) dibFmt = CF_DIB;
    if (dibFmt && grab(dibFmt, raw)) {
        vector<BYTE> bmpBytes;
        if (!DibToBmpBytes(raw.data(), raw.size(), bmpBytes)) return false;
        Obj tmp;                                        // 借一个临时对象完成解码
        tmp.type = OT::Image;
        tmp.imgBytes = std::make_shared<vector<BYTE>>(bmpBytes);
        if (!MakeBmp(tmp)) return false;
        return EncodePNG(tmp.bmp.get(), bytesOut);      // BMP 太大，统一转 PNG
    }
    if (IsClipboardFormatAvailable(CF_BITMAP)) {
        HANDLE h = GetClipboardData(CF_BITMAP);
        if (h) {
            Gdiplus::Bitmap* b = Gdiplus::Bitmap::FromHBITMAP((HBITMAP)h, nullptr);
            bool ok = false;
            if (b && b->GetLastStatus() == Gdiplus::Ok) ok = EncodePNG(b, bytesOut);
            delete b;
            return ok;
        }
    }
    return false;
}
// Ctrl+V：图片 → 图片对象；文字 → 文本框
void DoPaste() {
    CommitEdit();
    if (!OpenClipboard(g_hwnd)) return;

    // 落点：鼠标在画布上就贴光标处，否则贴视口中心
    RECT rc; GetClientRect(g_hwnd, &rc);
    double cx = g_mouseIn ? g_mwx : S2X((rc.right - rc.left) / 2.0);
    double cy = g_mouseIn ? g_mwy : S2Y(g_tbH + (rc.bottom - rc.top - g_tbH) / 2.0);

    bool done = false;
    vector<BYTE> png;
    if (ClipImage(png)) {
        Obj o;
        o.type = OT::Image;
        o.strokeW = 1.2;
        o.imgBytes = std::make_shared<vector<BYTE>>(std::move(png));
        if (MakeBmp(o)) {
            double iw = (double)o.bmp->GetWidth(), ih = (double)o.bmp->GetHeight();
            double k = (iw > 480.0) ? (480.0 / iw) : 1.0;
            o.w = iw * k; o.h = ih * k;
            o.x = cx - o.w / 2.0; o.y = cy - o.h / 2.0;
            SnapUndo();
            o.id = g_nextId++;
            g_objs.push_back(o);
            SetSel(o.id);
            done = true;
        }
    } else {
        wstring t;
        if (ClipText(t)) {
            Obj o;
            o.type = OT::Text;
            o.text = t;
            o.colorRGB = 0x1A1A1A;
            o.strokeW = DefStroke(OT::Text);
            o.w = 360; o.h = 46;                        // 先给一行高，随后按内容增高
            o.x = cx - o.w / 2.0; o.y = cy - o.h / 2.0;
            SnapUndo();
            o.id = g_nextId++;
            g_objs.push_back(o);
            AutoGrow(&g_objs.back());
            SetSel(o.id);
            done = true;
        }
    }
    CloseClipboard();
    if (done) InvalidateRect(g_hwnd, nullptr, FALSE);
}
