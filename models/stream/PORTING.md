# STREAM 物理模块来源与改动

本目录是 STREAM 的唯一源码。2026-10 起物理基准为 `C:\work\tirt` 的 TiRT / TIRTEB 工作树（`tirteb_run.py`、`base/pipeline.py`、`base/aerodynamic.py`、`base/eb.py`、`base/heat_conduction.py`、`base/tirteb_radiation.py`、`base/node_radiosity.py`、`base/canopy_closure.py`）。第一版曾按 StreamEarth 的 `tirt_analytic` 移植（见历史提交），此后只在本目录开发。

## 模块对照

| 本目录 | 参考来源 | 处理 |
| --- | --- | --- |
| `physics/optics.*` | `base/leafopt.py` fluspect、`base/soilopt.py` BSM、`scifuns.cpp` calctav | 数值逐项保留 |
| `physics/radiosity.*` | `base/node_radiosity.py` 的 `hom3` 调度、`base/canopy_closure.py` | 均匀冠层两层的 5 状态节点系统：交换因子、全阶短波/长波、有效发射率、方向多次散射 |
| `physics/canopy.*` | `base/canopy_closure.py` fractions/reflectance、`base/hotspot.py`、`base/row_closure.py`、`base/vnir.py` | 均匀冠层标准闭合：双尺度热点、两层光照叶近似、球面叶片一次散射系数 |
| `physics/shortwave.*` | `base/tirteb_radiation.py` `_plane_prepare_shortwave`、`base/radiation.py` 直散分配 | 默认 50 nm 分组（约 41 个波段）节点辐射度吸收，`spectralBinNm` 可设 1 做 1 nm 全谱；0.8/0.2 直散分配、晴空 DNI 限幅、PAR |
| `physics/energy.*` | `base/pipeline.py`、`base/aerodynamic.py`、`base/eb.py`、`base/biochemical.py`、`base/evapo.py` | TIRTEB 组件能量平衡、气动/气孔阻抗、温度修正、1/L 割线求解 |
| `physics/heat_conduction.*` | `base/heat_conduction.py` | `surface` 7 h 温度历史卷积（TIRTEB 为 48 h）、`depth` 2 m 土柱（16 节点，向后欧拉） |
| `physics/brightness.h` | `base/brightness.py` / TiRT c1、c2 | Planck 辐亮度与反演亮温 |
| `forcing/*` | 无（TiRT 读 CSV 气象） | 直接读 ERA5 / ERA5-Land NetCDF |
| `scene/classes.*`、`classes/classes.json` | `base/io.py` `read_defined`、`defined.txt` | IGBP 参数表，带单位说明 |
| `app/image.cpp` | 成像链路 | 逐像元 EB 时序与方向成像 |

## 能量平衡（`physics/energy.*`）

与 TIRTEB 现行默认值一致：

- **短波**：`shortwave_diffuse_fraction=0.2`；`-1` 时按 Erbs 反算。启用 `shortwave_enforce_solar_limit`：晴空 DNI（Meinel，Kasten–Young 大气质量、年内日地修正）为直射上限，超出的直射功率转为漫射，总短波保持不变。
- **长波**：`hom3` 节点辐射度全阶，土壤—冠层互易；节点发射按光照/阴影温度四次方与面积份额混合。
- **气动**：Raupach `d`、`z0`（`G1=max(3.3,(Cs+Cr·LAI/2)^-1/2)`），冠层上方/叶边界/冠层内到土壤分段阻抗，每段上限 400 s m⁻¹；有效风速 `sqrt(u²+wstar²)`，`wstar` 由场景显热；LAI<0.5 时按 smoothstep 混合到裸土；稳定层结全廓线统一 `xeff=min(1/L,1/zref)`。
- **气孔**：Farquhar/Ball-Berry，净光合 ≤0 时不再取 625000 s m⁻¹，而是 `stomatal_g0=0.01 mol m⁻² s⁻¹` 下限；阻抗换算用 `rho_air/M_air`。
- **温度修正**：默认 `temperature_method=robust`（0.8 阻尼、单步上限 5 K，最多 50 次迭代），可设 `direct`（含潜热导数的近似导数）；温度保护范围 240–380 K。
- **稳定度**：内层残差 ≤1e-4 W m⁻² 且阵风 H 残差 ≤1e-4 才进入符号区间；`1/L` 只用 H（`-κgH/(ρcpTa u*³)`），区间割线限 10%–90%，容差 `1e-7+1e-6|target|+|c|·1e-4/u*³`。
- **土壤热**：默认 `surface`（7 h 温度历史卷积，保存真实时间间隔）；`depth` 为 2 m 土柱，表层通量含半格储热；`direct` 为 `G = f·Rn`。
- **初值**：土壤与叶片取首日平均气温；热历史与土柱从该温度自洽推进。
- **水体（17）**：不求能量平衡，四组分取过境时刻插值的 ERA5 `skt`。

