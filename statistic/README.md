# Stream Studio 外部统计工具

代码不依赖 Stream Studio GUI，可读取输出 GeoTIFF、VoxelRT/VoxelEB 过程数据、体元流体数据以及 FacetRT/FacetEB 面元结果。

## 安装

```powershell
cd C:\work\streamstudio
python -m pip install -r statistic\requirements.txt
```

若无法安装 `rasterio`，只安装 `numpy Pillow` 仍可读取多数单波段 TIFF；多波段、NoData、坐标系信息建议使用 `rasterio`。

## 使用

分析一个 TIFF：

```powershell
python statistic\histream_statistics.py "工程\output\结果.tif"
```

分析一个三维过程 JSON，并查看 GPP/NPP：

```powershell
python statistic\histream_statistics.py "工程\output\process\voxeleb_T=DOY214_12-20.json" --fields gpp,npp
```

分析一个工程的全部输出：

```powershell
python statistic\histream_statistics.py "工程\output" -o "工程\statistics"
```

流体三维结果：

```powershell
python statistic\histream_statistics.py "工程\output\process\voxelfluid_T=DOY214_12-20.json" --fields windSpeed,airTemperature
```

面元正反面可选择 `--facet-side mean`、`front` 或 `back`。大量数据默认最多抽样 50000 个点写入 CSV 和三维图；完整数据始终用于统计，可用 `--max-points` 修改显示抽样数量。

## 建筑与森林小场景演示

```powershell
node statistic/run_scene_examples.mjs
```

需要已编译的 `models/bin_x64/Release/histream.exe`，以及安装 numpy、Pillow 的 Python（可用 `STREAM_STUDIO_PYTHON` 环境变量指定）。每次创建独立的 `output/component-scenes-*` 目录，运行单栋房屋、9 株简化树木两个 FacetEB 单时刻场景，再生成中文对照 PNG、汇总 CSV/TXT 和各组详细分布图。输入模型、位置与气象副本均保留。工程含绝对路径，迁移目录后需重新指定路径。

演示采用默认物化参数，森林整株统一赋予植被属性；不代表细分木质部、长期预热或实测标定的结果。对照图采用正反面均值、全量面元统计和统一温度色标，均值未按面积加权。

已有演示结果可重画：

```powershell
python statistic/scene_example_report.py "output/component-scenes-实际目录"
```

## 输出

### 土壤/背景、植被和建筑的温度与能量分布（默认）

新版 FacetEB/VoxelEB 过程结果携带组分标识。开启“能量过程”保存温度、显热、潜热和储热；同时开启“辐射过程”保存短波、长波和净辐射。运行后直接分析整个 `output/process` 目录：

```powershell
python statistic\histream_statistics.py "工程\output\process" -o "工程\statistics"
```

每个节点文件的统计目录新增 `components/`：

- `component_statistics.csv`：三类地物的温度与能量字段统计总表；
- `summary.txt`：各组数量、分类和统计口径；
- `soil/`、`vegetation/`、`building/`：各自的 TXT/CSV 摘要、抽样坐标表、直方图和三维着色图。

分类依据引擎保存的地物类型，土壤及背景归入 soil，植被包含树叶和木质部；光伏、水体、其他或未知对象不混入三类。体元按所属实例/网格类型分类，混合体元沿用引擎的归属，不做组分比例拆分。FacetEB 的光伏材质单独标识，即使其几何是树也不会算入植被。

温度在分组结果中转换为 ℃，能量通量保留 W/m²。统计覆盖每组全部有效元素，均值不按面积加权，不能把通量直接当作总功率。CSV 与三维图最多**每组**抽样 50000 个点，可通过 `--max-points` 调整；点表保留原始 `source_index`、XYZ 和组分名称。面元坐标为三角形中心，正反面采用 `--facet-side` 指定口径。

只处理保存文件中存在的字段：辐射和能量过程文件分别统计，不从影像反推组分、不补造缺失字段。旧结果缺少组分标识时生成 `component_notice.txt` 并提示重跑，仍保留原有整体统计。TIFF 本身没有组分掩膜，只能做原有影像统计。`--components none` 关闭分组；显式传入 `--fields` 时仍按原通用模式输出指定字段的图和点表。

### 通用输出

每个输入文件对应一个独立子目录：

- `summary.txt`：来源、模型、时间节点及各字段统计摘要；
- `statistics.csv`：全部波段或字段的数量、均值、标准差、分位数和极值；
- `sample_points.csv`：三维坐标及所选字段抽样数据；
- `*_heatmap.png`：TIFF 二维热力图；
- `*_3d.png`：三维结构及字段着色图；
- `*_histogram.png`：数值直方图。

所有 CSV 使用 UTF-8 BOM，可直接由 Excel 打开。统计使用全部有效值；抽样只影响点 CSV 和三维图片。
