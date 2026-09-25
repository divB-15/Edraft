// ============================================================================
//  ui.cpp  ——  交互控件与菜单
//
//    · 文本框就地编辑：用一个真实的 EDIT 子窗口覆盖在文本框上
//      （必须真控件，否则中文输入法 IME 无法工作）
//    · 右键上下文菜单：按选中对象的类型动态组装，支持多选批量修改
//    · 自定义数值输入框：线宽 / 字号可以手填任意数值（不用 .rc 资源，纯代码创建）
// ============================================================================
#include "edraft.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// ------------------------------------------------------------------ 标题栏
void UpdateTitle() {
    wstring t = APP_NAME;
    if (!g_file.empty()) {
        size_t d = g_file.find_last_of(L"\\/");
        t += L" — " + (d == wstring::npos ? g_file : g_file.substr(d + 1));
    }
    if (g_dirty) t += L" *";
    SetWindowTextW(g_hwnd, t.c_str());
}

void MarkTool(Tool t) {
    g_tool = t;
    RefreshButtons();                 // 自绘按钮靠 IsBtnChecked 决定外观，必须重绘
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------- 文本框就地编辑
// 按该文本框自己的字号/粗体/宽度重新量一次高度，保证内容不会被截断
void AutoGrow(Obj* o) {
    if (!o || o->type != OT::Text) return;
    HDC hdc = GetDC(g_hwnd);
    if (!hdc) return;
    Gdiplus::Graphics g(hdc);
    Gdiplus::Font f(FONT_NAME, (Gdiplus::REAL)o->fontSize,
                    o->bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                    Gdiplus::UnitPoint);
    Gdiplus::RectF bb;
    g.MeasureString(o->text.c_str(), -1, &f,
                    Gdiplus::RectF(0, 0, (Gdiplus::REAL)(o->w - 14.0f), 100000.0f), &bb);
    o->h = std::max(o->h, (double)bb.Height + o->fontSize * 0.8 + 6.0);
    ReleaseDC(g_hwnd, hdc);
}

void BeginEdit(int id) {
    Obj* o = ObjOf(id);
    if (!o || o->type != OT::Text) return;

    g_editingId = id;
    int sx = (int)W2X(o->x) + 2, sy = (int)W2Y(o->y) + 2;
    int sw = (int)(o->w * g_scale) - 4, sh = (int)(o->h * g_scale) - 4;
    if (sw < 30) sw = 30;
    if (sh < 24) sh = 24;

    // 编辑框字号 = 磅 × DPI/72 × 缩放，与 GDI+ 的 UnitPoint 渲染严格一致
    LONG fh = -(LONG)lround(o->fontSize * (g_dpi / 72.0) * g_scale);
    if (fh > -6) fh = -6;
    HFONT hf = CreateFontW(fh, 0, 0, 0, o->bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, FONT_NAME);
    static HFONT s_lastFont = nullptr;
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)hf, TRUE);
    if (s_lastFont) DeleteObject(s_lastFont);
    s_lastFont = hf;

    SetWindowTextW(g_edit, o->text.c_str());
    MoveWindow(g_edit, sx, sy, sw, sh, TRUE);
    ShowWindow(g_edit, SW_SHOW);
    SetFocus(g_edit);
    SendMessageW(g_edit, EM_SETSEL, 0, -1);           // 全选，方便直接覆盖重打
}

