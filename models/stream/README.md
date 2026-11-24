# STREAM 遥感影像模拟引擎（stream.exe）

Stream Studio 中与 HiStream 三维引擎相互独立的影像级引擎：一次运行对应一个过境时刻，输出整景光学反射率、热红外亮温与方向温度。组分温度有两种来源：

- **RT**：已知组分温度，直接给出 Tss/Tsh/Tcs/Tch（常数、单波段/24 波段栅格，或 4 波段 `components.tif`）；
- **EB**：未知温度，由逐小时气象强迫（一天到一年的数据包；以 ERA5 / ERA5-Land 为用例，见下文“气象强迫来源”一节）驱动，从过境前 `temperature.spinupHours` 个整点时次（缺省 7；2026-11-21 FIX B10 起相对过境往前启动，此前固定从过境当天 00 UTC 起算；2026-11-24 缺省由 24 h 改为 7 个时次）逐时次求解四组分能量平衡到过境时刻，再成像。物理与默认值对齐 TiRT/TIRTEB：`hom3` 节点辐射度全阶长波与短波、均匀冠层标准热点闭合、Raupach 气动阻抗（含对流阵风与 400 s m⁻¹ 分段限幅）、Farquhar 气孔 `g0` 下限、1/L 自适应松弛稳定度、7 h 表面热历史或 2 m 土柱。

本目录是 STREAM 的唯一源码；物理基准、模块对照以及与 TIRTEB 的有意差异见 [PORTING.md](PORTING.md)。

## 目录

| 目录 | 内容 |
| --- | --- |
| `physics/` | 纯计算，不读写文件：PROSPECT/BSM（`optics`）、冠层闭合与热点（`canopy`）、`hom3` 节点辐射度（`radiosity`）、TIRTEB 短波分配与吸收（`shortwave`）、四组分能量平衡（`energy`）、土壤热（`heat_conduction`）、亮温（`brightness.h`）、SPA（`spa`） |
| `forcing/` | ERA5 数据包登记与读取（`era5`）、像元气象的空间/时间插值（`pixel_forcing`）、UTC 时间（`utc`） |
| `scene/` | 网格与 GeoTIFF 读写（`raster`）、地类参数表（`classes`） |
| `app/` | 配置 `stream-image-config` v2（`config`）、计算链路（`image`）、命令行入口（`main`） |
| `classes/classes.json` | IGBP 地类参数表（冠层、叶片光学、光合、地面、空气动力学），带单位说明 |
| `defined/` | PROSPECT/BSM 系数 `optipar_new.csv`、参考太阳光谱 `Esun_.dat`/`Esky_.dat` |

## 构建

`npm run build:engine`（`tools/build-engine.ps1`）先编译 HiStream，再编译本目录。也可单独编译：

```powershell
cmake -S models/stream -B build/stream -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build/stream --config Release
```

依赖 vcpkg 的 GDAL 与 netCDF（与 HiStream 共用同一 vcpkg，因此运行库只部署一份）。编译后 `defined/` 与 `classes/classes.json` 复制为运行目录下的 `stream-defined/`。

## 命令行

```text
stream run <run-config.json>
stream era5-register <pack.json> [--product auto|era5|era5-land] <file.nc>...
```

配置由 `server/stream/config.mjs` 生成；进度以 `STREAM\tprogress\t<百分比>\t<阶段>` 写到 stdout，日志行以 `[STREAM]` 开头。

### stream-image-config v2

