# 下一版本更新说明（草稿，测试中）

状态：开发版测试阶段，**未发布**。版本号仍为 2.0.1，没有构建正式安装包，也没有创建新的交付目录。本文件记录待发布的变更，发布时再定版本号并改写为正式发布说明。

## 变更内容

**场景动画剥离到 STREAM IMAGING（2026-11-16）。** 场景动画研究功能（HiStream `eSceneAnimation`：撞击 impact-v1、火球/烟尘输运、刚体碎片、相机观测、耦合小镇与机场全天等）不再属于 Stream Studio，整体迁到 `C:/work/streamimaging`：

- 引擎代码（`models/histream/src/animation`、`shader/animation`、`cmake/scene-animation.cmake`、C++ 夹具）改为 STREAM IMAGING 的 HiStream 外挂扩展 `engine/scene-animation`，由其 `scripts/build-animation-engine.ps1` 编进本仓库的 HiStream 核心，输出到 STREAM IMAGING 自己的构建目录，不覆盖 `models/bin_x64`。
- 工具链 `tools/scene-animation`、示例 `assets/examples/impact_v1_building`、开发文档与研究计划、用户手册第 18 章与理论手册第 17 章随之迁出。
- 已隐藏的桌面面板、`/api/scene-animation/*` 服务、schema 中的场景动画导出、示例目录的 `eSceneAnimation` 条目、相关翻译和桌面设置键均已删除；代码存档在 STREAM IMAGING 的 `animation/studio-panel/`。

**HiStream 通用扩展接口。** 核心不再知道任何具体扩展：`src/base/extension.h` 提供模式注册表，CMake 选项 `HISTREAM_EXTENSION_DIRS` 列出外部扩展目录（各含 `histream-extension.cmake`），配置时生成注册函数。产品构建不带扩展，`histream.exe eSceneAnimation …` 返回用法提示（退出码 64；2026-11-20 起用法错误与 FacetEB 未收敛的退出码 2 分开）。`HISTREAM_ENABLE_SCENE_ANIMATION` 选项已删除。

**之前的草稿内容**（场景动画面板、impact-v1、渲染提速选项、内置示例）随功能迁出，原文存档在 STREAM IMAGING `docs/scene-animation/history/streamstudio-release-draft-20261112.md`。

### 模型修正（2026-11-20）

以下修正会改变计算结果，旧结果不能与新版逐值比较。方法见理论手册与 `models/stream/README.md`，修正清单见 `docs/model-remaining-issues-20261005.md` 顶部。

- **VoxelRT 反射率归一**：参考辐照度为 E_dir·cosθs + E_diff（天空不再乘 0.5），固体表面反射环境项去掉 0.5；固体接收面按外法线半球余弦加权，步数上限不再补天空。
- **烟/雾亮度**（VoxelRT、VoxelEB）：单次散射项按每球面度 HG 相函数计 π·p·E_b，环境项取 ½，介质光学亮度明显升高。
- **FacetEB 直散比**：取光照设置的 `directScatteringRatio`（此前存在 Esun/Esky 文件时被文件比例覆盖，约 0.806）；低太阳或封顶丢弃的直射计入漫射。
- **FacetRT 光照**：与 FacetEB 共用 Esun/Esky 光谱形状和直散比（总量仍取 700·cosθs + 50 W/m² 的历史值），不再固定 700/50。
- **Hapke 背景能量**：能量平衡改用守恒约定（Lambert 底值作白空反照率，直射按 A(μ0)/A_bh 保留太阳角变化），长波按 Lambert 半球发射率；影像改为天顶观测方向的方向反射因子/发射率。strength = 0 时不变。
- **STREAM**：亮温加反射天空项 (1−Σw)·B(T_sky)（`skyLongwave` 可设 `{"mode":"none"}` 恢复旧行为）；BSM 湿土改为 SCOPE soilwat（湿土短波红外反射率降低约 10–20%）；夜间及局地背光坡光照组分并入阴影；地形短波先水平分直散再投影；冠层上方阻抗 rar 括号位置更正（北京 Landsat 示例收敛像元 180326→180187）；地类 0/16 无气孔、水体按水面地面。
- **Facet 可见性 A-buffer 自动扩层**（不影响数值）：面元场景溢出时自动把片元层数由 64 加倍到 128/256 并重建资源，不再需要手工设置 `HISTREAM_FACET_RASTER_LAYERS`；crop_gpp_npp 等稠密冠层示例可在默认设置下运行。上限仍为 256 层与显存预算（`HISTREAM_FACET_RASTER_BUDGET_MB`）。
- **示例副本同名覆盖**（界面行为）：再次打开同一内置示例时覆盖 `<示例名>` 副本的工程文件并复用同一目录，副本 `output/` 中的历史结果保留；此前会累积 `forest_2`、`forest_3` 等重复副本。打开日志与复制对话框说明同步更新。
- **VoxelEB 夜间跳过短波**（不影响数值）：太阳在地平线下且实测短波为零的时间节点，先清零短波状态再跳过可见性、直射与散射阶数。forest 示例全天 5.5 min → 4.2 min，输出逐字节不变；日志新增 `SHORTWAVE_SKIPPED` 行。

