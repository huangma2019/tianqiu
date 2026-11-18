// WinHTTP 在线星表抓取实现
#include "catalog_download.h"
#include <winhttp.h>
#include "tq_gzip.h"          // 自建 gzip/DEFLATE 解压：解开 .csv.gz 星表
#include <fstream>
#include <sstream>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace tq {

static const std::vector<CatalogSource> kDefaultSources = {
    { L"HYG v4.4 · Codeberg（119614 颗，gzip CSV）",
      "https://codeberg.org/astronexus/hyg/media/branch/main/data/hyg/CURRENT/hyg_v44.csv.gz",
      "online:HYGv44" },
    { L"HYG v4.2 · Codeberg 旧版备用",
      "https://codeberg.org/astronexus/hyg/media/branch/main/data/hyg/OLDER/hyg_v42.csv.gz",
      "online:HYGv42" },
    { L"VizieR · Yale Bright Star Catalogue (BSC5, 9110 颗)",
      "https://vizier.cds.unistra.fr/viz-bin/asu-tsv?-source=V/50&-out=HR,Name,RAJ2000,DEJ2000,Vmag,SpType&-out.max=20000",
      "online:V/50" },
    { L"VizieR 旧域名镜像 (BSC5)",
      "https://vizier.u-strasbg.fr/viz-bin/asu-tsv?-source=V/50&-out=HR,Name,RAJ2000,DEJ2000,Vmag,SpType&-out.max=20000",
      "online:V/50" },
    { L"HYG 存档（GitHub raw 镜像）",
      "https://ghproxy.net/https://raw.githubusercontent.com/astronexus/HYG-Database/master/hygdata_v3.csv",
      "online:HYGv3" },
    { L"HYG 存档（GitHub raw 直连）",
      "https://raw.githubusercontent.com/astronexus/HYG-Database/master/hygdata_v3.csv",
      "online:HYGv3" },
};

const std::vector<CatalogSource>& DefaultSources() { return kDefaultSources; }

std::vector<CatalogSource> LoadSources(const std::wstring& dir) {
    std::wstring p = dir;
    if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
    p += L"sources.txt";
    std::ifstream f(p);
    std::vector<CatalogSource> out;
    if (!f) return out;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = Trim(line);
        if (t.empty() || t[0] == '#') continue;
        auto parts = Split(t, '|');
        if (parts.size() < 2) continue;
        CatalogSource s;
        s.name = Utf8ToWide(Trim(parts[0]));
        s.url  = Trim(parts[1]);
        s.tag  = parts.size() > 2 ? Trim(parts[2]) : std::string("online:custom");
        if (!s.url.empty()) out.push_back(s);
    }
    return out;
}