```jsonc
{
  "kind": "stream-image-config", "schemaVersion": 2,
  "outputDir": "D:\\out\\image01_...", "dataDir": "<engine>\\stream-defined",
  "overpass": "2019-07-15T02:30:00Z",
  "surface": { "type": { "file": "..." }, "lai": { "file": "..." }, "soilMoisture": { "value": 0.25 }, "dem": { "file": "..." } },
  "view": { "vza": { "value": 0 }, "vaa": { "value": 0 } },
  "optical": { "enabled": true, "bands": [{ "name": "B4", "center": 655, "fwhm": 37 }] },
  "thermal": { "enabled": true, "bands": [{ "name": "B10", "center": 10.9, "fwhm": 0.6 }] },
  "temperature": { "mode": "rt", "components": { "file": "components.tif" },
                   "energy": { "soilEmissivityModel": "hapke", "surfaceTemperatureNormalisation": "retrieval" } },
  // 或 { "mode": "eb", "era5": { "pack": "...\\pack.json" }, "soilMoisture": { "source": "era5" },
  //      "elevationCorrection": true, "energy": { "scheme": "tirt", "soilHeatMethod": "surface",
  //      "temperatureMethod": "robust", "diffuseFraction": 0.2, "enforceSolarLimit": true,
  //      "soilResistance": { "method": "constant", "value": 200 } } }
  "skyLongwave": { "mode": "auto" },   // 可选：反射天空项的下行长波，见下文“热红外观测算子”
  "region": { "startRow": 0, "endRow": 100, "startColumn": 0, "endColumn": 100 },
  "threads": 0
}
```

`view.vaa` 是从像元指向传感器的方位角（北起顺时针，与 SPA 太阳方位同一约定），VAA = 太阳方位角时为热点方向；“传感器看向像元”的方位需先加 180°。VZA、VAA、纬度、经度为常数时须分别在 [0, 90)、[−360, 360]、[−90, 90]、[−180, 360] 内，否则配置报错。

### 热红外观测算子

正向成像、LST/LSCT 反演与同化共用 `app/thermal_view.cpp`：四组分权重 w_k（TiRT 比例 × 发射率 + `hom3` 多次散射），冠层顶亮温

L = Σ w_k B(T_k) + (1 − Σ w_k) · B(T_sky)，T_sky = (L↓/σ)^¼

第二项是各向同性天空长波的反射（Kirchhoff：方向—半球反射率 = 1 − 方向有效发射率，2026-11-20 起加入）。`tir_surface_temperature.tif`（LST 产品约定）仍为 Σ w_k B(T_k) / ε，不含反射天空。L↓ 来源由 `skyLongwave` 决定：

| 配置 | L↓ |
| --- | --- |
| 省略或 `{ "mode": "auto" }` | EB：过境时刻气象强迫的 `strd`；RT：Swinbank 晴空公式 5.31×10⁻¹³ T⁶，T 取阴影叶片温度（无叶取阴影/光照土壤温度）代替气温；只做 LST 反演且没有组分温度时取该像元观测亮温的均值 |
| `{ "value": 350 }` 或 `{ "file": "...", "band": 1 }` | 指定常数 / 同网格栅格（W m⁻²，0 表示该像元不计） |
| `{ "mode": "none" }` | 不计反射天空（2026-11-20 之前的行为） |

L↓ 由背景状态一次确定，反演/同化改变组分温度后不再重算，保证反演与其结果的正向图像用同一算子。宽带 L↓ 折算为黑体天空温度会高估大气窗区（8–13 μm）的天空辐亮度，对 ε≈0.97 的地表约多出 0.1–0.2 K；需要更精确时请给出窗区等效的 L↓。`scene_summary.json` 的 `reflectedSky` 记录来源。

太阳在（局地坡面）地平线以下时没有光照组分：光照土壤/叶片的权重并入阴影组分（与光学路径一致），有效发射率不变。`soilEmissivityModel`、`soilEmissivityK`、`surfaceTemperatureNormalisation`、`soilEmissivity`、`soilEmissivityWet` 写在 `temperature.energy` 下，EB 与 RT 两种模式都读取。

### 能量平衡参数（`energy`）

