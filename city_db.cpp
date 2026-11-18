#include "city_db.h"
#include "sqlite3.h"
#include <windows.h>
#include <string>
#include <cstring>

static sqlite3* g_db = nullptr;

static bool HasColumn(const char* col) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, "PRAGMA table_info(cities)", -1, &st, nullptr) != SQLITE_OK)
        return false;
    bool found = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* n = sqlite3_column_text(st, 1);
        if (n && std::strcmp((const char*)n, col) == 0) { found = true; break; }
    }
    sqlite3_finalize(st);
    return found;
}

static std::wstring ColText16(sqlite3_stmt* st, int col) {
    const void* t = sqlite3_column_text16(st, col);
    int bytes = sqlite3_column_bytes16(st, col);
    if (!t || bytes <= 0) return L"";
    return std::wstring((const wchar_t*)t, (size_t)(bytes / 2));
}

static std::string ColUtf8(sqlite3_stmt* st, int col) {
    const unsigned char* t = sqlite3_column_text(st, col);
    int n = sqlite3_column_bytes(st, col);
    if (!t || n <= 0) return std::string();
    return std::string((const char*)t, (size_t)n);
}

bool citydb::Open() {
    if (g_db) return true;
    wchar_t mod[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, mod, MAX_PATH);
    std::wstring p(mod);
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p = p.substr(0, pos + 1);
    p += L"tq_cities.db";
    if (sqlite3_open16(p.c_str(), &g_db) != SQLITE_OK) { g_db = nullptr; return false; }
    sqlite3_exec(g_db,
        "CREATE TABLE IF NOT EXISTS cities(name TEXT, py TEXT, parent TEXT, lat REAL, lon REAL);"
        "CREATE INDEX IF NOT EXISTS idx_cities_name ON cities(name);"
        "CREATE INDEX IF NOT EXISTS idx_cities_py ON cities(py);",
        nullptr, nullptr, nullptr);
    // 兼容旧版数据库：缺列时补齐，避免查询失败导致下拉为空。
    if (!HasColumn("py"))     sqlite3_exec(g_db, "ALTER TABLE cities ADD COLUMN py TEXT;", nullptr, nullptr, nullptr);
    if (!HasColumn("parent")) sqlite3_exec(g_db, "ALTER TABLE cities ADD COLUMN parent TEXT;", nullptr, nullptr, nullptr);
    return true;
}

int citydb::Count() {
    if (!g_db) return 0;
    sqlite3_stmt* st = nullptr;
    int n = 0;
    if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM cities", -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

int citydb::LoadAll(std::vector<CityRecord>& out) {
    out.clear();
    if (!g_db) return 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db,
            "SELECT name,py,parent,lat,lon FROM cities ORDER BY name", -1, &st, nullptr) != SQLITE_OK)
        return 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        CityRecord r;
        r.name   = ColText16(st, 0);
        r.py     = ColUtf8(st, 1);
        r.parent = ColText16(st, 2);
        r.lat    = sqlite3_column_double(st, 3);
        r.lon    = sqlite3_column_double(st, 4);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    return (int)out.size();
}

bool citydb::Lookup(const std::wstring& name, double& lat, double& lon) {
    if (!g_db) return false;
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT lat,lon FROM cities WHERE name=? LIMIT 1";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text16(st, 1, name.c_str(),
                        (int)(name.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        lat = sqlite3_column_double(st, 0);
        lon = sqlite3_column_double(st, 1);
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}
