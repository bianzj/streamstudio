# Stream Studio 审阅：待确认后修改清单

来源：2026-10-28 全量静态审阅（共 387 处 `REVIEW[...]` 注释，分布在 60 个文件）。
本文档记录**不立即修改、等待作者确认**的事项。已修复的 bug / 内存泄漏见
`2026-10-28-fix-log.md`。

约定：

- 本轮修复**没有编译、没有运行、没有测试**（遵循项目 AGENTS.md 与本次任务约束）。
- 修复范围仅限：崩溃与越界、空指针解引用、内存/资源/句柄泄漏、明确逻辑错误、
  未捕获异常导致的崩溃、缺字段解析崩溃。
- 下列类别**不改代码**，先在本文档定方案，经确认后再动。

---

## 1. 数值常量与迭代上限（明确不改）

数值本身可能是标定/论文取值的产物，改动等于改物理结果，必须由作者决定。

| 位置 | 内容 | 需要确认 |
| --- | --- | --- |
| `models/histream/src/facetrt/facetrt_vulkan.cpp:1515` | 能量平衡固定 64 次 Jacobi 迭代，不检查残差 | 是否保留 64？是否引入残差判据、热启动？容差取多少 |
| `models/histream/src/voxeleb/command.cpp:291` | EB 迭代上限 50、生理迭代 25 | 是否改上限、未收敛时是否中断或标记 |
| `models/stream/physics/energy.cpp:25` | 实现为 0.8 阻尼牛顿 / 单步 5 K / 0.5 松弛 / 容差 1e-4+2%；PORTING.md 与 README 描述的是割线法 / 1.5 K / 1e-7+1e-6 | **文档与实现不一致**：以哪个为准？若要恢复割线法属于算法替换 |
| `models/stream/app/image.cpp:174` | EB 起算点固定为过境当天 00 UTC，不按经度 | 起转时长改为多少（建议 ≥24 h）？ERA5 读取时段是否相应扩大 |
| `models/stream/physics/energy.cpp:34` | 组分温度硬下限 240 K | 下限改 190–200 K？还是按 ERA5 skt 动态定 |
| `models/stream/app/image.cpp:284` | 土壤水分截断 0.05–0.55，ERA5 路径 0–1 | 两条路径统一到哪个区间 |
| `models/stream/app/image.cpp:397` | LAI 上限 100，MODIS 填充码 249–254 可能被当有效值 | 收紧到 10–15？填充码是否在数据侧声明为 nodata |
| `models/histream/src/base/hexvoxel.cpp:971` | 覆盖率饱和时 CI 由采样数 N 决定 | 是否给 CI 设物理上限、饱和体元是否单独标记 |
| `models/histream/src/base/hexvoxel.cpp:814` | 扩格用的 1e-9f 小于量级 ULP | 改用与量级相关的 eps，取多少 |

## 2. 坐标轴与 DEM 方向（明确不改，需作者确认）

这些是数据约定问题，改错会整体旋转/镜像地形，且没有回归数据无法验证。

| 位置 | 内容 | 需要确认 |
| --- | --- | --- |
| `models/histream/src/base/scene.cpp:90` | DEM 采样轴向（x→行并翻转）可能把地形转 90°；忽略 GeoTransform | 实际 GeoTIFF 的 row/col 与 north/east 对应关系；是否需要读 GeoTransform |
| `models/histream/src/base/scene.cpp:3132` | `XYZ2XZY` 交换 y/z 但未交换法向，镜像变换翻转绕序 | Z-up ↔ Y-up 的正式约定 |
| `models/histream/src/base/voxeldesigner.cpp:686` | 同上，导出 OBJ 时交换 y/z 导致法向朝内 | 导出坐标系约定 |
| `models/histream/src/base/fileio.cpp:1203` | `shapes` 的 (X,Y,Z) 被重排为 (Y,Z,X) | 三个轴的语义（东/北/高？） |
| `models/histream/src/usdtool/histream_usd.cpp` | USD Z-up → 北/东映射 | 与上一条保持一致 |
| `models/histream/src/base/scene.cpp:531,1338` | 异质体元旋转被四舍五入到 90° 倍数 | 是否支持任意角、逆映射是否需多点采样 |
| `models/stream/physics`: 观测方位角约定 | 热点判据隐含 VAA 为"像元指向传感器" | 是否提供 `viewAzimuthConvention` 配置 |

