#pragma once
// 在线星表抓取（WinHTTP）+ 导入数据库
#include <string>
#include <vector>
#include "star_catalog.h"

namespace tq {

struct CatalogSource {
    std::wstring name;
    std::string  url;
    std::string  tag;       // 写入 stars.source，重复抓取时按此去重
};

struct DownloadResult {
    bool        ok = false;
    int         inserted = 0;
    std::string url;
    std::wstring message;   // 面向用户的中文结果说明
};

// 默认候选源（依次尝试，直到成功）
const std::vector<CatalogSource>& DefaultSources();

// 从同目录 sources.txt 读取自定义源；格式：名称|URL|tag（每行一条，# 开头为注释）
std::vector<CatalogSource> LoadSources(const std::wstring& dir);

// 纯 HTTP(S) GET，自动跟随重定向、支持系统代理、可处理大响应
bool HttpGet(const std::string& url, std::string& body, std::wstring* err = nullptr);

// 依次尝试各源，成功后直接写入数据库（可在工作线程中调用）
bool FetchAndImport(StarCatalog& catalog, const std::vector<CatalogSource>& sources,
                    DownloadResult& result);

} // namespace tq