### 精度修正（2026-11-22，改变数值）

- **反射率不再截断到 1**：RayTracing、VoxelRT、VoxelEB 成像，FacetRT 的 GPU/CPU 光谱输出和 FacetEB 状态输出只剔除 NaN/Inf 与负值。朝阳坡面、热点、白墙、水面镜面等 BRF > 1 的像元不再被压低；星下点常规场景数值不变。显示拉伸不受影响。
- **RayTracing 散射深度**：新增独立参数 `control.raytracingDepth`（默认 16，范围 1–64），不再与 VoxelRT 的步数参数 `depth` 共用；第 3 次散射起按俄罗斯轮盘赌无偏终止（存活概率 = clamp(最大通量, 0.05, 1)）；周期边界穿越不再占用散射次数。原先实际截断在 4 阶，植被 842/860 nm 均值升高约 0.4%–1.5%，p99 升高 1.4%–5.7%，可见光基本不变。
- **RayTracing 阴影射线**：首次命中即终止，起点偏移由 11 mm 降为 1 mm，近距离遮挡不再漏光。
- **VoxelEB 地面自由对流**：土壤、水体、建筑表面温度高于气温时，并联加入 Kondo–Ishida/TSEB 自由对流导度 ≈ 0.0025·ΔT^(1/3) m/s。forest/urban 示例正午土壤显热由 7–14 W/m² 升到 30–66 W/m²，G/Rn 由 0.87–0.92 降到 0.76–0.84。完整 Monin–Obukhov 稳定度迭代见下条（可选）。
- **FacetEB 收敛判据更严**：在“残差 ≥ 2 W/m² 的面元 < 0.1%”之外，要求任一面元侧的温度等效残差 |R|/(dR/dT) ≤ 0.5 K。此前作物示例有节点在最大残差 43.8 W/m² 时被判为收敛，现在如实标为未收敛。
- **天空盒关闭时的场景边缘**（RayTracing、VoxelRT、VoxelEB）：射出场景的样本不再被整体丢弃或按 0 计入，像元值按“场景覆盖率”加权平均；完全无覆盖的像元仍由主机端填天空值。街景透视示例内部像元不变，500–630 个边缘像元变亮（VoxelRT 边缘 860 nm 均值 0.053→0.246）。
- **Facet 周期边界下的可见性分辨率**：投影窗口 x/y 收紧到中心块外接球（深度方向仍覆盖全部副本），GPU/CPU 后端一致。周期 8 的作物示例辐射度偏差由 +16% 降到约 +2%，反射率 > 1 的伪像元由 36 个降到 0；代价是可见性边数增加约 5 倍，周期 20 的城市 FacetEB 单节点 8.7 s → 11.8 s。
- **PROSPECT 指数积分 E1**：原 9.9 万步矩形积分丢掉了尾项（x = 0.01 时误差约 5%），改为级数加连分式（相对误差约 1e-15）。叶片近红外反射率变化约 1e-4。
- **VoxelRT/VoxelEB 叶倾角分布、G 与 Γ 自洽**：此前消光只用常数 G，叶片散射相函数固定为球形闭式，Kuusk 热点写死 G = 0.5，`LIDFa/LIDFb` 不参与计算；G ≠ 0.5 时单次散射被放大/缩小 0.5/G 倍。现在主机端由 `LIDFa/LIDFb`（SAIL 13 区间）求叶倾角频率传入 GPU，消光、太阳可见性、视线截获、直射投影和热点都用对应方向的 G(Ω)，Γ 按 SAIL `volscatt` 精确积分并满足 (1/π)∫Γ dΩ = (ρ+τ)G；植被接收体的漫射入射按 G(Ω) 加权。刚体/介质、`LIDFa = LIDFb = 0` 或超出定义域时回退为常数 G，Γ 按 G/0.5 归一；`LIDFa > 1` 为精确球形（G = 0.5，与修正前一致，相对差 ≤ 5e-7）。冻结算例（默认 −0.35/−0.15）：vrt_forest 842 nm 0.19240→0.19190（−0.3%）、668 nm +0.2%、亮温 +0.01 K；veb_forest 842 nm −0.13%，植被温度 −0.09 K、植被 Rn −0.9%；城市算例 ≤ 0.03%。平面型 LIDF (1, 0) 的森林 842 nm 比修正前（LIDF 被忽略）高 11.9%、668 nm 低 7%；G = 0.3 回退算例 842 nm −3.2%、668 nm +3.4%、亮温 +0.38 K（原热点式按 G = 0.5 计算，阳照背景被低估）。FacetRT/FacetEB/RayTracing 逐位不变。
- **周期边界覆盖全部传输射线**（RayTracing、VoxelRT、VoxelEB）：此前只有相机/成像射线跨边界，阴影、天空、散射射线和体元太阳/漫射/散射/长波传输在边界处直接逃逸，边界条带缺少邻域遮挡与多次散射。现在所有射线按射线顺序遍历域副本（每个副本只在射线位于其中的区间查询一次，命中点直接落在中心域坐标），每段自由程最多穿越 `periodicTraversalCount` 次，不占散射次数；RayTracing 阴影射线每个副本内仍首次命中即终止；体元浊介质出射积分按周期环绕取表。高于场景顶且不下行的射线直接逃逸，相机射线从场景顶开始计数，高处相机不再把穿越次数耗在空域里。验证（森林 25 m 规则格网算例，周期 8）：边缘格与中心格的差异 VoxelRT 2.1% → ≤ 0.05%，VoxelEB 冠层净辐射 −14.8% → +0.01%、冠层温度 −2.4 K → 0.00 K，RayTracing 0.96% → ≤ 0.14%（噪声水平）。开周期的示例变化：森林（周期 8）VoxelRT 668 nm −4.3%、VoxelEB 475 nm −4.3%、RayTracing 842 nm +0.45%；城市（周期 20）VoxelRT 星下点 −0.8%、VZA 75° 斜视 +17%，VoxelEB 亮温 +0.2 K，RayTracing ≤ 0.12%。未开周期的工程：VoxelRT/VoxelEB/Facet 逐位不变；RayTracing 约 3% 像元因着色器重新编译后的舍入差使路径分叉而不同，图像均值相对差 ≤ 5e-5（p99 ≤ 2.3e-4）。
- **中波红外计入太阳反射**（RayTracing、VoxelRT、VoxelEB）：此前 > 2.5 µm 的热红外波段只计 Planck 发射与天空长波，白天 3–5 µm 亮温偏低。现在 2.5–6 µm 波段在发射之外加入太阳直射与天空散射短波的反射，写法与光学分支相同（同一太阳可见性/热点，植被 Γ/G、固体 BRDF、水体 Cox–Munk；RayTracing 每个路径顶点的下一事件估计，VoxelRT 进入漫射场与多次散射，VoxelEB 成像单次散射）。光谱形状取内置 `defined/Esun_.dat` 2500–6000 nm 段除以其 400–2400 nm 积分（3.8 µm 处 0.0113 µm⁻¹，自然含 2.7/4.3 µm 吸收带，6.3 µm 水汽带处为 0）；绝对量 VoxelEB 用气象 Rin × 直射比例，RayTracing/VoxelRT 用 Haurwitz 晴空总辐照（θs = 30° 时 890 W m⁻²，3.8 µm 法向直射约 9.0 W m⁻² µm⁻¹）；天空散射按 Esky_/Esun_ 在 2.3–2.4 µm 的比值 0.0847 外推（上界）。共享代码 `shader/mwir_solar.glsl`。冻结算例 3.8 µm 白天亮温均值升高：rt_urban +3.76 K、vrt_urban +3.30 K（受光混凝土/土壤 p95 +6.6–7.3 K）、vrt_forest +2.67 K、veb_forest +2.37–2.50 K、veb_urban +3.45–3.53 K；场景平均等效反射率 ΔL/(E·cosθs/π) 森林 0.026–0.029、城市 0.038–0.043，与叶片 TIR 反射率 0.025、城市材料 0.05–0.08（含阴影）一致。同一运行中的 10.5 µm 波段、夜间节点（Rin = 0）的 3.8 µm 波段、全部 22 个旧算例（可见/近红外/长波）逐字节不变；耗时不变。VoxelEB 能量平衡不变（太阳能量已在短波 Rin 中）。FacetRT/FacetEB 尚未计入（见尚存问题）。
- **VoxelEB Monin–Obukhov 稳定度迭代**（`control.stabilityCorrection`，与 FacetEB 共用开关）：此前 VoxelEB 的气动阻抗固定按中性层结（JSON 路径 L = 0，aeresist 从不更新 L）。开启后，能量平衡迭代中按场景平均显热（budget 定点整数累加，每地面面积）与地面柱平均 u*（aeresist 累加）松弛（0.5）更新一个场景 1/L，限幅 −5 ≤ (z−d)/L ≤ 1，相似函数移植自 FacetEB `energyphysics.h`（Paulson/线性）；1/L 每变化一次即重算全部气动阻抗（u*、惯性/粗糙子层、冠层内 K_h 与林下阻抗），节点退出另要求 |Δ(z/L)| ≤ 0.02 + 0.02|z/L|。开启时地面自由对流与 MOST 导度取大而非并联，避免重复计入浮力。关闭时使用与旧版逐字节相同的中性着色器（`aeresist/budget/evapo_most.comp.spv` 由 `-DVOXELEB_MOST` 单独编译），结果逐位不变。冻结算例（开/关）：正午 z/L 收敛到 −4.9（u = 0.43–0.50 m/s，u* 0.04–0.07 m/s），叶温下降 2.7–6.2 K（veb_forest 305.4 → 300.2 K，气温 299.7 K）、叶片 LE +33–69%；土壤 H 与 G/Rn 基本不变（forest 0.84/0.77，urban 0.81/0.77）；夜间（新增 `veb_forest_night`、`veb_urban_night`，01:50/02:20，初始地温 10 °C）z/L 收敛到 +1，u* 降至 0.007–0.04 m/s，土壤/叶片显热绝对值降约 3 倍；迭代 7–27 次、全部节点残差达标、1/L 单调收敛无振荡；耗时 −10% ~ +6%；重复运行逐位一致。`ENERGY_BALANCE` 日志行与 `energy_balance_node=*.json` 新增 1/L、z/L、u*、场景 H、阻抗重算次数，运行记录显示 z/L 与 u*。
- **晴/阴两态热历史面积守恒**（FacetEB 全部表面，VoxelEB 土壤方法 1 的土壤与建筑）：每个面元侧/体元的受光与阴影部分各自保存热历史（结构面元表面温度、方法 1 的 8 节点表面温度序列、FacetEB 方法 2 的 12 层土柱）。此前受光比例 p 在相邻节点间变化时，新进入阴影的面积沿用"一直在阴影中"的冷历史，其储热凭空消失（阴影离开时反之）。现在每个节点开始时按面积守恒转移：p 减小时把 p_old−p_new 份额的晴态历史并入阴态（面积加权平均），p 增大时反之，Σ p·C·T 不变；p 不变的面元不做任何改动。VoxelEB 在体元温度记录中新增上次写历史时的阳照比例（`VoxelTempe.solarFraction`，结构体 8 → 12 字节），`evapo.comp` 用转移后的历史计算储热通量、`updateTp.comp` 在节点末提交；VoxelEB 方法 2 与水体本来就是单一混合状态，不受影响。实测（城市示例 08:50–13:20 共 10 个 30 min 节点，新增 `feb_urban_shadow`、`feb_urban_shadow_m1`、`veb_urban_shadow_m1`）旧做法的误差：FacetEB 阴影到达（p 一个节点内降幅 > 0.5）的地面面元当节点地表温度平均偏低 7.1 K（方法 2）/ 7.8 K（方法 1），最大 18–20 K，其后 1、2 个节点仍偏低约 5 K、2.7 K，G 多算 70–77 W m⁻²；建筑面元偏低 1.4 K（最大 4–6 K）、叶片 0.25 K；VoxelEB 方法 1 土壤体元偏低 6.6 K（最大 14 K）、建筑体元 1.7 K（最大 9.7 K），G 多算 97 / 32 W m⁻²。FacetEB 新增逐节点诊断 `facet_physics_node=*.json` → `thermalHistory`（转移面元侧数、旧做法会产生的热量跳变 J、热含量闭合残差 W m⁻²）：旧做法土壤（方法 2）面积平均闭合残差 5–42 W m⁻²（单面元最大约 3000 W m⁻²）、结构面元 1–7 W m⁻²，修正后均为 0。独立参考 `HISTREAM_THERMAL_HISTORY=mixed`（每节点合并为单一面积加权历史）与修正结果在阴影到达面元上平均差 0.003 K、全场最大 0.4 K；`HISTREAM_THERMAL_HISTORY=legacy` 复现旧结果（与旧引擎逐字节相同），仅供对照。旧算例：单节点（feb_urban 等）、夜间、全部 VoxelEB 方法 2 城市算例逐字节不变；feb_crop 第 2 节点叶片在阴影到达处平均 −0.03 K、全场最大 2.2 K，地面 rms 0.29 K；veb_forest 系列第 2 节点树干/枝（材料方法 1，体元类型为建筑）最大 1.8 K、类型均值 ≤ 0.006 K，土壤/叶片间接变化 ≤ 0.25 K。耗时不变。