## 3. 单位与物理约定

| 位置 | 内容 | 需要确认 |
| --- | --- | --- |
| `models/histream/src/base/structs.h` `MeteoMeta.Tsold` | 用 °C，其余温度用 K | 统一为 K？涉及文件格式与旧工程兼容 |
| `models/histream/src/base/xmlexamples.h:96` | 内置示例 `SoilSet` 按位置初始化，Tsoil 与 smc 互换（Tsoil=0.25、smc=25） | 确认为第 1 类错误后修正示例；同源问题见 `project-schema.js` 的 Tsoil 缺省 0.25 |
| 土壤水分 | `smc` 有时是百分比（25），有时是体积分数（0.45） | 定一个单位并在 schema/引擎侧校验 |
| `models/stream/physics/brightness.h:8` | c2 用 14388.29 μm·K（旧值），CODATA 为 14387.769 | 保留（与 TiRT 逐位一致）还是更新 |
| `models/histream/src/base/compo.cpp:479` | BSM 在 SMC<5% 时 mu<0，湿土反射率非物理 | 是否照 SCOPE 取 rwet=rdry |
| `models/stream/physics/longwave.h:44` | 叶片源项按 σT⁴ 而非 ε_lσT⁴，与 energy.cpp 发射项不闭合 | 统一 ε 约定后做闭合校验 |
| `models/stream/app/image.cpp:671` | 冠层顶辐亮度只有发射项，缺 (1−ε_eff)·L↓ | 是否加入下行辐射反射项；MIR 波段下限定多少 |
| `models/stream/physics/energy.cpp:140` | ERA5 的 10 m 风 / 2 m 气温直接当作参考高度 z 的值 | 是否做对数廓线外推 |
| `models/histream/src/facetrt/facetrtcore.cpp:894` | Collatz 的 Q10 项乘了 Tcor（默认 0），默认无温度响应 | 默认参数是否为预期 |
| `models/histream/src/facetrt/facetrtcore.cpp:972` | 宽带加权假设波段在 400–2400 nm 等距 | 传感器波段下的加权方案 |

## 3.5 修复过程中暴露出、需要你决策的新增项

这些是修 bug 时发现「顺带不修就会留下隐患」的问题，因为涉及默认值或行为改变，没有直接改。

### 3.5.1 `FluspectParam` 缺默认值 → 现在会导致加载失败

- 现状：`compo.cpp` 的 `fluspect` 已加参数校验（`N>=1`、`Cab/Cw/Cdm/Cs` 有限非负），
  非法时抛 `runtime_error`。但 `structs.h` 里 `FluspectParam` 的字段**没有默认初值**
  （`structs.h:171` 已有 REVIEW 标注）。
- 后果：**XML 配置里缺 `<N>` 时，从「静默用 0.2 继续算」变成「加载直接失败」**。
  JSON 路径因为默认 `N=1.5`，不受影响。
- 需要你选：
  - A. 给 `FluspectParam` 补默认成员初值（与 JSON 路径默认一致，`N=1.5`）——
    改动是「补默认值」，属于数值默认，因此本轮没做
  - B. 把 `fluspect` 的抛异常降级为「告警一次 + 用默认值继续」
  - C. 保持现在的抛异常，认为非法配置就该失败
- 建议 A：既消除未初始化 UB，也不改变合法配置的结果。

#### 建议的默认初值清单（供确认，本轮未写入代码）

