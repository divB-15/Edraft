// ============================================================================
//  main.cpp  ——  程序入口、全局状态定义、主窗口消息处理
//
//  交互总览：
//    左键点对象       选中（Ctrl+点 = 加减选）
//    空白左键拖动     框选（或平移，取决于功能区「框选/平移」开关）
//    Ctrl+空白拖动    强制框选加选
//    空格+拖动 / 中键 平移画布        滚轮 缩放
//    双击文本框       进入编辑        Enter / F2 也能进入
//    右键             当前对象的设置菜单
// ============================================================================
#include "edraft.h"

#include <algorithm>
#include <cmath>

// ============================ 全局状态定义 ============================
vector<Obj>         g_objs;
int                 g_nextId = 1;
double              g_scale = 1.0, g_panX = 40.0, g_panY = 76.0;
Tool                g_tool = Tool::Select;
bool                g_grid = true;
int                 g_emptyMode = 0;          // 0 = 空白拖动框选，1 = 空白拖动平移

vector<int>         g_sels;

double              g_dpi = 96.0, g_dpiScale = 1.0;
int                 g_bandH = 28, g_ribH = 58, g_tbH = 86;   // 顶栏 / 功能区 / 合计
HFONT               g_uiFont = nullptr;

Drag                g_drag = Drag::None;
int                 g_handle = -1;
double              g_sx0 = 0, g_sy0 = 0;
double              g_wx0 = 0, g_wy0 = 0, g_wx = 0, g_wy = 0;
double              g_mwx = 0, g_mwy = 0;
bool                g_mouseIn = false;
double              g_ox = 0, g_oy = 0, g_ow = 0, g_oh = 0;
double              g_obend = 0.5;
bool                g_epMoved = false, g_space = false;
int                 g_arrowFrom = -1, g_arrowFromSide = -1;
vector<MoveOrig>    g_moveOrig;

HWND                g_hwnd = nullptr, g_edit = nullptr;
int                 g_editingId = -1;
HINSTANCE           g_inst = nullptr;

wstring             g_file;
bool                g_dirty = false;
vector<vector<Obj>> g_undo, g_redo;
ULONG_PTR           g_gdiToken = 0;

int                 g_gx0[NGROUP], g_gx1[NGROUP];

// ============================ 自绘工具栏按钮 ============================
// 标准 Win32 按钮改不了背景色，要按分组配色只能用 BS_OWNERDRAW 自己画。
// 三种状态：普通（组色）/ 选中（组色压暗 + 白色内描边）/ 禁用（灰底灰字）
static LRESULT OnDrawItem(WPARAM /*wp*/, LPARAM lp) {
    LPDRAWITEMSTRUCT ds = (LPDRAWITEMSTRUCT)lp;
    if (!ds || ds->CtlType != ODT_BUTTON) return 0;
    const BtnDef* bd = FindBtn((int)ds->CtlID);
    if (!bd) return 0;

    HDC hdc = ds->hDC;
    RECT rc = ds->rcItem;
    int rgb = GROUPS[bd->grp].rgb;
    BYTE r = (BYTE)((rgb >> 16) & 255), gg = (BYTE)((rgb >> 8) & 255), b = (BYTE)(rgb & 255);
    bool dis = (ds->itemState & ODS_DISABLED) != 0;
    bool chk = IsBtnChecked((int)ds->CtlID) && !dis;

    COLORREF fill;
    if (dis)       fill = RGB(214, 217, 222);
    else if (chk)  fill = RGB((BYTE)(r * 0.58), (BYTE)(gg * 0.58), (BYTE)(b * 0.58));
    else           fill = RGB(r, gg, b);
    HBRUSH br = CreateSolidBrush(fill);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    if (chk) {                                          // 选中态：白色内描边
        HPEN p = CreatePen(PS_INSIDEFRAME, 2, RGB(255, 255, 255));
        HGDIOBJ ob = SelectObject(hdc, p);
        HGDIOBJ ob2 = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1);
        SelectObject(hdc, ob2);
        SelectObject(hdc, ob);
        DeleteObject(p);
    }
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, dis ? RGB(142, 146, 152) : RGB(255, 255, 255));
    HGDIOBJ of = SelectObject(hdc, g_uiFont ? g_uiFont : GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(hdc, bd->cap, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, of);
    return TRUE;
}