### 性能（2026-11-22，不改物理）

以下改动经冻结算例对照，结果逐位相同或差异 ≤ 1e-4（VoxelEB 温度阻尼为 ≤ 0.05 K）。同一台机器上各模式总耗时降到原来的 1/1.5–1/7。

- 五种模式启动时都调用的 PROSPECT 准备由约 10 s 降到毫秒级（E1 改写、系数按引用传递）。
- VoxelEB：未碰撞天空场只追踪一次、各光谱组按天空辐照度缩放（短波散射约 1.8×）；温度阻尼随迭代递减（迭代 29–35 → 16–20 次）；气动阻抗移出迭代循环。辐射缓冲区因此增加约 50%。
- Facet：耦合迭代中长波 Jacobi 从上一轮解热启动；FacetEB“去直射”求解只在末节点做；短波窗口与影像波段按残差提前收敛；窗口光学参数按材质预计算并行化；A-buffer 溢出时直接扩到所需层数。
- VoxelRT：太阳方向不变时多视角/航点只重跑成像（61 视角 14 s → 4.5 s）。
- RayTracing：批次按实测 GPU 耗时自适应切分（约 120 ms/批），消除开周期边界时的低占用和整图一次提交的 TDR 风险。
- 服务端：FacetEB 逐节点出图改为每次运行一个常驻出图线程（超过 300 万面元时仍逐节点开线程，阈值 `STREAM_STUDIO_FACET_STEP_RESIDENT_FACETS`）。

