# 体元网格跳空（HDDA）与漫射方向旋转

2026-11-26。适用 VoxelRT / VoxelEB（`models/histream`）。本文记录两项改动做了什么、没做什么、实测结果与开关。
**默认值（2026-11-26 更新）**：漫射方向旋转**默认关闭**（`legacy`，与基线可执行文件逐位相同），`rot` 为可选项；DDA 跳空**默认开启**（结果在浮点舍入内相同，不逐位相同；`HISTREAM_VOXEL_SKIP=off` 或工程 `control.voxelEmptySkip=false` 恢复旧逐格 DDA，此时与旧引擎逐位相同）。工程字段与环境变量见第 5 节，环境变量优先。

## 1. 现状（调查结论）

- 体元网格是**均匀单位格**：NanoVDB `GridBuilder<int32_t>(-1)`，值 = `bufferId`，背景 −1；体元尺寸只在推送常量 `setting.scale` 里（`scene.cpp` 建网格，`nanovdb_info.glsl` 读）。
- 主传输不是逐格 DDA：`ClosestHit`（`traceray_comp.glsl`）用 `rayQuery` 对体元代理面（`voxeldesigner.cpp` 的 `createTriVoxels`，每个浊介质体元 3 张中心面）求交，命中后用 `nanovdbGet` **点查询**取体元。空体元不在 BVH 里，硬件 BVH 已经是层级跳空；HDDA 对它没有意义。
- 真正逐格 DDA 的只有：`TurbidBoxSegmentOpticalDepth`、`TurbidPeriodicBegin/Next`（`TurbidEscapeTransmittance` 经由它们）、`escapeSegmentSource`（`voxelrad_scattering_VNIR.comp`）、`aeresist.comp` 的 `columnCanopy`。每格一次 `nanovdbGet`，空格也查。
- `radiationMaxSteps` / `solarVisibility stepLimit` 数的是 `ClosestHit` 次数（平面命中），与上述 DDA 无关，本次不改变它们。

## 2. 没有做可变分辨率（八叉树）体元，原因

单位格假设遍布全引擎，改成多级格要同时动：约 15 处 `floor(position ± 0.05)` 取格（`getVoxelIdSemi` 等）；`voxelLinks.voxelId`（整数 ivec3）与各逐体元缓冲的索引；推送常量里单一的 `setting.scale`；代理面恰落在格面/中面上的约定（`voxeldesigner.cpp`、`scene.cpp` 的世界对齐代理）；周期边界按“格数”折返；`aeresist` 的垂直列扫描；LBM 流体网格；异质体元 HEX 统计按格尺度提取；建筑 5 面状态；VoxelEB 的 N×64 方向场、天空透过率缓存、图像环境槽的步长。收益（空区/均质区用粗格）也有现成手段：粗格 + HEX 模型 5 的层级 mipmap（`functions.glsl` 的 `ResolveHierarchyTransmission`）。因此改做下面这个不改数据结构的方案。

## 3. HDDA 跳空（默认开启；`control.voxelEmptySkip` / `HISTREAM_VOXEL_SKIP`）

做法（只省略消光恒为 0 的空格，结果在浮点舍入内不变）：

1. 把 DDA 盒裁剪到 NanoVDB 根包围盒（`ConfigureTurbidGrid` 每线程读一次，存入 `turbidGridLo/Hi`）；周期边界只裁竖直方向。
2. `nanovdbGetDim`（`nanovdb_info.glsl`）一次树遍历同时给出值和节点边长：叶内 1，缺失叶 8，缺失下层节点 128，上层/根瓦片 4096。值 < 0 且边长 > 1 时，沿射线直接跳到该对齐块的出口（`TurbidBlockExit` + `TurbidDdaResume`）。
3. 周期边界：块不跨周期边缘（折返后的查找坐标按周期截断），周期块计数不变；`TurbidPeriodicNextCell` 是带跳空的 `TurbidPeriodicNext`。
4. `aeresist.comp` 的竖直列扫描按边长直接跳到块尾。

实测（RTX PRO 6000，**测试期间 GPU 被另一批用户任务占满，计时噪声很大**；下面取多次交替运行的最小/中位数）：

| 场景 | 结果 |
|---|---|
| forest（200×200×46 格，树冠占满高度） | 无可测差异（TIR 单次最快 35.6 → 33.3 ms，在噪声内） |
| urban_block（VoxelRT，周期 20） | diffuse 6 次派发总时间：基线 387–402 ms（很稳），HDDA 中位 360、最小 218 ms |
| forest_big（同一森林放进 400×400 m 域，800×800×46 格，空区多） | TIR 单次最快 109 → 29–61 ms（中位约 2.5×）；短波散射阶 s3 单次 156 → 46–105 ms；天空阶 s1 192 → 52–168 ms |

