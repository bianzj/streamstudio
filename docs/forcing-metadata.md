# 气象强迫元数据（forcing metadata）

2026-11-24 起，STREAM 能量平衡（EB）的气象强迫不再写死 ERA5 的变量名与单位，而由一份**元数据**描述“数据里每个物理量叫什么、什么单位、测在多高、怎么随时间取值”。换数据源（ERA5、ERA5-Land、GLDAS、气象站 CSV 或其他）只需换/改元数据，引擎按元数据把任何来源标准化为内部强迫量，物理核心不变。

- 引擎实现：`models/stream/forcing/forcing_metadata.{h,cpp}`（元数据解析、预设、标准化）、`forcing_source.h`（强迫源接口）、`era5.{h,cpp}`（网格数据包）、`station_source.{h,cpp}`（站点 CSV）。
- JSON Schema：`docs/forcing-metadata.schema.json`（界面校验用）。
- 命令行：`stream forcing-preset <名称>`（输出预设 JSON；不带名称列出预设）、`stream forcing-check <预设名|元数据.json> [--data 数据包索引.json | 站点.csv ...] [--json]`（校验并输出解析结果/警告）、`stream forcing-register <索引.json> --metadata <预设名|元数据.json> <文件.nc>...`（登记非 ERA5 的网格数据包）；原 `stream era5-register` 不变。

## 1 概念

| 概念 | 说明 |
| --- | --- |
| 数据形态 `form` | `gridded`：NetCDF（GRIB 请先转 NetCDF）规则经纬网，双线性插值；`station`：站点 CSV 时间序列，单站或多站 |
| 标准量 | 引擎内部使用的量，沿用 ERA5 命名但**单位固定为 SI**：`t2m`（K，2 m 气温）、`d2m`（K，2 m 露点）、`sp`（Pa，地面气压）、`u10`/`v10`（m s⁻¹，10 m 风）、`ssrd`/`strd`（向下短波/长波，见下）、可选 `skt`（K）、`swvl1`（m³ m⁻³）、`z`（m² s⁻²，地形位势） |
| 元数据 | 把任意来源映射到标准量的 JSON 文件，随数据包或工程保存；内置预设可原样使用，也可复制修改 |
| 标准化层 | 元数据 + 原始数据 → 标准量（单位换算、湿度换算、风分量、高度订正、时间重采样到逐小时、缺测/范围检查） |

标准化后所有来源都落在**逐小时整点**栅格上（EB 节点是 `marchStart + 3600 s × k`，`spinupHours` 以小时计与强迫时间步无关，见 `models/stream/README.md`）：瞬时量在整点取值，通量量存为“截至该整点的前一小时平均通量（W m⁻²）”。ERA5 路径保持原存储（逐小时累积 J m⁻²）逐位不变。

## 2 文件结构

```jsonc
{
  "kind": "stream-forcing-metadata",
  "schemaVersion": 1,
  "name": "my-station",              // 必填，显示用
  "preset": "station-csv",           // 可选：以预设为底，下面各项逐键覆盖（对象递归合并，数组整体替换）
  "description": "…",
  "form": "gridded",                 // gridded | station
  "time": {…},                       // 3.1
  "grid": {…},                       // 3.2 仅 gridded
  "station": {…},                    // 3.3 仅 station
  "variables": {…},                  // 4
  "heightCorrection": {…},           // 5
  "spatial": {…}                     // 6
}
```

未知键给出警告（不报错，便于以后扩展）；`kind`/`schemaVersion` 缺省时按 1 处理并警告。

## 3 时间、网格与站点

### 3.1 `time`

