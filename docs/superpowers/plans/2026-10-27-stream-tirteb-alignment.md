# STREAM 对齐 TiRT / TIRTEB：差异清单与实施方案

状态：方案 A（全物理对齐，城市保留四组分）+ 方向成像对齐已实施（2026-10-27）；Release 构建通过。按项目 `AGENTS.md` 未运行任何测试、对比或冒烟算例。

本方案只讨论 `models/stream`（`stream.exe`）及其配置与输出。HiStream 三维链路不在范围内。
参考实现固定为 `C:\work\tirt` 当前工作树（`tirteb_run.py` 2026-10-26、`base/pipeline.py`、`base/aerodynamic.py`、`base/eb.py`、`base/heat_conduction.py`、`base/tirteb_radiation.py`）。

## 1. 结论

STREAM 的 EB 链路来自 StreamEarth 的 `tirt_analytic` 非生长路径，而 TiRT/TIRTEB 在 2026-10 已完成一轮物理重写（统一 TiRT/TIRTEB、修正 TIRTEB 能量平衡）。两者目前不是“默认值差异”，而是**求解结构不同**：

- STREAM 用解析 Jacobian 的松弛 Newton（`relax=0.9`，残差 5 W/m²），TIRTEB 用 `WC=0.08` 的残差修正（默认 `temperature_method=robust`）并在不闭合时进入信赖域后备；
- STREAM 的 1/L 用 H+LE 虚温浮力，ζ 截断 [−5,1]，20 次外层；TIRTEB 只用 H、区间割线、60 次外层并在失败时联合求解 T/H/1/L；
- STREAM 的气动阻抗缺阵风、400 s/m 分段限幅、LAI<0.5 裸土过渡；`G1` 公式也与 Raupach/SCOPE 不同；
- STREAM 气孔在净光合 ≤0 时取 625000 s/m；TIRTEB 取 `stomatal_g0=0.01` 下限；
- STREAM 短波始终用 Erbs 且丢弃 <20 W/m²；TIRTEB 默认 0.8/0.2 + 晴空 DNI 限幅；
- STREAM 长波是解析两层闭合（k=1）；TIRTEB 是节点辐射度全阶；
- STREAM 土壤热是强迫–恢复；TIRTEB 默认 `surface`（48 h 温度历史卷积），另有 `depth`（2 m 土柱，16 节点）；
- 初温：STREAM 取 ERA5 `skt/stl2/t2m`；TIRTEB 取首日平均气温；
- 城市：STREAM 四组分简化；TIRTEB 屋顶/墙面/街道/植被多组分 + 城市辐射度。

因此“对齐”必须先确定深度；按 A（全物理对齐、保留四组分城市）估算，`models/stream` 需新增约 1200–1800 行、改动 `energy/shortwave/longwave/image` 四个模块。

## 2. STREAM 现状落点

| 文件 | 职责 | 关键位置 |
| --- | --- | --- |
| `models/stream/physics/energy.h` | 参数结构与 `EnergyState` | L7–55 |
| `models/stream/physics/energy.cpp` | 气动、气孔、土壤阻抗、Newton、稳定度 | `aerodynamicResistances` L69–114；`stomatalResistance` L117–175；`soilSurfaceResistance` L177–183；`newtonStep` L195–206；`solveEnergyNode` L212–363 |
| `models/stream/physics/shortwave.cpp` | 二流短波与直/散分配 | `erbsDiffuseFraction` L152–161；`absorbShortwave` L163–208（L169 的 20 W/m² 门槛） |
| `models/stream/physics/longwave.h` | 解析冠层长波 | `canopyLongwave` L36–73 |
| `models/stream/physics/effective_lai.cpp` | 方向有效 LAI（当前无垄行/树冠参数） | 全文件 |
| `models/stream/app/image.cpp` | 逐像元 EB 时序与成像 | EB 初温 L412–415；时序 L421–433；通量输出 L439–443 |
| `models/stream/classes/classes.json` | IGBP 参数表 | `soil` L5–12；`aerodynamics` L14；各类 `ground` |
| `server/stream/config.mjs` | 工程 → `stream-image-config` v2 | `buildEngineConfig` L203–266 |

## 3. 逐项差异

### 3.1 短波分配与限幅

