# TQ · 自建 C++ 天球仪

一个从零写的 Win32 自定义控件：GDI+ 双缓冲绘制天球，SQLite 保存天体数据，
WinHTTP 抓取公开星表。不依赖 Qt / MFC / WebView2 / OpenGL，只用 Windows 自带库。

## 编译与运行

需要 VS2022（含「使用 C++ 的桌面开发」工作负载）。双击 `build.bat` 即可，
脚本用 `vswhere` 自动定位 VS，产出 `TQ.exe`。

```
build.bat
TQ.exe
```

`sqlite3.c` / `sqlite3.h` 是 SQLite 官方 amalgamation（从同机 `ETF` 工程复用），
已随工程附带，无需额外安装。链接库：`user32 / gdi32 / gdiplus / comctl32 /
winhttp / ole32 / shell32 / comdlg32`。

`.csv.gz` 由工程自带的 `tq_gzip.h` 解压（零依赖 DEFLATE 解码 + CRC32/ISIZE 完整性
校验），不需要 zlib —— Windows Compression API 并不支持标准 GZIP。

## 功能

- **天球**：单位球 + 透视投影（相机距球心 3 倍球半径），背面自动剔除，
  赤道网格、天赤道、黄道、银河、IAU 星座边界（浅淡蓝，按赤道坐标插值）、
  星座连线（沿球面大圆插值）、深空天体标记。
- **天体**：按视星等决定点的大小/亮度、按光谱型决定颜色，一等以上亮星带光晕与星芒；
  名称标注带碰撞避让；悬停与选中高亮。
- **月亮与行星**：月亮用 Meeus 月球理论（ELP-2000 截断）推算相位、照度与距离；
  水星/金星/火星/木星/土星/天王星/海王星用 JPL 开普勒根数（含一次光行时修正）推算地心赤经赤纬，
  视星等按相位角经验式计算，位置随时间框与自转实时更新，可单击选中查看数据。
- **交互**：左键拖动旋转（虚拟轨迹球，3×3 旋转矩阵累积，无万向锁）、
  右键拖动平移、滚轮以鼠标位置为锚点缩放、单击选中、双击把该处天区平滑转到中心、
  右键上下文菜单；本地时间框输入 `HH:MM` 即时更新天球，输入 `21` 这类简写后按回车同样生效。
- **搜索**：输入中文名 / 英文名 / 拼音首字母定位，重名时弹出候选列表（星名 / 城市 / 方向三个候选框均为顶层弹出窗口，高度 160，激活时置于最前且不抢焦点，不被绘图区遮挡，选择后可直接继续输入）；
  在星图上单击或双击选中恒星时，输入框同步显示该星名称。
- **数据**：SQLite 持久化，内置精选星表离线可用，支持在线抓取与本地 CSV 导入。

## 快捷键

| 键 | 作用 | 键 | 作用 |
|---|---|---|---|
| 方向键 | 旋转 | `+` / `-` | 缩放 |
| `R` | 复位视图 | `G` | 赤道网格 |
| `Q` | 天赤道 | `E` | 黄道 |
| `B` | 银河 | `C` | 星座连线 |
| `J` | 星座边界 | `N` | 星座名称 |
| `L` | 亮星标注 | `D` | 深空天体 |
| `P` | 各大行星 | | |

## 观测方向

时间框右侧的「方向」框可选 8 个方位（支持汉字或拼音首字母，回车 / 点击候选选中），
默认「南」。方位角 `az` 自正北起向东量，仰角固定 35°：

| 北 | 东北 | 东 | 东南 | 南 | 西南 | 西 | 西北 |
|---|---|---|---|---|---|---|---|
| 0° | 45° | 90° | 135° | 180° | 225° | 270° | 315° |

「南 = 180°」与 `HorizonToEquatorial(az, 35, ...)` 原先硬编码的 180.0 一致；
复位会把方向恢复为「南」。

选定方向后仍可用左键拖动视线（含自转中）：拖动结束会把视线方向反算回方位角，
「方向」框自动回填最接近的八方位，自转则从新视线继续绕极轴旋转。

## 球心视角（视点进入天球内部）

工具栏提供「球心视角 / 外部视角」两个互斥按钮（当前视角高亮锁定），点按即直接切换；
程序启动、复位按钮与 `R` 键都进入**球心视角**；复位还会把观测地恢复为「北京市」、
本地时间恢复为 `20:00`，并按该时刻的正南 35° 重新定向。滚轮缩放只在当前视角内变化
（球外 0.25–14x，球心 1–400x），到达上下限后停住，**不会**自动跨越球面切换视角。
也可用「视图 → 球心视角 / 球外视角」菜单切换，或向控件发 `TQSM_SETINSIDE`
（`wParam = 1` 进入 / `0` 退出）。

