# 车辆温度模拟（FacetEB）

本文是 FacetEB「车辆温度模拟」模块的设计、使用与局限说明：停放与行驶两种工况，每辆车一个集总座舱节点，输出座舱空气温度、内饰节点温度、车身/玻璃/底盘各面元温度与座舱能量平衡分量。**人体适宜性（烫伤、热指数、分级、儿童/宠物滞留）本期不做，见第 8 节“后续工作”。**

默认全部关闭：旧工程、旧算例的 FacetEB 输出逐字节不变；只有材料 `type: "Vehicle"` 且 `vehiclePart` 有取值时才启用。

**第二版（部件分建模，见第 10 节）**：车窗按前挡风/侧窗/后窗分材料、内饰分仪表台/座椅/方向盘/车顶内衬门板四个节点、发动机块与发动机舱空气独立节点、引擎盖内侧接发动机舱空气、车漆颜色预设；新建车辆对象按网格组名自动绑定。第 1–9 节的“单一内饰节点”与“发动机热均分到底盘”是**旧集总路径**：车身材料缺少新字段（`cabin.interior`、`cabin.engine`）时仍走旧路径，与第一版逐位相同。

## 1. 范围与实现位置

| 项 | 做法 |
|---|---|
| 引擎 | `models/histream/src/facetrt/facetrtcore.cpp`（材料/XML 读写、车辆实例识别、节点内座舱步、输出） |
| 模式 | 仅 FacetEB。VoxelEB 体元 ≥0.5–1 m，车辆仅几个体元，**不实现座舱**，只能用船舶式钢板墙（`wallMode:'wall'`）与 `speed`；建议用 FacetEB 做车辆热分析。RT 模式（VoxelRT/RayTracing/FacetRT）仍用固定温度预设 `vehicle_temperature`（晴 315 K、阴 305 K） |
| 后端 | 座舱方程在主机侧求解，CPU 与 GPU 能量路径共用同一输入（壁板响应、指数），两者结果一致 |
| 不做 | 人体生理热模型、车内气流/分层、行驶中风向变化、VoxelEB 座舱 |

## 2. 车辆实例与部件

车辆 = 对象类型 `Vehicle` 的每个放置点（positions 文件的一行）为一个实例，实例编号保存在 `Scene.facetInstance`（逐面元，1 起）。网格组通过各自材料的 `vehiclePart` 区分：

| `params.vehiclePart` | 部件 | 物理处理 |
|---|---|---|
| `body` | 车顶/引擎盖/行李箱/车门板 | 多层墙体（`wallMode:'wall'`），外侧由面元能量平衡求解，内侧边界 = 本车座舱空气温度；携带本车的座舱与行驶参数 `params.cabin` |
| `glass` | 车窗玻璃 | 单层玻璃墙体（外侧吸收，内侧对座舱）；光谱材料的 `transmittance` 为太阳透射率 τ_sol |
| `underbody` | 轮胎/底盘/车轮罩/灯具 | 薄壳（两面与外界空气交换）；发动机热源作用于此组 |

模型文件 `assets/obj-library/vehicle/typical/modern_sedan.obj` 的组为 Blue（车身）、Windows、Black ×4、Grey ×4、Headlights、TailLights，示例把 Blue → 车身、Windows → 玻璃、其余 → 底盘/灯具。壁板外侧由天空视域自动判定（`kIndoorSkyView`）；网格不封闭而两面都见天空的面元退化为薄壳，运行索引 `buildingWalls.shellFallbackFacets` 会计数。

## 3. 座舱方程

每个实例两个节点：座舱空气 `T_a` 与内饰集总质量 `T_i`（仪表台、座椅、方向盘等合并为一个节点）。节点步长 Δt（30–60 min）远大于座舱时间常数（分钟级），采用后向欧拉（无条件稳定）：

```
C_a (T_a - T_a⁰)/Δt = Gi (T_i - T_a) + Q_wall + Q_occ + G_v (T_out - T_a) - Q_ac
C_i (T_i - T_i⁰)/Δt = f_abs Q_trans - Gi (T_i - T_a)
```