结论：小而满的场景几乎没有收益；稀疏大场景（树冠之间、建筑之上有大片空块）2–4×。

数值：不是逐位相同（跳空后 `tMax` 重新计算）。forest_big：相对差 99 分位 < 2.4e-7，99.99 分位 < 1.2e-5，最大 3.5e-4（个别体元）；forest：99.99 分位 < 4e-5，最大 5e-4；urban_block 影像均值差 ~1e-6。（现已改为默认开启，检验见第 5 节末尾。）

## 4. 漫射方向离散

现状：`DIFFUSE_DIRECTIONS[64]`（`global.glsl`）是确定性的全球面黄金角格点（|y| = (2k+1)/64，32 对精确反向，等立体角）。VoxelRT 漫射已经做“体元哈希方位偏移 + 5 组黄金角方位旋转”，但只绕 y 轴，64 个 |y| 层不变；VoxelEB 热红外漫射（TIR）完全固定格点；VoxelEB 短波散射的 `cumulated/induced` 是 N×64 的**逐方向场**，体元之间按方向下标互相读取（`induced[w*64+(k^1)]`）。

`HISTREAM_VOXEL_DIRROT=rot`（可选，非默认；`shader/voxel_dirrot.glsl`）：

- VoxelRT 漫射：每体元、每碰撞阶、每采样组一个哈希出的**均匀三维随机旋转**（pcg3d + Shoemake 四元数），射线数不变（5×64）。
- VoxelEB TIR：每体元一个固定的哈希旋转，**只由体元编号和面编号决定，不随能量平衡迭代变化**（TIR 每次迭代重算，方向集若变会让温度不动点抖动）。
- 旋转后 |y| 不再是 (2k+1)/64，所以叶片 G 与接收体权重（2G/ΣG、cos/Σcos）改为对旋转后的方向**解析计算**（`CanopyG`），不再查 `diffuseG[]` 表。各向同性天空下权重和仍恒为 1。
- `HISTREAM_VOXEL_DIRROT=rot-all` 目前等同 `rot`。

没有做 VoxelEB 短波散射（B3）：它的方向场在体元间按下标交换，只能所有体元共用同一个旋转；而默认只有 1 阶，全局旋转只是把同一套 64 方向格点整体转一个角度，射线效应的图样同样存在，没有平均效果。要真有收益必须 K 个全局旋转各跑一遍完整散射再平均（耗时 ×K，核表还要换成两个旋转之间的核），风险/收益比不合适，留作后续。

验证：

1. 离线（`tools/test-diffuse-directions.cpp`，目标 `histream_diffuse_directions_tests`，不需要 GPU）：水平地面上方一个球形发射体，对比解析值 (a/R)²cosθ。5×5 滤波后的相对 RMS（越小越好；a=0.5,h=3 / a=1,h=5 / a=0.75,h=8）：固定 64：4.8 / 3.2 / 3.6；旧版 VoxelRT（方位×5，320 条）：1.22 / 0.87 / 1.04；rot 单组（64 条，TIR 用）：2.06 / 1.36 / 2.02；rot ×5（320 条）：1.11 / 0.61 / 0.91；固定 320：2.03 / 1.17 / 1.35。要点：同样 64 条，哈希旋转比固定格点好 2 倍以上；VoxelRT 旧版方位随机化已拿到大部分收益，三维旋转再好 10–30%；固定格点即使加到 320 条仍有相干辐条。随机旋转把辐条变成无相干噪声，辐条指标（环内扇区变异系数）并不下降。
2. 引擎点源（`tests` 外的临时工程，scratch）：48×48×8 格裸土上 1 个 400 K 不透明立方体，10.5 µm，天空 250 K。影像（含引擎自带 5×5 滤波）与 4096 条射线（用 glslc `-DVOXEL_DIFFUSE_SETS=64` 单独编译的参考着色器）比较：相对解析形状的 RMS：legacy 0.73，rot 0.56，4096 射线参考 0.17–0.18；环内扇区变异系数 0.42 / 0.30 / 0.11。
3. 各向同性天空 + 平坦土壤：三种模式的亮温都是 250.600769 K，远场起伏 ~1e-4 K（离线检查中权重和 Σw[y>0] = 1，误差 0）。
4. forest（VoxelEB）rot：能量平衡 19 次迭代收敛（基线 20），10.5 µm 亮温均值 307.8388 vs 307.8400 K，逐体元温度相对差 99 分位 0.6%、最大 1.8%；urban_block rot：反射率/亮温均值与基线差 ≤1e-4。