| 项 | STREAM | TIRTEB |
| --- | --- | --- |
| 直/散比例 | 始终 Erbs（`erbsDiffuseFraction`） | `shortwave_diffuse_fraction` 默认 0.2；`-1` 才 Erbs |
| 晴空限幅 | 无 | `shortwave_enforce_solar_limit=1`，Meinel DNI + Kasten–Young 大气质量，超出直射转漫射 |
| 小通量 | `<20 W/m²` 直接归零 | 保留任意正入射 |
| 日地修正 | Erbs 用固定 1361 | `E0=1+0.033cos(2πDOY/365)` |

代码落点：`shortwave.cpp` `absorbShortwave` 签名增加 `diffuseFraction`、`solarDayOfYear`、`enforceLimit`；`image.cpp` L430 传入。

### 3.2 冠层消光与短波传输

| 项 | STREAM | TIRTEB |
| --- | --- | --- |
| 消光 | `ks=G/cos(sza)` 的二流 | `hom3` 节点辐射度，全阶散射，互易交换 |
| 叶片吸收划分 | 光照/阴影按 `-expm1` 深度 | 节点光照面积 `min(beam/extinction, leaf_area)`，逐节点分配 |
| PAR | 50 nm 分组（约 41 个波段，`spectralBinNm` 可调） | 1 nm 2001 波段 |

对齐时若只改分配与限幅、保留二流，可先得到“同输入同比例”的结果；若要逐像元与 TIRTEB 一致，必须换成 `hom3` 节点算子（2 个冠层节点 + 土壤，含上/下行状态，共 5 个状态）。

### 3.3 长波

STREAM `canopyLongwave` 是 k=1 的解析解（`longwave.h` L36–73），叶片两向发射；TIRTEB `prepare_longwave` 在节点算子上求全阶，含土壤—冠层互易与天空漫射。等温条件下两者都应给出净零，但方向性与层间分配不同，直接影响 Tls/Tlh。

### 3.4 空气动力学

| 项 | STREAM（`energy.cpp` L69–114） | TIRTEB（`base/aerodynamic.py`） |
| --- | --- | --- |
| 参考风 | `max(wind,0.2)` | `max(wind, min_wind_speed=0.05)` + 对流阵风 `wstar=sqrt(u²+wstar²)` |
| 粗糙度/位移 | `G1=max(3.3, sqrt(Cs+Cr·LAI/2))`，典型 LAI 下恒为 3.3，仅 LAI>109 才变 | `G1=max(3.3, (Cs+Cr·LAI/2)^-1/2)`，LAI<0.47 时 >3.3，稀疏冠层 `z0` 更小 |
| 分段限幅 | 无 | 每段 `max_aerodynamic_resistance=400 s/m` |
| LAI<0.5 | 直接走冠层式 | smoothstep 混合到裸土（`ustar`、`raa_soil`、`d`、`z0`） |
| 稳定度 | 每高度分别裁 ζ | 全廓线先裁 `xeff=min(1/L,1/zref)`，稳定段统一 `ψ=-5·z·xeff` |
| 裸土 `z0h` | `z0m` | `z0m/10` |
| 参考高度 | 10 m，冠层高时抬到 `hc+0.1` | `z`（默认 15 m；城市取 `max(15, 建筑高)`） |

### 3.5 气孔与土壤阻抗

- `stomatalResistance`（L117–175）：`A<0` 时 `rcw=625000`；TIRTEB `biochemical.py` L68/L150 用 `g0=0.01 mol m⁻² s⁻¹` 下限，并换算为阻抗。
- STREAM 土壤阻抗固定 `1.439e5 (θsat−θ)^3.14`；TIRTEB 模板给 `soil_rss=200`（constant 模式），`soil_rss_method=sellers` 才用含水量关系。
- 叶片发射率：STREAM `1−reflTir−transTir=0.975`；TIRTEB 默认 0.985；土壤 0.96 vs 0.955。

### 3.6 温度求解

STREAM（`newtonStep`/`rebalance`）：
- 解析 Jacobian，`relax=0.9`，单步 ±1.5 K，240–380 K；
- 收敛判据残差 ≤5 W/m² 且步长未被裁剪；
- 闭合后在新温度下复算一次。

TIRTEB（`base/eb.py`）：
- 默认 `robust`：`ΔT = WC·EBres / (rho·cp/(ra+rs) + 4σεT³)`，`WC=0.08`，土壤传热项 ×2；
- `direct` 才用含潜热导数的分母；
- 闭合判据 `|EBres| ≤ RAD_THRESHOLD` 且未裁剪；
- 不闭合时 `energy_solver` 信赖域后备（温度 + 阵风 H 作为未知量）。

