> 历史实现记录。文中测试脚本和临时结果已从项目清理；其中命令不再作为当前操作步骤。SCOPE 对比资料已迁至 D:\cases\streamstudio\模型对比\SCOPE，只有用户明确要求时才执行，日常工作不测试。

# FacetEB GPU 能量与生理计算

FacetEB 的 Vulkan 路径将入射辐射汇总、Collatz / Farquhar C3/C4 叶片生理耦合、面元能量通量、温度更新和收敛最大值归约放到 GPU。温度、通量及长波光学状态在耦合迭代期间驻留显存；每轮仅回传收敛标量。最终数组在时间节点完成后读取，输出格式不变。

原有 24 次气孔阻力二分、最多 32 次温度根求解、固定 64 次辐射迭代、外层耦合预算和所有收敛阈值保持原样。根求解使用 FP64；着色器自行实现双精度 exp/log，避免 GLSL 单精度超越函数降低根求解精度。浮点舍入允许产生微小差异，不要求逐位相同。

CPU 保留几何预处理、每节点土壤列响应准备、气象和短波输入组装、结果写盘。开启 MOST 稳定度修正时，冠层列汇总仍在 CPU，每次稳定度更新会读取状态并更新气动阻力。这里不宣称整个程序完全脱离 CPU。

GPU 能量路径需要 Vulkan `shaderFloat64`。缺少能力或匹配的 `energy.comp.spv` 会明确报错；显式设置环境变量 `HISTREAM_FACET_ENERGY_BACKEND=cpu` 可以运行保留的 CPU 能量对照路径，辐射后端继续沿用项目设置。运行索引 `faceteb_run.json` 的 `execution` 字段记录各阶段实际后端及迭代状态是否驻留显存。

## 形状因子复用

固定几何的面元可见性图在气象时间循环之前构建一次，所有时间节点及波段共享。温度、气象和光谱变化不重建这张图；太阳变化仅重新计算直射受光比例。跨进程磁盘缓存尚未实现，重新启动同一场景仍会构建可见性图。

## 玉米场景性能记录

2026-10-13，RTX PRO 6000 Blackwell，Threadripper 9980X；5×5 m、100 株玉米、原始 34,321 三角面，过滤一个退化面并增加两个底层背景面。采用相同合成晴天气象、48 个半小时节点、400–2400 nm 的 100 nm 光学窗口，温度容差 0.05 K，最多 60 次外层耦合。计时包含初始化和原生写盘，不含 GUI 图像生成。

最终构建实测 156.42 → 44.23 秒（3.54 倍），48/48 收敛。复测、逐节点二进制数值差异及引擎 SHA256 见 `.test/gpu-energy-20261013/day48-summary.json`。性能与数值一致性不构成真实玉米地温度验证。

原有光栅遮挡采样存在重复运行差异：深度相同的片元没有侧编号的排序次序，原引擎重复同一时刻也可产生不同受光比例，并经未采样面元插值传播。一次独立复测出现少量面元约 0.12 K 的中间时刻差异，最终时刻差异仍约 0.000062 K。此处保留原采样算法，不把跨运行采样变化误当成能量迁移误差，也不承诺独立运行逐位相同。

设置 `HISTREAM_FACET_VALIDATE_ENERGY=1` 可在同一运行、相同可见性图、辐射场和温度状态上核对 GPU 与保留的 CPU 入射汇总、能量通量和温度更新；验证不替换 GPU 状态。每次通量差异须不超过 0.01 W/m²，温度更新差异须不超过 0.0002 K（叶片 0.001 K）。取 `HISTREAM_FACET_VALIDATE_ENERGY=2` 为汇总模式：不在第一处不一致时中止，而是跑完全部节点，按“表面类别/字段”（`leaf`、`ground`、`ground(MOST)`、`water`、`wall`、`solid-shell`、`submerged-hull`）统计不一致个数与最大差，写入 `faceteb_run.json` 的 `energyVerification`（`mismatches`、`auditedFacetsByClass`），并在 stderr 打印；有不一致时退出码 3。该审计会执行 CPU 对照，耗时不能用作 GPU 性能结果。