| 量 | 公式 / 默认值 |
|---|---|
| `C_a = ρ cp V` | V = 2.8 m³（典型值，待核对：轿车 2.5–3 m³），ρ 取节点气压气温，cp = 1005 |
| `C_i` | 80 kJ/K（`interiorCapacity`，典型值，待核对：仪表台+座椅+内饰有效热容 50–150 kJ/K） |
| `Gi = h_i A_i` | h_i = 8 W m⁻² K⁻¹（对流约 3–6 加长波），A_i = 8 m²（`interiorH`、`interiorArea`，典型值，待核对） |
| `Q_trans` | 玻璃面元外侧 τ_sol·入射太阳（逐谱带 `windowOptics.transmittance × windowIncident`，含直射、散射、环境反射）× 面积；沿用 FacetEB 面元光谱路径，不另设透射模型 |
| `f_abs` | 0.8（`absorbedFraction`，进入内饰的份额；其余经车窗反射逸出，视为系统外损失；典型值，待核对） |
| `Q_occ` | `occupants × metabolicW`，每人 100 W（成人静坐代谢约 100–120 W；儿童/宠物仅数量与代谢设置） |
| `G_v = ρ cp V ACH/3600` | 关窗停放 ACH = 0.5（0.2–1）；开窗缝（`windowCracked`）取 `crackedACH` = 10（5–20）；行驶（speed>0）ACH = max(设定值, clamp(8 + 1.2 v, 10, 30)) 外循环；开空调时取内循环，ACH 上限 5 |
| `Q_ac` | 比例控制 `Q_ac = clamp(P (T_a - T_set)/band, 0, P)`；缺省 P = 3500 W（`acPower`）、T_set = 24 °C、band = 2 K；停放缺省关闭 |
| `Q_wall` | 壁板内侧热流 `Σ A q`，`q = G_in (T_last - T_a)`，`G_in = 1/(w_n/2λ_n + 1/h_in)` |

### 壁板耦合（线性响应）

后向欧拉壁板对其内侧温度是线性的：在表面温度 `T_s` 已解出后，内侧热流 `q(T_a) = q⁰ − K (T_a − T_a⁰)`，`K = G_in (1 − ∂T_last/∂T_a)`。主机取上一节点末座舱温度 `T_a⁰` 作壁板内侧边界求解表面能量平衡，再把 `q(T_a)` 并入座舱方程隐式求出 `T_a`，随后以新的 `T_a` 重算壁板响应并推进剖面，故**每块壁板与座舱能量平衡均精确闭合**。残余近似：表面能量平衡用的是 `T_a⁰` 而非 `T_a`（一阶滞后，外表面温度差约 `∂T_s/∂T_a·ΔT_a ≲ 0.1 ΔT_a`）。节点内迭代未做。

初值：起算节点座舱与内饰等于室外气温（停放过夜后的日出前状态，近似）。

### 车身与玻璃默认构造（新建工程）

| 材料 | 构造（外→内） | h_in |
|---|---|---|
| 车身 | 钢板 1.0 mm（λ 50、ρ 7850、cp 460）+ 内饰/隔热层 6 mm（λ 0.04、ρ 60、cp 1400） | 7 W m⁻² K⁻¹（5–10：自然对流 3–4 + 长波） |
| 玻璃 | 夹层/钢化 4.8 mm（λ 1.0、ρ 2500、cp 840） | 8 |
| 底盘/轮胎 | 薄壳 `heatCapacityPerArea` 6000 J m⁻² K⁻¹ | — |

光谱：车漆（浅色 ρ≈0.65 / 深色 ρ≈0.12，TIR 反射 0.1）；玻璃 τ_sol 前挡 0.60（0.55–0.65）、侧窗 0.70（0.65–0.75）、隔热玻璃可选 0.40；反射 0.08，TIR 反射 0.1。均为典型值，待核对。

## 4. 行驶与发动机

- `speed`（m/s）与 `heading`/`windDirection`（度）沿用船舶约定：外表面强迫对流系数 `5.8 + 4.1 √u_rel`，`u_rel² = u² + v² + 2 u v cos(heading − windDirection)`，未给方向时 `u² + v²`；按**实例**对车身、玻璃、底盘面元统一取同一速度。
- 发动机：`engineOn` 时热量 `enginePower × engineShare`（默认 15 kW × 0.3，进入罩下的份额；典型值，待核对）均分到 `underbody` 面元（薄壳源项，与船舶 `internalHeatPower` 同一机制）；缺省关闭。排气/轮胎/刹车摩擦热、停车后余热衰减不做。
- 空调：行驶默认可开（`acOn`），停放不开。