| 键 | 默认 | 含义 |
| --- | --- | --- |
| `scheme` | `tirt` | 仅接受 `tirt`（兼容旧值 `tirt-analytic`） |
| `soilHeatMethod` | `surface` | `surface` 7 h 温度历史；`depth` 2 m 土柱；`direct` 净辐射比例 |
| `temperatureMethod` | `robust` | `robust`（0.8 阻尼牛顿，单步最多 5 K）或 `direct`（含潜热导数的近似导数） |
| `diffuseFraction` | `0.2` | 直/散分配；`-1` 为 Erbs 反算 |
| `enforceSolarLimit` | `true` | 晴空 DNI 上限，超出直射转漫射 |
| `soilResistance.method` | `constant` | `constant` 或 `sellers`（含水量关系） |
| `soilResistance.value` | `200` | 常数土壤阻抗 s m⁻¹；须为 (0, 100000] 的数值，否则报错（2026-11-21 FIX B-S1） |
| `stomatalG0` | `0.01` | 气孔导度下限 mol m⁻² s⁻¹ |
| `minWindSpeed` / `maxAerodynamicResistance` | `0.05` / `400` | 风与分段气动阻抗限制 |
| `referenceHeight` | `15` | 气象参考高度 m（冠层更高时抬到冠层顶以上 0.1 m） |
| `soilEmissivity` / `leafEmissivity` | 地类值 / `0.985` | EB 宽带发射率；`soilEmissivity` 是干端值，给出时同时替换 `classes.json` 中所有土壤地类的 `ground.emissivity`，能量平衡与成像/反演共用；省略时两者都用地类值（表内土壤均为 0.955） |
| `soilEmissivityWet` | 干端 +0.03（≤0.995） | 饱和土壤发射率；土壤发射率按像元含水量在干端值与该值之间插值（见 `physics/soil_emissivity.h`） |
| `soilConductivity` / `soilVolumetricCapacity` | `1.55` / `1.298e6` | 土壤热参数（仅土壤地类） |
| `soilHeatFraction` | `0.25` | 仅 `direct` 方法使用 |
| `spectralBinNm` | `50` | EB 短波分组宽度（nm）；默认 50 nm（约 41 个波段），可设 `1` 做 1 nm 全谱，或设 10 折中 |
| `maxIterations` | `50` | 温度与 1/L 联合迭代上限 |
| `maxStabilityIterations` | — | 已不使用（温度与 1/L 共用 `maxIterations`），保留只为兼容旧配置 |
| `heatRoughnessKb` | 地类值 | 裸土整体阻抗的 kB⁻¹=ln(z0/z0h)；只作用于 LAI<0.5 的裸土路径（LAI≥0.5 无效，与 TiRT 相同，2026-11-21 FIX B4 说明） |