| 键 | 缺省 | 说明 |
| --- | --- | --- |
| `variable` | `["valid_time","time"]` | gridded：时间变量名/别名，取文件里第一个存在的；单位取其 `units` 属性（`seconds/minutes/hours/days since 日期[ 时刻]`），可用 `units` 覆盖 |
| `column` | `"time"` | station：CSV 时间列名 |
| `format` | `iso8601` | station：`iso8601`（`YYYY-MM-DD[ T]HH:MM[:SS]`，可带 `Z` 或 `±hh:mm`）、`epoch_seconds`、`epoch_milliseconds`、`yyyymmddhh`、`yyyymmddhhmm` |
| `stepHours` | 自动检测 | 记录间隔（小时，可为 0.25/1/3/6…）；写出时与检测到的中位间隔对照，相差 >1 % 给警告（累积量以此除时长，必须正确）。单变量可在变量对象里用 `stepHours` 覆盖 |
| `utcOffsetHours` | `0` | 时间戳相对 UTC 的偏移（本地时 = UTC + 偏移，例如北京时 `8`）；字符串里自带 `Z`/`±hh:mm` 时以字符串为准 |
| `maxGapHours` | `max(6, 2×步长)` | 瞬时量线性插值允许跨越的最大缺口；超出的整点为缺测（NaN） |

时间戳约定在**变量**上用 `stamp` 表示（见 4）。

### 3.2 `grid`（gridded）

`latitude`、`longitude`：坐标变量名或别名（缺省 `["latitude","lat"]`、`["longitude","lon"]`）。要求等间距的一维坐标（规则经纬网）；纬度可升可降；经度 0–360 与 ±180 自动识别，全球经度环绕沿用现有处理；数据变量里的经纬度维按其名字与坐标变量同名识别（即维度名须在上述别名中）。

### 3.3 `station`（站点 CSV）

```jsonc
"station": {
  "delimiter": ",",                   // 缺省 ","；也可 ";" "\t"
  "comment": "#",                     // 以此开头的行忽略
  "columns": {"id": "station_id", "latitude": "lat", "longitude": "lon", "elevation": "elev"},  // 全部可选
  "stations": [                       // 可选：站点登记表（经纬度/海拔/文件）
    {"id": "54511", "latitude": 39.8, "longitude": 116.5, "elevation_m": 31.3, "file": "54511.csv"}
  ]
}
```

数据文件由配置 `temperature.forcing.files` 提供（或登记表 `stations[].file`，相对元数据文件所在目录）。站点信息按下列顺序取：①登记表中 `id` 或文件名（不含扩展名）相同的条目；②CSV 行内的 `columns.id/latitude/longitude/elevation` 列（单文件多站点，按 id 分组；同一站点这些列取第一个有效值）；缺经纬度报错。海拔缺失时该站的高程订正不可用，气压若需由海拔估算则退回 1013.25 hPa 并警告。

多文件、多站点混合：每个文件可含一个或多个站点；站点 id 在全部文件中必须唯一。

## 4 `variables`：物理量映射

每个物理量（以及风/湿度的分量）是一个“通道对象”，通用字段如下。

| 键 | 缺省 | 说明 |
| --- | --- | --- |
| `source` | 必填（有 fallback 者除外） | 变量名/CSV 列名，字符串或别名列表（取第一个存在的）；大小写敏感，CSV 列名首尾空白忽略 |
| `unit` | 该物理量的缺省单位 | 源单位，见 4.9 |
| `scale`、`offset` | `1`、`0` | `值 = 原始值 × scale + offset`，先于单位换算（NetCDF 自带的 `scale_factor/add_offset` 由读取库先处理，不要重复写） |
| `kind` | 见各量 | `instant` 瞬时；`previous` 记录值保持到下一记录（零阶保持）；`interval_mean` 时段平均率（W m⁻² 等）；`accumulated` 时段内累积量（单位含能量，如 J m⁻²，按步长换算为平均率）；`cumulative` 自某个重置时刻起的累计量，用相邻记录差分 |
| `stamp` | `instant`/`previous` → `instant`，其余 → `interval_end` | 时间戳含义：`instant`（时刻）、`interval_start`/`interval_end`/`interval_mid`（该记录代表的时段起点/终点/中点；时段长度 = 步长） |
| `resetHourUtc` | `0` | 仅 `cumulative`：每日 UTC 该时刻重置（累计量在 `resetHourUtc` 之后第一个记录等于该时段累积本身）。检测到数值下降也视为重置 |
| `stepHours` | 取 `time.stepHours` | 单变量步长 |
| `height_m` | 见 5 | 观测/变量所对应的离地高度（m）；缺省值与警告见 5 |
| `missing` | `[]` | 缺测标记值列表（如 `[-9999, 999.9]`），换算成 NaN；NetCDF 的 `_FillValue`/`missing_value` 与绝对值 >1e30 恒视为缺测 |
| `range` | 无 | `[最小, 最大]`，**用声明单位**（scale/offset 之后）；超出者按缺测，数量在日志与 `scene_summary.json` 报告 |

