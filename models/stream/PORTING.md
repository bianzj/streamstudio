# STREAM 物理模块来源与改动

本目录是 STREAM 的唯一源码。2026-10 起物理基准为 `C:\work\tirt` 的 TiRT / TIRTEB 工作树（`tirteb_run.py`、`base/pipeline.py`、`base/aerodynamic.py`、`base/eb.py`、`base/heat_conduction.py`、`base/tirteb_radiation.py`、`base/node_radiosity.py`、`base/canopy_closure.py`）。第一版曾按 StreamEarth 的 `tirt_analytic` 移植（见历史提交），此后只在本目录开发。

## 模块对照

| 本目录 | 参考来源 | 处理 |
| --- | --- | --- |
| `physics/optics.*` | `base/leafopt.py` fluspect、`base/soilopt.py` BSM、`scifuns.cpp` calctav | 数值逐项保留；BSM 湿土于 2026-11-20 改为 SCOPE soilwat（k = 0..6 水膜 Poisson 混合、双程 tw），与 `soilopt._soilwat` 相差 < 2×10⁻⁶ |
| `physics/radiosity.*` | `base/node_radiosity.py` 的 `hom3` 调度、`base/canopy_closure.py` | 均匀冠层两层的 5 状态节点系统：交换因子、全阶短波/长波、有效发射率、方向多次散射 |
| `physics/canopy.*` | `base/canopy_closure.py` fractions/reflectance、`base/hotspot.py`、`base/row_closure.py`、`base/vnir.py` | 均匀冠层标准闭合：双尺度热点、两层光照叶近似、球面叶片一次散射系数 |
| `physics/shortwave.*` | `base/tirteb_radiation.py` `_plane_prepare_shortwave`、`base/radiation.py` 直散分配 | 默认 50 nm 分组（约 41 个波段）节点辐射度吸收，`spectralBinNm` 可设 1 做 1 nm 全谱；0.8/0.2 直散分配、晴空 DNI 限幅、PAR |
| `physics/energy.*` | `base/pipeline.py`、`base/aerodynamic.py`、`base/eb.py`、`base/biochemical.py`、`base/evapo.py` | TIRTEB 组件能量平衡、气动/气孔阻抗、温度修正、1/L 松弛求解 |
| `physics/heat_conduction.*` | `base/heat_conduction.py` | `surface` 7 h 温度历史卷积（与 TIRTEB `HISTORY_SECONDS` 相同）、`depth` 2 m 土柱（16 节点，向后欧拉） |
| `physics/brightness.h` | `base/brightness.py` / TiRT c1、c2 | Planck 辐亮度与反演亮温 |
| `forcing/*` | 无（TiRT 读 CSV 气象） | 气象强迫元数据驱动：ERA5 / ERA5-Land（原路径）、GLDAS 风格 3 h 网格、气象站 CSV，统一标准化为 ERA5 命名的 SI 标准量（`docs/forcing-metadata.md`） |
| `scene/classes.*`、`classes/classes.json` | `base/io.py` `read_defined`、`defined.txt` | IGBP 参数表，带单位说明 |
| `app/image.cpp` | 成像链路 | 逐像元 EB 时序与方向成像 |

## 能量平衡（`physics/energy.*`）

与 TIRTEB 现行默认值一致：