### 新工程默认值（2026-11-22）

- VoxelRT 辐射求解方法默认“光谱批处理加速”（与传统方法均值相同，快 1.1–1.6×）。
- FacetEB 温度松弛系数默认 0.8（原 0.5；作物/城市收敛所需迭代 16–19 → 9–12）。
- 已有工程中显式保存的 `radiationSolver=traditional`、`temperatureRelaxation=0.5` 保持不变。
- **新建 VoxelEB 工程默认开启大气稳定度修正**（`control.stabilityCorrection = true`，界面“RT–EB 耦合 → 大气稳定度”，VoxelEB 现在也显示该下拉框）；FacetEB 与其他模式新工程仍为中性。旧工程缺该字段按中性处理，显示与写出均以 `=== true` 判断；内置示例显式保存为 `false`，结果不变。
- **新工程按“标准”运行质量档**（`control.qualityPreset = 'standard'`），与此前 schema 默认值不同的三项以标准档为准：VoxelRT 真实碰撞次数 2 → 3；VoxelEB 短波散射阶数 1 → 2；VoxelEB 体元射线步数上限 64 → 按场景对角线体元数 × 1.5 自动取值（16 的倍数，64–4096；默认 60×60×15 m / 1 m 场景为 144）。采样数 32、RayTracing 散射次数 16、FacetEB 耦合迭代 60 与原默认相同。标定见用户手册 13.2.1：VoxelEB 2 阶比 1 阶植被吸收误差 4% → 1%、耗时约 +50%。