### 3.7 稳定度

STREAM（L320–349）：
- `1/L_target = −κ g (H/(ρcpTa) + 0.61E/ρ) / u*³`，乘 `(z−d)` 后裁到 [−5,1]；
- 松弛 0.5，变号后二分，`|Δζ|<0.01`，20 次。

TIRTEB（`pipeline.py` L140–219）：
- `x=1/L_target = −κ g H /(ρcpTa u*³)`，只用 H，不裁 L；
- 内层闭合且残差 ≤1e-4 W/m² 才建立区间；区间内割线取 10%–90%；
- 未建区间时 `x←(x+target)/2`；最终联合后备；
- 容差 `1e-7+1e-6|target|+|c|·1e-4/u*³`。

### 3.8 土壤热

STREAM：`G = I/√(2ω)·[(Ts−Ts,prev)/Δt + ω(Ts−T2)]`，`T2` 当日不变（`energy.cpp` L231–233、L268）。

TIRTEB：
- 默认 `surface`：48 h 温度历史卷积 `G=2I/√π ∫(dT/dt)/√(t−τ)dτ`（`heat_conduction.py` L144–162），保存真实时间间隔；
- `depth`：2 m 土柱，16 节点，几何加粗，向后欧拉，底部零通量，`G` 含表层半格储热；
- 城市屋顶/墙面 `depth` 用有限围护结构 + 固定室内温度。

### 3.9 初值与时序

| 项 | STREAM | TIRTEB |
| --- | --- | --- |
| 土壤初温 | ERA5 `skt`（00 UTC） | 首日平均气温（模拟首日全部时次） |
| 深层温度 | ERA5 `stl2/stl1` | 由热历史/土柱自洽推进 |
| 叶片初温 | ERA5 `t2m` | 同土壤初温 |
| 时次 | 00 UTC 到过境，整点 + 过境时刻 | 气象文件时间戳，实际间隔 |
| 水体 | 取 `skt` | 同 |

### 3.10 参数与输出

STREAM 的 `classes.json` 把冠层、叶片光学、光合、地面参数按 IGBP 固化；TIRTEB 用显式参数表（`input_tirteb.csv`，40+ 项），并把观测波段与宽带参数分开。STREAM 配置（`config.mjs`）只暴露地表类型、LAI、土壤水分、DEM、几何、波段、温度来源。

输出：STREAM `eb_fluxes.tif` 9 波段（Rn/LE/H/G/iterations/converged/L/u*/stabilityConverged）；TIRTEB 还记录残差、`stability_residual_m_inv`、`stability_limited`、阵风一致性、空气密度、气动粗糙度/位移/冠层权重、晴空指数等。

## 4. 参数映射（对齐时应落到 STREAM 的哪一层）

| TIRTEB 参数（默认） | STREAM 对应 | 处理 |
| --- | --- | --- |
| `lai` | 输入栅格 | 不变 |
| `height` / `leaf_width` | `classes.json` `canopy` | 保留 IGBP，但允许配置覆盖 |
| `hspot` | `canopy.hotspot`（当前未被 EB 使用） | 成像阶段接入 |
| `min_wind_speed` 0.05 | 无（0.2） | 新增 `aerodynamics.minWindSpeed` |
| `max_aerodynamic_resistance` 400 | 无 | 新增 |
| `aero_cd/cd1/cr/cssoil/psicor/rbs` | `aerodynamics` | 补 `cr`（当前用 `cd` 代替） |
| `stomatal_g0` 0.01 | 无 | 新增 |
| `stomatal_method` farquhar | 固定 Farquhar | 新增 `ballberry` 分支（可选） |
| `soil_rss` 200 / `soil_rss_method` | `resistance:"moisture"` | 改为 `soilResistance` 显式值 + 方法 |
| `soil_cs/rhos/lambda` | 只有 `thermalInertia` | 拆成三项，`I=sqrt(λ·ρ·c)` |
| `soil_heat_method` surface/depth | 固定强迫–恢复 | 新增，默认 `surface` |
| `soil_emissivity` 0.955 / `leaf_emissivity` 0.985 | 0.96 / 0.975 | 改默认 |
| `shortwave_diffuse_fraction` 0.2 | 无（Erbs） | 新增，默认 0.2 |
| `shortwave_enforce_solar_limit` 1 | 无 | 新增 |
| `dtime` 1800 | 实际时间差 | 保持 |
| `max_iter` 50 / `max_stability_iter` 60 | 50 / 20 | 改 60 |
| `temperature_method` robust | 解析 Newton | 改为 WC=0.08 residual 修正 |
| `z` 15 | 10 | 新增参考高度，默认 15 |

