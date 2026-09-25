// ============================================================================
//  model.cpp  ——  数据模型与几何计算
//
//  负责：对象查找、连接锚点、箭头路由（直线/折线）、包围盒、命中检测、
//        多选集合操作、撤销重做、视图（回到原点 / 适应窗口）。
//  这一层不含任何 GDI+ 绘制代码，也不直接操作窗口，便于单独理解与修改。
// ============================================================================
#include "edraft.h"

#include <algorithm>
#include <cmath>

// ------------------------------------------------------------ 选中集合的操作
bool IsSel(int id) {
    for (int s : g_sels) if (s == id) return true;
    return false;
}
int PrimaryId() {
    return g_sels.empty() ? -1 : g_sels.back();
}
void SetSel(int id) {
    g_sels.clear();
    if (id >= 0) g_sels.push_back(id);
}
void AddSel(int id) {
    if (id >= 0 && !IsSel(id)) g_sels.push_back(id);
}
void ToggleSel(int id) {
    if (id < 0) return;
    if (IsSel(id)) g_sels.erase(std::remove(g_sels.begin(), g_sels.end(), id), g_sels.end());
    else g_sels.push_back(id);
}

// ------------------------------------------------------------------ 对象查找
int IndexOf(int id) {
    for (size_t i = 0; i < g_objs.size(); ++i)
        if (g_objs[i].id == id) return (int)i;
    return -1;
}
Obj* ObjOf(int id) {
    int i = IndexOf(id);
    return i < 0 ? nullptr : &g_objs[i];
}
// 可作为箭头连接目标的类型（箭头本身不能被连接）
bool IsBox(OT t) { return t == OT::Rect || t == OT::Text || t == OT::Image; }

const wchar_t* TypeName(OT t) {
    switch (t) {
    case OT::Rect:  return L"方框";
    case OT::Text:  return L"文本框";
    case OT::Image: return L"图片";
    default:        return L"箭头";
    }
}

// 由原始字节解出位图。
// 注意：GDI+ 对某些格式是延迟解码的，IStream 必须活得比 Bitmap 久，
// 所以这里把流和位图一起存进 Obj（用 shared_ptr 管理生命周期）。
bool MakeBmp(Obj& o) {
    o.bmp.reset();
    o.stream.reset();
    if (!o.imgBytes || o.imgBytes->empty()) return false;

    IStream* p = SHCreateMemStream(o.imgBytes->data(), (UINT)o.imgBytes->size());
    if (!p) return false;
    o.stream = std::shared_ptr<IStream>(p, [](IStream* s) { if (s) s->Release(); });

    Gdiplus::Bitmap* b = new Gdiplus::Bitmap(p);
    if (!b || b->GetLastStatus() != Gdiplus::Ok) { delete b; return false; }
    o.bmp = std::shared_ptr<Gdiplus::Bitmap>(b);
    return true;
}

// ------------------------------------------------------------------ 连接锚点
// 每个对象四条边的中点各有一个连接锚点：0左 1上 2右 3下
void SidePoint(const Obj& b, int side, double& ax, double& ay) {
    switch (side) {
    case 0:  ax = b.x;             ay = b.y + b.h / 2.0; break;   // 左
    case 1:  ax = b.x + b.w / 2.0; ay = b.y;             break;   // 上
    case 2:  ax = b.x + b.w;       ay = b.y + b.h / 2.0; break;   // 右
    default: ax = b.x + b.w / 2.0; ay = b.y + b.h;       break;   // 下
    }
}
int NearestSide(const Obj& b, double wx, double wy) {
    int best = 0; double bd = 1e18;
    for (int s = 0; s < 4; ++s) {
        double ax, ay; SidePoint(b, s, ax, ay);
        double d = (wx - ax) * (wx - ax) + (wy - ay) * (wy - ay);
        if (d < bd) { bd = d; best = s; }
    }
    return best;
}
// 光标是否落在某个对象的锚点上（命中半径 9 屏幕像素，换算到世界坐标）
bool FindAnchor(double wx, double wy, int& oid, int& side) {
    double tol = 9.0 / g_scale;
    for (size_t i = g_objs.size(); i-- > 0;) {          // 从上层往下找，优先命中上层
        const Obj& o = g_objs[i];
        if (!IsBox(o.type)) continue;
        for (int s = 0; s < 4; ++s) {
            double ax, ay; SidePoint(o, s, ax, ay);
            double dx = wx - ax, dy = wy - ay;
            if (dx * dx + dy * dy <= tol * tol) { oid = o.id; side = s; return true; }
        }
    }
    return false;
}