起转节点数 `temperature.spinupHours`（与 `era5` 同级，**缺省 7**，范围 0–240，单位是逐小时气象强迫的**时次个数**，不是固定时刻或固定时长；2026-11-21 FIX B10 引入，2026-11-24 缺省由 24 h 改为 7 个时次）：能量平衡从过境前第 N 个整点时次开始逐时推进。取整规则（向前取整到整点，沿用 B10 的整点实现）：先取严格早于“过境前 5 min”的最后一个整点（过境 02:55 → 02:00；过境 03:00 → 02:00；过境 03:06 → 03:00），再往前数 N 个整点（含它自己），该整点就是起点；时次序列为起点…该整点，加上过境时刻本身，共 N+1 个时次。例：过境 02:55，N=7 → 起点前一日 20:00，共 8 个时次；起点永远相对过境往前数，不是固定的 00 UTC 或固定的 24 h。N=0 只求解过境时刻 1 个时次；非整数向上取整。数据包不覆盖起点时从数据包连续覆盖的最早时次开始并警告（ERA5-Land 起点前还需一小时），同时在 `scene_summary.json` 的 `temperature.notes` 写“起转不足”提示；过境时刻之前没有任何整点时次时（N>0）报错。`scene_summary.json` 记录 `spinupNodesRequested`/`spinupNodesUsed`（不含过境时刻的整点时次数）、`ebNodeCount`（含过境时刻）、`marchStart`、`spinupHoursUsed`（过境减起点，小时）。缺省 7 的理由：2 m 气温已对结果约束很强，不需要起转太久；7 与 TIRTEB 表面热历史保留的 7 h 对应（过境时历史恰好填满）；相对过境计数，与经度无关。兼容性：旧工程未写该字段时，缺省由 24 h 改为 7 个时次，结果会变化（见 docs/release-next-draft.md）；显式写了值的工程仍按其值，但含义由“小时”变为“时次个数”，过境不在整点时相差最多 1 个时次（例：过境 02:55、值 24，旧版 25 个整点时次，现为 24 个）；旧版的小数值现在向上取整。初值（2026-11-24 起）：土壤（光照/阴影）、叶片、土壤热历史（半无限方法的 `history`）与 `depth` 土柱各层的初温**全部取起转起点时刻的 2 m 气温 `t2m`**（土柱各层同温）；起点 `t2m` 无效时退回起点后第一个有效 `t2m`，再退回首个模拟日（起点起至多 24 h）的平均气温，并在日志警告。不再用 ERA5 `skt`/`stl1`/`stl2` 初始化土壤：土壤温度并不确切已知，初值统一用空气温度。起转只有几个节点，起点气温比“当天平均气温”更接近起点的实际状态。旧做法（2026-11-24 之前）：土壤取起点 `skt`、`depth` 土柱按 `stl1`/`stl2`、叶片取起点起至多 24 h 的平均气温。兼容性：旧工程重算结果会变化（见 docs/release-next-draft.md）；`inverseObukhov`、`sceneSensible` 等其他初值不变；水体像元仍取过境时刻的 ERA5 `skt`（数据包没有 `skt` 时水体改取过境时刻 2 m 气温并警告，`notes` 记“水体降级”）。

地形（`surface.terrain` 为 single / facet / hill / ridge）时，水平气象强迫短波先按 `diffuseFraction` 与晴空 DNI 上限（按真实太阳天顶角）分为直射/漫射，再把直射乘局地入射余弦 cos i / cos θs、漫射乘天空视角因子得到坡面短波；能量平衡以该坡面短波、固定的坡面漫射份额和局地入射角 i（冠层直射消光、光照组分比例、长波光照/阴影划分）求解，复合地形取面积平均的 max(0, cos i)。水平像元保持原算法。`eb_forcing.tif` 的下行短波为水平值。

纬度、经度省略时由地表类型栅格的地理参考换算为 WGS84。

## 气象强迫来源：ERA5 为用例，其他来源的变量约定与限制

（2026-11-24 评估，未实现 GLDAS 等适配器。）EB 的物理核心只用六个量：近地面气温 Ta（K）、水汽压 ea（hPa）、气压 p（hPa）、风速（m s⁻¹）、水平面向下短波（W m⁻²）与向下长波（W m⁻²）。ERA5 / ERA5-Land 只是目前唯一实现的读入方式（`forcing/era5.cpp`，索引 `stream-era5-pack`，配置字段 `temperature.era5.pack`）。EB 的初值与运行除 `t2m` 外不再依赖 `skt`/`stl1`/`stl2`：必需变量为 `u10 v10 t2m d2m sp ssrd strd`；`skt` 只在地类含水体时读取（缺省时水体降级为气温并警告），`stl1`/`stl2` 不再使用，`swvl1`（`soilMoisture.source=era5`）与 `z`（DEM 订正）可选。