### 运行质量档位与单次运行上限（2026-11-22）

- **运行质量三档**：仿真控制面板顶部新增“运行质量”（快速预览 / 标准 / 高精度 / 自定义），选择后只把当前模式实际读取的参数批量写入工程：RayTracing `samples`+`raytracingDepth`；VoxelRT `samples`+`voxelCollisionOrders`；VoxelEB `samples`+`shortwaveScatteringOrders`+`radiationMaxSteps`；FacetEB `couplingIterations`；FacetRT 无受管参数。手动修改受管参数后自动记为“自定义”；切换计算模式时按工程记录的档位写入新模式参数；VoxelEB 标准/高精度档的步数上限随场景尺寸、体元大小重算。各档数值由 `tools/regression` 冻结算例（`vrt_urban`、`veb_forest`、`feb_crop`、`rt_forest`，补充 `feb_urban`）以高精度为参考标定，标定表写入用户手册 13.2.1。
- **单次运行上限可在工程内设置**：新增 `control.runTimeoutMinutes`（1–1440 分钟；0/空 = 服务端默认 15 分钟，仍可用 `STREAM_STUDIO_RUN_TIMEOUT_MS` 修改），仿真控制面板顶部可填写。`/api/run` 与 `/api/run/preflight` 使用工程值；预检返回 `timeoutSource`（`project`/`server`）、`serverTimeoutSeconds`，预计超时时返回 `suggestedTimeoutMinutes`（预计 × 1.5 向上取整到分钟），界面弹窗提供“把上限调到建议值并运行 / 按当前上限运行 / 取消”。`/api/defaults` 新增 `runTimeoutSeconds`。
- 修正预检警告中预计耗时只显示单位、不显示数值的问题（服务端 `durationLabel`）。

### 界面（2026-11-22）

- **只显示实际生效的参数**：FacetRT/FacetEB 不再显示追踪深度与采样数；RayTracing 显示“最大散射次数”和“每像元路径数”；VoxelRT 的“追踪深度”改名“体元射线步数预算（×128）”；VoxelEB 新增“体元射线步数上限”（`radiationMaxSteps`）与“短波散射阶数”，隐藏引擎不读的耦合迭代/阈值/松弛；FacetEB 隐藏只记录不参与判据的温度收敛阈值；采样数上限 256 → 1024，与引擎一致。
- **运行前预检**：点击运行时先调用 `POST /api/run/preflight`，日志写出面元数/上限、可见性栅格、体元网格与辐射场显存估计、节点/方向/波段数、按本工程最近 3 次同模式运行估计的耗时和单次运行上限；预计超时、面元接近上限、显存过大等风险弹窗确认。预检失败不阻止运行。
- **运行质量摘要**：解析 `FACET_CONVERGENCE`、`FACET_TIMING`、`ENERGY_BALANCE`、`SHORTWAVE_SCATTERING`（含 `truncations=`），运行结束汇总节点数、未收敛节点、最多迭代、最大温度等效残差、耗时构成与步数截断，有问题时给出调参建议。
- **进度与结果**：进度栏显示当前阶段与按引擎进度估算的剩余时间；运行成功（或 FacetEB 未收敛但结果保留）后自动打开结果窗口；stdout 按行重新拼接，不再丢失被拆开的 PROGRESS 行。
- **体元预览**：按钮此前无效，现在按当前体元大小近似体元化场景对象，显示网格尺寸与占用体元数（体元过多时粗化显示，计数为上限估计）。
- **运行期间视口降帧**：引擎运行时三维视口约 10 fps 渲染，减少与引擎争用 GPU；在视口内拖动、缩放或按键后 1.5 s 内恢复全帧率。
- **FacetEB 自动后端**：设备不支持 shaderFloat64（或缺少 `energy.comp.spv`）时，“自动”不再报错，改为辐射仍用 GPU、能量平衡走 CPU 路径，并在日志中提示；显式选择 GPU 时仍报错并提示改选 CPU 或自动。
- **冠层投影系数 G**：混沌介质的叶倾角参数有效时，“投影系数 G”字段只读并说明 G 由 LIDF 推导；刚体/介质、`LIDFa = LIDFb = 0` 或参数超出定义域时可编辑并照常使用。资源库摘要中 G 显示为 `LIDF (a, b)` 或 `0.5（球形）`。工程中保存的 G 值不变。

