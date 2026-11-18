#pragma once
// ============================================================================
// 自建 Win32 天球仪控件（纯 C++ / GDI+，不依赖任何 UI 框架）
//
//   CelestialSphere::Register(hInst);
//   CelestialSphere sphere;
//   sphere.Create(hwndParent, 0, 0, 800, 600, 1001, hInst);
//   sphere.SetData(catalogData);
//   sphere.Goto(5.6, -1.2);
//
// 也可用原生 Win32 方式 CreateWindowW(TQ_SPHERE_CLASS, ...) 并把 CelestialSphere*
// 作为 lpCreateParams 传入；随后通过 TQSM_* 消息控制。
//
// 交互：左键拖动=旋转天球，右键拖动=平移，滚轮=缩放（以鼠标为锚点），
//       单击=选中天体，双击=把该处天区转到视图中心，方向键/+-/R/G/C/L/E/D 快捷键。
// 通知：控件向父窗口发送 WM_NOTIFY，code 为 TQN_*，lParam 指向 SphereNotify。
// ============================================================================
#include <windows.h>
#include <string>
#include <vector>
#include "star_catalog.h"

namespace tq {

#define TQ_SPHERE_CLASS L"TQ_CelestialSphereCtrl"

// --------------------------- 父窗口 -> 控件 ---------------------------
enum TQSphereMessage {
    TQSM_RELOAD       = WM_APP + 0x200,  // lParam = const CatalogData*（控件会拷贝）
    TQSM_SETOPTIONS   = WM_APP + 0x201,  // lParam = const SphereOptions*
    TQSM_GETOPTIONS   = WM_APP + 0x202,  // lParam = SphereOptions*（出参）
    TQSM_GETVIEW      = WM_APP + 0x203,  // lParam = SphereView*（出参）
    TQSM_SETVIEW      = WM_APP + 0x204,  // lParam = const SphereView*
    TQSM_GOTO         = WM_APP + 0x205,  // lParam = const SphereGoto*
    TQSM_RESET        = WM_APP + 0x206,
    TQSM_GETSTATS     = WM_APP + 0x207,  // lParam = SphereStats*（出参）
    TQSM_HIGHLIGHT    = WM_APP + 0x208,  // lParam = const wchar_t*（按名称高亮并居中）
    TQSM_SETINSIDE    = WM_APP + 0x209,  // wParam = 1 进入球心视角 / 0 回到外部视角
    TQSM_SETOBSERVER  = WM_APP + 0x20A,  // lParam = const SphereObserver*
};

// --------------------------- 控件 -> 父窗口 ---------------------------
#define TQN_HOVER        1   // 悬停天体变化
#define TQN_SELECT       2   // 单击选中天体
#define TQN_VIEWCHANGED  3   // 视图/选项变化
#define TQN_DBLCLICK     4   // 双击定位完成
#define TQN_RESET        5   // 请求父窗口执行完整复位（恢复初始观测地/时间）

struct SphereNotify {
    NMHDR  hdr;
    int    starIndex;              // -1 表示无天体
    double ra, dec, mag;
    wchar_t name[128];
    wchar_t nameCn[128];           // 中文名（无则为空）
    wchar_t nameEn[128];
    wchar_t con[16];
    wchar_t spectral[32];
    wchar_t info[1024];               // 中国传统星名等附加数据（标签\t值\n）
    wchar_t constName[64];            // 鼠标所在星座的中文名（贴图模式下有值）
    int    hip;
    double centerRA, centerDec, zoom;
    bool   inside;                 // 观测点是否已在天球内部（球心视角）
    double innerZoom;
};

struct SphereOptions {
    bool   showGrid = true;
    bool   showEquator = true;
    bool   showEcliptic = true;
    bool   showGalactic = false;
    bool   showConstellations = true;
    bool   showBorders = true;          // IAU 星座边界网格线（浅淡蓝）
    bool   showConstellationNames = false;
    bool   showLabels = true;
    bool   showDeepSky = true;
    bool   showMoon = true;
    bool   showPlanets = true;      // 太阳系各大行星（按观测时间实时推算）
    bool   showArt = false;         // 星座贴图（填充版，数据来自 const_art 表）
    bool   zodiacOnly = false;      // 只显示黄道十二宫的贴图
    bool   showNorthMark = true;
    bool   showCrosshair = true;
    bool   antialias = false;      // 抗锯齿描边代价很高，默认高速；需要精细时可在工具栏开启
    double limitingMag = 6.5;     // 显示星等上限（数值越大显示越多暗星）
    double labelMag = 2.2;        // 标注名称的星等上限
    int    maxStars = 30000;      // 单帧绘制预算
};

struct SphereView {
    double centerRA = 0.0;        // 视图中心赤经（小时）
    double centerDec = 0.0;       // 视图中心赤纬（度）
    double zoom = 1.0;            // 外部视角放大倍数（1 ~ kMaxZoom）
    double roll = 0.0;            // 视场旋转角（度）
    double panX = 0.0, panY = 0.0;
    bool   inside = false;        // false: 视点在球外；true: 视点在球心（gnomonic 投影）
    double innerZoom = 1.0;       // 球心视角的放大倍数
};

struct SphereGoto {
    double ra = 0.0, dec = 0.0;
    double zoom = -1.0;           // <0 表示保持当前缩放
    bool   animate = true;
};

struct SphereObserver {           // 观测者：地点 + 本地时间（用于地平坐标）
    double lat = 39.9042;         // 纬度（度，北正）
    double lon = 116.4074;        // 经度（度，东正）
    int    year = 2026, month = 1, day = 1;
    double hour = 20.0;           // 本地时间（小时，小数；中国统一 UTC+8）
    double azDeg = 180.0;         // 观测方位角（度，自北向东；180 = 正南）
};

struct SphereStats {
    int totalStars = 0;
    int drawnStars = 0;
    int hoverIndex = -1;
    int selectedIndex = -1;
    double centerRA = 0, centerDec = 0, zoom = 1;
    double centerAz = 0, centerAlt = 0;   // 视图中心的地平方位/高度角（拖动后由视图反算）
    double frameMs = 0;       // 最近一帧绘制耗时（指数平滑）
    bool   inside = false;
    double innerZoom = 1;
};

struct DrawCtx;   // 绘制上下文，定义在 celestial_sphere.cpp

// 动态天体的通知索引约定（SphereNotify::starIndex）：
//   >= 0        恒星在 CatalogData::stars 中的下标
//   -2          月球
//   -100 - i    第 i 颗行星（i 与 kPlanetCount / 行星表顺序一致）
//   -1000 - i   星座（i 对应 CatalogData::boundary 下标，贴图模式下才可能命中）
#define TQ_MOON_INDEX    (-2)
#define TQ_PLANET_BASE   (-100)   // 行星索引 = TQ_PLANET_BASE - i，i ∈ [0, 7)
#define TQ_CONST_BASE    (-1000)  // 星座索引 = TQ_CONST_BASE - i

// ------------------------------------------------------------------ 控件类
class CelestialSphere {
public:
    CelestialSphere();
    ~CelestialSphere();

