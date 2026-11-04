# 案例库与培训目录

## 用户发布（含培训案例）

D:/发布/streamstudio/2.0.1 将安装器、用户手册和随附案例放在同一版本目录。2026-11-04 起原 D:/examples 的培训案例已并入其中，D:/examples 只保留一份指向说明。examples 下共九例：

| 分类 | 文件夹 | 说明 | 验收 |
| --- | --- | --- | --- |
| 区域遥感影像 | MODIS-ERA5 | 北京 1 km，ERA5 驱动能量平衡 | 4×4 像元运行通过（2026-11-01） |
| 区域遥感影像 | Landsat-ERA5 | 北京 300 m，ERA5 驱动能量平衡 | 4×4 像元运行通过 |
| 区域遥感影像 | MODIS-LSCT | 北京 1 km，读取组分温度 | 4×4 像元运行通过 |
| 区域遥感影像 | Landsat-LSCT | 北京 300 m，读取组分温度 | 4×4 像元运行通过 |
| 农业与生态 | Forest（森林） | 光线追踪成像，eRaytracing | 未运行 |
| 农业与生态 | Grassland-Pine（草地与松树） | 体元能量平衡，eVoxelEB | 未运行 |
| 城市与建筑 | Beijing-Urban（北京城市） | 体元辐射传输，eVoxelRT | 未运行 |
| 光伏与新能源 | Tilted-PV（倾斜光伏板） | 面元辐射传输，eFacetRT | 未运行 |
| 海面与目标 | Ship-Sea（船舶与海面） | 光线追踪成像，eRaytracing | 未运行 |

影像案例默认 4×4 像元、2 线程；运行前复制到全英文可写路径。详见 [验收记录](testing/user-release-cases-20261101.md)。LSCT 条目读取既有组分温度，不执行温度反演。MODIS-ERA5 和 Landsat-ERA5 的 reference 为历史全图结果，综合收敛率 98.82% 和 99.95%，分别保留 204 和 82 个异常像元标记；数值收敛不等于观测精度。

合并前的 D:/examples 原样备份在 D:/cases/StreamStudio/项目清理归档/20261104-examples-merge。完整研究历史与维护资料不进入用户发布；此前误归集的大包保存在 D:/cases/StreamStudio/发布整理归档/20261101-开发与研究资料。

## 项目内部

assets/examples 提供五个精选内置案例：玉米、森林、城市、MODIS 能量平衡、Landsat 能量平衡。唯一内置索引为 assets/examples/catalog.json；玉米曲面光伏技术演示已撤下。

## 研究与维护

阿柔站、真实山地、虚拟山地保存在 D:/cases/真实地形 和 D:/cases/虚拟地形，入口见 D:/cases/StreamStudio地形案例.json；完整旧运行放在 D:/cases/StreamStudio/历史运行。

唯一 SCOPE 对比库位于 D:/cases/StreamStudio/模型对比/SCOPE；只有明确要求时运行。

培训案例机器索引为 D:/cases/StreamStudio/培训案例目录.json（已指向发布目录）。整理记录和待删除资料在 D:/cases/StreamStudio 下，不进入发布目录。上述开发机路径不是内置工程的运行依赖。