叶片的气孔电阻 $r_s$ 与叶温联立求解，用对数电阻二分、每个试探点内再解叶温，**一进入容差（ln r_s 差 ≤ 0.01）就退出**。CPU 单精度/GPU 双精度生理模块的舍入差会让退出步差一步，两侧 $r_s$ 因此可差约一个容差带（实测最大 ln 差 0.026，超过 2×0.01，因为退出带宽还取决于湿度反馈强度；未推导带宽公式），叶片潜热 ∝ 1/(r_b+r_s) 随之差零点几 W/m²（旧版审计在 feb_urban 第 23 节点 40842 号叶片面元报 CPU 38.875 / GPU 39.216 W/m²，两侧 $r_s$ 578.2 / 570.7 s/m，ln 差 0.013，r_b = 283 s/m）。这是求解器退出步的不连续，不是 CPU 与 GPU 公式不同，所以审计改为：（1）CPU 自由求解，只对 ln r_s 做 0.1 的粗界检查；（2）把 GPU 的 $r_s$（着色器 `energy.comp` 将叶片的 $r_s$ 写入通量槽 11，叶片没有热源，该槽对叶片原为 0）强加给 CPU 叶片，再逐项严格比较全部通量、斜率、气孔失配（ln，容差 5×10⁻⁴）与温度更新。故意把 GPU 的汽化潜热系数、Ball-Berry 斜率或地面显热改 1% 会分别报 `latent`、`stomatalMismatch(ln)`、`ground/sensible`。

最终 48 节点审计检查 507 次能量求值及 411 次温度更新，全部通过：入射辐射差异为 0，通量最大差异 0.000995636 W/m²，温度更新最大差异 0.000030518 K。审计耗时 197.34 秒，单独记录在 `.test/gpu-energy-20261013/day48-shadow-summary.json`。

同一几何与气象下，VoxelEB 默认一阶短波散射、关闭异质性体元与 GUI 图像：0.1 m 体元 43.92 秒，0.05 m 体元 58.75 秒，各完成 48 节点。VoxelEB 与 FacetEB 的离散方式、散射和生理算法不同，此表仅比较运行时间，不是精度等价试验。

## 验证与复现

2026-10-13 最终构建已通过 C3/C4 CPU/GPU 对照、48 节点共享输入审计、叶片生理/土壤导热/MOST 检查、SCOPE 1.73 单日检查及 SCOPE 2 全部 30 场景检查。SCOPE 2 维持原规则：13 个未过饱和场景用于主阈值评分，17 个过饱和场景单独保留；两种模型均无失败或未收敛场景，评分阈值未修改。完整结果在 `.test/gpu-energy-20261013/scope-homogeneous-doy169/results/` 和 `.test/gpu-energy-20261013/scope2-lhs30/results/`，状态记录为 `gpu-final-status.log`。

- `.test/gpu-energy-20261013/run-benchmark.mjs --day`：与修改前冻结结果逐节点比较温度与通量，记录受光比例是否变化，检查所有二进制数值有限，记录运行阶段和耗时。
- 同一命令增加 `--shadow`：启用共享辐射输入的逐迭代 CPU/GPU 检查，原引擎重复采样记录为 `.test/gpu-energy-20261013/old-sunlit-summary.json`。
- `node validation/facet-farquhar/run.mjs`：C3/C4、机理参数、夜间/木质对照、土壤导热和稳定度检查。
- SCOPE 一日和 30 场景验证按根目录 `AGENTS.md` 运行；可设置 `STREAM_STUDIO_VALIDATION_OUTPUT_ROOT` 和 `STREAM_STUDIO_VALIDATION_RESULTS_ROOT` 使用独立目录，评分与阈值不变。

构建必须同时运行 `histream` 和 `histream_runtime_resources`，确保 C++ 的缓冲区布局与编译后的着色器匹配。

## 缓冲区布局

`energy.comp` 与 `facetrtcore.cpp` 共用以下平坦 SSBO 布局，索引为 `[state][oriented side][field]`，两种状态依次为晴、阴。