`rot` 的代价是耗时（因此不作默认，由用户按需开启）：随机旋转破坏同一 warp 内射线方向的一致性，GPU 空闲时实测 forest 单次 TIR 28 → 61 ms（总耗时 4.4 → 4.8 s，+9%），urban_block VoxelRT 漫射 235 → 355 ms（总耗时 +10%），短波散射不变，显存不变。把解析 G 换成 32 点表插值没有提速（表插值在 μ→1 的端点相对误差可达 17%，已放弃）。开启：`control.voxelDirectionRotation = "rot"` 或 `$env:HISTREAM_VOXEL_DIRROT='rot'`。折中：`control.voxelDirectionRotationBlock`（1/2/4/8/16，缺省 1）或 `HISTREAM_VOXEL_DIRROT_BLOCK=n` 让 n³ 体元块共用一个旋转以恢复相干性——TIR 单次 61 → 57（n=2）/ 41（n=4）/ 37 ms（n=8）；但离线点源检验中 5×5 滤波误差随块变大而回升（a=0.5,h=3：VoxelRT 1.11 → 1.69（n=4）、TIR 2.06 → 2.43，已不优于旧版方位随机化）。

## 5. 开关与诊断

优先级：环境变量 > 工程 `control` 字段 > 引擎缺省。

| 工程字段（`control.`） | 环境变量 | 取值 | 缺省 |
|---|---|---|---|
| `voxelDirectionRotation` | `HISTREAM_VOXEL_DIRROT` | `"legacy"` / `"rot"`（环境另接受 `rot-all`、`off/0/false/fixed`） | `legacy` |
| `voxelDirectionRotationBlock` | `HISTREAM_VOXEL_DIRROT_BLOCK` | 1 / 2 / 4 / 8 / 16（rot 时 n³ 体元块共用一个旋转） | 1 |
| `voxelEmptySkip` | `HISTREAM_VOXEL_SKIP` | 布尔 / `on` `off` | `true`（on） |
| `voxelProxyGeometry` | `HISTREAM_VOXEL_PROXY` | `"full"` / `"position"` / `"positionNoIndex"` | `positionNoIndex`（输出与 `full` 逐位相同，见 `docs/voxel-proxy-geometry.md`） |
| — | `HISTREAM_VOXEL_PROXY_STATS` | `1` | 关；打印 `PROXY_GEOMETRY`、每个网格的 `PROXY_BLAS`（三角形数、顶点字节、BLAS 与 scratch 大小） |
| — | `HISTREAM_VOXEL_PASS_TIMING` | `1` | 关；打印各阶段累计耗时（VoxelEB 另有单次最快 `minCallMs` 与短波散射分阶段） |
| — | `HISTREAM_VOXEL_MERGE_STATS` | `1` / `only` | 关；打印两级体元潜力表（只读，已搁置，见下） |
| — | `HISTREAM_VOXEL_TEST_RADIANCE_VOXELS` | 整数 N ≥ 实际体元数 | 关；仅测试：VoxelEB 辐射缓冲按 N 个体元布局，用小场景检验超过 2³² 条目的 64 位下标（结果必须与正常运行逐位相同；缓冲按 N×792 B 分配，注意显存） |

`control.voxelDirectionRotation` 取其他值时引擎报错退出；块边长取最近的 1/2/4/8/16。位定义在 `src/base/voxelflags.h` 与 `shader/voxel_flags.glsl`，经推送常量 `voxelFlags` 传入（VoxelEB 复用原来未使用的 `maxIteration` 位置，VoxelRT 末尾追加；`VOXEL_FLAG_VALID` 位保证新旧着色器/可执行文件混用时按旧行为）。

### 跳空默认开启的检验（2026-11-26）

对 7 个场景各交替运行跳空开/关各 3 次（GPU 被其他任务占用，计时有噪声，下表取最小/中位）。最终产品对比（开 − 关）：

| 场景 | 能量平衡迭代 | 类均温差 | 净辐射差 | 影像波段均值差 | 耗时（PASS_TIMING 合计，关 → 开） |
|---|---|---|---|---|---|
| forest（VoxelEB，0.5 m） | 20 = 20 | < 1e-5 K | < 1e-5 W/m² | ≤ 3e-4 K（10.5 µm）、反射率 ≤ 2e-5 | 最小 4.7 → 1.8 s、中位 7.3 → 4.7 s |
| forest + 周期边界 8（VoxelEB） | 19 = 19 | < 1e-5 K | < 1e-5 W/m² | ≤ 4.5e-4 K、反射率 ≤ 2e-5 | 最小 2.59 → 2.55 s（持平） |
| forest + 雾椭球 + 两处火焰椭球（参与介质） | 50 = 50（342 个状态未收敛，开/关相同：固定温度介质不满足残差判据） | < 1e-5 K | < 1e-5 W/m² | ≤ 2.7e-4 K、反射率 ≤ 2e-5 | 7.5 → 6.9 s（−8%） |
| rainforest 5 m（VoxelEB） | 27 = 27 | < 1e-5 K | < 1e-5 W/m² | ≤ 1.4e-5 K | 2.03 → 2.06 s（持平） |
| rainforest 5 m（VoxelRT） | — | — | — | ≤ 2e-7 | 156 → 157 ms（持平） |
| urban_block（VoxelRT，周期 20） | — | — | — | ≤ 3.8e-5 K、反射率 ≤ 4e-6 | 253 → 228 ms（−10%） |
| crop_gpp_npp（0.5 m） | 4 = 4 | < 1e-5 K | < 1e-5 W/m² | ≤ 6e-5 K | aeresist 55 → 4.6 ms，其余持平 |

