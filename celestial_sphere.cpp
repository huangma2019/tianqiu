// 天球仪控件实现：Win32 自定义窗口类 + GDI+ 双缓冲 + 3D 球面投影
#include "celestial_sphere.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstring>

using std::min;
using std::max;
#include <objbase.h>      // GDI+ 头依赖 IStream / PROPID / byte 等 OLE 类型
#include <gdiplus.h>
using namespace Gdiplus;

namespace tq {

const double CelestialSphere::kCamDist = 3.0;
const double CelestialSphere::kMaxZoom = 14.0;
const double CelestialSphere::kMaxInnerZoom = 400.0;
static ULONG_PTR g_gdiToken = 0;

// 绘制上下文：全部是栈对象指针，由 Paint() 统一管理生命周期
struct DrawCtx {
    Graphics* g = nullptr;
    Pen*      grid = nullptr;
    Pen*      equator = nullptr;
    Pen*      ecliptic = nullptr;
    Pen*      galactic = nullptr;
    Pen*      border = nullptr;
    Pen*      constellation = nullptr;
    Pen*      deep = nullptr;
    Pen*      outline = nullptr;
    Pen*      tick = nullptr;
    Pen*      ray = nullptr;
    Pen*      marker = nullptr;
    Pen*      selected = nullptr;
    SolidBrush* star = nullptr;
    SolidBrush* text = nullptr;
    SolidBrush* dim = nullptr;
    SolidBrush* hud = nullptr;
    Font*      fontLabel = nullptr;
    Font*      fontSmall = nullptr;
    Font*      fontHud = nullptr;
};

// ------------------------------------------------------------------ 矩阵工具
static inline Vec3 MulR(const double R[3][3], const Vec3& v) {
    return { R[0][0]*v.x + R[0][1]*v.y + R[0][2]*v.z,
             R[1][0]*v.x + R[1][1]*v.y + R[1][2]*v.z,
             R[2][0]*v.x + R[2][1]*v.y + R[2][2]*v.z };
}
static inline Vec3 MulRT(const double R[3][3], const Vec3& v) {
    return { R[0][0]*v.x + R[1][0]*v.y + R[2][0]*v.z,
             R[0][1]*v.x + R[1][1]*v.y + R[2][1]*v.z,
             R[0][2]*v.x + R[1][2]*v.y + R[2][2]*v.z };
}
static void MulMat(const double A[3][3], const double B[3][3], double out[3][3]) {
    double t[3][3] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) t[i][j] += A[i][k] * B[k][j];
    std::memcpy(out, t, sizeof(t));
}
static inline Vec3 Slerp(const Vec3& a, const Vec3& b, double t) {
    double dot = Clamp(a.x*b.x + a.y*b.y + a.z*b.z, -1.0, 1.0);
    double om = std::acos(dot);
    if (om < 1e-7) return a;
    if (dot < -0.9999) {                       // 对径点：借助一个垂直方向绕行
        Vec3 p = { -a.y, a.x, 0.0 };
        double n = std::sqrt(p.x*p.x + p.y*p.y + p.z*p.z);
        if (n < 1e-6) p = { 1, 0, 0 }, n = 1;
        p.x /= n; p.y /= n; p.z /= n;
        double ang = t * PI;
        return { a.x*std::cos(ang) + p.x*std::sin(ang),
                 a.y*std::cos(ang) + p.y*std::sin(ang),
                 a.z*std::cos(ang) + p.z*std::sin(ang) };
    }
    double so = std::sin(om);
    double w1 = std::sin((1.0 - t) * om) / so, w2 = std::sin(t * om) / so;
    return { w1*a.x + w2*b.x, w1*a.y + w2*b.y, w1*a.z + w2*b.z };
}

// 银道系 -> 赤道系 旋转矩阵（IAU J2000 近似，行=银道基在赤道系的表示）
static const double kGalToEq[3][3] = {
    { -0.0548755604,  0.4941094279, -0.8676661490 },
    { -0.8734370902, -0.4448296300, -0.1980763734 },
    { -0.4838350155,  0.7469822445,  0.4559837762 }
};
static Vec3 GalacticVec(double lonDeg, double latDeg) {
    double l = lonDeg * D2R, b = latDeg * D2R;
    double cl = std::cos(l)*std::cos(b), sl = std::sin(l)*std::cos(b), sb = std::sin(b);
    return { kGalToEq[0][0]*cl + kGalToEq[1][0]*sl + kGalToEq[2][0]*sb,
             kGalToEq[0][1]*cl + kGalToEq[1][1]*sl + kGalToEq[2][1]*sb,
             kGalToEq[0][2]*cl + kGalToEq[1][2]*sl + kGalToEq[2][2]*sb };
}

// ------------------------------------------------------------------ 观测者 / 地平坐标工具
static double JulianDate(int y, int m, int d, double utcHour) {
    if (m <= 2) { m += 12; --y; }
    int A = y / 100;
    int B = 2 - A + A / 4;
    return std::floor(365.25 * (y + 4716.0)) + std::floor(30.6001 * (m + 1))
         + d + B - 1524.5 + utcHour / 24.0;
}
static double GMSTdeg(double jd) {
    double g = 280.46061837 + 360.98564736629 * (jd - 2451545.0);
    g = std::fmod(g, 360.0);
    if (g < 0) g += 360.0;
    return g;
}

// ------------------------------------------------------------------ 月球位置
// Meeus《天文算法》第47章简化月球理论（ELP-2000 截断），自行推导、无需联网。
// 输出地心赤道坐标与相位，精度约 0.3°，足够天文可视化。
// 项格式：{D, M, M', F, Σl(1e-6°), Σr(1e-3 km), Σb(1e-6°)}
struct MoonTerm { int d, m, mp, f; double l, r, b; };
static const MoonTerm kMoonTerms[] = {
    // 黄经/距离主要项
    {0,0,1,0, 6288774, -20905355, 0},
    {2,0,-1,0, 1274027, -3699111, 0},
    {2,0,0,0, 658314, -2955968, 0},
    {0,0,2,0, 213618, -569925, 0},
    {0,1,0,0, -185116, 48888, 0},
    {0,0,0,2, -114332, -3149, 0},
    {2,0,-2,0, 58793, 246158, 0},
    {2,-1,-1,0, 57066, -152138, 0},
    {2,0,1,0, 53322, -170733, 0},
    {2,-1,0,0, 45758, -204586, 0},
    {0,1,-1,0, -40923, -129620, 0},
    {1,0,0,0, -34720, 108743, 0},
    {0,1,1,0, -30383, 104755, 0},
    {2,0,0,-2, 15327, 10321, 0},
    {0,0,1,2, -12528, 0, 0},
    {0,0,1,-2, 10980, 79661, 0},
    {4,0,-1,0, 10675, -34782, 0},
    {0,0,3,0, 10034, -23210, 0},
    {4,0,-2,0, 8548, -21636, 0},
    {2,1,-1,0, -7888, 24208, 0},
    {2,1,0,0, -6766, 30824, 0},
    {1,0,-1,0, -5163, -8379, 0},
    {1,1,0,0, 4987, -16675, 0},
    {2,-1,1,0, 4036, -12831, 0},
    {2,0,2,0, 3994, -10445, 0},
    {4,0,0,0, 3861, -11650, 0},
    {2,0,-3,0, 3665, 14403, 0},
    {0,1,-2,0, -2689, -7003, 0},
    {2,0,-1,2, -2602, 0, 0},
    {2,-1,-2,0, 2390, 10056, 0},
    {1,0,1,0, -2348, 6322, 0},
    {2,-2,0,0, 2236, -9884, 0},
    {0,1,2,0, -2120, 5751, 0},
    {0,2,0,0, -2069, 0, 0},
    {2,-2,-1,0, 2048, -4950, 0},
    {2,0,1,-2, -1773, 4130, 0},
    {2,0,0,2, -1595, 0, 0},
    {4,-1,-1,0, 1215, -3958, 0},
    {0,0,2,2, -1110, 0, 0},
    {3,0,-1,0, -892, 3258, 0},
    {2,1,1,0, -810, 2616, 0},
    {4,-1,-2,0, 759, -1897, 0},
    {0,2,-1,0, -713, -2117, 0},
    {2,2,-1,0, -700, 2354, 0},
    {2,1,-2,0, 691, 0, 0},
    {2,-1,0,-2, 596, 0, 0},
    {4,0,1,0, 549, -1423, 0},
    {0,0,4,0, 537, -1117, 0},
    {4,-1,0,0, 520, -1571, 0},
    {1,0,-2,0, -487, -1739, 0},
    {2,1,0,-2, -399, 0, 0},
    {0,0,2,-2, -381, -4421, 0},
    {1,1,1,0, 351, 0, 0},
    {3,0,-2,0, -340, 0, 0},
    {4,0,-3,0, 330, 0, 0},
    {2,-1,2,0, 327, 0, 0},
    {0,2,1,0, -323, 1165, 0},
    {1,1,-1,0, 299, 0, 0},
    {2,0,3,0, 294, 0, 0},
    {2,0,-1,-2, 0, 8752, 0},
    // 黄纬项（仅 b）
    {0,0,0,1, 0, 0, 5128122},
    {0,0,1,1, 0, 0, 280602},
    {0,0,1,-1, 0, 0, 277693},
    {2,0,0,-1, 0, 0, 173237},
    {2,0,-1,1, 0, 0, 55413},
    {2,0,-1,-1, 0, 0, 46271},
    {2,0,0,1, 0, 0, 32573},
    {0,0,2,1, 0, 0, 17198},
    {2,0,1,-1, 0, 0, 9266},
    {0,0,2,-1, 0, 0, 8822},
    {2,-1,0,-1, 0, 0, 8216},
    {2,0,-2,-1, 0, 0, 4324},
    {2,0,1,1, 0, 0, 4200},
    {2,1,0,-1, 0, 0, -3359},
    {2,-1,-1,1, 0, 0, 2463},
    {2,-1,0,1, 0, 0, 2211},
    {2,-1,-1,-1, 0, 0, 2065},
    {0,1,-1,-1, 0, 0, -1870},
    {4,0,-1,-1, 0, 0, 1828},
    {0,1,0,1, 0, 0, -1794},
    {0,0,0,3, 0, 0, -1749},
    {0,1,-1,1, 0, 0, -1565},
    {1,0,0,1, 0, 0, -1491},
    {0,1,1,1, 0, 0, -1475},
    {0,1,1,-1, 0, 0, -1410},
    {0,1,0,-1, 0, 0, -1344},
    {1,0,0,-1, 0, 0, -1335},
    {0,0,3,1, 0, 0, 1107},
    {4,0,0,-1, 0, 0, 1021},
    {4,0,-1,1, 0, 0, 833},
    {0,0,1,-3, 0, 0, 777},
    {4,0,-2,1, 0, 0, 671},
    {2,0,0,-3, 0, 0, 607},
    {2,0,2,-1, 0, 0, 596},
    {2,-1,1,-1, 0, 0, 491},
    {2,0,-2,1, 0, 0, -451},
    {0,0,3,-1, 0, 0, 439},
    {2,0,2,1, 0, 0, 422},
    {2,0,-3,-1, 0, 0, 421},
    {2,1,-1,1, 0, 0, -366},
    {2,1,0,1, 0, 0, -351},
    {4,0,0,1, 0, 0, 331},
    {2,-1,1,1, 0, 0, 315},
    {2,-2,0,-1, 0, 0, -297},
    {0,1,2,-1, 0, 0, 284},
    {2,0,-3,1, 0, 0, 281},
    {2,-1,0,-3, 0, 0, 265},
    {4,-1,0,-1, 0, 0, 241},
    {4,0,1,1, 0, 0, 218},
    {0,0,2,2, 0, 0, -211},
};

// 由儒略日求月球地心赤道坐标（raH 小时、decDeg 度）与相位（illum 0..1，phaseDeg 0=新月..180=满月）
static void MoonEquatorial(double jd, double& raH, double& decDeg,
                           double& illum, double& distKm, double& elongDeg,
                           double& sunRaH, double& sunDecD) {
    double T = (jd - 2451545.0) / 36525.0;
    double T2 = T*T, T3 = T2*T, T4 = T3*T;
    double Lp = 218.3164477 + 481267.88123421*T - 0.0015786*T2 + T3/538841.0 - T4/65194000.0;
    double D  = 297.8501921 + 445267.1114034*T - 0.0018819*T2 + T3/545868.0 - T4/113065000.0;
    double M  = 357.5291092 + 35999.0502909*T - 0.0001536*T2 + T3/24490000.0;
    double Mp = 134.9633964 + 477198.8675055*T + 0.0087414*T2 + T3/69699.0 - T4/14712000.0;
    double F  = 93.2720950 + 483202.0175233*T - 0.0036539*T2 - T3/3526000.0 + T4/863310000.0;
    double A1 = 119.75 + 131.849*T;
    double A2 = 53.09 + 479264.290*T;
    double E  = 1.0 - 0.002516*T - 0.0000074*T2;

    double Dr = D*D2R, Mr = M*D2R, Mpr = Mp*D2R, Fr = F*D2R;
    double sumL = 0, sumR = 0, sumB = 0;
    for (const auto& t : kMoonTerms) {
        double arg = t.d*Dr + t.m*Mr + t.mp*Mpr + t.f*Fr;
        double e = (t.m == 0) ? 1.0 : ((std::abs(t.m) == 1) ? E : E*E);
        sumL += t.l * e * std::sin(arg);
        sumR += t.r * e * std::cos(arg);
        sumB += t.b * e * std::sin(arg);
    }
    double lambda = Lp + sumL/1e6
                  + (3958.0*std::sin(A1*D2R) + 1962.0*std::sin((Lp-F)*D2R) + 318.0*std::sin(A2*D2R))/1e6;
    double beta = sumB/1e6;
    distKm = 385000.56 + sumR/1e3;

    // 黄道 -> 赤道
    double eps = (23.439291111 - 0.013004167*T) * D2R;
    double lr = lambda*D2R, br = beta*D2R;
    double xe = std::cos(br)*std::cos(lr);
    double ye = std::cos(br)*std::sin(lr);
    double ze = std::sin(br);
    double ce = std::cos(eps), se = std::sin(eps);
    double xe2 = xe;
    double ye2 = ye*ce - ze*se;
    double ze2 = ye*se + ze*ce;
    raH = std::atan2(ye2, xe2)/H2R;
    if (raH < 0) raH += 24.0;
    decDeg = std::asin(Clamp(ze2, -1.0, 1.0))/D2R;

    // 相位：太阳视黄经（简化）
    double L0 = 280.46646 + 36000.76983*T + 0.0003032*T2;
    double Ms = 357.52911 + 35999.05029*T - 0.0001537*T2;
    double Cs = (1.914602 - 0.004817*T)*std::sin(Ms*D2R)
              + 0.019993*std::sin(2*Ms*D2R)
              + 0.000289*std::sin(3*Ms*D2R);
    double lambdaSun = L0 + Cs;
    double elong = std::fmod(lambda - lambdaSun, 360.0);
    if (elong < 0) elong += 360.0;
    illum = (1.0 - std::cos(elong*D2R)) * 0.5;
    elongDeg = elong;                       // 月日角距 0..360（<180 为盈，>180 为亏）

    // 太阳地心赤道坐标（用于确定月亮亮面朝向）
    double lrS = lambdaSun * D2R;
    double xeS = std::cos(lrS);
    double yeS = std::sin(lrS) * ce;
    double zeS = std::sin(lrS) * se;
    sunRaH = std::atan2(yeS, xeS) / H2R;
    if (sunRaH < 0) sunRaH += 24.0;
    sunDecD = std::asin(Clamp(zeS, -1.0, 1.0)) / D2R;
}
// 月相名称（illum=照度 0..1，phaseDeg 0=新月 / 180=满月）
static const wchar_t* MoonPhaseName(double illum, double phaseDeg) {
    const bool waxing = phaseDeg < 180.0;
    if (illum < 0.03) return L"新月";
    if (illum > 0.97) return L"满月";
    if (illum < 0.47) return waxing ? L"蛾眉月" : L"残月";
    if (illum > 0.53) return waxing ? L"盈凸月" : L"亏凸月";
    return waxing ? L"上弦月" : L"下弦月";
}
static void MoonLabel(wchar_t* buf, size_t n, double illum, double phaseDeg) {
    swprintf_s(buf, n, L"月亮 · %ls", MoonPhaseName(illum, phaseDeg));
}

