# HiStream 引擎回归测试

用内置示例（`assets/examples`）冻结一组覆盖五种模式的算例，用两版引擎在**完全相同的输入**上运行并对比结果与耗时。改引擎（尤其是着色器或求解器）前后都应跑一遍。

| 脚本 | 作用 |
|---|---|
| `cases.mjs` | 算例定义：示例、模式、气象时段、参数覆盖（含周期边界、多视角、街景透视等专项算例） |
| `prepare.mjs` | 按 `server.mjs` 的 `/api/run` 流程生成冻结输入（规范化、实例筛选/裁切、冻结气象） |
| `run.mjs` | 用指定引擎目录运行全部或部分算例，记录退出码、耗时和日志 |
| `compare.py` | 对比两个变体：TIFF 各波段统计、VoxelEB 分类型能量项与迭代数、FacetEB 收敛、FacetRT 波段统计；`--tolerance` 可用作"纯性能改动不得改变结果"的门禁，但只覆盖 TIFF 波段均值、VoxelEB 的 T 与 Rn、FacetRT 波段均值；FacetEB 收敛状态/迭代数、VoxelEB 迭代数的差异只报告（标 `[CHANGED, not gated]` 并在末尾汇总），不参与判定 |
| `rasterio_lite.py` | 仅依赖 numpy 的 GeoTIFF 读取 |

## 用法

```powershell
# 1. 准备冻结输入（一次即可；示例变化后重做）
node tools/regression/prepare.mjs D:\histream-regression

# 2. 用改动前的引擎跑基线（先把旧 Release 目录复制出来）
node tools/regression/run.mjs D:\histream-regression base --engine D:\engine-before

# 3. 编译新引擎后再跑
node tools/regression/run.mjs D:\histream-regression new        # 默认 models/bin_x64/Release

# 4. 对比；纯性能改动加 --tolerance 1e-4
python -I tools/regression/compare.py D:\histream-regression base new --tolerance 1e-4
```

只跑部分算例：在命令末尾列出算例名，例如 `... run.mjs D:\r new veb_urban feb_urban`。

临时参数覆盖：`$env:REGRESSION_SET='configuration.control.samples=256;configuration.control.periodicTraversalCount=8'`。

## 说明

- 示例更新、或 `prepare.mjs` 规范化逻辑变化时，旧变体的结果与新输入不可比，需要重跑基线。
- RayTracing 为蒙特卡洛结果，但随机序列固定，同一引擎重复运行逐位相同；改动采样逻辑后应比较统计量而非逐位一致。
- Facet 可见性栅格可用环境变量 `HISTREAM_FACET_RASTER_SIZE=2048/4096` 提高，用作采样偏差的参考解（显存受 4 GB 存储缓冲上限约束；CPU 后端 `HISTREAM_FACET_BACKEND=cpu` 配合 `HISTREAM_FACET_RASTER_LAYERS=256` 不受此限）。