### 4.1 气温 `airTemperature`

通道对象；`kind` 缺省 `instant`，`unit` 缺省 `K`，`height_m` 缺省 2。引擎量：`t2m`（K）。

### 4.2 湿度 `humidity`

`type` 必填，决定 `source` 的含义与缺省单位（各种湿度最终换算为露点 `d2m`，引擎在 `PixelForcing::at` 里由露点算水汽压）：

| `type` | 缺省 unit | 换算到水汽压 ea（hPa） |
| --- | --- | --- |
| `dewpoint` | K | 无需换算（`d2m` = 露点；ERA5 路径原样使用） |
| `specific`（比湿 q） | kg kg-1（也可 g kg-1） | `ea = q·p / (0.622 + 0.378 q)`，p 为地面气压（hPa） |
| `mixingRatio`（混合比 r） | kg kg-1 | `ea = r·p / (0.622 + r)` |
| `relative`（相对湿度） | %（也可 1 表示小数） | `ea = RH·es(Ta)`，`es` 用引擎同一 Tetens 公式（水面） |
| `vapourPressure` | hPa | 直接使用 |

再由 ea 反解露点：`Td = 237.3·L/(7.5 − L)`，`L = log10(ea/6.107)`（引擎 `saturationVapourPressure` 的反函数）；ea ≤ 0 为缺测。湿度需要 p 与 Ta 时使用**标准化后**的 `sp`/`t2m`。`height_m` 缺省 2；与气温同样适用 5 的规则（只记录并警告，不订正）。

### 4.3 气压 `pressure`

`type`：`surface`（地面气压，缺省）、`seaLevel`（海平面气压，用站点/格点海拔 `h` 按 `p = p0·exp(−g h / (R_d·T_a))` 折到地面，`T_a` 取同时刻 2 m 气温；没有海拔则报错）、`none`（无气压：用海拔按标准大气 `p = 101325·(1 − 2.25577e-5 h)^5.25588` 估算，没有海拔取 101325 Pa 并警告）。`unit` 缺省 `Pa`。网格数据的海拔来自 `orography`，站点来自站点海拔。

### 4.4 风 `wind`

`type`：`components`（`u`、`v` 两个通道对象，默认）、`speedDirection`（`speed` + `direction`，方向 `unit: deg`，`convention`：`from`（气象风向，缺省）或 `to`）、`speed`（只有风速：内部 `u10 = 风速, v10 = 0`，方向对 EB 无意义）。`height_m` 写在 `wind` 上（缺省 10）。风速单位：`m s-1`、`km h-1`、`kt`、`mph`。**EB 只用风速大小**。

### 4.5 向下短波 `shortwaveDown`（必需）

通道对象。`unit`：`W m-2`（功率）或能量面密度 `J m-2`、`kJ m-2`、`MJ m-2`、`Wh m-2`、`kWh m-2`、`cal cm-2`；`kind` 与 `unit` 必须匹配（`interval_mean`/`instant` 用功率；`accumulated`/`cumulative` 用能量），否则报错。

时间处理（保持通量的重采样）：把每条记录还原为时段 `[终点−步长, 终点]` 上的平均通量 `R`，再用晴空指数分配到逐小时：`R_h = k·mean(max(μ,0))`（μ 为小时内太阳天顶角余弦，按位置与时刻由 NOAA 近似式在 5 min 采样平均；`k = R / mean_{时段}(max(μ,0))`，夜间且 `R≈0` 时 k=0）。这样每个原始时段内的小时通量之和与原始通量×时段长度严格相等，白天形状跟随太阳。引擎随后仍按 `ssrd/cos(zenith)` 在小时中心之间插值。`instant` 的短波按线性插值（站点瞬时辐照度），警告“瞬时短波重采样不守恒”。步长 ≤1 h 且时刻整点对齐的数据无需重采样。

