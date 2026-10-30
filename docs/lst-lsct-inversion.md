# 地表温度与组分温度反演：方法与接口

状态：算法与接口说明（2026-10-30）。本文说明 STREAM / HiStream 体系里的 **LST（地表温度）** 与 **LSCT（地表组分温度）** 反演：算法由 LS4T（`C:\work\ls4t`）承担，STREAM 提供影像级观测算子与像元产品，HiStream 提供三维样本与 OSSE 真值。STREAM 引擎与 Stream Studio 工作台已内建**以 STREAM 正向热红外模型为观测算子**的 LST / LSCT 影像反演（第 6 节，`inversion` 配置块、工作台“温度反演”面板）；卫星级分裂窗与多传感器产品链仍由 LS4T 承担，`tools/stream-inverse/` 是单像元可微原型。本文逐项标注实际状态，不把规划能力写成已有功能。

物理推导与公式见[模型理论手册第 15 章](theory-manual.md#15-地表温度与组分温度反演)；界面与输入输出说明见[模型使用手册 5.3 节](user-manual.md#53-地表温度与组分温度反演lst--lsct)。需求与精度目标见 [stream-lst-lsct-requirements.md](stream-lst-lsct-requirements.md)，可微同化路线见 [stream-differentiable-assimilation](superpowers/plans/2026-10-27-stream-differentiable-assimilation.md)。

## 1. 分工

| 角色 | 承担 | 主要输出 |
|---|---|---|
| HiStream | 三维场景高保真正向模拟、样本生成、OSSE 真值 | “参数—观测”对、四组分温度真值、方向亮温、光学 BRF |
| STREAM | 影像级观测算子 `H(x)`：RT 组分温度 → 亮温；EB 气象驱动 → 组分温度 → 亮温 | 各波段亮温、方向温度、组分温度、能量通量 |
| LS4T | 卫星观测的影像级反演：分裂窗 LST、多角度/窗口 LSCT、归一化、降尺度、产品与 QA | L2LST / L2LSCT 及 L3N / L3C / L4A 产品 |
| STREAM-NET | 正向亮温残差修正（5 m 城市/航空域，推理尚未接入） | 修正后的模拟亮温，不是反演算子 |

原则：**单点算法不重复实现**。TiRT/TIRTEB 与 LS4T 已给出的公式、常数和默认值直接复用；STREAM 只负责把它们扩展到整景影像（栅格 I/O、逐像元并行、空间约束、几何配准、批量与时序、QA 与产品）。

## 2. 产品定义

| 产品 | 定义 | 单位 | 关键元数据 |
|---|---|---|---|
| LST | 传感器观测方向上的像元等效辐射温度（方向性 LST），不等于任一组分温度 | K | VZA/VAA、发射率来源、水汽/大气来源、波段中心与带宽 |
| LSCT | 像元内四组分温度：Tss（光照土壤）、Tsh（阴影土壤）、Tls（光照叶片）、Tlh（阴影叶片） | K（4 波段） | 组分顺序、组分比例/权重来源、LAI 与结构来源、观测角度数 |

LSCT 必须同时给出组分比例或权重来源，否则四个温度不可解释。单角度、单波段数据**不提供** LSCT 产品。

## 3. LST：分裂窗（当前实现，LS4T 基线）

`ls4t/base/lst.py` 的 `SplitWindowModel` 是 AATSR、SLSTR、MSG、MODIS、VIIRS、AHI、ASTER 七类传感器共用的**八特征广义分裂窗**核心；传感器目录只做数据映射与准备，不是七套独立物理算法。

输入：两个热红外波段亮温 `BT1/BT2`（K）、两波段发射率 `ε1/ε2`、柱水汽（cm）、观测天顶角（°）、可选云掩膜。输出：`temperature_k`、`quality_flag`。

系数为 `(VZA 节点, 水汽分箱边界, 8)` 的三维数组：相邻 VZA 节点线性插值系数，水汽只做分箱不插值，超出标定域不外推（VZA 最末节点允许，水汽最末边界不允许）。结果超出 220–365 K 置 NoData 并标记 `OUT_OF_RANGE`。

配套的输入构造：方向植被覆盖率 `fvc = 1 - exp(-G·LAI/cos θv)`（`G = 0.5`）与混合有效发射率（含多次散射项，公共默认散射因子 0.75，部分案例用 0.05 并需显式记录）。**T–ε 退化**：单波段热红外不能同时反演温度和发射率，单通道产品必须外部给定发射率与大气校正。

状态：算法与传感器入口已实现；系数表需按传感器与大气条件标定，SLSTR 已有旧系数导入入口（`import_legacy_coefficients`），其余传感器以配置式系数文件为主。L2LST 产品中的 `uncertainty` 目前由调用者给定（默认 1 K 填满），**尚未由残差或协方差传播**。

## 4. LSCT：辐射域线性反演（当前实现）

观测写为 `y = A x`：`A` 是有效发射率权重矩阵（`hom3` 四组分比例 × 组分发射率），`x` 是组分黑体辐亮度，最后用逆 Planck 转温度。四组分顺序固定为 `[soil_sunlit, soil_shaded, leaf_sunlit, leaf_shaded]`；2/3 组分模式是把两土壤或两叶片按权重合并，不是另一套物理。

| 求解器 | 位置 | 用途与边界 |
|---|---|---|
| `retrieve_components` | `base/lsct.py` | 逐像元加权 OLS（权重为观测辐射标准差），或固定 λ 的零中心 ridge；条件数 > 1e6 判病态；`x ≤ 0` 与 200–380 K 为**事后过滤**，不是有界最小二乘 |
| `retrieve_components_window` | `base/lsct.py` | 观测数少于组分时用 s×s 窗口补足方程数，前提是窗口内组分辐射局部常数；边缘用不完整窗口 |
| `inv_lsct2_window` / `inv_lsct3_window` | `base/lsct_window.py` | 距离加权残差 SSE + SLSQP，带辐射上下界、观测包络与相邻组分温差约束；温差用辐射差近似（`B(T_mean+d/2) - B(T_mean-d/2)`），`λ ≠ 10.8 μm` 时有系统偏差 |
| 分阶段与先验平差 | `cases/msg/` | 先分离光照土壤与“阴影土壤 + 植被”，再对残差求解，最后做 `‖Ax-y‖² + λ‖x-x_b‖²` 联合平差；裁剪后不重新拟合，不保证约束下最优 |
| 高斯 MAP | `cases/slstr/slstr_lsct3_bayes_scene.py` | 辐射似然 + 相关温度先验 + 温差软先验，两步 Gauss–Newton 与局部顺序约束投影；没有 MCMC 或后验样本，应称 MAP/Laplace 风格近似 |

重要边界：这些求解器给出的是**点估计**，`ComponentRetrievalResult` 有 `residual_rms`、`condition_number`、`quality_flag`，但没有组分后验协方差或温度标准差字段。

## 5. 数据契约

### 5.1 STREAM → LS4T

| STREAM 输出 | 作为 LS4T 输入 | 说明 |
|---|---|---|
| `tir_brightness_temperature.tif` | `BT1/BT2` | 按波段拆分；这是冠层顶亮温，LO 输入等级最低 |
| `tir_surface_temperature.tif` | 背景场 / 初值 | 方向温度，不等于组分温度 |
| `components.tif` | 真值或先验 | EB 结果，可作为 OSSE 真值或 LSCT 背景场 |
| `solar_geometry.tif`、`eb_forcing.tif` | 辅助输入 | 太阳几何、气温/水汽/气压/风速、土壤水分 |
| 工程地表类型、LAI、VZA/VAA | `ε`、`fvc`、观测几何 | 由同一网格派生，避免重采样失配 |

### 5.2 LS4T → STREAM

LS4T 的 LST/LSCT 结果可直接作为 STREAM **RT 模式**的组分温度输入（4 波段栅格或 `components.tif` 同格式），从而只改波段或观测角时不必重跑 EB。反过来，STREAM 的正向结果与 LS4T 的反演结果在同网格上比较时，必须统一：网格与坐标系、波段中心波长与响应、观测几何、时间（UTC 过境时刻）、大气状态。

### 5.3 顺序约束

两条路线不自动混合（见 LS4T `docs/SPATIAL_MODES.md`）：**先形成研究区再反演**，或**逐景反演再拼接**。同一组文件必须属于同一次获取、同一视角和同一目标网格，不能把不同时次或不同视角混在一次拼接里。

## 6. STREAM 内建反演（当前实现）

代码：`models/stream/app/inversion.cpp`（求解）、`app/thermal_view.cpp`（与正向成像共用的太阳位置、土壤方向发射率与四组分权重）、`app/config.cpp`（配置解析）、`server/stream/config.mjs`（工作台 → 引擎配置与校验）、`src/renderer/src/stream/workspace.js`（“温度反演”面板与结果图层）。

**观测算子。** 与正向成像逐项相同：TiRT 标准四组分比例 × 组分发射率 + `hom3` 多次散射项，土壤发射率按含水量与 Hapke 方向表取值，冠层顶、无大气。第 j 个观测的模拟亮温为

```text
L_j = sum_k w_jk(θv_j, φv_j) · B(λ_j, T_k)
y_j = B⁻¹(λ_j, L_j)                 （亮温观测）
y_j = B⁻¹(λ_j, L_j / ε_fix,j)       （LST 产品观测，ε_fix 为地类固定发射率下的有效发射率）
```

因此反演 STREAM 自己模拟的影像时，可以精确回到生成它的温度（闭合性）。地形坡面模式下观测权重仍按水平地表计算，日志给出警告。

**LST。** 每个像元一个辐射温度 T（四组分同温），对全部有效观测做 1/σ² 加权的 Gauss–Newton；观测方差 = σ_j² + (σ_ε · B(T) / B′(y_j))²，即把发射率误差传播进不确定度。输出 `lst.tif`、`lst_uncertainty.tif`（1σ = (Σ g_j²/σ_j²)^-1/2）、`lst_qa.tif`（质量位、残差 RMS、观测数、有效发射率）。

**LSCT。** 以本次运行的组分温度（RT 输入或 EB 解）为背景场 x_b，背景协方差 B = σ_b²[(1−r)I + r·11ᵀ]，求

```text
J(x) = Σ_j (y_j − h_j(x))² / σ_j² + (x − x_b)ᵀ B⁻¹ (x − x_b)
```

的 MAP 解（Gauss–Newton，温度限制在 150–400 K）。可选 s×s 窗口：窗口内同地类像元共享四组分温度，观测数随之增加。权重之和低于 1e-4 的组分视为不可观测，保留背景值。输出 `lsct.tif`（后验）、`lsct_uncertainty.tif`（后验协方差 (JᵀR⁻¹J + B⁻¹)⁻¹ 的对角线开方）、`lsct_qa.tif`（质量位、残差、信号自由度 DFS = m − tr(Â B⁻¹)、观测数、迭代次数）、`lsct_fractions.tif`（中心像元第一个观测的组分辐射占比）。后验组分温度同时替换本次热红外成像所用的组分温度，`tir_*.tif` 即后验状态的正向图像；EB 模式的 `components.tif` 仍写 EB 原解。

**质量位**（与 LS4T `base/quality.py` 一致，位 9 为 STREAM 扩展）：0 输入无效、1 云/掩膜、2 结果越界（200–380 K）、3 求解失败、4 病态（法方程不可逆）、8 信息量不足（LSCT 的 DFS < 1）、9 未收敛。

**配置块**（`stream-image-config` v2，可选）：

```jsonc
"inversion": {
  "target": "lsct",                         // "lst" | "lsct" | "none"
  "observations": [
    { "name": "B10", "kind": "brightness", "value": { "file": "obs_b10.tif" }, "wavelength": 10.9, "sigma": 0.5 },
    { "name": "B10o", "kind": "brightness", "value": { "file": "obs_b10_oblique.tif" }, "wavelength": 10.9, "sigma": 0.5,
      "vza": { "value": 55 }, "vaa": { "file": "vaa_oblique.tif" } }
  ],
  "mask": { "file": "cloud.tif" },          // 可选，非 0 不参与
  "prior": { "sd": 5, "correlation": 0 },   // LSCT 背景场误差（K）与组分间相关
  "emissivitySd": 0.01,                     // LST 发射率误差
  "window": 1,                              // LSCT 窗口，1–9 的奇数
  "maxIterations": 20
}
```

观测缺省 VZA/VAA 时沿用场景的观测几何；观测值超出 150–400 K（多为单位错误）会被剔除并计数。只启用反演、不启用光学/热红外成像也可运行；LSCT 在 RT 模式下必须提供四组分温度作为背景场。结果摘要写入 `scene_summary.json` 的 `inversion` 字段（方法、观测、成功/失败/云/越界/未收敛计数、平均残差与 DFS、各产品统计）。

**工作台。** 参数面板“温度反演”：选择 不反演 / LST / LSCT；逐个添加观测（亮温栅格或常数、波长、σ、可选独立视角），或“按热红外波段添加”；LSCT 设置背景 σ、组分相关、窗口与迭代次数，LST 设置发射率误差；可选云掩膜。运行按钮变为“运行反演”，结果下拉框列出反演产品，运行摘要显示成功像元、平均残差与平均 DFS。

**可识别性提示。** 单角度观测只约束组分的加权组合，LSCT 结果主要由背景决定（DFS 接近 1，质量位 8 常见）；多角度观测或窗口才能分离组分。不同波长的亮温几乎不提供组分温度的独立信息。

## 7. 同化接口（扩展方向）

第 6 节只反演温度。LAI、土壤水分等结构/水分参数的联合反演与 EnKF 同化尚未实现，`models/stream/PORTING.md` 把 EnKF 列为未移植项。规划的扩展接口如下：

```jsonc
"inverse": {
  "enabled": true,
  "method": "gauss-newton",          // 或 "lbfgs" / "enkf"
  "control": ["lai", "soilMoisture", "temperature"],
  "observations": {
    "optical": { "file": "obs_reflectance.tif", "bands": ["B4", "B5"] },
    "thermal": { "file": "obs_bt.tif", "bands": ["B10", "B11"] }
  },
  "prior": { "lai": { "mean": 3.0, "sd": 1.0 }, "soilMoisture": { "mean": 0.25, "sd": 0.08 } },
  "maxIterations": 20,
  "outputUncertainty": true
}
```

该块尚未被 `stream-image-config` 接受，属于规划接口。观测算子、代价函数与不确定性的公式见理论手册 15.6 节。

## 8. 实现边界与已知缺口

1. **内建反演只到温度**：`inversion` 块反演 LST/LSCT，不反演 LAI、土壤水分、发射率；`inverse` 联合同化块不被引擎接受；
2. **不确定度**：STREAM 内建反演给出 LST 1σ 与 LSCT 后验标准差；LS4T 的 L2LST `uncertainty` 仍是调用者给定的常数，LS4T 的 LSCT 求解器没有后验标准差字段；
3. **大气**：STREAM 是冠层顶结果，不做大气校正；内建反演的亮温观测须先做大气校正，否则大气误差会被吸进地表温度；
4. **发射率**：只有分裂窗路径用的混合有效发射率，没有多通道温度—发射率分离（TES）实现；ASTER 模块不含 TES；
5. **组分温度可识别性**：单角度 + 多波段亮温不能独立反演四组分，原型 Jacobian 条件数约 5.8e3、单组分 RMSE 约 5.9 K，必须靠多角度、多时相、光学 BRF 或强先验；
6. **窗口求解的隐含假设**：组分辐射在窗口内常数、边缘用不完整窗口、条件数与残差按整窗统计，都没有最少有效像元比例或空间相似性检查；
7. **优化器状态未检查**：部分历史窗口函数不检查 `success/status`，不收敛结果可能与收敛结果同样入库；
8. **传感器系数**：分裂窗系数按传感器与大气条件标定，跨场景泛化没有独立地面验证证据；MODIS 入口不等同官方 MODIS LST 算法；
9. **双实现漂移**：`tools/stream-inverse/` 与 `models/stream/physics/` 的常数、分支和默认值必须保持一致，改动需同步；
10. **内建反演的几何与窗口**：地形坡面模式下观测权重按水平地表计算；窗口只按地类筛选邻域，不检查空间相似性或最少有效像元比例；分裂窗不在 STREAM 内实现（STREAM 用物理观测算子直接拟合，分裂窗系数路线由 LS4T 承担）。

## 9. 验证要求

1. **OSSE**：HiStream 三维样本 → 加噪声 → 反演 → 与真值比较，给出分地类、分几何精度的偏差/RMSE 与条件数分布；
2. **闭合检查**：反演结果回代 STREAM 正向，残差应在观测噪声量级；
3. **交叉传感器**：同一时次不同传感器 LST 一致性，在统一发射率、大气与几何后比较；
4. **图级验证**：除点位 RMSE，还要有图级偏差、空间结构（不出现椒盐噪声）和 QA 有效率；晴空有效率 ≥ 95%；
5. **保守性检查**：68% 覆盖应接近给出的 1σ；不确定度若为常数填满，不能用于这一步，必须先补传播。

## 10. 待确认

1. 首个观测源：Landsat（单视角、2 波段）、Sentinel-3 SLSTR（双视角）还是 UAV 多角度；
2. 发射率来源：NBEM / ASTER / JPL 库 / 固定值，由谁提供并如何记录；
3. 大气：外部大气校正产品，还是 STREAM 内接 MODTRAN LUT（`assets/atmosphere/simple_modtran_lut.csv`）；
4. LSCT 验证数据：UAV 多角度实测，还是先用 HiStream 样本做 OSSE；
5. LST 定义：方向性温度还是归一化到天底，产品对标哪一家外部产品；
6. 联合同化：LAI / 土壤水分是否进入 `inversion` 块的控制变量，还是只在 LS4T 与 HiStream 一侧完成。
