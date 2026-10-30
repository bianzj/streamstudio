# STREAM 可微同化：现状、可行性与实施路线

状态：方案设计 + 单像元可微原型已实现（`tools/stream-inverse/`，autograd + SciPy，未改 `stream.exe`）。首步按 A 执行。

## 0. 角色分工（按项目定位）

- **HiStream + STREAM-NET**：生成样本、做正向模拟的残差修正（STREAM-NET 是亮温残差预测器，5 m 城市/航空域；推理尚未接入）。
- **STREAM**：反演算子。可微化的是 STREAM 的解析正向链 `H(x)`；HiStream 样本提供“参数—观测”对，用于标定/验证反演，也用于训练 STREAM-NET 的残差修正。
- **STREAM-NET 在同化中的位置**：观测算子偏差修正或可微代理；不直接充当物理反演器。

## 1. 现状

`models/stream` 是纯正向引擎：

- RT：给定四组分温度 Tss/Tsh/Tls/Tlh，算 BRF 与亮温；
- EB：ERA5 → 四组分能量平衡 → 成像；
- 没有观测算子、代价函数、优化器、先验协方差或不确定性输出；
- `models/stream/PORTING.md` 的“未移植”明确列出 EnKF 同化；`docs/theory-manual.md` §14 只给方法建议，并注明不是已有反演功能。

`C:\work\streamimaging\models\streamnet` 的 STREAM-NET 是**亮温残差预测器**（DilRes 集成，5 m；输入 5 个光学波段 + 模拟亮温 + 掩膜 + 7 个气象量；输出加到物理基线），README 注明推理尚未接入。它是正向偏差修正，不是反演算子。

## 2. 可微性分析（按模块）

| 模块 | 关键函数 | 可微性/处理 |
| --- | --- | --- |
| 冠层闭合 | `canopyFractions`（`physics/canopy.cpp`） | `exp/log/sqrt` 光滑；`LAI→0`、`sza→90`、`hspot→0`、`clamp` 是分界点，需平滑 `softplus/softmin` 或固定分支 |
| 节点辐射度 | `HomCanopy`、`solveLinear`（`physics/radiosity.cpp`） | 5×5 线性求解天然可微；高斯消元的主元选择不连续，AD 时改为固定消元顺序或解析逆 |
| 短波分配 | `admissibleDiffuseFraction`（`physics/shortwave.cpp`） | `max/min` 在限幅阈值处不可微；可用 `softmax` 或对阈值固定分支 |
| 气动/气孔 | `aerodynamicResistances`、`stomatalResistance`（`physics/energy.cpp`） | `log/sqrt/exp` 光滑；`max/min、clamp、smallerRoot` 有分界；气孔 `A≤0` 的 `g0` 取整是折点 |
| 温度修正 | `updateTemperature` | `clamp(±1.5 K, 240–380 K)` 与残差阈值产生折点；不宜逐迭代展开 |
| 稳定度 | `solveEnergyNode` 外层 | 区间割线、符号区间、`break` 是离散逻辑；**不要对求解过程 AD** |
| 成像 | `app/image.cpp` | OpenMP 逐像元循环，不能整体 AD；应把单像元函数抽成纯函数再矢量化 |

结论：RT 链可直接可微；EB 链应使用**隐式微分**或**弱约束**，不要展开迭代。

### 2.1 EB 的两种求导方式

1. **隐式微分**：收敛后 `R(T,x)=0`（四组分残差），
   `dT/dx = −(∂R/∂T)⁻¹ ∂R/∂x`。
   只需一次 4×4 线性求解；稳定度可把 `1/L` 一起作为未知量加入残差。
2. **弱约束 4D-Var**：把 Tss/Tsh/Tls/Tlh 也作为控制变量，EB 方程作为惩罚项：
   `J = 观测项 + 先验项 + λ_EB ||R(T,x)||²`。
   避免对非光滑求解器求导，代价是控制维数增加 4。

## 3. 同化要素

### 3.1 控制变量（建议按可识别性排序）

| 层级 | 变量 | 说明 |
| --- | --- | --- |
| 温度 | Tss/Tsh/Tls/Tlh 或 LST | 单波段 TIR 有 T–ε 退化，需多波段或固定发射率 |
| 结构 | LAI、冠层高度 | 光学多波段约束强，TIR 弱 |
| 水分 | 土壤水分 SM | 影响 BSM 光谱；（`sellers` 模式下）影响蒸发阻抗 |
| 光谱/发射率 | 叶片/土壤光谱尺度、ε | 与 LAI/温度强相关，需先验 |
| 大气 | 水汽、AOD | STREAM 目前是冠层顶，卫星同化必须先加大气 |

### 3.2 观测算子

```text
H_optical(x) = canopyReflectance(band, LAI, rho_leaf, rho_soil, geometry, diffuse)
H_tir(x)     = Σ_i w_i(x, geometry) · Planck_band(T_i)
```

- 多波段：逐波段调用；
- 多角度：循环 VZA/VAA；
- 多时相：EB 状态轨迹（4D-Var/EnKF）；
- 大气：接 MODTRAN LUT（`assets/atmosphere/simple_modtran_lut.csv`）或外部大气校正产品。

### 3.3 代价函数与不确定性

```text
J(x) = Σ_b ||W_b (H_b(x) − y_b)||² + (x − x_b)ᵀ B⁻¹ (x − x_b) + 平滑项
```

