#pragma once
// 星表数据层：SQLite 持久化 + 内置精选星表 + 文本/CSV/TSV 导入
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <mutex>
#include "sqlite3.h"
#include "tq_util.h"

namespace tq {

struct Vec3 { double x = 0, y = 0, z = 0; };

// 是否含 CJK 汉字（UTF-16 基本区）
inline bool HasHanChar(const std::wstring& s) {
    for (wchar_t c : s)
        if (c >= 0x4E00 && c <= 0x9FFF) return true;
    return false;
}

inline Vec3 RaDecToVec(double raHours, double decDeg) {
    double a = raHours * H2R, d = decDeg * D2R;
    double cd = std::cos(d);
    return { cd * std::cos(a), cd * std::sin(a), std::sin(d) };
}

// 带声调拼音字母 -> 基础拉丁字母（用于取音节首字母）
inline wchar_t ToneBaseLetter(wchar_t c) {
    if (c < 0x80) return c;
    switch (c) {
    case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
    case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
    case 0x0100: case 0x0101: case 0x01CD: case 0x01CE: return L'a';   // Ā ā Ǎ ǎ À-Å
    case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB:
    case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB:
    case 0x0112: case 0x0113: case 0x011A: case 0x011B: return L'e';   // Ē ē Ě ě
    case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF:
    case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF:
    case 0x012A: case 0x012B: case 0x01CF: case 0x01D0: return L'i';   // Ī ī Ǐ ǐ
    case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8:
    case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8:
    case 0x014C: case 0x014D: case 0x01D1: case 0x01D2: return L'o';   // Ō ō Ǒ ǒ
    case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC:
    case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC:
    case 0x016A: case 0x016B: case 0x01D3: case 0x01D4:
    case 0x01D5: case 0x01D6: case 0x01D7: case 0x01D8:
    case 0x01D9: case 0x01DA: case 0x01DB: case 0x01DC: return L'u';   // Ū ū Ǔ ǔ ü ǖ...
    default: return 0;
    }
}

// 从带声调拼音文本提取各音节首字母（词内音节连写，如 "Wángliáng" -> "wl"）
inline std::string PinyinSyllableInitials(const std::wstring& s) {
    std::string out;
    bool started = false, inVowel = false;
    for (wchar_t wc : s) {
        wchar_t b = ToneBaseLetter(wc);
        if (!b) { started = false; inVowel = false; continue; }   // 分隔符
        if (b >= L'A' && b <= L'Z') b += 32;
        if (b < L'a' || b > L'z') continue;
        char c = (char)b;
        bool isV = (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'v');
        if (!started) { out += c; started = true; inVowel = isV; continue; }
        if (isV) { inVowel = true; continue; }
        if (!inVowel) continue;                 // 声母连写（zh/ch/sh）
        if (c == 'n' || c == 'g') continue;     // 鼻音韵尾
        out += c; inVowel = false;              // 新音节声母
    }
    return out;
}

// 从 "标签\t值\n..." 文本中取某标签对应的值
inline std::wstring InfoFieldValue(const std::wstring& info, const std::wstring& label) {
    size_t p = info.find(label + L"\t");
    if (p == std::wstring::npos) return L"";
    p += label.size() + 1;
    size_t e = info.find(L'\n', p);
    if (e == std::wstring::npos) e = info.size();
    return info.substr(p, e - p);
}

// ---------------------------------------------------------------- 数据结构
struct Star {
    long long      id = 0;
    std::wstring   name;         // 中文/常用名
    std::wstring   nameCn;       // 中文名（运行时按 HIP 关联推导；无中文名则为空）
    std::wstring   nameEn;       // 英文名 / 拜耳编号 / 星表编号
    double         ra = 0;       // 赤经（小时 0..24）
    double         dec = 0;      // 赤纬（度 -90..90）
    double         mag = 6.0;    // 视星等（越小越亮）
    std::string    spectral;     // 光谱型
    std::string    con;          // 星座三字母缩写
    int            hip = 0;
    std::string    source;       // 数据来源标签
    std::wstring   info;         // 中国传统星名等附加数据（标签\t值\n，来自 stars.info 列）
    std::string    py;           // 拼音音节首字母（如 王良增九 -> "wlzix"），在线星回退用英文名
    Vec3           v;            // 单位向量（加载后预计算）
    COLORREF       color = RGB(255, 255, 255);
    bool operator<(const Star& o) const { return mag < o.mag; }   // 亮的排前面
};

struct ConstLine {
    std::string con;
    std::wstring nameCn;
    std::vector<std::pair<double, double>> pts;   // (ra hours, dec deg)
};

struct DeepSky {
    std::wstring name;
    std::wstring kind;
    double ra = 0, dec = 0, mag = 0;
    Vec3   v;
};

// ---------------------------------------------------------------- 星座贴图
// 数据来自 ziliao/constellation-db（Johan Meuris 的 88 星座具象图绘），
// 由 tools_import_art.py 解算成球面顶点后写入 tq_catalog.db 的 const_art 表。
// 顶点存的是 (ra 度, dec 度)，绘制时逐点投影，所以在任意投影/缩放下都不失真。
struct ArtLayer {
    int                tone = 60;     // 灰度 0..255，越大越亮
    std::vector<int>   counts;        // 每段点数；负值表示开放折线（只描边不填充）
    std::vector<float> radec;         // ra_deg, dec_deg 交替
};

struct ConstArt {
    std::string           abbr;       // IAU 三字母缩写，如 "Ori"
    double                centerRa = 0, centerDec = 0;   // 贴图中心（度），用于视场裁剪
    std::vector<ArtLayer> fill;       // 4 个亮度层，由暗到亮（tone 越大越亮）
};

// ---------------------------------------------------------------- 星座资料
// 数据来自 ziliao/88星座边界数据（88 星座传说与来源 + IAU 边界多边形），
// 由 tools_import_const_info.py 写入 tq_catalog.db 的 constellation_info /
// constellation_boundary 两表；悬停高亮边界、点击看星座资料都读这里。
struct ConstInfo {
    std::string  abbr;        // IAU 三字母缩写，如 "And"
    std::wstring nameCn;      // 中文名（仙女座）
    std::wstring nameLat;     // 拉丁名（Andromeda）
    std::wstring genitive;    // 所有格
    std::wstring meaning;     // 含义 / 象征
    std::wstring origin;      // 文化根源
    std::wstring myth;        // 神话传说
    std::wstring history;     // 命名历史与出处
    std::wstring family;      // 星座家族
    double       labelRa = 0, labelDec = 0;   // 星座名的标注点（ra 小时 / dec 度）
};

// 星座边界多边形。rings 里每个环是一串 (ra 小时, dec 度)，ra 已跨 0h 展平为
// 连续值（可能 <0 或 >24），点在多边形内判定与包围盒裁剪都靠这个展平值。
struct ConstBoundary {
    std::string abbr;
    std::vector<std::vector<std::pair<double, double>>> rings;
    double minRa = 0, maxRa = 0, minDec = 90, maxDec = -90;
};

struct CatalogData {
    std::vector<Star>      stars;      // 已按星等升序
    std::vector<ConstLine> lines;      // 星座图案连线（按折线组）
    std::vector<ConstLine> borders;    // IAU 官方星座边界（J2000，程序内置）
    std::vector<DeepSky>   deepsky;
    std::vector<ConstArt>  art;        // 星座贴图（填充版，来自数据库）
    std::vector<ConstInfo> constInfo;  // 星座资料（88 条，按缩写排序）
    std::vector<ConstBoundary> boundary;   // 星座边界多边形（与 constInfo 同序）
    void Prepare() {
        // 中文名可能来自内置星表记录，而在线星表记录只有英文名；
        // 两者按 HIP 对应，故先建 HIP->中文名 映射，再给每条记录推导中文名。
        std::map<int, std::wstring> cnByHip;
        for (const auto& s : stars)
            if (s.hip > 0 && HasHanChar(s.name))
                cnByHip.emplace(s.hip, s.name);
        for (auto& s : stars) {
            if (HasHanChar(s.name)) {
                s.nameCn = s.name;
            } else {
                auto it = cnByHip.find(s.hip);
                s.nameCn = (s.hip > 0 && it != cnByHip.end()) ? it->second : L"";
            }
            s.v = RaDecToVec(s.ra, s.dec);
            s.color = SpectralColor(s.spectral);
            // 拼音首字母：优先取 info 的「拼音」行，否则回退为英文名折叠
            s.py = PinyinSyllableInitials(InfoFieldValue(s.info, L"拼音"));
            if (s.py.empty()) {
                for (wchar_t c : s.nameEn) {
                    if (c >= L'A' && c <= L'Z') s.py += (char)(c - L'A' + 'a');
                    else if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) s.py += (char)c;
                }
            }
        }
        std::sort(stars.begin(), stars.end());
        for (auto& d : deepsky) d.v = RaDecToVec(d.ra, d.dec);
    }
};

