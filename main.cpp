// TQ · 天球仪 —— 宿主演示程序
// Win32 API + 自建天球控件 + SQLite + WinHTTP 在线星表
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <cmath>

using std::min;
using std::max;
#include <objbase.h>      // GDI+ 头依赖 IStream / PROPID / byte 等 OLE 类型
#include <gdiplus.h>
using namespace Gdiplus;

#include "tq_util.h"
#include "star_catalog.h"
#include "catalog_download.h"
#include "celestial_sphere.h"
#include "city_db.h"

using namespace tq;

// ------------------------------------------------------------------ ID
#define IDI_TQ_APP       101   // tq.rc 里编译进去的应用图标
#define IDC_SPHERE       1000
#define IDC_SEARCH       100
#define IDC_CITY         107
#define IDC_TIME         108
#define IDC_BTN_RESET    106
#define IDC_DIR          111   // 观测方向输入框（8 方位）
#define IDC_LABEL_STAR   112   // “星名：”标签，紧邻星名输入框
#define IDC_LABEL_CITY   113   // “地点：”标签，紧邻城市输入框
#define IDC_LABEL_TIME   114   // “时间：”标签，紧邻时间输入框
#define IDC_LABEL_DIR    115   // “方向：”标签，紧邻方向输入框
#define IDC_TG_GRID      200
#define IDC_TG_EQUATOR   201
#define IDC_TG_ECLIPTIC  202
#define IDC_TG_CONST     203
#define IDC_TG_LABEL     204
#define IDC_TG_DEEP      205
#define IDC_TG_GALACTIC  206
#define IDC_TG_CONSTNAME 207
#define IDC_TG_CROSS     208
#define IDC_TG_AA        209
#define IDC_TG_INSIDE    210
#define IDC_TG_OUTSIDE   211
#define IDC_TG_SPIN      212   // 天球自转/停止开关
#define IDC_TG_BORDER    213   // 星座边界开关（菜单/快捷键用，不占工具栏）
#define IDC_TG_MOON      214   // 月亮显示开关
#define IDC_TG_PLANETS   215   // 各大行星显示开关
#define IDC_TG_ZODIAC    216   // 星座贴图只看黄道十二宫
#define IDC_SPIN_DEC     251   // 自转倍率 -60
#define IDC_SPIN_INC     252   // 自转倍率 +60
#define IDC_SPIN_LABEL   253   // 自转倍率显示（默认 ×1）
#define IDC_MAG3         300   // 极限星等预设（ID 段需连续，供区间判断用）
#define IDC_MAG45        301
#define IDC_MAG65        302
#define IDC_MAG85        303

#define IDM_IMPORT       4001
#define IDM_EXIT         4002
#define IDM_FETCH        4003
#define IDM_RESEED       4004
#define IDM_CLEAR        4005
#define IDM_OPENDIR      4006
#define IDM_SHOW_ONLINE  4007   // 右键菜单：切换是否显示在线补充星表
#define IDM_RESET        4100
#define IDM_GRID         4101
#define IDM_EQUATOR      4102
#define IDM_ECLIPTIC     4103
#define IDM_CONST        4104
#define IDM_LABEL        4105
#define IDM_DEEP         4106
#define IDM_GALACTIC     4107
#define IDM_CONSTNAME    4108
#define IDM_BORDER       4109
#define IDM_NCP          4110
#define IDM_SCP          4111
#define IDM_ORION        4112
#define IDM_GALCENTER    4113
#define IDM_SUMMER       4114
#define IDM_PLANETS      4115
#define IDM_INSIDE_ON    4116
#define IDM_INSIDE_OFF   4117
#define IDM_ART          4118   // 星座贴图（填充版）开关
#define IDM_ZODIAC       4120   // 只看黄道十二宫贴图
#define IDM_ABOUT        4200
#define IDM_KEYS         4201

#define WM_APP_FETCH_DONE  (WM_APP + 0x50)
#define WM_APP_HIDE_CITYLIST (WM_APP + 0x51)   // 延迟判断焦点后再收起候选列表
#define WM_APP_CITY_SELECTALL (WM_APP + 0x52)  // 延迟全选输入框内容
#define WM_APP_HIDE_STARLIST (WM_APP + 0x53)   // 延迟判断焦点后再收起星名候选列表
#define WM_APP_STAR_SELECTALL (WM_APP + 0x54)  // 延迟全选星名输入框
#define WM_APP_HIDE_DIRLIST  (WM_APP + 0x55)  // 延迟判断焦点后再收起方向候选列表

// ------------------------------------------------------------------ 全局
static HWND           g_hwnd = nullptr;
static HINSTANCE      g_hInst = nullptr;
static std::wstring   g_exeDir;
// 堆分配且进程存续期间不释放：在线抓取在后台线程使用，退出时无需等待网络超时
static StarCatalog*   g_catalog = new StarCatalog();
static long long      g_totalStars = 0;    // 数据库总星数
static long long      g_shownStars = 0;    // 当前实际载入显示的星数
static bool           g_showOnline = false; // 默认仅显示内置星表
static CelestialSphere g_sphere;
static std::wstring   g_status = L"就绪";
static std::wstring   g_detailTitle = L"天体信息";
static std::vector<std::pair<std::wstring, std::wstring>> g_detail;
// 星名查找候选（EDIT + LISTBOX），仿城市输入：拼音首字母 / 英文名前缀 / 汉字包含
struct StarHit {
    std::wstring label;        // 列表显示文本（中文名 + 英文名）
    std::wstring displayName;  // 显示名（中文名优先）
    std::wstring name;         // 库内名称（Highlight 定位用）
    std::wstring nameEn;
    std::string  py;           // 拼音音节首字母
    std::string  en;           // 英文名折叠（小写字母/数字）
};
static std::vector<StarHit> g_starHits;    // 当前星表的全部查找候选
static std::vector<int>     g_starShown;   // 下拉项 -> g_starHits 下标
static bool                 g_starUpdating = false;
static HWND                 g_starList = nullptr;   // 候选列表浮层（按句柄访问）
static WNDPROC              g_oldStarEditProc = nullptr;
static WNDPROC              g_oldTimeEditProc = nullptr;
static WNDPROC              g_oldStarListProc = nullptr;
static WNDPROC              g_oldCityListProc = nullptr;
static WNDPROC              g_oldDirListProc  = nullptr;
static bool                 g_popupPicking = false;
static std::thread    g_worker;
static std::atomic<bool> g_busy{ false };
static HFONT          g_uiFont = nullptr;
static HBRUSH         g_brBar = nullptr, g_brPanel = nullptr, g_brStatus = nullptr;
static HBRUSH         g_brOn = nullptr, g_brOff = nullptr, g_brEdit = nullptr;
static HBRUSH         g_brTitle = nullptr, g_brHot = nullptr, g_brClose = nullptr;
// ---------------------------------------------------------------- DPI
// 进程按系统 DPI 感知（启动时在 wWinMain 里开启）：窗口直接按物理像素绘制，
// 1px 描边就是 1 个物理像素，不再被系统整窗拉伸糊掉。
// 布局仍按 96dpi 的逻辑坐标书写，统一经 S() 换算成物理像素。
static double g_dpiScale = 1.0;
static inline int S(int v) { return (int)(v * g_dpiScale + 0.5); }
static inline REAL SF(double v) { return (REAL)(v * g_dpiScale); }

static const int      kTitleH = 32, kToolH = 76, kStatusH = 28;   // 逻辑高度
static int            g_toolH = 76, g_statusH = 28;
// 自绘标题栏：与菜单栏合并成一条，去掉系统白底标题栏
static int            g_titleH = 32;
static HMENU          g_mainMenu = nullptr;   // 只用于取子菜单弹 TrackPopupMenu，不挂到窗口上
static int            g_capHot = 0;           // 标题栏热区：1/2/3 最小化/最大化/关闭，4..7 菜单项，0 无
static int            g_menuOpen = -1;        // 当前展开的菜单下标（高亮用）
static bool           g_titleTracking = false; // 已注册 WM_MOUSELEAVE
static HWND           g_info = nullptr;    // 右下角浮层信息卡（子窗口，默认隐藏）
static int            g_infoW = 372;
static void ShowInfoCard();
static void HideInfoCard();
static int            g_activeMag = IDC_MAG65;

// ------------------------------------------------------------------ 城市经纬度库
// 数据来自 SQLite 数据库 tq_cities.db（见 city_db.h / city_db.cpp），
// 由 "中国行政区划经纬度_扁平版 副本.csv" 导入，取质心经纬度。
// 输入拼音首字母（如 bj）时下拉筛出汉字市县名，选择后取其经纬度。

static void SetStatus(const std::wstring& s);   // 前置声明（定义在下方）

static std::vector<citydb::CityRecord> g_cities;      // 全部市县（按名称排序）
static std::vector<std::wstring>       g_cityLabels;  // 下拉显示文本（重名附加上级行政区）
static std::vector<int>                g_cityShown;   // 当前下拉项 -> g_cities 下标
static bool                            g_cityUpdating = false;

static void BuildCityLabels() {
    g_cityLabels.resize(g_cities.size());
    std::map<std::wstring, int> cnt;
    for (const auto& c : g_cities) cnt[c.name]++;
    for (size_t i = 0; i < g_cities.size(); ++i) {
        const auto& c = g_cities[i];
        if (cnt[c.name] > 1 && !c.parent.empty())
            g_cityLabels[i] = c.parent + L" · " + c.name;
        else
            g_cityLabels[i] = c.name;
    }
}

static bool IsAsciiLetters(const std::wstring& s) {
    if (s.empty()) return false;
    for (wchar_t ch : s)
        if (!((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z'))) return false;
    return true;
}

static std::string NarrowLower(const std::wstring& s) {
    std::string r;
    for (wchar_t ch : s) {
        if (ch >= L'A' && ch <= L'Z') r += (char)(ch - L'A' + 'a');
        else if (ch < 128) r += (char)ch;
        else r += '\x7f';
    }
    return r;
}

static bool StartsWithPy(const std::string& py, const std::string& prefix) {
    return py.size() >= prefix.size() && py.compare(0, prefix.size(), prefix) == 0;
}

// 三个候选列表都是 WS_POPUP 顶层窗口：用输入框的屏幕矩形定位到其正下方，
// 并置于 HWND_TOP，避免被绘图区 / 工具栏按钮等子窗口遮挡。
// 每次激活都重新定位，主窗口移动后位置仍然正确。
// height / minWidth 是逻辑值，输入框矩形已是物理像素
static void PlacePopupList(HWND lst, HWND ed, int height, int minWidth = 0) {
    if (!lst || !ed) return;
    RECT er{}; GetWindowRect(ed, &er);
    int w = er.right - er.left;
    if (w < S(minWidth)) w = S(minWidth);      // 方向候选带角度，需要比输入框更宽
    SetWindowPos(lst, HWND_TOP, er.left, er.bottom, w, S(height), SWP_NOACTIVATE);
}

// ------------------------------------------------------------------ 输入框悬停提示
// 用系统 ToolTip 控件，TTF_SUBCLASS 让它自动接管鼠标消息，
// 与已有的 EDIT 子类化（StarEditProc / CityEditProc / DirEditProc / TimeEditProc）按链式叠加。
static void CreateInputTooltips(HWND parent, HWND edStar, HWND edCity,
                                HWND edTime, HWND edDir) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                parent, nullptr, g_hInst, nullptr);
    if (!tip) return;
    struct TipItem { HWND h; const wchar_t* text; };
    const TipItem items[] = {
        { edStar, L"输入星名" },
        { edCity, L"输入城市" },
        { edTime, L"输入时间" },
        { edDir,  L"输入方位" },
    };
    for (const auto& it : items) {
        if (!it.h) continue;
        TOOLINFOW ti = {};
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = parent;
        ti.uId = (UINT_PTR)it.h;
        ti.lpszText = const_cast<wchar_t*>(it.text);
        // 未启用 v6 公共控件时旧版 comctl32 只接受不含 lParam 的结构尺寸，
        // ADDTOOL 会静默失败（TTM_GETTOOLCOUNT 为 0），故准备两种尺寸。
        ti.cbSize = sizeof(TOOLINFOW);
        if (!SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti)) {
            ti.cbSize = TTTOOLINFOW_V1_SIZE;
            SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
    }
    SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, (LPARAM)240);
    SendMessageW(tip, TTM_SETDELAYTIME, (WPARAM)TTDT_INITIAL, (LPARAM)MAKELONG(300, 0));
}

// 英文名折叠：只保留小写字母与数字（用于英文名前缀检索）
static std::string FoldEnKey(const std::wstring& s) {
    std::string r;
    for (wchar_t c : s) {
        if (c >= L'A' && c <= L'Z') r += (char)(c - L'A' + 'a');
        else if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) r += (char)c;
    }
    return r;
}

static bool IsExactCityLabel(const std::wstring& s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < g_cities.size(); ++i)
        if (g_cityLabels[i] == s || g_cities[i].name == s) return true;
    return false;
}

// 读取城市输入框（EDIT）的文本。输入与候选列表分离，不会被自动补全改写。
static std::wstring GetCityEditText() {
    HWND ed = GetDlgItem(g_hwnd, IDC_CITY);
    if (!ed) return L"";
    wchar_t buf[128] = { 0 };
    GetWindowTextW(ed, buf, 128);
    return buf;
}

static void ApplyObserverFromUI();              // 前置声明
static void PickCityFromList(int sel);          // 前置声明
static int  g_cityPicked = -1;                  // 最近从候选列表选中的市县下标
static HWND g_cityList = nullptr;              // 候选列表（WS_POPUP 浮层，按句柄访问）
static WNDPROC g_oldCityEditProc = nullptr;

// ------------------------------------------------------------------ 观测方向
// 方位角 az：自正北起向东量（0=北、90=东、180=南、270=西），与
// HorizonToEquatorial 的 azDeg 定义一致；仰角固定 35°，是另一个参数。
struct DirOpt { const wchar_t* name; double az; const char* py; };
static const DirOpt kDirections[] = {
    { L"北",   0.0,   "b"  },
    { L"东北", 45.0,  "db" },
    { L"东",   90.0,  "d"  },
    { L"东南", 135.0, "dn" },
    { L"南",   180.0, "n"  },
    { L"西南", 225.0, "xn" },
    { L"西",   270.0, "x"  },
    { L"西北", 315.0, "xb" },
};
static const int kDirCount = sizeof(kDirections) / sizeof(kDirections[0]);
static HWND     g_dirList = nullptr;
static WNDPROC  g_oldDirEditProc = nullptr;
static std::vector<int> g_dirShown;
static bool     g_dirUpdating = false;
static double   g_dirAz = 180.0;       // 最近一次有效方位角（输入中间态时沿用）
static bool     g_timeUpdating = false; // 自转时程序化更新时间框，抑制 EN_CHANGE 回灌