| 项 | 现状 | 通用 / ERA5 特有 |
| --- | --- | --- |
| 物理量与单位 | Ta（K）、ea（由露点换算）、p（Pa→hPa，按 DEM 高差订正）、风速（由 u10/v10 合成）、短波/长波（水平面 W m⁻²） | 通用（引擎内部量） |
| 变量名 | `u10 v10 t2m d2m sp ssrd strd skt swvl1 z` 硬编码在 `requiredVariables()`/`optionalVariables()`、`PixelForcing::at()`；服务端/界面文案与登记（`era5-register`、`era5Packs`）也写死 ERA5 | ERA5 特有 |
| 辐射量类型 | `ssrd`/`strd` 是**逐小时累积量**（J m⁻²，除以 3600；ERA5-Land 自 00 UTC 累积，逐时差分） | ERA5 特有；GLDAS 的 `SWdown_f_tavg`/`LWdown_f_tavg` 是时段平均通量（W m⁻²），不能再除 3600、不做差分 |
| 湿度 | 露点 `d2m`→ ea | ERA5 特有；GLDAS 为比湿 `Qair_f_inst`（kg kg⁻¹），需用 `Psurf_f_inst` 换算 ea = q·p/(0.622+0.378q) |
| 时间轴 | 逐小时整点：`covers()` 要求每个整点都有数据，时间 `llround` 到整点，`instant()` 在相邻整点线性插值；短波按“晴空指数 ssrd/cosθ”在小时中心之间插值 | 假设 1 h 步长（写死），思路通用 |
| 空间 | 规则经纬网、双线性、经度 0–360 与全球环绕；产品由 `|纬向步长|<0.15` 推断 era5 / era5-land（只影响累积差分） | 通用，0.25° / 1° 均可 |
| 高程订正 | `z`（m² s⁻²，÷g） | ERA5 特有；GLDAS 的地形为单独文件（单位 m） |
| 土壤水分 | `swvl1`（m³ m⁻³） | ERA5 特有；GLDAS Noah `SoilMoi0_10cm_inst`（kg m⁻²）÷100 才是体积含水量，或用 `soilMoisture.source=user` |

**3 小时（粗时间分辨率）强迫下的语义。** EB 的节点是 EB 自己的整点时间栅格（`marchStart + 3600 s × k`），步长 dt 取相邻节点间隔，表面热历史是 7 h 的**时间窗**（按实际经过的时间，不是 7 个条目），所以 `spinupHours` 的“时次个数”= 小时数，与强迫数据的时间步无关。3 小时强迫必须先重采样到 1 h 节点（瞬时量线性插值，通量量按晴空指数插值）；此时 7 个节点只跨约 2–3 个强迫时次，起点气温是插值值而不是观测，建议把 `spinupHours` 调到 ≥ 12，使土壤热历史与初值偏差充分衰减。不建议把 EB 节点直接改为 3 h（隐式求解仍稳定，但叶/土日变化与热历史窗口都会被过度平滑）。

**GLDAS 变量约定（拟）。** 近地面气温 `Tair_f_inst`（K）→ t2m；比湿 `Qair_f_inst` + `Psurf_f_inst`（Pa）→ d2m/ea、sp；风速 `Wind_f_inst`（须按产品文档核对参考高度）→ 风速；`SWdown_f_tavg`、`LWdown_f_tavg`（W m⁻²，时段平均）→ ssrd/strd（不做累积换算）；`Rainf*` 不参与 EB。时间戳需核对是时段起点、终点还是中点。完整适配的工作项与设计建议见 `docs/model-remaining-issues-20261005.md`（2026-11-22 更新）。

## 同化（直接模拟 / 同化）

配置块 `assimilation`（`stream-image-config` v2，可选）。只需给出观测是什么和数据文件，其余（观测/背景误差、迭代、控制变量）是 `app/assimilation.cpp` 的内置默认值：

```json
"assimilation": {
  "observation": "brightness",
  "data": "D:\\obs\\bt.tif",
  "optical": "D:\\obs\\reflectance.tif",
  "views": [{ "data": "D:\\obs\\bt_oblique.tif", "vza": { "value": 55 }, "vaa": { "value": 90 } }]
}
```