没有任何场景变慢或变差（远低于 0.1 K / 0.25 W/m² 的要求）。逐位差异只有浮点舍入量级：逐体元相对差 99 分位 < 2.4e-7，99.99 分位 < 4e-5，个别体元最大 5e-4（跳空后 DDA 的 `tMax` 重新计算）。

## 6. NaN 风险核查（`sampling.glsl` 余弦采样）

RayTracing 的 NaN 样本来自 `sampleHemisphereCosine` 对微小负数开根号（`RT_SAFE_COSINE_SAMPLING` 修复）。体元侧：该函数只被 `SampleBSDF` 调用，`voxelrt/`、`voxeleb/` 的全部 .comp 虽然 include 了 `bsdf.glsl`，但没有任何调用。用 glslc 对 4 个相关着色器（VoxelRT solar/image、VoxelEB solar/image）在定义与不定义该宏时各编译一次，SPIR-V 逐字节相同，证明它是死代码，体元路径没有该风险，也无需实测计数。旋转代码里的 `sqrt(1−u1)`、`sqrt(u1)` 参数来自 24 位均匀数（最大 1−2⁻²⁴），不会为负。为防以后在体元着色器里调用采样器，这 4 个着色器已定义该宏（SPIR-V 不变）。

## 6b. 两级体元（`fixscale`）可行性分析：结论 no-go（2026-11-26）

设想：`setting.scale` 为细格边长，新增 `setting.fixscale` = k·scale（k = 2/4/8）为粗格边长，粗格只用在 k³ 块内均质的区域，表面与异质处保留细格；k = 1（缺省）即关闭。先做合并潜力统计，未改任何着色器，缺省输出与基线逐位相同。

**统计工具**：`src/base/voxelmerge_stats.h`，在体元化之后由 `voxeleb.cpp` / `voxelrt.cpp` 调用。`HISTREAM_VOXEL_MERGE_STATS=1` 打印 `VOXEL_MERGE*` 表并继续运行，`=only` 打印后立即退出；不设变量时无作用。

**可合并的条件（严格）**：对齐的 k³ 块内全部细格被占据；类型、光谱、热、冠层、生化、气动编号及 HEX 模型相同；密度（HEX rho、几何叶密度或冠层密度）与三轴聚集指数的相对离散 (max−min)/mean ≤ 容差。只有浊介质（冠层 `structureType` 0，雾/火焰 2/3）可合并；刚性植被、土壤、建筑、水体保持表面细格。

**实测（体元总数减少 %：浊介质体元减少 %）**：

| 场景 | 体元数 | 浊介质占比 | k=2 严格 | k=4、k=8 |
|---|---:|---:|---:|---:|
| forest 0.5 m（VoxelEB） | 106,822 | 48% | 1.7 / 3.5 | 0 / 0 |
| urban_block 2 m（VoxelRT） | 65,664 | 11% | 3.4 / 32.2 | 0 / 0 |
| crop_gpp_npp 0.5 m | 3,472 | 54% | 0.8 / 1.5 | 0 / 0 |
| 雨林 EB 5 m | 3,101 | 87% | 22.3 / 25.7 | 0 / 0 |
| 雨林 RT 5 m | 3,327 | 23% | 0.2 / 0.9 | 0 / 0 |

- 容差 0、5%、10%、20% 的结果相同（仅 forest 按几何叶密度时例外）：决定因素是占据率而不是异质性，树冠是薄壳，k³ 块全被同种介质占满的很少。
- 体元数的大头是表面：forest 土壤 + 建筑占 52%，urban 占 89%，它们必须保持细格。k=2 即使理想合并，forest 最多减 42%、urban 最多减 9%。
- forest 辐射场显存 80.7 → 79.3 MB（k=2 严格），代理三角形 310,488 → 299,694；单体元约 792 B（VoxelEB 6 波段），`VoxelLink` 48 B。耗时取决于接收体元数 × 射线数，减 1.7% 体元对耗时几乎没有影响（未实测，为估计）。
- 仅雨林 EB 超过 20%，但那是 3101 个体元的小场景，k=2 对应 10 m 粗格。