### 运行管线（2026-11-22，不改数值）

- **桌面版服务端移出主进程**：`desktop/main.mjs` 用 Electron `utilityProcess.fork` 启动 `desktop/server-process.mjs`，HTTP 服务（含 FacetEB 出图线程）在独立进程中运行，主进程只负责窗口、文件对话框（经进程消息转发）和桌面设置 IPC。运行准备、出图、ENVI 转换、读结果不再冻结窗口。退出时先通知服务进程停止引擎再退出，5 s 未退出则结束整棵进程树；服务进程意外退出时清理它遗留的引擎进程，自动重启并重新载入页面，1 分钟内第 3 次退出则报错关闭。网页模式不变。
- **结果读取缓存**：`/api/results/read` 按 (路径, mtime, size) 缓存原始数据、已解码 TIFF 分块和已编码响应，按字节计的 LRU，默认 512 MB（`STREAM_STUDIO_RESULT_CACHE_MB`，0 为关闭）；响应头 `X-Cache: hit|miss`。彩色合成三个波段共用一次读盘/解码；变化分析和 STREAM 栅格读取也共用该缓存。文件被覆盖即失效，“删除所有结果”清空缓存。
- **三维结果二进制传输**：请求头 `Accept: application/vnd.stream-studio.result-v1` 时，面元/体元结果的几何坐标和逐面元/体元指标以 Float32（不能无损表示时用 Float64）二进制块回传，其余字段仍为 JSON；界面已适配。不带该请求头时仍返回原 JSON。2 万面元测试结果 3.66 MB → 1.92 MB，逐值一致。
- **运行准备不阻塞事件循环**：引擎 exe、工程与气象文件的 SHA-256 改为流式计算并按 (路径, mtime, size) 缓存；运行输入目录（含裁切后的 OBJ）与气象快照改为异步复制；准备期间拒绝第二个运行请求。

## 兼容性

