// ============================================================================
//  edraft.h  ——  Edraft 公共头文件：类型、常量、全局状态与函数声明
//
//  模块划分：
//    main.cpp   程序入口、主窗口过程（消息分发、鼠标/键盘交互）
//    model.cpp  数据模型与几何：对象查找、箭头路由、包围盒、命中检测、撤销重做
//    render.cpp 绘制：对象、网格、选中框、顶部栏目与功能区（含自绘按钮）
//    io.cpp     持久化与输入输出：.edraft 读写、图片编解码、剪贴板、PNG 导出
//    ui.cpp     交互控件：文本框就地编辑、自定义数值输入框、右键菜单、工具栏逻辑
// ============================================================================
#pragma once

#ifndef WINVER
#define WINVER 0x0A00              // 需要 Vista+ 的部分 API
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>              // GET_X_LPARAM / GET_Y_LPARAM / GET_WHEEL_DELTA_WPARAM
#include <gdiplus.h>
#include <objidl.h>                // IStream
#include <shlwapi.h>               // SHCreateMemStream

#include <memory>
#include <string>
#include <vector>

using std::string;
using std::vector;
using std::wstring;

// ------------------------------------------------------------------ 基本常量
// 说明：头文件里的常量一律用 inline constexpr（C++17 内联变量）。
// 若写成 static const，每个翻译单元都会各存一份，且未用到的会触发 -Wunused-variable 警告。
inline constexpr double MIN_SIZE = 24.0;                  // 对象最小尺寸（世界坐标）
inline constexpr const wchar_t* FONT_NAME  = L"Microsoft YaHei";   // 界面与正文默认字体
inline constexpr const wchar_t* APP_NAME   = L"Edraft";            // 程序名
inline constexpr const wchar_t* FILE_EXT   = L"edraft";            // 存档扩展名（只认这一种）
inline constexpr const wchar_t* FILE_HDR   = L"EDRAFT3";           // 存档文件头（含线型/折弯字段）
inline constexpr const wchar_t* FILE_FLT   = L"Edraft 文件 (*.edraft)\0*.edraft\0";

// ------------------------------------------------------------------ 类型枚举
// 对象类型；同时决定绘制层次（枚举顺序即 z 序，越靠后越上层）
enum class OT { Rect, Image, Text, Arrow };

// 工具（功能区左侧「指针 / 添加元素」的当前模式）
enum class Tool { Select, RectT, TextT, ArrowT, ImageT };

// 鼠标拖拽状态
enum class Drag { None, Pan, Move, Resize, NewRect, NewText, NewArrow, Marquee };

// 工具栏按钮命令 ID
enum {
    IDB_SELECT = 1001, IDB_RECT, IDB_TEXT, IDB_ARROW, IDB_IMAGE,
    IDB_OPEN, IDB_SAVE, IDB_SAVEAS, IDB_EXPORT, IDB_DEL,
    IDB_UNDO, IDB_REDO, IDB_FIT, IDB_HOME,
    IDB_PANMODE = 1015,      // 空白拖动缺省 = 平移
    IDB_SELMODE,             // 空白拖动缺省 = 框选
    IDB_GRID,                // 网格开关
    IDC_EDIT = 1100          // 文本框就地编辑用的 EDIT 子窗口
};

// 右键菜单命令 ID
enum {
    IDM_EDITTEXT = 2001, IDM_BOLD, IDM_DELOBJ,
    IDM_SIZE_BASE   = 2010,                    // +0..5  预设字号
    IDM_SIZE_CUSTOM = 2020,                    // 自定义字号
    IDM_COLOR_BASE  = 2030,                    // +0..7  颜色
    IDM_LINE_STRAIGHT = 2050, IDM_LINE_POLY,   // 直线 / 折线箭头
    IDM_HEAD_NONE = 2052, IDM_HEAD_SINGLE, IDM_HEAD_DOUBLE,
    IDM_DASH_SOLID = 2055, IDM_DASH_DASH, IDM_DASH_DOT,   // 实线 / 虚线 / 点线
    IDM_IMG_ORIG = 2060,                       // 图片恢复原始像素
    IDM_HOME = 2070, IDM_FIT2, IDM_GRIDM,      // 画布空白处菜单
    IDM_SW_BASE   = 2080,                      // +0..4  预设线宽
    IDM_SW_CUSTOM = 2090                       // 自定义线宽
};

// 手柄编号：0..3 四角，4..7 四边中点，8/9 箭头两端点，10 折线折点，11 方框移动手柄
enum { H_TL = 0, H_TR, H_BR, H_BL, H_TOP, H_RIGHT, H_BOTTOM, H_LEFT, H_HEAD, H_TAIL, H_BEND, H_MOVE };

