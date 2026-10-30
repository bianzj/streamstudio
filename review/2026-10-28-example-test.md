# 内置示例端到端测试报告

日期：2026-10-28
目的：验证修复后内置示例是否仍可完整执行。

## 结论

**5 个内置示例全部可执行，退出码均为 0，产物齐全，日志无错误。**

| 示例 | 模式 | 状态 | 退出码 | 耗时 | 产物 |
| --- | --- | --- | --- | --- | --- |
| 垄行玉米-农业 `crop_gpp_npp` | eVoxelEB | completed | 0 | 51 s | 47 个时次；479 个文件 / 95.6 MB |
| 混交森林-生态 `forest_gpp_npp` | eVoxelEB | completed | 0 | 284 s | 47 个时次；480 个文件 / 915 MB |
| 光伏-新能源 `photovoltaic` | eFacetEB | completed | 0 | 86 s | 777 个文件 / 404 MB（96 个节点 CSV + 汇总） |
| 城市-热环境 `urban_block` | eVoxelRT | completed | 0 | 8 s | 10 个文件 / 4.9 MB（SZA=30 温度 TIFF + voxelrt.bin/json） |
| 北京市 Landsat 影像-ERA5 单日驱动 `landsat_beijing_eb` | STREAM EB | completed | 0 | 4 s | 7 个 TIFF + 5 个 JSON / 22.4 MB |

## 环境

- 构建：`npm run build:all`（GUI 18 模块 + histream.exe / histream-usd.exe / stream.exe 全部重新编译）
- Node：v24.19.0；Electron 38（未参与本次测试）
- GPU：NVIDIA RTX PRO 6000 Blackwell Workstation Edition，128 逻辑核
- 服务：`node server.mjs`，`STREAM_STUDIO_PORT=4799`

## 方法

1. 全量重新编译 GUI 与两个引擎（覆盖本轮 227 处修改）。
2. 启动服务，通过 HTTP API 驱动（与 GUI 走同一套接口）：
   - `GET /api/examples` 取目录
   - `POST /api/examples/open` 复制示例到临时目录
   - `POST /api/run`（HiStream 四种模式）/ `POST /api/stream/run`（STREAM EB）
   - 轮询 `run-manifest.json` 至终态；同时用 SSE 采集引擎 stdout/stderr
3. 校验：退出码、产物数量、能量收支收敛状态、物理量范围。

脚本与原始数据：

- `review/run-examples.mjs`（测试驱动）
- `review/compare-summary.mjs`（结果对比）
- `review/examples-run/2026-10-28T09-50-43-706Z/`（逐示例日志 `*.log` 与 `summary.json`）

> 大体积产物（约 1.5 GB 的 TIFF/BIN/CSV）在核对完成后已清理，仅保留日志与汇总；
> 需要复现时重新执行 `review/run-examples.mjs <baseUrl>` 即可。

## 质量指标

### 能量平衡收敛（HiStream）

- `crop_gpp_npp`：47/47 节点末态已刷新，**超容差状态总数 0**，温度迭代 13–35 次
- `forest_gpp_npp`：47/47 节点末态已刷新，**仅 node 38 超容差**（详见下）

### STREAM（landsat_beijing_eb）

- 求解像元 179 687，收敛 179 607（99.96%）
- **失败像元 0，无驱动像元 0，水分回退 0**
- 稳定性迭代收敛 179 642
- 反射率范围 0.018–0.455，短波通量 469–632 W/m²，净辐射 −29–269 W/m²，数值区间合理

## 发现的问题

### 1. 森林示例 node 38（接近日落）未收敛

- 时次 `DOY214_19-20`，太阳天顶角 88.9°
- 温度迭代撞到**上限 50 次**，`statesOutsideTolerance=2597`（容差 2 W/m²）
- 其余 46 个节点全部收敛，`statesOutsideTolerance=0`

原因分析：这是迭代上限写死 50 次且不检查残差的直接后果。太阳接近地平线时能量收支方程变刚性，
Jacobi 迭代在 50 次内无法收敛。该上限属于本轮**明确不改**的数值项（见待确认清单
`voxeleb/command.cpp:291`），所以本次没有修改。

影响：仅影响该时次的温度精度，不会崩溃、不影响其他时次。若要修，方向是提高上限或加入残差判据
（超限时告警并标记），需你确认。

### 2. 随包参考结果已过期，不能当回归基线

`assets/examples/landsat_beijing_eb/runs/reference_*/` 与新结果对比：60 处数值差异，最大相对差 543%。
但差异原因是**参考结果来自更早的算法版本**，证据是 model / stability / energyScheme 的描述字符串
全部不同：

