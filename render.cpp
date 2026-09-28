// ============================================================================
//  render.cpp  ——  全部 GDI+ 绘制
//
//  三种坐标空间，务必分清：
//    1. 世界坐标：对象自身存储的位置尺寸（DrawScene 内部使用）
//    2. 屏幕坐标：窗口像素（DrawGrid / DrawOverlay / DrawChrome 使用）
//    3. 设备无关：字体用 UnitPoint（磅），由 GDI+ 按 DPI 自动换算
//  易错点：凡是复用"返回世界坐标"的辅助函数（ArrowPath / SidePoint），
//          在 overlay 层必须先用 PFS() 转成屏幕坐标再画，否则图形会整体偏移。
// ============================================================================
#include "edraft.h"

#include <algorithm>
#include <cmath>

// ------------------------------------------------------------------ 绘制辅助
// 画箭头头部：从尖端 (tx,ty) 沿反方向画两条短线。长度随线宽放大，但不过分
static void DrawArrowHead(Gdiplus::Graphics& g, Gdiplus::Pen& pen,
                          double tx, double ty, double ux, double uy, double sw) {
    double len = 6.0 + sw * 1.8;
    double ang = 0.42;                                  // 约 24°，两翼张开
    for (int s = -1; s <= 1; s += 2) {
        double ca = std::cos(ang * s), sa = std::sin(ang * s);
        double vx = -ux * ca - uy * sa;                 // 方向向量旋转 ±ang
        double vy =  ux * sa - uy * ca;
        g.DrawLine(&pen, PF(tx, ty), PF(tx + vx * len, ty + vy * len));
    }
}
static void ApplyDash(Gdiplus::Pen& pen, int dash) {
    if (dash == 1)      pen.SetDashStyle(Gdiplus::DashStyleDash);
    else if (dash == 2) pen.SetDashStyle(Gdiplus::DashStyleDot);
}

// ------------------------------------------------------------------ 单个对象
static void DrawObj(Gdiplus::Graphics& g, const Obj& o) {
    Gdiplus::REAL X = (Gdiplus::REAL)o.x, Y = (Gdiplus::REAL)o.y;
    Gdiplus::REAL W = (Gdiplus::REAL)o.w, H = (Gdiplus::REAL)o.h;

    // 分组方框：半透明填色 + 彩色边框，绘制层次最底
    if (o.type == OT::Rect) {
        Gdiplus::SolidBrush fill(C3(o.colorRGB, 34));
        g.FillRectangle(&fill, Gdiplus::RectF(X, Y, W, H));
        Gdiplus::Pen pen(C3(o.colorRGB), (Gdiplus::REAL)o.strokeW);
        ApplyDash(pen, o.dashMode);
        g.DrawRectangle(&pen, Gdiplus::RectF(X, Y, W, H));
        return;
    }
    // 图片
    if (o.type == OT::Image) {
        if (o.bmp) g.DrawImage(o.bmp.get(), Gdiplus::RectF(X, Y, W, H));
        else {                                          // 解码失败时画个占位虚线框
            Gdiplus::Pen pen(Gdiplus::Color(255, 160, 160, 160), 1.0f);
            pen.SetDashStyle(Gdiplus::DashStyleDash);
            g.DrawRectangle(&pen, Gdiplus::RectF(X, Y, W, H));
        }
        Gdiplus::Pen bd(Gdiplus::Color(120, 90, 90, 90), (Gdiplus::REAL)o.strokeW);
        g.DrawRectangle(&bd, Gdiplus::RectF(X, Y, W, H));
        return;
    }
    // 文本框：白底 + 边框 + 自动换行的正文（字号/粗体/颜色均按该框自身设置）
    if (o.type == OT::Text) {
        Gdiplus::SolidBrush bg(Gdiplus::Color(255, 255, 255, 255));
        g.FillRectangle(&bg, Gdiplus::RectF(X, Y, W, H));
        Gdiplus::Pen bd(Gdiplus::Color(255, 92, 92, 92), (Gdiplus::REAL)o.strokeW);
        ApplyDash(bd, o.dashMode);
        g.DrawRectangle(&bd, Gdiplus::RectF(X, Y, W, H));
        Gdiplus::Font f(FONT_NAME, (Gdiplus::REAL)o.fontSize,
                        o.bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                        Gdiplus::UnitPoint);
        Gdiplus::SolidBrush tx(C3(o.colorRGB));
        Gdiplus::StringFormat sf(Gdiplus::StringFormatFlagsNoClip);
        Gdiplus::RectF tr(X + 7.0f, Y + 5.0f, W - 14.0f, H - 10.0f);
        g.DrawString(o.text.c_str(), -1, &f, tr, &sf, &tx);
        return;
    }
    // 箭头：折线 + 头部（单/双）
    auto path = ArrowPath(o);
    Gdiplus::Pen pen(C3(o.colorRGB), (Gdiplus::REAL)o.strokeW);
    ApplyDash(pen, o.dashMode);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    for (size_t i = 1; i < path.size(); ++i) g.DrawLine(&pen, path[i - 1], path[i]);

    size_t n = path.size();
    if (o.headMode >= 1 && n >= 2) {                    // 终点箭头
        double dx = path[n - 1].X - path[n - 2].X, dy = path[n - 1].Y - path[n - 2].Y;
        double L = std::sqrt(dx * dx + dy * dy);
        if (L > 1e-6) DrawArrowHead(g, pen, path[n - 1].X, path[n - 1].Y, dx / L, dy / L, o.strokeW);
    }
    if (o.headMode == 2 && n >= 2) {                    // 双箭头：起点也画
        double dx = path[0].X - path[1].X, dy = path[0].Y - path[1].Y;
        double L = std::sqrt(dx * dx + dy * dy);
        if (L > 1e-6) DrawArrowHead(g, pen, path[0].X, path[0].Y, dx / L, dy / L, o.strokeW);
    }
}