球心视角下视线朝向进入前的方向，看到的是与球外视角**面对面**的那半边天球（对面天区，
例如球外中心 RA 6h，球心视角中心为 RA 18h）。投影为球心透视（gnomonic）：
屏幕铺满正前方那一块天区，不再显示球盘轮廓和刻度；继续放大就继续收窄视场。

- 球心视角缩放范围 0.25x（视场约 155°，近乎整个对面半球，便于拖动浏览）至 400x。
- 左键拖动 = 在天球内部朝鼠标方向转动视线（球内看的是对面内表面，
  旋转/平移输入已取反，手感为第一人称"转头"）；右键平移、双击居中、天体拾取、搜索定位、
  网格 / 天赤道 / 黄道 / 银河 / 星座连线 / 深空标记全部照常工作。
- 球外视角用透视投影并剔除切点以外的背面球冠；球心视角朝 `-z` 看，
  剔除 `p.z >= 0` 的身后半天球（对面天区），两者共用同一套 3×3 旋转矩阵。
  `SetViewRA` / `GetViewRA` 在内部视角下自动按对面方向换算，
  搜索定位、Goto 动画在两种视角下都指向正确的天区。
- HUD 和状态栏会标出 `球心视角` / `外部视角` 及当前放大倍数。
- `SphereView` / `SphereStats` / `SphereNotify` 增加了 `inside`、`innerZoom` 字段，
  外部宿主可读取或设置视角模式。

## 渲染性能

拖动卡顿的根因是 GDI+ **逐条绘图调用的固定开销**（原实现每帧约 9000 次
`DrawLine` / `FillEllipse`），以及两次全屏 `Graphics::DrawImage` 走 alpha 合成慢路径；
再加上每个 `WM_MOUSEMOVE` 都同步刷新主窗口状态栏和 15 个按钮。控件已做以下优化：

- 网格 / 天赤道 / 黄道 / 银河 / 星座连线 / 深空标记按画笔合并进 `GraphicsPath`，
  每类每帧只 `DrawPath` 一次；曲线采样放宽到 3°。
- 恒星写入独立 32bpp 像素图层（纯内存写、按球盘区域合成），不再逐颗调用 GDI+；
  主帧与背景层复用缓存，不每帧分配几 MB 位图。
- 球盘渐变 / 轮廓 / 刻度缓存为不随旋转变化的位图，逐行 `memcpy` 到主帧。
- 最终用 GDI `SetDIBitsToDevice` 直接把像素推到窗口 DC，绕开 `DrawImage` 的格式转换。
- 拖动时对主窗口刷新做 120ms 节流、松手补发；主窗口按 `ps.rcPaint` 只重绘脏区。
- 默认关闭高成本的抗锯齿描边（工具栏「平滑」可开启），标注数量上限收紧。

HUD 与状态栏实时显示 `帧 xx.x ms`。压力测试（8 万颗星入库、2548 颗可见、
1092×776 控件）稳态约 **12 ms / 帧（≈80 FPS）**；内置 152 颗星时更快。

## 数据源

程序启动即写入内置精选数据（**152 颗亮星含中文名、27 个深空天体**），
无网络也能完整使用。星座图案连线（89 个区域共 150 组折线）与 IAU 官方星座边界
（257 条分块线）由 `constellation_data.cpp` 内置，取自 `ziliao/88星座边界数据/`
的 J2000 CSV，不经过数据库。

从「数据 → 在线抓取公开星表」会在后台线程依次尝试 `sources.txt` 中的源，成功后解析入库：

| 源 | 内容 | 格式 |
|---|---|---|
| Codeberg · HYG v4.4 | 119,614 颗（HIP+Yale+Gliese 合并） | `.csv.gz`，自动解压 |
| VizieR · BSC5 (V/50) | 9,110 颗耶鲁亮星 | TSV，赤经赤纬为 `HH MM SS` |

`sources.txt` 格式为 `名称|URL|标签`，可自由增删。解析器自动识别 CSV / VizieR TSV /
gzip / 无表头四列文本，按列名匹配 `ra,dec,mag,proper,bayer,flam,con,spect,ci,hip,hr`
（RA 支持小时或度、十进制或六十进制）。

抓取在后台线程进行，依次尝试各源；十几 MB 的压缩星表若被 CDN 截断，gzip 尾部校验
会报错并自动重试一次，再失败则换下一个源。若网络确实不通，手动下载星表文件后用
「文件 → 导入本地星表 CSV / TSV…」即可，功能完全一致。

导入时只保留 `-2.0 <= 星等 <= 9.0` 的恒星以控制规模；需要更暗的星可修改
`star_catalog.cpp` 中 `ParseCatalogText` 的阈值，并同步调大 `main.cpp` 里
`ReloadCatalog` 的默认上限。

## 数据库

程序目录下自动生成 `tq_catalog.db`（WAL 模式）：