// ------------------------------------------------------------------ 箭头路由
// 解析箭头两端实际坐标：绑定端取目标对象对应边的锚点，自由端用自身存储的坐标
Ends ArrowEnds(const Obj& a) {
    Ends e{ a.x, a.y, a.x2, a.y2 };
    if (a.fromId >= 0) {
        if (Obj* b = ObjOf(a.fromId); b && IsBox(b->type)) {
            int s = (a.fromSide >= 0) ? a.fromSide : NearestSide(*b, e.x2, e.y2);
            SidePoint(*b, s, e.x1, e.y1);
        }
    }
    if (a.toId >= 0) {
        if (Obj* b = ObjOf(a.toId); b && IsBox(b->type)) {
            int s = (a.toSide >= 0) ? a.toSide : NearestSide(*b, e.x1, e.y1);
            SidePoint(*b, s, e.x2, e.y2);
        }
    }
    return e;
}

// 计算折线的路由信息。
// 思路：先从起点沿"出边法向"外伸 gap，再走正交折线，最后沿"入边法向"进入终点，
// 保证全程只有水平/垂直线段，不会出现斜线。
//   · 起止法向都水平 → S 形（中间一段竖直，位置由 bend 控制）
//   · 起止法向都垂直 → Z 形（中间一段水平，位置由 bend 控制）
//   · 一横一竖       → 只有一个直角拐点，无需 bend（形状唯一确定）
Route ArrowRoute(const Obj& a) {
    Route r;
    Ends e = ArrowEnds(a);
    const double gap = 18.0;                 // 端点与对象之间留出的空隙

    // 起点出边法向：优先用绑定的边，否则按两点相对方向推断
    double n1x = 0, n1y = 0, n2x = 0, n2y = 0;
    if (a.fromSide >= 0) {
        switch (a.fromSide) {
        case 0: n1x = -1; break; case 1: n1y = -1; break;
        case 2: n1x = 1;  break; default: n1y = 1;  break;
        }
    } else {
        double dx = e.x2 - e.x1, dy = e.y2 - e.y1;
        if (fabs(dx) >= fabs(dy)) n1x = (dx > 0 ? 1 : -1); else n1y = (dy > 0 ? 1 : -1);
    }
    if (a.toSide >= 0) {
        switch (a.toSide) {
        case 0: n2x = -1; break; case 1: n2y = -1; break;
        case 2: n2x = 1;  break; default: n2y = 1;  break;
        }
    } else {
        double dx = e.x1 - e.x2, dy = e.y1 - e.y2;
        if (fabs(dx) >= fabs(dy)) n2x = (dx > 0 ? 1 : -1); else n2y = (dy > 0 ? 1 : -1);
    }

    r.ax = e.x1 + n1x * gap; r.ay = e.y1 + n1y * gap;    // 出点
    r.bx = e.x2 + n2x * gap; r.by = e.y2 + n2y * gap;    // 入点
    r.n1h = (n1x != 0); r.n2h = (n2x != 0);

    if (r.n1h && r.n2h) {                     // S 形：中间竖直段
        double mx = r.ax + (r.bx - r.ax) * a.bend;
        r.hasBend = true; r.bendIsX = true;
        r.midX = mx; r.midY = (r.ay + r.by) / 2.0;
    } else if (!r.n1h && !r.n2h) {            // Z 形：中间水平段
        double my = r.ay + (r.by - r.ay) * a.bend;
        r.hasBend = true; r.bendIsX = false;
        r.midX = (r.ax + r.bx) / 2.0; r.midY = my;
    }
    return r;
}

