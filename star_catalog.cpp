// SQLite 星表存储层实现
#include "star_catalog.h"
#include <fstream>
#include <sstream>
#include <cctype>
#include <map>

namespace tq {

static void SetErr(std::wstring* err, const std::string& msg) {
    if (err) *err = Utf8ToWide(msg);
}
static void SetErrSql(std::wstring* err, const char* where, sqlite3* db) {
    if (err) *err = Utf8ToWide(std::string(where) + ": " + (db ? sqlite3_errmsg(db) : "no db"));
}
static std::string Lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// ------------------------------------------------------------------ Open/Close
bool StarCatalog::Open(const std::wstring& dbPath, std::wstring* err) {
    std::lock_guard<std::mutex> lk(mu_);
    Close();
    path_ = dbPath;
    std::string p = WideToUtf8(dbPath);
    int rc = sqlite3_open(p.c_str(), &db_);
    if (rc != SQLITE_OK) { SetErrSql(err, "sqlite3_open", db_); sqlite3_close(db_); db_ = nullptr; return false; }
    sqlite3_busy_timeout(db_, 5000);
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    return CreateSchema(err);
}

void StarCatalog::Close() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

bool StarCatalog::CreateSchema(std::wstring* err) {
    const char* sql =
        "CREATE TABLE IF NOT EXISTS stars("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name TEXT NOT NULL DEFAULT '',"
        " name_en TEXT NOT NULL DEFAULT '',"
        " ra REAL NOT NULL,"          /* 赤经：小时 0..24 */
        " dec REAL NOT NULL,"         /* 赤纬：度 -90..90 */
        " mag REAL NOT NULL DEFAULT 6.0,"
        " spectral TEXT DEFAULT '',"
        " con TEXT DEFAULT '',"
        " hip INTEGER DEFAULT 0,"
        " source TEXT DEFAULT '',"
        " info TEXT DEFAULT '');"
        "CREATE INDEX IF NOT EXISTS idx_stars_mag ON stars(mag);"
        "CREATE INDEX IF NOT EXISTS idx_stars_src ON stars(source);"
        "CREATE INDEX IF NOT EXISTS idx_stars_name ON stars(name);"
        "CREATE INDEX IF NOT EXISTS idx_stars_name_en ON stars(name_en);"
        "CREATE TABLE IF NOT EXISTS constellations("
        " abbr TEXT, name_cn TEXT, seq INTEGER, ra REAL, dec REAL);"
        "CREATE INDEX IF NOT EXISTS idx_con_abbr ON constellations(abbr,seq);"
        "CREATE TABLE IF NOT EXISTS deepsky("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name TEXT, kind TEXT, ra REAL, dec REAL, mag REAL);"
        "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT);";
    char* e = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &e) != SQLITE_OK) {
        SetErr(err, e ? e : "create schema failed"); sqlite3_free(e); return false;
    }
    // 兼容旧库：CREATE TABLE IF NOT EXISTS 不会给已有表补列
    bool hasInfo = false;
    sqlite3_stmt* chk = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA table_info(stars)", -1, &chk, nullptr) == SQLITE_OK) {
        while (sqlite3_step(chk) == SQLITE_ROW) {
            const unsigned char* n = sqlite3_column_text(chk, 1);
            if (n && std::string((const char*)n) == "info") { hasInfo = true; break; }
        }
        sqlite3_finalize(chk);
    }
    if (!hasInfo)
        sqlite3_exec(db_, "ALTER TABLE stars ADD COLUMN info TEXT DEFAULT '';",
                     nullptr, nullptr, nullptr);
    return true;
}

// ------------------------------------------------------------------ Meta
std::wstring StarCatalog::MetaGet(const char* key) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return L"";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT v FROM meta WHERE k=?", -1, &st, nullptr) != SQLITE_OK) return L"";
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    std::wstring r;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* t = sqlite3_column_text(st, 0);
        if (t) r = Utf8ToWide(reinterpret_cast<const char*>(t));
    }
    sqlite3_finalize(st);
    return r;
}