| 表 | 字段 |
|---|---|
| `stars` | `id, name, name_en, ra(小时), dec(度), mag, spectral, con, hip, source` |
| `deepsky` | `id, name, kind, ra, dec, mag` |
| `meta` | `k, v` |
| `const_art` | `abbr, style('fill'), layer, tone, counts(int32 点数/段), pts(float32 ra,dec 度)` |
| `const_art_info` | `abbr, center_ra, center_dec, has_fill, has_outline` |
| `constellation_info` | `abbr, name_cn, name_lat, genitive, meaning, origin, myth, history, family, label_ra, label_dec` |
| `constellation_boundary` | `abbr, ring, seq, ra_h, dec_d`（88 星座边界多边形，Ser 有两环） |

星座图案连线与 IAU 边界是程序内置常量，不入库；旧版库中的 `constellations`
表会在播种内置数据时清空。

## 星座贴图（填充版）

`const_art` 存的是 85 幅星座具象图的球面顶点（J2000 `ra/dec` 度），由
`tools_import_art.py` 从 `ziliao/constellation-db/constellation.db` 解算导入；
轮廓版已废弃，脚本不再导出，库中也已清理。

- 开关：右键菜单 / 「视图」菜单「星座贴图（填充版）」；工具栏「十二宫」只显示黄道十二宫。
- 每幅图 4 个灰度层（`tone` 越大越亮），同层多段按 `FillModeAlternate` 合并，
  重叠处自动挖洞；投影后逐点成面，任意投影/缩放下都不失真。
- 配色按星座下标循环取用淡蓝 / 浅紫 / 暗金等 8 种低饱和色调，重叠的相邻星座才分得清；
  同一星座内部只靠 `tone` 调不透明度。
- 相邻星座重叠或缩放很大时，只取离视场中心最近的 18 幅，并给整帧设 12 万顶点预算。

## 星座悬停与资料

贴图（填充版）打开后：

- 鼠标落在某个星座边界内 → 该星座边界加粗加亮（悬停淡紫 / 选中暗金），并在名称标注点画出星座名。
- 单击 → 右侧信息卡显示该星座资料（缩写/拉丁名/所有格/含义/家族/文化根源/传说/出处，
  长文本自动折行），**星座优先于天体**：点在恒星、月亮、行星上只要还在星座边界内，
  显示的都是星座资料。
- 判定用 `constellation_boundary` 的 J2000 多边形做点在多边形内测试；顶点赤经在加载时
  已跨 0h 展平为连续值，贴图/边界跨接缝也不会误判。
- 数据由 `tools_import_const_info.py` 从 `ziliao/88星座边界数据/*.csv` 导入。

`stars` 按 `mag` 建索引，内存中的星表按星等升序，绘制时提前 `break`，
并受 `SphereOptions::maxStars` 绘制预算约束。

## 在别的工程里复用控件

包含 `celestial_sphere.h/.cpp` + `star_catalog.*` + `tq_util.h`：

```cpp
tq::CelestialSphere::Register(hInst);
tq::CelestialSphere sphere;
sphere.Create(hwndParent, 0, 0, 800, 600, 1001, hInst);
sphere.SetData(catalogData);          // 也可 SendMessageW(h, TQSM_RELOAD, 0, &data)
sphere.Goto(5.6, -1.2);              // 转到猎户座
tq::SphereView v = sphere.GetView();
// 程序退出前：tq::CelestialSphere::Shutdown();
```

控件通过 `WM_NOTIFY` 向父窗口发 `TQN_HOVER / TQN_SELECT / TQN_VIEWCHANGED / TQN_DBLCLICK`，
`lParam` 为 `tq::SphereNotify*`。也可用 `TQSM_*` 消息控制（见头文件）。

## 文件

| 文件 | 作用 |
|---|---|
| `celestial_sphere.h/.cpp` | 控件：窗口类、旋转矩阵、透视投影、GDI+ 绘制、交互 |
| `star_catalog.h/.cpp` | SQLite 存储层 + CSV/TSV 解析导入 |
| `builtin_data.cpp` | 内置亮星 / 深空天体 / 星座中文名 |
| `constellation_data.cpp` | 内置 J2000 星座图案连线 + IAU 官方星座边界（由 `ziliao/88星座边界数据/` 生成） |
| `catalog_download.h/.cpp` | WinHTTP 抓取、重定向、gzip 解压、多源轮询 |
| `main.cpp` | 深色主题宿主窗口：工具栏、搜索、信息面板、菜单 |
| `tq_util.h` | 编码转换、CSV 分词、坐标格式化、光谱色 |
| `tq_gzip.h` | 零依赖 gzip/DEFLATE 解压器（含 CRC32 完整性校验） |
| `sources.txt` | 在线星表源配置 |
| `build.bat` | 一键编译 |

> 内置星表为演示用精选数据（J2000 近似坐标，HIP 编号仅供检索参考）；
> 精确完整的天体数据请通过在线抓取或本地 CSV 导入。