// 箭头的所有折线顶点（世界坐标）。直线只有 2 点，折线有 4~6 点。
vector<Gdiplus::PointF> ArrowPath(const Obj& a) {
    Ends e = ArrowEnds(a);
    vector<Gdiplus::PointF> p;
    p.push_back(PF(e.x1, e.y1));

    if (a.lineMode == 1) {                    // 折线
        Route r = ArrowRoute(a);
        p.push_back(PF(r.ax, r.ay));          // 出点
        if (r.n1h && r.n2h) {                 // S 形
            double mx = r.ax + (r.bx - r.ax) * a.bend;
            p.push_back(PF(mx, r.ay));
            p.push_back(PF(mx, r.by));
        } else if (r.n1h && !r.n2h) {         // 先横后纵，单拐点
            p.push_back(PF(r.bx, r.ay));
        } else if (!r.n1h && !r.n2h) {        // Z 形
            double my = r.ay + (r.by - r.ay) * a.bend;
            p.push_back(PF(r.ax, my));
            p.push_back(PF(r.bx, my));
        } else {                              // 先纵后横，单拐点
            p.push_back(PF(r.ax, r.by));
        }
        p.push_back(PF(r.bx, r.by));          // 入点
    }
    p.push_back(PF(e.x2, e.y2));
    return p;
}

// -------------------------------------------------------------------- 包围盒
void ObjBounds(const Obj& o, double& x0, double& y0, double& x1, double& y1) {
    if (o.type == OT::Arrow) {                // 箭头要覆盖整条折线
        x0 = y0 = 1e18; x1 = y1 = -1e18;
        for (const auto& pt : ArrowPath(o)) {
            x0 = std::min(x0, (double)pt.X); y0 = std::min(y0, (double)pt.Y);
            x1 = std::max(x1, (double)pt.X); y1 = std::max(y1, (double)pt.Y);
        }
    } else {
        x0 = o.x; y0 = o.y; x1 = o.x + o.w; y1 = o.y + o.h;
    }
}
void ContentBounds(double& x0, double& y0, double& x1, double& y1) {
    x0 = y0 = 1e18; x1 = y1 = -1e18;
    for (const Obj& o : g_objs) {
        double a, b, c, d;
        ObjBounds(o, a, b, c, d);
        x0 = std::min(x0, a); y0 = std::min(y0, b);
        x1 = std::max(x1, c); y1 = std::max(y1, d);
    }
    if (x0 > x1) { x0 = y0 = 0; x1 = y1 = 100; }   // 空画布兜底
}

// ------------------------------------------------------------------- 命中检测
static bool InBox(const Obj& o, double wx, double wy, double tol) {
    return wx >= o.x - tol && wx <= o.x + o.w + tol &&
           wy >= o.y - tol && wy <= o.y + o.h + tol;
}
// 分组方框只在边框附近可点，这样框内的文本框仍能被正常选中
static bool NearBorder(const Obj& o, double wx, double wy, double tol) {
    return InBox(o, wx, wy, 0) && !InBox(o, wx, wy, -tol);
}
static double DistSeg(double px, double py, double ax, double ay, double bx, double by) {
    double vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    double L = vx * vx + vy * vy;
    double t = L > 0 ? (wx * vx + wy * vy) / L : 0;
    t = std::max(0.0, std::min(1.0, t));
    double dx = wx - t * vx, dy = wy - t * vy;
    return std::sqrt(dx * dx + dy * dy);
}