// ------------------------------------------------------------------ 行星位置
// JPL / Standish《Keplerian Elements for Approximate Positions of the Major Planets》
// 1800–2050 年适用：日心黄道 J2000 根数（a[AU]、角度制）+ 每儒略世纪线性变化率。
// 误差约 1′（内行星）~ 数′（外行星），对可视化完全够用，无需任何外部星历文件。
struct PlanetElem { double a, e, I, L, peri, node; };
static const PlanetElem kPlanetElem[8] = {
    {  0.38709927, 0.20563593,  7.00497902,  252.25032350,  77.45779628,  48.33076593 }, // 水星
    {  0.72333566, 0.00677672,  3.39467605,  181.97909950, 131.60246718,  76.67984255 }, // 金星
    {  1.52371034, 0.09339410,  1.84969142,   -4.55343205, -23.94362959,  49.55953891 }, // 火星
    {  5.20288700, 0.04838624,  1.30439695,   34.39644051,  14.72847983, 100.47390909 }, // 木星
    {  9.53667594, 0.05386179,  2.48599187,   49.95424423,  92.59887831, 113.66242448 }, // 土星
    { 19.18916464, 0.04725744,  0.77263783,  313.23810451, 170.95427630,  74.01692503 }, // 天王星
    { 30.06992276, 0.00859048,  1.77004347,  -55.12002969,  44.96476227, 131.78422574 }, // 海王星
    {  1.00000261, 0.01671123, -0.00001531,  100.46457166, 102.93768193,   0.0        }, // 地球（月地质心）
};
static const PlanetElem kPlanetRate[8] = {
    { 0.00000037, 0.00001906, -0.00594749, 149472.67411175,  0.16047689, -0.12534081 },
    { 0.00000390, -0.00004107, -0.00078890, 58517.81538729,  0.00268329, -0.27769418 },
    { 0.00001847, 0.00007882, -0.00813131, 19140.30268499,  0.44441088, -0.29257343 },
    { -0.00011607, -0.00013253, -0.00183714, 3034.74612775,  0.21252668,  0.20469106 },
    { -0.00125060, -0.00050991,  0.00193609, 1222.49362201, -0.41897216, -0.28867794 },
    { -0.00196176, -0.00004397, -0.00242939,  428.48202785,  0.40805281,  0.04240589 },
    {  0.00026291, 0.00005105,  0.00035372,  218.45945325, -0.32241464, -0.00508664 },
    {  0.00000562, -0.00004392, -0.01294668, 35999.37244981,  0.32327364,  0.0        },
};

static const wchar_t* kPlanetNames[7] = {
    L"水星", L"金星", L"火星", L"木星", L"土星", L"天王星", L"海王星"
};
static const wchar_t* kPlanetNamesEn[7] = {
    L"Mercury", L"Venus", L"Mars", L"Jupiter", L"Saturn", L"Uranus", L"Neptune"
};
static const Gdiplus::Color kPlanetColors[7] = {
    Gdiplus::Color(255, 214, 210, 202),   // 水星：灰
    Gdiplus::Color(255, 255, 246, 208),   // 金星：淡黄
    Gdiplus::Color(255, 255, 138,  96),   // 火星：橙红
    Gdiplus::Color(255, 246, 228, 188),   // 木星：奶油
    Gdiplus::Color(255, 240, 220, 150),   // 土星：金黄
    Gdiplus::Color(255, 150, 230, 235),   // 天王星：青
    Gdiplus::Color(255, 120, 165, 255),   // 海王星：蓝
};

// 日心黄道 J2000 直角坐标（AU）；idx 0..6 为行星，7 为地球。T 为自 J2000 起的儒略世纪数。
static void HelioEcliptic(int idx, double T, double& x, double& y, double& z, double& r) {
    const PlanetElem& e0 = kPlanetElem[idx];
    const PlanetElem& ed = kPlanetRate[idx];
    double a    = e0.a    + ed.a    * T;
    double ecc  = e0.e    + ed.e    * T;
    double I    = e0.I    + ed.I    * T;
    double L    = e0.L    + ed.L    * T;
    double peri = e0.peri + ed.peri * T;
    double node = e0.node + ed.node * T;

    double M = L - peri;                      // 平近点角，归一到 (-180, 180]
    M = std::fmod(M, 360.0);
    if (M >  180.0) M -= 360.0;
    if (M < -180.0) M += 360.0;
    const double Mr = M * D2R;

    double E = Mr + ecc * std::sin(Mr);        // 牛顿迭代解开普勒方程
    for (int it = 0; it < 12; ++it) {
        double dE = (E - ecc * std::sin(E) - Mr) / (1.0 - ecc * std::cos(E));
        E -= dE;
        if (std::fabs(dE) < 1e-12) break;
    }
    const double xp = a * (std::cos(E) - ecc);                                  // 轨道平面
    const double yp = a * std::sqrt(max(0.0, 1.0 - ecc * ecc)) * std::sin(E);

    const double w  = (peri - node) * D2R;     // 近日点幅角
    const double nr = node * D2R;
    const double ir = I * D2R;
    const double cw = std::cos(w),  sw = std::sin(w);
    const double cO = std::cos(nr), sO = std::sin(nr);
    const double ci = std::cos(ir), si = std::sin(ir);

    const double u = xp * cw - yp * sw;        // 绕 ω 旋转
    const double v = xp * sw + yp * cw;
    x = u * cO - v * ci * sO;                  // 倾角 I + 升交点 Ω
    y = u * sO + v * ci * cO;
    z = v * si;
    r = std::sqrt(x*x + y*y + z*z);
}

// 地平坐标 -> 赤道坐标。az 自北向东(0=N,90=E,180=S,270=W)，alt 高度角；lat 纬度；lstDeg 本地恒星时(度)
static void HorizonToEquatorial(double azDeg, double altDeg, double latDeg, double lstDeg,
                                double& raHours, double& decDeg) {
    double az = azDeg * D2R, alt = altDeg * D2R, lat = latDeg * D2R, lst = lstDeg * D2R;
    double sinDec = std::sin(alt)*std::sin(lat) + std::cos(alt)*std::cos(lat)*std::cos(az);
    double dec = std::asin(Clamp(sinDec, -1.0, 1.0));
    double cosDec = std::cos(dec), cosLat = std::cos(lat);
    double sinH, cosH;
    if (cosDec < 1e-9 || cosLat < 1e-9) { sinH = 0; cosH = 0; }
    else {
        sinH = -std::cos(alt) * std::sin(az) / cosDec;
        cosH = (std::sin(alt) - std::sin(lat)*sinDec) / (cosLat * cosDec);
    }
    double H = std::atan2(sinH, cosH);      // 时角（东负西正）
    raHours = (lst - H) / H2R;
    decDeg  = dec / D2R;
    raHours = std::fmod(raHours, 24.0);
    if (raHours < 0) raHours += 24.0;
}
// 赤道坐标 -> 地平坐标（HorizonToEquatorial 的逆运算，约定完全对称）
static void EquatorialToHorizon(double raHours, double decDeg, double latDeg, double lstDeg,
                                double& azDeg, double& altDeg) {
    double ra = raHours * H2R, d = decDeg * D2R, lat = latDeg * D2R, lst = lstDeg * D2R;
    double H = lst - ra;                                   // 时角（东负西正）
    double sinAlt = std::sin(d)*std::sin(lat) + std::cos(d)*std::cos(lat)*std::cos(H);
    altDeg = std::asin(Clamp(sinAlt, -1.0, 1.0)) * R2D;
    double ca = std::cos(altDeg * D2R), cl = std::cos(lat);
    if (ca < 1e-9 || cl < 1e-9) { azDeg = 0.0; return; }    // 天顶/极点附近方位无定义
    double sinAz = -std::cos(d) * std::sin(H) / ca;
    double cosAz = (std::sin(d) - std::sin(lat)*sinAlt) / (ca * cl);
    azDeg = std::atan2(sinAz, cosAz) * R2D;
    if (azDeg < 0) azDeg += 360.0;
    if (azDeg >= 360.0) azDeg -= 360.0;                    // -0.0 之类会算出 360，归一成 0
}

static Vec3 HorizonToVec(double azDeg, double altDeg, double latDeg, double lstDeg) {
    double raH, decD;
    HorizonToEquatorial(azDeg, altDeg, latDeg, lstDeg, raH, decD);
    return RaDecToVec(raH, decD);
}

// ------------------------------------------------------------------ 生命周期
bool CelestialSphere::Register(HINSTANCE hInst) {
    if (!g_gdiToken) {
        GdiplusStartupInput si;
        if (GdiplusStartup(&g_gdiToken, &si, nullptr) != Gdiplus::Ok) return false;
    }
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &CelestialSphere::WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = TQ_SPHERE_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    return true;
}

void CelestialSphere::Shutdown() {
    if (g_gdiToken) { GdiplusShutdown(g_gdiToken); g_gdiToken = 0; }
}

CelestialSphere::CelestialSphere() { SetViewRA(6.0, 10.0, 0.0); }
CelestialSphere::~CelestialSphere() { Destroy(); }

bool CelestialSphere::Create(HWND parent, int x, int y, int w, int h, int ctrlId, HINSTANCE hInst) {
    hInst_ = hInst;
    hwnd_ = CreateWindowExW(0, TQ_SPHERE_CLASS, L"",
                            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                            x, y, w, h, parent, (HMENU)(INT_PTR)ctrlId, hInst, this);
    if (!hwnd_) return false;
    // 进程已开 DPI 感知，客户区是物理像素；字体与天体半径按同一比例放大，
    // 否则 125% 缩放下标注/行星会比窗口"变小一号"。
    {
        HDC dc = GetDC(hwnd_);
        const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) ReleaseDC(hwnd_, dc);
        dpiScale_ = (dpi > 0 ? dpi : 96) / 96.0;
    }
    RECT rc; GetClientRect(hwnd_, &rc);
    width_ = rc.right; height_ = rc.bottom;
    RecomputeGeometry();
    return true;
}

void CelestialSphere::Destroy() {
    if (hwnd_) { StopAnimation(); DestroyWindow(hwnd_); hwnd_ = nullptr; }
    delete reinterpret_cast<Bitmap*>(bgCache_);    bgCache_ = nullptr;
    delete reinterpret_cast<Bitmap*>(frameCache_); frameCache_ = nullptr;
    delete reinterpret_cast<Bitmap*>(starLayer_);  starLayer_ = nullptr;
}

// ------------------------------------------------------------------ 几何
void CelestialSphere::SetViewRA(double ra, double dec, double rollDeg) {
    double a = ra * H2R;
    double d = Clamp(dec, -90.0, 90.0) * D2R;
    double r = rollDeg * D2R;
    double sa = std::sin(a), ca = std::cos(a);
    double sd = std::sin(d), cd = std::cos(d);
    double base[3][3] = {
        { -sa,        ca,        0.0 },
        { -sd*ca,    -sd*sa,    cd  },
        {  cd*ca,     cd*sa,    sd  }
    };
    double cr = std::cos(r), sr = std::sin(r);
    double rz[3][3] = { { cr, -sr, 0 }, { sr, cr, 0 }, { 0, 0, 1 } };
    MulMat(rz, base, R_);
    if (inside_) {
        // 球心视角：相机在球心朝外看 center 方向的内表面。
        // 旧实现用固定 flip(diag(-1,1,-1)) 让“面朝南”时左=东右=西，但该翻转只对南北朝向
        // 正确；面朝东西时 up 取的是“赤纬/北极方向”而非天顶，使 right(=up×forward) 偏离
        // 真实右手边地平方向，方位显示错位（“部分方位不正确”）。
        // 这里改用真实几何：forward F = -center（使 Proj 的 q.z<0 可见），
        // up = 天顶方向在 ⟂F 平面的投影，right = up×F（对应地平方位 az+90），
        // 再绕 F 轴施加 roll，与球外视角对称。
        double v0x = cd*ca, v0y = cd*sa, v0z = sd;          // center 世界向量（不受 roll 影响）
        double F[3] = { -v0x, -v0y, -v0z };
        double tl = obsLat_ * D2R, tlst = LSTdeg() * D2R;   // 天顶（赤经=LST，赤纬=纬度）
        double vZx = std::cos(tl)*std::cos(tlst), vZy = std::cos(tl)*std::sin(tlst), vZz = std::sin(tl);
        double dotF = F[0]*vZx + F[1]*vZy + F[2]*vZz;
        double Un[3] = { vZx - dotF*F[0], vZy - dotF*F[1], vZz - dotF*F[2] };
        double ulen = std::sqrt(Un[0]*Un[0] + Un[1]*Un[1] + Un[2]*Un[2]);
        if (ulen > 1e-9) { Un[0]/=ulen; Un[1]/=ulen; Un[2]/=ulen; }
        double R0[3] = { Un[1]*F[2]-Un[2]*F[1], Un[2]*F[0]-Un[0]*F[2], Un[0]*F[1]-Un[1]*F[0] };
        if (R0[0]*(Un[1]*F[2]-Un[2]*F[1]) - R0[1]*(Un[0]*F[2]-Un[2]*F[0]) + R0[2]*(Un[0]*F[1]-Un[1]*F[0]) < 0)
            { R0[0]=-R0[0]; R0[1]=-R0[1]; R0[2]=-R0[2]; }   // 保证 (R0,Un,F) 同手性
        // 绕 F 轴施加 roll：R = R0*cos(r) + Un*sin(r); U = Un*cos(r) - R0*sin(r)
        double Rf[3] = { R0[0]*cr + Un[0]*sr, R0[1]*cr + Un[1]*sr, R0[2]*cr + Un[2]*sr };
        double Uf[3] = { Un[0]*cr - R0[0]*sr, Un[1]*cr - R0[1]*sr, Un[2]*cr - R0[2]*sr };
        R_[0][0]=Rf[0]; R_[0][1]=Rf[1]; R_[0][2]=Rf[2];
        R_[1][0]=Uf[0]; R_[1][1]=Uf[1]; R_[1][2]=Uf[2];
        R_[2][0]=F[0];  R_[2][1]=F[1];  R_[2][2]=F[2];
        }
        roll_ = rollDeg;
        SyncObserverFromView();   // 视线变了就把方位/高度角同步过去，自转时才能从新方向继续
        }