bool StarCatalog::MetaSet(const char* key, const std::wstring& value) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO meta(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    std::string v = WideToUtf8(value);
    sqlite3_bind_text(st, 2, v.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

long long StarCatalog::StarCount(std::wstring* err) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) { SetErr(err, "database not open"); return 0; }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM stars", -1, &st, nullptr) != SQLITE_OK) return 0;
    long long n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

// ------------------------------------------------------------------ 内置数据
bool StarCatalog::InsertStar(const Star& s, sqlite3_stmt* st) {
    std::string a = WideToUtf8(s.name), b = WideToUtf8(s.nameEn);
    std::string inf = WideToUtf8(s.info);
    sqlite3_reset(st); sqlite3_clear_bindings(st);
    sqlite3_bind_text(st, 1, a.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, b.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 3, s.ra);
    sqlite3_bind_double(st, 4, s.dec);
    sqlite3_bind_double(st, 5, s.mag);
    sqlite3_bind_text(st, 6, s.spectral.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, s.con.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 8, s.hip);
    sqlite3_bind_text(st, 9, s.source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 10, inf.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(st) == SQLITE_DONE;
}

bool StarCatalog::SeedLinesAndDeepSky(std::wstring* err) {
    // 星座连线 / 边界已随程序内置（constellation_data.cpp），不再落库；
    // 顺带清掉旧版 tq_catalog.db 的 constellations 表，避免两处数据不一致。
    sqlite3_exec(db_, "DELETE FROM constellations; DELETE FROM deepsky;", nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO deepsky(name,kind,ra,dec,mag) VALUES(?,?,?,?,?)",
                           -1, &st, nullptr) != SQLITE_OK) { SetErrSql(err, "prepare deepsky", db_); return false; }
    for (const auto& d : BuiltinDeepSky()) {
        std::string n = WideToUtf8(d.name), k = WideToUtf8(d.kind);
        sqlite3_reset(st); sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, n.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, k.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 3, d.ra);
        sqlite3_bind_double(st, 4, d.dec);
        sqlite3_bind_double(st, 5, d.mag);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    return true;
}

bool StarCatalog::SeedBuiltinIfEmpty(std::wstring* err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) { SetErr(err, "database not open"); return false; }
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM stars", -1, &st, nullptr);
    long long n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (n > 0) return true;      // 已有数据，不重复写入

    char* e = nullptr;
    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, &e); sqlite3_free(e);
    if (sqlite3_prepare_v2(db_,
        "INSERT INTO stars(name,name_en,ra,dec,mag,spectral,con,hip,source,info) VALUES(?,?,?,?,?,?,?,?,?,?)",
        -1, &st, nullptr) != SQLITE_OK) { SetErrSql(err, "prepare stars", db_); return false; }
    for (const auto& s : BuiltinStars()) InsertStar(s, st);
    sqlite3_finalize(st);
    bool ok = SeedLinesAndDeepSky(err);
    sqlite3_exec(db_, ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
    // 注意：此处已持有 mu_，不能再调用会加锁的 MetaSet（std::mutex 非递归）。
    // 是否已经播种可直接通过 stars 表判断，无需写 meta。
    return ok;
}

bool StarCatalog::ForceSeedBuiltin(std::wstring* err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) { SetErr(err, "database not open"); return false; }
    sqlite3_stmt* st = nullptr;
    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_prepare_v2(db_, "DELETE FROM stars WHERE source='builtin'", -1, &st, nullptr);
    sqlite3_step(st); sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db_,
        "INSERT INTO stars(name,name_en,ra,dec,mag,spectral,con,hip,source,info) VALUES(?,?,?,?,?,?,?,?,?,?)",
        -1, &st, nullptr) != SQLITE_OK) { SetErrSql(err, "prepare stars", db_); return false; }
    for (const auto& s : BuiltinStars()) InsertStar(s, st);
    sqlite3_finalize(st);
    bool ok = SeedLinesAndDeepSky(err);
    sqlite3_exec(db_, ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
    return ok;
}