- `observation`：`brightness`（波段数 = 热红外波段数，需启用热红外）、`lst`（1 波段，固定发射率约定，10.5 μm）、`components`（4 波段 Tss/Tsh/Tcs/Tch）；NaN 为缺测；
- `optical`（可选）：波段数 = 光学波段数，给出时 LAI 也作为控制变量；`views` 只用于亮温；
- 控制变量：RT 背景为四组分温度；EB 背景为土壤水分 + Vcmax（组分温度由过境时刻能量平衡的隐式微分 dT/dx = −(∂R/∂T)⁻¹∂R/∂x 给出）；不含发射率；
- 雅可比来自模型内置的可微算子：物理核为标量类型模板，同化以 `Dual<8>`（`physics/dual.h`）前向自动微分运行与成像相同的代码；
- 输出 `analysis_components.tif`、`background_components.tif`、`analysis_uncertainty.tif`、`analysis_qa.tif`（质量位、O−B、O−A、O−A RMS、DFS、观测数、迭代、MAP 代价 J_b、J_a；质量位见下文“温度反演”一节的 QA 表），EB 或有光学时加 `analysis_controls.tif`、`background_controls.tif`，EB 加 `analysis_fluxes.tif`；本次 `optical_*`/`tir_*` 为分析场的正向图像。

默认值：σ_o 亮温 0.5 K、LST 1.0 K、组分 2.0 K、反射率 0.02；σ_b 组分温度 5 K（相关 0.5）、土壤水分 0.10、Vcmax 30%、LAI 30%；RT 20 次、EB 10 次迭代。每个 Gauss–Newton 步都要在候选点求值、确认 MAP 代价下降后才接受，否则依次回溯到 1/2、1/4（2026-11-21 FIX S-5）。EB 背景的雅可比只是过境节点的隐式导数，不含热历史，GN 方向不一定是下降方向，因此多数 EB 像元以“线搜索停滞”（位 10，代价已下降，默认不拒收）结束。

## 温度反演（LST / LSCT，脚本接口）

配置块 `inversion`（可选，不能与 `assimilation` 同时使用）让引擎在同一次运行里从热红外观测反演 LST 或四组分温度 LSCT，观测算子与正向成像共用 `app/thermal_view.cpp` 的四组分权重（TiRT 比例 × 发射率 + `hom3` 多次散射、Hapke/含水量土壤发射率）；工作台不再单独列出：

- `target: "lst"`：每像元一个辐射温度，1/σ² 加权 Gauss–Newton，发射率误差并入不确定度；输出 `lst.tif`、`lst_uncertainty.tif`、`lst_qa.tif`；
- `target: "lsct"`：以 RT 输入或 EB 解为背景的 MAP Gauss–Newton，可选同地类 s×s 窗口；输出 `lsct.tif`、`lsct_uncertainty.tif`（后验 1σ）、`lsct_qa.tif`（质量位、残差、DFS、观测数、迭代、MAP 代价 J_b、J_a）、`lsct_fractions.tif`（中心像元首个有效观测的四组分归一化有效权重 w_k/Σw，不是几何面积比例）、`lsct_radiance_fractions.tif`（同一观测按后验温度计算的发射辐亮度贡献比例 w_k B(T_k)/Σ w_j B(T_j)，不含反射天空），后验组分温度同时用于本次热红外成像。

亮温观测的模型值含反射天空项（同上节），LST 产品观测不含；EB 运行时 `scene_summary.json` 的 `components` 统计与 `components.tif` 一致（EB 解），被反演/同化后验替换时另有 `posteriorComponents`。

质量位 0–8 与 LS4T `base/quality.py` 一致，位 9–11 为 STREAM 扩展。反演（`lst_qa.tif`/`lsct_qa.tif` 第 1 波段）与同化（`analysis_qa.tif` 第 1 波段）共用同一套定义（2026-11-21 FIX S-5 QA 调整）：

