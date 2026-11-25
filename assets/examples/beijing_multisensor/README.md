# 北京市 · 光学与热红外多传感器观测模拟

分类：区域遥感影像。层级：项目内置精选。

## 用途

同一个北京 300 m 场景（2019-08-15 02:55 UTC = 北京时间 10:55），先用 ERA5 能量平衡（EB）求一次四组分温度（光照/阴影土壤、光照/阴影叶片），再让多颗卫星的波段设置复用这组温度成像，比较各传感器的光学反射率与热红外亮温，并做两个专题实验：

- **观测角实验**：Landsat TIRS B10 与 MODIS 31，VZA 0/20/32/40/55°，主平面前向/后向，观察热点；
- **聚合实验**：300 m → 1 km，比较均值、标准差、最大值与热点面积占比。

本案例在同一个北京场景上完成 7 个传感器的光学 + 热红外一体化模拟，流程可一键复现，覆盖光谱、角度、尺度三类实验。输出是无大气的冠层顶反射率与亮温，传感器之间的数值差异反映波段设置的影响，不是各传感器实际可观测到的差异；结果尚未与实测数据对比（列入后续工作）。完整分析见报告 [docs/cases/beijing-multisensor/report.pdf](../../../docs/cases/beijing-multisensor/report.pdf)（14 页）。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `project.json` | 母工程：ERA5 能量平衡，输出 `components.tif`（与 landsat_beijing_eb 等价） |
| `variants/<传感器>.json` | 7 个传感器工程：landsat89、modis、sentinel2、aster、ecostress、viirs、fy3d_mersi2；RT 模式，组分温度默认读 `../reference/components.tif`，只含该传感器的波段 |
| `variants/angles_*.json` | 16 个观测角工程：`angles_{tirs_b10,modis_b31}_{vza00 / vza20,40,55_{back,fwd} / vza32_back}` |
| `sensors.json` | 传感器与波段表（中心波长、半高宽、官方范围、数据依据 `basis`、出处） |
| `run_all.mjs` | 一键运行脚本（母工程 → 各传感器 → 角度 → 汇总 → 聚合） |
| `raster_tools.py` | 统计、聚合与参考结果导出（依赖 numpy + osgeo；GDAL ≥ 3.11 才写 float16，旧版退回 uint16×10） |
| `inputs/`、`era5/` | 复制自 `assets/examples/landsat_beijing_eb/`（地类、LAI、DEM、ERA5 气象包），使本案例自包含 |
| `results/` | `summary.csv`（各传感器各波段统计）、`angles.csv`、`aggregation.csv` |
| `reference/` | 精简参考结果（约 14 MB）：`components.tif`、各传感器 `<id>_optical.tif`（float16）与 `<id>_tir_bt.tif`、三份 csv、`parent_scene_summary.json`、`manifest.json` |

栅格：EPSG:32650，300 m，592 × 598；光学有效像元 180408，热红外 182182（含水体 1774）。

## 传感器与波段

中心波长/带宽：光学为 nm，热红外为 μm。STREAM 以 center/fwhm 的**高斯近似**光谱响应函数成像（官方光谱响应函数作为后续工作接入）；官方给范围 [lo, hi] 的取中点与宽度。

| 传感器 | 光学波段 | 热红外波段 | 数据依据 | 出处 |
| --- | --- | --- | --- | --- |
| Landsat 8/9 OLI + TIRS | B1–B7 | B10 10.895、B11 12.005 | 官方规格 | NASA Landsat 带通表；USGS 波段页 |
| MODIS (Terra) | 1–7 | 31 (10.78–11.28)、32 (11.77–12.27) | 官方规格 | MODIS 规格页；NASA MCST 波段表（B32） |
| Sentinel-2A MSI | B2–B8、B8A、B11、B12 | — | 官方规格 | ESA SNAP 文档表 |
| ASTER | — | B10–B14 (8.125–11.65) | 官方规格 | JPL ASTER TIR 波段范围（B11 8.475–8.825） |
| ECOSTRESS | — | TIR1–5（8.29/8.78/9.20/10.49/12.09，FWHM 0.354/0.310/0.396/0.410/0.611） | 文献实测值 | 公开文献给出的在轨实测中心与 FWHM |
| VIIRS | M3、M4、M5、M7、M10、M11 | M15 10.763、M16 12.016 | 官方规格 | NOAA STAR 页 |
| FY-3D MERSI-II | CH1–CH4、CH6、CH7（470/550/650/865/1640/2130 nm，带宽均 50 nm） | CH24 10.8、CH25 12.0（带宽 1000 nm） | 官方规格 | 国家卫星气象中心（NSMC）MERSI-II 通道表 |

“数据依据”记录在 `sensors.json` 每个波段的 `basis` 字段：`official-spec` 官方规格、`official-srf` 官方光谱响应、`literature` 文献实测值、`nominal` 标称值。每个波段的出处链接与备注见 `sensors.json`。

## 运行

