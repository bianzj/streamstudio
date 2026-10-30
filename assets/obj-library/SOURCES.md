# OBJ 白模库来源

本目录外部模型主要来自 Kenney、Quaternius、OpenGameArt、Poly Haven 与 Khronos glTF Sample Assets。Stream Studio 计算仍以对象绑定的光谱、温度和物化属性为准；真实模型另将材质颜色写入 OBJ 顶点，用于区分树干、叶片、轮胎、玻璃和船体。资源统一按 Y 轴向上使用。

- Nature Kit: https://opengameart.org/content/nature-kit
- City Kit (Suburban): https://opengameart.org/node/121255
- Kenney Mini Forest: https://kenney.nl/assets/mini-forest
- Quaternius LowPoly Crops Pack: https://opengameart.org/content/lowpoly-crops-pack
- Quaternius Animated Tanks Pack: https://quaternius.com/packs/animatedtanks.html
- Quaternius Cars Pack: https://quaternius.com/packs/cars.html
- Quaternius Ships Pack: https://quaternius.com/packs/ships.html
- OpenGameArt Antonov An-28: https://opengameart.org/content/antonov-an-28-3d-model
- OpenGameArt Helicopter: https://opengameart.org/content/helicopter-0
- License: https://creativecommons.org/publicdomain/zero/1.0/

`crop/`、`tree_poplar.obj` 和 `tree_cypress.obj` 是 Stream Studio 自建的程序化低面数白模，采用米制 Y-up 坐标，供模型测试与大量实例场景使用。

`crop/maize-growth/` 与 `crop/wheat-growth/` 各含 4 个连续生长阶段。`large_scene/forest_demo_100m.obj` 由 CC0 树木和林下植被以固定随机种子生成，可通过 `node tools/generate-forest-scene.mjs` 重建。

RAMI-V 官方场景说明：https://rami-benchmark.jrc.ec.europa.eu/ 。DART 团队提供的官方 OBJ 转写包位于 https://dart.omp.eu/index.php#/doc；已将 HET07_JPS_SUM 的简化树木、1120 个实例位置以及场景元数据整理到 `assets/rami-library/HET07_JPS_SUM/`。原始高精度树木达到数 GB，未复制进项目。

## 真实结构模型

- Poly Haven（CC0）：https://polyhaven.com/models
  - `fir_sapling`
  - `island_tree_02`
  - `searsia_lucida`
  - `jacaranda_tree`
  - `fir_tree_01`
  - `pine_tree_01`
  - `tree_small_02`
  - `covered_car`
- Khronos glTF Sample Assets：https://github.com/KhronosGroup/glTF-Sample-Assets
  - `CarConcept`：初始模型由 Unity Fan 按 CC0 发布；详见模型目录 `SOURCE_README.md`。
  - `CesiumMilkTruck`：Cesium，CC-BY-4.0；使用时需要保留署名。

原始 glTF、1K 纹理及逐模型许可说明保存在 `vegetation/realistic`、`vegetation/typical`、`vehicle/realistic`、`vehicle/typical` 和 `ship/typical` 下的 `_..._source` 目录。可通过 `tools/import-realistic-models.py` 与 `tools/build-typical-obj-library.mjs` 重新生成用于 Stream Studio 的中等面数 OBJ。