**宽松规则（占据率 ≥50% 即合并）**：forest k=2 减 19.3%、k=4 减 5.9%，crop k=2 减 32.7%、k=4 减 27.0%，urban k=2 减 6.7%。它会把缺失的细格并入粗格，改变几何与光学体积，不满足“几何保持”；要成立需按叶面积守恒调整粗格密度与三轴遮挡（与现有异质性体元同一思路），最终产品误差未测。

**数据模型设计（未实现）**：粗格一个 `bufferId`、一个 `VoxelLink`（`voxelId` 为块最小角），其 k³ 个细 NanoVDB 格都映射到该 id，`setting.scale` 仍为细格边长。逐细格 DDA 积分（`TurbidBoxSegmentOpticalDepth`、`TurbidCellExtinction`）因消光按细格计算而保持精确；aeresist 列求和仍按细格计一次；周期平铺要求 `voxelSize.x/z` 能被 k 整除。需要改动的：代理面按粗格尺寸生成（含世界对齐 OBJ 代理）；diffuse/TIR/solar/scattering/aeresist 中接收点与射线起点取块中心；浊介质 `1.7320508` 体对角线钳位按 k 缩放；biochem/updateL 的高度用法；输出记录与类别均值权重（k³）；id 压缩。流体耦合与 HEX 模型 5 在启用时排除。合并遍历在主机侧体素化后、NanoVDB 句柄定稿前（`scene.cpp`）。

**结论**：严格合并在真实场景中减少 1%–3% 体元，收益小于 20% 的启动门槛，没有实现 `fixscale`。需要放宽精度时，现有办法是增大 `scale` 并开启异质性体元。若以后要做放宽版，应限定在 forest/crop，以“类别均值温度 ≤ 0.1 K、辐射 ≤ 0.25 W m⁻²”为最终产品验收，先在主机端合并并比较结果，再决定是否改着色器。

## 7. 辐射缓冲布局、规模上限与 fp16 / 64 位索引评估（2026-11-26）

### 7.0 已实施（2026-11-26，逐位不变）：VoxelRT 去掉方向暂存槽，VoxelEB 辐射缓冲 64 位寻址，规模上限改为明确报错

下面 7.1 以后是改动前的分析（保留作依据）；本节是实施结果。fp16 与多缓冲拆分**没有做**。

1. **VoxelRT：每体元 560 → 48 B（n_wave=3，−91%）。** `voxelRads` 的每体元槽数由 `64 + 2·n_wave` 降为 `2·n_wave`。64 个方向暂存槽原先只被“逐波段旧路径” `voxel_raytracing_diffuse` 使用（本线程独占的 atomicAdd）；现在每个方向追踪结束时立刻按求积权重并入本组的 Kahan 和（方向升序，与旧版“先写完 64 槽再升序求和”的顺序相同），不再占用显存。改动点：`voxelrt/voxelrad_diffuse_single.comp`（`voxelRadiationIndex`、`voxel_raytracing_diffuse` 新增 `cosineSum0 / leafProjectionSum0 / out iterationRadiance` 参数）、`voxelrt/voxelrad_image.comp:87-91`、`src/voxelrt/buffer.cpp`（步长）、`src/voxelrt/voxelrt.cpp`（`voxelrt.bin` 回读偏移）。
   两个保持逐位相同的细节：被加数必须先舍入成 float 再相加（`roundBarrier`：经一个编译器看不见的 0——1D 派发时 `gl_WorkGroupID.y` 恒为 0——绕一圈，否则编译器把 radiance 最后一个乘法与加法融合成 fma，相差约 1 ulp；改用共享内存 atomicAdd 同样逐位相同但慢 29%，线程私有数组慢 65%）；被跳过的方向（固体接收体的背面）对 Kahan 和加 0 而不是 `0×权重`（后者在编译后相差 1 ulp，原因未查清，经验上以加字面量 0 为准）。耗时：urban_block VoxelRT 漫射 234 → 236 ms（+1%）。