`structs.h`：

| 结构体 | 建议初值 |
| --- | --- |
| `FluspectParam` | `Cab 40, Cw 0.009, Cdm 0.012, Cs 0, N 1.5` |
| `SpectralXml` | `type = CUSTOM, refl_tir 0.05, tau_tir 0` |
| `SensorXml` | `projection = PERSPECTIVE, resolution {512,512}` |
| `LightXml` | `solarAngle {40,30}`（度）、`direct 0.9, diffuse 0.1` |
| `Background` | `sceneSize {100,10,100}, sceneOrigin {0,0,0}, stepsize_surface 1, stepsize_height 1, lat 40, lon 116` |
| `MeteoMeta` | `t0, RIn 0, Rli 0, u 0.1, p 1013, start/endYear 2019, Doy 1` |
| `Shape` | `shapetype = CUBE, height/width/length 1, pos {0,0,0}`；`PrimEntity::type = SOIL` |
| `ThermalXml` | 温度 300 K |
| `ObjMesh` / `PrimMesh` | `meshId -1, nIndices 0, nVertices 0, meshcenter {0,0,0}` |

`structs_cg.h`（CPU/GPU 共享，改前需确认着色器侧一致性）：

| 结构体 | 建议初值 |
| --- | --- |
| `LightSet` | `skyTemperature 250`（与 `LightXml` 一致）、`direction` 用归一化单位向量 |
| `AeroCond` | `type 0, ustar 0.3, hc_veg 1, hc_build 1, lai 1, leafwidth 0.1, cover 1`（`L` 保留 0，运行期更新） |
| `Canopy` | `lai 1, density 1, height 1, width 1, G 0.5, LIDFa -0.35, LIDFb -0.15, hspot 0.2, leafwidth 0.1` |
| `LeafBio` | `Vcmax 80, m 9, BallBerry 0.01, Type 3, kV 0.6396, Rdparam 0.015, Tparam {0.2,0.3,288,313,328}, Tyear 25, beta 0.507, kNPQs 0, qLs 1, stressfactor 1, Tcor 0` |
| `SoilSet` | `method 1, rss 2000, cs 1180, rhos 1800, lambdas 1.55, Tsoil 25, smc 0.25, SatWater 0.45` |
| `WaterSet` | `rss 0, heatCapacity 4.18e6, mixingDepth 1, evaporationCoefficient 1` |
| `BuildUp` | `method 1, rss 500, cs 1180, rhos 1800, lambdas 1.55, Tin 25` |
| `Meteo` | `z 10, t 0, u 0.1, p 1013, Rin 0, Rli 0, Ta 298.15, sm 0.25, ea 15, Ca 380, Oa 209, dTime 600` |
| `Spectral` / `Thermal` | `{0,0}` / 300 K |

注意：`Tsoil`/`Tin` 用摄氏度而其他温度字段用 K，这个单位不一致需要先统一，
否则补默认值只会把错误固化。

### 3.5.2 光谱长度不一致的 CUSTOM 材质

`compo.cpp` 现在按较短长度插值（原来是整段返回首个值）。输出光谱会变。
若历史工程里有波长表与反射率表长度不等的手工光谱，结果与以前不同。

### 3.5.3 大气 LUT 告警文本变更

旧文本 `Atmosphere LUT unavailable` 已改为带路径的中文说明。
如果有外部脚本或日志断言依赖旧文本，需要同步。

### 3.5.4 `voxel_boundary` 输出三角形顺序

去重改用 `unordered_set` 遍历，几何集合不变但三角形**顺序**变了。
若有下游按顺序比对二进制输出，需要改为按集合比对。

### 3.5.5 `saveImage1` / `saveImage` 失败返回值

由「恒返回 1」改为失败返回 0。当前无活跃调用方，但外部脚本若依赖返回值需注意。