// 命中哪个对象。顺序 = 绘制层次的反序：先箭头，再文本框/图片，最后分组方框
int HitTest(double wx, double wy) {
    double tol = 6.0 / g_scale;
    for (size_t i = g_objs.size(); i-- > 0;) {
        const Obj& o = g_objs[i];
        if (o.type != OT::Arrow) continue;
        auto p = ArrowPath(o);
        for (size_t k = 1; k < p.size(); ++k)
            if (DistSeg(wx, wy, p[k - 1].X, p[k - 1].Y, p[k].X, p[k].Y) < tol) return o.id;
    }
    for (size_t i = g_objs.size(); i-- > 0;) {
        const Obj& o = g_objs[i];
        if ((o.type == OT::Text || o.type == OT::Image) && InBox(o, wx, wy, 0)) return o.id;
    }
    for (size_t i = g_objs.size(); i-- > 0;) {
        const Obj& o = g_objs[i];
        if (o.type == OT::Rect && NearBorder(o, wx, wy, tol)) return o.id;
    }
    return -1;
}

// 方框"整体移动"手柄：画在包围盒左上角的外侧，与四角缩放手柄错开
void RectMoveHandlePos(const Obj& o, double& sx, double& sy) {
    double off = 12.0 * std::max(1.0, g_dpiScale);
    sx = W2X(o.x) - off;
    sy = W2Y(o.y) - off;
}

// 命中哪个手柄（屏幕坐标）。手柄只在"恰好选中一个对象"时存在。
// 编号：0..3 四角，4..7 四边中点，8/9 箭头两端点，10 折线折点，11 方框移动手柄
int HitHandle(int sx, int sy) {
    if (g_sels.size() != 1) return -1;
    Obj* o = ObjOf(PrimaryId());
    if (!o) return -1;

    double R = 6.0 * std::max(1.0, g_dpiScale);
    auto isNear = [&](double px, double py) { return fabs(sx - px) <= R && fabs(sy - py) <= R; };

    if (o->type == OT::Arrow) {
        if (o->lineMode == 1) {                        // 折点手柄优先于端点
            Route r = ArrowRoute(*o);
            if (r.hasBend && isNear(W2X(r.midX), W2Y(r.midY))) return H_BEND;
        }
        Ends e = ArrowEnds(*o);
        if (isNear(W2X(e.x1), W2Y(e.y1))) return H_HEAD;
        if (isNear(W2X(e.x2), W2Y(e.y2))) return H_TAIL;
        return -1;
    }
    double L = W2X(o->x), T = W2Y(o->y);
    double W = o->w * g_scale, H = o->h * g_scale;

    // 方框的移动手柄优先判定（它就在左上角外侧，且用途与缩放完全不同）
    if (o->type == OT::Rect) {
        double mx, my;
        RectMoveHandlePos(*o, mx, my);
        if (isNear(mx, my)) return H_MOVE;
    }

    double cx = L + W / 2.0, cy = T + H / 2.0;
    double px[8] = { L, L + W, L + W, L,     cx, L + W, cx, L };
    double py[8] = { T, T,     T + H, T + H, T,  cy,    T + H, cy };
    for (int i = 0; i < 8; ++i) if (isNear(px[i], py[i])) return i;
    return -1;
}

// 解析箭头端点的吸附目标：优先落在锚点上，其次落在对象内部（取最近边），否则自由点
void ResolveBind(double wx, double wy, int& bid, int& bside) {
    int aid, aside;
    if (FindAnchor(wx, wy, aid, aside)) { bid = aid; bside = aside; return; }
    int h = HitTest(wx, wy);
    if (h >= 0) {
        if (Obj* b = ObjOf(h); b && IsBox(b->type)) {
            bid = h; bside = NearestSide(*b, wx, wy); return;
        }
    }
    bid = -1; bside = -1;
}