**界面**：StreamStudio 的“打开示例”中选择“北京市 · 光学与热红外多传感器观测模拟”，指定保存目录，程序复制案例后打开。母工程 `project.json` 用于 EB 求解；`variants/` 里的工程可在遥感影像模拟工作台逐个打开运行（组分温度默认读 `reference/components.tif`，不先跑母工程也能成像）。

**命令行**（仓库根目录，需要 Python 的 numpy 与 osgeo）：

```text
node assets\examples\beijing_multisensor\run_all.mjs --out <输出目录> [--write-reference] [--reuse-parent] [--only id,...]
```

共 24 次引擎运行（母工程 1 + 传感器 7 + 角度 16），引擎总耗时约 13 s，整条流程约 14 s。`--write-reference` 刷新 `reference/` 与 `results/`；`--reuse-parent` 复用已有母工程输出；`--only` 只跑指定工程。输出目录里每个工程一个子目录（`optical_reflectance.tif`、`tir_brightness_temperature.tif`、`run-config.json`、`stream.log`），`multisensor-manifest.json` 为运行清单。

**报告与图**（只用仓库内 `reference/` 即可重建）：

```text
python tools\build-beijing-multisensor-report.py [--source reference|run] [--run-dir <输出目录>]
```

## 预期结果（关键数字，数值来自 `results/*.csv`）

- 母工程：180403/180408 个 EB 像元收敛，5 个未收敛，144 个用了 1/L 括号法回退；太阳天顶角 31.85°、方位角 138.82°。
- 光学 NDVI：Landsat 0.746、MODIS 0.734、Sentinel-2 0.752、VIIRS 0.764、MERSI-II 0.737；差异来自红光/近红外波段的位置与宽度。母工程（EB 路径）与 Landsat 变体（RT 路径）的反射率差最大 0.0034（B7），NDVI 差 +0.002。
- 热红外亮温（K，场景均值）：Landsat B10/B11 303.796/303.787；ASTER 由 B10 303.829 单调降到 B14 303.793；同波段不同传感器差 <0.01 K。
- 观测角（B10 相对天底 303.796 K）：后向 VZA 20/32（热点）/40/55° 为 +0.47/+1.78/+0.47/-0.43 K，前向 20/40/55° 为 -0.20/-0.42/-0.66 K。
- 聚合（B10，300 m → 1 km 均值）：均值 −0.006 K，标准差 2.40 → 2.21 K，最大值 316.74 → 314.90 K，热点占比 0.050 → 0.034；B5 为 0.014，NDVI 为 0.011。

## 数据来源与出处

输入数据与 landsat_beijing_eb 相同：地类由 2019 GLC_FCS30D 映射到 IGBP，DEM 为 FABDEM，LAI 为 Landsat 30 m 产品，300 m 输入由 30 m 网格聚合；ERA5 气象日期为 2019-08-15（约 25 km）。传感器波段出处见上表与 `sensors.json`。软件许可证不代替输入数据各自的再分发条款。

## 适用范围与后续工作

**适用范围**

- 输出是无大气的冠层顶反射率与亮温，传感器之间的数值差异反映波段设置（中心波长与带宽）的影响，不是各传感器实际可观测到的差异；
- 结果尚未与实测数据（Landsat 8 B10、MOD11 产品等）对比；
- 各波段采用高斯近似光谱响应，数据依据见上表；
- STREAM 是单层冠层模型，适合区域尺度，角度实验反映冠层内部的光照/阴影分配；
- ERA5 约 25 km 强迫，300 m 内气象近似常数；
- 角度实验只做热红外；聚合用 GDAL 平均/取最大，1 km 网格从原栅格西北角起算，不是 MODIS 正弦网格；
- 引擎日志的“未知地类 171834”是市域外 255 填充像元的统计项（592 × 598 − 182182）。

**后续工作**

- 城市三维结构效应：用 HiStream 的面元/体元模式补充；
- 与 Landsat 8 B10 实测、MOD11 产品对比；
- 接入大气校正（MODTRAN 查找表），给出各传感器入瞳辐亮度；
- 用各传感器官方光谱响应函数替代高斯近似，并补充光学 BRDF 角度实验。

## 参考结果说明

`reference/` 是 run_all.mjs 的一次完整运行的精简拷贝（光学为 float16，亮温为 float32），数值与 `results/` 一致，仅耗时列略有不同；它是历史执行结果，不是打开案例后新生成的结果。更换引擎版本后应重新运行 `run_all.mjs --write-reference` 刷新。

## 报告

- 报告 PDF：[docs/cases/beijing-multisensor/report.pdf](../../../docs/cases/beijing-multisensor/report.pdf)；
- 配图：[docs/cases/beijing-multisensor/figures/](../../../docs/cases/beijing-multisensor/figures/)；亮温与 LST 差表：[tables/tir_bt_minus_lst.csv](../../../docs/cases/beijing-multisensor/tables/tir_bt_minus_lst.csv)；分地类统计：[tables/class_analysis.json](../../../docs/cases/beijing-multisensor/tables/class_analysis.json)；
- 生成脚本：`tools/build-beijing-multisensor-report.py`（报告）、`tools/plot-beijing-multisensor.py`（配图）。