bool StarCatalog::ClearImported(std::wstring* err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) { SetErr(err, "database not open"); return false; }
    char* e = nullptr;
    int rc = sqlite3_exec(db_, "DELETE FROM stars WHERE source<>'builtin'", nullptr, nullptr, &e);
    if (rc != SQLITE_OK) { SetErr(err, e ? e : "delete failed"); sqlite3_free(e); return false; }
    return true;
}

// ------------------------------------------------------------------ Reload
static void ReadStarRow(sqlite3_stmt* st, Star& s) {
    auto text = [&](int i) -> std::string {
        const unsigned char* p = sqlite3_column_text(st, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    };
    s.id       = sqlite3_column_int64(st, 0);
    s.name     = Utf8ToWide(text(1));
    s.nameEn   = Utf8ToWide(text(2));
    s.ra       = sqlite3_column_double(st, 3);
    s.dec      = sqlite3_column_double(st, 4);
    s.mag      = sqlite3_column_double(st, 5);
    s.spectral = text(6);
    s.con      = text(7);
    s.hip      = sqlite3_column_int(st, 8);
    s.source   = text(9);
    s.info     = Utf8ToWide(text(10));
}

bool StarCatalog::Reload(CatalogData& out, double magLimit, std::wstring* err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) { SetErr(err, "database not open"); return false; }
    out = CatalogData();

    sqlite3_stmt* st = nullptr;
    const char* q = "SELECT id,name,name_en,ra,dec,mag,spectral,con,hip,source,info "
                    "FROM stars WHERE mag<=? ORDER BY mag ASC";
    if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) != SQLITE_OK) { SetErrSql(err, "prepare reload", db_); return false; }
    sqlite3_bind_double(st, 1, magLimit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Star s; ReadStarRow(st, s); out.stars.push_back(std::move(s));
    }
    sqlite3_finalize(st);

    // 星座图案连线 / IAU 官方边界都随程序内置（constellation_data.cpp，J2000）。
    // 不从数据库读取，否则旧版 tq_catalog.db 里的演示连线会覆盖新数据。
    out.lines   = BuiltinLines();
    out.borders = BuiltinBorders();

    if (sqlite3_prepare_v2(db_, "SELECT name,kind,ra,dec,mag FROM deepsky", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            DeepSky d;
            const unsigned char* p = sqlite3_column_text(st, 0);
            d.name = p ? Utf8ToWide(reinterpret_cast<const char*>(p)) : L"";
            p = sqlite3_column_text(st, 1);
            d.kind = p ? Utf8ToWide(reinterpret_cast<const char*>(p)) : L"";
            d.ra = sqlite3_column_double(st, 2);
            d.dec = sqlite3_column_double(st, 3);
            d.mag = sqlite3_column_double(st, 4);
            out.deepsky.push_back(d);
        }
        sqlite3_finalize(st);
    }

    LoadConstellationArt(out);
    LoadConstellationInfo(out);
    out.Prepare();
    return true;
}