- **短波**：`shortwave_diffuse_fraction=0.2`；`-1` 时按 Erbs 反算（晴空指数含日地距离因子 E0，与 TiRT `radiation.py` 一致，2026-11-21 FIX C7；太阳在地平线以下时散射比取 1）。启用 `shortwave_enforce_solar_limit`：晴空 DNI（Meinel，Kasten–Young 大气质量、年内日地修正）为直射上限，超出的直射功率转为漫射，总短波保持不变。受光面不被照亮（SPA 天顶角 ≥ 90° 或坡面背光）时全部短波按漫射分配，不再丢弃直射部分；节点短波与冠层直射几何使用同一 SPA 太阳位置（2026-11-21 FIX C10）。
- **气孔温度修正**（2026-11-21 FIX B3）：所有 C3/C4 地类开启 Vcmax/Rd/Kc/Ko 温度修正（TiRT `tcor=1`、StreamEarth `Tcor=1`），`classes.json` 省略 `temperatureCorrection` 时缺省为 true。此前只有草地开启，且 PORTING 未将其列为有意差异。
- **kB⁻¹**（2026-11-21 FIX B4 说明）：`heatRoughnessKb` 只作用于裸土整体阻抗（LAI=0 全部、LAI<0.5 按 1−smoothstep 权重），LAI≥0.5 时由冠层阻抗网络决定，与 TiRT `_bulk_resistance`（z0h=0.1·z0 只用于裸土/城市）相同。
- **长波**：`hom3` 节点辐射度全阶，土壤—冠层互易；节点发射按光照/阴影温度四次方与面积份额混合。
- **气动**：Raupach `d`、`z0`（`G1=max(3.3,(Cs+Cr·LAI/2)^-1/2)`），冠层上方/叶边界/冠层内到土壤分段阻抗，每段上限 400 s m⁻¹；有效风速 `sqrt(u²+wstar²)`，`wstar` 由场景显热；LAI<0.5 时按 smoothstep 混合到裸土；稳定层结全廓线统一 `xeff=min(1/L,1/zref)`。
- **气孔**：Farquhar/Ball-Berry，净光合 ≤0 时不再取 625000 s m⁻¹，而是 `stomatal_g0=0.01 mol m⁻² s⁻¹` 下限；阻抗换算用 `rho_air/M_air`。
- **温度修正**：默认 `temperature_method=robust`（0.8 阻尼、单步上限 5 K，最多 50 次迭代），可设 `direct`（含潜热导数的近似导数）；温度保护范围 175–380 K（2026-11-24 起；原下限 240 K 会把极寒像元钳死且不收敛）。过境节点在 `maxIterations` 内未闭合时，继续最多 4 × `maxIterations` 次迭代的 1/L 括号法回退（先在冻结的 1/L 下用 0.25 欠松弛的温度/阵风更新闭合残差，再对 1/L 扩展 + 二分；不比原终点更好则保留原终点）；移植时该回退只作用于过境节点，起转节点不用。LAI 有效范围 0–20（nodata、负值、>20 视为无效输入）。同化的 EB 雅可比对 LAI 包含气动阻抗、位移高度、粗糙度、u*、rac 与叶边界层的偏导（`aerodynamicResistances` 对标量类型模板化），1/L 与阵风仍冻结。
- **稳定度**：`1/L` 松弛更新，限制在 ±5 m⁻¹（2026-11-24：松弛系数自适应，初值 0.5，1/L 误差变号时减半、下限 0.02，同号时每步增长 10%、上限 0.5。固定 0.5 对强不稳定冠层——u* 约 0.05–0.2 m s⁻¹、LAI 2–3、目标映射斜率远小于 −1——会进入 6 周期极限环，50 次迭代不收敛），目标仅使用 H（`-κgH/(ρcpTa u*³)`）。迭代中要求组分最大能量残差 ≤0.5 W m⁻²、阵风显热残差 ≤1 W m⁻²、稳定度残差 ≤`1e-4+0.02|target|` m⁻¹。返回前重新计算，最终验收分别使用 5 W m⁻²、2 W m⁻²及两倍上述稳定度容差；最终 QA 与迭代提前退出条件不同。
- **土壤热**：默认 `surface`（7 h 温度历史卷积，保存真实时间间隔）；`depth` 为 2 m 土柱，表层通量含半格储热；`direct` 为 `G = f·Rn`。
- **初值与起转**（2026-11-21 FIX B9/B10 更正）：能量平衡从过境前第 `temperature.spinupHours` 个整点时次（**缺省 7，2026-11-24 由 24 h 改为 7 个时次**；起点相对过境往前数，不是固定的 00 UTC 或 24 h；向前取整到整点：最后一个早于“过境前 5 min”的整点往前数 N 个）开始逐时推进。改 7 的理由：2 m 气温已对结果约束很强，不需要起转太久；7 与 TIRTEB 表面热历史保留的 7 h 对应；相对过境计数，与经度无关。ERA5 数据包不覆盖该起点时退回数据包连续覆盖的最早时次并给出警告，`scene_summary.json` 的 `temperature.notes` 写“起转不足”，`spinupNodesRequested`/`spinupNodesUsed`/`spinupHoursUsed`/`marchStart` 记录实际起转。**初值（2026-11-24 改为全部用空气温度）**：土壤（光照/阴影）、叶片、土壤热历史（半无限方法 `history`）与 `depth` 土柱各层（同温）的初温都取起转起点时刻的 2 m 气温 `t2m`；起点 `t2m` 无效时退回起点后第一个有效 `t2m`，再退回首个模拟日（起点起至多 24 h）平均气温并在日志警告。理由：土壤温度并不确切已知（用户决定：EB 所有初值都用空气温度，不用 ERA5 `skt`/`stl1`/`stl2` 初始化土壤）；起转只有 7 个左右的节点，起点气温比“当天平均气温”更接近起点的实际状态。**旧做法**：土壤取起转起点的 `skt`（无效时退回首日平均气温），`depth` 土柱按 `stl1`/`stl2` 初始化，叶片取起点起至多 24 h 平均气温。`inverseObukhov`、`sceneSensible` 等其他初值不变。此前文档写“土壤与叶片取首日平均气温”与代码（`image.cpp` initialState）不符；此前起点固定为过境当天 00 UTC，东亚上午过境只积分约 2.5 h；2026-11-21 改为过境前 24 h，2026-11-24 改为过境前 7 个整点时次。
- **水体（17）**：不求能量平衡，四组分取过境时刻插值的 ERA5 `skt`（`skt` 现为可选变量；数据包没有 `skt` 时水体改取过境时刻 2 m 气温并警告，`scene_summary.json` 的 `temperature.notes` 记“水体降级”）。