    static bool Register(HINSTANCE hInst);       // 注册窗口类 + 启动 GDI+
    static void Shutdown();                      // 程序退出时关闭 GDI+

    bool Create(HWND parent, int x, int y, int w, int h, int ctrlId, HINSTANCE hInst);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    bool IsValid() const { return hwnd_ != nullptr; }

    // 便捷 API（内部转成 SendMessage）
    void           SetData(const CatalogData& data);
    void           SetOptions(const SphereOptions& o);
    SphereOptions  GetOptions() const;
    void           SetView(const SphereView& v);
    SphereView     GetView() const;
    void           SetObserver(const SphereObserver& o);   // 设置观测者并定向到正南天空
    SphereObserver GetObserver() const;
    void           SetAutoSpin(bool on);                    // 天球自转（按真实恒星时速度的倍数推进本地时间）
    bool           IsAutoSpin() const;
    void           SetSpinRate(double rate);                // 自转倍率（相对真实恒星时速度，1=真实）
    double         GetSpinRate() const;
    void           Goto(double raHours, double decDeg, double zoom = -1.0, bool animate = true);
    void           Reset();
    SphereStats    GetStats() const;
    void           Highlight(const std::wstring& keyword);

    // 行星（水星、金星、火星、木星、土星、天王星、海王星）
    static const int kPlanetCount = 7;
    struct PlanetState {
        Vec3   v = { 0, 0, 1 };      // 地心赤道单位向量
        double raH = 0, decD = 0;    // 赤经（小时）/ 赤纬（度）
        double mag = 0;              // 视星等（经验公式）
        double distAU = 0;           // 地心距离（AU）
        double helioAU = 0;          // 日心距离（AU）
        double elongDeg = 0;         // 与太阳的角距（度）
        bool   valid = false;
    };
    const PlanetState& GetPlanet(int i) const { return planets_[i < 0 ? 0 : (i >= kPlanetCount ? kPlanetCount - 1 : i)]; }

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);

