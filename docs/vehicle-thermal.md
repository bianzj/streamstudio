# 车辆温度模拟（FacetEB）

本文是 FacetEB「车辆温度模拟」模块的设计、使用与局限说明：停放与行驶两种工况，每辆车一个集总座舱节点，输出座舱空气温度、内饰节点温度、车身/玻璃/底盘各面元温度与座舱能量平衡分量。**人体适宜性（烫伤、热指数、分级、儿童/宠物滞留）本期不做，见第 8 节“后续工作”。**

默认全部关闭：旧工程、旧算例的 FacetEB 输出逐字节不变；只有材料 `type: "Vehicle"` 且 `vehiclePart` 有取值时才启用。

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