// 星座资料 / 边界多边形（tools_import_const_info.py 导入）。
// 旧库没有这两张表时静默跳过，功能退化为"不显示星座资料"。
void StarCatalog::LoadConstellationInfo(CatalogData& out) {
    sqlite3_stmt* st = nullptr;
    auto text = [](sqlite3_stmt* s, int col) -> std::wstring {
        const unsigned char* p = sqlite3_column_text(s, col);
        return p ? Utf8ToWide(reinterpret_cast<const char*>(p)) : std::wstring();
    };

    if (sqlite3_prepare_v2(db_,
            "SELECT abbr,name_cn,name_lat,genitive,meaning,origin,myth,history,family,"
            "label_ra,label_dec FROM constellation_info ORDER BY abbr",
            -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            ConstInfo ci;
            const unsigned char* a = sqlite3_column_text(st, 0);
            ci.abbr    = a ? reinterpret_cast<const char*>(a) : "";
            ci.nameCn  = text(st, 1);
            ci.nameLat = text(st, 2);
            ci.genitive = text(st, 3);
            ci.meaning = text(st, 4);
            ci.origin  = text(st, 5);
            ci.myth    = text(st, 6);
            ci.history = text(st, 7);
            ci.family  = text(st, 8);
            ci.labelRa  = sqlite3_column_double(st, 9);
            ci.labelDec = sqlite3_column_double(st, 10);
            out.constInfo.push_back(std::move(ci));
        }
        sqlite3_finalize(st);
    }

    if (sqlite3_prepare_v2(db_,
            "SELECT abbr,ring,ra_h,dec_d FROM constellation_boundary ORDER BY abbr,ring,seq",
            -1, &st, nullptr) == SQLITE_OK) {
        int curRing = -1;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* a = sqlite3_column_text(st, 0);
            std::string abbr = a ? reinterpret_cast<const char*>(a) : "";
            const int ring = sqlite3_column_int(st, 1);
            const double ra = sqlite3_column_double(st, 2);
            const double dec = sqlite3_column_double(st, 3);
            if (out.boundary.empty() || out.boundary.back().abbr != abbr) {
                ConstBoundary nb;
                nb.abbr = abbr;
                nb.minRa = 1e9; nb.maxRa = -1e9;
                nb.minDec = 1e9; nb.maxDec = -1e9;
                out.boundary.push_back(std::move(nb));
                curRing = -1;
            }
            ConstBoundary& b = out.boundary.back();
            if (ring != curRing) { b.rings.emplace_back(); curRing = ring; }
            auto& pts = b.rings.back();
            // ra 跨 0h 时按"最短跨越"选择相邻份，展平成连续值（可能 <0 或 >24）
            double r = ra;
            if (!pts.empty()) {
                const double prev = pts.back().first;
                while (r - prev > 12.0)  r -= 24.0;
                while (r - prev < -12.0) r += 24.0;
            }
            pts.emplace_back(r, dec);
            if (r < b.minRa) b.minRa = r;
            if (r > b.maxRa) b.maxRa = r;
            if (dec < b.minDec) b.minDec = dec;
            if (dec > b.maxDec) b.maxDec = dec;
        }
        sqlite3_finalize(st);
    }
}

// 星座贴图：const_art 表每行是一个「星座 + 图层」，只取填充版（style='fill'）；
// counts 为 int32 数组（每段点数），pts 为 float32 数组（ra,dec 交替）。
void StarCatalog::LoadConstellationArt(CatalogData& out) {
    sqlite3_stmt* st = nullptr;

    std::map<std::string, std::pair<double, double>> centers;
    if (sqlite3_prepare_v2(db_, "SELECT abbr,center_ra,center_dec FROM const_art_info",
                           -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* a = sqlite3_column_text(st, 0);
            if (!a) continue;
            centers[reinterpret_cast<const char*>(a)] =
                { sqlite3_column_double(st, 1), sqlite3_column_double(st, 2) };
        }
        sqlite3_finalize(st);
    }

    if (sqlite3_prepare_v2(db_,
            "SELECT abbr,layer,tone,counts,pts FROM const_art "
            "WHERE style='fill' ORDER BY abbr,layer",
            -1, &st, nullptr) != SQLITE_OK)
        return;      // 老版本数据库没有贴图表：静默跳过

    std::map<std::string, size_t> index;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* a = sqlite3_column_text(st, 0);
        if (!a) continue;
        std::string abbr = reinterpret_cast<const char*>(a);

        auto it = index.find(abbr);
        if (it == index.end()) {
            out.art.emplace_back();
            out.art.back().abbr = abbr;
            auto c = centers.find(abbr);
            if (c != centers.end()) {
                out.art.back().centerRa  = c->second.first;
                out.art.back().centerDec = c->second.second;
            } else {
                out.art.back().centerRa = 0; out.art.back().centerDec = 0;
            }
            it = index.emplace(abbr, out.art.size() - 1).first;
        }

        ArtLayer layer;
        layer.tone = sqlite3_column_int(st, 2);
        const void* cb = sqlite3_column_blob(st, 3);
        int cn = sqlite3_column_bytes(st, 3);
        const void* pb = sqlite3_column_blob(st, 4);
        int pn = sqlite3_column_bytes(st, 4);
        if (cb && cn >= 4) {
            const int* ic = static_cast<const int*>(cb);
            layer.counts.assign(ic, ic + cn / 4);
        }
        if (pb && pn >= 8) {
            const float* fp = static_cast<const float*>(pb);
            layer.radec.assign(fp, fp + pn / 4);
        }
        if (layer.counts.empty() || layer.radec.empty()) continue;

        out.art[it->second].fill.push_back(std::move(layer));
    }
    sqlite3_finalize(st);
}