// 按 IAU 缩写查星座资料（88 条线性查找足够快，调用点都在鼠标/单击事件里）
inline const ConstInfo* FindConstInfo(const CatalogData& d, const std::string& abbr) {
    for (const auto& c : d.constInfo)
        if (c.abbr == abbr) return &c;
    return nullptr;
}

// ---------------------------------------------------------------- 数据库封装
class StarCatalog {
public:
    StarCatalog() = default;
    ~StarCatalog() { Close(); }
    StarCatalog(const StarCatalog&) = delete;
    StarCatalog& operator=(const StarCatalog&) = delete;

    bool Open(const std::wstring& dbPath, std::wstring* err = nullptr);
    void Close();
    bool IsOpen() const { return db_ != nullptr; }
    std::wstring Path() const { return path_; }

    // 首次启动：写入内置亮星 / 星座连线 / 深空天体
    bool SeedBuiltinIfEmpty(std::wstring* err = nullptr);
    bool ForceSeedBuiltin(std::wstring* err = nullptr);   // 覆盖同名内置记录

    // 从数据库载入内存（magLimit 控制星等上限）
    bool Reload(CatalogData& out, double magLimit, std::wstring* err = nullptr);

    long long StarCount(std::wstring* err = nullptr) const;
    std::wstring MetaGet(const char* key) const;
    bool MetaSet(const char* key, const std::wstring& value);