## 5. 参数一览（车身材料 `params.cabin`）

`enabled`、`occupants`(0–8)、`metabolicW`、`volume`、`interiorCapacity`、`interiorArea`、`interiorH`、`absorbedFraction`、`ventilationACH`、`windowCracked`、`crackedACH`、`acOn`、`acPower`、`acSetpoint`(°C)、`acBand`、`engineOn`、`enginePower`、`engineShare`、`speed`、`heading`、`windDirection`。每个车辆状态（如“停放晴天浅色”“行驶开空调”）用一份车身材料，对象的网格组绑定到它。

## 6. 输出

- `diagnostics/vehicle_cabin_node=<n>.json`：每辆车一行——座舱空气温度、内饰节点温度、座舱−室外、车身/玻璃外侧与内侧面积平均温度、底盘温度、ACH、能量平衡分量（透射太阳、内饰吸收、壁板→座舱、乘员、通风损失、空调、空气与内饰储热、残差与相对残差）。
- `faceteb_run.json`：`vehicles`（实例列表与面积）、`vehicleCabinSummary`（每车最高座舱温度及节点/儒略时间、最高内饰温度、最大相对闭合残差）。
- 日志行 `VEHICLE_CABIN\t...`；面元温度沿用常规面元输出。

## 7. 验证与文献范围

闭合、稳态解析与 CPU/GPU：座舱节点能量闭合相对残差 ≤ 2.7×10⁻¹²；`tools/test-vehicle-cabin.py` 的隐式步在 600/1800/3600 s、ACH 0.5/10/25 下收敛到解析稳态 `Gv(Ta−Tout)=K(Tw−Ta)+Q_occ+f_abs·Q_trans`（相对差 < 10⁻⁶），空调过载时受功率上限；GPU 与 CPU 能量路径整场（28 节点）座舱/内饰/车身/玻璃/底盘平均温度最大差 1.5×10⁻⁵ K（座舱在主机侧求解，两后端共用同一输入，`HISTREAM_FACET_VALIDATE_ENERGY` 未单独纳入座舱量）；旧 FacetEB/VoxelEB 抽样算例输出逐字节不变。数字与解读见用户手册 9.3.4 和理论手册。

文献对照（均凭记忆，运行环境无法联网，标“待核对”）：McLaren et al. 2005（*Pediatrics*）关窗停放 60 min 升约 19 ℃，其中约 80% 在前 30 min；Grundstein et al. 2009 峰值多比外界高 20–30 K，仪表台/方向盘表面 60–80 ℃。本模型晴面浅色车座舱峰值比室外高 22 K（树荫 6 K、建筑遮阴 10 K、行驶 15 K）、内饰节点 60–64 ℃，同量级偏低端；起升速度从清晨渐强日照起算，不是热车起点，不能直接对照“前 30 min”。

## 8. 后续工作（本期不实现）

人体适宜性评估：座舱平均辐射温度近似、内饰接触面温度（内饰节点 + 表面热阻）、烫伤阈值（Moritz–Henriques / ISO 13732-1 接触烫伤温度-时间曲线，按金属/塑料/织物差异）、Steadman 热指数（座舱水汽压≈室外，乘员产湿可选）、分级（舒适/偏热/危险/极端，工程参考）、儿童/宠物滞留到达危险体温阈值的时间粗估、运行质量摘要“危险辆数”。仅保留思路与参考标准，不进入默认输出。

## 9. 局限

集总座舱无分层与气流；玻璃透射的光不再在车内多次反射（`f_abs` 吸收，其余逸出）；内饰接触面温度仅给节点温度，仪表台直晒表面温度会高于节点温度；壁板用 `T_a⁰` 一阶滞后；车身网格不封闭处退化为薄壳；环境水汽/风向动态不处理。

## 10. 部件分建模（第二版）

车辆对象的各部件分开建模并按网格组名自动绑定。所有缺省值均为典型值，**待核对**（来源列在表中）；没有新字段时走第 1–9 节的旧集总路径。

### 10.1 自动绑定规则（`project-schema.js` 的 `VEHICLE_BINDING_RULES`）