// -------------------------------------------------------------------- 场景层
// 在"世界坐标变换"下绘制所有对象；导出 PNG 时复用本函数，保证所见即所得
void DrawScene(Gdiplus::Graphics& g) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    Gdiplus::Matrix m((Gdiplus::REAL)g_scale, 0, 0, (Gdiplus::REAL)g_scale,
                      (Gdiplus::REAL)g_panX, (Gdiplus::REAL)g_panY);
    g.SetTransform(&m);

    auto pass = [&](OT t) { for (const Obj& o : g_objs) if (o.type == t) DrawObj(g, o); };
    pass(OT::Rect); pass(OT::Image); pass(OT::Text); pass(OT::Arrow);

    if (g_drag == Drag::NewArrow) {                     // 拉箭头时的预览虚线
        Gdiplus::Pen pen(Gdiplus::Color(180, 40, 40, 40), 1.6f);
        pen.SetDashStyle(Gdiplus::DashStyleDash);
        g.DrawLine(&pen, PF(g_wx0, g_wy0), PF(g_wx, g_wy));
    }
    Gdiplus::Matrix idm;
    g.SetTransform(&idm);
}

// ---------------------------------------------------------------------- 网格
// 屏幕坐标绘制，步长随缩放自适应（保持屏幕上 20~80 像素一格）；只铺满画布区，不参与 PNG 导出
void DrawGrid(Gdiplus::Graphics& g, int W, int H) {
    if (!g_grid) return;
    double step = 10.0;
    for (int i = 0; i < 40 && step * g_scale < 20.0; ++i) step *= 2.0;
    for (int i = 0; i < 40 && step * g_scale > 80.0; ++i) step /= 2.0;

    double sx0 = std::floor(S2X(g_cvX) / step) * step, sx1 = std::ceil(S2X(W) / step) * step;
    double sy0 = std::floor(S2Y(g_cvY) / step) * step, sy1 = std::ceil(S2Y(H) / step) * step;
    Gdiplus::Pen minor(Gdiplus::Color(60, 120, 140, 165), 1.0f);
    Gdiplus::Pen major(Gdiplus::Color(100, 95, 120, 150), 1.0f);

    int k = 0;
    for (double wx = sx0; wx <= sx1; wx += step, ++k) {
        float sxx = (float)W2X(wx) + 0.5f;              // +0.5 让 1px 线不虚
        g.DrawLine((k % 5 == 0) ? &major : &minor, sxx, (float)g_cvY, sxx, (float)H);
    }
    k = 0;
    for (double wy = sy0; wy <= sy1; wy += step, ++k) {
        float syy = (float)W2Y(wy) + 0.5f;
        g.DrawLine((k % 5 == 0) ? &major : &minor, (float)g_cvX, syy, (float)W, syy);
    }
}