- `R`：逐波段辐射噪声 + 代表性误差；
- `B`：地类表/文献先验；
- 后验不确定性：Laplace 近似 `(Jᵀ R⁻¹ J + B⁻¹)⁻¹`。

## 4. 实现路线

| 路线 | 做法 | 优点 | 风险 |
| --- | --- | --- | --- |
| A. Python/JAX 可微正演 | 复刻 hom3 RT + Planck（+ EB 隐式微分），GPU 矢量化 | 开发快、AD 成熟、易做 OSSE | 与 `stream.exe` 双实现，需数值对齐 |
| B. C++ AD | Enzyme/CppAD/ADOL-C 接 `physics/*` | 单一二进制 | Windows/MSVC/OpenMP/GDAL 工程复杂；EB 分支需改写 |
| C. C++ 有限差分 | 增加 `inverse` 模式，FD Jacobian + Gauss–Newton | 无新依赖，少量参数快落地 | 参数多时正演次数线性增长 |
| D. STREAM-NET 代理 | 用现有 PyTorch 网络做可微残差/初值 | 现成梯度 | 只预测亮温残差、输入含观测光学、域为 5 m HiStream，不能当物理反演器 |

推荐：**A 先做原型 → C 做产品化兜底**；D 只作为观测偏差修正或初值。

## 5. 分阶段计划

**阶段 1：单像元可微正演（原型）**
- 输入：LAI、SM、四组分温度、叶片/土壤光谱、几何、波段；
- 输出：BRF、BT；
- 用中心差分校验 `dH/dx`；
- 合成 OSSE：给定真值 → 加噪声 → 反演 LAI/SM/温度，检查偏差与不确定性。

**阶段 2：小场景矢量化**
- JAX `vmap`/GPU 逐像元；
- Gauss–Newton 或 L-BFGS + 先验；
- 输出反演图、残差图、后验标准差；
- 与 `stream.exe` 在相同输入下逐像元对齐（容差另定）。

**阶段 3：EB 时间序列同化**
- 隐式微分或弱约束；
- EnKF/4D-Var 更新温度/土壤水分/LAI；
- 与 ERA5 驱动、过境观测联合。

**阶段 4：产品化**
- `stream-image-config` 增加 `inverse` 块；
- 工作台新增“反演/同化”模式与结果图层；
- 输出 `inversion_state.tif`、`inversion_uncertainty.tif`、`inversion_residual.tif`、`inversion_summary.json`；
- 验证：OSSE + 真实 Landsat/Sentinel 小场景，报告偏差/RMSE/覆盖度。

## 6. 接口草案

```jsonc
"inverse": {
  "enabled": true,
  "method": "gauss-newton",          // 或 "lbfgs" / "enkf"
  "control": ["lai", "soilMoisture", "temperature"],
  "observations": {
    "optical": { "file": "obs_reflectance.tif", "bands": ["B4", "B5"] },
    "thermal": { "file": "obs_bt.tif", "bands": ["B10"] }
  },
  "prior": { "lai": { "mean": 3.0, "sd": 1.0 }, "soilMoisture": { "mean": 0.25, "sd": 0.08 } },
  "maxIterations": 20,
  "outputUncertainty": true
}
```

## 7. 主要风险

1. **T–ε 退化**：单波段 TIR 不能同时反演温度和发射率；需多波段 TIR 或固定 ε。
2. **大气**：STREAM 是冠层顶结果；不先做大气校正会把大气误差吸进地表参数。
3. **EB 非光滑**：限幅、割线、分支使逐迭代 AD 不可靠；用隐式/弱约束。
4. **可识别性**：LAI、叶/土光谱尺度、SM 在仅有少量波段时强相关；需要先验/多角度/多时相。
5. **双实现漂移**：Python 原型与 `stream.exe` 必须共用常量、光谱数据和分支约定；建议把 `stream.exe` 作为参考实现做逐像元回归（需用户明确要求才运行对比）。
6. **STREAM-NET 域限制**：5 m、城市/航空场景训练，不能直接用于 IGBP 像元反演。

## 8. 原型实施状态（单像元，A）

位置：`tools/stream-inverse/`（`stream_inverse.py`、`README.md`、`demo-sample.json`、`vendor/autograd`）。

已实现：`hom3` 节点辐射度、四组分热点、有效发射率、Planck/亮温、视角间隙、autograd 梯度、Gauss–Newton 反演、梯度校验、样本 JSON 导入/导出。

demo（BT 噪声 0.3 K，seed=1）：

| 指标 | 结果 |
| --- | --- |
| 梯度相对误差 | 7e-9 |
| LAI 误差 | 0.04（真值 2.5） |
| 四组分温度均值误差 | 约 1.3 K |
| 单组分温度 RMSE | 约 5.9 K |
| 观测 RMSE | 0.17 K |
| Jacobian 条件数 | 约 5.8e3 |

结论：LAI/有效温度可识别；**单个晴/阴组分温度仅靠多波段 BT 不能独立反演**（条件数高、组分温度强相关）。需要多角度/多波段 TIR、光学 BRF、时间序列或更强先验。

## 9. 待确认

1. 下一步：加完整光学 BRF / 接大气 / EB 时间序列 / 多像元 JAX 矢量化。
2. 观测源：Landsat、Sentinel-2/3、UAV，或先做 OSSE。
3. 是否允许 Python/JAX 原型与 `stream.exe` 双实现，还是必须单一 C++ 二进制。