新建/新导入的车辆对象（FacetEB 模式，对象类型 Vehicle，“车辆参数”折叠区勾选“自动绑定”）按 OBJ 网格组名逐行选择光谱与物化材料；用户仍可在“Mesh 属性映射”表逐行改。**已有工程不重新绑定。** 规则自上而下，第一条匹配生效：

| 组名（不区分大小写） | 光谱 | 物化材料（部件） |
|---|---|---|
| `*hood*`/`*bonnet*`（含导入时由漆面组拆出的 `<漆面组>_hood`） | 车漆颜色 | `vehicle_hood`（引擎盖，内侧 = 发动机舱空气） |
| `windshield`/`windscreen`/`front window`/`Windows_front` | `vehicle_glass_front`（τ 0.60） | `vehicle_glass_laminated`（夹层 2.1+0.76+2.1 mm） |
| `rear window`/`Windows_rear` | `vehicle_glass_rear`（τ 0.65） | `vehicle_glass`（钢化 4.8 mm） |
| `side window`/`Windows_side` | `vehicle_glass_side`（τ 0.70） | `vehicle_glass` |
| `Windows`/`glass`（未拆分时） | `vehicle_glass_side` | `vehicle_glass` |
| `Headlights`/`TailLights`/`*light*`/`*lamp*` | `vehicle_lamp_glass`（τ 0，ε 0.9） | `vehicle_lamp`（薄壳） |
| `part_2__covered_car` | `vehicle_tyre_rubber` | `vehicle_tyre` |
| `*covered_car*`（篷布，保持原有篷布处理） | 保持当前所选（篷布光谱） | 车身材料 |
| `Black`/`tyre`/`tire`/`wheel`/`rubber` | `vehicle_tyre_rubber` | `vehicle_tyre`（薄壳） |
| `Grey`/`gray`/`trim`/`plastic`/`bumper`/`grille` | `vehicle_trim_grey` | `vehicle_trim`（薄壳） |
| `White`/`Blue`/`Red`/`Silver`/…/`paint`/`body`/`car` | 车漆颜色（下拉） | 本车车身材料（钢板 + 内饰层，`<对象名>_body`） |
| 其余 | 车漆颜色 | 本车车身材料 |

窗户膜选“隔热”时前挡风取 `vehicle_glass_front_film`（τ 0.45），侧/后窗取 `vehicle_glass_side_film`（τ 0.38）。

**按面元方向拆组**：库里的轿车 `Windows` 是单个组，前挡风与其他窗无法取不同透射率。导入时 `splitVehicleObj` 以尾灯组质心→前灯组质心为车头方向（水平），把玻璃组按面元法向与位置拆成 `Windows_front`（法向近似前后向、位于玻璃组前半）、`Windows_side`（|法向·横向| > 0.6）、`Windows_rear`；漆面组中位于挡风玻璃底边之前、法向向上、不高于挡风底边 0.12 m 的面元拆成 `<漆面组>_hood`。拆分后的 OBJ 是工程里的副本（库文件不改，面元数不变，顶点数据整体前置以保持索引）；缺少前灯/尾灯组时不拆，`Windows` 按侧窗处理。引擎盖以外的前翼子板、保险杠等仍属车身（内侧 = 座舱空气，近似）。

### 10.2 车窗玻璃

| 窗 | 材料 | 光谱 τ_sol / ρ_sol / α | 来源（典型值待核对） |
|---|---|---|---|
| 前挡风 | 夹层 2.1 + PVB 0.76 + 2.1 mm（λ 1.0/0.2/1.0） | 0.60 / 0.25 / 0.15 | 太阳控制夹层玻璃的产品范围 τ 0.55–0.65 |
| 侧窗 | 钢化 4.8 mm | 0.70 / 0.08 / 0.22 | 绿色钢化玻璃 τ 0.65–0.75 |
| 后窗 | 钢化 4.8 mm（隐私玻璃） | 0.65 / 0.10 / 0.25 | 同上 |
| 隔热膜/Low-E | 同上 | 前 0.45、侧后 0.38（ρ 0.30–0.32） | 隔热膜 τ 0.35–0.45 |
| 灯具玻璃 | 薄壳 5.4 kJ m⁻² K⁻¹ | 0 / 0.08 | 灯具不透入座舱 |

