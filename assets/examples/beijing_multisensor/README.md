# 北京市 · 光学与热红外多传感器观测模拟

分类：区域遥感影像。层级：项目内置精选。

## 用途

同一个北京 300 m 场景（2019-08-15 02:55 UTC = 北京时间 10:55），先用 ERA5 能量平衡（EB）求一次四组分温度（光照/阴影土壤、光照/阴影叶片），再让多颗卫星的波段设置复用这组温度成像，比较各传感器的光学反射率与热红外亮温，并做两个专题实验：

- **观测角实验**：Landsat TIRS B10 与 MODIS 31，VZA 0/20/32/40/55°，主平面前向/后向，观察热点；
- **聚合实验**：300 m → 1 km，比较均值、标准差、最大值与热点面积占比。

这是功能与研究情景示例，**没有任何真实观测验证**；各传感器结果是无大气的冠层顶方向量，不是传感器间可观测差异。完整分析见报告 [docs/cases/beijing-multisensor/report.pdf](../../../docs/cases/beijing-multisensor/report.pdf)（13 页）。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `project.json` | 母工程：ERA5 能量平衡，输出 `components.tif`（与 landsat_beijing_eb 等价） |
| `variants/<传感器>.json` | 7 个传感器工程：landsat89、modis、sentinel2、aster、ecostress、viirs、fy3d_mersi2；RT 模式，组分温度默认读 `../reference/components.tif`，只含该传感器的波段 |
| `variants/angles_*.json` | 16 个观测角工程：`angles_{tirs_b10,modis_b31}_{vza00 / vza20,40,55_{back,fwd} / vza32_back}` |
| `sensors.json` | 传感器与波段表（中心波长、半高宽、官方范围、出处、是否待核对） |
| `run_all.mjs` | 一键运行脚本（母工程 → 各传感器 → 角度 → 汇总 → 聚合） |
| `raster_tools.py` | 统计、聚合与参考结果导出（依赖 numpy + osgeo；GDAL ≥ 3.11 才写 float16，旧版退回 uint16×10） |
| `inputs/`、`era5/` | 复制自 `assets/examples/landsat_beijing_eb/`（地类、LAI、DEM、ERA5 气象包），使本案例自包含 |
| `results/` | `summary.csv`（各传感器各波段统计）、`angles.csv`、`aggregation.csv` |
| `reference/` | 精简参考结果（约 14 MB）：`components.tif`、各传感器 `<id>_optical.tif`（float16）与 `<id>_tir_bt.tif`、三份 csv、`parent_scene_summary.json`、`manifest.json` |

栅格：EPSG:32650，300 m，592 × 598；光学有效像元 180408，热红外 182182（含水体 1774）。

## 传感器与波段

中心波长/带宽：光学为 nm，热红外为 μm。STREAM 以 center/fwhm 的**高斯近似**光谱响应函数成像，没有使用各传感器真实的 SRF；官方给范围 [lo, hi] 的取中点与宽度。

| 传感器 | 光学波段 | 热红外波段 | 出处 | 待核对 |
| --- | --- | --- | --- | --- |
| Landsat 8/9 OLI + TIRS | B1–B7 | B10 10.895、B11 12.005 | NASA Landsat 带通表；USGS 波段页 | 无（USGS 页被拦截，值取 NASA 带通 PDF 摘要与论文表） |
| MODIS (Terra) | 1–7 | 31 (10.78–11.28)、32 (11.77–12.27) | modis.gsfc.nasa.gov 规格页（访问失败，数值取自二手引用：NASA 演示 / LP DAAC 文档） | **B32 待核对** |
| Sentinel-2A MSI | B2–B8、B8A、B11、B12 | — | ESA SNAP 文档表 | 无 |
| ASTER | — | B10–B14 (8.125–11.65) | ASTER 特性页 | **B11 待核对** |
| ECOSTRESS | — | TIR1–5（8.285/8.785/9.060/10.522/12.001） | L1B 产品规范 ECO1B_PSD | **全部 5 个波段待核对**（带宽为标称值，TIR3–TIR5 中心也有 0.1 μm 级版本差） |
| VIIRS | M3、M4、M5、M7、M10、M11 | M15 10.763、M16 12.016 | NOAA STAR 页 | 无 |
| FY-3D MERSI-II | CH1–CH4、CH6、CH7 | CH24 (10.3–11.3)、CH25 (11.5–12.5) | ESSD 文献；WMO OSCAR | **光学 CH1–CH4、CH6、CH7 待核对**（OSCAR 页没取回通道表，值为标称） |

“待核对”即 `sensors.json` 中 `verify: true`：数值为标称值或检索摘要，未在官方原文逐项核对，引用前请核对。每个波段的出处链接与备注见 `sensors.json`。

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
- 观测角（B10 相对天底 303.796 K）：后向 VZA 20/32（热点）/40/55° 为 +0.47/+1.78/+0.47/−0.43 K，前向 20/40/55° 为 −0.20/−0.42/−0.66 K。
- 聚合（B10，300 m → 1 km 均值）：均值 −0.006 K，标准差 2.40 → 2.21 K，最大值 316.74 → 314.90 K，热点占比 0.050 → 0.034；B5 为 0.014，NDVI 为 0.011。

## 数据来源与出处

输入数据与 landsat_beijing_eb 相同：地类由 2019 GLC_FCS30D 映射到 IGBP，DEM 为 FABDEM，LAI 为 Landsat 30 m 产品，300 m 输入由 30 m 网格聚合；ERA5 气象日期为 2019-08-15（约 25 km）。传感器波段出处见上表与 `sensors.json`。软件许可证不代替输入数据各自的再分发条款。

## 限制

- 单层冠层、无城市三维结构，角度实验的热点效应不能外推到真实城市；
- ERA5 约 25 km 强迫，300 m 内气象近似常数；
- 高斯 SRF 近似，无大气校正；亮温是冠层顶方向量，**不是传感器间可观测差异**；
- 角度实验只做热红外，没有光学 BRDF；
- 聚合用 GDAL 平均/取最大，1 km 网格从原栅格西北角起算，**不是 MODIS 正弦网格**；
- 引擎日志的“未知地类 171834”是市域外 255 填充像元（592 × 598 − 182182），不是数据缺陷；
- 未做真实观测验证；待补：Landsat 8 B10 实测、MOD11 产品、真实光谱响应函数。

## 参考结果说明

`reference/` 是 run_all.mjs 的一次完整运行的精简拷贝（光学为 float16，亮温为 float32），数值与 `results/` 一致，仅耗时列略有不同；它是历史执行结果，不是打开案例后新生成的结果。更换引擎版本后应重新运行 `run_all.mjs --write-reference` 刷新。

## 报告

- 报告 PDF：[docs/cases/beijing-multisensor/report.pdf](../../../docs/cases/beijing-multisensor/report.pdf)；
- 配图：[docs/cases/beijing-multisensor/figures/](../../../docs/cases/beijing-multisensor/figures/)；亮温与 LST 差表：[tables/tir_bt_minus_lst.csv](../../../docs/cases/beijing-multisensor/tables/tir_bt_minus_lst.csv)；
- 生成脚本：`tools/build-beijing-multisensor-report.py`（报告）、`tools/plot-beijing-multisensor.py`（配图）。
