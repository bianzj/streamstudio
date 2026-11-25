# 北京市 · 光学与热红外多传感器观测模拟

分类：区域遥感影像。层级：项目内置精选。（骨架，说明正文由下一步任务补全）

## 内容

同一个北京 300 m 场景（2019-08-15 02:55 UTC = 北京 10:55），先用 ERA5 能量平衡求一次四组分温度，再让多颗卫星的波段设置复用这组温度成像，比较各传感器的光学反射率、热红外亮温，并做观测角实验与 300 m → 1 km 聚合实验。

## 目录

| 路径 | 内容 |
| --- | --- |
| `project.json` | 母工程：ERA5 能量平衡，输出 `components.tif`（与 landsat_beijing_eb 等价） |
| `variants/<传感器>.json` | 各传感器工程：复用母工程的组分温度，只含该传感器的波段 |
| `variants/angles_*.json` | 观测角实验：Landsat TIRS B10 与 MODIS 31，VZA 0/20/40/55°，主平面前向/后向 |
| `sensors.json` | 传感器与波段表（中心波长、半高宽、出处、是否待核对） |
| `run_all.mjs` | 一键运行脚本（母工程 → 各传感器 → 角度 → 汇总 → 聚合） |
| `inputs/`、`era5/` | 复制自 `assets/examples/landsat_beijing_eb/`（地类、LAI、DEM、ERA5 气象包），使本案例自包含 |
| `reference/` | 精简参考结果 |

## 数据来源

输入数据来源与 landsat_beijing_eb 相同：地类由 2019 GLC_FCS30D 映射到 IGBP，DEM 为 FABDEM，LAI 为 Landsat 30 m 产品，300 m 输入由 30 m 网格聚合；ERA5 气象日期为 2019-08-15。数值收敛与成像结果不代表独立观测精度。

## 运行

```text
node assets/examples/beijing_multisensor/run_all.mjs [--engine <stream.exe>] [--out <输出目录>]
```

（运行方式、参数与结果说明待补全。）