    // 按名称模糊搜索（返回 SQL LIKE 结果）
    std::vector<Star> Search(const std::wstring& keyword, int limit = 40) const;

    // 导入在线抓取 / 本地文件的纯文本（自动识别 CSV / VizieR TSV）
    bool ImportText(const std::string& text, const std::string& sourceTag,
                    int& inserted, std::wstring* err = nullptr);
    bool ImportFile(const std::wstring& path, int& inserted, std::wstring* err = nullptr);

    bool ClearImported(std::wstring* err = nullptr);

private:
    bool CreateSchema(std::wstring* err);
    bool SeedLinesAndDeepSky(std::wstring* err);
    bool InsertStar(const Star& s, sqlite3_stmt* stmt);
    void LoadConstellationArt(CatalogData& out);   // 读 const_art 表（旧库无此表则跳过）
    void LoadConstellationInfo(CatalogData& out);  // 读 constellation_info / _boundary 表

    sqlite3*       db_ = nullptr;
    std::wstring   path_;
    mutable std::mutex mu_;
};

// 内置亮星表（离线可用，含中文名），定义于 star_catalog_data.cpp 之外，见 star_catalog.cpp
const std::vector<Star>& BuiltinStars();
const std::vector<ConstLine>& BuiltinLines();
const std::vector<ConstLine>& BuiltinBorders();
const std::vector<DeepSky>& BuiltinDeepSky();

// 星座缩写 -> 中文名
std::wstring ConstellationName(const std::string& abbr);

} // namespace tq