// 方向框文本 -> 方位角；未匹配（正在输入拼音等）时沿用上次有效值。
static double DirectionAzimuth() {
    wchar_t buf[32] = {};
    GetDlgItemTextW(g_hwnd, IDC_DIR, buf, 32);
    std::wstring s(buf);
    for (int i = 0; i < kDirCount; ++i)
        if (s == kDirections[i].name) { g_dirAz = kDirections[i].az; return g_dirAz; }
    return g_dirAz;
}

// 拖动 / 定位 / 自转改变了视线方位后，把「方向」框回填成最接近的八方位。
// 留 2.5° 回滞（>20° 才换名字）：正好落在两个方位中间时不动文本，避免来回跳。
static void SyncDirectionFromView(double az) {
    if (!std::isfinite(az)) return;                // 观测者尚未生效时视图方位无意义
    HWND ed = GetDlgItem(g_hwnd, IDC_DIR);
    if (!ed || GetFocus() == ed) return;          // 用户正在输入时不打断
    int best = -1;
    double bd = 1e9;
    for (int i = 0; i < kDirCount; ++i) {
        double d = std::fabs(az - kDirections[i].az);
        if (d > 180.0) d = 360.0 - d;
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0 || bd > 20.0) return;
    wchar_t cur[32] = {};
    GetDlgItemTextW(g_hwnd, IDC_DIR, cur, 32);
    if (std::wstring(cur) == kDirections[best].name) { g_dirAz = az; return; }
    g_dirUpdating = true;
    SetWindowTextW(ed, kDirections[best].name);
    g_dirUpdating = false;
    g_dirAz = az;
}

static void RefreshDirList(const std::wstring& key, bool show) {
    HWND lst = g_dirList;
    if (!lst) return;
    g_dirUpdating = true;
    SendMessageW(lst, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lst, LB_RESETCONTENT, 0, 0);
    g_dirShown.clear();
    const std::string py = NarrowLower(key);
    const bool alpha = IsAsciiLetters(key);
    for (int i = 0; i < kDirCount; ++i) {
        bool match = false;
        if (key.empty()) match = true;
        else if (alpha) match = StartsWithPy(kDirections[i].py, py);
        else match = StartsWithPy(kDirections[i].py, py)
                  || std::wstring(kDirections[i].name).find(key) != std::wstring::npos;
        if (match) {
            wchar_t label[64];
            swprintf_s(label, L"%ls  %.0f°", kDirections[i].name, kDirections[i].az);
            SendMessageW(lst, LB_ADDSTRING, 0, (LPARAM)label);
            g_dirShown.push_back(i);
        }
    }
    SendMessageW(lst, WM_SETREDRAW, TRUE, 0);
    SendMessageW(lst, LB_SETCURSEL, (WPARAM)-1, 0);
    if (show && !g_dirShown.empty()) {
        ShowWindow(g_starList, SW_HIDE); ShowWindow(g_cityList, SW_HIDE);
        ShowWindow(lst, SW_SHOWNA);
        PlacePopupList(lst, GetDlgItem(g_hwnd, IDC_DIR), 160, 110);
    } else {
        ShowWindow(lst, SW_HIDE);
    }
    g_dirUpdating = false;
}

static void PickDirFromList(int sel) {
    if (sel < 0 || sel >= (int)g_dirShown.size()) return;
    const DirOpt& d = kDirections[g_dirShown[sel]];
    g_dirAz = d.az;
    g_dirUpdating = true;
    SetWindowTextW(GetDlgItem(g_hwnd, IDC_DIR), d.name);
    g_dirUpdating = false;
    ShowWindow(g_dirList, SW_HIDE);
    SetForegroundWindow(g_hwnd);
    SetFocus(GetDlgItem(g_hwnd, IDC_DIR));
    ApplyObserverFromUI();
}

// 点击弹出全部方向；上下键移动、回车选中、Esc 收起（与城市输入框一致）。
static LRESULT CALLBACK DirEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_LBUTTONUP) {
        LRESULT r = CallWindowProcW(g_oldDirEditProc, hwnd, msg, wp, lp);
        SendMessageW(hwnd, EM_SETSEL, 0, (LPARAM)-1);
        if (g_dirList && !IsWindowVisible(g_dirList)) RefreshDirList(L"", true);
        return r;
    }
    if (msg == WM_KEYDOWN) {
        HWND lst = g_dirList;
        if (lst && IsWindowVisible(lst)) {
            int n   = (int)SendMessageW(lst, LB_GETCOUNT, 0, 0);
            int cur = (int)SendMessageW(lst, LB_GETCURSEL, 0, 0);
            if (wp == VK_DOWN && n > 0) {
                SendMessageW(lst, LB_SETCURSEL, (WPARAM)min(cur + 1, n - 1), 0); return 0;
            }
            if (wp == VK_UP && n > 0) {
                SendMessageW(lst, LB_SETCURSEL, (WPARAM)max(cur - 1, 0), 0); return 0;
            }
            if (wp == VK_RETURN) { PickDirFromList(cur >= 0 ? cur : 0); return 0; }
            if (wp == VK_ESCAPE) { ShowWindow(lst, SW_HIDE); return 0; }
        }
    }
    return CallWindowProcW(g_oldDirEditProc, hwnd, msg, wp, lp);
}

// 输入框键盘处理：上下键移动候选高亮、回车选中、Esc 收起。
static LRESULT CALLBACK CityEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_LBUTTONUP) {
        LRESULT r = CallWindowProcW(g_oldCityEditProc, hwnd, msg, wp, lp);
        SendMessageW(hwnd, EM_SETSEL, 0, (LPARAM)-1);   // 鼠标点击结束后全选内容
        return r;
    }
    if (msg == WM_KEYDOWN) {
        HWND lst = g_cityList;
        if (lst && IsWindowVisible(lst)) {
            int n   = (int)SendMessageW(lst, LB_GETCOUNT, 0, 0);
            int cur = (int)SendMessageW(lst, LB_GETCURSEL, 0, 0);
            if (wp == VK_DOWN && n > 0) {
                SendMessageW(lst, LB_SETCURSEL, (WPARAM)min(cur + 1, n - 1), 0);
                return 0;
            }
            if (wp == VK_UP && n > 0) {
                SendMessageW(lst, LB_SETCURSEL, (WPARAM)max(cur - 1, 0), 0);
                return 0;
            }
            if (wp == VK_RETURN && cur >= 0) {
                PickCityFromList(cur);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                ShowWindow(lst, SW_HIDE);
                return 0;
            }
        }
    }
    return CallWindowProcW(g_oldCityEditProc, hwnd, msg, wp, lp);
}

// 按输入词重建候选：拼音首字母按前缀匹配，汉字按名称包含匹配。
// 候选写入独立 LISTBOX，不触碰输入框，因此连续键入不会被吞字符。
static void RefreshCityList(const std::wstring& key, bool show) {
    HWND lst = g_cityList;
    if (!lst || g_cities.empty()) return;
    g_cityUpdating = true;
    SendMessageW(lst, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lst, LB_RESETCONTENT, 0, 0);
    g_cityShown.clear();
    const std::string py = NarrowLower(key);
    const bool alpha = IsAsciiLetters(key);
    const size_t kMaxShown = 400;
    for (size_t i = 0; i < g_cities.size() && g_cityShown.size() < kMaxShown; ++i) {
        bool match = false;
        if (key.empty()) match = true;
        else if (alpha) match = StartsWithPy(g_cities[i].py, py);
        else match = StartsWithPy(g_cities[i].py, py)
                  || g_cityLabels[i].find(key) != std::wstring::npos;
        if (match) {
            SendMessageW(lst, LB_ADDSTRING, 0, (LPARAM)g_cityLabels[i].c_str());
            g_cityShown.push_back((int)i);
        }
    }
    SendMessageW(lst, WM_SETREDRAW, TRUE, 0);
    SendMessageW(lst, LB_SETCURSEL, (WPARAM)-1, 0);
    if (show && !g_cityShown.empty()) {
        ShowWindow(g_starList, SW_HIDE); ShowWindow(g_dirList, SW_HIDE);
        ShowWindow(lst, SW_SHOWNA);
        PlacePopupList(lst, GetDlgItem(g_hwnd, IDC_CITY), 160);
    } else {
        ShowWindow(lst, SW_HIDE);
    }
    g_cityUpdating = false;
}

// 选中候选项：回填汉字名称、收起候选、应用经纬度。
static void PickCityFromList(int sel) {
    if (sel < 0 || sel >= (int)g_cityShown.size()) return;
    int idx = g_cityShown[sel];
    g_cityPicked = idx;
    g_cityUpdating = true;
    SetWindowTextW(GetDlgItem(g_hwnd, IDC_CITY), g_cityLabels[idx].c_str());
    g_cityUpdating = false;
    ShowWindow(g_cityList, SW_HIDE);
    SetForegroundWindow(g_hwnd);
    SetFocus(GetDlgItem(g_hwnd, IDC_CITY));
    ApplyObserverFromUI();
}

// 根据城市下拉框与时间输入框，设置观测者并定向到正南天空
static bool IsCompleteTimeText(const std::wstring& s) {
    if (s.find(L':') == std::wstring::npos) return false;
    int hh = -1, mm = -1;
    if (swscanf_s(s.c_str(), L"%d:%d", &hh, &mm) != 2) return false;
    return hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59;
}

static void ApplyObserverFromUI() {
    // 经纬度沿用当前观测者：城市框内容未匹配时也不能吞掉时间变更。
    SphereObserver cur = g_sphere.GetObserver();
    double lat = cur.lat, lon = cur.lon; bool found = false;
    std::wstring picked;
    if (g_cityPicked >= 0 && g_cityPicked < (int)g_cities.size()) {
        const auto& c = g_cities[g_cityPicked];        // 按所选候选精确取经纬度
        lat = c.lat; lon = c.lon; picked = c.name; found = true;
        g_cityPicked = -1;
    } else {
        std::wstring s = GetCityEditText();
        if (!s.empty()) {
            for (size_t i = 0; i < g_cities.size(); ++i) {
                if (g_cityLabels[i] == s || g_cities[i].name == s) {
                    lat = g_cities[i].lat; lon = g_cities[i].lon;
                    picked = g_cities[i].name; found = true; break;
                }
            }
            if (!found && citydb::Lookup(s, lat, lon)) { picked = s; found = true; }
        }
    }
    wchar_t tb[16]; GetDlgItemTextW(g_hwnd, IDC_TIME, tb, 16);
    int hh = 20, mm = 0;
    if (swscanf_s(tb, L"%d:%d", &hh, &mm) >= 1) {
        if (hh < 0) hh = 0; if (hh > 23) hh = 23;
        if (mm < 0) mm = 0; if (mm > 59) mm = 59;
    } else { hh = 20; mm = 0; }
    SYSTEMTIME st; GetLocalTime(&st);
    tq::SphereObserver o;
    o.lat = lat; o.lon = lon; o.year = st.wYear; o.month = st.wMonth; o.day = st.wDay;
    o.hour = hh + mm / 60.0;
    o.azDeg = DirectionAzimuth();
    g_sphere.SetObserver(o);
    wchar_t msg[160];
    if (found)
        swprintf_s(msg, L"观测地：%s  %.3f°N  %.3f°E  ·  本地时间 %02d:%02d",
                   picked.c_str(), lat, lon, hh, mm);
    else
        swprintf_s(msg, L"本地时间 %02d:%02d（观测地沿用上次设置）", hh, mm);
    SetStatus(msg);
}

// 时间框回车立即应用（EN_CHANGE 只认完整 HH:MM，回车也接受 "21" 这类简写）
static LRESULT CALLBACK TimeEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        ApplyObserverFromUI();
        return 0;
    }
    return CallWindowProcW(g_oldTimeEditProc, hwnd, msg, wp, lp);
}

// 复位：恢复初始观测地 / 时间（北京市、20:00），重置视角后按该状态重新定向。
static void ResetViewToInitial() {
    g_cityPicked = -1;
    g_dirAz = 180.0;
    g_dirUpdating = true;                 // 抑制回填文本触发候选下拉
    SetWindowTextW(GetDlgItem(g_hwnd, IDC_DIR),  L"南");
    g_dirUpdating = false;
    SetWindowTextW(GetDlgItem(g_hwnd, IDC_CITY), L"北京市");
    SetWindowTextW(GetDlgItem(g_hwnd, IDC_TIME), L"20:00");
    g_sphere.Reset();            // 球心视角 1x、清平移/选中，并按当前观测者定向
    ApplyObserverFromUI();      // 确保 obs* 为北京市 + 系统当天 20:00
}

static const int kToggleIds[] = {
    IDC_TG_GRID, IDC_TG_EQUATOR, IDC_TG_ECLIPTIC, IDC_TG_CONST, IDC_TG_LABEL,
    IDC_TG_DEEP, IDC_TG_GALACTIC, IDC_TG_CONSTNAME, IDC_TG_CROSS, IDC_TG_AA,
    IDC_TG_MOON, IDC_TG_PLANETS, IDC_TG_SPIN, IDC_TG_ZODIAC
};
// 开关 ID 不连续（212/214/215 与 200–209 掺杂），统一查表判断，避免再写死区间上限。
static bool InToggleList(int id) {
    for (int t : kToggleIds) if (t == id) return true;
    return false;
}

// 天球自转倍率：×1 为真实速度；此后每档 +60，即 ×1 → ×60 → ×120 → ×180 …
// 除 ×1 外所有档位都是 60 的整数倍（旧实现从 ×1 直接累加，会出现 ×61 ×121 这种尾巴）
static int g_spinMult = 1;

// ------------------------------------------------------------------ 小工具
static void SetStatus(const std::wstring& s) {
    g_status = s;
    if (g_hwnd) {
        RECT rc; GetClientRect(g_hwnd, &rc);
        RECT r = { 0, rc.bottom - g_statusH, rc.right, rc.bottom };
        InvalidateRect(g_hwnd, &r, FALSE);
    }
}

// 参数是 96dpi 逻辑坐标，内部换算成物理像素
static void Place(int id, int x, int y, int w, int h) {
    HWND c = GetDlgItem(g_hwnd, id);
    if (c) MoveWindow(c, S(x), S(y), S(w), S(h), TRUE);
}