void CelestialSphere::GetViewRA(double& ra, double& dec, double& rollDeg) const {
    if (!inside_) {
        // 球外视角：R_ = rz*base，直接反推（中心=F=base 第2行，up=base 第1行）。
        double v2[3] = { R_[2][0], R_[2][1], R_[2][2] };
        double v0[3] = { R_[0][0], R_[0][1], R_[0][2] };
        dec = std::asin(Clamp(v2[2], -1.0, 1.0)) * R2D;
        ra  = std::atan2(v2[1], v2[0]) * R2D / 15.0;
        if (ra < 0) ra += 24.0;
        double a = ra * H2R, d = dec * D2R;
        double sa = std::sin(a), ca = std::cos(a), sd = std::sin(d), cd = std::cos(d);
        double dotR = v0[0]*(-sa) + v0[1]*(ca);
        double dotU = v0[0]*(-sd*ca) + v0[1]*(-sd*sa) + v0[2]*(cd);
        rollDeg = std::atan2(-dotU, dotR) * R2D;
        return;
    }
    // 球心视角：与 SetViewRA 的内部构造对称，用天顶基反推（不再依赖固定 flip）。
    double v0[3] = { -R_[2][0], -R_[2][1], -R_[2][2] };   // F = -center
    double decv = std::asin(Clamp(v0[2], -1.0, 1.0)) * R2D;
    double rav = std::atan2(v0[1], v0[0]) * R2D / 15.0;
    if (rav < 0) rav += 24.0;
    double d = decv * D2R, a = rav * H2R;
    double cd = std::cos(d), sd = std::sin(d), ca = std::cos(a), sa = std::sin(a);
    double F[3] = { -cd*ca, -cd*sa, -sd };
    double tl = obsLat_ * D2R, tlst = LSTdeg() * D2R;
    double vZx = std::cos(tl)*std::cos(tlst), vZy = std::cos(tl)*std::sin(tlst), vZz = std::sin(tl);
    double dotF = F[0]*vZx + F[1]*vZy + F[2]*vZz;
    double Un[3] = { vZx - dotF*F[0], vZy - dotF*F[1], vZz - dotF*F[2] };
    double ulen = std::sqrt(Un[0]*Un[0] + Un[1]*Un[1] + Un[2]*Un[2]);
    if (ulen > 1e-9) { Un[0]/=ulen; Un[1]/=ulen; Un[2]/=ulen; }
    double Rb[3] = { Un[1]*F[2]-Un[2]*F[1], Un[2]*F[0]-Un[0]*F[2], Un[0]*F[1]-Un[1]*F[0] };
    if (Rb[0]*(Un[1]*F[2]-Un[2]*F[1]) - Rb[1]*(Un[0]*F[2]-Un[2]*F[0]) + Rb[2]*(Un[0]*F[1]-Un[1]*F[0]) < 0)
        { Rb[0]=-Rb[0]; Rb[1]=-Rb[1]; Rb[2]=-Rb[2]; }
    double dr0 = R_[0][0]*Un[0] + R_[0][1]*Un[1] + R_[0][2]*Un[2];   // R0m·Un
    double drb = R_[0][0]*Rb[0] + R_[0][1]*Rb[1] + R_[0][2]*Rb[2];   // R0m·Rb
    ra = rav; dec = decv; rollDeg = std::atan2(dr0, drb) * R2D;
}

double CelestialSphere::LSTdeg() const {
    double jd = JulianDate(obsYear_, obsMonth_, obsDay_, obsHour_ - 8.0);  // 北京时区 UTC+8
    double v = std::fmod(GMSTdeg(jd) + obsLon_, 360.0);
    if (v < 0) v += 360.0;
    return v;
}

void CelestialSphere::PointToObserverSky() {
    double lst = LSTdeg();
    double raH = 0, decD = 0;
    // 方位/高度角取当前视线（默认 35°，拖动后跟随视图），roll 一并保留，
    // 否则自转每 33ms 重新定向一次会把拖动带来的倾斜抹掉。
    HorizonToEquatorial(obsAz_, obsAlt_, obsLat_, lst, raH, decD);
    SetViewRA(raH, decD, roll_);   // 两种视角下 SetViewRA 均以传入方向为屏幕中心
}

// 视图方向反算成观测方位/高度角。自转计时器按 obsAz_/obsAlt_ 重新定向，
// 拖动若不回写这两个值，下一次 tick 就会把拖动的结果覆盖掉（表现为"拖不动"）。
void CelestialSphere::SyncObserverFromView() {
    if (!hasObserver_) return;
    double ra, dec, roll;
    GetViewRA(ra, dec, roll);
    double az = 0, alt = 0;
    EquatorialToHorizon(ra, dec, obsLat_, LSTdeg(), az, alt);
    if (std::isfinite(az) && std::isfinite(alt)) { obsAz_ = az; obsAlt_ = alt; }
}

void CelestialSphere::SetObserver(const SphereObserver& o) {
    obsLat_ = o.lat; obsLon_ = o.lon;
    obsYear_ = o.year; obsMonth_ = o.month; obsDay_ = o.day; obsHour_ = o.hour;
    obsAz_ = o.azDeg;
    obsAlt_ = 35.0;      // 显式选定方向时回到标准仰角
    roll_ = 0.0;         // 新方向不沿用旧视角的倾斜
    hasObserver_ = true;
    PointToObserverSky();
    InvalidateRect(hwnd_, nullptr, FALSE);
    SendNotify(TQN_VIEWCHANGED, -1);
}

SphereObserver CelestialSphere::GetObserver() const {
    return SphereObserver{ obsLat_, obsLon_, obsYear_, obsMonth_, obsDay_, obsHour_, obsAz_ };
}