2. **VoxelEB：辐射缓冲 64 位寻址。** 方向场、天空缝隙透过率缓存、图像环境槽的元素下标原先是 32 位 `uint`：`N×96 ≥ 2³²`（N ≥ 44.7 M）时天空缓存静默回绕、写坏方向场；`N×(96+n_wave/2) ≥ 2³²`（n_wave=6：43.4 M）时图像环境槽被**静默丢弃**（图像退回单次散射，不报错）。现在全部用 `uint64_t` 计算并经物理地址访问（`shader/voxel_radindex.glsl`：GLSL 不允许 64 位数组下标，所以 `VOXEL_RAD(i)` 展开为 `VoxelRadianceBuffer(setting.radianceAddress + i*8).values[0]`），去掉了 `buffer.cpp` 里 32 位守卫与丢弃逻辑。只是地址运算，数值逐位不变；耗时：forest 单次 TIR 24.5 → 24.5 ms，短波散射阶 30.5 → 31.0 ms（+2%）。
3. **规模上限改为带体元数与上限的明确错误**（`src/base/voxellimits.h`）：VoxelRT 的辐射 SSBO（旧版超过 4 GiB 时静默截断/越界）与 `VoxelLink`/`VoxelDir`/`VoxelNetRad`/`VoxelTempe`；VoxelEB 的 `VoxelLink`、`VoxelDir`、`VoxelTempe`、`VoxelNetRad`、`VoxelPnet`、`VoxelRaa`、`VoxelRss`、`VoxelAir`、`VoxelHeatflux`、`TLAST`（4 GiB / 64 B = 67.1 M）与土壤惯性历史；1D 派发组数（`maxComputeWorkGroupCount[0]`，本机 2³¹−1，规范下限 65535 即 4.19 M 体元）；VoxelEB 辐射缓冲分配失败时报体元数、字节数和每体元字节数；散射统计的 32 位计数器（67.1 M）。例（真实运行）：`VoxelRT: 4,000,000 voxels need 6,400,000,000 bytes for the radiance (voxelRads) buffer (1600 B per voxel), above this GPU's maxStorageBufferRange of 4,294,967,295 bytes (at most 2,684,354 voxels with this buffer). Reduce the scene size or use a larger voxel edge.`
4. **新的规模上限（本机，n_wave=3 的 VoxelRT / n_wave=6 的 VoxelEB）。** VoxelRT：7.67 M → 89.5 M 体元（受 `VoxelLink` 48 B 的 4 GiB 范围限制；n_wave=10：26.8 M，n_wave=100：2.7 M）。VoxelEB：43.4 M（静默）→ 约 67 M（`TLAST` 4 GiB 范围，明确报错），之后是显存。**几何代理面是另一个上限**：每个体元的代理面顶点不共享（实体表面体元 6 个顶点 ×44 B = 264 B），一个网格的顶点缓冲超过 4 GiB 时（平地土壤约 16 M 体元可以，20 M 体元进程崩溃，无报错）失败，本次未处理，见本节末尾“仍未解决”。

验证（RTX PRO 6000，GPU 被他人占用时的结果）：

| 检查 | 结果 |
|---|---|
| forest、forest+周期 8、forest+雾/火焰、urban_block（含加速求解器）、rainforest 5 m（EB 与 RT）、crop_gpp_npp；默认 / `SKIP=off` / `DIRROT=rot` 三种开关 | 24 个组合的全部输出文件（TIFF、`voxelrt.bin`、各 `.bin`）SHA-256 与改动前的引擎逐位相同 |
| `DIRROT=legacy` + `SKIP=off` | forest 6/6、urban_block 2/2 与**最初的基线可执行文件**逐位相同 |
| VoxelRT 平地土壤 3200×3200 m、1 m 体元（10.2 M 体元，超过旧上限 7.67 M，n_wave=3） | 旧引擎静默给出错误结果（660 nm 反射率 0.1265、10.5 µm 亮温 302.73 K）；新引擎 0.16301 / 0.20455 / 303.574 K，与 64×64 m 小场景逐位一致（均为 0.163007 / 0.204549 / 303.5744）；漫射 236 ms |
| VoxelRT 4000×4000 m（16 M 体元） | 0.163007 / 0.204549 / 303.5744 K，漫射 368 ms，墙钟 8 s |
| VoxelEB 虚拟 50 M 与 70 M 体元（测试开关 `HISTREAM_VOXEL_TEST_RADIANCE_VOXELS=N`：辐射缓冲按 N 个体元布局，天空缓存与图像环境槽的偏移超过 2³² 条目；分配 39.6 GB 与 55.4 GB） | forest 的全部输出与正常运行逐位相同 |
| VoxelEB 4 M 体元、800 个波段（辐射缓冲 15.9 GB） | 正常完成，墙钟 5.8 s |
| 守卫：`tools/test-voxel-limits.cpp`（目标 `histream_voxel_limits_tests`，读本机设备限制，不分配显存） | 恰在上限不报错、超 1 个体元报错且信息含体元数与上限：全部通过；整机运行时的 VoxelRT 100 波段 4 M 体元与 VoxelEB 辐射缓冲 119 GB 分配失败（测试开关）均给出明确错误并以状态 3 退出 |

**仍未解决：** 平地土壤约 20 M 体元以上（单个网格顶点缓冲 > 4 GiB）在建几何/BLAS 时进程崩溃（访问冲突，无报错）；VoxelRT 与 VoxelEB 共用该几何路径。需要共享顶点或紧凑顶点格式（每顶点 12 B 而非 44 B）才能越过；这是 7.1 候选改动第 5 项，未做。
### 7.1 改动前的布局与分析（保留作依据）