## 方向成像（`app/image.cpp`）

- **光学**：`canopyReflectance` = 一次散射（球面叶片系数、热点四组分）+ 节点辐射度多次散射（直射与天空）。不再使用 StreamEarth 的二流核。
- **热红外**：`fractions × 发射率 + homThermalScatter` 得到四组分有效发射率，按 Planck 在波段中心波长合成发射辐亮度，加反射天空项 (1 − Σw)·B(T_sky)（T_sky 由下行长波换算，见 README“热红外观测算子”）后反演亮温；方向温度 = 发射辐亮度/有效发射率反演，不含反射天空。太阳在局地地平线以下时，光照组分的权重并入阴影组分。半高宽只记录，不参与积分（与 TiRT 观测一致）。
- **波段发射率**：叶片 `1−reflTir−transTir`；土壤/不透水面/冰雪/水体用 `classes.json` 的 `ground.emissivity`；EB 宽带叶片发射率另取 `leafEmissivity=0.985`，土壤干端发射率与成像共用地类值（配置 `energy.soilEmissivity` 时两者同时替换）。土壤发射率随像元含水量在干端值与该值的饱和端之间插值（`physics/soil_emissivity.h`，与 BSM 共用 `SMC ≥ 0.05`、`SMCp = 0.25` 的几何，缺省增益 +0.03、上限 0.995，`energy.soilEmissivityWet` 可覆盖饱和值）；不透水面与冰雪不随含水量变化。

## 与 TIRTEB 的有意差异

1. **短波光谱分组**：EB 默认按 50 nm 分组（约 41 个波段），与 TIRTEB 当前默认一致；`energy.spectralBinNm` 可设为 1 做 1 nm 全谱（叶片/土壤反射率取组内均值，太阳谱按组积分）。成像按波段中心单色计算。
2. **土壤热历史窗**：STREAM 与 TIRTEB 均只保留最近 7 h（`physics/heat_conduction.cpp` 的 `kHistorySeconds`）；更早的清晨温度不再进入表面热通量卷积。
3. **城市**：保留不透水地面 + 绿地冠层的四组分简化，不移植 TIRTEB 的屋顶/墙面/街道多组分与城市辐射度。城市结果不能与 TIRTEB 城市分支逐像元比较。
4. **几何**：冠层只支持均匀冠层与裸地；垄行、离散树冠（`physics/effective_lai.cpp` 已编译但未调用，地类表中给出 `treeStand`/`canopyHeight`/`crownShape`/`rows` 等参数时运行日志警告并在 `scene_summary.json` 的 `canopyStructure` 中记录“按均质冠层处理”）与 DEM 光线追踪未移植。地形（2026-11-21 FIX B-S4 更正，此前写“单坡、复合地形未移植”与代码不符）：已实现单一坡（single）、DEM 面元（facet）、山包复合坡（hill）与山脊复合坡（ridge）——短波按坡面入射与天空视角（hill 为锥面方位解析积分，`forcing/pixel_forcing.cpp`），热红外在坡面局地坐标中计算（天顶角与相对方位角，2026-11-21 FIX S-2），hill/ridge 按 12 方位面元或两翼加端面合成（`app/thermal_view.cpp`）；LST/LSCT 反演与同化使用同一地形观测算子（2026-11-21 FIX S-1）。不计他山遮挡与周围地面反射。
5. **冠层上方阻抗 rar**：稳定度修正项 phstar 放在 1/(κu*) 括号内（2026-11-20 修正；TIRTEB `aerodynamic.py` 与 SCOPE resistances.m 把它直接减在 s m⁻¹ 量纲的阻抗上）。中性层结无差别；北京 Landsat 300 m 示例白天不稳定像元组分温度平均降低约 0.02–0.27 K，个别像元达 5–6 K。
6. **联合后备求解**：标量温度/稳定度不收敛时本版只标记 `converged=false` 并保留结果，尚未移植 TIRTEB 的信赖域 T/H/1/L 联合后备。
7. **土壤阻抗默认值**：`input_tirteb.csv` 的 `soil_rss=200` 常数模式；STREAM 同为常数 200，可在配置中改为 `sellers` 含水量关系。土壤水分仍驱动 BSM 光谱；用户输入的土壤水大于 1 时按百分数除以 100，BSM 内限制在 0.05–0.55。
8. **叶片发射率**：EB 宽带取 0.985；成像按 `1−reflTir−transTir`（现表为 0.975）。TIRTEB 同样区分宽带与观测波段发射率。