- `tools/build-engine.ps1` 构建的产品引擎只含核心（显式清空 `HISTREAM_EXTENSION_DIRS`），不再含场景动画与 Bullet 碰撞模块，也不再需要本地 Bullet 源码；三维 RT/EB 与 STREAM 遥感工作流的代码路径不变。
- 耦合小镇和机场全天所用的 FacetEB 集中参数热节点（`facetrt/lumpedthermal.h` 等）属于核心，保留；其解析夹具从 `tools/scene-animation/tests/lumped_thermal_fixture.cpp` 移为 `tools/test-facet-lumped-thermal.cpp`（目标 `histream_lumped_thermal_tests`）。
- 三维工程 `schemaVersion = 6`、遥感工程 `schemaVersion = 2` 不变。STREAM IMAGING 运行时仍从本仓库导入 `project-schema.js` 和 `tools/runtime-scene.mjs`。
- HiStream 退出码：64 为命令行用法错误（含不带工程路径运行，原先静默跑内置示例；内置示例须显式 `--builtin-example`），2 只表示 FacetEB 计算结束但有未收敛节点；1/3/4 为运行、资源创建、初始化失败。按旧退出码 2 判断用法错误的脚本需修改。
- `voxelrt.json` 过程字段：2.5–5 µm 波段由 `illumination_<波长>` 改名为 `spectral_radiance_<波长>`（与 > 5 µm 波段一致，按 Planck 辐亮度计），读取这些字段的脚本需改名；`radiosity_<波长>` 别名不变。2.5–6 µm 波段的 `spectral_radiance_<波长>`（环境场）自 MWIR 太阳反射修正起含太阳直射与天空散射的贡献（白天变大，夜间与 > 6 µm 不变），字段名不变。
- 中波红外（2.5–6 µm）白天亮温随 MWIR 太阳反射修正升高（叶片约 3 K，受光土壤/建筑 5–10 K）；依赖旧 MWIR 数值的对比或反演需重跑。RayTracing/VoxelRT 的太阳绝对量由晴空模型给定，与工程里设置的组分温度无关，阴天场景的 MWIR 太阳项会偏大。
- 2026-11-22 精度修正后，旧工程重新运行会有以下数值变化：VoxelEB 地面显热升高；RayTracing 近红外升高；反射率可大于 1；天空背景下物体边缘变亮；开周期边界的 Facet 工程更准但更慢。FacetEB 判据更严，已保存的低迭代上限（如 12 次）可能被报告为未收敛（退出码 2，结果保留）。VoxelRT/VoxelEB 的混沌介质改用 `LIDFa/LIDFb` 决定 G(Ω)：默认参数下变化 < 0.3%，非球形 LIDF 或 G ≠ 0.5 的工程变化可达 10% 量级；想保持旧的常数 G 行为可把 `LIDFa`、`LIDFb` 都设为 0（G = 0.5 时与旧结果相同，G ≠ 0.5 时单次散射按 G/0.5 修正）。`Canopy` GPU 结构体新增 `lidfValid/lidf[13]/diffuseG[32]`（244 字节），C++ 与着色器需同时更新。
- 周期边界传输一致化后，开周期的工程边界条带变暗/变冷到与内部一致，整体均值随之变化（森林周期 8 约 −4%，城市斜视可达 +17%）。耗时（同机、预热后）：VoxelRT 周期 8 森林 6.4 → 7.6 s，VoxelEB 周期 8 森林 10.0 → 14.2 s、周期 20 城市 2.9 → 3.5 s；RayTracing 不变或略快（1.3 s）。未开周期的 VoxelEB 短波散射 pass 也慢了约 1/3（森林 9.9 → 12.0 s，结果逐位不变），原因未查明。`RayRTSetting` 新增 `sceneTop`（占用原对齐填充，仍为 80 字节），C++ 与 `settings.h` 需同时更新。
- 工程新增 `control.raytracingDepth`（RayTracing 散射次数），旧工程缺省按 16 处理；`control.depth` 只对 VoxelRT 有意义（步数预算 ×128）。
- 工程新增 `control.qualityPreset`（`fast`/`standard`/`high`/`custom`）与 `control.runTimeoutMinutes`。旧工程缺少这两项时按 `custom` 与 0（服务端默认上限）处理，已有参数不变；缺少 `voxelCollisionOrders`/`shortwaveScatteringOrders`/`radiationMaxSteps` 的旧工程仍按 2/1/64 运行，只有新工程才按标准档取 3/2/自动。旧版本打开新工程会忽略这两个字段。
- VoxelEB 稳定度迭代：GPU `EBState` 由 4 字节扩为 1544 字节（地面柱计数与 3×128 个定点累加槽，`structs_cg.h` 与 `parameters.h` 的 `VOXELEB_MOST` 分支同步），`FluidParameters.localExchange` 的保留分量 y/z 用于稳定度开关与场景 1/L；新增 3 个 `*_most.comp.spv`（构建清单已登记）。旧 XML 工程中 `<Aerodynamics><L>` 在中性管线中照旧使用；开启稳定度修正时由迭代得到的场景 L 取代。
- 新增环境变量：`HISTREAM_FACET_RASTER_SIZE`（Facet 可见性栅格边长，用于生成高分辨率参考解）、`STREAM_STUDIO_FACET_STEP_RESIDENT_FACETS`（常驻出图线程的面元数阈值，默认 3,000,000）、`STREAM_STUDIO_RESULT_CACHE_MB`（结果读取缓存上限，默认 512）、`STREAM_STUDIO_RESULT_CACHE_DEBUG=1`（服务端打印缓存命中日志）。
- 回归测试工具 `tools/regression/`：从内置示例生成 13 个冻结算例，用两版引擎运行并对比；`compare.py --tolerance` 可作为“纯性能改动不得改变结果”的门禁。新增中波红外算例 `rt_urban_mwir`、`vrt_urban_mwir`、`vrt_forest_mwir`、`veb_forest_mwir`、`veb_urban_mwir`、`veb_urban_mwir_night`（波段 3800/10500 nm，输出亮温）。

- 晴/阴热历史守恒转移后，多节点 FacetEB 和使用土壤方法 1 的多节点 VoxelEB 结果变化（阴影移动处地表、墙面变暖数 K）；单节点、夜间、VoxelEB 方法 2 工程不变。GPU `VoxelTempe` 由 8 字节扩为 12 字节（`solarFraction`，`structs_cg.h` 与 `parameters.h` 同步）。新增诊断环境变量 `HISTREAM_THERMAL_HISTORY`（FacetEB：`remap` 默认 / `legacy` 旧做法 / `mixed` 单一混合历史），仅供对照。回归工具新增 `feb_urban_shadow`、`feb_urban_shadow_m1`、`veb_urban_shadow_m1`；`compare.py` 按元数据读取 VoxelEB 过程记录宽度（方法 1 为 10 个浮点数，原先固定按 18 读取会报错）。

## 已做的检查（2026-11-22）