void CommitEdit() {
    if (g_editingId < 0) return;
    int id = g_editingId;
    g_editingId = -1;

    wchar_t buf[8192];
    GetWindowTextW(g_edit, buf, 8192);
    ShowWindow(g_edit, SW_HIDE);

    Obj* o = ObjOf(id);
    if (!o) return;
    wstring t = buf;
    if (t == o->text) return;

    SnapUndo();
    o = ObjOf(id);
    if (!o) return;
    o->text = t;
    AutoGrow(o);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------- 自定义数值输入
static HWND g_ib = nullptr, g_ibEdit = nullptr;      // 输入框窗口与其中的编辑框
static int  g_ibKind = 0;                            // 0 = 线宽，1 = 字号

// 把输入的数值应用到所有选中的同类对象
static void ApplyCustomValue(int kind, double v) {
    Obj* prim = ObjOf(PrimaryId());
    if (!prim) return;
    if (kind == 0) {
        v = std::max(STROKE_MIN, std::min(STROKE_MAX, v));
        SnapUndo();
        for (Obj& o : g_objs)
            if (o.type != OT::Image && IsSel(o.id)) o.strokeW = v;
    } else {
        v = std::max(6.0, std::min(200.0, v));
        SnapUndo();
        for (Obj& o : g_objs)
            if (o.type == OT::Text && IsSel(o.id)) { o.fontSize = v; AutoGrow(&o); }
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 关闭输入框并显式把主窗口拉回前台。
// 背景：TrackPopupMenu 之后主窗口常拿不回激活权，若再禁用它就会表现为"被最小化"，
// 所以这里既不禁用主窗口，也在关闭时主动恢复。
static void CloseInputBox(HWND h) {
    DestroyWindow(h);
    if (g_hwnd) {
        if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        SetForegroundWindow(g_hwnd);
        SetActiveWindow(g_hwnd);
    }
}

LRESULT CALLBACK InputProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        HINSTANCE hi = ((LPCREATESTRUCT)l)->hInstance;
        wchar_t hint[64];
        swprintf(hint, 64, g_ibKind == 0 ? L"线宽（%g ~ %g）" : L"字号 磅（6 ~ 200）",
                 STROKE_MIN, STROKE_MAX);
        HWND st = CreateWindowW(L"STATIC", hint, WS_CHILD | WS_VISIBLE,
                                14, 14, 232, 20, h, nullptr, hi, nullptr);
        g_ibEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                 14, 38, 232, 24, h, nullptr, hi, nullptr);
        HWND ok = CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                66, 76, 76, 28, h, (HMENU)1, hi, nullptr);
        HWND cc = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE,
                                150, 76, 76, 28, h, (HMENU)2, hi, nullptr);
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        for (HWND c : { st, g_ibEdit, ok, cc }) SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 1) {                          // 确定
            wchar_t buf[64] = { 0 };
            GetWindowTextW(g_ibEdit, buf, 64);
            ApplyCustomValue(g_ibKind, wcstod(buf, nullptr));
            CloseInputBox(h);
        } else if (LOWORD(w) == 2) {                   // 取消
            CloseInputBox(h);
        }
        return 0;
    case WM_DESTROY:
        g_ib = nullptr;
        if (g_hwnd) {
            EnableWindow(g_hwnd, TRUE);                // 保险：确保主窗口一定启用
            if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        }
        return 0;
    case WM_CLOSE:
        CloseInputBox(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void ShowInputBox(int kind, double cur) {
    if (g_ib) { SetForegroundWindow(g_ib); return; }
    if (!ObjOf(PrimaryId())) return;

    g_ibKind = kind;
    RECT rc; GetWindowRect(g_hwnd, &rc);
    int W = 260, H = 150;
    int x = rc.left + ((rc.right - rc.left) - W) / 2;
    int y = rc.top + ((rc.bottom - rc.top) - H) / 2;

    g_ib = CreateWindowExW(WS_EX_DLGMODALFRAME, L"EdraftInputBox",
                           kind == 0 ? L"自定义线宽" : L"自定义字号",
                           WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE,
                           x, y, W, H, g_hwnd, nullptr, g_inst, nullptr);
    if (!g_ib) return;

    wchar_t buf[64];
    swprintf(buf, 64, L"%g", cur);
    SetWindowTextW(g_ibEdit, buf);
    SendMessageW(g_ibEdit, EM_SETSEL, 0, -1);
    SetFocus(g_ibEdit);
    SetForegroundWindow(g_ib);
    // 注意：这里刻意【不】禁用主窗口 —— 禁用 owner 会让它无法被激活，
    // 与菜单后的激活问题叠加就会表现为"主窗口被最小化"。
}

// ------------------------------------------------------------- 右键上下文菜单
// 按选中集合动态组装：统计各类对象数量，只显示对当前选择有意义的项
void ShowContextMenu(HWND hwnd, int sx, int sy) {
    if (sy < g_tbH) return;
    double wx = S2X(sx), wy = S2Y(sy);

    int hit = HitTest(wx, wy);
    if (hit >= 0 && !IsSel(hit)) SetSel(hit);
    InvalidateRect(hwnd, nullptr, FALSE);

    int nSel = (int)g_sels.size();
    int nText = 0, nArrow = 0, nBox = 0;
    for (int sid : g_sels) {
        if (Obj* o = ObjOf(sid)) {
            if (o->type == OT::Text) ++nText;
            else if (o->type == OT::Arrow) ++nArrow;
            if (o->type != OT::Image) ++nBox;
        }
    }
    Obj* prim = ObjOf(PrimaryId());

    HMENU m = CreatePopupMenu();
    HMENU sm = nullptr, cm = nullptr, wm = nullptr, dm = nullptr;
    POINT pt{ sx, sy };
    ClientToScreen(hwnd, &pt);

    if (nSel > 0 && prim) {
        wchar_t info[160];                             // 首行显示"是什么 / 选了几个"
        if (nSel == 1) {
            if (prim->type == OT::Arrow) swprintf(info, 160, L"箭头 #%d", prim->id);
            else swprintf(info, 160, L"%s #%d   %.0f × %.0f",
                          TypeName(prim->type), prim->id, prim->w, prim->h);
        } else {
            swprintf(info, 160, L"已选中 %d 个对象", nSel);
        }
        AppendMenuW(m, MF_STRING | MF_DISABLED | MF_GRAYED, 0, info);
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

        if (nText > 0) {                               // —— 文本框专属 ——
            if (nSel == 1) AppendMenuW(m, MF_STRING, IDM_EDITTEXT, L"编辑文字");
            AppendMenuW(m, MF_STRING | (prim->bold ? MF_CHECKED : 0), IDM_BOLD, L"粗体");
            sm = CreatePopupMenu();
            for (int i = 0; i < 6; ++i) {
                wchar_t b[32];
                swprintf(b, 32, L"%g 磅", FONT_SIZES[i]);
                AppendMenuW(sm, MF_STRING | (fabs(prim->fontSize - FONT_SIZES[i]) < 0.1 ? MF_CHECKED : 0),
                            IDM_SIZE_BASE + i, b);
            }
            AppendMenuW(sm, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(sm, MF_STRING, IDM_SIZE_CUSTOM, L"自定义…");
            AppendMenuW(m, MF_POPUP, (UINT_PTR)sm, L"字号");
        }
        if (nArrow > 0) {                              // —— 箭头专属 ——
            AppendMenuW(m, MF_STRING | (prim->lineMode == 0 ? MF_CHECKED : 0), IDM_LINE_STRAIGHT, L"直线箭头");
            AppendMenuW(m, MF_STRING | (prim->lineMode == 1 ? MF_CHECKED : 0), IDM_LINE_POLY, L"折线箭头");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING | (prim->headMode == 0 ? MF_CHECKED : 0), IDM_HEAD_NONE, L"无箭头");
            AppendMenuW(m, MF_STRING | (prim->headMode == 1 ? MF_CHECKED : 0), IDM_HEAD_SINGLE, L"单箭头");
            AppendMenuW(m, MF_STRING | (prim->headMode == 2 ? MF_CHECKED : 0), IDM_HEAD_DOUBLE, L"双箭头");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            dm = CreatePopupMenu();
            AppendMenuW(dm, MF_STRING | (prim->dashMode == 0 ? MF_CHECKED : 0), IDM_DASH_SOLID, L"实线");
            AppendMenuW(dm, MF_STRING | (prim->dashMode == 1 ? MF_CHECKED : 0), IDM_DASH_DASH, L"虚线");
            AppendMenuW(dm, MF_STRING | (prim->dashMode == 2 ? MF_CHECKED : 0), IDM_DASH_DOT, L"点线");
            AppendMenuW(m, MF_POPUP, (UINT_PTR)dm, L"线型");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        }
        if (nBox > 0) {                                // —— 线宽与颜色（方框/文本/箭头通用） ——
            wm = CreatePopupMenu();
            for (int i = 0; i < NSW; ++i)
                AppendMenuW(wm, MF_STRING | (fabs(prim->strokeW - STROKES[i]) < 0.05 ? MF_CHECKED : 0),
                            IDM_SW_BASE + i, SWNAMES[i]);
            AppendMenuW(wm, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(wm, MF_STRING, IDM_SW_CUSTOM, L"自定义…");
            AppendMenuW(m, MF_POPUP, (UINT_PTR)wm, L"线宽");

            cm = CreatePopupMenu();
            for (int i = 0; i < NCOL; ++i)
                AppendMenuW(cm, MF_STRING | (prim->colorRGB == COLORS[i].rgb ? MF_CHECKED : 0),
                            IDM_COLOR_BASE + i, COLORS[i].name);
            AppendMenuW(m, MF_POPUP, (UINT_PTR)cm, L"颜色");
        }
        if (nSel == 1 && prim->type == OT::Image)
            AppendMenuW(m, MF_STRING, IDM_IMG_ORIG, L"按原始像素显示");

        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_DELOBJ, (nSel > 1) ? L"删除选中对象" : L"删除此对象");
    } else {                                           // —— 点空白处：画布菜单 ——
        AppendMenuW(m, MF_STRING, IDM_HOME, L"回到原点");
        AppendMenuW(m, MF_STRING, IDM_FIT2, L"适应窗口");
        AppendMenuW(m, MF_STRING | (g_grid ? MF_CHECKED : 0), IDM_GRIDM, L"显示网格");
    }

    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    // TrackPopupMenu 之后主窗口常拿不回前台激活，必须显式处理，否则会"掉到后台/看似最小化"
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);

    if (sm) DestroyMenu(sm);
    if (cm) DestroyMenu(cm);
    if (wm) DestroyMenu(wm);
    if (dm) DestroyMenu(dm);
    DestroyMenu(m);
}

