# STREAM 遥感影像模拟引擎（stream.exe）

Stream Studio 中与 HiStream 三维引擎相互独立的影像级引擎：一次运行对应一个过境时刻，输出整景光学反射率、热红外亮温与方向温度。组分温度有两种来源：

- **RT**：已知组分温度，直接给出 Tss/Tsh/Tcs/Tch（常数、单波段/24 波段栅格，或 4 波段 `components.tif`）；
- **EB**：未知温度，由 ERA5 / ERA5-Land 逐小时数据（一天到一年的数据包）驱动，从过境当天 00 UTC 逐时次求解四组分能量平衡到过境时刻，再成像。物理与默认值对齐 TiRT/TIRTEB：`hom3` 节点辐射度全阶长波与短波、均匀冠层标准热点闭合、Raupach 气动阻抗（含对流阵风与 400 s m⁻¹ 分段限幅）、Farquhar 气孔 `g0` 下限、1/L 割线稳定度、7 h 表面热历史或 2 m 土柱。

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
  "temperature": { "mode": "rt", "components": { "file": "components.tif" } },
  // 或 { "mode": "eb", "era5": { "pack": "...\\pack.json" }, "soilMoisture": { "source": "era5" },
  //      "elevationCorrection": true, "energy": { "scheme": "tirt", "soilHeatMethod": "surface",
  //      "temperatureMethod": "robust", "diffuseFraction": 0.2, "enforceSolarLimit": true,
  //      "soilResistance": { "method": "constant", "value": 200 } } }
  "region": { "startRow": 0, "endRow": 100, "startColumn": 0, "endColumn": 100 },
  "threads": 0
}
```

### 能量平衡参数（`energy`）

| 键 | 默认 | 含义 |
| --- | --- | --- |
| `scheme` | `tirt` | 仅接受 `tirt`（兼容旧值 `tirt-analytic`） |
| `soilHeatMethod` | `surface` | `surface` 7 h 温度历史；`depth` 2 m 土柱；`direct` 净辐射比例 |
| `temperatureMethod` | `robust` | `robust`（WC=0.08 残差修正）或 `direct`（含潜热导数的近似导数） |
| `diffuseFraction` | `0.2` | 直/散分配；`-1` 为 Erbs 反算 |
| `enforceSolarLimit` | `true` | 晴空 DNI 上限，超出直射转漫射 |
| `soilResistance.method` | `constant` | `constant` 或 `sellers`（含水量关系） |
| `soilResistance.value` | `200` | 常数土壤阻抗 s m⁻¹ |
| `stomatalG0` | `0.01` | 气孔导度下限 mol m⁻² s⁻¹ |
| `minWindSpeed` / `maxAerodynamicResistance` | `0.05` / `400` | 风与分段气动阻抗限制 |
| `referenceHeight` | `15` | 气象参考高度 m（冠层更高时抬到冠层顶以上 0.1 m） |
| `soilEmissivity` / `leafEmissivity` | `0.955` / `0.985` | EB 宽带发射率；`soilEmissivity` 是干端值 |
| `soilEmissivityWet` | 干端 +0.03（≤0.995） | 饱和土壤发射率；土壤发射率按像元含水量在 `soilEmissivity` 与该值之间插值（见 `physics/soil_emissivity.h`） |
| `soilConductivity` / `soilVolumetricCapacity` | `1.55` / `1.298e6` | 土壤热参数（仅土壤地类） |
| `soilHeatFraction` | `0.25` | 仅 `direct` 方法使用 |
| `spectralBinNm` | `50` | EB 短波分组宽度（nm）；默认 50 nm（约 41 个波段），可设 `1` 做 1 nm 全谱，或设 10 折中 |
| `maxIterations` / `maxStabilityIterations` | `50` / `60` | 内层/外层迭代上限 |

纬度、经度省略时由地表类型栅格的地理参考换算为 WGS84。

## 温度反演（LST / LSCT）

配置块 `inversion`（`stream-image-config` v2，可选）让引擎在同一次运行里从热红外观测反演 LST 或四组分温度 LSCT，观测算子与正向成像共用 `app/thermal_view.cpp` 的四组分权重（TiRT 比例 × 发射率 + `hom3` 多次散射、Hapke/含水量土壤发射率）：

- `target: "lst"`：每像元一个辐射温度，1/σ² 加权 Gauss–Newton，发射率误差并入不确定度；输出 `lst.tif`、`lst_uncertainty.tif`、`lst_qa.tif`；
- `target: "lsct"`：以 RT 输入或 EB 解为背景的 MAP Gauss–Newton，可选同地类 s×s 窗口；输出 `lsct.tif`、`lsct_uncertainty.tif`（后验 1σ）、`lsct_qa.tif`（质量位、残差、DFS、观测数、迭代）、`lsct_fractions.tif`，后验组分温度同时用于本次热红外成像。

质量位与 LS4T `base/quality.py` 一致（位 9 为未收敛）。字段说明与配置示例见 [地表温度与组分温度反演：方法与接口](../../docs/lst-lsct-inversion.md) 第 6 节。卫星分裂窗 LST 与产品链由 LS4T 承担。

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
| `eb_forcing.tif` | EB：过境时刻气温、水汽压、气压、风速、下行短波、下行长波、土壤水分 |
| `eb_fluxes.tif` | EB：净辐射、显热、潜热、土壤热通量（W m⁻²）、迭代次数、是否收敛、Obukhov 长度（m）、摩擦速度 u*（m s⁻¹）、稳定度是否收敛；以及能量残差、稳定度残差、稳定度限幅、阵风残差、空气密度、气动粗糙度/位移/冠层权重、短波限幅标记 |
| `scene_summary.json` | 像元计数、各波段统计、EB 时次、收敛像元数等 |

GeoTIFF 沿用地表类型栅格的网格和坐标系，空值为 NaN。