// ---------------------------------------------------------------- HTTP GET
static bool HttpGetOnce(const std::string& url, std::string& body,
                        std::string& redirect, std::wstring* err) {
    auto fail = [&](const std::string& msg) -> bool {
        if (err) *err = Utf8ToWide(msg);
        return false;
    };
    std::wstring wurl = Utf8ToWide(url);
    URL_COMPONENTSW comp = {};
    comp.dwStructSize = sizeof(comp);
    wchar_t host[256] = {}, path[4096] = {}, extra[4096] = {};
    comp.lpszHostName = host;  comp.dwHostNameLength = _countof(host);
    comp.lpszUrlPath  = path;  comp.dwUrlPathLength  = _countof(path);
    comp.lpszExtraInfo = extra; comp.dwExtraInfoLength = _countof(extra);
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &comp))
        return fail("invalid URL: " + url);

    HINTERNET hSession = WinHttpOpen(L"TQ-CelestialSphere/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return fail("WinHttpOpen failed");
    WinHttpSetTimeouts(hSession, 8000, 8000, 15000, 30000);

    HINTERNET hConn = WinHttpConnect(hSession, host, comp.nPort, 0);
    if (!hConn) { WinHttpCloseHandle(hSession); return fail("WinHttpConnect failed"); }

    std::wstring reqPath = std::wstring(path) + std::wstring(extra);
    if (reqPath.empty()) reqPath = L"/";
    DWORD flags = (comp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", reqPath.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession);
        return fail("WinHttpOpenRequest failed");
    }

    const wchar_t* hdrs =
        L"Accept: */*\r\n"
        L"User-Agent: Mozilla/5.0 (compatible; TQ-CelestialSphere/1.0)\r\n";
    WinHttpAddRequestHeaders(hReq, hdrs, (DWORD)wcslen(hdrs), WINHTTP_ADDREQ_FLAG_ADD);

    BOOL sent = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(hReq, nullptr)) {
        DWORD code = GetLastError();
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession);
        wchar_t b[128]; swprintf_s(b, L"HTTP request failed (Win32 error %lu)", code);
        if (err) *err = b;
        return false;
    }

    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

    if (status >= 300 && status < 400) {
        wchar_t loc[2048] = {}; DWORD llen = sizeof(loc);
        if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                loc, &llen, WINHTTP_NO_HEADER_INDEX) && loc[0]) {
            std::wstring wloc(loc);
            if (wloc.find(L"://") == std::wstring::npos) {
                std::wstring scheme = (comp.nScheme == INTERNET_SCHEME_HTTPS) ? L"https://" : L"http://";
                wloc = scheme + std::wstring(host) + (wloc[0] == L'/' ? L"" : L"/") + wloc;
            }
            redirect = WideToUtf8(wloc);
        }
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession);
        return false;   // 交由上层重定向
    }
    if (status < 200 || status >= 300) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession);
        wchar_t b[128]; swprintf_s(b, L"HTTP status %lu", status);
        if (err) *err = b;
        return false;
    }

    body.clear();
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
        if (body.size() + avail > 64u * 1024u * 1024u) break;   // 上限 64 MB
        // WinHTTP 要求单次读取长度不超过 QueryDataAvailable 报告的可用字节数，
        // 否则会破坏分块数据流；截断问题由 gzip 尾部校验 + 重试来处理。
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(hReq, &chunk[0], avail, &got) || got == 0) break;
        chunk.resize(got);
        body.append(chunk);
    }
    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession);
    if (body.empty()) return fail("empty response body");
    return true;
}

bool HttpGet(const std::string& url, std::string& body, std::wstring* err) {
    std::string current = url;
    for (int hop = 0; hop < 5; ++hop) {
        std::string redirect;
        std::wstring e;
        if (HttpGetOnce(current, body, redirect, &e)) return true;
        if (redirect.empty()) { if (err) *err = e; return false; }
        current = redirect;
    }
    if (err) *err = L"too many redirects";
    return false;
}

// ---------------------------------------------------------------- gzip 解压
// Codeberg / 多数星表站发布的是 .csv.gz；Windows Compression API 不支持标准 GZIP，
// 因此用 tq_gzip.h 中零依赖的 DEFLATE 解压器就地解压。
static bool MaybeGunzip(std::string& body, std::wstring* err) {
    if (body.size() < 3 || (unsigned char)body[0] != 0x1F || (unsigned char)body[1] != 0x8B)
        return true;                                    // 普通文本，无需处理
    std::string plain;
    if (!gzip::Decompress(body, plain, err)) return false;
    body.swap(plain);
    return true;
}

// ---------------------------------------------------------------- 抓取 + 入库
bool FetchAndImport(StarCatalog& catalog, const std::vector<CatalogSource>& sources,
                    DownloadResult& result) {
    result = DownloadResult();
    std::wstring attempts;
    for (const auto& src : sources) {
        // 大文件（十几 MB 的 .csv.gz）偶尔会被 CDN 截断，同一源最多重试一次。
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::string text;
            std::wstring e;
            if (!HttpGet(src.url, text, &e)) {
                attempts += src.name + L"：下载失败（" + e + L"）";
                if (attempt == 0) { attempts += L"，重试…\n"; continue; }
                attempts += L"\n";
                break;
            }
            if (!MaybeGunzip(text, &e)) {
                attempts += src.name + L"：解压失败（" + e + L"）";
                if (attempt == 0) { attempts += L"，重试…\n"; continue; }
                attempts += L"\n";
                break;
            }
            int inserted = 0;
            std::wstring info;
            if (!catalog.ImportText(text, src.tag, inserted, &info)) {
                attempts += src.name + L"：下载成功但解析失败（" + info + L"）\n";
                break;
            }
            result.ok = true;
            result.inserted = inserted;
            result.url = src.url;
            result.message = src.name + L" 抓取成功，" + info + L"\n" + attempts;
            return true;
        }
    }
    result.message = L"所有数据源均不可用：\n" + attempts +
                     L"\n可编辑 sources.txt 自定义数据源，或用「导入本地 CSV」加载已下载的星表文件。";
    return false;
}

} // namespace tq