    // --- 几何 ---
    void  PointToObserverSky();             // 按当前观测者/方位角/高度角定向到屏幕中心
    void  SyncObserverFromView();           // 视图方向反算回 obsAz_/obsAlt_（拖动、定位后调用）
    void  SetViewRA(double raHours, double decDeg, double rollDeg);
    void  GetViewRA(double& raHours, double& decDeg, double& rollDeg) const;
    void  RotateBy(double dxPixels, double dyPixels);
    void  RecomputeGeometry();
    struct Proj { bool visible = false; double x = 0, y = 0, z = 0, rel = 1; };
    Proj  Project(const Vec3& v) const;
    bool  ScreenToSphere(double sx, double sy, Vec3& world) const;
    int   HitTest(int mx, int my) const;
    int   PickObject(int mx, int my) const;          // 贴图模式下星座优先于天体
    int   ConstellationAt(int mx, int my) const;     // 光标落在哪个星座边界内（-1 = 无）

    // --- 绘制 ---
    struct LabelItem { int idx = -1; double x = 0, y = 0, r = 0; };
    void  Paint(HDC hdc);
    void  Render(DrawCtx& c);
    void  DrawHorizonLayer(DrawCtx& c);   // 地平圈 / 方位标记 / 天顶（观测者地平坐标系）
    void  EnsureBackgroundCache();             // 静态背景层：不随旋转变化
    void  CopyBackgroundToFrame();             // 纯 memcpy，替代全屏 DrawImage
    void  DrawStarLayer(DrawCtx& c, std::vector<LabelItem>& labels);
    void  DrawConstellationArt(DrawCtx& c);   // 星座贴图（填充版）
    void  DrawConstHighlight(DrawCtx& c, int idx, bool selected);   // 高亮某个星座的边界 + 星座名
    void  DrawMoon(DrawCtx& c);
    void  UpdateMoon();
    void  DrawPlanets(DrawCtx& c);
    void  UpdatePlanets();
    double MoonRadius() const;             // 月盘像素半径（绘制与命中判定共用）
    double PlanetRadius(int i) const;      // 行星盘面像素半径
    void  SendNotify(int code, int index);
    void  ZoomAt(int mx, int my, double factor);
    void  ToggleAndNotify(bool SphereOptions::* field);
    void  StartAnimation(const Vec3& target, double zoomTo);
    void  StopAnimation();

    HWND      hwnd_ = nullptr;
    HINSTANCE hInst_ = nullptr;