// ============================ 拖拽中的尺寸计算 ============================
// 根据手柄编号算出新的包围盒。
// 图片的四个角是等比缩放（对角锁定）；其余四角自由；四边中点只改单轴。
static void ApplyResize(Obj* o) {
    double dx = g_wx - g_wx0, dy = g_wy - g_wy0;
    double nx = g_ox, ny = g_oy, nw = g_ow, nh = g_oh;

    bool lockAspect = (o->type == OT::Image) && g_handle < 4;
    if (lockAspect) {
        double ar = (g_oh > 1e-6) ? (g_ow / g_oh) : 1.0;
        double newW = (g_handle == H_TL || g_handle == H_BL) ? (g_ow - dx) : (g_ow + dx);
        if (newW < MIN_SIZE) newW = MIN_SIZE;
        double newH = newW / ar;
        if (newH < MIN_SIZE) { newH = MIN_SIZE; newW = newH * ar; }
        nw = newW; nh = newH;
        switch (g_handle) {                             // 对角保持不动
        case H_TL: nx = g_ox + g_ow - nw; ny = g_oy + g_oh - nh; break;
        case H_TR: ny = g_oy + g_oh - nh; break;
        case H_BR: break;
        case H_BL: nx = g_ox + g_ow - nw; break;
        }
    } else if (g_handle < 4) {                          // 四角：自由缩放
        switch (g_handle) {
        case H_TL: nx = g_ox + dx; ny = g_oy + dy; nw = g_ow - dx; nh = g_oh - dy; break;
        case H_TR: ny = g_oy + dy; nw = g_ow + dx; nh = g_oh - dy; break;
        case H_BR: nw = g_ow + dx; nh = g_oh + dy; break;
        case H_BL: nx = g_ox + dx; nw = g_ow - dx; nh = g_oh + dy; break;
        }
    } else {                                            // 四边中点：单轴
        switch (g_handle) {
        case H_TOP:    ny = g_oy + dy; nh = g_oh - dy; break;
        case H_RIGHT:  nw = g_ow + dx; break;
        case H_BOTTOM: nh = g_oh + dy; break;
        case H_LEFT:   nx = g_ox + dx; nw = g_ow - dx; break;
        }
    }
    if (nw < MIN_SIZE) nw = MIN_SIZE;
    if (nh < MIN_SIZE) nh = MIN_SIZE;
    o->x = nx; o->y = ny; o->w = nw; o->h = nh;
}

// 折线折点：按光标位置反推中间段的位置比例 bend（允许拖出线段之外一点）
static void ApplyBend(Obj* o) {
    Route r = ArrowRoute(*o);
    double nb = 0.5;
    if (r.bendIsX) {
        double span = r.bx - r.ax;
        nb = (fabs(span) < 1e-6) ? 0.5 : (g_wx - r.ax) / span;
    } else {
        double span = r.by - r.ay;
        nb = (fabs(span) < 1e-6) ? 0.5 : (g_wy - r.ay) / span;
    }
    o->bend = std::max(-0.5, std::min(1.5, nb));
}