static HWND MakeButton(int id, const wchar_t* text, bool toggle) {
    return CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_OWNERDRAW,
        0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

static void ReloadCatalog(double magLimit = 8.5) {
    CatalogData d;
    std::wstring err;
    if (g_catalog->Reload(d, magLimit, &err)) {
        // 默认只显示内置星表；勾选「显示在线星表」后才加入在线补充数据。
        // Reload 内部已 Prepare（含中文名按 HIP 推导），过滤不会破坏这些结果。
        if (!g_showOnline) {
            d.stars.erase(
                std::remove_if(d.stars.begin(), d.stars.end(),
                    [](const Star& s) { return s.source != "builtin"; }),
                d.stars.end());
        }
        g_shownStars = (long long)d.stars.size();
        // 重建星名查找候选（供 EDIT+LISTBOX 拼音 / 英文 / 汉字检索）
        g_starHits.clear();
        g_starHits.reserve(d.stars.size());
        for (const auto& s : d.stars) {
            StarHit hh;
            hh.displayName = s.nameCn.empty() ? s.name : s.nameCn;
            hh.name = s.name;
            hh.nameEn = s.nameEn;
            hh.label = hh.displayName;
            if (!s.nameEn.empty() && s.nameEn != hh.displayName) hh.label += L"  " + s.nameEn;
            hh.py = s.py;
            hh.en = FoldEnKey(s.nameEn);
            g_starHits.push_back(std::move(hh));
        }
        g_sphere.SetData(d);
        g_totalStars = g_catalog->StarCount();
        wchar_t b[160];
        swprintf_s(b, L"数据库载入 %d 颗恒星、%d 组星座连线、%d 个深空天体、%d 套星座贴图",
                   (int)d.stars.size(), (int)d.lines.size(), (int)d.deepsky.size(),
                   (int)d.art.size());
        SetStatus(b);
    } else SetStatus(L"载入失败：" + err);
}

static bool IsToggleOn(int id) {
    SphereOptions o = g_sphere.GetOptions();
    switch (id) {
    case IDC_TG_GRID:      return o.showGrid;
    case IDC_TG_EQUATOR:   return o.showEquator;
    case IDC_TG_ECLIPTIC:  return o.showEcliptic;
    case IDC_TG_CONST:     return o.showConstellations;
    case IDC_TG_BORDER:    return o.showBorders;
    case IDC_TG_LABEL:     return o.showLabels;
    case IDC_TG_DEEP:      return o.showDeepSky;
    case IDC_TG_GALACTIC:  return o.showGalactic;
    case IDC_TG_CONSTNAME: return o.showConstellationNames;
    case IDC_TG_CROSS:     return o.showCrosshair;
    case IDC_TG_AA:        return o.antialias;
    case IDC_TG_MOON:      return o.showMoon;
    case IDC_TG_PLANETS:   return o.showPlanets;
    case IDC_TG_ZODIAC:    return o.zodiacOnly;
    default: return false;
    }
}

static void RefreshToggles() {
    for (int id : kToggleIds) {
        HWND h = GetDlgItem(g_hwnd, id);
        if (h) InvalidateRect(h, nullptr, FALSE);
    }
    for (int id = IDC_MAG3; id <= IDC_MAG85; ++id) {
        HWND h = GetDlgItem(g_hwnd, id);
        if (h) InvalidateRect(h, nullptr, FALSE);
    }
}

static void RefreshViewButtons() {
    for (int id : { IDC_TG_INSIDE, IDC_TG_OUTSIDE }) {
        HWND b = GetDlgItem(g_hwnd, id);
        if (b) InvalidateRect(b, nullptr, FALSE);
    }
}

static void ToggleOption(int id) {
    SphereOptions o = g_sphere.GetOptions();
    switch (id) {
    case IDC_TG_GRID:      o.showGrid = !o.showGrid; break;
    case IDC_TG_EQUATOR:   o.showEquator = !o.showEquator; break;
    case IDC_TG_ECLIPTIC:  o.showEcliptic = !o.showEcliptic; break;
    case IDC_TG_CONST:     o.showConstellations = !o.showConstellations; break;
    case IDC_TG_BORDER:    o.showBorders = !o.showBorders; break;
    case IDC_TG_LABEL:     o.showLabels = !o.showLabels; break;
    case IDC_TG_DEEP:      o.showDeepSky = !o.showDeepSky; break;
    case IDC_TG_GALACTIC:  o.showGalactic = !o.showGalactic; break;
    case IDC_TG_CONSTNAME: o.showConstellationNames = !o.showConstellationNames; break;
    case IDC_TG_CROSS:     o.showCrosshair = !o.showCrosshair; break;
    case IDC_TG_AA:        o.antialias = !o.antialias; break;
    case IDC_TG_MOON:      o.showMoon = !o.showMoon; break;
    case IDC_TG_PLANETS:   o.showPlanets = !o.showPlanets; break;
    case IDC_TG_ZODIAC:
        o.zodiacOnly = !o.zodiacOnly;
        // 打开十二宫过滤时若贴图还关着，顺手打开，否则点完看不到任何变化
        if (o.zodiacOnly) o.showArt = true;
        break;
    case IDC_MAG3:  o.limitingMag = 3.0; g_activeMag = id; break;
    case IDC_MAG45: o.limitingMag = 4.5; g_activeMag = id; break;
    case IDC_MAG65: o.limitingMag = 6.5; g_activeMag = id; break;
    case IDC_MAG85: o.limitingMag = 8.5; g_activeMag = id; break;
    default: return;
    }
    g_sphere.SetOptions(o);
    RefreshToggles();
}

// 信息卡里不再显示的项（标签完全匹配即跳过）：名称/十进制坐标/星座与其它行重复，
// 拼音、传统星名、中文通用名、拜耳·弗氏命名、是否在 HYG 星表中对普通用户价值不大
static bool InfoItemHidden(const std::wstring& label) {
    static const wchar_t* kHidden[] = {
        L"名称", L"十进制坐标", L"星座", L"传统星名", L"拼音",
        L"中文通用名", L"拜耳/弗氏命名", L"在HYG星表中",
    };
    for (const wchar_t* s : kHidden)
        if (label == s) return true;
    return false;
}

// 信息卡值列约 220px 宽（13.5px 字号）：汉字按 2 个单位、ASCII 按 1 个单位折行，
// 首行带标签，后续行标签留空；超过 maxLines 行就省略号截断（卡片高度有限）。
static void PushWrapped(const wchar_t* label, const std::wstring& val, int maxLines) {
    const int kUnits = 34;
    size_t i = 0;
    int line = 0;
    while (i < val.size() && line < maxLines) {
        size_t j = i;
        int units = 0;
        while (j < val.size() && units + (val[j] >= 0x2E80 ? 2 : 1) <= kUnits) {
            units += (val[j] >= 0x2E80 ? 2 : 1);
            ++j;
        }
        std::wstring piece = val.substr(i, j - i);
        const bool last = (j >= val.size());
        if (!last && line + 1 >= maxLines) piece += L"…";   // 后面还有内容但行数用完了
        g_detail.push_back({ line == 0 ? std::wstring(label) : std::wstring(), piece });
        i = j;
        ++line;
    }
    if (line == 0) g_detail.push_back({ label, L"" });
}

static void ShowStarDetail(const SphereNotify& n) {
    if (n.starIndex == -1) {          // 点到空白天区：收起信息卡
        g_detail.clear();
        HideInfoCard();
        return;
    }
    g_detail.clear();
    if (n.starIndex == TQ_MOON_INDEX) {
        g_detailTitle = L"月亮";
        wchar_t rab[64], decb[64];
        FormatRA(n.ra, rab, 64); FormatDec(n.dec, decb, 64);
        g_detail.push_back({ L"赤经 (瞬时)", rab });
        g_detail.push_back({ L"赤纬 (瞬时)", decb });
        // 相位 / 照度 / 距离（来自 n.info：标签\t值\n）
        std::wstring extra(n.info);
        for (size_t pos = 0; pos < extra.size();) {
            size_t eol = extra.find(L'\n', pos);
            if (eol == std::wstring::npos) eol = extra.size();
            std::wstring line = extra.substr(pos, eol - pos);
            size_t tab = line.find(L'\t');
            if (tab != std::wstring::npos)
                g_detail.push_back({ line.substr(0, tab), line.substr(tab + 1) });
            pos = eol + 1;
        }
    } else if (n.starIndex <= TQ_PLANET_BASE &&
               n.starIndex > TQ_PLANET_BASE - CelestialSphere::kPlanetCount) {
        // 行星：位置由开普勒根数按观测时间实时推算，无 J2000 固定坐标
        g_detailTitle = n.name;
        wchar_t b[128];
        wchar_t rab[64], decb[64];
        FormatRA(n.ra, rab, 64); FormatDec(n.dec, decb, 64);
        g_detail.push_back({ L"英文名", n.nameEn });
        g_detail.push_back({ L"类型", L"太阳系行星" });
        g_detail.push_back({ L"赤经 (瞬时)", rab });
        g_detail.push_back({ L"赤纬 (瞬时)", decb });
        swprintf_s(b, L"%.2f", n.mag);
        g_detail.push_back({ L"视星等", b });
        // 日心距离 / 地心距离 / 距角（n.info：标签\t值\n）
        std::wstring extra(n.info);
        for (size_t pos = 0; pos < extra.size();) {
            size_t eol = extra.find(L'\n', pos);
            if (eol == std::wstring::npos) eol = extra.size();
            std::wstring line = extra.substr(pos, eol - pos);
            size_t tab = line.find(L'\t');
            if (tab != std::wstring::npos)
                g_detail.push_back({ line.substr(0, tab), line.substr(tab + 1) });
            pos = eol + 1;
        }
    } else if (n.starIndex <= TQ_CONST_BASE) {
        // 星座（贴图模式点中星座边界内）：资料按「标签\t值」逐行给出，
        // 传说/文化根源/出处是长文本，按行折行后再进卡片
        g_detailTitle = n.nameCn[0] ? n.nameCn : n.name;
        std::wstring extra(n.info);
        for (size_t pos = 0; pos < extra.size();) {
            size_t eol = extra.find(L'\n', pos);
            if (eol == std::wstring::npos) eol = extra.size();
            std::wstring ln = extra.substr(pos, eol - pos);
            size_t tab = ln.find(L'\t');
            if (tab != std::wstring::npos) {
                const std::wstring key = ln.substr(0, tab);
                const std::wstring val = ln.substr(tab + 1);
                if (!val.empty()) {
                    int lines = 1;
                    if (key == L"传说") lines = 6;
                    else if (key == L"文化根源" || key == L"出处") lines = 2;
                    PushWrapped(key.c_str(), val, lines);
                }
            }
            pos = eol + 1;
        }
    } else if (n.starIndex < 0) {
        g_detailTitle = L"天体信息";
        g_detail.push_back({ L"提示", L"将鼠标移到恒星上查看信息" });
        g_detail.push_back({ L"操作", L"单击可选中，双击可居中" });
        g_detail.push_back({ L"数据库", g_catalog->Path() });
    } else {
        g_detailTitle = n.name[0] ? n.name : n.nameEn;
        wchar_t b[128];
        wchar_t rab[64], decb[64];
        FormatRA(n.ra, rab, 64); FormatDec(n.dec, decb, 64);
        g_detail.push_back({ L"中文名", n.nameCn });   // 无中文名则留空
        g_detail.push_back({ L"英文名", n.nameEn });
        g_detail.push_back({ L"赤经 (J2000)", rab });
        g_detail.push_back({ L"赤纬 (J2000)", decb });
        swprintf_s(b, L"%.2f", n.mag);       g_detail.push_back({ L"视星等", b });
        g_detail.push_back({ L"光谱型", n.spectral });
        if (n.con[0]) g_detail.push_back({ L"所属星座", n.con });   // 星表未标注星座时不留空行
        if (n.hip) { swprintf_s(b, L"HIP %d", n.hip); g_detail.push_back({ L"编号", b }); }
        // 中国传统星名等附加数据（stars.info 列，格式：标签 \t 值 \n）
        std::wstring extra(n.info);
        for (size_t pos = 0; pos < extra.size();) {
            size_t eol = extra.find(L'\n', pos);
            if (eol == std::wstring::npos) eol = extra.size();
            std::wstring line = extra.substr(pos, eol - pos);
            size_t tab = line.find(L'\t');
            if (tab != std::wstring::npos) {
                std::wstring key = line.substr(0, tab);
                if (!InfoItemHidden(key))
                    g_detail.push_back({ key, line.substr(tab + 1) });
            }
            pos = eol + 1;
        }
    }
    ShowInfoCard();
}

// ------------------------------------------------------------------ 数据操作
static void DoFetch() {
    if (g_busy) { SetStatus(L"正在抓取，请稍候…"); return; }
    if (g_worker.joinable()) g_worker.join();
    g_busy = true;
    SetStatus(L"正在连接公开星表（VizieR / HYG）…");
    HWND hwnd = g_hwnd;
    g_worker = std::thread([hwnd] {
        auto sources = LoadSources(g_exeDir);
        if (sources.empty()) sources = DefaultSources();
        DownloadResult r;
        FetchAndImport(*g_catalog, sources, r);
        auto* msg = new std::wstring(r.message);
        PostMessageW(hwnd, WM_APP_FETCH_DONE, r.ok ? 1 : 0, (LPARAM)msg);
        g_busy = false;
    });
}

static void DoImportFile() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"星表文件 (*.csv;*.tsv;*.txt;*.dat)\0*.csv;*.tsv;*.txt;*.dat\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = L"选择星表 CSV / TSV 文件";
    if (!GetOpenFileNameW(&ofn)) return;
    int inserted = 0;
    std::wstring info;
    if (g_catalog->ImportFile(file, inserted, &info)) {
        ReloadCatalog();
        SetStatus(L"导入成功：" + info);
    } else SetStatus(L"导入失败：" + info);
}

// ------------------------------------------------------------------ 星名查找（EDIT + LISTBOX，拼音首字母）
static void PickStarFromList(int sel);
static void RefreshStarList(const std::wstring& key, bool show);

// 输入框键盘处理：上下键移动候选高亮、回车选中、Esc 收起。
static LRESULT CALLBACK StarEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_LBUTTONUP) {
        LRESULT r = CallWindowProcW(g_oldStarEditProc, hwnd, msg, wp, lp);
        SendMessageW(hwnd, EM_SETSEL, 0, (LPARAM)-1);   // 鼠标点击结束后全选内容
        return r;
    }
    if (msg == WM_KEYDOWN) {
        HWND lst = g_starList;
        if (lst && IsWindowVisible(lst)) {
            int n   = (int)SendMessageW(lst, LB_GETCOUNT, 0, 0);
            int cur = (int)SendMessageW(lst, LB_GETCURSEL, 0, 0);
            if (wp == VK_DOWN && n > 0) { SendMessageW(lst, LB_SETCURSEL, (WPARAM)std::min(cur + 1, n - 1), 0); return 0; }
            if (wp == VK_UP && n > 0)   { SendMessageW(lst, LB_SETCURSEL, (WPARAM)std::max(cur - 1, 0), 0); return 0; }
            if (wp == VK_RETURN) {
                // 候选弹出但未用上下键高亮时，回车直接选首条（LB_GETCURSEL 为 -1）。
                if (n > 0) PickStarFromList(cur >= 0 ? cur : 0);
                return 0;
            }
            if (wp == VK_ESCAPE) { ShowWindow(lst, SW_HIDE); return 0; }
        } else if (wp == VK_RETURN) {
            wchar_t buf[128] = {};                     // 列表未弹出时回车：重建候选并选中首条
            GetWindowTextW(hwnd, buf, 128);
            RefreshStarList(buf, true);
            if (!g_starShown.empty()) PickStarFromList(0);
            return 0;
        }
    }
    return CallWindowProcW(g_oldStarEditProc, hwnd, msg, wp, lp);
}