// ------------------------------------------------------------------ 覆盖层
// 是否需要显示四边连接锚点：箭头工具激活 / 正在拉箭头 / 选中了箭头 / 正在拖端点
bool ShowAnchors() {
    if (g_tool == Tool::ArrowT || g_drag == Drag::NewArrow) return true;
    if (g_drag == Drag::Resize && (g_handle == H_HEAD || g_handle == H_TAIL)) return true;
    if (Obj* o = ObjOf(PrimaryId()); o && o->type == OT::Arrow) return true;
    return false;
}

void DrawOverlay(Gdiplus::Graphics& g) {
    Gdiplus::Matrix idm;
    g.SetTransform(&idm);                                // 覆盖层一律用屏幕坐标
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    // 1) 四边连接锚点。两种显示时机：
    //    · 箭头工具激活时：给所有对象显示，方便找连接点
    //    · 否则：只给【已选中】的对象显示，兼作"我选了哪些"的视觉提示
    //    （鼠标靠近会高亮；多选时这是判断选中范围最直观的标记）
    {
        bool showAll = ShowAnchors();
        int hovId = -1, hovSide = -1;
        FindAnchor(g_mwx, g_mwy, hovId, hovSide);
        for (const Obj& o : g_objs) {
            if (!IsBox(o.type)) continue;
            if (!showAll && !IsSel(o.id)) continue;
            for (int s = 0; s < 4; ++s) {
                double ax, ay; SidePoint(o, s, ax, ay);
                double sx = W2X(ax), sy = W2Y(ay);
                bool hot = (o.id == hovId && s == hovSide);
                float r = hot ? 6.0f : 4.0f;
                Gdiplus::SolidBrush fb(hot ? Gdiplus::Color(255, 240, 90, 40)
                                           : Gdiplus::Color(230, 255, 255, 255));
                Gdiplus::Pen bp(hot ? Gdiplus::Color(255, 200, 60, 20)
                                    : Gdiplus::Color(255, 60, 130, 200), 1.4f);
                g.FillEllipse(&fb, Gdiplus::RectF((Gdiplus::REAL)(sx - r), (Gdiplus::REAL)(sy - r), r * 2, r * 2));
                g.DrawEllipse(&bp, Gdiplus::RectF((Gdiplus::REAL)(sx - r), (Gdiplus::REAL)(sy - r), r * 2, r * 2));
            }
        }
    }
    // 2) 新建方框/文本框时的预览
    if (g_drag == Drag::NewRect || g_drag == Drag::NewText) {
        double x0 = std::min(g_wx0, g_wx), y0 = std::min(g_wy0, g_wy);
        double x1 = std::max(g_wx0, g_wx), y1 = std::max(g_wy0, g_wy);
        Gdiplus::Pen pen(Gdiplus::Color(255, 40, 120, 200), 1.4f);
        pen.SetDashStyle(Gdiplus::DashStyleDash);
        g.DrawRectangle(&pen, Gdiplus::RectF((Gdiplus::REAL)W2X(x0), (Gdiplus::REAL)W2Y(y0),
                                             (Gdiplus::REAL)((x1 - x0) * g_scale),
                                             (Gdiplus::REAL)((y1 - y0) * g_scale)));
    }
    // 3) 框选矩形
    if (g_drag == Drag::Marquee) {
        double x0 = std::min(g_wx0, g_wx), y0 = std::min(g_wy0, g_wy);
        double x1 = std::max(g_wx0, g_wx), y1 = std::max(g_wy0, g_wy);
        double L = W2X(x0), T = W2Y(y0);
        double W = (x1 - x0) * g_scale, H = (y1 - y0) * g_scale;
        Gdiplus::SolidBrush fb(Gdiplus::Color(40, 40, 120, 200));
        g.FillRectangle(&fb, Gdiplus::RectF((Gdiplus::REAL)L, (Gdiplus::REAL)T,
                                            (Gdiplus::REAL)W, (Gdiplus::REAL)H));
        Gdiplus::Pen pen(Gdiplus::Color(255, 30, 110, 220), 1.4f);
        pen.SetDashStyle(Gdiplus::DashStyleDash);
        g.DrawRectangle(&pen, Gdiplus::RectF((Gdiplus::REAL)L, (Gdiplus::REAL)T,
                                             (Gdiplus::REAL)W, (Gdiplus::REAL)H));
    }

    // 4) 每个选中对象的选中框；手柄只在单选时出现
    bool single = (g_sels.size() == 1);
    for (int sid : g_sels) {
        Obj* o = ObjOf(sid);
        if (!o) continue;
        Gdiplus::Pen pen(Gdiplus::Color(255, 30, 110, 220), 1.6f);
        pen.SetDashStyle(Gdiplus::DashStyleDash);

        if (o->type == OT::Arrow) {
            auto p = ArrowPath(*o);
            for (size_t i = 1; i < p.size(); ++i)       // 世界坐标 → PFS() 转屏幕坐标
                g.DrawLine(&pen, PFS(p[i - 1].X, p[i - 1].Y), PFS(p[i].X, p[i].Y));
            if (single) {
                if (o->lineMode == 1) {                 // 折点手柄（橙色）
                    Route r = ArrowRoute(*o);
                    if (r.hasBend) {
                        Gdiplus::SolidBrush fb(Gdiplus::Color(255, 255, 170, 60));
                        Gdiplus::Pen hp(Gdiplus::Color(255, 200, 110, 20), 1.6f);
                        double bx = W2X(r.midX), by = W2Y(r.midY);
                        g.FillEllipse(&fb, Gdiplus::RectF((Gdiplus::REAL)(bx - 5), (Gdiplus::REAL)(by - 5), 10, 10));
                        g.DrawEllipse(&hp, Gdiplus::RectF((Gdiplus::REAL)(bx - 5), (Gdiplus::REAL)(by - 5), 10, 10));
                    }
                }
                Gdiplus::PointF pts[2] = { PFS(p.front().X, p.front().Y), PFS(p.back().X, p.back().Y) };
                for (int i = 0; i < 2; ++i) {           // 两端点手柄（白=起点 黄=终点）
                    Gdiplus::SolidBrush fb(i == 0 ? Gdiplus::Color(255, 255, 255, 255)
                                                  : Gdiplus::Color(255, 255, 225, 160));
                    Gdiplus::Pen hp(Gdiplus::Color(255, 30, 110, 220), 1.6f);
                    g.FillEllipse(&fb, Gdiplus::RectF(pts[i].X - 5, pts[i].Y - 5, 10, 10));
                    g.DrawEllipse(&hp, Gdiplus::RectF(pts[i].X - 5, pts[i].Y - 5, 10, 10));
                }
            }
            continue;
        }

        double L = W2X(o->x), T = W2Y(o->y);
        double W = o->w * g_scale, H = o->h * g_scale;

        // 淡蓝高亮填充：多选时让"到底选中了哪些"一眼可见
        Gdiplus::RectF sr((Gdiplus::REAL)L, (Gdiplus::REAL)T, (Gdiplus::REAL)W, (Gdiplus::REAL)H);
        Gdiplus::SolidBrush sfb(Gdiplus::Color(26, 30, 110, 220));
        g.FillRectangle(&sfb, sr);
        g.DrawRectangle(&pen, sr);

        // 方框专用：左上角外侧的圆形"整体移动"手柄，另画一段细线连到顶点。
        // 因为方框只有细边框可点，直接拖边框很难点中。
        if (single && o->type == OT::Rect) {
            double mx, my;
            RectMoveHandlePos(*o, mx, my);
            Gdiplus::Pen lp(C3(o->colorRGB), 1.4f);
            g.DrawLine(&lp, PF(mx, my), PF(L, T));
            Gdiplus::RectF mr((Gdiplus::REAL)(mx - 6), (Gdiplus::REAL)(my - 6), 12, 12);
            Gdiplus::SolidBrush fb(C3(o->colorRGB));
            Gdiplus::Pen hp(Gdiplus::Color(255, 255, 255, 255), 1.6f);
            g.FillEllipse(&fb, mr);
            g.DrawEllipse(&hp, mr);
        }

        if (!single) continue;

        double cx = L + W / 2.0, cy = T + H / 2.0;
        double px[8] = { L, L + W, L + W, L,     cx, L + W, cx, L };
        double py[8] = { T, T,     T + H, T + H, T,  cy,    T + H, cy };
        for (int i = 0; i < 8; ++i) {
            Gdiplus::SolidBrush fb(Gdiplus::Color(255, 255, 255, 255));
            Gdiplus::Pen hp(Gdiplus::Color(255, 30, 110, 220), 1.4f);
            if (i < 4) {                                 // 四角：方块
                g.FillRectangle(&fb, Gdiplus::RectF((Gdiplus::REAL)(px[i] - 4), (Gdiplus::REAL)(py[i] - 4), 8, 8));
                g.DrawRectangle(&hp, Gdiplus::RectF((Gdiplus::REAL)(px[i] - 4), (Gdiplus::REAL)(py[i] - 4), 8, 8));
            } else {                                     // 四边中点：小圆
                g.FillEllipse(&fb, Gdiplus::RectF((Gdiplus::REAL)(px[i] - 3.5f), (Gdiplus::REAL)(py[i] - 3.5f), 7, 7));
                g.DrawEllipse(&hp, Gdiplus::RectF((Gdiplus::REAL)(px[i] - 3.5f), (Gdiplus::REAL)(py[i] - 3.5f), 7, 7));
            }
        }
    }
}