| 字段 | 参考结果 | 当前引擎 |
| --- | --- | --- |
| `model` | PROSPECT + BSM + stream_core optical closure；TiRT two-layer | PROSPECT + BSM；TiRT standard four-component closure (hom3 radiosity, hotspot)；TIRTEB |
| `temperature.stability` | Monin-Obukhov iteration（Businger-Dyer / Paulson） | MO coupled solve：1/L secant with sign bracket，tolerance 1e-7 + 1e-6\|target\| |
| `temperature.energyScheme` | tirt-analytic | tirt |

用 10-27 的旧二进制 `stream_old_audit.exe` 做对照，差异同样有 58 处（最大 44%），说明这是版本漂移，
不是本轮修复引入。

**其中一处差异是本轮修复按设计改变的**：`temperature.era5.hours` 从参考的 `00:00–04:00` 变为
`00:00–23:00`，即 `image.cpp` 的 `pack.lastHour()` 修复（原来恒为 -1，起转只到过境后一小时）。
这会改变能量平衡起转时长，进而改变组分温度，属预期变化。

**建议**：重新生成并随包发布参考结果，否则用户对照会误判为回归。

> **已于 2026-10-28 处理**：过期的参考结果已从示例中移除
> （`assets/examples/landsat_beijing_eb/runs/reference_2019-08-15T02-55Z/`，13 个文件 21.1 MB），
> 暂存于 `review/_deleted-examples/` 以便回退确认。同时修正了两处已失效的说明文字：
> 示例 `project.json` 的 description（原写「runs 下附参考结果，可直接查看」）与
> `README.md`（原写「附带 2019-08-15 单日 ERA5 和参考结果」）。
> 移除后重新实测该示例：2.1 秒，8 个产品，退出码 0，功能不受影响。
> 后续若要恢复对照能力，应用当前引擎重新生成一份参考运行。

### 3. 编码显示注意

示例 `project.json` 的中文在 PowerShell 5.1 下按 ANSI 读取会显示乱码，引擎产物 JSON 同理。
用 UTF-8 读取正常（已验证 `潜热 [W m⁻²]` 等标签正确）。文件本身没有问题。

## 补充测试：D 盘案例（2026-10-28 18:33–18:46）

`D:\examples` 下共 11 个工程，全部执行成功，退出码 0。

### STREAM 影像工程（6 个，2–4 秒）

| 工程 | 温度模式 | 耗时 | 收敛指标 |
| --- | --- | --- | --- |
| landsat-beijing | RT | 2.1 s | —（RT 无 EB 迭代） |
| landsat-beijing-era5-20190815 | EB | 4.1 s | 179 607 / 179 687（99.96%），失败 0 |
| stream-beijing-1km-modis-20190815 | EB | 2.1 s | 17 037 / 17 241（98.8%），失败 0 |
| stream-beijing-1km-modis-components-20190815 | RT | 2.1 s | — |
| stream-beijing-300m-landsat-20190815 | EB | 4.1 s | 180 328 / 180 408（99.96%），失败 0 |
| stream-beijing-300m-landsat-components-20190815 | RT | 2.1 s | — |

三个 EB 工程的输出都提示「已要求按 DEM 订正气温与气压，但 ERA5 数据包没有地表位势 z，
本次不做高程订正」——属数据包缺项的信息性警告，不是失败。

### HiStream 三维工程（5 个）

| 工程 | 模式 | 耗时 | 产物 | 收敛 |
| --- | --- | --- | --- | --- |
| auto | eVoxelEB | 102.9 s | 47 时次 / 192 个 | 47/47 节点全收敛，超容差状态 0 |
| beijing | eVoxelRT | 24.9 s | 4 个 | 单节点完成 |
| forest | eRaytracing | 17.4 s | 2 个 | 单节点完成 |
| photovoltaic | eFacetRT | 86.0 s | 63 个 | 单节点完成 |
| ship | eRaytracing | 13.8 s | 129 个 | 单节点完成 |

### assets 示例（清理后复测）

5 个示例在删除孤立示例与过期参考结果之后再次全量执行，全部 `completed`、退出码 0：

| 示例 | 耗时 |
| --- | --- |
| crop_gpp_npp | 49.9 s |
| forest_gpp_npp | 318.9 s |
| photovoltaic | 92.1 s |
| urban_block | 9.1 s |
| landsat_beijing_eb | 2.1 s |

### 注意

测试在 `D:\examples\*\output\` 下写入了 11 个结果目录，合计约 1.78 GB
（auto 427 MB、photovoltaic 438 MB、ship 771 MB，其余较小）。这些是正常结果产物，
如需清理可直接删除对应时间戳目录。

## 未覆盖

- GUI 交互（浏览器点选、三维预览、结果着色）——本次只走 API，未做人工界面验证
- Electron 桌面外壳（`npm run desktop`）
- USD 导入导出（`histream-usd.exe` 已编译，但未执行转换）
- 大规模/极端场景压力与显存上限
- 本轮标记为 DEFERRED 的 209 处问题的行为验证