## 气象强迫（`forcing/*`；ERA5 为用例）

- 2026-11-24 起变量名/单位/高度/时间步由气象强迫元数据描述（`forcing_metadata`、`forcing_source`、`forcing_resample`、`station_source`，规格见 `docs/forcing-metadata.md`）；ERA5 / ERA5-Land 布局（预设 `era5`、`era5-land`）仍走原读取路径，结果逐字节不变，以下细节指这一原路径。GLDAS 预设未经真实数据验证。

- 数据包：任意时段逐小时数据，可多文件；登记时写 `kind = stream-era5-pack` 索引，`complete` 表示必需变量逐小时连续，`fullYear` 表示覆盖整年。
- 必需 `u10 v10 t2m d2m sp ssrd strd`，可选 `skt`（仅水体像元用）、`stl1 stl2`（不再使用，可有可无）、`swvl1 z`（2026-11-24 前 `skt stl1` 为必需）；兼容 CDS 新旧 NetCDF 与 ERA5-Land 累积辐射差分。
- 空间双线性、时间线性；短波按 `通量/cos(太阳天顶角)` 插值再乘当前 `cos`；水汽压由露点按 Tetens 换算。
- 高程订正（有 `z` 且给 DEM）：温度 6.5 K km⁻¹、气压压高公式，水汽压不变且不超过饱和值。

## 地类参数（`classes/classes.json`）

数值来自 TiRT/TIRTEB 的 `read_defined` 默认值与 StreamEarth 地类表，以下为有意调整：

- 土壤：`emissivity=0.955`（干端值，实际发射率按像元含水量升到饱和端）、`conductivity=1.55`、热惯量 1418.4（`volumetricCapacity` 由 `I²/λ` 换算为 1.298e6）、`surfaceResistance=200` 常数模式。
- 城市（13）：不透水地面（反射率 0.15、发射率 0.95、表面阻抗 3662.86 s m⁻¹、热惯量 2000、导热率 1.5）+ 输入 LAI 的城市绿地（C3，Vcmax 60）。
- 冰雪（15）：地面反射率 0.75、发射率 0.985、表面阻抗 100 s m⁻¹、导热率 0.5、热惯量 600；不进行光合作用。

  - 2026-11-24：冰雪类的组分温度上限 273.15 K，多余能量记为融化热汇（`energy.iceMelt`，缺省开启；TiRT/TIRTEB 没有此限制，这是 STREAM 的物理新增）；`leafEmissivity` 只作用于 EB，成像/反演用地类 1 − reflTir − transTir。
- 裸地（16）、未分类（0）：光合途径为 `none`（代码中为 0），按无气孔处理（叶片阻抗 625000 s m⁻¹）；裸地叶片结构参数 N = 1.5（PROSPECT 要求 N ≥ 1，加载时校验）。
- 水体（17）：`ground.surface = water`，按平面地面处理，发射率 0.99 不随土壤水分变化，不建 Hapke 角度表。
- 填充值 18 与 255 一律视为填充。

## 未移植

植被生长（CTSM）、水碳收支、EnKF 同化、降水、垄行与树冠结构参数（`effective_lai.cpp` 未接入，见上文“几何”）、六组分城市、`legacy` 能量平衡方案、TIRTEB 的联合信赖域后备。（2026-11-21 FIX B-S4：删去此处“参数化山体与单一坡面”，二者已实现。）