std::vector<Star> StarCatalog::Search(const std::wstring& keyword, int limit) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Star> out;
    if (!db_ || keyword.empty()) return out;
    sqlite3_stmt* st = nullptr;
    const char* q = "SELECT id,name,name_en,ra,dec,mag,spectral,con,hip,source,info FROM stars "
                    "WHERE name LIKE ? OR name_en LIKE ? OR info LIKE ? ORDER BY mag ASC LIMIT ?";
    if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) != SQLITE_OK) return out;
    std::string k = "%" + WideToUtf8(keyword) + "%";
    sqlite3_bind_text(st, 1, k.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, k.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, k.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Star s; ReadStarRow(st, s); out.push_back(std::move(s));
    }
    sqlite3_finalize(st);
    return out;
}

// ------------------------------------------------------------------ 文本解析
struct HeaderMap {
    std::map<std::string, int> col;
    bool has(const char* k) const { return col.find(k) != col.end(); }
    int get(const char* k, int def = -1) const {
        auto it = col.find(k); return it == col.end() ? def : it->second;
    }
    std::string field(const std::vector<std::string>& v, const char* k) const {
        int i = get(k); return (i >= 0 && i < (int)v.size()) ? Trim(v[i]) : "";
    }
    std::string firstField(const std::vector<std::string>& v,
                           std::initializer_list<const char*> keys) const {
        for (auto k : keys) { std::string s = field(v, k); if (!s.empty()) return s; }
        return "";
    }
};

static HeaderMap MakeHeader(const std::vector<std::string>& cells) {
    HeaderMap m;
    for (size_t i = 0; i < cells.size(); ++i) {
        std::string k = Lower(Trim(cells[i]));
        // 去掉 VizieR 表头可能的 '#' 与空格
        std::string t;
        for (char c : k) if (!std::isspace((unsigned char)c) && c != '#' && c != '-' && c != '_') t += c;
        if (!t.empty() && m.col.find(t) == m.col.end()) m.col[t] = (int)i;
        if (!k.empty() && m.col.find(k) == m.col.end()) m.col[k] = (int)i;
    }
    return m;
}