| 位 | 值 | 含义 | 默认拒收（`rejectQaBits=895`） |
| --- | --- | --- | --- |
| 0 | 1 | 输入无效 | 是 |
| 1 | 2 | 云/掩膜 | 是 |
| 2 | 4 | 超出合理范围（反演 200–380 K；同化控制量停在边界） | 是 |
| 3 | 8 | 反演失败（背景点算子求值失败等；该像元不计入分析，分析场为背景） | 是 |
| 4 | 16 | 病态（法方程奇异、EB 隐式导数无效） | 是 |
| 5 | 32 | LS4T 归一化失败（STREAM 不设置） | 是 |
| 6 | 64 | LS4T 廓线不匹配（STREAM 不设置） | 是 |
| 7 | 128 | LS4T 融合拒收（STREAM 不设置） | 否 |
| 8 | 256 | 信息量不足（LSCT DFS < 1；同化 DFS < 0.2） | 是 |
| 9 | 512 | 未收敛：达到迭代上限时代价仍在下降，或法方程奇异，或每个试探步长上算子都求值失败 | 是 |
| 10 | 1024 | 线搜索停滞：至少一个 Gauss–Newton 步已降低 MAP 代价，之后沿 GN 方向（步长 1、1/2、1/4）无法再降低；输出代价最低的已求值状态 | 否 |
| 11 | 2048 | 未更新：没有任何步长能降低代价，分析 = 背景（J_a = J_b） | 否 |

“收敛”的判据是 GN 步长：每个控制量的步长都小于 1e-3 倍背景 σ（LSCT 为 1e-3 K）。MAP 代价 J = Σ(y−h)ᵀR⁻¹(y−h) + (x−x_b)ᵀB⁻¹(x−x_b) 写在 `analysis_qa.tif` 第 8/9 波段（背景/分析）和 `lsct_qa.tif` 第 6/7 波段。线搜索保证 J_a ≤ J_b。各停止原因的像元数记在 `scene_summary.json` 中：同化为 `assimilation.stopReasons`，反演为 `inversion.counts`。字段说明与配置示例见 [地表温度与组分温度反演：方法与接口](../../docs/lst-lsct-inversion.md) 第 6 节。卫星分裂窗 LST 与产品链由 LS4T 承担。

## 反演原型

`tools/stream-inverse/` 是“HiStream 生成样本、STREAM 做反演”的可微单像元原型：用 autograd 复刻本文的 `hom3` 节点辐射度与 Planck 链，做梯度校验和合成 LAI/组分温度反演。当前只支持均匀冠层、单像元、多波段亮温与视角间隙；完整 BRF、大气、EB 时间序列和多像元矢量化尚未接入。见 [tools/stream-inverse/README.md](../../tools/stream-inverse/README.md)。

LST（分裂窗）与 LSCT（组分温度）反演的算法、产品字段、数据契约与已知缺口见 [地表温度与组分温度反演：方法与接口](../../docs/lst-lsct-inversion.md)。

## 输出

| 文件 | 内容 |
| --- | --- |
| `optical_reflectance.tif` | 各光学波段冠层顶方向反射率 |
| `tir_brightness_temperature.tif`、`tir_surface_temperature.tif` | 各热红外波段亮温、发射率归一化方向温度（K） |
| `solar_geometry.tif` | 过境时刻太阳天顶角、方位角 |
| `components.tif` + `components.json` | EB：过境时刻 Tss、Tsh、Tcs、Tch（K），可直接作为 RT 的组分温度文件 |
| `eb_forcing.tif` | EB：过境时刻气温、水汽压、气压、风速、下行短波（水平面）、下行长波、土壤水分 |
| `eb_fluxes.tif` | EB：净辐射、显热、潜热、土壤热通量（W m⁻²）、迭代次数、是否收敛、Obukhov 长度（m）、摩擦速度 u*（m s⁻¹）、稳定度是否收敛；以及能量残差、稳定度残差、稳定度限幅、阵风残差、空气密度、气动粗糙度/位移/冠层权重、短波限幅标记 |
| `scene_summary.json` | 像元计数、各波段统计、EB 时次、收敛像元数、反射天空来源（`reflectedSky`）等 |

GeoTIFF 沿用地表类型栅格的网格和坐标系，空值为 NaN。