### 4.6 向下长波 `longwaveDown`（必需，除非 fallback）

通道对象，规则同 4.5，但时间分配用保持时段平均的线性插值（相邻时段中点之间线性，再逐时段加常数使时段平均不变）。缺少长波数据时 `fallback`：`"none"`（缺省，报错）或 `"brutsaert"`（Brutsaert 1975 晴空参数化：`LW = ε σ T_a⁴`，`ε = 1.24·(ea/T_a)^(1/7)`，ea hPa、T_a K）。**使用参数化时日志与 `scene_summary.json` 强制警告：不含云，阴天长波偏低（可达 20–60 W m⁻²）**；默认关闭。短波缺失无参数化，一律报错。

### 4.7 可选量

| 键 | 引擎量 | 说明 |
| --- | --- | --- |
| `precipitation` | — | `kg m-2 s-1`、`mm h-1`、`mm s-1`、`m s-1`（`instant`/`interval_mean`）或 `mm`、`m`、`kg m-2`（`accumulated`）。**EB 不使用**，解析并校验后记录在摘要 |
| `skinTemperature` | `skt` | K/degC/degF；仅水体像元使用（无此项时水体改用气温并警告，沿用现有行为）。**不再用于初值** |
| `soilMoisture` | `swvl1` | `m3 m-3`、`%`、或 `kg m-2`（须同时给 `thickness_m`，体积含水量 = 值 / (1000 × thickness_m)，GLDAS `SoilMoi0_10cm_inst` 用 0.1）。仅 `soilMoisture.source = "era5"` 时使用（兼容旧名，含义是“取气象强迫的土壤水分”） |
| `orography` | `z` | `m2 s-2`（位势）或 `m`（海拔，换算为 ×g）。供 DEM 高程订正（见 6）；站点用 `station` 海拔 |
| `directShortwave`、`diffuseShortwave` | — | 解析并校验，**EB 目前不使用**（内部按 `diffuseFraction`/Erbs 分配），写入摘要的 `ignored` |

### 4.8 内置预设

| 预设 | 形态 | 步长 | 要点 |
| --- | --- | --- | --- |
| `era5` | gridded | 1 h | `t2m` `d2m` `sp` `u10` `v10`；`ssrd`/`strd` 为 J m⁻² `accumulated`（终点戳，÷3600）；`skt` `swvl1` `z` 可选；高度 2 m/10 m |
| `era5-land` | gridded | 1 h | 同 `era5`，但 `ssrd`/`strd` 为 `cumulative`、`resetHourUtc: 0`（自 00 UTC 累积，逐时差分，01 UTC 记录即首小时累积） |
| `gldas-noah-0.25`、`gldas-noah-1.0` | gridded | 3 h | GLDAS-2.x Noah NetCDF4：`Tair_f_inst`（K）、`Qair_f_inst`（比湿 kg kg⁻¹）、`Psurf_f_inst`（Pa）、`Wind_f_inst`（风速 m s⁻¹）、`SWdown_f_tavg`/`LWdown_f_tavg`（W m⁻²，`interval_mean`，`stamp: interval_end`）、`AvgSurfT_inst`、`SoilMoi0_10cm_inst`（kg m⁻²，厚度 0.1 m）、`Rainf_f_tavg`；时间变量 `time`（分钟自某日期）。两预设内容相同，只是名称区分分辨率 |
| `station-csv` | station | 自动检测 | 列名 `time`、`station_id`、`lat`、`lon`、`elev`、`temp`（°C）、`rh`（%）、`pressure`（hPa）、`wind_speed`（m s⁻¹）、`swdown`、`lwdown`（W m⁻²，`interval_mean`，终点戳）；**不写高度**，运行时按默认高度并警告，用户应在元数据里补上 `height_m` |