配置层建议：`stream-image-config` 增加 `energy` 子对象（`scheme`、`soilHeatMethod`、`diffuseFraction`、`temperatureMethod`、`aerodynamics` 覆盖），工程 `stream-project.json` 由工作台传入；未给出时全部取 TIRTEB 默认值，旧工程行为改变但可显式回退（`scheme:"stream-earth"`）。

## 5. 分阶段实施

**阶段 0：参数与默认值（低风险）**
1. `classes.json`：`soil.emissivity=0.955`、`leaf.emissivity=0.985`、`soilResistance`、`soilCs/Rhos/Lambda`、`aerodynamics.{minWindSpeed,maxResistance,cr}`。
2. `energy.h`：`EnergySurface` 增加字段与默认值；`EnergyState` 增加热历史/土柱/诊断字段。
3. `config.mjs`：编译 `energy` 子对象并校验；缺省值集中在一个 `defaults` 常量。
4. 文档：`models/stream/README.md`、`PORTING.md` 更新参数来源。

**阶段 1：短波分配与限幅（独立、可先做）**
1. 增加 `shortwavePartition(global, sza, doy, requested, enforce)`，返回 `{fraction, limited, limit}`。
2. `absorbShortwave` 使用该比例，删除 20 W/m² 门槛（改为 `global<=0` 才零）。
3. 输出 `eb_forcing.tif` 增加直射法向、限幅标记（或写 `eb_summary.json`）。

**阶段 2：气动、气孔、温度修正（核心）**
1. 重写 `aerodynamicResistances`：Raupach `G1`、`cr`、`min_wind_speed`、阵风（用上一轮 `sceneSensible`）、分段 400 s/m、LAI<0.5 smoothstep 混合、稳定廓线统一裁 `xeff`。
2. 气孔：`A<=0` 时按 `g0` 计算阻抗；阻抗换算用 `rho_air/M_air`。
3. 温度修正：把解析 Jacobian Newton 换成 `WC=0.08` 的 residual 修正（`direct`/`robust` 两分支），保留 ±1.5 K 与 240–380 K 保护；残差阈值改 1e-4（内层）与 5 W/m²（最终）。

**阶段 3：土壤热与初值**
1. 新增 `physics/heat_conduction.*`：`surface` 历史卷积 + `depth` 土柱，移植 `heat_conduction.py` 的层结构与向后欧拉。
2. `EnergyState` 保存 `surfaceHistory[2]`、`depthProfile[2]`、`historyDt`；每步只在最终温度提交一次。
3. `image.cpp` 初始化改为首日平均气温；逐像元保留状态链。

**阶段 4：稳定度与联合闭合**
1. `pipeline.py` L140–219 的 `x=1/L` 割线 + 区间 + 容差。
2. `energy_solver.py` 的信赖域后备（可选第二阶段；先做标量收敛 + 显式 `converged=false`）。
3. `NodeResult` 增加 `residual`、`stabilityResidual`、`limited`、`gustResidual`、`airDensity`、`z0/d/weight`。

**阶段 5：长波与短波节点算子（与 TIRTEB 逐像元一致的前提）**
1. 移植 `node_radiosity` 的 `hom3` 调度（5 状态线性系统）与 `canopy_closure.fractions` 的 hom 分支。
2. 长波全阶、短波全阶；替换 `longwave.h` 与二流。
3. 方向成像（若确认）：`view_fractions` + `thermal_scatter` + 热点。

**阶段 6：输出与文档**
1. `eb_fluxes.tif` 增加波段：残差、`stabilityResidual`、`stabilityLimited`、`gustResidual`、`airDensity`、`z0/d/weight`、`clearnessIndex`、`partitionLimited`。
2. `eb_forcing.tif` 增加直射法向/限幅。
3. `scene_summary.json` 增加未闭合像元、限幅时次数。
4. `docs/user-manual.md` 5.2、`models/stream/README.md`、`PORTING.md` 更新。

## 6. 风险与边界