- 每项性能/精度改动单独编译引擎，在覆盖五种模式的冻结算例上对照；第二轮用 `tools/regression/` 13 个算例对比第一轮引擎：结果变化只出现在周期边界 Facet 算例与街景边缘像元，其余不变或 ≤ 1e-4。
- `tools/test-facet-energy-convergence.cpp` 补充 0.5 K 上限用例并通过。
- Electron 中端到端：五种模式参数列表、体元预览、运行、自动打开结果、运行质量摘要与预检弹窗。
- 未做 SCOPE 对比；VoxelEB 自由对流与 RayTracing 深度会改变既有 SCOPE 指标，发布前如需对比应单独运行。
- 叶倾角/G/Γ 自洽：`tools/test-lidf-gamma.cpp`（cl 单文件编译）全部通过——G 与暴力方位积分及独立 PROSAIL 双精度转写差 < 2e-7（平面、直立、倾斜、极端、均匀、默认与精确球形），⟨G⟩ = 1/2，(1/π)∫Γ_R,T = G 相对差 < 2e-5，连续球形极限与 Ross 闭式差 < 2e-7。16 个冻结算例（含新增 `vrt_forest`、`vrt_forest_planophile`、`vrt_forest_g03`）对比修正前引擎：Facet/RayTracing 输出逐字节相同（仅耗时/时间戳字段不同）；`LIDFa = 2` 的森林 VoxelRT/VoxelEB 与修正前相对差 ≤ 5e-7；新引擎重复运行结果相同，耗时与修正前持平。
- 周期边界传输一致化：`tools/regression/prepare.mjs` 支持用 `positions` 替换示例的位置文件，新增 `rt/vrt/veb_forest_periodic` 与 25 m 规则格网的 `rt/vrt/veb_lattice_periodic` 共 6 个算例（总 22 个）。对比 LIDF 修正后的引擎：未开周期的 VoxelRT/VoxelEB/Facet 算例逐字节相同，RayTracing 仅舍入级路径分叉；格网算例按 16 个格子统计边缘/中心差（数字见上）。
- 运行管线（服务端独立进程、结果缓存、二进制传输）：网页模式接口检查（示例列表、打开示例、RayTracing 运行、同一 TIFF 第二次读取 `X-Cache: hit`、touch 后失效）；二进制与 JSON 逐值一致；桌面模式 CDP 端到端（打开 urban_block → RayTracing → 结果自动打开），进程树确认服务在 utility 进程且 histream 为其子进程；运行中强杀服务进程后自动重启、页面重新载入、无遗留 histream；运行中关闭窗口后无遗留进程；asar 内 utilityProcess + Worker 可用。
- 运行质量档位与单次运行上限：`tools/regression` 用基线引擎 `bin_v0` 对 `vrt_urban`、`veb_forest`、`feb_crop`、`rt_forest`（补充 `feb_urban`）各跑三档并以高精度为参考对比（标定表见用户手册 13.2.1）；网页模式（`STREAM_STUDIO_PORT=4199`，无头 Edge + CDP）实测：旧工程显示“自定义”且参数不变，三档切换后 `project.json` 写入对应参数，手动改采样数切为“自定义”，切换 FacetEB/RayTracing/VoxelRT 按档位写入新模式参数，修改体元大小时 VoxelEB 步数随之重算；工程上限设 1 分钟时预检返回 60 s（`timeoutSource=project`）与建议 10 分钟，选择“按当前上限运行”的 `veb_forest` 全天运行在 61 s 时被终止（`status=timeout`，“运行超过 1 分钟，已终止”），选择“调到建议值并运行”后工程写入 10 分钟并在 3 分 43 秒内完成。
- 未验证：桌面（Electron）模式下的运行质量档位与上限流程。
- 中波红外太阳反射：新增 6 个 MWIR 算例对比修正前引擎（`bin_periodic`），逐像元按 ΔL = B(T新) − B(T旧) 反推等效反射率与材质 TIR 反射率一致（数字见上）；10.5 µm 波段与夜间算例逐字节相同；22 个旧算例 86 个 TIFF 与 `periodic_final` 逐字节相同。未与 MODTRAN/DART 等外部模型对比。
- VoxelEB 稳定度迭代：中性管线的 `aeresist/budget/evapo.comp.spv` 与修正前引擎（`bin_mwir`）逐字节相同；9 个 VoxelEB 算例（含新增 2 个夜间算例）关闭稳定度时 TIFF/诊断二进制与 `mwir_final` 逐字节相同；开启时逐算例对比能量项与迭代（数字见上），`veb_urban`、`veb_forest_night` 重复运行逐字节相同；`project-schema.js` 检查新建 VoxelEB 为 true、FacetEB/VoxelRT 为 false、旧工程缺字段为 false、XML 写出一致。未在界面中实测下拉框；未做 SCOPE 对比。

## 已做的检查（2026-11-16）

- 产品引擎重建通过；`eSceneAnimation` 被拒绝（退出码 2）、`--list-devices` 正常；`histream_lumped_thermal_tests` 9 项通过；核心 eFacetEB 集中参数场景（`test_lumped_scene.py`）6 项通过。
- STREAM IMAGING 动画引擎（核心 + 扩展）构建通过；impact-v1 公式夹具 157/157；`test_animation.py`、`test_mesh.py`、`test_sensor.py` 与统一入口最小网格样例在 GPU 上通过。
- 界面重建（`npm run build`）通过；新服务示例列表为 5 个精选案例，`/api/scene-animation/*` 返回 404，“解决方案”菜单只含三维与解析两组。

## 发布前仍需

- 定版本号，`npm run pack:win` 生成安装包；
- 干净 Windows 环境安装、升级与卸载；HiStream 三维模式和 STREAM 的回归；
- 用 `tools/prepare_user_release.py` 生成交付目录（脚本已写好，尚未实际使用）。