## 方向成像（`app/image.cpp`）

- **光学**：`canopyReflectance` = 一次散射（球面叶片系数、热点四组分）+ 节点辐射度多次散射（直射与天空）。不再使用 StreamEarth 的二流核。
- **热红外**：`fractions × 发射率 + homThermalScatter` 得到四组分有效发射率，按 Planck 在波段中心波长合成辐亮度，再反演亮温；方向温度 = 辐亮度/有效发射率反演。半高宽只记录，不参与积分（与 TiRT 观测一致）。
- **波段发射率**：叶片 `1−reflTir−transTir`；土壤/不透水面/冰雪用 `classes.json` 的 `ground.emissivity`；EB 宽带发射率另取 `leafEmissivity=0.985`、`soilEmissivity=0.955`。土壤发射率随像元含水量在干端值与该值的饱和端之间插值（`physics/soil_emissivity.h`，与 BSM 共用 `SMC ≥ 0.05`、`SMCp = 0.25` 的几何，缺省增益 +0.03、上限 0.995，`energy.soilEmissivityWet` 可覆盖饱和值）；不透水面与冰雪不随含水量变化。

## 与 TIRTEB 的有意差异

1. **短波光谱分组**：EB 默认按 50 nm 分组（约 41 个波段），与 TIRTEB 当前默认一致；`energy.spectralBinNm` 可设为 1 做 1 nm 全谱（叶片/土壤反射率取组内均值，太阳谱按组积分）。成像按波段中心单色计算。
2. **土壤热历史窗**：STREAM 与 TIRTEB 均只保留最近 7 h（`physics/heat_conduction.cpp` 的 `kHistorySeconds`）；更早的清晨温度不再进入表面热通量卷积。
3. **城市**：保留不透水地面 + 绿地冠层的四组分简化，不移植 TIRTEB 的屋顶/墙面/街道多组分与城市辐射度。城市结果不能与 TIRTEB 城市分支逐像元比较。
4. **几何**：只支持均匀冠层与裸地；垄行、离散树冠、单坡、复合地形、DEM 光线追踪未移植。
5. **联合后备求解**：标量温度/稳定度不收敛时本版只标记 `converged=false` 并保留结果，尚未移植 TIRTEB 的信赖域 T/H/1/L 联合后备。
6. **土壤阻抗默认值**：`input_tirteb.csv` 的 `soil_rss=200` 常数模式；STREAM 同为常数 200，可在配置中改为 `sellers` 含水量关系。土壤水分仍驱动 BSM 光谱；用户输入的土壤水大于 1 时按百分数除以 100，BSM 内限制在 0.05–0.55。
7. **叶片发射率**：EB 宽带取 0.985；成像按 `1−reflTir−transTir`（现表为 0.975）。TIRTEB 同样区分宽带与观测波段发射率。

## ERA5（`forcing/*`）

- 数据包：任意时段逐小时数据，可多文件；登记时写 `kind = stream-era5-pack` 索引，`complete` 表示必需变量逐小时连续，`fullYear` 表示覆盖整年。
- 必需 `u10 v10 t2m d2m sp ssrd strd skt stl1`，可选 `stl2 swvl1 z`；兼容 CDS 新旧 NetCDF 与 ERA5-Land 累积辐射差分。
- 空间双线性、时间线性；短波按 `通量/cos(太阳天顶角)` 插值再乘当前 `cos`；水汽压由露点按 Tetens 换算。
- 高程订正（有 `z` 且给 DEM）：温度 6.5 K km⁻¹、气压压高公式，水汽压不变且不超过饱和值。

## 地类参数（`classes/classes.json`）

数值来自 TiRT/TIRTEB 的 `read_defined` 默认值与 StreamEarth 地类表，以下为有意调整：

- 土壤：`emissivity=0.955`（干端值，实际发射率按像元含水量升到饱和端）、`conductivity=1.55`、热惯量 1418.4（`volumetricCapacity` 由 `I²/λ` 换算为 1.298e6）、`surfaceResistance=200` 常数模式。
- 城市（13）：不透水地面（反射率 0.15、发射率 0.95、表面阻抗 3662.86 s m⁻¹、热惯量 2000、导热率 1.5）+ 输入 LAI 的城市绿地（C3，Vcmax 60）。
- 冰雪（15）：地面反射率 0.75、发射率 0.985、表面阻抗 100 s m⁻¹、导热率 0.5、热惯量 600；不进行光合作用。
- 裸地（16）、未分类（0）：光合途径为 0，按无气孔处理。
- 填充值 18 与 255 一律视为填充。

## 未移植

植被生长（CTSM）、水碳收支、EnKF 同化、降水、参数化山体与单一坡面、垄行与树冠结构参数、六组分城市、`legacy` 能量平衡方案、TIRTEB 的联合信赖域后备。