// 把文本解析为星表（自动识别 CSV / VizieR TSV）
static bool ParseCatalogText(const std::string& raw, std::vector<Star>& out,
                             std::wstring& format, std::wstring* err) {
    std::string text = raw;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.erase(0, 3);

    std::vector<std::string> lines;
    {
        std::istringstream is(text);
        std::string l;
        while (std::getline(is, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!Trim(l).empty()) lines.push_back(l);
        }
    }
    if (lines.size() < 2) { SetErr(err, "empty catalog text"); return false; }

    // 找表头：VizieR 会在一列 '#' 注释之后输出列名行（同样以 '#' 开头）
    size_t hi = 0;
    bool headerIsComment = false;
    for (; hi < lines.size(); ++hi) {
        const std::string& l = lines[hi];
        if (l[0] == '#') {
            std::string ll = Lower(l);
            bool looksHeader = ll.find("raj2000") != std::string::npos ||
                               ll.find("dej2000") != std::string::npos ||
                               (ll.find("ra") != std::string::npos && ll.find("dec") != std::string::npos) ||
                               (ll.find("proper") != std::string::npos && ll.find("mag") != std::string::npos);
            if (looksHeader) { headerIsComment = true; break; }
            continue;
        }
        break;
    }
    if (hi >= lines.size()) { SetErr(err, "no header found"); return false; }

    std::string headerLine = headerIsComment ? lines[hi].substr(1) : lines[hi];
    while (!headerLine.empty() && std::isspace((unsigned char)headerLine[0])) headerLine.erase(0, 1);

    char sep = (headerLine.find('\t') != std::string::npos) ? '\t' : ',';
    bool isTsv = (sep == '\t');
    auto split = [&](const std::string& l) {
        if (isTsv) {
            if (l.find('\t') == std::string::npos) {
                std::vector<std::string> v;
                std::istringstream ss(l); std::string x;
                while (ss >> x) v.push_back(x);
                return v;
            }
            std::vector<std::string> v;
            std::string cur;
            for (char c : l) {
                if (c == '\t') { v.push_back(Trim(cur)); cur.clear(); }
                else cur += c;
            }
            v.push_back(Trim(cur));
            return v;
        }
        return SplitCsvLine(l);
    };

    HeaderMap h = MakeHeader(split(headerLine));
    size_t dataStart = hi + 1;

    bool vizier = isTsv && (h.has("raj2000") || h.has("dej2000"));
    bool csv    = h.has("ra") || h.has("rah") || h.has("radeg") || h.has("raj2000");
    if (!vizier && !csv) {
        // 无表头：尝试按 4 列 (name,ra,dec,mag) 或 (ra,dec,mag,name) 推断
        h.col.clear();
        h.col["name"] = 0; h.col["ra"] = 1; h.col["dec"] = 2; h.col["mag"] = 3;
        dataStart = hi;
        format = L"无表头（按 name,ra,dec,mag 推断）";
    } else {
        format = vizier ? L"VizieR TSV" : L"CSV";
    }

    auto idxOf = [&](std::initializer_list<const char*> keys) -> int {
        for (auto k : keys) { int i = h.get(k); if (i >= 0) return i; }
        return -1;
    };
    int iRA = idxOf({"ra", "rah", "radeg", "raj2000", "ra2000", "raicrs", "raj"});
    int iDE = idxOf({"dec", "dedeg", "dej2000", "de2000", "deicrs", "dej", "declination"});
    int iMag = idxOf({"mag", "vmag", "v", "vmagnitude", "visualmag", "photmag"});
    int iName = idxOf({"proper", "name", "nameen", "starname", "designation", "commonname"});
    int iSp = idxOf({"spect", "sptype", "spectral", "sp", "spectype", "type"});
    int iCon = idxOf({"con", "constellation", "const", "conabbr"});
    int iHip = idxOf({"hip", "hipparcos", "hipno"});
    int iHr = idxOf({"hr", "hrno"});
    int iBayer = idxOf({"bayer", "bf", "greek"});
    int iFlam = idxOf({"flam", "flamsteed"});
    int iCi = idxOf({"ci", "bv", "bmv", "colorindex"});

    if (iRA < 0 || iDE < 0) { SetErr(err, "cannot locate RA/DEC columns in header"); return false; }

    auto fieldAt = [&](const std::vector<std::string>& v, int i) -> std::string {
        return (i >= 0 && i < (int)v.size()) ? Trim(v[i]) : "";
    };

    int parsed = 0;
    for (size_t li = dataStart; li < lines.size(); ++li) {
        const std::string& line = lines[li];
        if (line[0] == '#') continue;                 // 注释 / 页脚
        auto v = split(line);
        if (v.size() < 2) continue;

        double ra, dec, mag;
        if (vizier) {
            ra = ParseSexagesimal(fieldAt(v, iRA), true, -1);
            dec = ParseSexagesimal(fieldAt(v, iDE), false, -999);
        } else {
            ra = ToDouble(fieldAt(v, iRA), -1);
            dec = ToDouble(fieldAt(v, iDE), -999);
            if (ra > 24.0001) ra /= 15.0;            // 度数 -> 小时
            // 若 RA 是 "HH MM SS"，ToDouble 会失败 -> 兜底
            if (ra < 0 && fieldAt(v, iRA).find(' ') != std::string::npos)
                ra = ParseSexagesimal(fieldAt(v, iRA), true, -1);
        }
        mag = (iMag >= 0) ? ToDouble(fieldAt(v, iMag), 99.0) : 6.0;
        // 只保留肉眼/双筒可见范围的恒星，避免把 12 万颗暗星全塞进内存。
        // 天狼星 -1.46 是最亮恒星；mag < -2 基本可判定为异常数据（如 HYG 首行的太阳）。
        // 如需更暗的星，调大 9.0 并同步调大 main.cpp 中 ReloadCatalog 的默认上限。
        if (ra < 0 || dec < -90 || dec > 90 || mag < -2.0 || mag > 9.0) continue;
        if (!std::isfinite(ra) || !std::isfinite(dec)) continue;

        Star s;
        s.ra = ra - std::floor(ra / 24.0) * 24.0;
        s.dec = dec; s.mag = mag;
        s.spectral = fieldAt(v, iSp);
        s.con = fieldAt(v, iCon);
        s.hip = ToInt(fieldAt(v, iHip), 0);

        std::string bayer = fieldAt(v, iBayer), flam = fieldAt(v, iFlam);
        std::string designation;
        if (!flam.empty()) designation = flam + " " + s.con;
        else if (!bayer.empty()) designation = bayer + " " + s.con;
        if (designation.empty() && iHr >= 0 && !fieldAt(v, iHr).empty())
            designation = "HR " + fieldAt(v, iHr);
        if (designation.empty() && s.hip) designation = "HIP " + std::to_string(s.hip);

        std::string proper = fieldAt(v, iName);
        s.name = Utf8ToWide(proper.empty() ? designation : proper);
        s.nameEn = Utf8ToWide(designation.empty() ? proper : designation);
        if (s.name.empty()) continue;

        if (s.spectral.empty() && iCi >= 0) {
            // 无光谱型时由色指数粗估
            double ci = ToDouble(fieldAt(v, iCi), 0.6);
            s.spectral = ci < 0.0 ? "B" : ci < 0.3 ? "A" : ci < 0.55 ? "F" :
                         ci < 0.8 ? "G" : ci < 1.2 ? "K" : "M";
        }
        out.push_back(s);
        ++parsed;
    }
    SetErr(err, "");
    return parsed > 0;
}