**VoxelEB（每体元字节，n_wave = 6）。** 辐射缓冲 `voxelRads`（经 `setting.radianceAddress` 物理地址访问，`buffer_reference`，不受 4 GiB 描述符范围限制）= 方向场 64×`VoxelRad`（8 B）= 512 B + 天空缝隙透过率 64×float = 256 B（紧跟在场之后，每个 `VoxelRad` 条目存 2 个方向）+ 图像环境系数 n_wave×float = 4·n_wave B → 792 B（实测：forest 106,822 体元 = 84,603,024 B）。其余逐体元缓冲（描述符绑定，各自受 4 GiB 范围限制）：`VoxelLink` 48、`VoxelDir` 8、`VoxelTempe` 12、`VoxelNetRad` 16、`VoxelPnet` 8、`VoxelRaa` 4、`VoxelRss` 8、`VoxelAir` 36、`VoxelHeatflux` 40、`TLAST` 8 层×8 = 64（土壤方法 1 另有 12 模态×8 = 96 B）→ 244 B（+96）。合计约 1036 B/体元（1132 B 含土壤记忆）。浊介质体元另有代理面：每体元 6 个三角形，顶点不共享（每顶点 `VertexAttribute` 44 B）→ 864 B 顶点/索引 + BLAS 约 200 B，已与辐射场同量级。

**VoxelRT。** `voxelRads` 是普通 SSBO（`VK_WHOLE_SIZE` 绑定，`voxelrt/descriptor.cpp:123`），每体元 (64 + 2·n_wave)×8 B（n_wave=3：560 B）：前 64 槽是方向暂存区（仅“逐波段旧路径” `voxel_raytracing_diffuse` 用，由本线程自己 atomicAdd 累加、自己读回加权；谱包路径与第 2 阶起不用），后 n_wave 槽是各波段环境辐射，再 n_wave 槽是按阶次的存储。主机端还要建同样大小的全零 `std::vector` 并上传（`voxelrt/buffer.cpp:293-295`），读回时再整块下载（`voxelrt.cpp:380`）。

**限制与本机（RTX PRO 6000 Blackwell）。** `maxStorageBufferRange` = 4,294,967,295（4 GiB − 1）；`maxBufferSize` = 1 TiB；`maxPushConstantsSize` = 256；`maxComputeWorkGroupCount[0]` = 2,147,483,647（1D 派发每组 64 体元，其他 GPU 常为 65535 → 4.19M 体元，见 `voxelrt/command.cpp:64` 的既有 DEFERRED）；`shaderInt64`、`shaderFloat16`、`storageBuffer16BitAccess`、`bufferDeviceAddress` 均支持；设备本地显存堆 94.3 GiB（实测时已被他人任务占用约 27 GiB）。

| 上限 | 公式 | 体元数 |
|---|---|---|
| VoxelRT 辐射 SSBO 4 GiB（**无任何检查，越界即静默截断**） | 4 GiB / (8·(64+2·n_wave)) | n_wave=3：7.67 M；n_wave=10：6.4 M；n_wave=100：2.0 M |
| VoxelEB 天空缝隙透过率索引 `N×96 < 2³²`（`voxelrad_scattering_VNIR.comp:641` 的 uint；`buffer.cpp:650` 的守卫只管图像环境槽，天空缓存未受保护） | 2³² / 96 | 44.7 M |
| VoxelEB 图像环境槽索引（超限时静默放弃该槽，成像退化为单次散射，不报错） | 2³² / (96 + n_wave/2) | n_wave=6：43.4 M |
| VoxelEB `TLAST` SSBO 4 GiB | 4 GiB / 64 B | 67 M |
| VoxelEB 方向场索引 `N×64 < 2³²`；`command.cpp:563` 的散射统计守卫 | 2³² / 64 | 67 M |
| VoxelEB `VoxelLink` / `VoxelHeatflux` / `VoxelAir` SSBO | 4 GiB / 48 / 40 / 36 | 89 M / 107 M / 119 M |
| 显存 94 GiB，无他人占用 | 94 GiB / (1036 B + 浊介质占比 × ~1.06 kB) | 约 60–90 M（浊介质占比 0.5–0） |

即本机现状：VoxelEB 约 **43 M 体元**（天空缝隙索引先到顶，且在 43–67 M 之间会静默越界而不是报错）；VoxelRT 约 **7.7 M 体元**（n_wave=3）。24 GB 显卡上 VoxelEB 的显存上限约 12–15 M 体元。

**候选改动与估计。**