// 按输入词重建候选：拼音首字母 / 英文名前缀匹配，汉字按名称包含匹配。
static void RefreshStarList(const std::wstring& key, bool show) {
    HWND lst = g_starList;
    if (!lst || g_starHits.empty()) return;
    if (key.empty()) {
        g_starUpdating = true;
        SendMessageW(lst, LB_RESETCONTENT, 0, 0);
        SendMessageW(lst, WM_SETREDRAW, TRUE, 0);
        g_starShown.clear();
        ShowWindow(lst, SW_HIDE);
        g_starUpdating = false;
        return;
    }
    g_starUpdating = true;
    SendMessageW(lst, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lst, LB_RESETCONTENT, 0, 0);
    g_starShown.clear();
    const std::string py = NarrowLower(key);
    const bool alpha = IsAsciiLetters(key);
    const size_t kMaxShown = 400;
    for (size_t i = 0; i < g_starHits.size() && g_starShown.size() < kMaxShown; ++i) {
        bool match;
        if (alpha) match = StartsWithPy(g_starHits[i].py, py) || StartsWithPy(g_starHits[i].en, py);
        else       match = g_starHits[i].displayName.find(key) != std::wstring::npos
                        || g_starHits[i].label.find(key) != std::wstring::npos;
        if (match) {
            SendMessageW(lst, LB_ADDSTRING, 0, (LPARAM)g_starHits[i].label.c_str());
            g_starShown.push_back((int)i);
        }
    }
    SendMessageW(lst, WM_SETREDRAW, TRUE, 0);
    SendMessageW(lst, LB_SETCURSEL, (WPARAM)-1, 0);
    if (show && !g_starShown.empty()) {
        ShowWindow(g_cityList, SW_HIDE); ShowWindow(g_dirList, SW_HIDE);
        ShowWindow(lst, SW_SHOWNA);
        PlacePopupList(lst, GetDlgItem(g_hwnd, IDC_SEARCH), 160);
    } else {
        ShowWindow(lst, SW_HIDE);
    }
    g_starUpdating = false;
}

// 星图上选中天体后，把名称回填到左上角星名输入框。
// 与候选列表选中时同一口径（中文名优先）；必须抑制 EN_CHANGE，
// 否则回填本身会触发拼音检索，把候选下拉重新弹出来。
static void SyncSearchBox(const SphereNotify& n) {
    if (n.starIndex < 0) return;
    const wchar_t* disp = n.nameCn[0] ? n.nameCn : n.name;
    if (!disp[0]) return;
    HWND ed = GetDlgItem(g_hwnd, IDC_SEARCH);
    if (!ed) return;
    g_starUpdating = true;
    SetWindowTextW(ed, disp);
    g_starUpdating = false;
    if (g_starList) ShowWindow(g_starList, SW_HIDE);
}

// 选中候选项：回填名称、收起候选、高亮定位。
static void PickStarFromList(int sel) {
    if (sel < 0 || sel >= (int)g_starShown.size()) return;
    const StarHit& hh = g_starHits[g_starShown[sel]];
    g_starUpdating = true;
    SetWindowTextW(GetDlgItem(g_hwnd, IDC_SEARCH), hh.displayName.c_str());
    g_starUpdating = false;
    ShowWindow(g_starList, SW_HIDE);
    SetForegroundWindow(g_hwnd);
    SetFocus(GetDlgItem(g_hwnd, IDC_SEARCH));
    g_sphere.Highlight(hh.name);
    SetStatus(L"已定位：" + hh.displayName + (hh.nameEn.empty() ? L"" : L"  " + hh.nameEn));
}

// 顶层 POPUP 列表的 LBN_SELCHANGE 通知并不可靠（窗口未激活时可能延后甚至丢失），
// 表现为：鼠标能高亮候选项却选不中、列表也不收起。
// 这里直接在列表窗口里拦截鼠标抬起，按当前选中项完成选择，绕开通知路由。
static LRESULT CALLBACK PopupListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC old = (hwnd == g_starList) ? g_oldStarListProc :
                  (hwnd == g_cityList) ? g_oldCityListProc : g_oldDirListProc;
    if (!old) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_LBUTTONUP) {
        g_popupPicking = true;             // 抑制期间到达的 LBN_SELCHANGE，避免重复回填
        LRESULT r = CallWindowProcW(old, hwnd, msg, wp, lp);
        g_popupPicking = false;
        int sel = (int)SendMessageW(hwnd, LB_GETCURSEL, 0, 0);
        if (sel >= 0) {
            if (hwnd == g_starList)      PickStarFromList(sel);
            else if (hwnd == g_cityList) PickCityFromList(sel);
            else if (hwnd == g_dirList)  PickDirFromList(sel);
        }
        return r;
    }
    return CallWindowProcW(old, hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------ 菜单
static HMENU MakeMainMenu() {
    HMENU m = CreateMenu();

    HMENU f = CreatePopupMenu();
    AppendMenuW(f, MF_STRING, IDM_IMPORT,  L"导入本地星表 CSV / TSV…");
    AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(f, MF_STRING, IDM_OPENDIR, L"打开程序目录（sources.txt / 数据库）");
    AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(f, MF_STRING, IDM_EXIT,    L"退出");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)f, L"文件(&F)");

    HMENU d = CreatePopupMenu();
    AppendMenuW(d, MF_STRING, IDM_FETCH,  L"在线抓取公开星表(&U)");
    AppendMenuW(d, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(d, MF_STRING, IDM_RESEED, L"重建内置精选星表");
    AppendMenuW(d, MF_STRING, IDM_CLEAR,  L"清除在线导入的数据");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)d, L"数据(&D)");

    HMENU v = CreatePopupMenu();
    AppendMenuW(v, MF_STRING, IDM_GRID,      L"赤道网格\tG");
    AppendMenuW(v, MF_STRING, IDM_EQUATOR,   L"天赤道\tQ");
    AppendMenuW(v, MF_STRING, IDM_ECLIPTIC,  L"黄道\tE");
    AppendMenuW(v, MF_STRING, IDM_GALACTIC,  L"银河\tB");
    AppendMenuW(v, MF_STRING, IDM_CONST,     L"星座连线\tC");
    AppendMenuW(v, MF_STRING, IDM_BORDER,    L"星座边界\tJ");
    AppendMenuW(v, MF_STRING, IDM_CONSTNAME, L"星座名称\tN");
    AppendMenuW(v, MF_STRING, IDM_ART,       L"星座贴图（填充版）");
    AppendMenuW(v, MF_STRING, IDM_LABEL,     L"亮星标注\tL");
    AppendMenuW(v, MF_STRING, IDM_DEEP,      L"深空天体\tD");
    AppendMenuW(v, MF_STRING, IDM_PLANETS,   L"各大行星\tP");
    AppendMenuW(v, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(v, MF_STRING, IDM_NCP,       L"转到北天极");
    AppendMenuW(v, MF_STRING, IDM_SCP,       L"转到南天极");
    AppendMenuW(v, MF_STRING, IDM_ORION,     L"转到猎户座");
    AppendMenuW(v, MF_STRING, IDM_GALCENTER, L"转到银心方向");
    AppendMenuW(v, MF_STRING, IDM_SUMMER,    L"转到夏季大三角");
    AppendMenuW(v, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(v, MF_STRING, IDM_INSIDE_ON,  L"球心视角（天球内部）");
    AppendMenuW(v, MF_STRING, IDM_INSIDE_OFF, L"球外视角（经典天球仪）");
    AppendMenuW(v, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(v, MF_STRING, IDM_RESET,     L"复位视图\tR");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)v, L"视图(&V)");

    HMENU h = CreatePopupMenu();
    AppendMenuW(h, MF_STRING, IDM_KEYS,  L"快捷键说明");
    AppendMenuW(h, MF_STRING, IDM_ABOUT, L"关于 TQ 天球仪");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)h, L"帮助(&H)");
    return m;
}

