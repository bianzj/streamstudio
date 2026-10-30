# STREAM 可微反演原型（单像元）

## 角色分工

- **HiStream / STREAM-NET**：生成样本、做正向模拟的残差修正（5 m 城市/航空场景，亮温残差）；
- **STREAM**：反演算子。本文具把 STREAM 的单像元正向链做成可微的 `H(x)`，供变分同化/Gauss–Newton 反演使用；
- 合成样本（`--export-sample`）模拟 HiStream 导出的“参数—观测”对；真实 HiStream 样本可按同一 JSON 结构接入。

## 已实现

- 均匀冠层四组分比例（TiRT 标准闭合、双尺度热点）；
- `hom3` 节点辐射度（土壤 + 两层冠层、5 状态、多次散射）；
- 有效发射率与 Planck 辐亮度/亮温；
- 视角间隙 `gap`（LAI 敏感观测代理）；
- 基于 autograd 的梯度与 Gauss–Newton 反演（`scipy.optimize.least_squares`）；
- 梯度校验与合成 LAI/温度反演 demo。

数值对应 `models/stream/physics/canopy.cpp`、`radiosity.cpp`、`brightness.h`；尚未包含完整球面叶片 BRF、大气和 EB 时间序列。

## 运行

依赖：numpy、scipy；`autograd` 已放在 `vendor/`（1.9.1，无需安装）。

```powershell
$env:PYTHONPATH = "C:\work\streamstudio\tools\stream-inverse;C:\work\streamstudio\tools\stream-inverse\vendor"
C:\work\miniconda\python.exe tools\stream-inverse\stream_inverse.py --demo
```

其他入口：

```powershell
# 导出一份 HiStream 风格的样本 JSON
python stream_inverse.py --export-sample demo-sample.json

# 在样本上做反演
python stream_inverse.py --sample demo-sample.json
```

## demo 结果（噪声 0.3 K，seed=1）

| 量 | 结果 |
| --- | --- |
| 梯度相对误差（autograd vs 中心差分） | `7e-9` |
| LAI | 真值 2.5 → 反演 2.46（误差 0.04） |
| 四组分温度均值误差 | 约 1.3 K |
| 单组分温度 RMSE | 约 5.9 K |
| 亮温/间隙观测 RMSE | 0.17 K |
| 观测 Jacobian 条件数 | 约 5.8e3 |

结论：**LAI 和有效温度可识别，单个晴/阴组分温度高度相关**。只有多波段亮温时，4 个组分温度不能独立反演；需要多角度、多波段 TIR、光学 BRF、时间序列或更强先验。这是物理模型的真实可识别性，不是优化器问题。

## 样本 JSON 结构

```jsonc
{
  "kind": "stream-inversion-sample",
  "schemaVersion": 1,
  "config": {
    "sza": 35.0, "saa": 150.0,
    "views": [[0, 0], [35, 90], [55, 180]],
    "bands": [10.5, 11.5, 12.0],
    "hotspot": 0.2,
    "leaf_emissivity": 0.985, "soil_emissivity": 0.955
  },
  "observations": [/* bt: views x bands, then gap: views */],
  "sigma_bt": 0.3, "sigma_gap": 0.02,
  "truth": [2.5, 312, 302, 308, 301],
  "prior": [2.0, 305, 300, 303, 300],
  "prior_sd": [1.0, 8, 8, 8, 8],
  "initial": [1.5, 302, 298, 301, 299]
}
```

`truth` 仅用于评估；HiStream 导出时可省略。

## 下一步

1. 加完整 `canopyReflectance`（球面叶片一次散射 + 节点多次散射），用光学 BRF 约束 LAI/光谱；
2. 接大气 LUT（`assets/atmosphere/simple_modtran_lut.csv`）或外部大气校正；
3. EB 温度用隐式微分或弱约束，支持时间序列 4D-Var/EnKF；
4. 多像元矢量化（JAX）与 HiStream 样本批量接入；
5. 评估后决定是否进 `stream.exe` 或作为工作台“反演”模式的后端。