// 处理菜单命令。样式类改动一律批量应用到所有选中的同类对象
void HandleMenu(int id) {
    CommitEdit();
    Obj* prim = ObjOf(PrimaryId());

    if (id >= IDM_SIZE_BASE && id < IDM_SIZE_BASE + 6) {
        if (!prim) return;
        double v = FONT_SIZES[id - IDM_SIZE_BASE];
        SnapUndo();
        for (Obj& o : g_objs)
            if (o.type == OT::Text && IsSel(o.id)) { o.fontSize = v; AutoGrow(&o); }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id == IDM_SIZE_CUSTOM) { ShowInputBox(1, prim ? prim->fontSize : 14.0); return; }
    if (id == IDM_SW_CUSTOM)   { ShowInputBox(0, prim ? prim->strokeW : 4.0);   return; }

    if (id >= IDM_COLOR_BASE && id < IDM_COLOR_BASE + NCOL) {
        if (!prim) return;
        int c = COLORS[id - IDM_COLOR_BASE].rgb;
        SnapUndo();
        for (Obj& o : g_objs)
            if (o.type != OT::Image && IsSel(o.id)) o.colorRGB = c;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id >= IDM_SW_BASE && id < IDM_SW_BASE + NSW) {
        if (!prim) return;
        double v = STROKES[id - IDM_SW_BASE];
        SnapUndo();
        for (Obj& o : g_objs)
            if (o.type != OT::Image && IsSel(o.id)) o.strokeW = v;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }

    if (!prim) {                                       // 画布菜单（未选中任何对象）
        if (id == IDM_HOME) GoHome();
        else if (id == IDM_FIT2) ZoomFit(g_hwnd);
        else if (id == IDM_GRIDM) { g_grid = !g_grid; RefreshButtons(); InvalidateRect(g_hwnd, nullptr, FALSE); }
        return;
    }

    switch (id) {
    case IDM_EDITTEXT:
        if (g_sels.size() == 1 && prim->type == OT::Text) BeginEdit(PrimaryId());
        break;
    case IDM_BOLD: {
        bool nb = !prim->bold;                         // 以主选中对象为基准统一取反
        SnapUndo();
        for (Obj& o : g_objs)
            if (o.type == OT::Text && IsSel(o.id)) { o.bold = nb; AutoGrow(&o); }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        break;
    }
    case IDM_LINE_STRAIGHT: case IDM_LINE_POLY: {
        int v = (id == IDM_LINE_POLY) ? 1 : 0;
        SnapUndo();
        for (Obj& o : g_objs)
            if (o.type == OT::Arrow && IsSel(o.id)) o.lineMode = v;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        break;
    }
    case IDM_HEAD_NONE: case IDM_HEAD_SINGLE: case IDM_HEAD_DOUBLE: {
        int v = (id == IDM_HEAD_NONE) ? 0 : ((id == IDM_HEAD_SINGLE) ? 1 : 2);
        SnapUndo();
        for (Obj& o : g_objs)
            if (o.type == OT::Arrow && IsSel(o.id)) o.headMode = v;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        break;
    }
    case IDM_DASH_SOLID: case IDM_DASH_DASH: case IDM_DASH_DOT: {
        int v = (id == IDM_DASH_SOLID) ? 0 : ((id == IDM_DASH_DASH) ? 1 : 2);
        SnapUndo();
        for (Obj& o : g_objs)
            if (o.type != OT::Image && IsSel(o.id)) o.dashMode = v;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        break;
    }
    case IDM_IMG_ORIG:
        if (g_sels.size() == 1 && prim->type == OT::Image && prim->bmp) {
            SnapUndo();
            prim->w = (double)prim->bmp->GetWidth();
            prim->h = (double)prim->bmp->GetHeight();
            InvalidateRect(g_hwnd, nullptr, FALSE);
        }
        break;
    case IDM_DELOBJ: DeleteSelected(); break;
    case IDM_HOME:   GoHome(); break;
    case IDM_FIT2:   ZoomFit(g_hwnd); break;
    case IDM_GRIDM:  g_grid = !g_grid; RefreshButtons(); InvalidateRect(g_hwnd, nullptr, FALSE); break;
    }
}