**GLDAS 待用户确认项**（没有可用的真实 GLDAS 数据验证，仅按产品命名与文档推断）：①`Tair_f_inst`/`Qair_f_inst` 参考高度按 2 m、`Wind_f_inst` 按 10 m 写入预设（Noah LSM 的常规强迫高度；**待用户对照产品 README 确认**）；②3 h 文件的 `*_tavg` 取“截至文件时刻的前 3 h 平均”，即 `stamp: interval_end`（依据：GLDAS-2.x 起始文件为 `…0300`，若时标为时段起点则 00:00 文件会出现；**待用户确认**）；③`*_inst` 视为该时刻瞬时值；④时间变量为分钟自 1948-01-01（2.0）或 2000-01-01（2.1），由 `units` 属性解析，无需写死。

### 4.9 单位一览

- 温度：`K`、`degC`（`C`、`°C`）、`degF`（`F`）
- 气压/水汽压：`Pa`、`hPa`（`mbar`、`mb`）、`kPa`、`mmHg`、`inHg`
- 比湿/混合比：`kg kg-1`（`1`）、`g kg-1`
- 相对湿度：`%`、`1`（小数）
- 风速：`m s-1`、`km h-1`、`kt`、`mph`；风向：`deg`
- 辐射功率：`W m-2`、`kW m-2`、`MJ m-2 h-1`、`MJ m-2 d-1`、`cal cm-2 min-1`；辐射能量：`J m-2`、`kJ m-2`、`MJ m-2`、`Wh m-2`、`kWh m-2`、`cal cm-2`
- 降水：`kg m-2 s-1`、`mm s-1`、`mm h-1`、`m s-1`、`mm`、`m`、`kg m-2`
- 土壤水分：`m3 m-3`、`%`、`kg m-2`；地形：`m2 s-2`、`m`
- 写法容忍：`m s-1`/`m/s`/`m s^-1`，`W m-2`/`W/m2`/`W m^-2`，`kg kg-1`/`kg/kg`；其余写法报“单位不识别”。

## 5 高度订正 `heightCorrection`

**引擎内部参考高度**：EB 的气象强迫按 ERA5 约定为 **风 10 m（`u10/v10`）、气温与露点 2 m**；`physics/energy.cpp` 的气动阻抗把它们直接当作 `z = max(energy.referenceHeight, 冠层高 + 0.1 m)`（缺省 15 m）处的值使用，这一差别是已知近似（`energy.cpp` 里的 DEFERRED 注记），**本机制不改变它**。元数据的高度订正只负责把源数据的 `height_m` 折算到上述约定高度（“目标高度”）。

```jsonc
"heightCorrection": {
  "wind": {"mode": "log", "targetHeight_m": 10, "roughness_m": 0.03},
  "airTemperature": {"mode": "none", "targetHeight_m": 2, "lapseRate_K_per_m": 0.0065}
}
```

| 量 | 缺省行为 | 规则 |
| --- | --- | --- |
| 风 | `mode: log` | `|height − target| ≤ 0.01 m` 不订正。否则按中性对数律 `u(target) = u(height)·ln(target/z0) / ln(height/z0)`（所有分量同乘该系数），`z0 = roughness_m`（缺省 0.03 m，开阔草地量级；**要求 height、target 都大于 z0**，否则报错）；`mode: none` 只记录并警告 |
| 气温、露点 | `mode: none` | 高度不同只记录并警告；`mode: lapse`：`T(target) = T(height) − Γ·(target − height)`，`Γ = lapseRate_K_per_m`（缺省 0.0065），只订正气温；露点不订正（湿度高度不同仍只警告），引擎的 `min(es(Td), es(Ta))` 保证水汽压不超过饱和 |

`height_m` 缺失：风缺省 10 m，气温/湿度缺省 2 m，**日志与摘要给警告**“未声明高度，按默认值处理”，并且**不做订正**（默认等于目标）。预设 `era5`/`era5-land`/`gldas-*` 显式写高度；`station-csv` 故意不写。

ERA5 预设高度 = 目标高度，订正系数恒为 1，**不做任何算术**，因此逐位不变。

## 6 `spatial`：空间插值与高程订正