// ---------------------------------------------------------------- 新建对象
static void CreateBox(Drag d) {
    double x0 = std::min(g_wx0, g_wx), y0 = std::min(g_wy0, g_wy);
    double w = fabs(g_wx - g_wx0), h = fabs(g_wy - g_wy0);
    if (w < 12 || h < 12) {                             // 拖太小或只是点一下 → 用默认尺寸
        w = (d == Drag::NewRect) ? 300 : 360;
        h = (d == Drag::NewRect) ? 200 : 108;
    }
    Obj o;
    o.type = (d == Drag::NewRect) ? OT::Rect : OT::Text;
    o.colorRGB = (o.type == OT::Rect) ? 0x4A82B4 : 0x1A1A1A;
    o.strokeW = DefStroke(o.type);
    o.x = x0; o.y = y0; o.w = w; o.h = h;
    SnapUndo();
    o.id = g_nextId++;
    g_objs.push_back(o);
    SetSel(o.id);
    if (o.type == OT::Text) BeginEdit(o.id);
    MarkTool(Tool::Select);
}
static void CreateArrow() {
    int toId = -1, toSide = -1;
    ResolveBind(g_wx, g_wy, toId, toSide);
    if (toId >= 0 && toId == g_arrowFrom) { toId = -1; toSide = -1; }   // 禁止自连

    double dx = g_wx - g_wx0, dy = g_wy - g_wy0;
    if (std::sqrt(dx * dx + dy * dy) <= 8.0 / g_scale) return;          // 距离太短视为误操作

    Obj o;
    o.type = OT::Arrow;
    o.fromId = g_arrowFrom; o.fromSide = g_arrowFromSide;
    o.toId = toId; o.toSide = toSide;
    o.colorRGB = 0x282828;
    o.strokeW = DefStroke(OT::Arrow);
    o.x = g_wx0; o.y = g_wy0; o.x2 = g_wx; o.y2 = g_wy;
    SnapUndo();
    o.id = g_nextId++;
    g_objs.push_back(o);
    SetSel(o.id);
    MarkTool(Tool::Select);
}
// 框选结束：包围盒与选框相交即选中（比较宽容）。
// 特例：方框是"容器"，若选框完全落在它内部，说明用户只想选里面的内容，
// 此时不选中方框本身 —— 只有选框越过（或整个框住）它的边缘才算选中。
static void FinishMarquee() {
    double dx = g_wx - g_wx0, dy = g_wy - g_wy0;
    if (fabs(dx) * g_scale <= 4 && fabs(dy) * g_scale <= 4) return;     // 只是点了一下

    double x0 = std::min(g_wx0, g_wx), x1 = std::max(g_wx0, g_wx);
    double y0 = std::min(g_wy0, g_wy), y1 = std::max(g_wy0, g_wy);
    bool add = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    double m = 4.0 / g_scale;               // 判断"完全落在内部"时留的容差（世界坐标）

    for (const Obj& o : g_objs) {
        double a, b, c, e;
        ObjBounds(o, a, b, c, e);
        bool overlap = !(c < x0 || a > x1 || e < y0 || b > y1);
        if (!overlap) continue;

        if (o.type == OT::Rect) {
            bool fullyInside = (x0 >= a + m && x1 <= c - m && y0 >= b + m && y1 <= e - m);
            if (fullyInside) continue;      // 选框在方框肚子里 → 不选中方框
        }
        if (add) AddSel(o.id);
        else if (!IsSel(o.id)) g_sels.push_back(o.id);
    }
}