// ------------------------------------------------------------------ 可调参数
inline constexpr double FONT_SIZES[] = { 10, 12, 14, 18, 24, 32 };   // 字号预设（磅）
inline constexpr double STROKES[]    = { 2, 4, 6, 10, 16 };           // 线宽预设
inline constexpr const wchar_t* SWNAMES[] =
    { L"细 (2)", L"中 (4)", L"粗 (6)", L"很粗 (10)", L"特粗 (16)" };
// 自定义线宽的允许范围。注意不能命名为 SW_MIN/SW_MAX：winuser.h 已占用 SW_MAX 宏
inline constexpr double STROKE_MIN = 0.5, STROKE_MAX = 60.0;

struct ColItem { const wchar_t* name; int rgb; };
inline constexpr ColItem COLORS[] = {      // 右键「颜色」菜单的 8 种预设
    { L"黑", 0x1A1A1A }, { L"灰", 0x6B7280 }, { L"红", 0xC0392B }, { L"橙", 0xD35400 },
    { L"绿", 0x1E8449 }, { L"蓝", 0x1F6FB2 }, { L"紫", 0x7D3C98 }, { L"棕", 0x8B5A2B }
};
inline constexpr int NCOL = (int)(sizeof(COLORS)  / sizeof(COLORS[0]));
inline constexpr int NSW  = (int)(sizeof(STROKES) / sizeof(STROKES[0]));

// 各类型默认线宽：方框 10 / 箭头 6 / 文本框 4（图片边框固定较细，见 io.cpp）
inline double DefStroke(OT t) {
    return (t == OT::Rect) ? 10.0 : ((t == OT::Arrow) ? 6.0 : 4.0);
}

// ------------------------------------------------------- 功能区分组与按钮定义
enum { G_PTR = 0, G_ADD, G_EDIT, G_FILE, G_VIEW, NGROUP };
struct GroupDef { const wchar_t* label; int rgb; };
inline constexpr GroupDef GROUPS[NGROUP] = {   // 每组一个主题色
    { L"指针",     0x2F6FB2 },   // 蓝
    { L"添加元素", 0x128A5A },   // 绿
    { L"编辑",     0xB87400 },   // 橙
    { L"文件",     0x6D4BA8 },   // 紫
    { L"视图",     0x0E7C86 },   // 青
};
struct BtnDef { int id; const wchar_t* cap; int w; int grp; bool checkable; };
inline constexpr BtnDef BTNS[] = {
    { IDB_SELECT,  L"选择",   52, G_PTR,  true  },
    { IDB_PANMODE, L"平移",   44, G_PTR,  true  },
    { IDB_SELMODE, L"框选",   44, G_PTR,  true  },
    { IDB_RECT,    L"方框",   50, G_ADD,  true  },
    { IDB_TEXT,    L"文本",   50, G_ADD,  true  },
    { IDB_ARROW,   L"箭头",   50, G_ADD,  true  },
    { IDB_IMAGE,   L"图片",   50, G_ADD,  false },   // 一次性动作，不保持选中态
    { IDB_UNDO,    L"撤销",   50, G_EDIT, false },
    { IDB_REDO,    L"重做",   50, G_EDIT, false },
    { IDB_DEL,     L"删除",   50, G_EDIT, false },
    { IDB_OPEN,    L"打开",   48, G_FILE, false },
    { IDB_SAVE,    L"保存",   48, G_FILE, false },
    { IDB_SAVEAS,  L"另存为", 58, G_FILE, false },
    { IDB_EXPORT,  L"导出",   48, G_FILE, false },
    { IDB_FIT,     L"适应",   50, G_VIEW, false },
    { IDB_HOME,    L"原点",   50, G_VIEW, false },
    { IDB_GRID,    L"网格",   50, G_VIEW, true  },
};
inline constexpr int NBTN = (int)(sizeof(BTNS) / sizeof(BTNS[0]));

// ------------------------------------------------------------------ 数据模型
struct Obj {
    int id = 0;
    OT type = OT::Rect;

    double x = 0, y = 0, w = 0, h = 0;   // 包围盒（世界坐标）
    double x2 = 0, y2 = 0;               // 箭头：自由端点坐标（两端都绑定时此值不参与绘制）

    wstring text;                         // 文本框内容
    std::shared_ptr<vector<BYTE>> imgBytes;   // 图片原始字节（存档用，PNG 格式）
    std::shared_ptr<Gdiplus::Bitmap> bmp;     // 解码后的位图（显示用）
    std::shared_ptr<IStream> stream;          // 位图解码依赖的流，必须与 bmp 同生命周期