发射率 0.9（`refTir` 0.1，TIR 透射 0）。FacetEB 能量路径的短波是宽带 τ_sol，可见光/近红外分谱透射不分别进入座舱方程。窗户面积与朝向由实际面元决定；开窗缝沿用 `windowCracked`/`crackedACH`（缝 10 ACH、全开 30 ACH）。**各窗透射量**：`vehicle_cabin_node=<n>.json` 的 `windowTransmitted_W` 按网格组名给出每节点透射太阳功率（W），运行汇总 `vehicleCabinSummary[].maxWindowTransmitted_W` 给出各窗峰值。

### 10.3 内饰分节点与联立方程

车身材料 `params.cabin.interior.enabled` 为真时，单一内饰节点换成四个节点 k = 仪表台、座椅、方向盘、车顶内衬/门板；座舱空气与它们联立（维数 1 + 4 = 5，加发动机节点时 7），每节点后向欧拉，小型高斯消元（部分选主元，N ≤ 8）：

```
C_a (T_a − T_a⁰)/Δt = Σ_k hA_k (T_k − T_a) + (1−φ) Q_w + Q_occ + Gv (T_out − T_a) + G_fw (T_bay − T_a) − Q_ac
C_k (T_k − T_k⁰)/Δt = s_k f_abs Q_trans − hA_k (T_k − T_a) + φ w_k Q_w
Q_w = Σ A q(T_b) = Q_w⁰ − K_w (T_b − T_b⁰),   T_b = (1−φ) T_a + φ Σ_k w_k T_k,   w_k = radA_k / Σ radA
```

壁板内侧边界取 `T_b`：壁板内侧换热系数 `h_in` 中的 φ 份（辐射）与内饰质量的辐射平均温度交换，其余 1−φ（对流）与座舱空气交换；壁板热量按 (1−φ)、φ·w_k 分给空气与各节点，所以空气 + 内饰 + 壁板总能量精确闭合（`vehicle_cabin_node` 给每节点残差与总残差）。内饰与壳体内表面的辐射交换因此经壁板边界实现，节点自身温度对壳体的直接耦合较弱，**直射下的主要散热由有效 hA（含对长波辐射）承担**。

| 节点 | 吸收份额 s_k | 热容 C_k（kJ/K） | 有效 hA_k（W/K） | 辐射面积 radA（m²） | 备注（典型值待核对） |
|---|---|---|---|---|---|
| 仪表台 | 0.30 | 10 | 8 | 1.0 | 受光最强，近似水平/斜面，吸收率 ≈ 0.9；约 6 kg 发泡塑料表层 |
| 座椅 | 0.35 | 织物 35 / 皮革 50 | 24 | 3.0 | 织物 e≈200、皮革 e≈450 J m⁻² K⁻¹ s⁻⁰·⁵（接触温度用） |
| 方向盘 | 0.05 | 2 | 2 | 0.25 | 约 0.8 kg |
| 车顶内衬/门板 | 0.30 | 60 | 40 | 8.0 | 含 A/B/C 柱饰板 |

有效 hA = (h_c + h_r) A，h ≈ 8 W m⁻² K⁻¹（对流 3–5 + 长波 4–5）。φ 缺省 0.45。份额之和须为 1，`f_abs` 沿用 `absorbedFraction`（0.8）。**透射太阳按用户份额分配，不按光线方向**（按透射光线方向与内饰朝向分配需要面元光谱路径给出每束光落点，涉及 GPU 结构，风险大，未做；份额缺省值取 0.3/0.35/0.05/0.3，各车可改）。

座椅接触面温度（仅作温度输出）：两个半无限体接触，`T_c = (e_seat T_seat + e_skin T_skin)/(e_seat + e_skin)`，e_skin = 1100、T_skin = 34 ℃；输出在 `interiorNodes[seat].contactTemperature_K`。

### 10.4 发动机与发动机舱

`cabin.engine.enabled` 为真时：发动机块（C_e）与发动机舱空气（C_b）两个节点，并入同一线性系统。`Q_eng = enginePower × engineShare`（缺省 15 kW × 0.3，保留作为总量）拆为 散热器（进发动机舱空气，0.5）、发动机块（0.3）、排气（0.2，均摊到 `underbody` 面元，作底盘热源）：