// ============================ 主窗口过程 ============================
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        MakeToolbar(hwnd);
        RefreshButtons();
        UpdateTitle();
        UpdateUndoButtons();
        return 0;

    case WM_DRAWITEM:
        return OnDrawItem(wp, lp);

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (HIWORD(wp) == EN_KILLFOCUS && id == IDC_EDIT) { CommitEdit(); return 0; }
        if (HIWORD(wp) == 0 && id >= 2000) { HandleMenu(id); return 0; }   // 菜单命令
        if (HIWORD(wp) != BN_CLICKED) break;

        CommitEdit();
        SetFocus(hwnd);        // 点过按钮后焦点要还给主窗口，否则 Ctrl+V 等快捷键收不到
        switch (id) {
        case IDB_SELECT:  MarkTool(Tool::Select); break;
        case IDB_PANMODE: g_emptyMode = 1; RefreshButtons(); break;
        case IDB_SELMODE: g_emptyMode = 0; RefreshButtons(); break;
        case IDB_RECT:    MarkTool(Tool::RectT); break;
        case IDB_TEXT:    MarkTool(Tool::TextT); break;
        case IDB_ARROW:   MarkTool(Tool::ArrowT); break;
        case IDB_IMAGE:   MarkTool(Tool::ImageT); InsertImage(hwnd); MarkTool(Tool::Select); break;
        case IDB_GRID:    g_grid = !g_grid; RefreshButtons(); InvalidateRect(hwnd, nullptr, FALSE); break;
        case IDB_OPEN:  { wstring p; if (PickFile(hwnd, p, false, FILE_FLT, FILE_EXT)) LoadDoc(p); break; }
        case IDB_SAVE:
            if (g_file.empty()) { wstring p; if (PickFile(hwnd, p, true, FILE_FLT, FILE_EXT)) SaveDoc(p); }
            else SaveDoc(g_file);
            break;
        case IDB_SAVEAS: { wstring p = g_file; if (PickFile(hwnd, p, true, FILE_FLT, FILE_EXT)) SaveDoc(p); break; }
        case IDB_EXPORT: ExportPNG(hwnd); break;
        case IDB_DEL:    DeleteSelected(); break;
        case IDB_UNDO:   DoUndo(); break;
        case IDB_REDO:   DoRedo(); break;
        case IDB_FIT:    ZoomFit(hwnd); break;
        case IDB_HOME:   GoHome(); break;
        }
        return 0;
    }

    // ---------------------------------------------------------- 鼠标按下
    case WM_LBUTTONDOWN: {
        CommitEdit();
        SetFocus(hwnd);
        int sx = GET_X_LPARAM(lp), sy = GET_Y_LPARAM(lp);
        if (sy < g_tbH) return 0;
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        g_sx0 = sx; g_sy0 = sy;
        g_wx0 = S2X(sx); g_wy0 = S2Y(sy);
        g_wx = g_wx0; g_wy = g_wy0;

        if (g_space) { g_drag = Drag::Pan; SetCapture(hwnd); return 0; }

        if (g_tool == Tool::RectT) { g_drag = Drag::NewRect;  SetCapture(hwnd); return 0; }
        if (g_tool == Tool::TextT) { g_drag = Drag::NewText;  SetCapture(hwnd); return 0; }
        if (g_tool == Tool::ArrowT) {
            g_drag = Drag::NewArrow;
            ResolveBind(g_wx0, g_wy0, g_arrowFrom, g_arrowFromSide);
            if (g_arrowFrom >= 0)                       // 起点吸附到锚点
                if (Obj* o = ObjOf(g_arrowFrom)) SidePoint(*o, g_arrowFromSide, g_wx0, g_wy0);
            SetCapture(hwnd);
            return 0;
        }
        // 框选工具：先看有没有命中手柄
        int h = HitHandle(sx, sy);
        if (h == H_MOVE) {
            // 方框的移动手柄：等同于"抓住方框本身"，直接整体移动
            g_drag = Drag::Move;
            g_moveOrig.clear();
            for (int sid : g_sels)
                if (Obj* o = ObjOf(sid)) g_moveOrig.push_back({ sid, o->x, o->y, o->x2, o->y2 });
            SnapUndo();
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (h >= 0) {
            if (Obj* o = ObjOf(PrimaryId())) {
                g_handle = h;
                g_drag = Drag::Resize;
                g_epMoved = false;
                g_ox = o->x; g_oy = o->y; g_ow = o->w; g_oh = o->h;
                g_obend = o->bend;
                SnapUndo();
                SetCapture(hwnd);
            }
            return 0;
        }
        int hit = HitTest(g_wx0, g_wy0);
        if (hit >= 0) {                                 // 命中对象 → 整体移动
            if (ctrl) ToggleSel(hit);
            else if (!IsSel(hit)) SetSel(hit);
            g_drag = Drag::Move;
            g_moveOrig.clear();
            for (int sid : g_sels)
                if (Obj* o = ObjOf(sid)) g_moveOrig.push_back({ sid, o->x, o->y, o->x2, o->y2 });
            SnapUndo();
        } else {                                        // 空白：Ctrl 强制框选，否则按缺省设置
            if (ctrl) g_drag = Drag::Marquee;
            else { g_sels.clear(); g_drag = (g_emptyMode == 1) ? Drag::Pan : Drag::Marquee; }
        }
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MBUTTONDOWN:
        CommitEdit();
        g_sx0 = GET_X_LPARAM(lp); g_sy0 = GET_Y_LPARAM(lp);
        g_drag = Drag::Pan;
        SetCapture(hwnd);
        return 0;
    case WM_RBUTTONUP:
        ShowContextMenu(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    // ---------------------------------------------------------- 鼠标移动
    case WM_MOUSEMOVE: {
        int sx = GET_X_LPARAM(lp), sy = GET_Y_LPARAM(lp);
        g_mwx = S2X(sx); g_mwy = S2Y(sy);
        g_mouseIn = (sy >= g_tbH);

        if (g_drag == Drag::None) {
            if (ShowAnchors()) InvalidateRect(hwnd, nullptr, FALSE);   // 锚点悬停高亮
            return 0;
        }
        if (g_drag == Drag::Pan) {
            g_panX += sx - g_sx0;
            g_panY += sy - g_sy0;
            g_sx0 = sx; g_sy0 = sy;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        g_wx = S2X(sx); g_wy = S2Y(sy);
        if (g_drag == Drag::Marquee) { InvalidateRect(hwnd, nullptr, FALSE); return 0; }

        if (g_drag == Drag::Move) {
            double dx = g_wx - g_wx0, dy = g_wy - g_wy0;
            for (const MoveOrig& mo : g_moveOrig) {     // 选中几个就一起移动几个
                if (Obj* o = ObjOf(mo.id)) {
                    o->x = mo.x + dx; o->y = mo.y + dy;
                    if (o->type == OT::Arrow) { o->x2 = mo.x2 + dx; o->y2 = mo.y2 + dy; }
                }
            }
        } else if (g_drag == Drag::Resize) {
            Obj* o = ObjOf(PrimaryId());
            if (!o) return 0;
            if (g_handle == H_BEND) {
                ApplyBend(o);
            } else if (g_handle == H_HEAD || g_handle == H_TAIL) {
                // 拖端点即解绑，跟随光标；松手时再重新吸附
                if (!g_epMoved) {
                    g_epMoved = true;
                    if (g_handle == H_HEAD) { o->fromId = -1; o->fromSide = -1; }
                    else { o->toId = -1; o->toSide = -1; }
                }
                if (g_handle == H_HEAD) { o->x = g_wx; o->y = g_wy; }
                else { o->x2 = g_wx; o->y2 = g_wy; }
            } else {
                ApplyResize(o);
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    // ---------------------------------------------------------- 鼠标松开
    case WM_LBUTTONUP:
    case WM_MBUTTONUP: {
        if (g_drag == Drag::None) return 0;
        ReleaseCapture();
        Drag d = g_drag;
        g_drag = Drag::None;
        int sx = GET_X_LPARAM(lp), sy = GET_Y_LPARAM(lp);
        g_wx = S2X(sx); g_wy = S2Y(sy);

        if (d == Drag::Marquee)     { FinishMarquee(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        if (d == Drag::Pan)         return 0;
        if (d == Drag::NewRect || d == Drag::NewText) { CreateBox(d); }
        else if (d == Drag::NewArrow)                 { CreateArrow(); }
        else if (d == Drag::Resize && (g_handle == H_HEAD || g_handle == H_TAIL)) {
            if (g_epMoved) {                           // 端点松手：重新判断吸附
                if (Obj* o = ObjOf(PrimaryId())) {
                    int bid = -1, bs = -1;
                    ResolveBind(g_wx, g_wy, bid, bs);
                    if (IsSel(bid)) { bid = -1; bs = -1; }
                    if (g_handle == H_HEAD) { o->fromId = bid; o->fromSide = bs; }
                    else { o->toId = bid; o->toSide = bs; }
                }
            } else if (!g_undo.empty()) { g_undo.pop_back(); UpdateUndoButtons(); }
        } else if (d == Drag::Resize && g_handle == H_BEND) {
            if (Obj* o = ObjOf(PrimaryId()); o && !g_undo.empty() && o->bend == g_obend) {
                g_undo.pop_back(); UpdateUndoButtons();          // 没真的动 → 撤销这次快照
            }
        } else if (d == Drag::Resize) {
            if (Obj* o = ObjOf(PrimaryId()); o && !g_undo.empty() &&
                o->x == g_ox && o->y == g_oy && o->w == g_ow && o->h == g_oh) {
                g_undo.pop_back(); UpdateUndoButtons();
            }
        } else if (d == Drag::Move) {
            bool same = true;
            for (const MoveOrig& mo : g_moveOrig)
                if (Obj* o = ObjOf(mo.id); o && (o->x != mo.x || o->y != mo.y)) { same = false; break; }
            if (same && !g_undo.empty()) { g_undo.pop_back(); UpdateUndoButtons(); }
        }
        g_handle = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        int sx = GET_X_LPARAM(lp), sy = GET_Y_LPARAM(lp);
        if (sy < g_tbH) return 0;
        int hit = HitTest(S2X(sx), S2Y(sy));
        if (hit >= 0) {
            SetSel(hit);
            if (Obj* o = ObjOf(hit); o && o->type == OT::Text) BeginEdit(hit);
            else InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    // ---------------------------------------------------------- 滚轮缩放
    case WM_MOUSEWHEEL: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        double wx = S2X(pt.x), wy = S2Y(pt.y);          // 以光标为锚点缩放
        double k = (GET_WHEEL_DELTA_WPARAM(wp) > 0) ? 1.12 : (1.0 / 1.12);
        g_scale = std::max(0.05, std::min(8.0, g_scale * k));
        g_panX = pt.x - wx * g_scale;
        g_panY = pt.y - wy * g_scale;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    // ---------------------------------------------------------- 键盘
    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (wp == VK_SPACE) { g_space = true; return 0; }          // 空格 = 临时平移
        if (ctrl && wp == 'Z') { DoUndo(); return 0; }
        if (ctrl && wp == 'Y') { DoRedo(); return 0; }
        if (ctrl && wp == 'V') { DoPaste(); return 0; }
        if (ctrl && wp == 'S') {
            SendMessageW(hwnd, WM_COMMAND,
                         MAKEWPARAM((GetKeyState(VK_SHIFT) & 0x8000) ? IDB_SAVEAS : IDB_SAVE, BN_CLICKED), 0);
            return 0;
        }
        if (ctrl && wp == 'O') { SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDB_OPEN, BN_CLICKED), 0); return 0; }
        if (ctrl && wp == 'A') {                                    // 全选
            g_sels.clear();
            for (const Obj& o : g_objs) g_sels.push_back(o.id);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wp == VK_RETURN || wp == VK_F2) {
            if (g_sels.size() == 1)
                if (Obj* o = ObjOf(PrimaryId()); o && o->type == OT::Text) { CommitEdit(); BeginEdit(PrimaryId()); }
            return 0;
        }
        if (wp == VK_DELETE) { DeleteSelected(); return 0; }
        if (wp == VK_ESCAPE) { CommitEdit(); g_sels.clear(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        return 0;
    }
    case WM_KEYUP:
        if (wp == VK_SPACE) g_space = false;
        return 0;

    // ---------------------------------------------------------- 绘制
    case WM_ERASEBKGND:
        return 1;                                       // 双缓冲绘制，禁止擦背景以免闪烁
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right - rc.left, H = rc.bottom - rc.top;
        if (W > 0 && H > 0) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bm = CreateCompatibleBitmap(hdc, W, H);
            HGDIOBJ old = SelectObject(mem, bm);
            {
                Gdiplus::Graphics g(mem);
                Gdiplus::SolidBrush bg(Gdiplus::Color(255, 246, 247, 249));
                g.FillRectangle(&bg, Gdiplus::RectF(0, 0, (Gdiplus::REAL)W, (Gdiplus::REAL)H));

                if (H > g_tbH) {                        // 画布区裁剪掉工具栏
                    Gdiplus::RectF clip(0, (Gdiplus::REAL)g_tbH, (Gdiplus::REAL)W, (Gdiplus::REAL)(H - g_tbH));
                    g.SetClip(clip);
                }
                DrawGrid(g, W, H);
                DrawScene(g);
                DrawOverlay(g);
                g.ResetClip();
                DrawChrome(g, W);                       // 工具栏画在画布之上
            }
            BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bm);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    // ---------------------------------------------------------- 关闭
    case WM_CLOSE: {
        CommitEdit();
        if (g_dirty && MessageBoxW(hwnd, L"当前内容未保存，是否退出？", APP_NAME,
                                   MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================ 程序入口 ============================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    g_inst = hInst;

    // DPI 感知：不声明的话高分屏上整个窗口会被系统位图拉伸，文字发虚
    SetProcessDPIAware();
    {
        HDC sdc = GetDC(nullptr);
        int dpiY = GetDeviceCaps(sdc, LOGPIXELSY);
        ReleaseDC(nullptr, sdc);
        if (dpiY < 48) dpiY = 96;
        g_dpi = (double)dpiY;
        g_dpiScale = g_dpi / 96.0;
        g_bandH = (int)ceil(28.0 * g_dpiScale);
        g_ribH  = (int)ceil(58.0 * g_dpiScale);
        g_tbH   = g_bandH + g_ribH;
    }

    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&g_gdiToken, &gsi, nullptr);
    g_uiFont = CreateFontW(-(int)lround(13.0 * g_dpiScale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, FONT_NAME);

    // 从 exe 资源里取图标（ID 1，由 app.rc 经 windres 编进 app_res.o）。
    // 必须同时设 hIcon 与 hIconSm：只嵌资源不设类图标的话，
    // 资源管理器里能看到图标，但窗口标题栏/任务栏仍是默认图标。
    HICON hIconBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                       LR_DEFAULTCOLOR);
    HICON hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                      LR_DEFAULTCOLOR);

    const wchar_t* CLS = L"EdraftMainWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;    // 没有 CS_DBLCLKS 就收不到双击消息
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = hIconBig ? hIconBig : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = hIconSm;
    RegisterClassExW(&wc);

    WNDCLASSEXW ic{};                                   // 自定义数值输入框的窗口类
    ic.cbSize = sizeof(ic);
    ic.lpfnWndProc = InputProc;
    ic.hInstance = hInst;
    ic.lpszClassName = L"EdraftInputBox";
    ic.hCursor = LoadCursor(nullptr, IDC_ARROW);
    ic.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    ic.hIcon = wc.hIcon;
    ic.hIconSm = wc.hIconSm;
    RegisterClassExW(&ic);

    // WS_CLIPCHILDREN：父窗口重绘时不覆盖工具栏按钮等子窗口，避免拖动时工具栏闪烁
    g_hwnd = CreateWindowExW(0, CLS, APP_NAME,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             (int)(1240 * g_dpiScale), (int)(820 * g_dpiScale),
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    // 文本框就地编辑用的 EDIT 子窗口，平时隐藏
    g_edit = CreateWindowW(L"EDIT", L"",
                           WS_CHILD | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                           0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_EDIT, hInst, nullptr);
    ShowWindow(g_edit, SW_HIDE);

    g_panY = g_tbH + 40.0;
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_uiFont) DeleteObject(g_uiFont);
    Gdiplus::GdiplusShutdown(g_gdiToken);
    return (int)msg.wParam;
}