```jsonc
"spatial": {
  "interpolation": "idw",        // gridded 固定 bilinear；station: nearest | idw（缺省）
  "idwPower": 2,
  "maxStations": 4,              // ≤4（引擎权重槽数）
  "maxDistanceKm": 0,            // 0 = 不限；超出的站点不参与，全部超出则该像元无强迫
  "lapseRateKPerKm": 6.5         // 高程订正的气温递减率，缺省 6.5（等于现有硬编码）
}
```

- gridded：沿用现有双线性权重（NaN 感知、经度环绕）。
- station：`nearest` 取最近站；`idw` 取最近 `maxStations` 个站按 `1/d^power` 加权（球面距离，权重归一；站点缺测时按剩余有效站点重新归一，沿用 NaN 感知）。
- 高程订正沿用引擎现有开关 `temperature.elevationCorrection`（缺省 true）与 DEM：气温 `−Γ·Δh`，气压用压高公式，水汽压不变并受饱和上限限制；`Δh = DEM 高度 − 源海拔`，源海拔对网格是 `orography`（`z/g`），对站点是加权后的站点海拔（引擎里统一暴露为标准量 `z = 海拔×g`）。没有源海拔则不订正并警告。

## 7 校验与错误信息

校验在读取元数据时（结构、枚举、单位、kind/unit 匹配）和打开数据时（变量是否存在）两步完成；`stream forcing-check` 两步都能做。所有消息中文，带元数据名与字段路径，不会静默降级（降级只发生在下表标“警告”的情形）。

| 情形 | 级别 | 消息（模板） |
| --- | --- | --- |
| 元数据不是合法 JSON / `kind` 不对 | 错误 | `气象强迫元数据 <来源> 无法解析：<原因>` |
| 预设名不存在 | 错误 | `未知的气象强迫预设 "<名称>"；可用：era5、era5-land、gldas-noah-0.25、gldas-noah-1.0、station-csv` |
| 必需物理量缺少定义 | 错误 | `气象强迫元数据 "<名称>"：缺少 variables.<量>（<含义>）` |
| 数据里找不到变量/列 | 错误 | `气象强迫数据缺少变量 "<source>"（variables.<量>.source，已尝试别名：…）；数据里有：<前 20 个>。请在元数据中改 source 或加别名` |
| 单位不识别 | 错误 | `气象强迫元数据 "<名称>"：variables.<量>.unit "<unit>" 不能识别；该物理量可用单位：<列表>` |
| kind 与 unit 不匹配 | 错误 | `variables.<量>：kind "accumulated" 需要能量单位（如 J m-2），收到 "W m-2"` |
| 枚举值非法（type/kind/stamp/mode…） | 错误 | `variables.<量>.<键> "<值>" 无效；可选：…` |
| 时间变量/列缺失、无法解析 | 错误 | `…找不到时间变量（已尝试 valid_time、time）` / `第 <行> 行时间 "<文本>" 无法按 <format> 解析` |
| 步长声明与数据不符 | 警告 | `time.stepHours = 3 与数据检测到的间隔 1 h 不一致，按声明处理` |
| 高度缺失 | 警告 | `variables.wind 未声明 height_m，按默认 10 m 处理且不订正（请核对数据来源）` |
| 高度与目标不同且不订正 | 警告 | `气温高度 10 m 与引擎约定 2 m 不同，heightCorrection.airTemperature.mode = none，未订正` |
| 风订正 | 信息 | `风速由 2 m 订正到 10 m（对数律，z0 = 0.03 m，系数 1.381）` |
| 无气压且无海拔 | 警告 | `没有气压且没有站点/格点海拔，地面气压取 1013.25 hPa` |
| 长波参数化 | 警告 | `没有向下长波，按 Brutsaert 晴空参数化（不含云，阴天偏低）` |
| 范围外/缺测值 | 警告 | `variables.<量>：<n> 个值超出 range 或为缺测标记，已按缺测处理` |
| 站点缺经纬度 | 错误 | `站点 "<id>"（<文件>）缺少经纬度：请在 station.stations 登记或在 CSV 里提供 lat/lon 列` |
| 数据覆盖不足 | 错误 | 沿用现有“气象数据包未覆盖本次能量平衡所需时段 …”，对粗步长另说明“原始记录需包住该时段” |