static HMENU MakeContextMenu() {
    HMENU p = CreatePopupMenu();
    // 开关类菜单项按当前选项状态显示对勾，与工具栏按钮状态一致
    const SphereOptions o = g_sphere.GetOptions();
    AppendMenuW(p, MF_STRING, IDM_RESET,     L"复位视图");
    AppendMenuW(p, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(p, MF_STRING | (o.showGrid ? MF_CHECKED : 0),           IDM_GRID,      L"赤道网格");
    AppendMenuW(p, MF_STRING | (o.showEcliptic ? MF_CHECKED : 0),       IDM_ECLIPTIC,  L"黄道");
    AppendMenuW(p, MF_STRING | (o.showConstellations ? MF_CHECKED : 0), IDM_CONST,     L"星座连线");
    AppendMenuW(p, MF_STRING | (o.showBorders ? MF_CHECKED : 0),        IDM_BORDER,    L"星座边界");
    AppendMenuW(p, MF_STRING | (o.showArt ? MF_CHECKED : 0),    IDM_ART,    L"星座贴图（填充版）");
    AppendMenuW(p, MF_STRING | (o.zodiacOnly ? MF_CHECKED : 0), IDM_ZODIAC, L"只看黄道十二宫");
    AppendMenuW(p, MF_STRING | (o.showLabels ? MF_CHECKED : 0),         IDM_LABEL,     L"亮星标注");
    AppendMenuW(p, MF_STRING | (o.showDeepSky ? MF_CHECKED : 0),        IDM_DEEP,      L"深空天体");
    AppendMenuW(p, MF_STRING | (o.showPlanets ? MF_CHECKED : 0),        IDM_PLANETS,   L"各大行星");
    AppendMenuW(p, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(p, MF_STRING, IDM_NCP,       L"转到北天极");
    AppendMenuW(p, MF_STRING, IDM_ORION,     L"转到猎户座");
    AppendMenuW(p, MF_STRING | (g_showOnline ? MF_CHECKED : 0),
                IDM_SHOW_ONLINE, L"显示在线星表");
    return p;
}

// ------------------------------------------------------------------ 标题栏（与菜单栏合并）
// 系统标题栏已去掉（WM_NCCALCSIZE 返回 0），这一条由程序自己画：
// 左起 图标 + 窗口标题 + 四个菜单项，右端是最小化/最大化/关闭按钮。
static const wchar_t* kMenuTexts[4] = { L"文件", L"数据", L"视图", L"帮助" };
static const wchar_t* kTitleText  = L"TQ · 天球星空  |  Celestial Sphere";

struct TitleLayout {
    RECT icon;      // 应用图标
    RECT text;      // 窗口标题
    RECT item[4];   // 菜单项
    RECT btn[3];    // 0 最小化 / 1 最大化 / 2 关闭
};

static int TextPixelWidth(HDC dc, const wchar_t* s) {
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(dc, s, lstrlenW(s), &sz);
    return sz.cx;
}

static TitleLayout GetTitleLayout(HWND h) {
    TitleLayout tl = {};
    RECT rc; GetClientRect(h, &rc);
    HDC dc = GetDC(h);
    HFONT old = (HFONT)SelectObject(dc, g_uiFont);
    const int titleW = TextPixelWidth(dc, kTitleText);   // 物理像素（字体已按 DPI 缩放）
    int x = S(42) + titleW + S(16);
    for (int i = 0; i < 4; ++i) {
        const int w = TextPixelWidth(dc, kMenuTexts[i]) + S(24);
        tl.item[i] = { x, 0, x + w, g_titleH };
        x += w;
    }
    SelectObject(dc, old);
    ReleaseDC(h, dc);

    const int iconSz = S(28);   // 四芒星要够大才显得出四个尖，标题栏 32px 里给到 28
    tl.icon = { S(8), (g_titleH - iconSz) / 2, S(8) + iconSz, (g_titleH - iconSz) / 2 + iconSz };
    tl.text = { S(42), 0, S(42) + titleW + S(6), g_titleH };
    const int btnW = S(46);
    int bx = rc.right;
    for (int i = 2; i >= 0; --i) { tl.btn[i] = { bx - btnW, 0, bx, g_titleH }; bx -= btnW; }
    return tl;
}

// 返回：0 其它（可拖动区） / 1-3 窗口按钮 / 4-7 菜单项
static int TitleBarHitTest(HWND h, POINT pt) {
    const TitleLayout tl = GetTitleLayout(h);
    for (int i = 0; i < 3; ++i)
        if (PtInRect(&tl.btn[i], pt)) return 1 + i;
    for (int i = 0; i < 4; ++i) {
        if (tl.item[i].right > tl.btn[0].left) break;   // 窗口太窄，菜单项已被隐藏
        if (PtInRect(&tl.item[i], pt)) return 4 + i;
    }
    return 0;
}

// bg：按钮当前的背景画刷。「还原」字形用两个叠放的方块，前面那块先用底色填掉，
// 否则后块的边线会穿过前块，看起来像四个小方块错位。
static void DrawCaptionGlyph(HDC dc, int idx, const RECT& r, COLORREF color, HBRUSH bg) {
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const int a = S(5), b = S(6), sq = S(9);
    if (idx == 1 && IsZoomed(g_hwnd)) {
        // 后块 + 前块各 9×9，只错开 3px（与原来一致的间距），前块先用底色填掉压住的部分
        RECT back  = { cx - b, cy - a, cx - b + sq, cy - a + sq };
        RECT front = { cx - S(3), cy - S(3), cx - S(3) + sq, cy - S(3) + sq };
        FillRect(dc, &front, bg);
        HBRUSH frame = CreateSolidBrush(color);
        FrameRect(dc, &back, frame);
        FrameRect(dc, &front, frame);
        DeleteObject(frame);
        return;
    }

    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN oldPen = (HPEN)SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (idx == 0) {                                    // 最小化
        MoveToEx(dc, cx - a, cy, nullptr);
        LineTo(dc, cx + a, cy);
    } else if (idx == 1) {                             // 最大化
        Rectangle(dc, cx - a, cy - a, cx + b, cy + b);
    } else {                                           // 关闭
        MoveToEx(dc, cx - a, cy - a, nullptr); LineTo(dc, cx + a, cy + a);
        MoveToEx(dc, cx + a, cy - a, nullptr); LineTo(dc, cx - a, cy + a);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// 应用标志：四芒星。几何与 tools_make_icon.py 里生成 tq.ico 的参数一致
// （外接半径 R、内凹 0.235R、正十字朝向），保证标题栏和桌面图标是同一个图形。
static void DrawStarGlyph(Graphics& g, REAL cx, REAL cy, REAL R) {
    PointF pts[8];
    for (int i = 0; i < 8; ++i) {
        const REAL a = (REAL)(-90.0 + i * 45.0) * 3.14159265358979f / 180.0f;
        const REAL rad = (i % 2 == 0) ? R : R * 0.235f;      // 四个尖 + 四个内凹
        pts[i] = PointF(cx + rad * cosf(a), cy + rad * sinf(a));
    }
    SolidBrush brush(Color(255, 58, 150, 255));
    g.FillPolygon(&brush, pts, 8);
}

static void DrawTitleBar(HDC dc) {
    RECT rc; GetClientRect(g_hwnd, &rc);
    const TitleLayout tl = GetTitleLayout(g_hwnd);

    RECT bar = { 0, 0, rc.right, g_titleH };
    FillRect(dc, &bar, g_brTitle);
    RECT line = { 0, g_titleH - 1, rc.right, g_titleH };
    FillRect(dc, &line, g_brOn);

    // 标题栏的星标直接矢量绘制，不从 exe 图标位图缩放：
    // 图标位图在非整数 DPI/像素对齐下重采样会发虚、出锯齿，现画的边缘始终干净。
    {
        const REAL sz = (REAL)(tl.icon.right - tl.icon.left);
        if (sz > 0) {
            Graphics g(dc);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            DrawStarGlyph(g, (REAL)tl.icon.left + sz / 2.0f, (REAL)tl.icon.top + sz / 2.0f,
                          sz * 0.47f);
        }
    }

    HFONT oldFont = (HFONT)SelectObject(dc, g_uiFont);
    SetBkMode(dc, TRANSPARENT);
    RECT t = tl.text;
    SetTextColor(dc, RGB(208, 222, 242));
    DrawTextW(dc, kTitleText, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    for (int i = 0; i < 4; ++i) {
        RECT r = tl.item[i];
        if (r.right > tl.btn[0].left) break;
        const bool hot = (g_menuOpen == i) || (g_capHot == 4 + i);
        if (hot) FillRect(dc, &r, g_brHot);
        SetTextColor(dc, hot ? RGB(238, 246, 255) : RGB(178, 196, 220));
        DrawTextW(dc, kMenuTexts[i], -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }

    for (int i = 0; i < 3; ++i) {
        RECT r = tl.btn[i];
        const bool hot = (g_capHot == 1 + i);
        HBRUSH bg = hot ? ((i == 2) ? g_brClose : g_brHot) : g_brTitle;
        if (hot) FillRect(dc, &r, bg);
        DrawCaptionGlyph(dc, i, r, hot ? RGB(255, 255, 255) : RGB(212, 226, 242), bg);
    }
    SelectObject(dc, oldFont);
}

// ------------------------------------------------------------------ 绘制
static void DrawBar(HDC hdc, const RECT& rc) {
    RECT r = { 0, 0, rc.right, g_titleH + g_toolH };
    FillRect(hdc, &r, g_brBar);
    r.top = g_titleH + g_toolH - S(2); r.bottom = g_titleH + g_toolH;
    FillRect(hdc, &r, g_brOn);
    DrawTitleBar(hdc);
}

static void DrawStatus(HDC hdc, const RECT& rc) {
    RECT r = { 0, rc.bottom - g_statusH, rc.right, rc.bottom };
    FillRect(hdc, &r, g_brStatus);
    HFONT old = (HFONT)SelectObject(hdc, g_uiFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(186, 202, 224));
    r.left += S(10); r.top += S(5);
    DrawTextW(hdc, g_status.c_str(), -1, &r, DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, old);
}

// 圆角矩形路径（GDI+ 没有现成的 AddRoundRect）
static void AddRoundRectPath(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    const REAL d = r * 2.0f;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

// 信息卡排版度量：ShowInfoCard 算高度、DrawInfoCardContent 绘制共用，改字号要一起调
static const int kCardHeadH = 48;   // 第一行的 y
static const int kCardRowH  = 27;   // 每个「标签 / 值」行高
static const int kCardFootH = 38;   // 底部统计行
static const int kCardTitleH = 42;  // 标题栏高度
static const REAL kCardLabelX = 14.0f, kCardValueX = 102.0f;

// 卡片内容（画到任意 Graphics：真透明图层或回退用的窗口 HDC）
static void DrawInfoCardContent(Graphics& g, REAL w, REAL h) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);

    GraphicsPath card;
    AddRoundRectPath(card, 0.0f, 0.0f, w, h, SF(14.0));
    SolidBrush bg(Color(82, 16, 24, 42));           // 半透明底：能看到下面的星点，但不影响文字对比度
    g.FillPath(&bg, &card);

    GraphicsState st = g.Save();
    g.SetClip(&card);                               // 标题栏跟随圆角裁切
    SolidBrush titleBg(Color(97, 32, 47, 74));
    g.FillRectangle(&titleBg, 0.0f, 0.0f, w, SF(kCardTitleH));
    g.Restore(st);
    SolidBrush accent(Color(255, 112, 196, 238));
    g.FillRectangle(&accent, 0.0f, SF(kCardTitleH) - 1.0f, w, 1.5f);
    Pen border(Color(180, 110, 190, 235), 1.0f);
    g.DrawPath(&border, &card);

    // 文字配色对齐常见编辑器（暗底上的浅灰白），不再是偏蓝的冷色
    SolidBrush white(Color(255, 232, 235, 239));    // 值：浅灰白 #E8EBEF
    SolidBrush dim(Color(255, 172, 180, 190));      // 标签：中性灰 #ACB4BE
    Font ft(L"Microsoft YaHei UI", SF(16.5), FontStyleBold, UnitPixel);
    Font fl(L"Microsoft YaHei UI", SF(13.0), FontStyleRegular, UnitPixel);
    Font fv(L"Microsoft YaHei UI", SF(13.5), FontStyleRegular, UnitPixel);
    Font fx(L"Microsoft YaHei UI", SF(17.0), FontStyleRegular, UnitPixel);

    const REAL labelX = SF(kCardLabelX), valueX = SF(kCardValueX);

    // 标题显示天体名称（信息卡里不再单列「名称」行），过长时省略号截断
    {
        RectF tb(labelX, SF(7.0), w - labelX - SF(40.0), SF(30.0));
        StringFormat tf;
        tf.SetTrimming(StringTrimmingEllipsisCharacter);
        tf.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(g_detailTitle.c_str(), -1, &ft, tb, &tf, &white);
    }
    g.DrawString(L"×", -1, &fx, PointF(w - SF(30.0), SF(7.0)), &dim);

    // 单行「标签 / 值」紧凑布局，值超宽时省略号截断
    REAL yy = SF(kCardHeadH);
    for (const auto& kv : g_detail) {
        g.DrawString(kv.first.c_str(), -1, &fl, PointF(labelX, yy), &dim);
        RectF box(valueX, yy, w - valueX - SF(14.0), SF(kCardRowH - 5));
        StringFormat fmt;
        fmt.SetTrimming(StringTrimmingEllipsisCharacter);
        fmt.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(kv.second.c_str(), -1, &fv, box, &fmt, &white);
        yy += SF(kCardRowH);
        if (yy > h - SF(kCardFootH)) break;
    }

    Pen sep(Color(90, 110, 140, 180), 1.0f);
    g.DrawLine(&sep, labelX, h - SF(30.0), w - labelX, h - SF(30.0));
    wchar_t cnt[96];
    swprintf_s(cnt, L"显示 %lld / 数据库 %lld 颗", g_shownStars, g_totalStars);
    g.DrawString(cnt, -1, &fl, PointF(labelX, h - SF(24.0)), &dim);
}

// 分层窗口 + 逐像素 alpha：先画到 32bpp ARGB 位图，预乘后交给 UpdateLayeredWindow，
// 这样只有卡片底色半透明，文字仍然完全不透明（与整窗常量 alpha 的效果不同）。
static bool g_infoULW = false;       // 已成功使用 UpdateLayeredWindow
static bool g_infoTried = false;

static bool PaintInfoCardLayered() {
    if (!g_info) return false;
    RECT rc; GetClientRect(g_info, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return false;

    Bitmap bmp(w, h, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        DrawInfoCardContent(g, (REAL)w, (REAL)h);
    }

    Rect rr(0, 0, w, h);
    BitmapData bd;
    if (bmp.LockBits(&rr, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;              // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(mem); ReleaseDC(nullptr, screen);
        bmp.UnlockBits(&bd);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, dib);

    // ARGB -> 预乘 ARGB（UpdateLayeredWindow 要求）
    for (int y = 0; y < h; ++y) {
        const BYTE* s = (const BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
        BYTE* d = (BYTE*)bits + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            const BYTE a = s[x * 4 + 3];
            d[x * 4 + 0] = (BYTE)(s[x * 4 + 0] * a / 255);
            d[x * 4 + 1] = (BYTE)(s[x * 4 + 1] * a / 255);
            d[x * 4 + 2] = (BYTE)(s[x * 4 + 2] * a / 255);
            d[x * 4 + 3] = a;
        }
    }
    bmp.UnlockBits(&bd);

    RECT wr; GetWindowRect(g_info, &wr);
    POINT dst = { wr.left, wr.top };
    SIZE size = { w, h };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    const BOOL ok = UpdateLayeredWindow(g_info, screen, &dst, &size, mem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok != FALSE;
}

// 把信息卡贴到天球右下角并按内容调整高度；不显示时不占位
static void ShowInfoCard() {
    if (!g_info) return;
    RECT rc; GetClientRect(g_hwnd, &rc);
    const int topH = g_titleH + g_toolH;
    const int w = min(S(g_infoW), max(S(220), (int)rc.right - S(40)));

    // 高度上限按显示器工作区算：窗口本身可能超出屏幕（高 DPI 小屏），
    // 若按窗口高度算，星座资料那十几行会被截掉尾部。
    MONITORINFO mi = { sizeof(mi) };
    RECT wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) wa = mi.rcWork;

    int h = S(kCardHeadH) + (int)g_detail.size() * S(kCardRowH) + S(kCardFootH);

    // 卡片底边贴窗口右下角，但不越过工作区下沿；左侧同理
    const int pad = S(3);
    POINT pb = { 0, (LONG)((int)rc.bottom - pad) };
    POINT px = { (LONG)((int)rc.right - w - pad), 0 };
    ClientToScreen(g_hwnd, &pb);                      // 卡片是顶层窗口，位置用屏幕坐标
    ClientToScreen(g_hwnd, &px);
    // 显示器矩形可能来自物理像素，而窗口坐标偶尔被系统虚拟化过（差 1/dpi），
    // 两个尺度都夹一次取更严的那个，保证卡片一定落在可见区域内
    const int visRight  = min((int)wa.right,  (int)(wa.right  / g_dpiScale));
    const int visBottom = min((int)wa.bottom, (int)(wa.bottom / g_dpiScale));
    const int cardBottom = min((int)pb.y, visBottom - pad);
    int cardLeft = min((int)px.x, visRight - w - pad);
    cardLeft = max(cardLeft, (int)wa.left + pad);

    // 优先不盖住工具栏；内容确实放不下时再向上占满工作区（星座资料行数多）
    POINT pbar = { 0, (LONG)(topH + S(4)) };
    ClientToScreen(g_hwnd, &pbar);
    const int roomBar  = cardBottom - max((int)pbar.y, (int)wa.top + pad);
    const int roomFull = cardBottom - ((int)wa.top + pad);
    if (h > roomBar) h = roomFull;
    h = min(h, max(S(160), roomFull));

    SetWindowPos(g_info, HWND_TOP, cardLeft, cardBottom - h, w, h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);

    if (!g_infoTried) {
        g_infoTried = true;
        g_infoULW = PaintInfoCardLayered();
        if (!g_infoULW) {
            // 老系统不支持分层子窗口：退化为常量 alpha + 普通 WM_PAINT 自绘
            SetLayeredWindowAttributes(g_info, 0, 116, LWA_ALPHA);
        }
    }
    if (g_infoULW) PaintInfoCardLayered();
    else InvalidateRect(g_info, nullptr, FALSE);
}

static void HideInfoCard() {
    if (g_info) ShowWindow(g_info, SW_HIDE);
}

static LRESULT CALLBACK InfoCardProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (!g_infoULW) {          // ULW 模式下内容由图层合成，不需要窗口自绘
            RECT rc; GetClientRect(h, &rc);
            Graphics g(dc);
            DrawInfoCardContent(g, (REAL)rc.right, (REAL)rc.bottom);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int x = (int)(short)LOWORD(l), y = (int)(short)HIWORD(l);
        RECT rc; GetClientRect(h, &rc);
        if (x >= rc.right - S(34) && y <= S(kCardTitleH)) {   // 右上角 × 关闭
            ShowWindow(h, SW_HIDE);
            return 0;
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

static void DrawToggle(DRAWITEMSTRUCT* d) {
    HDC hdc = d->hDC;
    bool on = false;
    if (d->CtlID == IDC_TG_INSIDE)        on = g_sphere.GetStats().inside;
    else if (d->CtlID == IDC_TG_OUTSIDE)  on = !g_sphere.GetStats().inside;
    else if (d->CtlID == IDC_TG_SPIN)     on = g_sphere.IsAutoSpin();
    else if (InToggleList((int)d->CtlID)) on = IsToggleOn((int)d->CtlID);
    else if (d->CtlID >= IDC_MAG3 && d->CtlID <= IDC_MAG85) on = (g_activeMag == (int)d->CtlID);
    else on = (d->itemState & ODS_SELECTED) != 0;

    HBRUSH bg = on ? g_brOn : g_brOff;
    FillRect(hdc, &d->rcItem, bg);
    RECT frame = d->rcItem;
    HPEN pen = CreatePen(PS_SOLID, 1, on ? RGB(96, 172, 214) : RGB(52, 66, 88));
    HPEN old = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldb = (HBRUSH)SelectObject(hdc, (HBRUSH)GetStockObject(NULL_BRUSH));
    Rectangle(hdc, frame.left, frame.top, frame.right, frame.bottom);
    SelectObject(hdc, oldb); SelectObject(hdc, old);
    DeleteObject(pen);

    wchar_t text[128] = {};
    GetWindowTextW(d->hwndItem, text, 128);
    HFONT oldf = (HFONT)SelectObject(hdc, g_uiFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, on ? RGB(232, 246, 255) : RGB(158, 176, 200));
    DrawTextW(hdc, text, -1, &d->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldf);
}

// ------------------------------------------------------------------ 布局
static void Layout() {
    RECT rc; GetClientRect(g_hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    const int topH = g_titleH + g_toolH;             // 标题栏 + 工具栏
    int bodyH = H - topH - g_statusH;
    // 天球铺满整个窗口宽度；天体信息改为右下角浮层，不再长期占位
    if (g_sphere.IsValid())
        MoveWindow(g_sphere.Hwnd(), 0, topH, max(100, W), max(100, bodyH), TRUE);
    if (g_info && IsWindowVisible(g_info)) ShowInfoCard();   // 窗口尺寸变化时重新贴边

    int x = 10, y = kTitleH + 8;
    // 标签放置：按当前字体测量文本像素宽，仅加 2px 间距，使输入框自然右移且不显过宽。
    // 量出来的是物理像素，换算回逻辑值再加进累加器。
    auto placeLabel = [&](int id, const wchar_t* t) {
        int w = 48;
        HDC dc = GetDC(g_hwnd);
        HFONT old = (HFONT)SelectObject(dc, g_uiFont);
        SIZE sz = { 0, 0 };
        if (GetTextExtentPoint32W(dc, t, lstrlenW(t), &sz)) w = (int)(sz.cx / g_dpiScale + 0.5);
        SelectObject(dc, old); ReleaseDC(g_hwnd, dc);
        int lw = w + 2;
        Place(id, x, y, lw, 26);
        x += lw;
    };

    placeLabel(IDC_LABEL_STAR, L"星名：");
    Place(IDC_SEARCH,      x, y, 80, 26);    // 星名框宽度缩小 1/3
    {
        x += 86;                            // 80 宽 + 6 间距
        PlacePopupList(g_starList, GetDlgItem(g_hwnd, IDC_SEARCH), 160);
    }
    placeLabel(IDC_LABEL_CITY, L"地点：");
    {
        Place(IDC_CITY, x, y, 80, 26);     // 与星名框等宽
        PlacePopupList(g_cityList, GetDlgItem(g_hwnd, IDC_CITY), 160);
        x += 86;
    }
    placeLabel(IDC_LABEL_TIME, L"时间：");
    Place(IDC_TIME,        x, y,  64, 26); x += 70;
    placeLabel(IDC_LABEL_DIR, L"方向：");
    {
        Place(IDC_DIR, x, y, 64, 26);
        PlacePopupList(g_dirList, GetDlgItem(g_hwnd, IDC_DIR), 160, 110);
        x += 70;
    }
    x += 14;
    Place(IDC_BTN_RESET,   x, y,  72, 26); x += 78;   // 复位键移到输入框组行尾
    Place(IDC_TG_SPIN,    x, y,  64, 26); x += 70;
    Place(IDC_SPIN_DEC,   x, y,  28, 26); x += 32;
    Place(IDC_SPIN_LABEL, x, y,  52, 26); x += 56;
    Place(IDC_SPIN_INC,   x, y,  28, 26);

    x = 10; y = kTitleH + 42;
    struct { int id; const wchar_t* t; int w; } row[] = {
        { IDC_TG_INSIDE,    L"球心视角", 76 },
        { IDC_TG_OUTSIDE,   L"外部视角", 76 },
        { IDC_TG_GRID,      L"G 网格",   66 },
        { IDC_TG_EQUATOR,   L"Q 赤道",   66 },
        { IDC_TG_ECLIPTIC,  L"E 黄道",   66 },
        { IDC_TG_GALACTIC,  L"B 银河",   66 },
        { IDC_TG_CONST,     L"C 星座",   66 },
        { IDC_TG_CONSTNAME, L"N 名称",   66 },
        { IDC_TG_LABEL,     L"L 标注",   66 },
        { IDC_TG_DEEP,      L"D 深空",   66 },
        { IDC_TG_CROSS,     L"准线",     56 },
        { IDC_TG_AA,        L"平滑",     56 },
        { IDC_TG_MOON,      L"月亮",     56 },
        { IDC_TG_PLANETS,   L"行星",     56 },
        { IDC_TG_ZODIAC,    L"十二宫",   66 },
    };
    for (const auto& it : row) { Place(it.id, x, y, it.w, 26); x += it.w + 5; }
    x += 12;
    Place(IDC_MAG3,  x, y, 46, 26); x += 50;
    Place(IDC_MAG45, x, y, 46, 26); x += 50;
    Place(IDC_MAG65, x, y, 46, 26); x += 50;
    Place(IDC_MAG85, x, y, 46, 26);
}

// ------------------------------------------------------------------ 窗口过程
static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_hwnd = h;
        g_uiFont = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        g_brBar    = CreateSolidBrush(RGB(18, 24, 38));
        g_brPanel  = CreateSolidBrush(RGB(16, 21, 34));
        g_brStatus = CreateSolidBrush(RGB(12, 16, 26));
        g_brOn     = CreateSolidBrush(RGB(28, 62, 92));
        g_brOff    = CreateSolidBrush(RGB(24, 31, 46));
        g_brEdit   = CreateSolidBrush(RGB(12, 18, 30));
        g_brTitle  = CreateSolidBrush(RGB(13, 18, 29));
        g_brHot    = CreateSolidBrush(RGB(33, 45, 66));
        g_brClose  = CreateSolidBrush(RGB(198, 45, 32));

        HWND starLabel = CreateWindowExW(0, L"STATIC", L"星名：",
                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                    0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_LABEL_STAR, g_hInst, nullptr);
        SendMessageW(starLabel, WM_SETFONT, (WPARAM)g_uiFont, TRUE);

        HWND cityLabel = CreateWindowExW(0, L"STATIC", L"地点：",
                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                    0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_LABEL_CITY, g_hInst, nullptr);
        SendMessageW(cityLabel, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        HWND timeLabel = CreateWindowExW(0, L"STATIC", L"时间：",
                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                    0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_LABEL_TIME, g_hInst, nullptr);
        SendMessageW(timeLabel, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        HWND dirLabel = CreateWindowExW(0, L"STATIC", L"方向：",
                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                    0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_LABEL_DIR, g_hInst, nullptr);
        SendMessageW(dirLabel, WM_SETFONT, (WPARAM)g_uiFont, TRUE);

        HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                    0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_SEARCH, g_hInst, nullptr);
        SendMessageW(ed, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        SendMessageW(ed, EM_SETCUEBANNER, TRUE, (LPARAM)L"输入星名");

        // 星名查找：EDIT + 拼音首字母候选列表（LISTBOX 浮层），仿城市输入
        // 不用 CBS_DROPDOWN：type-ahead 自动补全会吞掉连续键入的字符。
        g_starList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                    WS_POPUP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                    0, 0, 10, 10, h, nullptr, g_hInst, nullptr);
        SendMessageW(g_starList, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        ShowWindow(g_starList, SW_HIDE);
        g_oldStarListProc = (WNDPROC)SetWindowLongPtrW(g_starList, GWLP_WNDPROC, (LONG_PTR)PopupListProc);
        g_oldStarEditProc = (WNDPROC)SetWindowLongPtrW(ed, GWLP_WNDPROC, (LONG_PTR)StarEditProc);

        // 城市输入框（EDIT）+ 拼音首字母候选列表（LISTBOX 浮层）+ 本地时间输入
        // 不用 CBS_DROPDOWN：它在下拉展开时的 type-ahead 自动补全会吞掉连续键入的字符。
        HWND cityEd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                    0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_CITY, g_hInst, nullptr);
        SendMessageW(cityEd, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        SendMessageW(cityEd, EM_SETCUEBANNER, TRUE, (LPARAM)L"输入拼音首字母，如 bt");

        // 候选列表为主窗口子窗口，紧贴输入框下方；工具栏按钮均带
        // WS_CLIPSIBLINGS，其 owner-draw 绘制不会溢出盖住本列表。
        g_cityList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                    WS_POPUP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                    0, 0, 10, 10, h, nullptr, g_hInst, nullptr);
        SendMessageW(g_cityList, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        ShowWindow(g_cityList, SW_HIDE);
        g_oldCityListProc = (WNDPROC)SetWindowLongPtrW(g_cityList, GWLP_WNDPROC, (LONG_PTR)PopupListProc);
        g_oldCityEditProc = (WNDPROC)SetWindowLongPtrW(cityEd, GWLP_WNDPROC, (LONG_PTR)CityEditProc);

        citydb::Open();
        citydb::LoadAll(g_cities);
        BuildCityLabels();
        SetWindowTextW(cityEd, L"北京市");   // 默认北京
        HWND te = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"20:00",
                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_CENTER | WS_TABSTOP,
                    0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_TIME, g_hInst, nullptr);
        SendMessageW(te, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        SendMessageW(te, EM_SETCUEBANNER, TRUE, (LPARAM)L"HH:MM");
        g_oldTimeEditProc = (WNDPROC)SetWindowLongPtrW(te, GWLP_WNDPROC, (LONG_PTR)TimeEditProc);

        // 观测方向框（EDIT + LISTBOX）：8 个方位，默认正南 180°
        HWND dirEd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"南",
                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_CENTER | WS_TABSTOP,
                    0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_DIR, g_hInst, nullptr);
        SendMessageW(dirEd, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        SendMessageW(dirEd, EM_SETCUEBANNER, TRUE, (LPARAM)L"方向");
        g_dirList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                    WS_POPUP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                    0, 0, 10, 10, h, nullptr, g_hInst, nullptr);
        SendMessageW(g_dirList, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        ShowWindow(g_dirList, SW_HIDE);
        g_oldDirListProc = (WNDPROC)SetWindowLongPtrW(g_dirList, GWLP_WNDPROC, (LONG_PTR)PopupListProc);
        g_oldDirEditProc = (WNDPROC)SetWindowLongPtrW(dirEd, GWLP_WNDPROC, (LONG_PTR)DirEditProc);

        // 四个输入框的鼠标悬停提示
        CreateInputTooltips(h, ed, cityEd, te, dirEd);

        // 天球自转：开关 + 倍率增减（默认 ×1，每次 ±60 倍）
        MakeButton(IDC_TG_SPIN, L"自转", true);
        MakeButton(IDC_SPIN_DEC, L"▼", true);    // 减少 60 倍
        MakeButton(IDC_SPIN_INC, L"▲", true);    // 增加 60 倍
        HWND lbl = CreateWindowExW(0, L"STATIC", L"×1",
                    WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                    0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_SPIN_LABEL, g_hInst, nullptr);
        SendMessageW(lbl, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        g_spinMult = 1;
        g_sphere.SetSpinRate(1.0);   // 默认真实速度（未自转）

        MakeButton(IDC_BTN_RESET,  L"复位", false);

        MakeButton(IDC_TG_GRID,      L"G 网格", true);
        MakeButton(IDC_TG_EQUATOR,   L"Q 赤道", true);
        MakeButton(IDC_TG_ECLIPTIC,  L"E 黄道", true);
        MakeButton(IDC_TG_GALACTIC,  L"B 银河", true);
        MakeButton(IDC_TG_CONST,     L"C 星座", true);
        MakeButton(IDC_TG_CONSTNAME, L"N 名称", true);
        MakeButton(IDC_TG_LABEL,     L"L 标注", true);
        MakeButton(IDC_TG_DEEP,      L"D 深空", true);
        MakeButton(IDC_TG_CROSS,     L"准线",   true);
        MakeButton(IDC_TG_AA,        L"平滑",   true);
        MakeButton(IDC_TG_MOON,      L"月亮",   true);
        MakeButton(IDC_TG_PLANETS,   L"行星",   true);
        MakeButton(IDC_TG_ZODIAC,    L"十二宫", true);
        MakeButton(IDC_TG_INSIDE,    L"球心视角", true);
        MakeButton(IDC_TG_OUTSIDE,   L"外部视角", true);
        MakeButton(IDC_MAG3,  L"3.0", true);
        MakeButton(IDC_MAG45, L"4.5", true);
        MakeButton(IDC_MAG65, L"6.5", true);
        MakeButton(IDC_MAG85, L"8.5", true);

        // 菜单不再挂到窗口上（否则会多出一条系统白底菜单栏）：
        // 菜单项由自绘标题栏绘制，点击时用 TrackPopupMenu 弹出对应子菜单。
        g_mainMenu = MakeMainMenu();

        if (!g_sphere.Create(h, 0, g_titleH + g_toolH, 800, 600, IDC_SPHERE, g_hInst))
            SetStatus(L"天球控件创建失败");
        else {
            ReloadCatalog();
            SendMessageW(g_sphere.Hwnd(), TQSM_SETINSIDE, 1, 0);   // 默认进入球心视角
            ApplyObserverFromUI();                                  // 默认北京 20:00 正南天空
        }
        // 工具栏控件必须位于绘制区(sphere)之上，否则其下拉列表会被 sphere 遮挡而打不开。
        // g_sphere 后创建、z-order 更高，故这里把工具栏控件统一提到最前。
        {
            int tb[] = { IDC_CITY, IDC_TIME, IDC_DIR, IDC_LABEL_STAR, IDC_LABEL_CITY, IDC_LABEL_TIME, IDC_LABEL_DIR, IDC_TG_SPIN, IDC_SPIN_DEC, IDC_SPIN_INC, IDC_SPIN_LABEL,
                         IDC_BTN_RESET,
                         IDC_TG_INSIDE, IDC_TG_OUTSIDE, IDC_TG_GRID, IDC_TG_EQUATOR,
                         IDC_TG_ECLIPTIC, IDC_TG_GALACTIC, IDC_TG_CONST, IDC_TG_CONSTNAME,
                         IDC_TG_LABEL, IDC_TG_DEEP, IDC_TG_CROSS, IDC_TG_AA,
                         IDC_MAG3, IDC_MAG45, IDC_MAG65, IDC_MAG85, IDC_TG_MOON,
                         IDC_TG_PLANETS, IDC_TG_ZODIAC };
            for (int i = 0; i < (int)(sizeof(tb) / sizeof(tb[0])); ++i) {
                HWND c = GetDlgItem(h, tb[i]);
                if (c) SetWindowPos(c, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            }
        }
        // 右下角天体信息浮层：带 owner 的顶层分层窗口（UpdateLayeredWindow 不支持子窗口），
        // WS_EX_TOOLWINDOW 不进任务栏，不抢焦点；owner 关系保证它始终浮在主窗口之上
        g_info = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                 L"TQ_InfoCard", L"", WS_POPUP,
                                 0, 0, 10, 10, h, nullptr, g_hInst, nullptr);
        if (!g_info) {
            // 老系统不支持分层子窗口：退化成不透明浮动卡片，功能不受影响
            g_info = CreateWindowExW(WS_EX_NOACTIVATE, L"TQ_InfoCard", L"",
                                     WS_CHILD, 0, 0, 10, 10, h, nullptr, g_hInst, nullptr);
        }
        Layout();
        return 0;
    }

    case WM_SIZE:
        Layout();
        InvalidateRect(h, nullptr, TRUE);
        return 0;

    case WM_MOVE:
        if (g_info && IsWindowVisible(g_info)) ShowInfoCard();   // 浮层跟随主窗口移动
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        // 只重绘与脏区相交的部分：信息卡是独立子窗口，不参与主窗口绘制
        if (ps.rcPaint.top < g_titleH + g_toolH) DrawBar(hdc, rc);
        if (ps.rcPaint.bottom > rc.bottom - g_statusH) DrawStatus(hdc, rc);
        EndPaint(h, &ps);
        return 0;
    }

    // 去掉系统标题栏：客户区覆盖整个窗口；最大化时对齐到显示器工作区，不压任务栏
    case WM_NCCALCSIZE:
        if (w) {
            if (IsZoomed(h) || IsIconic(h)) {
                // 最大化 / 最小化时窗口矩形会比屏幕大一圈（隐藏的缩放边框），
                // 直接把客户区对齐到显示器工作区，避免压住任务栏或多屏串味。
                NCCALCSIZE_PARAMS* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(l);
                MONITORINFO mi = { sizeof(mi) };
                if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
                    p->rgrc[0] = mi.rcWork;
            }
            return 0;
        }
        break;

    case WM_NCHITTEST: {
        POINT pt = { (int)(short)LOWORD(l), (int)(short)HIWORD(l) };
        ScreenToClient(h, &pt);
        RECT rc; GetClientRect(h, &rc);
        if (!IsZoomed(h)) {                       // 最大化时不需要拖拽边框
            const int b = 6;
            const bool L = pt.x < b, R = pt.x >= rc.right - b;
            const bool T = pt.y < b, B = pt.y >= rc.bottom - b;
            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
        }
        if (pt.y < g_titleH)
            return (TitleBarHitTest(h, pt) >= 1) ? HTCLIENT : HTCAPTION;   // 按钮/菜单自己处理，其余可拖动
        return HTCLIENT;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { (int)(short)LOWORD(l), (int)(short)HIWORD(l) };
        int hot = 0;
        if (pt.y < g_titleH) {
            const int ht = TitleBarHitTest(h, pt);
            if (ht >= 1) hot = ht;
        }
        if (hot != g_capHot) {
            g_capHot = hot;
            RECT r; GetClientRect(h, &r); r.bottom = g_titleH;
            InvalidateRect(h, &r, FALSE);
        }
        if (!g_titleTracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            g_titleTracking = TrackMouseEvent(&tme) != FALSE;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_titleTracking = false;
        if (g_capHot != 0) {
            g_capHot = 0;
            RECT r; GetClientRect(h, &r); r.bottom = g_titleH;
            InvalidateRect(h, &r, FALSE);
        }
        return 0;

    // 自绘标题栏没有 WS_CAPTION，系统的「拖标题栏移动」不会自己生效，这里手动进入移动循环
    case WM_NCLBUTTONDOWN: {
        POINT pt = { (int)(short)LOWORD(l), (int)(short)HIWORD(l) };
        ScreenToClient(h, &pt);
        if (pt.y >= 0 && pt.y < g_titleH && TitleBarHitTest(h, pt) == 0) {
            ReleaseCapture();
            SendMessageW(h, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
            return 0;
        }
        break;
    }

    // 双击标题栏 = 最大化 / 还原（自绘标题栏拿不到系统默认行为，自己实现）
    case WM_NCLBUTTONDBLCLK: {
        POINT pt = { (int)(short)LOWORD(l), (int)(short)HIWORD(l) };
        ScreenToClient(h, &pt);
        if (pt.y >= 0 && pt.y < g_titleH && TitleBarHitTest(h, pt) == 0) {
            SendMessageW(h, WM_SYSCOMMAND, IsZoomed(h) ? SC_RESTORE : SC_MAXIMIZE, 0);
            return 0;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { (int)(short)LOWORD(l), (int)(short)HIWORD(l) };
        if (pt.y >= g_titleH) break;
        const int ht = TitleBarHitTest(h, pt);
        if (ht == 1) { SendMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0); return 0; }
        if (ht == 2) { SendMessageW(h, WM_SYSCOMMAND, IsZoomed(h) ? SC_RESTORE : SC_MAXIMIZE, 0); return 0; }
        if (ht == 3) { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
        if (ht >= 4 && g_mainMenu) {
            const int idx = ht - 4;
            HMENU sub = GetSubMenu(g_mainMenu, idx);
            if (sub) {
                const TitleLayout tl = GetTitleLayout(h);
                POINT sp = { tl.item[idx].left, tl.item[idx].bottom };
                ClientToScreen(h, &sp);
                g_menuOpen = idx;
                RECT r; GetClientRect(h, &r); r.bottom = g_titleH;
                InvalidateRect(h, &r, FALSE);
                UpdateWindow(h);
                SetForegroundWindow(h);
                const int cmd = (int)TrackPopupMenu(sub,
                        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_LEFTBUTTON,
                        sp.x, sp.y, 0, h, nullptr);
                g_menuOpen = -1;
                InvalidateRect(h, &r, FALSE);
                if (cmd) SendMessageW(h, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
            }
            return 0;
        }
        break;   // 拖动区由 HTCAPTION 交给系统处理，不会走到这里
    }

    case WM_DRAWITEM:
        DrawToggle(reinterpret_cast<DRAWITEMSTRUCT*>(l));
        return TRUE;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkColor(dc, RGB(12, 18, 30));
        SetTextColor(dc, RGB(220, 234, 250));
        return (LRESULT)g_brEdit;
    }

    case WM_NOTIFY: {
        SphereNotify* n = reinterpret_cast<SphereNotify*>(l);
        if (!n || n->hdr.hwndFrom != g_sphere.Hwnd()) break;
        switch (n->hdr.code) {
        case TQN_HOVER: {
            const wchar_t* disp = n->nameCn[0] ? n->nameCn : n->name;
            std::wstring s = n->starIndex != -1
                ? std::wstring(L"悬停：") + disp + L"  " + n->nameEn
                : L"就绪  ·  左键拖动旋转 / 右键拖动平移 / 滚轮缩放 / 双击居中";
            if (n->constName[0]) s += std::wstring(L"   ·   星座：") + n->constName;
            SetStatus(s);
            break;
        }
        case TQN_SELECT:
            if (n->starIndex == -1) {          // 点到空白天区：收起信息卡，恢复就绪提示
                HideInfoCard();
                SetStatus(L"就绪  ·  左键拖动旋转 / 右键拖动平移 / 滚轮缩放 / 双击居中");
                break;
            }
            ShowStarDetail(*n);
            if (n->starIndex > TQ_CONST_BASE)
                SyncSearchBox(*n);             // 单击 / 双击选中的星同步到星名输入框（星座不回填）
            SetStatus(n->starIndex <= TQ_CONST_BASE
                ? std::wstring(L"已选中星座：") + (n->nameCn[0] ? n->nameCn : n->name)
                : std::wstring(L"已选中：") + (n->nameCn[0] ? n->nameCn : n->name));
            break;
        case TQN_VIEWCHANGED: {
            RefreshToggles();
            SphereStats s = g_sphere.GetStats();
            wchar_t rab[64], decb[64];
            FormatRA(s.centerRA, rab, 64); FormatDec(s.centerDec, decb, 64);
            wchar_t b[220];
            swprintf_s(b, L"%ls  视图中心 RA %ls  DEC %ls   |   缩放 %.2fx   |   可见 %d 颗   |   %lld 颗在库   |   帧 %.1f ms",
                       s.inside ? L"[球心视角]" : L"[外部视角]", rab, decb,
                       s.inside ? s.innerZoom : s.zoom,
                       s.drawnStars, g_totalStars, s.frameMs);
            SetStatus(b);
            RefreshViewButtons();
            if (std::isfinite(s.centerAz))
                SyncDirectionFromView(s.centerAz);   // 视线方位变了就同步「方向」框
            // 自转（周日运动）时，时间框跟随模拟时间流逝（程序化更新，抑制回灌）
            if (g_sphere.IsAutoSpin()) {
                tq::SphereObserver ob = g_sphere.GetObserver();
                int hh = (int)ob.hour;
                int mm = (int)((ob.hour - hh) * 60.0 + 0.5);
                if (mm >= 60) { mm -= 60; hh += 1; }
                hh %= 24;   // 自转可越过 24h，显示时取模
                wchar_t tb[8];
                swprintf_s(tb, L"%02d:%02d", hh, mm);
                g_timeUpdating = true;
                SetWindowTextW(GetDlgItem(g_hwnd, IDC_TIME), tb);
                g_timeUpdating = false;
            }
            break;
        }
        case TQN_DBLCLICK:
            SyncSearchBox(*n);                 // 双击定位完成后再同步一次（与选中状态一致）
            SetStatus(n->starIndex >= 0
                ? std::wstring(L"已定位：") + (n->nameCn[0] ? n->nameCn : n->name)
                : L"已把该天区转到视图中心");
            break;
        case TQN_RESET:
            ResetViewToInitial();              // R 键与复位按钮走同一路径
            break;
        }
        return 0;
    }

    case WM_CONTEXTMENU: {
        if ((HWND)w == g_sphere.Hwnd()) {
            int x = (int)(short)LOWORD(l), y = (int)(short)HIWORD(l);
            if (x < 0 || y < 0) { POINT p; GetCursorPos(&p); x = p.x; y = p.y; }
            HMENU pop = MakeContextMenu();
            TrackPopupMenu(pop, TPM_LEFTALIGN | TPM_RIGHTBUTTON, x, y, 0, h, nullptr);
            DestroyMenu(pop);
        }
        return 0;
    }

    case WM_APP_CITY_SELECTALL: {
        SendMessageW(GetDlgItem(h, IDC_CITY), EM_SETSEL, 0, (LPARAM)-1);
        return 0;
    }

    case WM_APP_HIDE_CITYLIST: {
        HWND f   = GetFocus();
        HWND lst = g_cityList;
        HWND ed  = GetDlgItem(h, IDC_CITY);
        if (f != lst && f != ed) ShowWindow(lst, SW_HIDE);   // 焦点确实离开了输入/候选区
        return 0;
    }

    case WM_APP_STAR_SELECTALL: {
        SendMessageW(GetDlgItem(h, IDC_SEARCH), EM_SETSEL, 0, (LPARAM)-1);
        return 0;
    }
    case WM_APP_HIDE_STARLIST: {
        HWND f = GetFocus();
        HWND ed = GetDlgItem(h, IDC_SEARCH);
        if (f != g_starList && f != ed) ShowWindow(g_starList, SW_HIDE);
        return 0;
    }
    case WM_APP_HIDE_DIRLIST: {
        HWND f  = GetFocus();
        HWND ed = GetDlgItem(h, IDC_DIR);
        if (f != g_dirList && f != ed) ShowWindow(g_dirList, SW_HIDE);
        return 0;
    }

    case WM_APP_FETCH_DONE: {
        std::wstring* msg = reinterpret_cast<std::wstring*>(l);
        std::wstring text = msg ? *msg : L"(无消息)";
        delete msg;
        ReloadCatalog();
        SetStatus(text);
        if (!w) MessageBoxW(h, text.c_str(), L"在线抓取", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    case WM_COMMAND: {
        // 候选列表是 WS_POPUP（无子窗口 ID），按窗口句柄识别其通知
        if (l && (HWND)l == g_cityList) {
            if (g_popupPicking) return 0;
            if (HIWORD(w) == LBN_SELCHANGE) {
                int sel = (int)SendMessageW(g_cityList, LB_GETCURSEL, 0, 0);
                PickCityFromList(sel);
                SetFocus(GetDlgItem(h, IDC_CITY));
            }
            return 0;
        }
        if (l && (HWND)l == g_starList) {
            if (g_popupPicking) return 0;
            if (HIWORD(w) == LBN_SELCHANGE) {
                int sel = (int)SendMessageW(g_starList, LB_GETCURSEL, 0, 0);
                PickStarFromList(sel);
                SetFocus(GetDlgItem(h, IDC_SEARCH));
            }
            return 0;
        }
        if (l && (HWND)l == g_dirList) {
            if (g_popupPicking) return 0;
            if (HIWORD(w) == LBN_SELCHANGE) {
                int sel = (int)SendMessageW(g_dirList, LB_GETCURSEL, 0, 0);
                PickDirFromList(sel);
                SetFocus(GetDlgItem(h, IDC_DIR));
            }
            return 0;
        }
        int id = LOWORD(w);
        switch (id) {
        case IDC_TG_INSIDE:
            SendMessageW(g_sphere.Hwnd(), TQSM_SETINSIDE, 1, 0);
            RefreshViewButtons();
            return 0;
        case IDC_TG_OUTSIDE:
            SendMessageW(g_sphere.Hwnd(), TQSM_SETINSIDE, 0, 0);
            RefreshViewButtons();
            return 0;
        case IDM_FETCH:   DoFetch(); return 0;
        case IDM_SHOW_ONLINE:
            g_showOnline = !g_showOnline;
            ReloadCatalog();
            InvalidateRect(g_hwnd, nullptr, FALSE);   // 刷新右侧面板底部星数（否则要等 resize 才刷新）
            SetStatus((g_showOnline ? L"已显示在线补充星表（共 " : L"仅显示内置星表（共 ")
                      + std::to_wstring(g_shownStars) + L" 颗）");
            return 0;
        case IDM_IMPORT: DoImportFile(); return 0;
        case IDC_BTN_RESET: case IDM_RESET:   ResetViewToInitial(); return 0;
        case IDM_RESEED: {
            std::wstring e;
            if (g_catalog->ForceSeedBuiltin(&e)) { ReloadCatalog(); SetStatus(L"内置精选星表已重建"); }
            else SetStatus(L"重建失败：" + e);
            return 0;
        }
        case IDM_CLEAR: {
            if (MessageBoxW(h, L"确定清除所有在线导入 / 本地导入的恒星？\n内置精选星表会保留。",
                            L"确认", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            std::wstring e;
            if (g_catalog->ClearImported(&e)) { ReloadCatalog(); SetStatus(L"已清除导入数据，仅保留内置星表"); }
            else SetStatus(L"清除失败：" + e);
            return 0;
        }
        case IDM_OPENDIR: {
            ShellExecuteW(h, L"open", g_exeDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case IDM_EXIT: DestroyWindow(h); return 0;

        case IDM_GRID:     ToggleOption(IDC_TG_GRID); return 0;
        case IDM_EQUATOR:  ToggleOption(IDC_TG_EQUATOR); return 0;
        case IDM_ECLIPTIC: ToggleOption(IDC_TG_ECLIPTIC); return 0;
        case IDM_CONST:    ToggleOption(IDC_TG_CONST); return 0;
        case IDM_BORDER:   ToggleOption(IDC_TG_BORDER); return 0;
        case IDM_LABEL:    ToggleOption(IDC_TG_LABEL); return 0;
        case IDM_DEEP:     ToggleOption(IDC_TG_DEEP); return 0;
        case IDM_GALACTIC: ToggleOption(IDC_TG_GALACTIC); return 0;
        case IDM_CONSTNAME:ToggleOption(IDC_TG_CONSTNAME); return 0;
        case IDM_PLANETS:  ToggleOption(IDC_TG_PLANETS); return 0;

        case IDM_ART: {
            SphereOptions o = g_sphere.GetOptions();
            o.showArt = !o.showArt;
            g_sphere.SetOptions(o);
            SetStatus(o.showArt ? L"星座贴图：已打开" : L"星座贴图：已关闭");
            return 0;
        }
        case IDM_ZODIAC:
            ToggleOption(IDC_TG_ZODIAC);
            return 0;

        case IDM_NCP:       g_sphere.Goto(0.0,  90.0, 1.8); return 0;
        case IDM_SCP:       g_sphere.Goto(0.0, -90.0, 1.8); return 0;
        case IDM_ORION:     g_sphere.Goto(5.60, -1.0, 1.7); return 0;
        case IDM_GALCENTER: g_sphere.Goto(17.76, -28.94, 1.5); return 0;
        case IDM_SUMMER:    g_sphere.Goto(19.0,  25.0, 1.2); return 0;
        case IDM_INSIDE_ON:
            SendMessageW(g_sphere.Hwnd(), TQSM_SETINSIDE, 1, 0);
            RefreshViewButtons();
            return 0;
        case IDM_INSIDE_OFF:
            SendMessageW(g_sphere.Hwnd(), TQSM_SETINSIDE, 0, 0);
            RefreshViewButtons();
            return 0;

        case IDC_CITY:
            if (g_cityUpdating) return 0;
            if (HIWORD(w) == EN_SETFOCUS) {
                // EN_SETFOCUS 发生在鼠标按下阶段，此时全选会被随后的 EDIT 默认
                // 鼠标处理（放置光标）清掉，故投递消息延迟到点击结束后执行。
                PostMessageW(h, WM_APP_CITY_SELECTALL, 0, 0);
                return 0;
            }
            if (HIWORD(w) == EN_CHANGE) {
                std::wstring key = GetCityEditText();
                if (IsExactCityLabel(key)) {
                    ShowWindow(g_cityList, SW_HIDE);
                    ApplyObserverFromUI();          // 手输完整汉字名 / 选择后回填
                } else if (IsAsciiLetters(key)) {
                    RefreshCityList(key, true);     // 拼音首字母 -> 弹出匹配汉字候选
                } else if (key.empty()) {
                    RefreshCityList(L"", false);
                } else {
                    RefreshCityList(key, true);     // 汉字子串匹配
                }
                return 0;
            }
            if (HIWORD(w) == EN_KILLFOCUS) {
                // 不能立即隐藏：点击候选项时 EDIT 先失焦，列表一旦隐藏会吞掉这次
                // 鼠标点击，LBN_SELCHANGE 就不会触发。EN_KILLFOCUS 的 lParam 是 EDIT
                // 自身句柄而非新焦点窗口，故改为投递消息，等焦点切换完成后再判断。
                PostMessageW(h, WM_APP_HIDE_CITYLIST, 0, 0);
                return 0;
            }
            return 0;
        case IDC_DIR:
            if (g_dirUpdating) return 0;
            if (HIWORD(w) == EN_CHANGE) {
                wchar_t buf[32] = {};
                GetDlgItemTextW(g_hwnd, IDC_DIR, buf, 32);
                std::wstring key(buf);
                if (key.empty()) RefreshDirList(L"", false);
                else             RefreshDirList(key, true);
                for (int i = 0; i < kDirCount; ++i)
                    if (key == kDirections[i].name) { ApplyObserverFromUI(); break; }
                return 0;
            }
            if (HIWORD(w) == EN_KILLFOCUS) {
                PostMessageW(h, WM_APP_HIDE_DIRLIST, 0, 0);
                return 0;
            }
            return 0;
        case IDC_TIME:
            if (HIWORD(w) == EN_CHANGE) {
                if (g_timeUpdating) return 0;   // 自转同步时间框的程序化修改，不回灌
                // 输入完整 HH:MM 就实时更新，不必等输入框失焦；中间态不重绘视图。
                wchar_t tb[16] = {};
                GetDlgItemTextW(g_hwnd, IDC_TIME, tb, 16);
                if (IsCompleteTimeText(tb)) ApplyObserverFromUI();
                return 0;
            }
            if (HIWORD(w) == EN_KILLFOCUS) ApplyObserverFromUI();
            return 0;

        case IDC_SEARCH:
            if (g_starUpdating) return 0;
            if (HIWORD(w) == EN_SETFOCUS) {
                // EN_SETFOCUS 发生在鼠标按下阶段，全选会被默认处理清掉，故延迟执行
                PostMessageW(h, WM_APP_STAR_SELECTALL, 0, 0);
                return 0;
            }
            if (HIWORD(w) == EN_CHANGE) {
                wchar_t kbuf[128] = {};
                GetDlgItemTextW(g_hwnd, IDC_SEARCH, kbuf, 128);
                std::wstring key(kbuf);
                if (key.empty()) RefreshStarList(L"", false);
                else             RefreshStarList(key, true);   // 拼音首字母 / 英文名前缀 / 汉字包含
                return 0;
            }
            if (HIWORD(w) == EN_KILLFOCUS) {
                // 不能立即隐藏：点击候选项时 EDIT 先失焦，列表隐藏会吞掉点击。
                PostMessageW(h, WM_APP_HIDE_STARLIST, 0, 0);
                return 0;
            }
            return 0;

        case IDC_TG_SPIN:
            g_sphere.SetAutoSpin(!g_sphere.IsAutoSpin());
            RefreshToggles();
            SetStatus(g_sphere.IsAutoSpin()
                ? (L"天球自转：开启 ×" + std::to_wstring(g_spinMult))
                : L"天球自转：停止");
            return 0;
        case IDC_SPIN_INC:
            // ×1 → ×60 → ×120 → …：除 ×1 外均为 60 的整数倍
            g_spinMult = (g_spinMult < 60) ? 60 : g_spinMult + 60;
            if (g_spinMult > 999960) g_spinMult = 999960;   // 上限也保持 60 的整数倍
            g_sphere.SetSpinRate((double)g_spinMult);
            SetWindowTextW(GetDlgItem(h, IDC_SPIN_LABEL), (L"×" + std::to_wstring(g_spinMult)).c_str());
            if (g_sphere.IsAutoSpin())
                SetStatus(L"天球自转：×" + std::to_wstring(g_spinMult));
            return 0;
        case IDC_SPIN_DEC:
            g_spinMult = (g_spinMult <= 60) ? 1 : g_spinMult - 60;
            if (g_spinMult < 1) g_spinMult = 1;
            g_sphere.SetSpinRate((double)g_spinMult);
            SetWindowTextW(GetDlgItem(h, IDC_SPIN_LABEL), (L"×" + std::to_wstring(g_spinMult)).c_str());
            if (g_sphere.IsAutoSpin())
                SetStatus(L"天球自转：×" + std::to_wstring(g_spinMult));
            return 0;

        case IDM_ABOUT:
            MessageBoxW(h,
                L"TQ · 自建 C++ 天球仪\n\n"
                L"纯 Win32 + GDI+ 自绘控件，SQLite 持久化，WinHTTP 抓取公开星表。\n\n"
                L"左键拖动旋转天球 · 右键拖动平移 · 滚轮以鼠标为锚点缩放\n"
                L"单击选中天体 · 双击把该天区转到中心\n\n"
                L"视角切换由工具栏「球心视角 / 外部视角」按钮控制，缩放不会自动跨视角；\n"
                L"球心视角下拖动为第一人称转头，外部视角为抓天球。R 复位到球心视角。\n\n"
                L"内置精选亮星 / 星座连线 / 深空天体，支持 VizieR (BSC5) 与 HYG CSV。\n"
                L"月亮与七大行星按观测时间实时推算（Meeus 月球理论 + JPL 开普勒根数）。\n"
                L"数据库：tq_catalog.db（程序目录）",
                L"关于 TQ 天球仪", MB_OK | MB_ICONINFORMATION);
            return 0;
        case IDM_KEYS:
            MessageBoxW(h,
                L"方向键  旋转天球\n+ / -   缩放\nR  复位视图\n"
                L"G  赤道网格    Q  天赤道    E  黄道    B  银河\n"
                L"C  星座连线    J  星座边界    N  星座名称  L  亮星标注  D  深空天体\n"
                L"P  各大行星（水星/金星/火星/木星/土星/天王星/海王星）\n\n"
                L"鼠标：左键拖动旋转、右键拖动平移、滚轮缩放、双击居中、右键菜单\n\n"
                L"视角切换由工具栏「球心视角 / 外部视角」按钮控制，缩放不自动跨视角；\n"
                L"球心视角下拖动为第一人称转头，外部视角为抓天球。",
                L"快捷键", MB_OK | MB_ICONINFORMATION);
            return 0;
        default:
            if (id >= IDC_MAG3 && id <= IDC_MAG85) { ToggleOption(id); return 0; }
            // 开关按钮按 kToggleIds 表分发：ID 段不连续（IDC_TG_MOON=214、IDC_TG_SPIN=212），
            // 用区间判断会漏掉它们，导致工具栏点击无反应。
            for (int t : kToggleIds) {
                if (t == id) { ToggleOption(id); return 0; }
            }
        }
        return 0;
    }

    case WM_DESTROY:
        // 抓取线程只访问长生命周期的 g_catalog，直接分离，避免关窗时等待网络超时
        if (g_worker.joinable()) g_worker.detach();
        g_sphere.Destroy();
        CelestialSphere::Shutdown();
        DeleteObject(g_uiFont);
        DeleteObject(g_brBar); DeleteObject(g_brPanel); DeleteObject(g_brStatus);
        DeleteObject(g_brOn); DeleteObject(g_brOff); DeleteObject(g_brEdit);
        DeleteObject(g_brTitle); DeleteObject(g_brHot); DeleteObject(g_brClose);
        if (g_mainMenu) { DestroyMenu(g_mainMenu); g_mainMenu = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ------------------------------------------------------------------ 入口
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInst = hInst;

    // 先开 DPI 感知，再建任何窗口：系统按整窗位图拉伸会把 1px 线拉成 1.25px 糊边。
    // 用「系统 DPI 感知」而不是 Per-Monitor V2：只需要一个缩放比，
    // 跨不同 DPI 的显示器时交给系统拉伸，不需要额外处理 WM_DPICHANGED。
    SetProcessDPIAware();
    {
        HDC dc = GetDC(nullptr);
        const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(nullptr, dc);
        g_dpiScale = (dpi > 0 ? dpi : 96) / 96.0;
    }
    g_titleH = S(kTitleH);
    g_toolH  = S(kToolH);
    g_statusH = S(kStatusH);

    // 声明整个进程为暗色应用：弹出菜单、消息框等系统控件跟随暗色主题。
    // uxtheme 的未公开接口（序号 135/136），Win10 1903+ 有效；取不到就静默跳过。
    // 模块必须常驻，不能 FreeLibrary，否则主题钩子被卸载会导致弹菜单崩溃。
    if (HMODULE ux = LoadLibraryW(L"uxtheme.dll")) {
        using PfnSetPreferredAppMode = int(WINAPI*)(int);
        using PfnFlushMenuThemes    = void(WINAPI*)();
        if (auto setMode = (PfnSetPreferredAppMode)GetProcAddress(ux, MAKEINTRESOURCEA(135))) {
            setMode(2);   // 2 = ForceDark
            if (auto flush = (PfnFlushMenuThemes)GetProcAddress(ux, MAKEINTRESOURCEA(136))) flush();
        }
    }
    wchar_t mod[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, mod, MAX_PATH);
    std::wstring full(mod);
    size_t p = full.find_last_of(L"\\/");
    g_exeDir = (p == std::wstring::npos) ? L".\\" : full.substr(0, p + 1);

    std::wstring err;
    if (!g_catalog->Open(g_exeDir + L"tq_catalog.db", &err)) {
        MessageBoxW(nullptr, (L"无法打开数据库：" + err).c_str(), L"TQ", MB_OK | MB_ICONERROR);
        return 1;
    }
    g_catalog->SeedBuiltinIfEmpty(&err);
    g_catalog->MetaSet("app", L"TQ Celestial Sphere 1.0");

    if (!CelestialSphere::Register(hInst)) {
        MessageBoxW(nullptr, L"GDI+ 初始化失败", L"TQ", MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &MainProc;
    wc.hInstance = hInst;
    HICON appIconBig   = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_TQ_APP), IMAGE_ICON,
                                          GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    const int smIco = max((int)GetSystemMetrics(SM_CXSMICON), S(28));   // 与自绘标题栏的图标槽一致
    HICON appIconSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_TQ_APP), IMAGE_ICON,
                                          smIco, smIco, 0);
    if (!appIconBig)   appIconBig   = LoadIcon(nullptr, IDI_APPLICATION);
    if (!appIconSmall) appIconSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_TQ_APP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.hIcon = appIconBig;
    wc.hIconSm = appIconSmall;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"TQ_MainWindow";
    wc.lpszMenuName = nullptr;
    RegisterClassExW(&wc);

    // 右下角信息卡（子窗口类）：无背景擦除，全部自绘
    WNDCLASSEXW ic = {};
    ic.cbSize = sizeof(ic);
    ic.style = 0;
    ic.lpfnWndProc = &InfoCardProc;
    ic.hInstance = hInst;
    ic.hCursor = LoadCursor(nullptr, IDC_ARROW);
    ic.hbrBackground = nullptr;
    ic.lpszClassName = L"TQ_InfoCard";
    RegisterClassExW(&ic);

    // 默认尺寸按显示器工作区收着放并居中：高 DPI 下 1360×880 会比可用区域还高，
    // 窗口底部被切掉，信息卡底部统计行跟着看不见
    RECT wa = { 0, 0, 1360, 880 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int waW = (int)(wa.right - wa.left), waH = (int)(wa.bottom - wa.top);
    const int winW = min(S(1360), max(S(760), waW - S(40)));
    const int winH = min(S(880),  max(S(520), waH - S(40)));
    const int winX = (int)wa.left + (waW - winW) / 2;
    const int winY = (int)wa.top + (waH - winH) / 2;

    // 不要 WS_CAPTION：标题栏与菜单栏合并成一条，由 WM_PAINT 自绘（见 DrawTitleBar）
    HWND h = CreateWindowExW(0, L"TQ_MainWindow", L"TQ · 天球星空  |  Celestial Sphere",
                             WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
                             WS_SYSMENU | WS_CLIPCHILDREN,
                             winX, winY, winW, winH,
                             nullptr, nullptr, hInst, nullptr);
    if (!h) return 1;

    // 自绘标题栏通过 WM_GETICON 取图标，这里显式挂上，任务栏/Alt-Tab 也用它
    SendMessageW(h, WM_SETICON, ICON_BIG,   (LPARAM)appIconBig);
    SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)appIconSmall);

    // 让系统原生菜单、滚动条等跟随暗色主题（Win10 2004+ 支持；老系统调用失败无副作用）
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        typedef HRESULT(WINAPI* PfnSetAttr)(HWND, DWORD, LPCVOID, DWORD);
        if (auto setAttr = (PfnSetAttr)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
            BOOL dark = TRUE;
            setAttr(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
            // 自绘标题栏后系统仍会画一圈窗口边框，这里一并染成暗色（Win11）
            COLORREF border = RGB(40, 52, 72);
            setAttr(h, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
            COLORREF caption = RGB(13, 18, 29);
            setAttr(h, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
        }
        FreeLibrary(dwm);
    }

    ShowWindow(h, nCmdShow);
    UpdateWindow(h);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
