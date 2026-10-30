# 土壤含水量 → 热红外发射率（2026-10-30）

## 需求

“加上含水量热辐射这一个过程，土壤的含水量越多发射率越高。” 此前两个引擎的热红外发射率都与含水量无关：

- 三维（histream）：`> 2.5 µm` 波段一律取材质的 `refTir/tauTir`，`ε = 1 − refTir − tauTir`；VoxelEB 用固定 10500 nm 波段，FacetEB 用 `surface.tirReflectance`，两者都来自同一组常数。
- 影像（stream）：土壤发射率取 `classes.json` 的 `ground.emissivity`（0.955，或 `energy.soilEmissivity` 覆盖），Hapke 只改方向性。

反射率一侧本来已随含水量变化（BSM，0.4–2.5 µm），所以本次只补热红外这一环，并让两者共用同一个含水量字段。

## 采用的口径

以**光谱材质 BSM 的 SMC** 作为驱动字段（与 BSM 反射率同一字段），两个引擎共用同一组常数与同一公式：

```text
SMC     = clamp(SMC, 0, 1)              # >1 按百分比换算（与 BSM 一致）
mu      = max(SMC − 0.05, 0) / 0.25     # 0.05 = BSM 干端阈值，0.25 = BSM SMCp
w       = 1 − exp(−mu)                  # 0 = 干端，→ 1 = 饱和
ρ_TIR   = ρ_dry + (ρ_wet − ρ_dry)·w
ε_TIR   = 1 − ρ_TIR − τ_TIR
```

- 干端 `ρ_dry` = 材质面板的“TIR 反射率”，`SMC ≤ 0.05` 时结果与改动前逐位相同。
- 湿端 `ρ_wet` = 新增的“湿端 TIR 反射率”（`refTirWet`）。缺省（缺字段或负值）取干端发射率 `+0.03`，上限 0.995；也可显式给出。
- 含水量只降低 TIR 反射率：若湿端反射率高于干端，取干端值，保证发射率随含水量单调不减。
- 只对 BSM 材质生效。叶片（PROSPECT）、混凝土、导入波谱等没有土壤含水量，保持原值。

数值示例（τ = 0）：土壤典型 `refTir = 0.05`（ε = 0.95）在 SMC = 0.25 时 `w = 0.551`，`ε ≈ 0.967`；SMC = 0.55 时 `w ≈ 0.865`，`ε ≈ 0.976`。影像引擎地类值 0.955 同样在 SMC = 0.25 时升到约 0.972。

## 改动点

三维：

| 文件 | 改动 |
| --- | --- |
| `models/histream/src/base/soil_tir_emissivity.h` | 新增：公式与常数（`soiloptics::tirReflectance`） |
| `models/histream/src/base/compo.cpp` | `resolveSpectrals` 的 `> 2.5 µm` 分支改用上式（VoxelRT/VoxelEB 的传感器波段与 10500 nm 固定波段） |
| `models/histream/src/facetrt/facetrtcore.cpp` | 光谱 XML 增加 `<ref_TIR_wet>`；`loadSurfaceParameters` 读入并按上式给出 `surface.tirReflectance`（FacetRT/FacetEB 的长波与成像共用） |
| `models/histream/src/base/structs.h` | `SpectralXml::refl_tir_wet`（-1 = 自动） |
| `models/histream/src/base/fileio_json.cpp`、`fileio.cpp` | 从 project.json 的 `refTirWet` 与旧 XML 的 `<ref_TIR_wet>` 读入 |
| `src/renderer/src/main.js`、`project-schema.js` | 光谱材质面板新增“湿端 TIR 反射率”，写入 `refTirWet` 并导出 `<ref_TIR_wet>` |

影像：

| 文件 | 改动 |
| --- | --- |
| `models/stream/physics/soil_emissivity.h` | 新增：同一公式（`stream::soilEmissivityAtMoisture`、`soilSaturatedEmissivity`） |
| `models/stream/app/config.{h,cpp}` | 新增 `energy.soilEmissivityWet`（负值 = 干端 +0.03；须 ≥ `soilEmissivity`） |
| `models/stream/app/image.cpp` | EB 宽带土壤发射率按像元含水量；热红外成像的 Hapke 表改为“干端表 + 饱和表”按同一权重混合；retrieval 归一化仍用固定干端地类值 |
| `server/stream/config.mjs` | `soilEmissivityWet` 加入 energy 覆盖白名单，手工写在 `stream-project.json` 时不丢 |

文档：`docs/theory-manual.md`（公式）、`docs/user-manual.md`（13.3 与土壤水分输入说明）、`models/stream/README.md`、`models/stream/PORTING.md`。

## 明确未改

- 物化属性里的“土壤含水量 SMC”（`soilSet`）仍是水热参数：FacetEB 里只做 `< 0.03` 的干湿判据，VoxelEB 里作为土壤阻抗的参考含水量。它不驱动发射率，界面上两个 SMC 的分工已写入手册。
- `tools/stream-inverse`（Python 单像元原型）直接把 `soil_emissivity` 当输入，不涉及含水量。
- 不透水面、冰雪、水体发射率不变（它们不是土壤）。

## 验证状态

按 `AGENTS.md` 未运行任何算例或测试，未构建。已完成的静态自查：

- `SpectralXml` 的所有构造点均为值初始化（`{}`），新增成员不需要额外初始化；
- TIR 反射率的两个消费路径（`resolveSpectrals` 与 `loadSurfaceParameters`）都改为同一函数，FacetEB 的长波、FacetRT 的成像、VoxelEB/VoxelRT 的 10500 nm 波段一致；
- 反射率与透射率仍满足 `ρ + τ ≤ 1`（湿端反射率按 `1 − τ` 夹取）；
- 影像引擎里 `soilEmissivityTables` 的构建条件与 `hapke` 判据一致，非土壤地类不查表。

建议的构建与验收（需要时执行）：

```powershell
npm run build:engine                       # histream
npm run build:engine:stream                # 若脚本名不同，见 package.json
```

验收对照：同一工程把 BSM 光谱材质的 SMC 由 8 改到 38（`refTir = 0.05`、τ = 0），预期 TIR 反射率 0.0466 → 0.0280、发射率 0.9534 → 0.9720，FacetEB 的 `diagnostics/` 与成像亮温随之变化；影像是把土壤水分由 0.05 改到 0.55，预期 `tir_brightness_temperature.tif` 上升、`optical_reflectance.tif` 同时变化。