- 材质：32 个 float/面元；0–9 为植被/光伏标记、储热/对流/PV/土壤参数；10–17 为 Vcmax、Ball–Berry、生化类型与温度修正；18–22 为 Collatz 温度参数；23–26 为 Tyear、beta、qLs、kNPQs。
- 节点输入：12 个 float/状态/侧；依次为上一节点温度、短波吸收、PAR、短波入射、叶片/土壤气动阻力、相对冠层深度、土壤热通量斜率与截距、TIR 反射/透射、受光比例。
  - **城市街谷显热（2026-11-27，`control.urbanAerodynamics`）**：不再借用输入槽 I[6]；街谷空气温度 T_can 与墙/路面换热系数 h_s 随耦合迭代变化，作为 push constant 的 `canyonAir`、`canyonSurface`（`EnergyPush` 56 字节，主机每次迭代后 `setEnergyCanyon`）。材料槽：墙 P[30] = −1、P[31] = 1（导度 h_s，参考 T_can）；屋顶 P[30] = −1 − 屋顶系数、P[31] = 0（参考气温）；道路（地面）P[29] > 0.5（导度 h_s，参考 T_can，阻抗 ρc_p/h_s 同时用于潜热）；街谷内的叶片与树下地面 P[31] = −1（显热参考 T_can）。未开启时这些槽与 push 字段不被读取。公式与验证见理论手册、发布草稿。
- 通量：12 个 float/状态/侧；依次为净辐射、显热及斜率、潜热及斜率、储热及斜率、光伏功率及斜率、叶温更新、气孔对数失配、保留字段。
- 归约：7 个 uint32；前五项为非负 float 的位表示（热残差、等效温差、气孔失配、叶片残差、温度更新最大值），第六项为错误标志，第七项为能量残差未达标（|残差| ≥ 2 W/m²）的面元计数，供收敛判据使用。非有限状态或无效生理根直接报错。

## 2026-11-20 修正

- **温度容差不参与收敛判断。** 项目中的 `temperatureTolerance`（上文“温度容差 0.05 K”）只写入 `faceteb_run.json` 的 `convergence.temperatureTolerance_K`，并以 `temperatureToleranceRole` 标明“仅记录”。节点是否收敛只看能量残差面元比例：|残差| ≥ 2 W/m² 的面元少于 0.1%。收敛策略和迭代预算没有改动。
- **异质背景（Hapke）在能量平衡中保持能量守恒。** 此前能量短波直接使用“法向观测”的 Hapke 方向反射因子作为辐射度求解的半球反射率。该因子在天底无热点处归一，其半球积分高于 Lambert 值（底值 0.2 时约 1.3–2.5 倍），会额外产生能量。现在的约定：
  - Lambert 值作为白空（双半球）反照率，天空漫射按该值反射；
  - 直射按 base·A(μ0)/A_bh 反射，即只保留 Hapke 反照率随太阳角的变化（底值 0.2 时，天顶角 0°/60°/85° 约为 0.81/1.08/1.54 倍）；
  - 两项按入射通量加权。A(μ0) 和 A_bh 对 Hapke 因子做 16 点 Gauss-Legendre（μ）×32 点方位求积后制表；
  - 长波能量同理，保留 Lambert 发射率作为半球发射率，方向发射率只用于影像。

  影像路径（FacetRT 光谱影像、FacetEB 逐节点光谱影像）仍用方向反射因子和方向发射率，观测方向统一为天顶，即星下点传感器。此前反射率按面元法向观测、发射率按天顶观测；对水平地面两者相同。强度为 0 时结果与此前完全相同。GPU 光谱路径的面元参数 `F[f+9]`（相角）和 `F[f+11]`（观测余弦）改由 CPU 按天顶方向计算，着色器及缓冲布局不变。影像中的 Hapke 因子同样未按半球守恒归一，与 VoxelRT / RayTracing 共用的 `background_hapke.glsl` 约定一致，是否统一重归一需另行决定。