## 4. 需要设计决策（不宜在无测试条件下直接改）
| 位置 | 内容 | 建议方向 |
| --- | --- | --- |
| `server.mjs:463` | 本地 API 只校验 Host/Origin/Content-Type，无会话令牌；GET 可绕过 Origin | 启动时生成随机令牌经 preload 注入，全部 `/api` 校验自定义头；改动涉及 desktop/main/preload，需一起测 |
| `server.mjs:2383` | "删除全部结果"可能删除 `terrain/`、`textures/` 等输入 | 收紧为 `outDir` 与 `runs/<id>`，但要确认现有目录布局 |
| `server.mjs:4160,4169,4220` | 目录列举/资源读取/打开资源管理器允许工程外路径 | 统一用 `realPathWithin`；会影响现有"打开任意结果"用法 |
| `server.mjs:1988` | project.json 等直接覆盖写，无原子替换 | 改临时文件 + rename；需确认与自动保存频率的交互 |
| `electron-builder.yml:42` | `docs/` 整目录进安装包（含内部审计与计划文档） | 白名单只打包用户/理论手册 PDF |
| `server/stream/index.mjs:258` | ERA5 所有 .nc 路径走命令行，超 Windows 32K 上限 | 改为响应文件/清单文件传参，需同步改 `models/stream/app/config.cpp` |
| `server/stream/index.mjs:157` + `workspace.js:921` | 前后端 `projectPath` 字段位置不一致 | 统一字段位置；涉及两侧同时改 |
| `server/stream/config.mjs:206`、`main.js:4097` | `Number('')`/`Number(null)`=0 被当有效值 | 需要统一"空值"语义与表单默认值策略 |
| `models/histream/shader/*` 与 `structs_cg.h` | 已核对布局一致 | 后续改动需保持 scalar 布局 |

## 5. 语义/算法改进（确认后单独排期）

- `facetrt_vulkan.cpp:1141`、`facetrtcore.cpp:1810`：无栅格样本的面元按全开阔天空处理，
  冠层内部入射辐射系统性高估。改法（抖动多采样 / 邻域估计）影响性能与结果。
- `facetrt_vulkan.cpp:1071`：整张边哈希表一次回读（最大约 6 GiB）。需 GPU 侧压缩方案。
- `voxelrt/buffer.cpp:201`：大场景 rads 缓冲超 `maxStorageBufferRange`。
  参考 VoxelEB 改 `buffer_reference`，属结构性改动。
- 体元 1D 派发超过 `maxComputeWorkGroupCount`（`voxelrt/command.cpp:41`、
  `voxeleb/command.cpp:201`）：需改 2D 派发或分批。
- `scene.cpp` 性能：体元化遍历三角形整个 3D 包围盒、每格 6 次堆分配裁剪。
- `utils.cpp:388`：指数积分截断在 t=100，x=0.001 时误差约 29%。需改积分方案并验证精度。
- `facetrtcore.cpp:1489`：CPU 光栅缺 top-left 规则，与 GPU 结果不一致。属正确性问题，
  但会改变现有 CPU 结果，需与 GPU 对齐后验证。

## 6. 未完成的审阅覆盖

以下区域本轮没有审到或只部分审到，后续需要补：

- `models/histream/src/voxeleb/voxeleb.cpp`（73 KB）、`voxeleb/descriptor.cpp`、
  `voxeleb/pipeline.cpp`、`voxeleb/observer_capture.inl`
- `models/histream/src/raytracing/*`（仅 grep 扫过）
- `models/histream/src/voxelrt/pipeline.cpp`、`voxelrt/descriptor.cpp`（部分）
- `models/histream/src/base/reflectance_texture.cpp`
- `src/renderer/src/main.js` 第 620–1099、1305–1845 行（Three.js 对象密集区）
- `models/stream/forcing/*`、`app/config.cpp`、`scene/*`、`physics/` 多数文件
  （子代理上下文耗尽，只有零星注释）