```
C_e (T_e − T_e⁰)/Δt = f_blk Q_eng − G_eb (T_e − T_bay)
C_b (T_bay − T_bay⁰)/Δt = G_eb (T_e − T_bay) + f_rad Q_eng − G_g (T_bay − T_out) − G_fw (T_bay − T_a) + Q_hood
G_g = ρ c_p (V̇₀ + k v),   Q_hood = Σ A q_hood(T_bay) （引擎盖壁板，线性响应，内侧 = 发动机舱空气）
```

| 参数 | 缺省 | 说明（典型值待核对） |
|---|---|---|
| C_e | 70 kJ/K | 缸体 + 机油 + 冷却液有效热容；停放余热慢时间常数 ≈ 56 min（文献/经验：熄火后约 0.5–1 h 冷却） |
| C_b | 15 kJ/K | 舱内空气 + 附件 |
| G_eb | 40 W/K | 机体与舱内空气/引擎盖/防火墙的对流 + 辐射 |
| V̇₀、k | 0.04 m³/s、0.004 m² | 前格栅通风 = V̇₀ + k v（怠速/行驶；15 m/s ≈ 0.10 m³/s） |
| G_fw | 2 W/K | 防火墙向座舱传热，0 = 不传 |
| 分配 | 散热器 0.5 / 机体 0.3 / 排气 0.2 | 和为 1 |
| `engineWarm` | 关；`warmTemperature` 90 ℃ | 起始机体温度；舱内空气起始 = 室外 + 0.3 ΔT；`engineOn=false` 后余热按两节点指数衰减 |

停放（`engineOn=false`）缺省不加热，行驶时开启；`engineWarm` 仅设初值。引擎盖面元用材料 `vehicle_hood`（钢板 0.8 mm + 内衬 10 mm，`h_in` 10），其外侧仍由面元能量平衡求解。

### 10.5 车漆

`vehicle_paint_{white,silver,black,red,blue,grey,black_cool}`：ρ_sol = 0.65 / 0.45 / 0.06 / 0.30 / 0.22 / 0.30 / 0.25（近红外冷黑漆），ε = 0.88（`refTir` 0.12，漆 0.85–0.9）。其他外表面发射率：玻璃 0.9、轮胎 0.9、塑料饰件 0.9、裸露金属 `vehicle_metal_bare` 0.2（ρ_sol 0.55，可选）。“车辆参数”折叠区里的“车漆颜色”下拉决定漆面组（含引擎盖）的光谱。

### 10.6 参数写入（`params.cabin` 扩展，向后兼容）

```
cabin.interior = { enabled, seatType, shellRadFraction, seatEffusivity,
                   dashboard|seat|wheel|lining: { share, capacity, hcA, radArea } }
cabin.engine   = { enabled, warm, warmTemperature, blockCapacity, bayCapacity, blockToBay, bayVent0, bayVentK,
                   radiatorShare, blockShare, exhaustShare, firewallG }
```

XML：`vcIntModel/vcIntShares/vcIntCapacities/vcIntHcA/vcIntRadArea/vcShellRadFraction/vcSeatEffusivity`、`vcEngModel/vcEngWarm/vcEngWarmTemp/vcEngCaps/vcEngBlockBay/vcEngBayVent/vcEngSplit/vcFirewallG`；`vehiclePart` 增加 4（引擎盖）。缺这些字段 = 旧行为。界面：对象导入对话框的“车辆参数”折叠区（车型/座舱容积、乘员数、停放/行驶、发动机开关与功率、热车、空调、通风关窗/缝/开窗、窗户膜、车漆颜色、座椅类型）。

### 10.7 输出

`vehicle_cabin_node=<n>.json`（分节点路径新增）：`interiorNodes[]`（温度、吸收太阳、对流到空气、壳体辐射得热、储热、残差）、`windowTransmitted_W`、`shellBoundaryTemperature_K`、`balance_W`（含 `firewallFromBay`，空气 + 全部内饰总残差）、`engine`（块/舱内空气温度、引擎盖内外表面、分项功率与独立闭合）；`faceteb_run.json` 的 `vehicleCabinSummary[]` 增加 `maxInteriorNodeTemperature_C`、`maxWindowTransmitted_W`、`maxEngineBlockTemperature_C`、`maxEngineBayTemperature_C`、`maxEngineRelativeClosure`。日志行 `VEHICLE_PARTS`。

