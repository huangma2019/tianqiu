#pragma once
#include <string>
#include <vector>

// 全国行政区划市县经纬度库（SQLite：tq_cities.db）。
// 数据由 "中国行政区划经纬度_扁平版 副本.csv" 导入，取质心经纬度，
// py 列为名称的拼音首字母（如 北京市 -> bjs），供下拉按首字母检索。
namespace citydb {

struct CityRecord {
    std::wstring name;    // 市县汉字名称
    std::wstring parent;  // 上级行政区名称（用于区分重名）
    std::string  py;      // 名称拼音首字母，全小写
    double lat = 0, lon = 0;
};

// 打开/创建 exe 同目录下的 tq_cities.db，并建立 cities 表。重复调用安全。
bool Open();

// 记录总数。
int  Count();

// 一次读入全部市县（按名称排序），用于拼音首字母过滤下拉。返回条数。
int  LoadAll(std::vector<CityRecord>& out);

// 按名称精确查询经纬度。
bool Lookup(const std::wstring& name, double& lat, double& lon);

} // namespace citydb
