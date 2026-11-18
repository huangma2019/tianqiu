"""
把 ziliao/88星座边界数据 下的三张表导入 tq_catalog.db，供程序做星座悬停 / 点击查星座用。

输入：
  88星座传说与来源.csv    中文名 / 拉丁名 / 所有格 / 缩写 / 含义 / 文化根源 / 神话传说 / 命名历史 / 家族
  88星座名称对照表.csv    每个星座的名称标注点（画星座名用）
  星座边界多边形_J2000.csv 88 个星座的边界多边形顶点（J2000 赤经赤纬）

输出表：
  constellation_info    (abbr PK, name_cn, name_lat, genitive, meaning, origin,
                         myth, history, family, label_ra, label_dec)
                        label_ra 为小时，label_dec 为度
  constellation_boundary(abbr, ring, seq, ra_h, dec_d, PK(abbr,ring,seq))
                        按 seq 顺序连成闭合多边形；Ser 有两块（ring 0/1），其余只有 ring 0

用法： python tools_import_const_info.py
"""

import collections
import csv
import io
import os
import sqlite3

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'ziliao', '88星座边界数据')
DB = os.path.join(HERE, 'tq_catalog.db')


def read_csv(name):
    path = os.path.join(SRC, name)
    with io.open(path, 'r', encoding='utf-8-sig', newline='') as f:
        return list(csv.DictReader(f))


def main():
    myth = read_csv('88星座传说与来源.csv')
    names = read_csv('88星座名称对照表.csv')
    poly = read_csv('星座边界多边形_J2000.csv')

    label = {}
    for r in names:
        abbr = r['IAU缩写'].strip()
        label[abbr] = (float(r['标注点_赤经h_J2000']), float(r['标注点_赤纬deg_J2000']))

    info_rows = []
    for r in myth:
        abbr = r['缩写'].strip()
        ra, dec = label.get(abbr, (0.0, 0.0))
        info_rows.append((
            abbr,
            r['中文名'].strip(),
            r['拉丁名'].strip(),
            r['所有格'].strip(),
            r['含义/象征'].strip(),
            r['文化根源'].strip(),
            r['神话传说'].strip(),
            r['命名历史与出处'].strip(),
            r['星座家族'].strip(),
            ra, dec,
        ))

    # 顶点按 区域 -> 序号 排列；序号回绕说明开了新环（蛇夫座持蛇座 Ser 有两块），
    # 单独记 ring 号，程序端按环做点在多边形内判定与绘制。
    by_region = collections.defaultdict(list)
    for r in poly:
        by_region[r['IAU缩写'].strip()].append(
            (int(r['顶点序号']), float(r['赤经h_J2000']), float(r['赤纬deg_J2000'])))

    bnd_rows = []
    for abbr, vs in by_region.items():
        vs.sort(key=lambda t: t[0])
        ring, prev = 0, 0
        for idx, (seq, ra, dec) in enumerate(vs):
            if seq < prev:              # 序号回绕 = 新的一环
                ring += 1
            prev = seq
            bnd_rows.append((abbr, ring, idx, ra, dec))

    c = sqlite3.connect(DB)
    cur = c.cursor()
    cur.execute("""CREATE TABLE IF NOT EXISTS constellation_info(
        abbr TEXT PRIMARY KEY, name_cn TEXT, name_lat TEXT, genitive TEXT,
        meaning TEXT, origin TEXT, myth TEXT, history TEXT, family TEXT,
        label_ra REAL, label_dec REAL)""")
    cur.execute("DROP TABLE IF EXISTS constellation_boundary")
    cur.execute("""CREATE TABLE IF NOT EXISTS constellation_boundary(
        abbr TEXT, ring INTEGER, seq INTEGER, ra_h REAL, dec_d REAL,
        PRIMARY KEY(abbr, ring, seq))""")
    cur.execute("DELETE FROM constellation_info")
    cur.execute("DELETE FROM constellation_boundary")
    cur.executemany("INSERT INTO constellation_info VALUES(?,?,?,?,?,?,?,?,?,?,?)", info_rows)
    cur.executemany("INSERT INTO constellation_boundary VALUES(?,?,?,?,?)", bnd_rows)
    cur.execute("CREATE INDEX IF NOT EXISTS idx_bnd_abbr ON constellation_boundary(abbr)")
    c.commit()

    print('constellation_info   :', cur.execute("select count(*) from constellation_info").fetchone()[0])
    print('constellation_boundary:', cur.execute("select count(*) from constellation_boundary").fetchone()[0],
          '顶点，覆盖', cur.execute("select count(distinct abbr) from constellation_boundary").fetchone()[0], '个星座')
    c.close()


if __name__ == '__main__':
    main()