### 10.8 验证与算例数字

**闭合**：座舱（空气 + 4 个内饰节点，含壁板热量）每节点相对残差 ≤ 3.4×10⁻¹³（每节点残差也单独输出，≤ 10⁻¹¹ W）；发动机（块 + 舱内空气，含防火墙与引擎盖壁板）相对残差 ≤ 1.0×10⁻¹²。**单元检查** `tools/test-vehicle-cabin.py`（扩展）：5 节点联立（高斯消元）稳态——φ = 0 与闭式解 `T_a = (Gv T_out + K T_w + Q_occ + Q_sol)/(Gv + K)`、`T_k = T_a + s_k Q_sol/hA_k` 一致（< 10⁻⁶ K），φ = 0.45 时节点、空气与总平衡残差 ≤ 5×10⁻¹² W；步长 60/600/3600 s 瞬态逐步闭合 ≤ 1.3×10⁻¹⁰ W；发动机熄火后余热衰减与两指数解析解（2×2 特征分解）比较：Δt = 10 s 误差 3.7×10⁻⁴、60 s 2.2×10⁻³（相对初始温差，后向欧拉的一阶误差），慢时间常数 55.8 min。`tools/test-vehicle-parts.mjs`：绑定规则表、OBJ 拆组（面元数保持）、XML 字段与旧行为、校验。

**CPU/GPU**：`feb_urban_vehicle_parts_park`（5 辆车、28 个节点）在 GPU 与 CPU（`HISTREAM_FACET_ENERGY_BACKEND=cpu`）能量路径下整场面元平均温度逐节点最大差 3.1×10⁻⁵ K（float32 均值量化），单面元最大 0.028 K（迭代噪声），座舱/内饰四节点/发动机块与舱内空气/车身/玻璃/底盘面积平均温度最大差 ≤ 6.7×10⁻⁶ K，窗户透射量完全一致（主机侧求解，两后端共用输入）；CPU 294 s、GPU 187 s。

**旧算例**：缺新字段的车辆材料走旧路径；`feb_urban`、`feb_crop`、`feb_urban_ship`、`feb_urban_vehicle_park/_drive/_ac`（基线 `vehicle_final`）全部输出文件逐字节不变（VoxelEB `veb_urban`、`veb_urban_ship_plate` 仅诊断 JSON 中的 `elapsedMs` 计时字段不同，其余逐字节不变）。上一轮车辆算例本身没有新字段所以不变；同样的场景用新建工程缺省（分节点内饰 + 发动机模型 + 分窗光谱）建立的新算例 `feb_urban_vehicle_parts_*` 与旧集总结果的差别见下表（量级：座舱峰值 −2.7 K、行驶 −1.5 K，内饰由一个节点拆为四个）。

**城市示例白天（节点 13–39，06:00–20:00，晴天，室外峰值 28 ℃）**，`feb_urban_vehicle_parts_park/_drive/_ac`：

| 车 | 座舱气温峰值 | 仪表台 | 座椅（接触温度） | 方向盘 | 内衬/门板 | 引擎盖外表面 | 前挡风/侧窗/后窗透射峰值（W） |
|---|---|---|---|---|---|---|---|
| 停放白漆晴面 | 47.6 ℃（+19.5 K） | 78.6 | 56.5（37.5） | 66.8 | 47.8 | 39.4 ℃ | 509 / 342 / 472 |
| 停放银漆晴面 | 50.4 ℃（+22.3 K） | 81.3 | 59.3（37.9） | 69.5 | 50.6 | 48.7 ℃ | 513 / 339 / 462 |
| 停放黑漆晴面 | 55.2 ℃（+27.2 K） | 85.8 | 64.0（38.6） | 74.1 | 55.5 | 65.4 ℃ | 503 / 336 / 460 |
| 停放黑漆树荫 | 38.0 ℃（+8.2 K） | 44.1 | 39.7（34.9） | 41.8 | 38.0 | 39.0 ℃ | 85 / 94 / 84 |
| 行驶 15 m/s、不开空调 | 41.2 ℃（+13.2 K） | 72.9 | 50.9（36.6） | 61.0 | 42.1 | 37.5 ℃ | 509 / 342 / 472 |
| 行驶 15 m/s、开空调 | 24.7 ℃（最大制冷 1.23 kW） | 61.3 | 39.1（34.8） | 49.3 | 32.5 | 37.5 ℃ | 509 / 342 / 472 |