## 8 与现有 `era5` 配置的关系

- 旧工程只写 `temperature.era5.pack`、不写元数据：**默认使用内置预设**，由数据包索引的 `product` 决定（`era5-land` → `era5-land`，其余 → `era5`）；行为与此前逐位相同。
- 新增配置（均可选）：

```jsonc
"temperature": {
  "mode": "eb",
  "era5": {"pack": "…/era5-pack.json"},        // 旧字段，保持兼容
  "forcing": {                                    // 新
    "pack": "…/pack.json",                       // gridded 数据包索引；缺省取 era5.pack
    "files": ["…/54511.csv"],                    // station 的 CSV
    "metadata": "gldas-noah-0.25"                // 预设名 | 元数据 JSON 路径 | 内联对象
  },
  "forcingMetadata": "…/forcing-metadata.json"   // 等价于 forcing.metadata 的简写
}
```

- 元数据来源优先级：`temperature.forcing.metadata` / `temperature.forcingMetadata` > 数据包索引里的 `"metadata"` 键（预设名，或相对索引目录的 JSON 路径；`forcing-register --metadata` 写入）> 按 `product` 的预设。`form: station` 时必须有 `forcing.files` 或元数据登记表中的文件；`form: gridded` 时必须有数据包（`forcing.pack` 或 `era5.pack`）。
- `soilMoisture.source`（`era5`/`user`）沿用，含义“取气象强迫的土壤水分 / 用户栅格”。
- 数据包索引：`era5-register` 写出的 `kind: stream-era5-pack` 不变；`forcing-register` 写 `kind: stream-forcing-pack`（字段同，另有 `metadata`、`stepHours`，`period`/`complete` 以原始记录计）。两种索引引擎都能打开。

## 9 运行记录

- 日志：`气象强迫元数据：<名称>（<预设|文件路径|内联>）…`、各量映射一行、高度订正实际处理、全部警告。
- `scene_summary.json` → `temperature.forcing`：

```jsonc
"forcing": {
  "metadata": {"name": "era5", "origin": "preset|file|inline|pack", "path": "…", "form": "gridded", "stepHours": 1},
  "variables": {"airTemperature": {"source": "t2m", "unit": "K", "kind": "instant", "height_m": 2, "standardised": "t2m (K)"}, …},
  "heightCorrection": {"wind": {"from_m": 2, "to_m": 10, "mode": "log", "factor": 1.381}, "airTemperature": {"applied": false}},
  "derived": ["d2m 由比湿 + 气压换算"],
  "ignored": ["precipitation", "directShortwave"],
  "valuesRemoved": {"airTemperature": 0},
  "warnings": ["…"]
}
```

`temperature.era5` 保持原样（旧消费者继续读它）。

## 10 给服务端与界面

1. 界面设置项：数据来源类型（ERA5 / ERA5-Land / GLDAS / 站点 CSV / 自定义）；自定义时编辑每个物理量的 `source`、`unit`、`kind`、`height_m`、湿度 `type`、风 `type`、时间 `stepHours/utcOffsetHours/stamp`；站点形态的站点表与插值方式；高度订正模式。保存为工程里的 `forcing-metadata.json`，把路径写入 `temperature.forcing.metadata`。
2. 预设：`stream forcing-preset` 列出，`stream forcing-preset <名称>` 输出完整 JSON 供复制编辑；界面也可只写 `{"preset": "gldas-noah-0.25", "variables": {"wind": {"height_m": 2}}}` 这样的覆盖。
3. 校验：`stream forcing-check <元数据> [--data …] --json` 输出 `{"ok", "errors": [], "warnings": [], "resolved": {…}}`（同 `scene_summary` 的 `forcing` 结构）；`docs/forcing-metadata.schema.json` 做结构校验。
4. 登记：ERA5 继续用 `era5-register`；其他网格数据用 `forcing-register`；站点 CSV 无需登记，直接写进 `forcing.files`。
5. 服务端文案：数据包登记等处的 “ERA5 数据包” 可改为“气象强迫数据”，仅在选 ERA5 预设时出现 ERA5 字样。