- 按项目 `AGENTS.md`：不创建、不运行任何测试、回归或对比；只做编译与静态检查。数值一致性若需确认，由用户明确指定范围后再跑。
- `depth` 土柱与 `surface` 历史都会改变现有内置示例的参考结果；旧工程需要显式 `scheme` 回退。
- 城市若保持四组分（方案 A），与 TIRTEB 城市分支不会逐像元一致；这一点必须在文档中写明。
- 阵风与稳定度联合求解需要内层残差 ≤1e-4 W/m²，现有 5 W/m² 闭合标准必须同步收紧，否则外层永远不会建立区间。

## 7. 工作量估算（方案 A，不含成像）

| 阶段 | 内容 | 预计改动 |
| --- | --- | --- |
| 0 | 参数与默认值 | ~150 行（`classes.json`、`energy.h`、`config.mjs`） |
| 1 | 短波分配与限幅 | ~150 行（`shortwave.*`、`image.cpp`） |
| 2 | 气动/气孔/温度修正 | ~400 行（`energy.cpp` 重写） |
| 3 | 土壤热与初值 | ~350 行（新增 `heat_conduction.*`、`image.cpp`） |
| 4 | 稳定度与联合闭合 | ~250 行（`energy.cpp`、`NodeResult`） |
| 5 | 长波/短波节点算子 | ~600 行（新增 `node_radiosity.*`） |
| 6 | 输出与文档 | ~150 行 |
| — | 合计 | **约 1900 行** |

方案 C 约为阶段 0–4 的子集（约 900 行）；方案 D 约 300 行。

## 8. 验收判据

1. `cmake --build build/stream --config Release` 通过；`stream run` 对内置示例“北京市 Landsat 影像-ERA5 单日驱动”正常产出全部文件。
2. 旧工程的 `stream-project.json` 可读；未显式给 `energy` 时按新默认运行，给 `scheme:"stream-earth"` 时数值与当前版本一致。
3. 单像元（平面、均匀冠层、裸地）在相同 ERA5、LAI、组分光学下，`output.csv` 级别的中间量（Rn/LE/H/G、L、u*、残差）与 TIRTEB 在 1e-6 内一致——**该对照属于用户明确要求的验证，默认不运行**。
4. 文档中写明：城市四组分简化在方案 A 下与 TIRTEB 城市分支不逐像元一致；长波/短波若停在阶段 4，也未与 TIRTEB 全阶散射一致。

## 9. 实施状态（2026-10-27）

已完成：

- 新增 `physics/radiosity.*`（`hom3` 节点系统、全阶短波/长波、有效发射率、方向多次散射）、`physics/canopy.*`（标准热点四组分、球面叶片一次散射、BRF）、`physics/heat_conduction.*`（7 h 表面热历史 / 2 m 土柱）、`physics/brightness.h`。
- 重写 `physics/shortwave.*`（0.8/0.2 + 晴空 DNI 限幅、节点辐射度吸收、PAR）与 `physics/energy.*`（TIRTEB 气动/气孔/温度修正/1/L 割线）。
- `app/image.cpp`：EB 初值改首日平均气温，成像改用 `canopyReflectance` 与 `homThermalScatter` + Planck；`eb_fluxes.tif` 扩到 18 波段。
- `classes.json`、`config.*`、`server/stream/config.mjs`：TIRTEB 参数与默认值；`energy` 子对象（含 `spectralBinNm`，默认 50 nm，可设 1 做 1 nm 全谱）。
- `PORTING.md`、`README.md`、`docs/user-manual.md` 5.2、工作台提示与波段标签已更新；Release 构建与 GUI 打包构建通过，`build-manifest.json` 已刷新。

尚未完成（有意保留）：

- EB 短波默认 50 nm 分组（约 41 个波段）；`energy.spectralBinNm=1` 可换回 1 nm 全谱（约 50 倍成本）。
- 表面热历史窗按项目要求取 7 h（TIRTEB 为 48 h）。
- 未移植信赖域 T/H/1/L 联合后备；不收敛时只标记 `converged=false`。
- 城市仍为四组分简化；不支持垄行/树冠/单坡/复合地形。
- 工作台尚未暴露 `energy` 参数（配置文件可用，GUI 用默认值）。

## 10. 需要确认

1. 是否需要补上联合后备求解器。
2. 是否把 `energy` 参数加到工作台（以及是否保留旧工程数值回退开关）。
3. 城市多组分是否另开一轮。