1. **VoxelRT 去掉 64 槽方向暂存区（建议做）。** 旧路径改为线程内 `float incoming[64]`（本线程自己累加，顺序与现在的 atomicAdd 相同，预期逐位相同）；每体元 560 → 48 B（n_wave=3，−91%），4 GiB 上限 7.7 M → 89 M 体元；谱包路径与第 2 阶起本来就不用。改动点：`voxelrt/buffer.cpp:282-295`（步长）、`voxelrt.cpp:351-410`（读回偏移）、`voxelrad_diffuse_single.comp` 的 `voxelRadiationIndex` 与 `voxelrad_image.comp:84-88` 的 `voxelBandRadiationIndex`（步长公式三处同步）。风险低，可用 SHA-256 与基线核对。
2. **VoxelEB 64 位索引（建议做，逐位相同）。** `voxelRads[voxel*64 + angle]` 改为以 `setting.radianceAddress + uint64(voxel)*stride` 为基址或把下标换成 `uint64_t`（`shaderInt64` 在 NVIDIA/AMD/Intel 近年卡上都有；Vulkan 1.2 要求并不保证，需在建设备时检查）。去掉 43 M/44.7 M 两个静默上限，转而受 `TLAST`（67 M，可同样改 `buffer_reference`）与显存限制。成本：每次访问一次 64 位乘加，相对射线遍历可忽略；约 45 处访问（scattering、TIR、image_environment、voxelrad_image 各一处）要同步改。同时把缺失的守卫补上（越界直接报错，不静默）。
3. **VoxelEB 方向场与天空缓存存 fp16（有条件）。** 场 64×(cumulated, induced) 各 half → 256 B，天空缝隙 64×half → 128 B，图像环境系数保持 fp32：792 → 408 B（−48%），整体 1036 → 652 B/体元（−37%）。累加仍用 fp32：TIR 的 `atomicAdd(voxelRads[own].induced, …)` 实为“本线程独占自己体元”，可改成线程内数组累加后一次写出（不依赖 fp16 原子）；散射阶读邻居的 `induced`（只读）。精度：half 相对误差 4.9e-4（舍入到最近，无偏），64 个方向平均后 ≲1e-4；值域（TIR ≤ ~1100 W/m²；短波按组照度归一后在 0–1.x，下限 6e-5 以下进入非规格数）安全，天空透过率 ≥ 1e-4 截断阈值。会改变数值（不再逐位相同），需做最终产品验证（目标：类均温 < 0.1 K、通量 < 0.25 W/m²，预计远优于）。收益主要在显存：本机（94 GiB）索引上限先到顶，所以 fp16 对本机几乎没有帮助，对 16–24 GB 显卡是 +30% 左右的体元数（含代理面时：24 GB 约 12.7 M → 16.9 M）。若与第 2 项合用，本机上限由 43 M 提到 ~67 M（`TLAST` 4 GiB 上限；显存约 44 GB + 代理面）。
4. **多缓冲拆分（不建议）。** `buffer_reference` 单缓冲可到 1 TiB，拆分只增加代码；VoxelRT 若不愿做第 1 项，可把 `voxelRads` 也改成 `buffer_reference`（与 VoxelEB 相同，需 64 位索引），上限由 4 GiB 改为显存。
5. **代理面压缩（顺带）。** 浊介质体元的 864 B 顶点/索引是 VoxelEB 显存的另一半：顶点只需位置（12 B 而非 44 B）并共享顶点（每体元 12 个而非 18 个）→ 约 −75%；需要确认 `GetShadeState` 不读 nrm/color/texCoord，涉及 `voxeldesigner.cpp`、`accelstruct.cpp` 的 `vertexStride`；风险中等。**（2026-11-26 已做仅位置格式，未做顶点共享，输出逐位相同，缺省开启；见 `docs/voxel-proxy-geometry.md`，平坦场景面片上限约 1626 万 → 约 4000 万，转为单个 BLAS 的分配上限。）**

**建议（go/no-go）。** 第 1 项 GO（VoxelRT 规模 ×11，风险低，逐位可验证）；第 2 项 GO（VoxelEB 取消 43 M 静默上限，逐位相同，并补报错守卫）；第 3 项在 24 GB 以下显卡成为瓶颈时再做（数值会变，需最终产品验证）；第 4 项不做；第 5 项作为显存次优先。第 1、2 项合计工作量约各半天到一天；本节所有上限均为从代码和设备限制推算的估计，未做满载实测。

## 8. 局限

- HDDA 单测只覆盖 forest、forest_big、urban_block；周期边界下的跳空依赖折返坐标，改动周期逻辑时需重测。
- rot 没有 VoxelEB 短波散射的支持；热红外之外的 VoxelEB 辐射仍用固定格点。
- 计时在被占满的 GPU 上取得，只可作量级参考。

- 规模：VoxelEB 约 67 M 体元（`TLAST` 4 GiB 范围，明确报错），VoxelRT 约 89 M（n_wave=3）；但几何代理面的单网格顶点缓冲超过 4 GiB（平地土壤约 20 M 体元）时进程崩溃，当前实际能运行的最大场景由它决定（实测 16 M 体元可以）。`TLAST` 与 `VoxelLink` 改成 `buffer_reference` 可以越过 4 GiB 范围，未做。