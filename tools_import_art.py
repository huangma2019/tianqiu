# -*- coding: utf-8 -*-
"""
把 ziliao/constellation-db/constellation.db 里的星座贴图（填充版）
转换成紧凑的球面顶点数据，写入本程序自建的 tq_catalog.db。

轮廓版已废弃（程序只画填充版），这里不再导出 outline，旧库里的 outline 行
由导入时先清空 const_art 一并清掉。

导出表：
  const_art       (abbr, style, layer, tone, counts BLOB<int32>, pts BLOB<float32 ra,dec>)
                  style='fill'    4 个亮度层，同层多条多边形用 even-odd 合并填充
  const_art_info  (abbr, center_ra, center_dec, has_fill, has_outline)

用法： python tools_import_art.py
"""
import os
import sqlite3
import struct
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE, "ziliao", "constellation-db", "constellation.db")
DST = os.path.join(BASE, "tq_catalog.db")


def pack_i32(vals):
    return struct.pack("<%di" % len(vals), *vals)


def pack_f32(vals):
    return struct.pack("<%df" % len(vals), *vals)


def main():
    if not os.path.exists(SRC) or not os.path.exists(DST):
        print("缺少源库或目标库:", SRC, DST)
        return 1

    src = sqlite3.connect(SRC)
    sc = src.cursor()
    dst = sqlite3.connect(DST)
    dc = dst.cursor()

    dc.execute("DROP TABLE IF EXISTS const_art")
    dc.execute("DROP TABLE IF EXISTS const_art_info")
    dc.execute("""CREATE TABLE const_art (
        abbr   TEXT NOT NULL,
        style  TEXT NOT NULL,
        layer  INTEGER NOT NULL,
        tone   INTEGER NOT NULL,
        counts BLOB NOT NULL,
        pts    BLOB NOT NULL,
        PRIMARY KEY (abbr, style, layer)
    ) WITHOUT ROWID""")
    dc.execute("""CREATE TABLE const_art_info (
        abbr        TEXT PRIMARY KEY,
        center_ra   REAL,
        center_dec  REAL,
        has_fill    INTEGER,
        has_outline INTEGER
    ) WITHOUT ROWID""")

    abbrs = [r[0] for r in sc.execute(
        "SELECT abbr FROM constellations WHERE has_image=1 ORDER BY abbr")]
    art_rows = []
    info_rows = []
    total_pts = 0

    for abbr in abbrs:
        # ---------------- 填充版 ----------------
        levels = dict(sc.execute(
            "SELECT band, level FROM bands WHERE constellation=?", (abbr,)).fetchall())
        paths = sc.execute(
            "SELECT band, seq, n_points FROM paths WHERE constellation=? ORDER BY band, seq",
            (abbr,)).fetchall()
        if paths:
            # 逐 (band, seq) 取顶点
            verts = {}
            for band, seq, idx, ra, dec in sc.execute(
                    "SELECT band, seq, idx, ra_deg, dec_deg FROM vertices "
                    "WHERE constellation=? ORDER BY band, seq, idx", (abbr,)):
                if ra is None or dec is None:
                    continue
                verts.setdefault((band, seq), []).extend((ra, dec))

            bands = {}
            for band, seq, n_points in paths:
                v = verts.get((band, seq))
                if not v:
                    continue
                cnt = len(v) // 2
                b = bands.setdefault(band, [[], []])
                b[0].append(cnt)          # 闭合多边形
                b[1].extend(v)

            for band in sorted(bands):
                counts, pts = bands[band]
                tone = int(levels.get(band, 60))
                art_rows.append((abbr, "fill", band, tone, pack_i32(counts), pack_f32(pts)))
                total_pts += len(pts) // 2
            has_fill = 1
        else:
            has_fill = 0

        # 轮廓版已废弃，不再导出
        has_outline = 0

        tr = sc.execute("SELECT center_ra, center_dec FROM transforms WHERE constellation=?",
                        (abbr,)).fetchone()
        cra, cdec = tr if tr else (None, None)
        info_rows.append((abbr, cra, cdec, has_fill, has_outline))

    dc.executemany(
        "INSERT INTO const_art(abbr,style,layer,tone,counts,pts) VALUES(?,?,?,?,?,?)", art_rows)
    dc.executemany(
        "INSERT INTO const_art_info(abbr,center_ra,center_dec,has_fill,has_outline) "
        "VALUES(?,?,?,?,?)", info_rows)
    dst.commit()

    print("const_art 行数 :", len(art_rows))
    print("  其中 fill   :", sum(1 for r in art_rows if r[1] == "fill"))

    print("const_art_info :", len(info_rows))
    print("顶点总数       :", total_pts)
    print("目标库大小     : %.1f MB" % (os.path.getsize(DST) / 1048576.0))
    src.close()
    dst.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
