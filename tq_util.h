#pragma once
// 公共工具：字符串/编码/数学。仅头文件。
#include <windows.h>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdlib>

namespace tq {

constexpr double PI  = 3.14159265358979323846;
constexpr double D2R = PI / 180.0;
constexpr double R2D = 180.0 / PI;
constexpr double H2R = PI / 12.0;          // 1 hour = 15 degrees

inline double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// UTF-8 源码中的普通字符串 -> 宽字符串（本工程源文件统一 UTF-8 编码）
inline std::wstring W(const char* s) { return s ? Utf8ToWide(std::string(s)) : L""; }

inline std::string Trim(std::string s) {
    const char* ws = " \t\r\n\"'";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

inline std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

// 支持双引号包裹字段的 CSV 行解析
inline std::vector<std::string> SplitCsvLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
            else quoted = !quoted;
        } else if (c == ',' && !quoted) {
            out.push_back(Trim(cur)); cur.clear();
        } else cur += c;
    }
    out.push_back(Trim(cur));
    return out;
}

inline double ToDouble(const std::string& s, double def = 0.0) {
    std::string t = Trim(s);
    if (t.empty()) return def;
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    return (end == t.c_str()) ? def : v;
}

inline int ToInt(const std::string& s, int def = 0) {
    std::string t = Trim(s);
    if (t.empty()) return def;
    char* end = nullptr;
    long v = std::strtol(t.c_str(), &end, 10);
    return (end == t.c_str()) ? def : (int)v;
}

// 解析 "HH MM SS" / "HH:MM:SS" / "+DD MM SS" / "-DD'MM" / 十进制度
// hours 参数仅用于语义说明（数值单位由调用方决定），保留以便扩展。
inline double ParseSexagesimal(const std::string& s, bool /*hours*/, double def = 0.0) {
    std::string t = Trim(s);
    if (t.empty()) return def;
    double sign = 1.0;
    if (t[0] == '-' || t[0] == '+') {
        if (t[0] == '-') sign = -1.0;
        t.erase(0, 1);
    }
    std::vector<std::string> p;
    std::string cur;
    for (char c : t) {
        if (c == ' ' || c == ':' || c == 'h' || c == 'm' || c == 'd' ||
            c == '\t' || c == '\'' || c == '"') {
            if (!cur.empty()) { p.push_back(cur); cur.clear(); }
        } else cur += c;
    }
    if (!cur.empty()) p.push_back(cur);
    if (p.empty()) return def;
    if (p.size() == 1) return sign * ToDouble(p[0], def);   // 已是十进制
    double v = ToDouble(p[0]) + ToDouble(p[1]) / 60.0
             + ToDouble(p.size() > 2 ? p[2] : "0") / 3600.0;
    return sign * v;
}

inline void FormatRA(double hours, wchar_t* buf, size_t n) {
    double h = hours - std::floor(hours / 24.0) * 24.0;
    int hh = (int)h;
    int mm = (int)((h - hh) * 60.0);
    double ss = ((h - hh) * 60.0 - mm) * 60.0;
    swprintf_s(buf, n, L"%02dh %02dm %04.1fs", hh, mm, ss);
}

inline void FormatDec(double deg, wchar_t* buf, size_t n) {
    wchar_t sign = deg >= 0 ? L'+' : L'-';
    double a = std::fabs(deg);
    int dd = (int)a;
    int mm = (int)((a - dd) * 60.0);
    double ss = ((a - dd) * 60.0 - mm) * 60.0;
    swprintf_s(buf, n, L"%c%02d\u00B0 %02d' %04.1f\"", sign, dd, mm, ss);
}

// 光谱型 -> 近似 RGB（O 蓝白 -> M 橙红）
inline COLORREF SpectralColor(const std::string& sp) {
    if (sp.empty()) return RGB(240, 244, 255);
    char c = sp[0];
    if (c >= 'a' && c <= 'z') c -= 32;
    switch (c) {
    case 'O': return RGB(170, 195, 255);
    case 'B': return RGB(195, 216, 255);
    case 'A': return RGB(232, 240, 255);
    case 'F': return RGB(255, 250, 235);
    case 'G': return RGB(255, 241, 205);
    case 'K': return RGB(255, 214, 165);
    case 'M': return RGB(255, 178, 140);
    case 'W': return RGB(180, 210, 255);
    case 'C': return RGB(255, 160, 130);
    default:  return RGB(240, 244, 255);
    }
}

} // namespace tq