（内饰为各节点峰值温度，℃；窗户透射量为全天各窗峰值，同一时刻前挡风 307–453 W。）漆面外表面：停放同一晴面站位引擎盖外表面 白 39.4 ℃、银 48.7 ℃、黑 65.4 ℃（黑比白高 26 K、比银高 17 K）；车身外表面全车面积平均 白 37.9 ℃、黑 48.1 ℃（高 10 K，含侧面与阴影面）。发动机：行驶（15 m/s）发动机块 99.8 ℃、发动机舱空气 66.9 ℃（比室外高 39 K）、排气份额 0.9 kW 入底盘；停放热车（`engineWarm`，起始 90 ℃）块温 44.0（第一节点）→ 33.2 → 27.0 → 23.9 → 22.7 → 22.2 ℃（室外 11.4 → 20.0 ℃），2.5 h 后仅比室外高 2 K。

**文献量级对照**（联网核对）：直晒仪表台 60–80 ℃、座椅 45–65 ℃、方向盘 45–70 ℃——白漆 78.6/56.5/66.8、银漆 81.3/59.3/69.5 在范围内（节点为集总质量，仪表台直晒表面温度会更高；黑漆仪表台 85.8 ℃ 略超出上沿）。Grundstein et al.（*BAMS* 2010 摘要）关窗停放座舱升温约 4 K/5 min、16 K/30 min、26 K/60 min，峰值多比室外高 20–30 K：本例晴面 +19.5 至 +27.2 K ✓（[AMS 摘要](https://headlines.ametsoc.org/2010/10/01/overheating-in-cars/)）。黑色车漆比白色高：ADAC 正午实测黑 65 ℃、白 44 ℃（> 20 K，[GTÜ 引述](https://www.gtue.de/blog/sicherheit-praxis/schwarz-oder-weiss-alles-ganz-sch%C3%B6n-hei%C3%9F)；车内仅差约 5 K）；广州日报曝晒 1 h 黑色引擎盖 62.5 ℃、白色 46.3 ℃（高 16 K，车厢仅差约 4 K，[来源](http://auto.cnr.cn/wxby/20171108/t20171108_524016851.shtml)）：本例引擎盖外表面黑比白高 26 K（略高于实测 16–21 K，峰值时刻低风速、本例白漆 ρ_sol 0.65 偏高，待核对），座舱高 7.6 K（文献 4–5 K，偏大，待核对）。熄火后发动机冷却约 0.5–1 h（经验值，非严格文献，待核对）。内饰与发动机分项参数均为典型值，未用实测标定。

**耗时**：停放/行驶/空调三个新算例 186.6/174.1/171.9 s，与旧集总算例 180.1/171.8/171.6 s 同量级（座舱与发动机方程在主机侧每节点解 ≤ 7×7 线性系统，耗时可忽略；总耗时由辐射/视域计算决定）；旧算例耗时与基线无可测差异。

### 10.9 局限

不做人体舒适性（烫伤、热指数、分级）；座舱无气流分层；透射太阳按用户份额分到各内饰（缺省 0.3/0.35/0.05/0.3），不按光线方向与内饰朝向分配，也无光线多次反射（`f_abs` = 0.8 之外逸出）；内饰与壳体的辐射交换经壁板内侧边界的辐射份额 φ 实现，节点自身与壳体的直接辐射耦合较弱，直射时的散热主要由有效 hA 承担；除引擎盖外的前翼子板/保险杠等车身面元内侧仍为座舱空气；排气热均摊到全部底盘薄壳面元（不是局部管路）；玻璃光谱是宽带 τ_sol，可见/近红外分谱透射不分别进入座舱；引擎盖与发动机舱的识别依赖前灯/尾灯组确定车头方向，缺这两个组时不拆组；车辆参数各车一份车身材料，行驶中速度/风向为常数；热车只设初值，停车后余热按固定参数衰减。文献对照中的实测数字来自网页摘要，未逐条核对原文。