void CelestialSphere::SetAutoSpin(bool on) {
    if (on == autoSpin_) return;
    autoSpin_ = on;
    if (on) {
        lastSpinTick_ = GetTickCount64();
        SetTimer(hwnd_, 2, 33, nullptr);   // ~30fps 自转步进
    } else {
        KillTimer(hwnd_, 2);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    SendNotify(TQN_VIEWCHANGED, -1);
}

bool CelestialSphere::IsAutoSpin() const { return autoSpin_; }

void CelestialSphere::SetSpinRate(double rate) {
    if (rate <= 0) rate = 1.0;
    spinRate_ = rate;
}

double CelestialSphere::GetSpinRate() const { return spinRate_; }

void CelestialSphere::RotateBy(double dx, double dy) {
    // 球心视角下相机看的是天球内表面（对面），同一个旋转增量在内外表面
    // 上星点的屏幕移动方向相反；取反输入，使球内拖动手感与外部一致
    //（像在球心朝鼠标方向转头看，而不是隔着球去"抓"对面的天球）。
    if (inside_) { dx = -dx; dy = -dy; }
    // 旋转增益：球外按球盘半径（既有手感）；球心视角按 gnomonic 焦距，拖动 ≈ 1:1
    // 抓住天区。不能用 radius_——它跟的是外部视角的 zoom_（切进球心时被设成 14），
    // 会让球心视角的拖动僵硬到几乎不动（600px 才转过十几度）。
    const double k = inside_ ? 1.0 / max(80.0, innerFocal_)
                             : PI / (2.0 * max(80.0, radius_));
    double ay = dx * k, ax = dy * k;
    double ca2 = std::cos(ay), sa2 = std::sin(ay);
    double cx2 = std::cos(ax), sx2 = std::sin(ax);
    double ry[3][3] = { { ca2, 0, sa2 }, { 0, 1, 0 }, { -sa2, 0, ca2 } };
    double rx[3][3] = { { 1, 0, 0 }, { 0, cx2, -sx2 }, { 0, sx2, cx2 } };
    double tmp[3][3];
    MulMat(ry, rx, tmp);
    MulMat(tmp, R_, R_);
    double ra, dec, roll; GetViewRA(ra, dec, roll); roll_ = roll;
    SyncObserverFromView();
}

void CelestialSphere::RecomputeGeometry() {
    cx_ = width_ * 0.5;
    cy_ = height_ * 0.5;
    const double baseFocal = min(width_, height_) * 0.44 *
                             std::sqrt(kCamDist*kCamDist - 1.0);
    radius_ = min(width_, height_) * 0.44 * zoom_;
    focal_  = baseFocal * zoom_;                    // 外部视角：等价于 radius_*sqrt(C²-1)

    // 球心视角用 gnomonic 投影：focal = (短边/2) / tan(FOV/2)。
    // 初始约 75° 视场，只显示正前方天区（而非整个前半球）；
    // 继续滚轮放大 innerZoom 增大，视场随之收窄。
    innerFocal_ = min(width_, height_) * 0.65 * innerZoom_;
}

CelestialSphere::Proj CelestialSphere::Project(const Vec3& v) const {
    Proj p;
    if (focal_ <= 0) return p;
    Vec3 q = MulR(R_, v);
    if (inside_) {
        // 视点穿过球面到达球心后，视线仍朝向进入前的 -z 方向，
        // 因此看到的是与球外视角"面对面"的那半边天球（远端半球 p.z<0），
        // 而不是球外看到的近端半球。gnomonic（球心透视）投影。
        if (q.z >= -0.02) return p;                       // 可见 p.z<0 的远端半球
        double k = innerFocal_ / (-q.z);
        p.visible = true;
        p.x = cx_ + panX_ + q.x * k;
        p.y = cy_ + panY_ - q.y * k;
        p.z = q.z;
        p.rel = 1.0;
        return p;
    }
    if (q.z <= 1.0 / kCamDist) return p;                 // 球外：切点以外的背面
    double k = focal_ / (kCamDist - q.z);
    p.visible = true;
    p.x = cx_ + panX_ + q.x * k;
    p.y = cy_ + panY_ - q.y * k;
    p.z = q.z;
    p.rel = (kCamDist - 1.0) / (kCamDist - q.z);         // 中心=1，边缘略小
    return p;
}

bool CelestialSphere::ScreenToSphere(double sx, double sy, Vec3& world) const {
    if (focal_ <= 0) return false;
    if (inside_) {
        // 球心视角朝 -z 看，gnomonic 反投影：视线方向 (X, Y, -1) 与单位球交点
        double X = (sx - cx_ - panX_) / innerFocal_;
        double Y = -(sy - cy_ - panY_) / innerFocal_;
        double n = std::sqrt(X*X + Y*Y + 1.0);
        Vec3 p = { X / n, Y / n, -1.0 / n };
        world = MulRT(R_, p);
        return true;
    }
    double X = (sx - cx_ - panX_) / focal_;
    double Y = -(sy - cy_ - panY_) / focal_;
    double a = X*X + Y*Y + 1.0;
    double b = -2.0 * kCamDist;
    double c = kCamDist*kCamDist - 1.0;
    double disc = b*b - 4.0*a*c;
    if (disc < 0) return false;
    double t = (-b - std::sqrt(disc)) / (2.0*a);         // 近侧交点
    Vec3 p = { t*X, t*Y, kCamDist - t };
    double n = std::sqrt(p.x*p.x + p.y*p.y + p.z*p.z);
    if (n < 1e-9) return false;
    p.x /= n; p.y /= n; p.z /= n;
    world = MulRT(R_, p);
    return true;
}

int CelestialSphere::HitTest(int mx, int my) const {
    int best = -1;
    const double hitR = 14.0 * dpiScale_;      // 命中半径随 DPI 放大，手感保持一致
    double bestD = hitR * hitR;
    const int lim = (int)min((size_t)max(0, opt_.maxStars), data_.stars.size());
    for (int i = 0; i < lim; ++i) {
        const Star& s = data_.stars[i];
        if (s.mag > opt_.limitingMag) break;
        Proj p = Project(s.v);
        if (!p.visible) continue;
        double dx = p.x - mx, dy = p.y - my;
        double d = dx*dx + dy*dy;
        if (d < bestD) { bestD = d; best = i; }
    }
    // 行星（动态天体，命中范围略大，优先于恒星）
    if (opt_.showPlanets) {
        for (int i = 0; i < kPlanetCount; ++i) {
            if (!planets_[i].valid) continue;
            Proj p = Project(planets_[i].v);
            if (!p.visible) continue;
            if (inside_ && (-p.z) < 0.05) continue;      // 与 DrawPlanets 的剔除条件保持一致
            double rr = PlanetRadius(i) + 7.0;           // 小盘面 + 抖动余量
            double dx = p.x - mx, dy = p.y - my;
            if (dx*dx + dy*dy <= rr*rr) return TQ_PLANET_BASE - i;
        }
    }
    // 月亮（动态大天体，优先于恒星）：命中区覆盖月盘、外圈柔光以及右侧「月亮 · 月相」标签
    if (opt_.showMoon && moonValid_) {
        Proj p = Project(moonV_);
        if (p.visible && !(inside_ && (-p.z) < 0.05)) {
            const double r = MoonRadius();
            const double dx = p.x - mx, dy = p.y - my;
            const double rr = r * 2.6 + 8.0;             // 外圈柔光半径 3.2r，取 2.6r 兼顾不抢占邻星
            if (dx*dx + dy*dy <= rr*rr) return TQ_MOON_INDEX;

            wchar_t lb[48];
            MoonLabel(lb, 48, moonIllum_, moonPhaseDeg_);
            const double lbX = p.x + r + 3.0;
            const double lbY = p.y - 9.0;
            const double lbW = (double)wcslen(lb) * 12.0 + 8.0;
            const double lbH = 18.0;
            if (mx >= lbX && mx <= lbX + lbW && my >= lbY && my <= lbY + lbH)
                return TQ_MOON_INDEX;
        }
    }
    return best;
}

// 光标落在哪个星座的边界内。用 J2000 边界多边形做点在多边形内判定：
// 顶点 ra 已在加载时展平为连续值，这里把光标经度按最近的 24h 周期对齐后
// 用奇偶规则判定（Ser 这类双环星座两个环一起参与）。
int CelestialSphere::ConstellationAt(int mx, int my) const {
    if (data_.boundary.empty() || !opt_.showArt) return -1;
    Vec3 w;
    if (!ScreenToSphere((double)mx, (double)my, w)) return -1;
    double ra = std::atan2(w.y, w.x) * R2D / 15.0;
    if (ra < 0) ra += 24.0;
    const double dec = std::asin(Clamp(w.z, -1.0, 1.0)) * R2D;

    for (size_t i = 0; i < data_.boundary.size(); ++i) {
        const ConstBoundary& b = data_.boundary[i];
        if (dec < b.minDec || dec > b.maxDec) continue;
        for (int k = -1; k <= 1; ++k) {                  // 贴图可能跨 0h，三份副本都试
            const double r = ra + k * 24.0;
            if (r < b.minRa - 2.0 || r > b.maxRa + 2.0) continue;
            bool inside = false;
            for (const auto& ring : b.rings) {
                const size_t n = ring.size();
                for (size_t j = 0; j < n; ++j) {
                    double x0 = ring[j].first, y0 = ring[j].second;
                    double x1 = ring[(j + 1) % n].first, y1 = ring[(j + 1) % n].second;
                    while (x0 - r > 12.0)  { x0 -= 24.0; x1 -= 24.0; }
                    while (r - x0 > 12.0)  { x0 += 24.0; x1 += 24.0; }
                    if ((y0 > dec) == (y1 > dec)) continue;
                    const double t = (dec - y0) / (y1 - y0);
                    if (r < x0 + t * (x1 - x0)) inside = !inside;
                }
            }
            if (inside) return (int)i;
        }
    }
    return -1;
}

// 单击 / 双击命中：贴图模式下星座优先于天体（需求：点到天体上也显示星座信息）
int CelestialSphere::PickObject(int mx, int my) const {
    if (opt_.showArt) {
        const int c = ConstellationAt(mx, my);
        if (c >= 0) return TQ_CONST_BASE - c;
    }
    return HitTest(mx, my);
}

// ------------------------------------------------------------------ 通知
void CelestialSphere::SendNotify(int code, int index) {
    HWND parent = GetParent(hwnd_);
    if (!parent) return;
    SphereNotify n = {};
    n.hdr.hwndFrom = hwnd_;
    n.hdr.idFrom = (UINT_PTR)GetDlgCtrlID(hwnd_);
    n.hdr.code = code;
    n.starIndex = index;
    n.hip = 0;
    if (index >= 0 && index < (int)data_.stars.size()) {
        const Star& s = data_.stars[index];
        n.ra = s.ra; n.dec = s.dec; n.mag = s.mag; n.hip = s.hip;
        wcsncpy_s(n.name,    s.name.c_str(),   _TRUNCATE);
        wcsncpy_s(n.nameCn,  s.nameCn.c_str(), _TRUNCATE);
        wcsncpy_s(n.nameEn,  s.nameEn.c_str(), _TRUNCATE);
        std::wstring cn = ConstellationName(s.con);
        wcsncpy_s(n.con,     cn.c_str(),       _TRUNCATE);
        wcsncpy_s(n.spectral, Utf8ToWide(s.spectral).c_str(), _TRUNCATE);
        wcsncpy_s(n.info, s.info.c_str(), _TRUNCATE);
    }
    else if (index == TQ_MOON_INDEX && moonValid_) {
        wcsncpy_s(n.name, L"月亮", _TRUNCATE);
        wcsncpy_s(n.nameEn, L"Moon", _TRUNCATE);
        n.ra = moonRaH_; n.dec = moonDecD_;
        n.mag = -12.7;
        swprintf_s(n.info, L"类型\t地球卫星\n相位\t%ls\n照度\t%.0f%%\n距离\t%.0f km\n",
                   MoonPhaseName(moonIllum_, moonPhaseDeg_),
                   moonIllum_ * 100.0, moonDist_);
    }
    else if (index <= TQ_PLANET_BASE && index > TQ_PLANET_BASE - kPlanetCount) {
        const int i = TQ_PLANET_BASE - index;
        const PlanetState& ps = planets_[i];
        wcsncpy_s(n.name,   kPlanetNames[i],   _TRUNCATE);
        wcsncpy_s(n.nameEn, kPlanetNamesEn[i], _TRUNCATE);
        n.ra = ps.raH; n.dec = ps.decD; n.mag = ps.mag;
        swprintf_s(n.info, L"日心距离\t%.3f AU\n地心距离\t%.3f AU\n距角\t%.1f°\n",
                   ps.helioAU, ps.distAU, ps.elongDeg);
    }
    else if (index <= TQ_CONST_BASE) {
        // 星座：资料全塞进 info（标签\t值\n），父窗口统一按行渲染
        const int ci = TQ_CONST_BASE - index;
        if (ci >= 0 && ci < (int)data_.boundary.size()) {
            const ConstBoundary& b = data_.boundary[ci];
            const ConstInfo* info = FindConstInfo(data_, b.abbr);
            const std::wstring abbrW = Utf8ToWide(b.abbr);
            wcsncpy_s(n.name,   info && !info->nameCn.empty() ? info->nameCn.c_str() : abbrW.c_str(), _TRUNCATE);
            wcsncpy_s(n.nameCn, info ? info->nameCn.c_str()  : abbrW.c_str(), _TRUNCATE);
            wcsncpy_s(n.nameEn, info ? info->nameLat.c_str() : L"", _TRUNCATE);
            wcsncpy_s(n.con,    abbrW.c_str(), _TRUNCATE);
            if (info) {
                n.ra = info->labelRa; n.dec = info->labelDec;
                std::wstring s;
                s += L"缩写\t"     + abbrW;
                s += L"\n拉丁名\t" + info->nameLat;
                s += L"\n所有格\t" + info->genitive;
                s += L"\n含义\t"   + info->meaning;
                s += L"\n家族\t"   + info->family;
                s += L"\n文化根源\t" + info->origin;
                s += L"\n传说\t"   + info->myth;
                s += L"\n出处\t"   + info->history;
                wcsncpy_s(n.info, s.c_str(), _TRUNCATE);
            }
        }
    }
    // 悬停：贴图模式下顺带告知父窗口光标所在星座
    if (hoverConst_ >= 0 && hoverConst_ < (int)data_.boundary.size()) {
        const ConstInfo* hc = FindConstInfo(data_, data_.boundary[hoverConst_].abbr);
        wcsncpy_s(n.constName,
                  hc && !hc->nameCn.empty() ? hc->nameCn.c_str()
                                            : Utf8ToWide(data_.boundary[hoverConst_].abbr).c_str(),
                  _TRUNCATE);
    }
    double ra, dec, roll; GetViewRA(ra, dec, roll);
    n.centerRA = ra; n.centerDec = dec; n.zoom = zoom_;
    n.inside = inside_; n.innerZoom = innerZoom_;
    SendMessageW(parent, WM_NOTIFY, (WPARAM)n.hdr.idFrom, (LPARAM)&n);
}

// ------------------------------------------------------------------ 动画
void CelestialSphere::StartAnimation(const Vec3& target, double zoomTo) {
    double ra, dec, roll; GetViewRA(ra, dec, roll);
    animFrom_ = RaDecToVec(ra, dec);
    animTo_ = target;
    animT_ = 0.0;
    animZoomFrom_ = inside_ ? innerZoom_ : zoom_;
    animZoomTo_ = zoomTo;
    anim_ = true;
    SetTimer(hwnd_, 1, 25, nullptr);
}
void CelestialSphere::StopAnimation() {
    if (anim_) { KillTimer(hwnd_, 1); anim_ = false; }
}

// ------------------------------------------------------------------ 便捷 API
void CelestialSphere::SetData(const CatalogData& d)      { if (hwnd_) SendMessageW(hwnd_, TQSM_RELOAD, 0, (LPARAM)&d); }
void CelestialSphere::SetOptions(const SphereOptions& o) { if (hwnd_) SendMessageW(hwnd_, TQSM_SETOPTIONS, 0, (LPARAM)&o); }
SphereOptions CelestialSphere::GetOptions() const {
    SphereOptions o; if (hwnd_) SendMessageW(hwnd_, TQSM_GETOPTIONS, 0, (LPARAM)&o); return o;
}
void CelestialSphere::SetView(const SphereView& v) { if (hwnd_) SendMessageW(hwnd_, TQSM_SETVIEW, 0, (LPARAM)&v); }
SphereView CelestialSphere::GetView() const {
    SphereView v; if (hwnd_) SendMessageW(hwnd_, TQSM_GETVIEW, 0, (LPARAM)&v); return v;
}
void CelestialSphere::Reset() { if (hwnd_) SendMessageW(hwnd_, TQSM_RESET, 0, 0); }
SphereStats CelestialSphere::GetStats() const {
    SphereStats s; if (hwnd_) SendMessageW(hwnd_, TQSM_GETSTATS, 0, (LPARAM)&s); return s;
}
void CelestialSphere::Highlight(const std::wstring& kw) { if (hwnd_) SendMessageW(hwnd_, TQSM_HIGHLIGHT, 0, (LPARAM)kw.c_str()); }
void CelestialSphere::Goto(double ra, double dec, double zoom, bool animate) {
    SphereGoto g; g.ra = ra; g.dec = dec; g.zoom = zoom; g.animate = animate;
    if (hwnd_) SendMessageW(hwnd_, TQSM_GOTO, 0, (LPARAM)&g);
}

// ------------------------------------------------------------------ 批量线绘制
// 把上千条短线段收进一个 GraphicsPath，最后只做一次 DrawPath。
// GDI+ 单次 DrawLine 的固定开销很高，这是拖动卡顿的主要来源。
struct PathBatch {
    GraphicsPath path;
    std::vector<PointF> buf;
    PathBatch() { buf.reserve(512); }
    void Flush() {
        if (buf.size() >= 2) {
            path.StartFigure();
            path.AddLines(buf.data(), (INT)buf.size());
        }
        buf.clear();
    }
    void Begin() { Flush(); }
    void Add(bool visible, double x, double y) {
        if (visible) buf.push_back(PointF((REAL)x, (REAL)y));
        else Flush();
    }
    void Segment(double x1, double y1, double x2, double y2) {
        path.StartFigure();
        path.AddLine((REAL)x1, (REAL)y1, (REAL)x2, (REAL)y2);
    }
    void Draw(Graphics& g, Pen& pen) {
        Flush();
        if (path.GetPointCount() > 1) g.DrawPath(&pen, &path);
    }
};

// 静态背景层：球盘渐变 / 轮廓 / 刻度只与尺寸、缩放、平移有关，旋转时直接复用
void CelestialSphere::EnsureBackgroundCache() {
    Pen outline(Color(160, 92, 128, 172), 1.4f);
    Pen tick(Color(90, 110, 146, 170), 1.0f);
    if (bgCache_ && bgW_ == width_ && bgH_ == height_ &&
        bgInside_ == inside_ &&
        (inside_ || (std::fabs(bgR_ - radius_) < 0.01 &&
                     bgPX_ == panX_ && bgPY_ == panY_ && bgGrid_ == opt_.showGrid)))
        return;

    delete reinterpret_cast<Bitmap*>(bgCache_);
    Bitmap* nb = new Bitmap(width_, height_, PixelFormat32bppRGB);  // 背景全不透明
    Graphics bg(nb);
    bg.SetSmoothingMode(SmoothingModeAntiAlias);
    bg.Clear(Color(255, 7, 10, 18));

    if (inside_) {
        // 球心视角：观测点在球内，没有球盘轮廓和刻度，整屏就是正前方天区
        bgCache_ = nb;
        bgW_ = width_; bgH_ = height_;
        bgR_ = radius_; bgPX_ = panX_; bgPY_ = panY_;
        bgGrid_ = opt_.showGrid; bgInside_ = inside_;
        return;
    }

    const REAL R = (REAL)radius_;
    const REAL ox = (REAL)(cx_ + panX_), oy = (REAL)(cy_ + panY_);
    RectF disk(ox - R, oy - R, 2*R, 2*R);
    {
        GraphicsPath p; p.AddEllipse(disk);
        PathGradientBrush pgb(&p);
        pgb.SetCenterColor(Color(255, 20, 29, 50));
        Color edge(255, 9, 13, 26);
        int cnt = 1;
        pgb.SetSurroundColors(&edge, &cnt);
        pgb.SetFocusScales(0.25f, 0.25f);
        bg.FillPath(&pgb, &p);
    }
    bg.DrawEllipse(&outline, disk);

    if (opt_.showGrid) {
        GraphicsPath ticks;
        for (int i = 0; i < 72; ++i) {
            double ang = i * 5.0 * D2R;
            double ca = std::cos(ang), sa = std::sin(ang);
            double r2 = R + ((i % 3 == 0) ? 8.0 : 3.5);
            ticks.StartFigure();
            ticks.AddLine((REAL)(ox + ca*R), (REAL)(oy + sa*R),
                          (REAL)(ox + ca*r2), (REAL)(oy + sa*r2));
        }
        bg.DrawPath(&tick, &ticks);
    }

    bgCache_ = nb;
    bgW_ = width_; bgH_ = height_;
    bgR_ = radius_; bgPX_ = panX_; bgPY_ = panY_;
    bgGrid_ = opt_.showGrid; bgInside_ = inside_;
}

// 背景层 -> 主帧：两张都是 32bppRGB，逐行 memcpy 比整帧 GDI+ 合成快得多
void CelestialSphere::CopyBackgroundToFrame() {
    Bitmap* dst = reinterpret_cast<Bitmap*>(frameCache_);
    Bitmap* src = reinterpret_cast<Bitmap*>(bgCache_);
    if (!dst || !src) return;
    Rect rc(0, 0, width_, height_);
    BitmapData a, b;
    if (dst->LockBits(&rc, ImageLockModeWrite, PixelFormat32bppRGB, &a) != Gdiplus::Ok) return;
    if (src->LockBits(&rc, ImageLockModeRead, PixelFormat32bppRGB, &b) != Gdiplus::Ok) {
        dst->UnlockBits(&a);
        return;
    }
    const size_t rowBytes = (size_t)width_ * 4;
    for (int y = 0; y < height_; ++y)
        std::memcpy(reinterpret_cast<BYTE*>(a.Scan0) + (size_t)y * a.Stride,
                    reinterpret_cast<BYTE*>(b.Scan0) + (size_t)y * b.Stride, rowBytes);
    src->UnlockBits(&b);
    dst->UnlockBits(&a);
}

// 星点层：直接写 32bpp 像素（零次 GDI+ 绘图调用），再一次性合成到主帧
void CelestialSphere::DrawStarLayer(DrawCtx& c, std::vector<LabelItem>& labels) {
    labels.clear();
    lastDrawn_ = 0;

    Bitmap* layer = reinterpret_cast<Bitmap*>(starLayer_);
    if (!layer || (int)layer->GetWidth() != width_ || (int)layer->GetHeight() != height_) {
        delete layer;
        layer = new Bitmap(width_, height_, PixelFormat32bppARGB);
        starLayer_ = layer;
    }
    Rect rc(0, 0, width_, height_);
    BitmapData bd;
    if (layer->LockBits(&rc, ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return;

    BYTE* base = reinterpret_cast<BYTE*>(bd.Scan0);
    const INT stride = bd.Stride;
    for (int y = 0; y < height_; ++y)
        std::memset(base + (size_t)y * stride, 0, (size_t)width_ * 4);

    auto put = [&](int x, int y, DWORD argb) {
        if ((unsigned)x >= (unsigned)width_ || (unsigned)y >= (unsigned)height_) return;
        reinterpret_cast<DWORD*>(base + (size_t)y * stride)[x] = argb;
    };
    auto disc = [&](double x, double y, double r, DWORD argb) {
        int x0 = max(0, (int)(x - r - 1)), x1 = min(width_ - 1, (int)(x + r + 1));
        int y0 = max(0, (int)(y - r - 1)), y1 = min(height_ - 1, (int)(y + r + 1));
        double rr = r*r + 0.25;
        for (int py = y0; py <= y1; ++py) {
            double dy = py - y;
            DWORD* row = reinterpret_cast<DWORD*>(base + (size_t)py * stride);
            for (int px = x0; px <= x1; ++px) {
                double dx = px - x;
                if (dx*dx + dy*dy <= rr) row[px] = argb;
            }
        }
    };

    const int lim = (int)min((size_t)max(0, opt_.maxStars), data_.stars.size());
    // 球心视角的缩放由 innerZoom_ 决定；用 zoom_（外部视角缩放）会让刚启动时
    // 星点被错误放大，且复位后与初始画面不一致。
    const double zoomK = Clamp(std::sqrt(inside_ ? innerZoom_ : zoom_), 0.65, 2.4);
    const double ox = cx_ + panX_, oy = cy_ + panY_;
    const double R2 = radius_ * radius_ * 1.002;

    for (int i = 0; i < lim; ++i) {
        const Star& s = data_.stars[i];
        if (s.mag > opt_.limitingMag) break;
        Proj q = Project(s.v);
        if (!q.visible) continue;
        // 球心视角（gnomonic）在地平线附近投影系数发散：贴近地平线的星点会被拉到极远，
        // 自转时以爆炸速度掠过屏幕，视觉上像另一层在反向乱动。剔除地平线附近（|z|<0.06）。
        if (inside_ && (-q.z) < 0.06) continue;
        if (!inside_) {                                  // 球心视角下天区铺满全屏，不裁剪
            double ddx = q.x - ox, ddy = q.y - oy;
            if (ddx*ddx + ddy*ddy > R2) continue;       // 球盘裁剪
        }
        // 球心视角可见的是整个前半球，其中绝大多数投影在屏幕外；
        // 提前剔除可避免无谓的像素写入，lastDrawn_ 也只统计真正看得见的星。
        if (q.x < -40 || q.x > width_ + 40 ||
            q.y < -40 || q.y > height_ + 40) continue;
        ++lastDrawn_;

        double lum = Clamp((opt_.limitingMag - s.mag) / 7.0, 0.0, 1.0);
        double r = (0.65 + 3.7 * lum * lum) * zoomK * q.rel;
        int cr = GetRValue(s.color), cg = GetGValue(s.color), cb = GetBValue(s.color);
        DWORD tint = 0xFF000000u | (cr << 16) | (cg << 8) | cb;
        double bright = Clamp(0.46 + 0.54 * lum, 0.4, 1.0);
        DWORD core = 0xFF000000u | ((int)(cr*bright) << 16) |
                     ((int)(cg*bright) << 8) | (int)(cb*bright);

        if (s.mag <= 1.8) {
            disc(q.x, q.y, r * 4.5, (24u << 24) | (tint & 0xFFFFFFu));
            disc(q.x, q.y, r * 2.5, (52u << 24) | (tint & 0xFFFFFFu));
            if (s.mag < 0.6) {
                int reach = (int)(r * 6);
                for (int k = -reach; k <= reach; ++k) {
                    put((int)q.x + k, (int)q.y, tint);
                    put((int)q.x, (int)q.y + k, tint);
                }
            }
        }
        if (r < 1.25) {
            int px = (int)std::floor(q.x), py = (int)std::floor(q.y);
            put(px, py, core); put(px + 1, py, core);
            put(px, py + 1, core); put(px + 1, py + 1, core);
        } else disc(q.x, q.y, r, core);

        if (opt_.showLabels && labels.size() < 220 &&
            (s.mag <= opt_.labelMag || i == hoverIndex_ || i == selectedIndex_))
            labels.push_back({ i, q.x, q.y, r });
    }

    layer->UnlockBits(&bd);

    // 外部视角只合成球盘外接矩形；球心视角天区铺满全屏，需整屏合成
    int x0 = 0, y0 = 0, x1 = width_, y1 = height_;
    if (!inside_) {
        x0 = max(0, (int)(cx_ + panX_ - radius_ - 2));
        y0 = max(0, (int)(cy_ + panY_ - radius_ - 2));
        x1 = min(width_,  (int)(cx_ + panX_ + radius_ + 2));
        y1 = min(height_, (int)(cy_ + panY_ + radius_ + 2));
    }
    if (x1 > x0 && y1 > y0) {
        RectF dst((REAL)x0, (REAL)y0, (REAL)(x1 - x0), (REAL)(y1 - y0));
        c.g->DrawImage(layer, dst, (REAL)x0, (REAL)y0,
                       (REAL)(x1 - x0), (REAL)(y1 - y0), UnitPixel);
    }
}

// ------------------------------------------------------------------ 绘制
void CelestialSphere::Paint(HDC hdc) {
    if (width_ <= 0 || height_ <= 0) return;
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f;
    }();
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    // 复用主帧缓冲，避免每帧分配/释放几 MB 内存
    Bitmap* bmp = reinterpret_cast<Bitmap*>(frameCache_);
    if (!bmp || frameW_ != width_ || frameH_ != height_) {
        delete bmp;
        bmp = new Bitmap(width_, height_, PixelFormat32bppRGB);  // 不透明帧，不走 alpha 合成
        frameCache_ = bmp;
        frameW_ = width_; frameH_ = height_;
    }
    // 必须在主 Graphics 创建之前完成：两者都不能在 Bitmap 被 Graphics 占用时 LockBits
    EnsureBackgroundCache();
    CopyBackgroundToFrame();
    {
    Graphics mem(bmp);
    mem.SetSmoothingMode(opt_.antialias ? SmoothingModeAntiAlias : SmoothingModeHighSpeed);
    mem.SetTextRenderingHint(opt_.antialias ? TextRenderingHintAntiAliasGridFit
                                           : TextRenderingHintSingleBitPerPixel);
    // 两张缓存位图都是 1:1 整帧贴图，禁止双线性插值可省下一次全屏重采样
    mem.SetInterpolationMode(InterpolationModeNearestNeighbor);
    mem.SetPixelOffsetMode(PixelOffsetModeHighSpeed);

    Pen grid(Color(205, 120, 162, 205), 1.4f);
    Pen equator(Color(110, 120, 196, 214), 1.6f);
    Pen ecliptic(Color(210, 214, 168, 92), 1.5f);
    Pen galactic(Color(180, 150, 120, 190), 1.0f);
    Pen border(Color(95, 138, 198, 235), 1.0f);   // 浅淡蓝：IAU 星座边界
    Pen constellation(Color(120, 96, 178, 226), 1.2f);
    Pen deep(Color(130, 130, 226, 172), 1.0f);
    Pen outline(Color(160, 92, 128, 172), 1.4f);
    Pen tick(Color(90, 110, 146, 170), 1.0f);
    Pen ray(Color(150, 226, 238, 255), 1.0f);
    Pen marker(Color(210, 255, 214, 120), 1.6f);
    Pen selected(Color(240, 120, 220, 255), 2.0f);
    SolidBrush star(Color(255, 255, 255, 255));
    SolidBrush text(Color(225, 214, 232, 250));
    SolidBrush dim(Color(150, 150, 178, 200));
    SolidBrush hud(Color(176, 10, 16, 30));
    Font fLabel(L"Microsoft YaHei UI", (REAL)(11.5 * dpiScale_), FontStyleRegular, UnitPixel);
    Font fSmall(L"Microsoft YaHei UI", (REAL)(9.5  * dpiScale_), FontStyleRegular, UnitPixel);
    Font fHud  (L"Consolas",           (REAL)(12.0 * dpiScale_), FontStyleRegular, UnitPixel);

    DrawCtx c;
    c.g = &mem;
    c.grid = &grid; c.equator = &equator; c.ecliptic = &ecliptic; c.galactic = &galactic;
    c.border = &border;
    c.constellation = &constellation; c.deep = &deep; c.outline = &outline;
    c.tick = &tick; c.ray = &ray; c.marker = &marker; c.selected = &selected;
    c.star = &star; c.text = &text; c.dim = &dim; c.hud = &hud;
    c.fontLabel = &fLabel; c.fontSmall = &fSmall; c.fontHud = &fHud;

    Render(c);
    }   // mem 在此析构，之后才允许 LockBits(bmp)

    // 绕过 GDI+ DrawImage（到窗口 HDC 时走慢速 alpha/格式转换路径），
    // 直接用 GDI 把 32bpp DIB 推到屏幕，等价于一次内存拷贝。
    {
        Rect rc(0, 0, width_, height_);
        BitmapData bd;
        if (bmp->LockBits(&rc, ImageLockModeRead, PixelFormat32bppRGB, &bd) == Gdiplus::Ok) {
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = width_;
            bi.bmiHeader.biHeight = -height_;      // 负值 = top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetDIBitsToDevice(hdc, 0, 0, width_, height_, 0, 0, 0, height_,
                              bd.Scan0, &bi, DIB_RGB_COLORS);
            bmp->UnlockBits(&bd);
        }
    }
    QueryPerformanceCounter(&t1);
    if (freq.QuadPart > 0) {
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        frameMs_ = (frameMs_ <= 0) ? ms : frameMs_ * 0.8 + ms * 0.2;
    }
}

void CelestialSphere::UpdateMoon() {
    double jd = JulianDate(obsYear_, obsMonth_, obsDay_, obsHour_ - 8.0);
    double raH, decD, illum, dist, elong, sunRaH, sunDecD;
    MoonEquatorial(jd, raH, decD, illum, dist, elong, sunRaH, sunDecD);
    moonRaH_ = raH; moonDecD_ = decD;
    moonV_ = RaDecToVec(raH, decD);
    sunV_ = RaDecToVec(sunRaH, sunDecD);
    moonIllum_ = illum;
    moonDist_ = dist;
    moonPhaseDeg_ = elong;
    moonValid_ = true;
}

double CelestialSphere::MoonRadius() const {
    // 视觉放大：真实月面张角仅约 0.5°，这里按 7px 基准放大 2 倍，便于观察月相
    return 14.0 * dpiScale_ * Clamp(std::sqrt(inside_ ? innerZoom_ : zoom_), 0.8, 2.6);
}

double CelestialSphere::PlanetRadius(int i) const {
    const double zf = Clamp(std::sqrt(inside_ ? innerZoom_ : zoom_), 1.0, 1.8);
    return Clamp(4.6 - (planets_[i].mag + 2.0) * 0.42, 2.0, 5.0) * zf * dpiScale_;
}

// 按观测时间推算七大行星的地心赤道坐标（含一次光行时修正）、视星等与距角。
void CelestialSphere::UpdatePlanets() {
    const double jd = JulianDate(obsYear_, obsMonth_, obsDay_, obsHour_ - 8.0);
    const double T  = (jd - 2451545.0) / 36525.0;
    const double eps = 23.4392911 * D2R;          // J2000 黄赤交角
    const double ce = std::cos(eps), se = std::sin(eps);

    double ex, ey, ez, er;
    HelioEcliptic(7, T, ex, ey, ez, er);          // 地球位置
    if (er < 1e-6) er = 1.0;

    // 太阳地心方向（黄道 -> 赤道），用于距角
    const double sxE = -ex, syE = -ey, szE = -ez;
    const double sxQ = sxE;
    const double syQ = syE * ce - szE * se;
    const double szQ = syE * se + szE * ce;

    for (int i = 0; i < kPlanetCount; ++i) {
        PlanetState& ps = planets_[i];
        double px, py, pz, pr;
        HelioEcliptic(i, T, px, py, pz, pr);

        double gx = px - ex, gy = py - ey, gz = pz - ez;
        double d = std::sqrt(gx*gx + gy*gy + gz*gz);
        if (d < 1e-6) { ps.valid = false; continue; }

        // 光行时修正：看到的是行星在 t - Δ/c 时刻的位置
        const double jd2 = jd - d * 0.0057755183;
        HelioEcliptic(i, (jd2 - 2451545.0) / 36525.0, px, py, pz, pr);
        gx = px - ex; gy = py - ey; gz = pz - ez;
        d = std::sqrt(gx*gx + gy*gy + gz*gz);
        if (d < 1e-6) { ps.valid = false; continue; }

        // 黄道 -> 赤道（J2000）
        const double xe = gx;
        const double ye = gy * ce - gz * se;
        const double ze = gy * se + gz * ce;

        double ra = std::atan2(ye, xe) / H2R;
        if (ra < 0) ra += 24.0;
        ps.raH  = ra;
        ps.decD = std::asin(Clamp(ze / d, -1.0, 1.0)) / D2R;
        ps.v    = { xe / d, ye / d, ze / d };
        ps.distAU  = d;
        ps.helioAU = pr;
        ps.elongDeg = std::acos(Clamp(ps.v.x * sxQ / er + ps.v.y * syQ / er + ps.v.z * szQ / er,
                                     -1.0, 1.0)) / D2R;

        // 相位角（太阳-行星-地球）与视星等经验式（Astronomical Almanac）
        const double cosPh = Clamp((pr*pr + d*d - er*er) / (2.0 * pr * d), -1.0, 1.0);
        const double ph = std::acos(cosPh) / D2R;
        const double base = 5.0 * std::log10(max(1e-6, pr * d));
        switch (i) {
        case 0: ps.mag = -0.42 + base + 0.0380*ph - 0.000273*ph*ph + 0.000002*ph*ph*ph; break;
        case 1: ps.mag = -4.40 + base + 0.0009*ph + 0.000239*ph*ph - 0.00000065*ph*ph*ph; break;
        case 2: ps.mag = -1.52 + base + 0.016*ph; break;
        case 3: ps.mag = -9.40 + base + 0.005*ph; break;
        case 4: ps.mag = -8.88 + base + 0.044*ph; break;
        case 5: ps.mag = -7.19 + base; break;
        default: ps.mag = -6.87 + base; break;
        }
        ps.valid = true;
    }
}

void CelestialSphere::DrawMoon(DrawCtx& c) {
    if (!opt_.showMoon || !moonValid_) return;
    Proj q = Project(moonV_);
    if (!q.visible) return;
    if (inside_ && (-q.z) < 0.05) return;      // 地平附近剔除，避免投影发散
    if (!inside_) {
        double dx = q.x - (cx_ + panX_), dy = q.y - (cy_ + panY_);
        if (dx*dx + dy*dy > radius_*radius_*1.002) return;
    }
    Graphics& g = *c.g;
    REAL mx = (REAL)q.x, my = (REAL)q.y;
    REAL r = (REAL)MoonRadius();

    // 亮面朝向太阳：把“月亮处指向太阳的切向”投影到屏幕，得到一个屏幕上的方向
    double ddx = 1.0, ddy = 0.0;
    {
        const Vec3& m = moonV_;
        double dot = sunV_.x*m.x + sunV_.y*m.y + sunV_.z*m.z;
        double tx = sunV_.x - dot*m.x, ty = sunV_.y - dot*m.y, tz = sunV_.z - dot*m.z;
        double qx = R_[0][0]*tx + R_[0][1]*ty + R_[0][2]*tz;
        double qy = R_[1][0]*tx + R_[1][1]*ty + R_[1][2]*tz;
        double len = std::sqrt(qx*qx + qy*qy);
        if (len > 1e-9) { ddx = qx/len; ddy = -qy/len; }   // 屏幕 y 与 q.y 反向
    }

    // 外层光晕：两层径向渐隐（越靠外越透明），避免单色实心圆在月盘外留下硬边白圈。
    // 中心点被月盘覆盖，实际可见部分从月盘边缘开始，透明度自然过渡到 0。
    {
        auto glow = [&](REAL rad, int centerA, const Color& tint) {
            GraphicsPath gp;
            gp.AddEllipse(RectF(mx - rad, my - rad, rad * 2, rad * 2));
            PathGradientBrush pgb(&gp);
            pgb.SetCenterPoint(PointF(mx, my));
            pgb.SetCenterColor(Color((BYTE)centerA, tint.GetR(), tint.GetG(), tint.GetB()));
            Color edge(0, tint.GetR(), tint.GetG(), tint.GetB());
            int cnt = 1;
            pgb.SetSurroundColors(&edge, &cnt);
            g.FillPath(&pgb, &gp);
        };
        // 真实月光偏中性灰白（而非米黄），月盘放大后光晕相应调淡、收窄，避免糊成一团光斑
        const Color warm(255, 246, 244, 236);       // 暖白
        glow(r * 1.6f, 15, warm);                   // 贴近日面的柔光
        glow(r * 2.4f, 6, warm);                    // 更远更淡的外晕
    }

    // 盈亏：逐像素按相位画亮/暗。k = cos(月日角距) = 1 - 2*照度，
    // 亮面朝太阳（+t 方向），盈亏（蛾眉/上弦/盈凸/满月/亏凸/下弦/残月）自动正确。
    double k = 1.0 - 2.0 * Clamp(moonIllum_, 0.0, 1.0);
    int R = (int)std::ceil(r) + 2;
    int diam = 2 * R;
    if (diam >= 4) {
        Bitmap bmp(diam, diam, PixelFormat32bppARGB);
        Rect brc(0, 0, diam, diam);
        BitmapData bd;
        if (bmp.LockBits(&brc, ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
            BYTE* base = reinterpret_cast<BYTE*>(bd.Scan0);
            int stride = bd.Stride;
            for (int py = 0; py < diam; ++py) {
                DWORD* row = reinterpret_cast<DWORD*>(base + (size_t)py * stride);
                double dy = py - R;
                for (int px = 0; px < diam; ++px) {
                    double dx = px - R;
                    double d2 = dx*dx + dy*dy;
                    if (d2 > r*r) { row[px] = 0; continue; }
                    double t = dx*ddx + dy*ddy;            // 沿太阳方向
                    double s = -dx*ddy + dy*ddx;           // 垂直方向
                    double w2 = r*r - s*s;
                    double w = (w2 > 0) ? std::sqrt(w2) : 0.0;
                    bool lit = (t >= k * w);               // 亮面朝太阳
                    double dd = std::sqrt(d2);
                    double aa = Clamp(r - dd + 0.5, 0.0, 1.0);
                    DWORD col;
                    if (lit) {
                        // 月面取中性灰白（真实月光几乎不带黄），临边略压暗以显出球面感
                        double shade = 0.88 + 0.12 * (1.0 - dd / r);
                        col = 0xFF000000u
                            | ((DWORD)(247.0 * shade) << 16)
                            | ((DWORD)(244.0 * shade) << 8)
                            | (DWORD)(234.0 * shade);
                    } else {
                        col = 0xFF1C2129u;                 // 暗面：地球反照的深灰蓝
                    }
                    row[px] = (col & 0x00FFFFFFu) | ((DWORD)(aa * 255.0) << 24);
                }
            }
            bmp.UnlockBits(&bd);
            g.DrawImage(&bmp, mx - (REAL)R, my - (REAL)R, (REAL)diam, (REAL)diam);
        }
    }

    // 标签：月相名称
    wchar_t lb[48];
    MoonLabel(lb, 48, moonIllum_, moonPhaseDeg_);
    g.DrawString(lb, -1, c.fontSmall, PointF(mx + r + 5, my - 7), c.text);
}

// 行星绘制：视星等决定盘面大小，用各自颜色区分；标签随缩放同步。
void CelestialSphere::DrawPlanets(DrawCtx& c) {
    if (!opt_.showPlanets) return;
    Graphics& g = *c.g;
    for (int i = 0; i < kPlanetCount; ++i) {
        const PlanetState& ps = planets_[i];
        if (!ps.valid) continue;
        Proj q = Project(ps.v);
        if (!q.visible) continue;
        if (inside_ && (-q.z) < 0.05) continue;
        if (!inside_) {
            double dx = q.x - (cx_ + panX_), dy = q.y - (cy_ + panY_);
            if (dx*dx + dy*dy > radius_*radius_*1.002) continue;
        }
        const REAL x = (REAL)q.x, y = (REAL)q.y;
        const REAL r = (REAL)PlanetRadius(i);

        SolidBrush fill(kPlanetColors[i]);
        g.FillEllipse(&fill, x - r, y - r, r * 2, r * 2);

        // 悬停 / 选中：加一圈淡淡的指示环
        if (hoverIndex_ == TQ_PLANET_BASE - i || selectedIndex_ == TQ_PLANET_BASE - i) {
            Pen ring(Color(210, 255, 224, 150), 1.2f);
            g.DrawEllipse(&ring, x - r - 3, y - r - 3, r * 2 + 6, r * 2 + 6);
        }

        g.DrawString(kPlanetNames[i], -1, c.fontSmall, PointF(x + r + 5, y - 7), c.text);
    }
}

// 星座贴图（填充版）：顶点是球面 (ra 度, dec 度)，逐点投影后成面，任意投影下都不失真。
// 每幅贴图 4 个灰度层，同层多段按奇偶规则合并，重叠处自动挖洞。
void CelestialSphere::DrawConstellationArt(DrawCtx& c) {
    if (!opt_.showArt || data_.art.empty()) return;
    Graphics& g = *c.g;
    const double ox = cx_ + panX_, oy = cy_ + panY_;
    const double reach = width_ * (inside_ ? 2.0 : 4.0);   // 单点投影距离上限，超出即断开
    const double R2 = radius_ * radius_ * 1.15 * 1.15;

    // 黄道十二宫（zodiacOnly 时按此过滤）
    static const char* kZodiac[] = {
        "Ari", "Tau", "Gem", "Cnc", "Leo", "Vir",
        "Lib", "Sco", "Sgr", "Cap", "Aqr", "Psc"
    };

    // 先用贴图中心做粗筛：贴图本体直径一般不超过 40°，中心不在视场里的整块跳过
    struct Cand { size_t idx; double d2; };
    std::vector<Cand> pick;
    pick.reserve(data_.art.size());
    for (size_t k = 0; k < data_.art.size(); ++k) {
        const ConstArt& art = data_.art[k];
        if (art.fill.empty()) continue;
        if (opt_.zodiacOnly) {
            bool zod = false;
            for (const char* z : kZodiac)
                if (art.abbr == z) { zod = true; break; }
            if (!zod) continue;
        }
        // centerRa 存的是度，RaDecToVec 吃小时
        Proj q = Project(RaDecToVec(art.centerRa / 15.0, art.centerDec));
        if (!q.visible) continue;
        const double dx = q.x - ox, dy = q.y - oy;
        if (inside_) {
            if (-q.z < 0.35) continue;                       // 贴近地平线的贴图投影发散
            if (std::fabs(dx) > width_ * 1.2 || std::fabs(dy) > height_ * 1.2) continue;
        } else if (dx*dx + dy*dy > R2) {
            continue;                                        // 贴图中心落在球盘外
        }
        pick.push_back({ k, dx*dx + dy*dy });
    }
    if (pick.empty()) return;

    // 缩到最小时可能有几十个星座同时入选。按离视场中心的距离取最近的若干个，
    // 并给整帧设顶点预算：否则一帧要投影几十万个顶点，自转时明显掉帧。
    const size_t kMaxArt = 18;
    if (pick.size() > kMaxArt) {
        std::partial_sort(pick.begin(), pick.begin() + kMaxArt, pick.end(),
                          [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
        pick.resize(kMaxArt);
    }
    size_t budget = 120000;

    for (const Cand& cd : pick) {
        const ConstArt& art = data_.art[cd.idx];
        const std::vector<ArtLayer>& layers = art.fill;
        // 每幅贴图一套色相，以淡蓝 / 浅紫 / 暗金为主，相邻星座重叠时才分得清。
        // 贴图按缩写排序载入，用下标取色即可让相邻星座落在不同色上，且跨会话稳定。
        static const struct { BYTE r, g, b; } kArtTint[] = {
            { 158, 202, 238 },   // 淡蓝
            { 184, 172, 236 },   // 浅紫
            { 208, 180, 130 },   // 暗金
            { 148, 208, 220 },   // 青蓝
            { 200, 176, 224 },   // 藕荷
            { 220, 200, 152 },   // 浅金
            { 168, 190, 216 },   // 灰蓝
            { 192, 168, 198 },   // 紫灰
        };
        const auto& tint = kArtTint[cd.idx % (sizeof(kArtTint) / sizeof(kArtTint[0]))];
        for (size_t li = 0; li < layers.size() && budget > 0; ++li) {
            const ArtLayer& L = layers[li];
            if (L.counts.empty() || L.radec.size() < 4) continue;
            // tone 是灰度：靠它给同一星座的不同层叠出明暗层次
            const int alpha = (int)Clamp(22.0 + L.tone * 0.85, 0.0, 120.0);
            SolidBrush brush(Color((BYTE)alpha, tint.r, tint.g, tint.b));

            GraphicsPath path;
            path.SetFillMode(FillModeAlternate);

            size_t k = 0;
            for (int cnt : L.counts) {
                const int n = cnt < 0 ? -cnt : cnt;
                if (n <= 0) break;
                if (k + (size_t)n * 2 > L.radec.size()) break;

                std::vector<PointF> pts;
                pts.reserve((size_t)n);
                bool allOk = true;
                for (int i = 0; i < n; ++i, k += 2) {
                    Proj q = Project(RaDecToVec(L.radec[k] / 15.0, L.radec[k + 1]));
                    const double dx = q.x - ox, dy = q.y - oy;
                    if (!q.visible || std::fabs(dx) >= reach || std::fabs(dy) >= reach)
                        allOk = false;
                    pts.emplace_back((REAL)q.x, (REAL)q.y);
                }
                budget -= (size_t)n;

                // 部分顶点不可见（跨过球面/飞出视场）：填充会糊成一大片，整段丢弃
                if (allOk && n >= 3) path.AddPolygon(pts.data(), (INT)n);
            }

            if (path.GetPointCount() > 2) g.FillPath(&brush, &path);
        }
        if (budget == 0) break;
    }
}

// 高亮一个星座：边界多边形投影后描粗描亮（悬停=青色，选中=金色），
// 并在名称标注点画出星座名。边界顶点很稀（每星座十几到几十个），
// 沿 ra/dec 先加密再投影，弧线才不会折成多边形。
void CelestialSphere::DrawConstHighlight(DrawCtx& c, int idx, bool selected) {
    if (idx < 0 || idx >= (int)data_.boundary.size()) return;
    Graphics& g = *c.g;
    const ConstBoundary& b = data_.boundary[idx];
    const double lim = width_ * (inside_ ? 2.0 : 4.0);
    const double ox = cx_ + panX_, oy = cy_ + panY_;

    // 悬停=淡紫，选中=暗金：都压低饱和度，既压得住贴图又不抢星点
    const Color line(selected ? (BYTE)212 : (BYTE)200,
                     selected ? (BYTE)214 : (BYTE)196,
                     selected ? (BYTE)178 : (BYTE)178,
                     selected ? (BYTE)92  : (BYTE)244);
    Pen glow(Color(selected ? (BYTE)78 : (BYTE)52, line.GetR(), line.GetG(), line.GetB()),
             (REAL)(selected ? 7.0 : 5.0));
    Pen pen(line, (REAL)(selected ? 2.8 : 2.2));

    GraphicsPath path;
    for (const auto& ring : b.rings) {
        const size_t n = ring.size();
        if (n < 3) continue;
        std::vector<PointF> pts;
        for (size_t i = 0; i < n; ++i) {
            const auto& p = ring[i];
            const auto& q = ring[(i + 1) % n];
            const double dra = q.first - p.first, ddec = q.second - p.second;
            const double span = max(std::fabs(dra) * 15.0, std::fabs(ddec));
            const int steps = (int)Clamp(std::ceil(span / 0.6), 1.0, 128.0);
            for (int s = 0; s < steps; ++s) {
                const double t = (double)s / steps;
                const Proj pr = Project(RaDecToVec(p.first + dra * t, p.second + ddec * t));
                const bool ok = pr.visible &&
                                std::fabs(pr.x - ox) < lim && std::fabs(pr.y - oy) < lim;
                if (!ok) {
                    // 不可见就断成新的一段，避免把线段拉到屏幕外的极点
                    if (pts.size() >= 2) path.AddLines(pts.data(), (INT)pts.size());
                    pts.clear();
                    continue;
                }
                pts.emplace_back((REAL)pr.x, (REAL)pr.y);
            }
        }
        if (pts.size() >= 2) path.AddLines(pts.data(), (INT)pts.size());
    }
    if (path.GetPointCount() > 1) {
        g.DrawPath(&glow, &path);
        g.DrawPath(&pen, &path);
    }

    // 星座名（标在名称标注点上，带半透明底，压在贴图上也能读）
    const ConstInfo* info = FindConstInfo(data_, b.abbr);
    const std::wstring nm = info && !info->nameCn.empty() ? info->nameCn
                                                         : Utf8ToWide(b.abbr);
    if (nm.empty() || !info) return;
    const Proj lp = Project(RaDecToVec(info->labelRa, info->labelDec));
    if (!lp.visible) return;
    if (std::fabs(lp.x - ox) > width_ * 0.6 || std::fabs(lp.y - oy) > height_ * 0.6) return;

    RectF box(0, 0, 0, 0);
    g.MeasureString(nm.c_str(), -1, c.fontLabel, PointF(0, 0), &box);
    const RectF dst((REAL)lp.x - box.Width / 2, (REAL)lp.y - box.Height / 2,
                    box.Width + 10, box.Height + 4);
    SolidBrush bg(Color(selected ? 150 : 120, 10, 16, 26));
    g.FillRectangle(&bg, dst);
    Pen edge(Color(selected ? 200 : 150, line.GetR(), line.GetG(), line.GetB()), 1.0f);
    g.DrawRectangle(&edge, dst);
    g.DrawString(nm.c_str(), -1, c.fontLabel,
                 PointF(dst.X + 5, dst.Y + 1), selected ? c.text : c.dim);
}

void CelestialSphere::Render(DrawCtx& c) {
    UpdateMoon();
    UpdatePlanets();
    Graphics& g = *c.g;
    const REAL R = (REAL)radius_;
    const REAL ox = (REAL)(cx_ + panX_), oy = (REAL)(cy_ + panY_);

    // 球外视角裁剪到球盘内；球心视角天区铺满全屏，不裁剪
    if (!inside_) {
        GraphicsPath clip;
        clip.AddEllipse(RectF(ox - R, oy - R, 2*R, 2*R));
        g.SetClip(&clip);
    }

    // 球心视角下靠近地平线的点会投影到极远处，超出阈值就断开，
    // 避免把几十万像素长的线段塞进 GraphicsPath。
    // q.x/q.y 是屏幕绝对坐标，阈值必须以视图中心为原点：若直接和 0.85*width_
    // 比，截断线会落在屏幕内部（距右边缘 15%），折线会在到达右边缘前提前消失。
    auto addPt = [&](PathBatch& batch, const Proj& q) {
        const double lim = width_ * (inside_ ? 2.0 : 4.0);
        double dx = q.x - (cx_ + panX_), dy = q.y - (cy_ + panY_);
        if (q.visible && std::fabs(dx) < lim && std::fabs(dy) < lim)
            batch.Add(true, q.x, q.y);
        else
            batch.Add(false, 0.0, 0.0);
    };

    // 参数曲线 / 大圆弧：折线并入 PathBatch，最后统一一次 DrawPath
    auto addCurve = [&](PathBatch& batch, const std::function<Vec3(double)>& fn,
                        double t0, double t1, int steps) {
        batch.Begin();
        for (int i = 0; i <= steps; ++i) {
            double t = t0 + (t1 - t0) * i / steps;
            addPt(batch, Project(fn(t)));
        }
        batch.Flush();
    };
    auto addArc = [&](PathBatch& batch, const Vec3& a, const Vec3& b) {
        double dot = Clamp(a.x*b.x + a.y*b.y + a.z*b.z, -1.0, 1.0);
        double om = std::acos(dot);
        if (om < 1e-7) return;
        int n = max(1, (int)std::ceil(om / (3.5 * D2R)));
        double so = std::sin(om);
        batch.Begin();
        for (int i = 0; i <= n; ++i) {
            double t = (double)i / n;
            double w1 = std::sin((1.0 - t)*om) / so, w2 = std::sin(t*om) / so;
            Vec3 v = { w1*a.x + w2*b.x, w1*a.y + w2*b.y, w1*a.z + w2*b.z };
            addPt(batch, Project(v));
        }
        batch.Flush();
    };

    // --- 星座贴图（最底层的装饰图：垫在网格与连线之下）---
    DrawConstellationArt(c);

    // --- 赤道坐标网格（约 2500 段 -> 1 次 DrawPath）---
    if (opt_.showGrid) {
        PathBatch gb;
        for (int dec = -80; dec <= 80; dec += 20) {
            if (dec == 0 && opt_.showEquator) continue;
            addCurve(gb, [&](double t) { return RaDecToVec(t/15.0, (double)dec); }, 0, 360, 120);
        }
        for (int h = 0; h < 24; h += 2)
            addCurve(gb, [&](double t) { return RaDecToVec((double)h, t); }, -89, 89, 60);
        gb.Draw(g, *c.grid);
    }
    if (opt_.showEquator) {
        PathBatch eb;
        addCurve(eb, [&](double t) { return RaDecToVec(t/15.0, 0.0); }, 0, 360, 180);
        eb.Draw(g, *c.equator);
    }

    // --- 黄道 ---
    if (opt_.showEcliptic) {
        double eps = 23.4393 * D2R;
        double ce = std::cos(eps), se = std::sin(eps);
        PathBatch pb;
        addCurve(pb, [&](double t) {
            double l = t * D2R;
            return Vec3{ std::cos(l), std::sin(l)*ce, std::sin(l)*se };
        }, 0, 360, 180);
        pb.Draw(g, *c.ecliptic);
    }

    // --- 银河（银道面附近的光带）---
    // 只用三条线表达这条带子：银纬 b=0 的中线（略亮）+ b=±12 的两条外轮廓。
    // 全部 1° 采样 + GDI+ 样条（AddCurve）插值，放大到十几倍轮廓依然平滑。
    // 注：曾按每 5° 画 7 条银纬线铺出"带宽"，但放大后就是一排等间距平行线，
    //     看起来像画错的网格，故收敛为中线 + 双侧外轮廓。
    if (opt_.showGalactic) {
        Color base(255, 150, 120, 190);
        c.galactic->GetColor(&base);
        Pen gCore(Color(200, base.GetR(), base.GetG(), base.GetB()), 1.3f);
        Pen gEdge(Color(85,  base.GetR(), base.GetG(), base.GetB()), 1.0f);

        // 沿一条银纬线取点，遇到不可见/飞出屏幕的点就断开，分段交给 AddCurve
        auto addBand = [&](GraphicsPath& path, double bDeg) {
            std::vector<PointF> seg;
            seg.reserve(361);
            auto flushSeg = [&]() {
                if (seg.size() >= 3)       path.AddCurve(seg.data(), (INT)seg.size());
                else if (seg.size() == 2)  path.AddLine(seg[0], seg[1]);
                seg.clear();
            };
            const double lim = width_ * (inside_ ? 2.0 : 4.0);
            for (int i = 0; i <= 360; ++i) {
                Proj q = Project(GalacticVec((double)i, bDeg));
                double dx = q.x - (cx_ + panX_), dy = q.y - (cy_ + panY_);
                if (q.visible && std::fabs(dx) < lim && std::fabs(dy) < lim)
                    seg.emplace_back((REAL)q.x, (REAL)q.y);
                else
                    flushSeg();
            }
            flushSeg();
        };

        static const int kBands[] = { -12, 0, 12 };
        GraphicsPath core, edge;
        for (int b : kBands)
            addBand(*(b == 0 ? &core : &edge), (double)b);
        if (core.GetPointCount() > 1) g.DrawPath(&gCore, &core);
        if (edge.GetPointCount() > 1) g.DrawPath(&gEdge, &edge);
    }

    // --- IAU 官方星座边界（J2000 分块线，浅淡蓝）---
    // 边界是沿赤经/赤纬划定的行政界线（岁差后成曲线），必须按赤道坐标插值：
    // 若像星座连线那样走大圆弧，等赤纬的线段会被拉向极点而画错。
    if (opt_.showBorders) {
        PathBatch bb;
        auto seg = [&](double ra0, double dec0, double ra1, double dec1) {
            double dra = ra1 - ra0;
            if (dra > 12.0) dra -= 24.0;         // 赤经 0h 接缝取最短方向
            if (dra < -12.0) dra += 24.0;
            double ddec = dec1 - dec0;
            double cosd = std::cos((dec0 + dec1) * 0.5 * D2R);
            double span = std::sqrt(dra * dra * 225.0 * cosd * cosd + ddec * ddec);
            int n = (int)std::ceil(span / 2.0);  // 约每 2° 采样一点
            if (n < 1) n = 1;
            if (n > 64) n = 64;
            bb.Begin();
            for (int i = 0; i <= n; ++i) {
                double t = (double)i / n;
                addPt(bb, Project(RaDecToVec(ra0 + dra * t, dec0 + ddec * t)));
            }
            bb.Flush();
        };
        for (const auto& b : data_.borders)
            for (size_t i = 0; i + 1 < b.pts.size(); ++i)
                seg(b.pts[i].first, b.pts[i].second,
                    b.pts[i+1].first, b.pts[i+1].second);
        bb.Draw(g, *c.border);
    }

    // --- 悬停 / 选中的星座：加粗加亮边界 + 星座名（描在边界线之上）---
    if (opt_.showArt) {
        if (selConst_ >= 0 && selConst_ != hoverConst_) DrawConstHighlight(c, selConst_, true);
        if (hoverConst_ >= 0) DrawConstHighlight(c, hoverConst_, selConst_ == hoverConst_);
    }

    // --- 星座连线 ---
    if (opt_.showConstellations) {
        PathBatch cb;
        for (const auto& l : data_.lines)
            for (size_t i = 0; i + 1 < l.pts.size(); ++i)
                addArc(cb,
                       RaDecToVec(l.pts[i].first,   l.pts[i].second),
                       RaDecToVec(l.pts[i+1].first, l.pts[i+1].second));
        cb.Draw(g, *c.constellation);
    }

    // --- 深空天体 ---
    if (opt_.showDeepSky) {
        PathBatch db;
        for (const auto& d : data_.deepsky) {
            Proj q = Project(d.v);
            if (!q.visible) continue;
            double x = q.x, y = q.y;
            db.Segment(x-4, y-4, x+4, y-4);
            db.Segment(x+4, y-4, x+4, y+4);
            db.Segment(x+4, y+4, x-4, y+4);
            db.Segment(x-4, y+4, x-4, y-4);
            db.Segment(x-7, y, x-4, y);
            db.Segment(x+4, y, x+7, y);
            db.Segment(x, y-7, x, y-4);
            db.Segment(x, y+4, x, y+7);
        }
        db.Draw(g, *c.deep);
    }

    // --- 恒星：直接写像素图层，一次 DrawImage 合成 ---
    std::vector<LabelItem> labels;
    DrawStarLayer(c, labels);

    // --- 行星 / 月亮（动态天体，叠加在恒星层之上）---
    DrawPlanets(c);
    DrawMoon(c);

    // --- 名称标注（带简单避让） ---
    if (!labels.empty()) {
        std::vector<RectF> placed;
        auto overlap = [](const RectF& a, const RectF& b) {
            return !(a.X + a.Width < b.X || b.X + b.Width < a.X ||
                     a.Y + a.Height < b.Y || b.Y + b.Height < a.Y);
        };
        int count = 0;
        for (const auto& it : labels) {
            if (count > 70) break;      // 每次 DrawString 都很贵，密集星场限制标注数量
            const Star& s = data_.stars[it.idx];
            const std::wstring& label = s.nameCn.empty() ? s.name : s.nameCn;  // 中文名优先
            if (label.empty()) continue;
            REAL w = (REAL)(label.size() * 12.0 + 10.0);
            RectF box((REAL)(it.x + it.r + 4), (REAL)(it.y - 9), w, 17.0f);
            bool ok = true;
            for (const auto& p : placed) if (overlap(box, p)) { ok = false; break; }
            if (!ok) continue;
            placed.push_back(box);
            g.DrawString(label.c_str(), -1, c.fontLabel, PointF(box.X, box.Y), c.text);
            ++count;
        }
    }

    // --- 星座名称 ---
    if (opt_.showConstellationNames) {
        std::vector<std::string> done;
        for (const auto& l : data_.lines) {
            if (std::find(done.begin(), done.end(), l.con) != done.end()) continue;
            if (l.pts.empty()) continue;
            Proj q = Project(RaDecToVec(l.pts[l.pts.size()/2].first, l.pts[l.pts.size()/2].second));
            if (!q.visible) continue;
            done.push_back(l.con);
            g.DrawString(l.nameCn.c_str(), -1, c.fontSmall,
                         PointF((REAL)q.x, (REAL)q.y), c.dim);
        }
    }

    // --- 北天极标记 ---
    if (opt_.showNorthMark) {
        Proj q = Project(Vec3{ 0, 0, 1 });
        if (q.visible) {
            REAL x = (REAL)q.x, y = (REAL)q.y;
            g.DrawEllipse(c.marker, x-7, y-7, 14.0f, 14.0f);
            g.DrawLine(c.marker, x-11, y, x+11, y);
            g.DrawLine(c.marker, x, y-11, x, y+11);
            g.DrawString(L"NCP", -1, c.fontSmall, PointF(x+12, y-6), c.dim);
        }
    }

    // --- 观测者地平圈 / 方位标记 ---
    if (hasObserver_) DrawHorizonLayer(c);

    // --- 悬停 / 选中高亮 ---
    auto mark = [&](int idx, Pen& pen, float radius) {
        if (idx < 0 || idx >= (int)data_.stars.size()) return;
        Proj q = Project(data_.stars[idx].v);
        if (!q.visible) return;
        REAL x = (REAL)q.x, y = (REAL)q.y;
        g.DrawEllipse(&pen, x-radius, y-radius, 2*radius, 2*radius);
        g.DrawLine(&pen, x-radius-6, y, x-radius-2, y);
        g.DrawLine(&pen, x+radius+2, y, x+radius+6, y);
    };
    mark(hoverIndex_,    *c.marker,   9.0f);
    mark(selectedIndex_, *c.selected, 13.0f);

    // --- 中心十字准线 ---
    if (opt_.showCrosshair) {
        REAL x = ox, y = oy;
        g.DrawLine(c.tick, x-11, y, x-4, y);
        g.DrawLine(c.tick, x+4, y, x+11, y);
        g.DrawLine(c.tick, x, y-11, x, y-4);
        g.DrawLine(c.tick, x, y+4, x, y+11);
        g.DrawEllipse(c.tick, x-2, y-2, 4.0f, 4.0f);
    }

    g.ResetClip();

    // --- HUD ---
    double ra, dec, roll; GetViewRA(ra, dec, roll);
    wchar_t rab[64], decb[64], line[320];
    FormatRA(ra, rab, 64); FormatDec(dec, decb, 64);
    swprintf_s(line, L"%ls  RA %ls  DEC %ls  |  %.2fx  |  %d / %d 颗  |  帧 %.1f ms",
               inside_ ? L"球心视角" : L"外部视角", rab, decb,
               inside_ ? innerZoom_ : zoom_, lastDrawn_,
               (int)data_.stars.size(), frameMs_);
    const REAL ds = (REAL)dpiScale_;
    g.FillRectangle(c.hud, 8.0f * ds, 8.0f * ds, 780.0f * ds, 26.0f * ds);
    g.DrawString(line, -1, c.fontHud, PointF(16.0f * ds, 11.0f * ds), c.text);
}

// ------------------------------------------------------------------ 观测者叠加层
void CelestialSphere::DrawHorizonLayer(DrawCtx& c) {
    Graphics& g = *c.g;
    double lst = LSTdeg();
    Pen horizonPen(Color(150, 205, 225, 210), 1.4f);
    Pen altPen   (Color(90, 140, 165, 150), 1.0f);
    auto addPt = [&](PathBatch& b, const Proj& q) {
        double dx = q.x - (cx_ + panX_), dy = q.y - (cy_ + panY_);
        bool ok = q.visible && std::fabs(dx) < width_ * 4.0 && std::fabs(dy) < height_ * 4.0;
        b.Add(ok, q.x, q.y);
    };
    // 地平线（高度 0°）
    PathBatch pb; pb.Begin();
    for (int az = 0; az <= 360; az += 2)
        addPt(pb, Project(HorizonToVec((double)az, 0.0, obsLat_, lst)));
    pb.Draw(g, horizonPen);
    // 高度圈 30°（地平线上方参考）
    PathBatch pb2; pb2.Begin();
    for (int az = 0; az <= 360; az += 3)
        addPt(pb2, Project(HorizonToVec((double)az, 30.0, obsLat_, lst)));
    pb2.Draw(g, altPen);
    // 方位标记：北 / 东 / 南 / 西（地平圈上）
    SolidBrush dot(Color(235, 245, 235, 130));
    SolidBrush shadow(Color(170, 0, 0, 0));
    auto mark = [&](double az, const wchar_t* label) {
        Proj q = Project(HorizonToVec(az, 0.0, obsLat_, lst));
        if (!q.visible) return;
        g.FillEllipse(&dot, (REAL)q.x - 3.5f, (REAL)q.y - 3.5f, 7.0f, 7.0f);
        g.DrawString(label, -1, c.fontLabel, PointF((REAL)q.x + 7.0f, (REAL)q.y - 10.0f), &shadow);
        g.DrawString(label, -1, c.fontLabel, PointF((REAL)q.x + 6.0f, (REAL)q.y - 11.0f), c.text);
    };
    mark(0,   L"北");
    mark(90,  L"东");
    mark(180, L"南");
    mark(270, L"西");
    // 天顶（赤经 = 本地恒星时，赤纬 = 纬度）
    Proj zq = Project(RaDecToVec(lst / 15.0, obsLat_));
    if (zq.visible) {
        g.FillEllipse(&dot, (REAL)zq.x - 3.5f, (REAL)zq.y - 3.5f, 7.0f, 7.0f);
        g.DrawString(L"天顶", -1, c.fontLabel, PointF((REAL)zq.x + 7.0f, (REAL)zq.y - 10.0f), &shadow);
        g.DrawString(L"天顶", -1, c.fontLabel, PointF((REAL)zq.x + 6.0f, (REAL)zq.y - 11.0f), c.text);
    }
}

// ------------------------------------------------------------------ 窗口过程
LRESULT CALLBACK CelestialSphere::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        if (cs && cs->lpCreateParams)
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(h, m, w, l);
    }
    CelestialSphere* self = reinterpret_cast<CelestialSphere*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, m, w, l);
    return self->Handle(m, w, l);
}

void CelestialSphere::ZoomAt(int mx, int my, double factor) {
    // 鼠标所指的天球方向作锚点，缩放后把它对齐回鼠标位置。
    // 缩放只在当前视角内变化：球外限制在 [0.25, kMaxZoom]，球心限制在 [0.25, kMaxInnerZoom]；
    // 不自动跨越球面切换视角，跨视角只能由「球心 / 外部」按钮显式切换。
    Vec3 anchor;
    const bool hasAnchor = ScreenToSphere(mx, my, anchor);

    if (inside_) {
        innerZoom_ = Clamp(innerZoom_ * factor, 0.25, kMaxInnerZoom);
    } else {
        zoom_ = Clamp(zoom_ * factor, 0.25, kMaxZoom);
    }

    RecomputeGeometry();
    if (hasAnchor) {
        Proj q = Project(anchor);
        if (q.visible) {
            panX_ += mx - q.x;
            panY_ += my - q.y;
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    SendNotify(TQN_VIEWCHANGED, -1);
}

void CelestialSphere::ToggleAndNotify(bool SphereOptions::* field) {
    opt_.*field = !(opt_.*field);
    InvalidateRect(hwnd_, nullptr, FALSE);
    SendNotify(TQN_VIEWCHANGED, -1);
}

LRESULT CelestialSphere::Handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        width_  = LOWORD(l);
        height_ = HIWORD(l);
        RecomputeGeometry();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd_, &ps);
        Paint(hdc);
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_DESTROY:
        StopAnimation();
        return 0;

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;

    // ------------------------------------------------------------ 控制消息
    case TQSM_RELOAD:
        if (l) data_ = *reinterpret_cast<const CatalogData*>(l);
        hoverIndex_ = selectedIndex_ = -1;
        hoverConst_ = selConst_ = -1;      // 数据换了，星座下标作废
        InvalidateRect(hwnd_, nullptr, FALSE);
        SendNotify(TQN_VIEWCHANGED, -1);
        return 0;

    case TQSM_SETOPTIONS:
        if (l) opt_ = *reinterpret_cast<const SphereOptions*>(l);
        if (!opt_.showArt) { hoverConst_ = -1; selConst_ = -1; }   // 关掉贴图就没有星座高亮
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case TQSM_GETOPTIONS:
        if (l) *reinterpret_cast<SphereOptions*>(l) = opt_;
        return 0;

    case TQSM_GETVIEW: {
        if (l) {
            SphereView& v = *reinterpret_cast<SphereView*>(l);
            double roll;
            GetViewRA(v.centerRA, v.centerDec, roll);
            v.roll = roll; v.zoom = zoom_; v.panX = panX_; v.panY = panY_;
            v.inside = inside_; v.innerZoom = innerZoom_;
        }
        return 0;
    }

    case TQSM_SETVIEW:
        if (l) {
            const SphereView& v = *reinterpret_cast<const SphereView*>(l);
            StopAnimation();
            SetViewRA(v.centerRA, v.centerDec, v.roll);
            zoom_ = Clamp(v.zoom, 0.25, kMaxZoom);
            panX_ = v.panX; panY_ = v.panY;
            inside_ = v.inside;
            innerZoom_ = Clamp(v.innerZoom, 1.0, kMaxInnerZoom);
            RecomputeGeometry();
            InvalidateRect(hwnd_, nullptr, FALSE);
            SendNotify(TQN_VIEWCHANGED, -1);
        }
        return 0;

    case TQSM_SETINSIDE: {
        bool want = (w != 0);
        if (want != inside_) {
            StopAnimation();
            // 内外视角的 R_ 约定相反（球心视角 R_[2] = -center），同一片 R_ 换过来看到的是
            // 对面天区 —— 这是既定语义（见 README「球心视角」），故此处不重建矩阵。
            inside_ = want;
            innerZoom_ = 1.0;
            zoom_ = kMaxZoom;               // 与滚轮自然切换时保持同一状态
            panX_ = 0.0; panY_ = 0.0;
            RecomputeGeometry();
            InvalidateRect(hwnd_, nullptr, FALSE);
            SendNotify(TQN_VIEWCHANGED, -1);
        }
        return 0;
    }

    case TQSM_GOTO:
        if (l) {
            const SphereGoto& g = *reinterpret_cast<const SphereGoto*>(l);
            double z = (g.zoom > 0) ? g.zoom : (inside_ ? innerZoom_ : zoom_);
            if (g.animate) StartAnimation(RaDecToVec(g.ra, g.dec), z);
            else {
                StopAnimation();
                SetViewRA(g.ra, g.dec, 0.0);
                if (inside_) innerZoom_ = Clamp(z, 1.0, kMaxInnerZoom);
                else         zoom_     = Clamp(z, 0.25, kMaxZoom);
                RecomputeGeometry();
                InvalidateRect(hwnd_, nullptr, FALSE);
                SendNotify(TQN_VIEWCHANGED, -1);
            }
        }
        return 0;

    case TQSM_RESET:
        StopAnimation();
        inside_ = true; innerZoom_ = 1.0; zoom_ = 1.0;   // 默认 / 复位回到球心视角
        obsAlt_ = 35.0; roll_ = 0.0;                     // 复位不保留拖动带来的仰角/倾斜
        selConst_ = -1;
        // 复位按当前观测者（地点 + 本地时间）重新定向，而非写死的赤道坐标。
        if (hasObserver_) PointToObserverSky();
        else              SetViewRA(6.0, 10.0, 0.0);
        panX_ = 0.0; panY_ = 0.0;
        selectedIndex_ = hoverIndex_ = -1;
        RecomputeGeometry();
        InvalidateRect(hwnd_, nullptr, FALSE);
        SendNotify(TQN_VIEWCHANGED, -1);
        return 0;

    case TQSM_GETSTATS:
        if (l) {
            SphereStats& s = *reinterpret_cast<SphereStats*>(l);
            double roll;
            s.totalStars = (int)data_.stars.size();
            s.drawnStars = lastDrawn_;
            s.hoverIndex = hoverIndex_;
            s.selectedIndex = selectedIndex_;
            s.zoom = zoom_;
            s.frameMs = frameMs_;
            s.inside = inside_;
            s.innerZoom = innerZoom_;
            GetViewRA(s.centerRA, s.centerDec, roll);
            // 报观测方位而非从 R_ 反算：内外视角的 R_ 约定相反，切换瞬间反算会得到
            // 天球对面的方位，用它回填「方向」框会把观测方向带偏。obsAz_/obsAlt_
            // 已由 SyncObserverFromView 在拖动、定位后同步，足够反映真实视线。
            if (hasObserver_) {
                s.centerAz = obsAz_; s.centerAlt = obsAlt_;
            } else {
                s.centerAz = s.centerAlt = std::nan("");
            }
        }
        return 0;

    case TQSM_HIGHLIGHT:
        if (l) {
            std::wstring kw(reinterpret_cast<const wchar_t*>(l));
            for (size_t i = 0; i < data_.stars.size(); ++i) {
                if (data_.stars[i].name == kw || data_.stars[i].nameEn == kw ||
                    data_.stars[i].name.find(kw) != std::wstring::npos ||
                    data_.stars[i].nameEn.find(kw) != std::wstring::npos) {
                    selectedIndex_ = (int)i;
                    selConst_ = -1;
                    StartAnimation(data_.stars[i].v,
                                   inside_ ? innerZoom_ : max(zoom_, 1.6));
                    SendNotify(TQN_SELECT, selectedIndex_);
                    return 0;
                }
            }
        }
        return 0;

    // ------------------------------------------------------------ 鼠标
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        SetFocus(hwnd_);
        StopAnimation();
        dragging_ = true; moved_ = false;
        panMode_ = (m == WM_RBUTTONDOWN);
        downX_ = lastX_ = (int)(short)LOWORD(l);
        downY_ = lastY_ = (int)(short)HIWORD(l);
        downHit_ = (m == WM_LBUTTONDOWN) ? PickObject(downX_, downY_) : -1;
        SetCapture(hwnd_);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = (int)(short)LOWORD(l), y = (int)(short)HIWORD(l);
        if (dragging_) {
            int dx = x - lastX_, dy = y - lastY_;
            // 用系统拖动阈值（默认 4px）：阈值太小会把"手抖的单击"误判成拖动，月亮/行星点不中
            if (std::abs(x - downX_) + std::abs(y - downY_) > max(3, GetSystemMetrics(SM_CXDRAG)))
                moved_ = true;
            lastX_ = x; lastY_ = y;
            if (moved_) {
                if (panMode_) {
                    // pan 是加性屏幕偏移，与视角无关：右键拖动让星图跟随鼠标
                    panX_ += dx; panY_ += dy;
                }
                else RotateBy(dx, dy);
                InvalidateRect(hwnd_, nullptr, FALSE);
                // 拖动时同步刷新主窗口的状态栏和 15 个按钮，比渲染本身还慢；
                // 这里做 120ms 节流，松手时再补发一次。
                DWORD now = GetTickCount();
                if (now - lastViewNotify_ >= 120) {
                    lastViewNotify_ = now;
                    SendNotify(TQN_VIEWCHANGED, -1);
                }
            }
        } else {
            // 悬停分两路：天体沿用原有命中；星座另算一份，用来加粗高亮边界并显示星座名
            int h = HitTest(x, y);
            int cst = ConstellationAt(x, y);
            if (h != hoverIndex_ || cst != hoverConst_) {
                hoverIndex_ = h;
                hoverConst_ = cst;
                InvalidateRect(hwnd_, nullptr, FALSE);
                SendNotify(TQN_HOVER, hoverIndex_);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
        if (!dragging_) return 0;
        ReleaseCapture();
        dragging_ = false;
        int x = (int)(short)LOWORD(l), y = (int)(short)HIWORD(l);
        if (!moved_) {
            // 负索引是动态天体（-2 月亮 / -100-i 行星），同样允许单击选中；
            // 点空白天区也要发一次 SELECT(-1)，父窗口据此收起信息卡
            if (m == WM_LBUTTONUP) {
                selectedIndex_ = downHit_;
                selConst_ = (selectedIndex_ <= TQ_CONST_BASE)
                          ? TQ_CONST_BASE - selectedIndex_ : -1;
                InvalidateRect(hwnd_, nullptr, FALSE);
                SendNotify(TQN_SELECT, selectedIndex_);
            } else if (m == WM_RBUTTONUP) {
                POINT pt = { x, y };
                ClientToScreen(hwnd_, &pt);
                SendMessageW(GetParent(hwnd_), WM_CONTEXTMENU, (WPARAM)hwnd_,
                             MAKELPARAM(pt.x, pt.y));
            }
        } else {
            SendNotify(TQN_VIEWCHANGED, -1);   // 拖动结束补齐最后一次状态刷新
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        int x = (int)(short)LOWORD(l), y = (int)(short)HIWORD(l);
        Vec3 world;
        if (ScreenToSphere(x, y, world)) {
            int hit = PickObject(x, y);
            if (hit >= 0) {
                selectedIndex_ = hit;                 // 双击星星：确保选中该星
                world = data_.stars[hit].v;           // 用星的真实方向定位，而非点击像素反投影
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (hit == TQ_MOON_INDEX && moonValid_) {
                selectedIndex_ = hit;
                world = moonV_;
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (hit <= TQ_PLANET_BASE && hit > TQ_PLANET_BASE - kPlanetCount) {
                selectedIndex_ = hit;
                world = planets_[TQ_PLANET_BASE - hit].v;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            StartAnimation(world, inside_ ? innerZoom_ : zoom_);
            SendNotify(TQN_DBLCLICK, hit);            // 带星索引，父窗口据此联动星名输入框
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        int sx = (int)(short)LOWORD(l), sy = (int)(short)HIWORD(l);
        POINT pt = { sx, sy };
        ScreenToClient(hwnd_, &pt);
        ZoomAt(pt.x, pt.y, std::pow(1.13, delta / 120.0));
        return 0;
    }

    case WM_KEYDOWN: {
        switch (w) {
        case VK_LEFT:   StopAnimation(); RotateBy(-18, 0); break;
        case VK_RIGHT:  StopAnimation(); RotateBy( 18, 0); break;
        case VK_UP:     StopAnimation(); RotateBy(0, -18); break;
        case VK_DOWN:   StopAnimation(); RotateBy(0,  18); break;
        case VK_ADD: case VK_OEM_PLUS:   ZoomAt((int)cx_, (int)cy_, 1.2); return 0;
        case VK_SUBTRACT: case VK_OEM_MINUS: ZoomAt((int)cx_, (int)cy_, 1.0/1.2); return 0;
        case 'R': SendNotify(TQN_RESET, -1); return 0;   // 交给父窗口做完整复位
        case 'G': ToggleAndNotify(&SphereOptions::showGrid); return 0;
        case 'C': ToggleAndNotify(&SphereOptions::showConstellations); return 0;
        case 'J': ToggleAndNotify(&SphereOptions::showBorders); return 0;
        case 'L': ToggleAndNotify(&SphereOptions::showLabels); return 0;
        case 'E': ToggleAndNotify(&SphereOptions::showEcliptic); return 0;
        case 'D': ToggleAndNotify(&SphereOptions::showDeepSky); return 0;
        case 'N': ToggleAndNotify(&SphereOptions::showConstellationNames); return 0;
        case 'B': ToggleAndNotify(&SphereOptions::showGalactic); return 0;
        case 'Q': ToggleAndNotify(&SphereOptions::showEquator); return 0;
        case 'P': ToggleAndNotify(&SphereOptions::showPlanets); return 0;
        default: return DefWindowProcW(hwnd_, m, w, l);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        SendNotify(TQN_VIEWCHANGED, -1);
        return 0;
    }

    case WM_TIMER:
        if (w == 1 && anim_) {
            animT_ += 0.045;
            double t = Clamp(animT_, 0.0, 1.0);
            double e = t*t*(3.0 - 2.0*t);
            Vec3 c = Slerp(animFrom_, animTo_, e);
            double dec = std::asin(Clamp(c.z, -1.0, 1.0)) * R2D;
            double ra = std::atan2(c.y, c.x) * R2D / 15.0;
            if (ra < 0) ra += 24.0;
            SetViewRA(ra, dec, 0.0);
            double z = animZoomFrom_ + (animZoomTo_ - animZoomFrom_) * e;
            if (inside_) innerZoom_ = Clamp(z, 1.0, kMaxInnerZoom);
            else         zoom_      = Clamp(z, 0.25, kMaxZoom);
            RecomputeGeometry();
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (t >= 1.0) { StopAnimation(); SendNotify(TQN_VIEWCHANGED, -1); }
        }
        else if (w == 2 && autoSpin_) {
            // 经典周日视运动：推进观测时间（恒星时），天球绕固定的极轴旋转。
            // 相机（视线方位 obsAz、天顶、屏幕朝向）保持固定 —— 不再改视图赤经，
            // 否则球心视角下视线会在天空中漂移，出现“转动＋平移叠加”。
            // 1 真实秒 ≈ 24h/恒星日 的模拟小时数；spinRate_ 为倍率。
            ULONGLONG now = GetTickCount64();
            double dt = (double)(now - lastSpinTick_) / 1000.0;
            lastSpinTick_ = now;
            if (dt > 0 && dt < 2.0) {                 // 跳过后台挂起造成的巨跳
                double dHours = (24.0 / 86164.0905) * dt * spinRate_;
                obsHour_ += dHours;   // 允许越过 24h：JulianDate 会正确换算跨天，勿在此归一否则 LST 跳变
                PointToObserverSky();                 // 以 obsAz 重新定向；LST 更新 → 星体周日出没
                InvalidateRect(hwnd_, nullptr, FALSE);
                DWORD tnow = GetTickCount();
                if (tnow - lastViewNotify_ >= 120) {
                    lastViewNotify_ = tnow;
                    SendNotify(TQN_VIEWCHANGED, -1);
                }
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

} // namespace tq