    CatalogData    data_;
    SphereOptions  opt_;
    double  R_[3][3] = {};                 // 世界(赤道) -> 相机 的旋转矩阵
    double  roll_ = 0.0;
    double  zoom_ = 1.0;
    double  panX_ = 0.0, panY_ = 0.0;
    bool    inside_ = false;           // true: 观测点位于球心，从天球内部朝外看
    double  innerZoom_ = 1.0;          // 球心视角放大倍数
    double  innerFocal_ = 0;           // 球心 gnomonic 投影焦距
    int     width_ = 0, height_ = 0;
    double  dpiScale_ = 1.0;               // 物理像素 / 96dpi，字体与天体半径按此放大
    double  cx_ = 0, cy_ = 0;             // 视图中心（像素）
    double  radius_ = 0;                   // 球的像素半径
    double  focal_ = 0;                    // 投影焦距（像素）

    // --- 交互状态 ---
    bool    dragging_ = false;
    bool    moved_ = false;
    bool    panMode_ = false;              // true=右键平移, false=左键旋转
    int     downX_ = 0, downY_ = 0, lastX_ = 0, lastY_ = 0, downHit_ = -1;
    int     hoverIndex_ = -1, selectedIndex_ = -1;
    int     hoverConst_ = -1, selConst_ = -1;   // 悬停 / 选中的星座（CatalogData::boundary 下标）
    int     lastDrawn_ = 0;
    double  frameMs_ = 0;                  // 帧耗时（指数平滑）
    DWORD   lastViewNotify_ = 0;           // 拖动时状态通知节流

    double  LSTdeg() const;                 // 本地恒星时（度，基于 obsLon_ 与北京时间）

    // --- 观测者（地平坐标）---
    double  obsLat_ = 39.9042, obsLon_ = 116.4074;
    int     obsYear_ = 2026, obsMonth_ = 1, obsDay_ = 1;
    double  obsHour_ = 20.0;                // 本地时间（小时，UTC+8）
    double  obsAz_ = 180.0;                 // 观测方位角（度，自北向东）
    double  obsAlt_ = 35.0;                 // 视线高度角（度）：默认 35°，拖动后跟随视图
    bool    hasObserver_ = false;

    // --- 月亮（动态天体，按观测时间推导）---
    Vec3    moonV_ = { 0, 0, 1 };
    Vec3    sunV_  = { 0, 0, 1 };
    double  moonRaH_ = 0.0, moonDecD_ = 0.0;
    double  moonIllum_ = 0.0, moonDist_ = 0.0, moonPhaseDeg_ = 0.0;
    bool    moonValid_ = false;

    // --- 行星（动态天体，按观测时间用开普勒根数推导）---
    PlanetState planets_[kPlanetCount];

    // --- 天球自转 ---
    bool    autoSpin_ = false;
    double  spinRate_ = 60.0;                // 自转倍率（相对真实恒星时速度，1=真实）
    ULONGLONG lastSpinTick_ = 0;             // 自转定时器上次触发时刻

    // --- 渲染缓存（Gdiplus::Bitmap*，用 void* 避免头文件依赖 GDI+）---
    void*   frameCache_ = nullptr;         // 主帧缓冲，避免每帧分配 3MB
    void*   bgCache_ = nullptr;            // 静态背景层（球盘 + 轮廓 + 刻度）
    void*   starLayer_ = nullptr;          // 星点层，直接写像素后一次合成
    int     frameW_ = 0, frameH_ = 0;
    int     bgW_ = 0, bgH_ = 0;
    double  bgR_ = -1, bgPX_ = 0, bgPY_ = 0;
    bool    bgGrid_ = false;
    bool    bgInside_ = false;

    // --- 转场动画 ---
    bool    anim_ = false;
    double  animT_ = 0;
    Vec3    animFrom_, animTo_;
    double  animZoomFrom_ = 1, animZoomTo_ = 1;

    static const double kCamDist;          // 外部视角：相机距球心距离（球半径 = 1）
    static const double kMaxZoom;          // 外部视角放大上限，到达后继续滚入球心视角
    static const double kMaxInnerZoom;     // 球心视角放大上限
};

} // namespace tq