    int fromId = -1, toId = -1;          // 箭头两端绑定的对象 id（-1 = 自由端点）
    int fromSide = -1, toSide = -1;      // 绑定到的边：0左 1上 2右 3下（-1 = 自动取最近边）

    int    lineMode = 0;                 // 0 直线  1 折线
    int    headMode = 1;                 // 0 无箭头 1 单箭头 2 双箭头
    int    dashMode = 0;                 // 0 实线  1 虚线  2 点线
    double bend     = 0.5;               // 折线中间段位置比例（可拖动折点手柄调整）

    int    colorRGB  = 0x1A1A1A;         // 文字色 / 边框色 / 箭头色
    double strokeW   = 1.2;              // 线宽
    double fontSize  = 14.0;             // 文本框字号（磅）
    bool   bold      = false;            // 文本框粗体
};

// 箭头两端点
struct Ends { double x1, y1, x2, y2; };
// 折线路由信息（保证严格正交）
struct Route {
    double ax = 0, ay = 0, bx = 0, by = 0;   // 出点 / 入点
    bool n1h = false, n2h = false;           // 起止法向是否为水平
    bool hasBend = false;                    // 是否有可拖动的中间段
    bool bendIsX = false;                    // true=中间段竖直(控 mx)；false=中间段水平(控 my)
    double midX = 0, midY = 0;               // 折点手柄位置（世界坐标）
};
// 多选整体移动时记录的原始位置
struct MoveOrig { int id; double x, y, x2, y2; };

// ------------------------------------------------------------------ 全局状态
// （定义都在 main.cpp，这里只作 extern 声明）
extern vector<Obj>         g_objs;
extern int                 g_nextId;
extern double              g_scale, g_panX, g_panY;   // 视图：缩放与平移
extern Tool                g_tool;
extern bool                g_grid;                    // 是否显示网格
extern int                 g_emptyMode;               // 空白左键拖动：0=框选 1=平移

extern vector<int>         g_sels;                    // 当前选中的对象 id 集合（支持多选）

extern double              g_dpi, g_dpiScale;         // 系统 DPI 与缩放系数
extern int                 g_bandH, g_ribH, g_tbH;    // 顶部栏目 / 功能区 / 合计高度
extern HFONT               g_uiFont;

extern Drag                g_drag;
extern int                 g_handle;
extern double              g_sx0, g_sy0;              // 按下时的屏幕坐标
extern double              g_wx0, g_wy0;              // 按下时的世界坐标
extern double              g_wx,  g_wy;               // 当前世界坐标
extern double              g_mwx, g_mwy;              // 鼠标世界坐标（锚点悬停高亮用）
extern bool                g_mouseIn;                 // 鼠标是否在画布区（决定粘贴落点）
extern double              g_ox, g_oy, g_ow, g_oh;    // 拖拽前原值
extern double              g_obend;                   // 折点拖动前的 bend
extern bool                g_epMoved, g_space;
extern int                 g_arrowFrom, g_arrowFromSide;
extern vector<MoveOrig>    g_moveOrig;

extern HWND                g_hwnd, g_edit;
extern int                 g_editingId;
extern HINSTANCE           g_inst;

extern wstring             g_file;
extern bool                g_dirty;
extern vector<vector<Obj>> g_undo, g_redo;
extern ULONG_PTR           g_gdiToken;

extern int                 g_gx0[NGROUP], g_gx1[NGROUP];   // 各分组横向范围

// ------------------------------------------------------------------ 坐标变换
// 世界坐标 ↔ 屏幕坐标。屏幕 = 世界 × 缩放 + 平移
inline double W2X(double wx) { return wx * g_scale + g_panX; }
inline double W2Y(double wy) { return wy * g_scale + g_panY; }
inline double S2X(double sx) { return (sx - g_panX) / g_scale; }
inline double S2Y(double sy) { return (sy - g_panY) / g_scale; }

inline Gdiplus::PointF PF(double x, double y) {                    // 直接按屏幕坐标建点
    return Gdiplus::PointF((Gdiplus::REAL)x, (Gdiplus::REAL)y);
}
inline Gdiplus::PointF PFS(double wx, double wy) {                 // 世界坐标 → 屏幕点
    return PF(W2X(wx), W2Y(wy));
}
inline Gdiplus::Color C3(int rgb, BYTE a = 255) {                  // 0xRRGGBB → GDI+ 颜色
    return Gdiplus::Color(a, (BYTE)((rgb >> 16) & 255),
                             (BYTE)((rgb >> 8) & 255), (BYTE)(rgb & 255));
}