- **低太阳角下的直射守恒。** 水平直射换算为法向直射时，太阳高度余弦 ≤ 1e-4 被丢弃的部分，以及超过 1400 W/m² 上限被截去的部分，现在计入天空漫射。水平总短波与气象驱动相等，能量求解与光谱影像使用同一光照。
- **直射比例由光照设置决定。** 与 VoxelEB 一致，`Esun_.dat` / `Esky_.dat` 只提供光谱形状，直射/漫射份额由 `directScatteringRatio`（工程 `light.direct`）决定。此前两条光谱联合归一，有文件时实际份额取自文件（随附文件约 0.806），设置不起作用。
- **对象材质分类。** 没有 `surfaceEnergy` 块的材质不再一律按叶片处理，判定规则依次为：
  1. `componentType` 为 Vegetation 时按叶片处理，材质名含 wood 的除外；
  2. `componentType` 为其他非空类型时按固体处理；
  3. 无 `componentType` 的旧 XML 按 `leafBio` / `soilSet` 块判断；
  4. 仍无法判断时保持旧默认，按叶片处理。

  同时，空的 bioNames 不再误取第一个 `surfaceEnergy` 块。
- **初始土温。** 工程 JSON 未设置 `meteo.Tsold`（缺省或 null）时，不再写出 `initialSoilTemperature`，地面初温取首个气象节点气温。运行索引 `initialSoilTemperature.source` 记录实际来源。
- **能量快照读取。** `readEnergyState(true)` 按最近一次能量求解实际所在的 ping-pong 缓冲读取辐射度，迭代数取实际值，不再固定为 `m_radiosityA` 和 64。

## 2026-11-27 修正与新增

- **未收敛面元计数自检改为容差比较。** 每个节点结束时，主机按下载的通量重数一遍“|残差| ≥ 2 W m⁻²”的面元数，与 GPU 归约的计数比较。GPU 用双精度累加残差、主机用单精度，残差恰在阈值附近的面元可能只被一侧计入；此前差 1 个就以退出码 1 终止整个运行（`feb_crop` 某参数组合可复现）。现在：差值 ≤ 面元总数的 1 % 只在 stderr 警告；节点用满迭代预算时无论差多少都不终止（以运行为先）；其余情况（节点提前收敛且差值 > 1 %）仍报错，因为这说明两份实现不一致、收敛判定本身不可信。每个节点在 `faceteb_run.json` 的 `completed[]` 里记录 `energyFailingFacetsHostRecount` 与 `energyFailingFacetCountDifference`。差值为 0 时输出与此前逐位相同。
- **收敛点切线敏感度（`control.facetSensitivity`）。** 逗号分隔的参数名，环境变量 `HISTREAM_FACET_SENSITIVITY` 优先；目前支持 `leaf.vcmax`（P[10]，全部植被面元）、`leaf.ballBerrySlope`（P[11]）、`ground.surfaceResistance`（P[7]，全部地面面元）、`ground.thermalConductivity`（不在 P[] 里：按 λ(1 ± h) 重算地面侧土壤柱响应，替换输入槽 I[7]、I[8]；方法 2 用 `soilResponse` 的斜率与截距，方法 1 的储热系数按 √λ 缩放）。每个节点收敛后，从收敛态热启动，把参数乘以 (1 ± h)（`facetSensitivityStep`，缺省 0.05）各做 `facetSensitivityIterations`（缺省 24）次耦合迭代——两侧次数相同，所以基态本身残余的收敛漂移在中心差分里一阶抵消；若沿用收敛判据，扰动解会在第 0 步就满足判据，导数为 0。结果写 `process/facetsens_T=<token>.bin`（[参数][面侧][7]，float32，字段同 `faceteb_T=*.bin`：潜热、显热、表面热通量、温度、净辐射、受光/阴影部分温度），单位为 d/d ln θ；`completed[].sensitivity` 给出面积加权类别均值与耗时，`faceteb_run.json` 顶层 `sensitivity` 记录设置。只用 GPU 能量路径；不重建可见性图、不重算短波；**节点间热历史冻结**（上一节点状态取未扰动值），稳定度修正开启时 1/L 冻结。敏感度只读 GPU 状态、不改主机端状态，开启后原有产品与关闭时逐位相同（`feb_crop` 含影像 10 个产品文件核对，4 个参数全开再核对一次）。验证数字见 `docs/differentiable-inversion-and-surrogate.md`。
- **BSM 土壤光谱在含水量 ≤ 5 % 时取干土光谱**（`compo.cpp`，与 SCOPE BSM、STREAM 一致），此前 μ < 0 时被外推成比干土更亮；含水量 > 5 % 的结果逐位不变。