// ------------------------------------------------------------ 顶部栏目与功能区
// 画三层：深色顶栏（程序名 + 版本 + 当前文件名）、浅色功能区底板、分组底板与组名。
// 功能区有两种排布：横排（顶栏下一条）与竖排（左侧一列），由 g_tbVert 决定。
// 按钮本身是自绘子窗口，由 WM_DRAWITEM 单独绘制，会盖在本函数画的底板之上。
void DrawChrome(Gdiplus::Graphics& g, int W, int H) {
    Gdiplus::SolidBrush band(Gdiplus::Color(255, 47, 59, 76));
    g.FillRectangle(&band, Gdiplus::RectF(0, 0, (Gdiplus::REAL)W, (Gdiplus::REAL)g_bandH));

    Gdiplus::Font tf(FONT_NAME, (Gdiplus::REAL)(13.0 * g_dpiScale),
                     Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush tw(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::StringFormat lsf;
    lsf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::RectF tr((Gdiplus::REAL)(12 * g_dpiScale), 0,
                      (Gdiplus::REAL)(400 * g_dpiScale), (Gdiplus::REAL)g_bandH);
    g.DrawString(APP_NAME, -1, &tf, tr, &lsf, &tw);

    // 版本号紧跟程序名后面：量一次实际宽度再定位，避免不同字体/DPI 下错位
    Gdiplus::RectF mb;
    g.MeasureString(APP_NAME, -1, &tf,
                    Gdiplus::RectF(0, 0, (Gdiplus::REAL)W, (Gdiplus::REAL)g_bandH), &mb);
    Gdiplus::Font vf(FONT_NAME, (Gdiplus::REAL)(10.0 * g_dpiScale),
                     Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush vw(Gdiplus::Color(190, 176, 190, 214));
    Gdiplus::RectF vr((Gdiplus::REAL)(12 * g_dpiScale + mb.Width + 6 * g_dpiScale), 0,
                      (Gdiplus::REAL)(140 * g_dpiScale), (Gdiplus::REAL)g_bandH);
    g.DrawString(APP_VER, -1, &vf, vr, &lsf, &vw);

    wstring right = g_file.empty() ? L"未命名"
                  : g_file.substr(g_file.find_last_of(L"\\/") == wstring::npos
                                  ? 0 : g_file.find_last_of(L"\\/") + 1);
    if (g_dirty) right += L" *";
    Gdiplus::SolidBrush sw(Gdiplus::Color(200, 220, 228, 240));
    Gdiplus::StringFormat rsf;
    rsf.SetAlignment(Gdiplus::StringAlignmentFar);
    rsf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::RectF rr(0, 0, (Gdiplus::REAL)(W - 14 * g_dpiScale), (Gdiplus::REAL)g_bandH);
    g.DrawString(right.c_str(), -1, &tf, rr, &rsf, &sw);

    // ---- 功能区底板 ----
    Gdiplus::SolidBrush rib(Gdiplus::Color(255, 243, 244, 246));
    if (!g_tbVert) {
        g.FillRectangle(&rib, Gdiplus::RectF(0, (Gdiplus::REAL)g_bandH,
                                             (Gdiplus::REAL)W, (Gdiplus::REAL)g_ribH));
    } else {
        g.FillRectangle(&rib, Gdiplus::RectF(0, (Gdiplus::REAL)g_bandH,
                                             (Gdiplus::REAL)g_ribW,
                                             (Gdiplus::REAL)(H - g_bandH)));
        // 竖排时在右边缘画一条分隔线，和画布区分开
        Gdiplus::Pen edge(Gdiplus::Color(255, 214, 218, 226), 1.0f);
        g.DrawLine(&edge, (Gdiplus::REAL)(g_ribW - 1), (Gdiplus::REAL)g_bandH,
                           (Gdiplus::REAL)(g_ribW - 1), (Gdiplus::REAL)H);
    }

    // ---- 分组底板与组名 ----
    for (int gi = 0; gi < NGROUP; ++gi) {
        int rgb = GROUPS[gi].rgb;
        BYTE r = (BYTE)((rgb >> 16) & 255), gg = (BYTE)((rgb >> 8) & 255), b = (BYTE)(rgb & 255);
        Gdiplus::SolidBrush pb(Gdiplus::Color(22, r, gg, b));      // 极淡的分组底板
        Gdiplus::Font lf(FONT_NAME, (Gdiplus::REAL)(10.5 * g_dpiScale),
                         Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush lb(Gdiplus::Color(255, (BYTE)(r * 0.8), (BYTE)(gg * 0.8), (BYTE)(b * 0.8)));
        Gdiplus::StringFormat csf;
        csf.SetAlignment(Gdiplus::StringAlignmentCenter);

        if (!g_tbVert) {
            g.FillRectangle(&pb, Gdiplus::RectF((Gdiplus::REAL)g_gx0[gi],
                                                (Gdiplus::REAL)(g_bandH + 3 * g_dpiScale),
                                                (Gdiplus::REAL)(g_gx1[gi] - g_gx0[gi]),
                                                (Gdiplus::REAL)(g_ribH - 6 * g_dpiScale)));
            Gdiplus::RectF lr((Gdiplus::REAL)g_gx0[gi],
                              (Gdiplus::REAL)(g_bandH + 36 * g_dpiScale),
                              (Gdiplus::REAL)(g_gx1[gi] - g_gx0[gi]),
                              (Gdiplus::REAL)(16 * g_dpiScale));
            g.DrawString(GROUPS[gi].label, -1, &lf, lr, &csf, &lb);
        } else {
            double lh = (double)g_vLblH;
            g.FillRectangle(&pb, Gdiplus::RectF((Gdiplus::REAL)(3 * g_dpiScale),
                                                (Gdiplus::REAL)g_gy0[gi],
                                                (Gdiplus::REAL)(g_ribW - 6 * g_dpiScale),
                                                (Gdiplus::REAL)(g_gy1[gi] - g_gy0[gi])));
            Gdiplus::RectF lr((Gdiplus::REAL)(6 * g_dpiScale), (Gdiplus::REAL)g_gy0[gi],
                              (Gdiplus::REAL)(g_ribW - 12 * g_dpiScale), (Gdiplus::REAL)lh);
            g.DrawString(GROUPS[gi].label, -1, &lf, lr, &csf, &lb);   // 竖排：标题居中在组首
        }
    }
}

// -------------------------------------------------------------- 工具栏按钮
const BtnDef* FindBtn(int id) {
    for (int i = 0; i < NBTN; ++i) if (BTNS[i].id == id) return &BTNS[i];
    return nullptr;
}
// 按钮的"选中态"：当前工具 / 空白拖动模式 / 网格开关 / 置顶
bool IsBtnChecked(int id) {
    switch (id) {
    case IDB_SELECT:  return g_tool == Tool::Select;
    case IDB_RECT:    return g_tool == Tool::RectT;
    case IDB_TEXT:    return g_tool == Tool::TextT;
    case IDB_ARROW:   return g_tool == Tool::ArrowT;
    case IDB_PANMODE: return g_emptyMode == 1;
    case IDB_SELMODE: return g_emptyMode == 0;
    case IDB_GRID:    return g_grid;
    case IDB_TOPMOST: return g_topmost;
    }
    return false;
}
// 重绘所有按钮。注意：对父窗口 InvalidateRect 不会重绘子窗口，必须逐个按钮来。
// 顺带刷新"横排/竖排"按钮的标题 —— 它显示的是"点一下会变成什么"。
void RefreshButtons() {
    for (int i = 0; i < NBTN; ++i) {
        HWND h = GetDlgItem(g_hwnd, BTNS[i].id);
        if (!h) continue;
        if (BTNS[i].id == IDB_TBV) SetWindowTextW(h, g_tbVert ? L"横排" : L"竖排");
        InvalidateRect(h, nullptr, FALSE);
    }
}

// 创建全部自绘按钮（BS_OWNERDRAW）。只建一次，位置交给 LayoutToolbar。
void MakeToolbar(HWND hwnd) {
    for (int i = 0; i < NBTN; ++i) {
        if (GetDlgItem(hwnd, BTNS[i].id)) continue;
        CreateWindowW(L"BUTTON", BTNS[i].cap, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)BTNS[i].id, g_inst, nullptr);
    }
    LayoutToolbar(hwnd);
}

// 按当前方向摆放按钮，并算出分组底板范围与画布区原点。
// 窗口大小变化时也要调用（竖排时按钮高度会随可用高度自动收缩，尽量都塞得下）。
void LayoutToolbar(HWND hwnd) {
    if (!hwnd) return;
    RECT rc{}; GetClientRect(hwnd, &rc);
    const int H = rc.bottom;
    auto S = [](double v) { return (int)lround(v * g_dpiScale); };

    if (!g_tbVert) {
        // ------------------------------------------------ 横排：顶栏下面一整条
        int pad = S(8), gap = S(3), ggap = S(16), bh = S(TB_BTN_H);
        // 窗口不够宽时，先把按钮宽度和间距一起等比压窄（最多压到 TB_HSHRINK），
        // 这样"窗口略小"不会立刻把后面几组按钮挤到窗口外；再窄就只能改用竖排了。
        int need = 2 * pad + (NGROUP - 1) * ggap;
        for (int i = 0; i < NBTN; ++i) need += S(BTNS[i].w) + gap;
        double k = 1.0;
        if (rc.right > 0 && need > rc.right) k = std::max(TB_HSHRINK, (double)rc.right / need);
        auto SK = [&](int v) { return (int)lround(v * k); };

        int p2 = SK(pad), gap2 = SK(gap), ggap2 = SK(ggap);
        int by = g_bandH + S(6);
        int x = p2;
        for (int gi = 0; gi < NGROUP; ++gi) {
            g_gx0[gi] = x - SK(S(5));
            for (int i = 0; i < NBTN; ++i) {
                if (BTNS[i].grp != gi) continue;
                int bw = SK(S(BTNS[i].w));
                if (HWND h = GetDlgItem(hwnd, BTNS[i].id)) MoveWindow(h, x, by, bw, bh, TRUE);
                x += bw + gap2;
            }
            g_gx1[gi] = x - gap2 + SK(S(5));
            x += ggap2;
        }
        g_cvX = 0;
        g_cvY = g_bandH + g_ribH;
    } else {
        // ------------------------------------------------ 竖排：左侧一列
        // 20 个按钮一列排下来，高度是唯一要操心的事：按客户的可用高度反推按钮高度，
        // 装不下就先换"紧凑间距"，再不够就把按钮压到下限（此时最下面的按钮会被窗口裁掉）。
        int bw0 = S(6);                                   // 左右内边距
        int bw = g_ribW - 2 * bw0;
        if (bw < S(30)) bw = S(30);
        int avail = H - g_bandH - S(6) - S(8);

        struct Lay { int lblH, lblGap, gap, ggap, bh; };
        Lay lay{};
        auto tryMode = [&](bool compact, Lay& out) -> bool {
            out.lblH  = compact ? S(12) : S(TB_VLBL_H);
            out.lblGap = compact ? 0 : S(3);
            out.gap   = compact ? S(1) : S(2);
            out.ggap  = compact ? S(4) : S(7);
            int fix = NGROUP * (out.lblH + out.lblGap)
                    + (NBTN - NGROUP) * out.gap + (NGROUP - 1) * out.ggap;
            int bh = (int)((avail - fix) / (double)NBTN);
            if (bh > S(TB_VMAX_H)) bh = S(TB_VMAX_H);
            out.bh = bh;
            return bh >= S(TB_VMIN_H);
        };
        if (!tryMode(false, lay)) { Lay compact{}; tryMode(true, compact); lay = compact; }
        if (lay.bh < S(TB_VMIN_H)) lay.bh = S(TB_VMIN_H);

        int y = g_bandH + S(6);
        for (int gi = 0; gi < NGROUP; ++gi) {
            g_gy0[gi] = y;
            y += lay.lblH + lay.lblGap;                   // 组名占一行
            for (int i = 0; i < NBTN; ++i) {
                if (BTNS[i].grp != gi) continue;
                if (HWND h = GetDlgItem(hwnd, BTNS[i].id)) MoveWindow(h, bw0, y, bw, lay.bh, TRUE);
                y += lay.bh + lay.gap;
            }
            g_gy1[gi] = y - lay.gap;
            y += lay.ggap;
        }
        g_cvX = g_ribW;
        g_cvY = g_bandH;
        // 组名行高随紧凑模式变化，DrawChrome 要跟着用同一个值
        g_vLblH = lay.lblH;
    }

    HWND hv = GetDlgItem(hwnd, IDB_TBV);
    if (hv) SetWindowTextW(hv, g_tbVert ? L"横排" : L"竖排");
}

// 切换功能区排布。按钮不重建，只重新摆放 + 重绘，所以切换是瞬时的。
void SetToolbarVert(HWND hwnd, bool vert) {
    if (g_tbVert == vert) return;
    CommitEdit();
    g_tbVert = vert;
    LayoutToolbar(hwnd);
    RefreshButtons();
    InvalidateRect(hwnd, nullptr, FALSE);
}