// ------------------------------------------------------------ 选中集合的操作
bool IsSel(int id);                 // 是否选中
int  PrimaryId();                   // 主选中对象（最后一个），手柄只作用于它
void SetSel(int id);                // 单选
void AddSel(int id);                // 追加
void ToggleSel(int id);             // 切换

// ============================ model.cpp ============================
int    IndexOf(int id);
Obj*   ObjOf(int id);
bool   IsBox(OT t);
const wchar_t* TypeName(OT t);
bool   MakeBmp(Obj& o);                                  // 由 imgBytes 解码出 bmp

void   SidePoint(const Obj& b, int side, double& ax, double& ay);   // 某条边中点的锚点
int    NearestSide(const Obj& b, double wx, double wy);             // 离某点最近的边
bool   FindAnchor(double wx, double wy, int& oid, int& side);       // 光标是否落在锚点上

Ends   ArrowEnds(const Obj& a);                          // 解析箭头两端（含绑定）
Route  ArrowRoute(const Obj& a);                         // 折线路由信息
vector<Gdiplus::PointF> ArrowPath(const Obj& a);         // 箭头折线顶点序列

void   ObjBounds(const Obj& o, double& x0, double& y0, double& x1, double& y1);
void   ContentBounds(double& x0, double& y0, double& x1, double& y1);

int    HitTest(double wx, double wy);                    // 命中对象（返回 id，-1 无）
int    HitHandle(int sx, int sy);                        // 命中手柄（返回编号，-1 无）
// 方框"整体移动"手柄的屏幕坐标（左上角外侧）。方框只有细边框可点，
// 单独给一个圆形手柄会好用得多；放在外侧是为了不和四角/四边的缩放手柄重叠。
void   RectMoveHandlePos(const Obj& o, double& sx, double& sy);
void   ResolveBind(double wx, double wy, int& bid, int& bside);   // 解析箭头端点吸附
void   DeleteSelected();                                 // 删除选中（连带其绑定箭头）

void   SnapUndo();                                       // 记录一次撤销快照
void   DoUndo();
void   DoRedo();
void   UpdateUndoButtons();                              // 按栈是否为空置灰按钮

void   GoHome();                                         // 回到原点
void   ZoomFit(HWND hwnd);                               // 适应窗口

// ============================ render.cpp ============================
void   DrawScene(Gdiplus::Graphics& g);                  // 所有对象（世界坐标变换内）
void   DrawGrid(Gdiplus::Graphics& g, int W, int H);     // 半透明网格
void   DrawOverlay(Gdiplus::Graphics& g);                // 选中框/手柄/锚点/预览（屏幕坐标）
bool   ShowAnchors();                                    // 是否需要显示四边连接锚点
void   DrawChrome(Gdiplus::Graphics& g, int W);          // 顶部栏目 + 功能区底板与组名
void   MakeToolbar(HWND hwnd);                           // 创建自绘按钮并记下分组范围
void   RefreshButtons();                                 // 重绘所有工具栏按钮
bool   IsBtnChecked(int id);                             // 按钮是否处于选中态
const BtnDef* FindBtn(int id);

// ============================ io.cpp ============================
string W2U8(const wstring& w);
wstring U82W(const string& s);
bool   ReadAll(const wstring& path, string& data);
bool   WriteAll(const wstring& path, const string& data);
bool   PickFile(HWND hwnd, wstring& out, bool save,
                const wchar_t* filter, const wchar_t* defExt);
bool   HasEdraftExt(const wstring& p);
wstring EnsureEdraftExt(const wstring& p);

bool   SaveDoc(const wstring& path);
bool   LoadDoc(const wstring& path);
void   InsertImage(HWND hwnd);
void   ExportPNG(HWND hwnd);
int    EncoderClsid(const WCHAR* mime, CLSID* clsid);
void   DoPaste();                                        // Ctrl+V：文字→文本框，图片→图片

// ============================ ui.cpp ============================
void   AutoGrow(Obj* o);                                 // 文本框按内容自动增高
void   BeginEdit(int id);                                // 进入就地编辑
void   CommitEdit();                                     // 结束就地编辑并写回
void   MarkTool(Tool t);                                 // 切换工具并刷新按钮
void   UpdateTitle();                                    // 标题栏显示文件名与未保存标记
void   ShowContextMenu(HWND hwnd, int sx, int sy);       // 右键菜单
void   HandleMenu(int id);                               // 处理菜单命令
LRESULT CALLBACK InputProc(HWND h, UINT m, WPARAM w, LPARAM l);   // 自定义数值输入框