// 删除选中对象；同时删除两端中任一被删的箭头（否则箭头会指向不存在的对象）
void DeleteSelected() {
    if (g_sels.empty()) return;
    CommitEdit();
    SnapUndo();
    for (size_t i = 0; i < g_objs.size();) {
        bool kill = IsSel(g_objs[i].id);
        if (!kill && g_objs[i].type == OT::Arrow)
            kill = IsSel(g_objs[i].fromId) || IsSel(g_objs[i].toId);
        if (kill) g_objs.erase(g_objs.begin() + i);
        else ++i;
    }
    g_sels.clear();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------------ 撤销重做
// 采用"整表快照"：每次改动前把 g_objs 整份压栈。对象少、图片以 shared_ptr 共享，
// 代价很低，换来的是实现简单、绝不会漏掉某个字段。
static bool SameState(const vector<Obj>& a, const vector<Obj>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const Obj& p = a[i]; const Obj& q = b[i];
        if (p.id != q.id || p.type != q.type) return false;
        if (p.x != q.x || p.y != q.y || p.w != q.w || p.h != q.h) return false;
        if (p.x2 != q.x2 || p.y2 != q.y2) return false;
        if (p.text != q.text) return false;
        if (p.fromId != q.fromId || p.toId != q.toId) return false;
        if (p.fromSide != q.fromSide || p.toSide != q.toSide) return false;
        if (p.lineMode != q.lineMode || p.headMode != q.headMode || p.dashMode != q.dashMode) return false;
        if (p.bend != q.bend || p.colorRGB != q.colorRGB || p.strokeW != q.strokeW) return false;
        if (p.fontSize != q.fontSize || p.bold != q.bold) return false;
    }
    return true;
}
// 撤销/重做按钮按栈是否为空置灰，让"能不能点"直接可见
void UpdateUndoButtons() {
    if (!g_hwnd) return;
    HWND u = GetDlgItem(g_hwnd, IDB_UNDO);
    HWND r = GetDlgItem(g_hwnd, IDB_REDO);
    if (u) { EnableWindow(u, g_undo.empty() ? FALSE : TRUE); InvalidateRect(u, nullptr, FALSE); }
    if (r) { EnableWindow(r, g_redo.empty() ? FALSE : TRUE); InvalidateRect(r, nullptr, FALSE); }
}
void SnapUndo() {
    if (!g_undo.empty() && SameState(g_undo.back(), g_objs)) return;  // 状态没变就不记，避免"撤销了个寂寞"
    g_undo.push_back(g_objs);
    if (g_undo.size() > 60) g_undo.erase(g_undo.begin());
    g_redo.clear();                       // 产生新改动后，重做栈失效
    g_dirty = true;
    UpdateUndoButtons();
}
static void PruneSel() {
    for (size_t i = 0; i < g_sels.size();) {
        if (IndexOf(g_sels[i]) < 0) g_sels.erase(g_sels.begin() + i);
        else ++i;
    }
}
void DoUndo() {
    if (g_undo.empty()) return;
    CommitEdit();
    g_redo.push_back(g_objs);
    g_objs = g_undo.back();
    g_undo.pop_back();
    PruneSel();
    g_dirty = true;
    UpdateUndoButtons();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
void DoRedo() {
    if (g_redo.empty()) return;
    CommitEdit();
    g_undo.push_back(g_objs);
    g_objs = g_redo.back();
    g_redo.pop_back();
    PruneSel();
    g_dirty = true;
    UpdateUndoButtons();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------- 视图
// 回到原点：缩放归 1，把世界坐标 (0,0) 放在画布左上角附近
void GoHome() {
    g_scale = 1.0;
    g_panX = 40.0;
    g_panY = g_tbH + 40.0;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
// 缩放平移到刚好容纳全部内容
void ZoomFit(HWND hwnd) {
    if (g_objs.empty()) return;
    double x0, y0, x1, y1;
    ContentBounds(x0, y0, x1, y1);
    RECT rc; GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top - g_tbH;
    if (cw <= 0 || ch <= 0) return;
    double m = 40.0;
    g_scale = std::min((cw - 2 * m) / std::max(1.0, x1 - x0),
                       (ch - 2 * m) / std::max(1.0, y1 - y0));
    g_scale = std::max(0.05, std::min(4.0, g_scale));
    g_panX = (cw - (x1 - x0) * g_scale) / 2.0 - x0 * g_scale;
    g_panY = g_tbH + (ch - (y1 - y0) * g_scale) / 2.0 - y0 * g_scale;
    InvalidateRect(hwnd, nullptr, FALSE);
}