bool StarCatalog::ImportText(const std::string& text, const std::string& sourceTag,
                             int& inserted, std::wstring* err) {
    inserted = 0;
    std::vector<Star> rows;
    std::wstring fmt;
    if (!ParseCatalogText(text, rows, fmt, err)) return false;

    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) { SetErr(err, "database not open"); return false; }
    char* e = nullptr;
    sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &e); sqlite3_free(e);

    sqlite3_stmt* del = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM stars WHERE source=?", -1, &del, nullptr);
    sqlite3_bind_text(del, 1, sourceTag.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(del); sqlite3_finalize(del);

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
        "INSERT INTO stars(name,name_en,ra,dec,mag,spectral,con,hip,source,info) VALUES(?,?,?,?,?,?,?,?,?,?)",
        -1, &st, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        SetErrSql(err, "prepare import", db_); return false;
    }
    for (auto& s : rows) {
        s.source = sourceTag;
        if (InsertStar(s, st)) ++inserted;
    }
    sqlite3_finalize(st);
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    if (err) {
        wchar_t buf[256];
        swprintf_s(buf, L"导入 %d 颗（格式：%ls）", inserted, fmt.c_str());
        *err = buf;
    }
    return inserted > 0;
}

bool StarCatalog::ImportFile(const std::wstring& path, int& inserted, std::wstring* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { SetErr(err, "cannot open file"); return false; }
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty()) { SetErr(err, "file is empty"); return false; }
    std::string tag = "file:" + WideToUtf8(path);
    return ImportText(data, tag, inserted, err);
}

} // namespace tq
