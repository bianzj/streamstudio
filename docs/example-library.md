# 案例库与培训目录

## 项目内部

assets/examples提供五个精选案例：玉米、森林、城市、MODIS能量平衡、Landsat能量平衡。唯一内置索引为assets/examples/catalog.json；玉米曲面光伏技术演示已撤下。

## 项目外部培训

D:/examples提供九个独立培训案例和一份开始使用.md：

- MODIS-ERA5
- Landsat-ERA5
- MODIS-LSCT
- Landsat-LSCT
- 船舶与海面
- 草地与松树（农业与生态）
- 森林（农业与生态）
- 北京城市（城市与建筑）
- 倾斜光伏板（光伏与新能源，当前FacetRT）

每例以project.json为入口，README说明步骤。遥感输入在inputs，气象在era5，一份参考产品在reference，新运行写入output。LSCT条目是读取既有组分温度的示例，不执行温度反演，保留旧温度来源的质量限制。

MODIS-ERA5和Landsat-ERA5已有参考的综合收敛率为98.82%和99.95%，分别保留204和82个异常像元标记。数值收敛不等于观测精度。

## 研究与维护

阿柔站、真实山地、虚拟山地保存在D:/cases/真实地形和D:/cases/虚拟地形，入口见D:/cases/StreamStudio地形案例.json。草地与松树、森林、北京城市、倾斜光伏板已纳入D:/examples培训目录；完整旧运行放在D:/cases/StreamStudio/历史运行。

唯一SCOPE对比库位于D:/cases/StreamStudio/模型对比/SCOPE；只有明确要求时运行。

培训机器索引位于D:/cases/StreamStudio/培训案例目录.json。整理记录和待删除资料在D:/cases/StreamStudio下，均不进入培训目录。上述开发机路径不是内置工程的运行依赖。
