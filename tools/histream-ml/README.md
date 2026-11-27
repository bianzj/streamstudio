# histream-ml：FacetEB 神经代理与可微反演原型

HiStream 三维面元能量平衡（FacetEB）的**神经代理**、**引擎内切线敏感度**的读取，以及基于这两种梯度的参数反演。设计、验证数字与边界见
[docs/differentiable-inversion-and-surrogate.md](../../docs/differentiable-inversion-and-surrogate.md)。

状态：研究原型，命令行使用，**未接入服务端和工作台**。覆盖 `feb_crop`（C4 玉米，FacetEB）：两节点（参数空间 v1）与 48 节点日循环（v2，加 BSM 土壤含水量）。

## 环境

代理训练需要 PyTorch，不进项目依赖；用独立 venv（CPU 版足够，训练 5 个成员约半分钟）：

```powershell
python -m venv <env>
<env>\Scripts\python -m pip install torch --index-url https://download.pytorch.org/whl/cpu
<env>\Scripts\python -m pip install -r tools\histream-ml\requirements.txt
```

引擎取 `models/bin_x64/Release/histream.exe`，可用环境变量 `HISTREAM_ENGINE_DIR` 改（`osse_engine.py` 另有 `--engine-dir`）。引擎内敏感度需要 2026-11-27 之后构建的引擎（`control.facetSensitivity`）。生成的数据、模型放在项目外（例如 `<work>`），不要提交。

## 流程

```powershell
# 0. 冻结输入（与回归测试相同）：生成 <work>/cases/feb_crop/input（两节点）
node tools/regression/prepare.mjs <work> feb_crop
#    日循环：复制该目录为 cases/feb_crop_day/input，并把 project.json 的 meteo.start/end 改成 0/48

# 1. 采样并运行引擎（两节点每点约 4 s；日循环约 48 s，用 --lean 只写能量过程文件）
python gen_dataset.py --case <work>/cases/feb_crop/input --out <work>/data/train.jsonl --n 512 --seed 1 --workers 3
python gen_dataset.py --case <work>/cases/feb_crop/input --out <work>/data/test.jsonl  --n 64  --seed 99 --workers 3
python gen_dataset.py --case <work>/cases/feb_crop/input --out <work>/data/repeat.jsonl --repeat 6     # 引擎重复性
python gen_dataset.py --case <work>/cases/feb_crop_day/input --out <work>/day/train.jsonl --n 512 --seed 1 --space v2 --lean

# 2. 训练代理（5 成员集成），测试集评估，导出 ONNX，算交叉验证残差（反演用的代理误差协方差）
python train_surrogate.py --train <work>/data/train.jsonl --test <work>/data/test.jsonl --out <work>/surrogate
#    日循环加 --include-unconverged（个别面元在阈值附近振荡，类别均值已稳定，见文档）

# 3. 核对梯度：代理自动微分雅可比 vs 引擎中心差分
python check_gradients.py --case <work>/cases/feb_crop/input --model <work>/surrogate --test <work>/data/test.jsonl

# 4. 单个 OSSE 反演：代理 MAP + Laplace + 引擎校正迭代
python invert.py --case <work>/cases/feb_crop/input --model <work>/surrogate --test <work>/data/test.jsonl --index 5 --obs temperature

# 5. 统计 OSSE：多真值 × 多噪声，后验覆盖率；--refine N 再做 N 次引擎校正
python osse_stats.py --model <work>/surrogate --train <work>/data/train.jsonl --test <work>/data/test.jsonl `
    --case <work>/cases/feb_crop/input --seeds 3 --refine 40 --out <work>/osse_stats.json

# 6. 只用引擎（不用代理）的 Gauss–Newton OSSE：雅可比来自引擎内切线敏感度（不支持的参数用引擎差分补列）
python osse_engine.py --case <work>/cases/feb_crop/input --test <work>/data/test.jsonl --truths 6 --out <work>/osse_engine.json
```

## 文件

| 文件 | 作用 |
|---|---|
| `hsml/params.py` | 参数空间 v1/v2（Vcmax、Ball-Berry 斜率、土壤导热率、土壤表面阻抗、[v2] BSM 含水量；气温偏移、风速倍率、短波倍率），写入工程与气象文件；数据与模型自动切换到记录的空间 |
| `hsml/engine.py` | 运行 `histream.exe eFacetEB`，把结果压成类别均值观测向量；`run(..., sensitivity=[...])` 读引擎内切线敏感度，返回归一化坐标的雅可比 |
| `hsml/surrogate.py` | MLP 集成（线性旁路 + SiLU）、评估、交叉验证残差与代理误差协方差、ONNX 导出 |
| `hsml/inversion.py` | 可微 MAP（torch 雅可比 + scipy 信赖域，代理误差按完整协方差白化）、Laplace 后验、引擎校正迭代、只用引擎的 Levenberg–Marquardt（`gauss_newton_engine`）、有限差分雅可比 |
| `gen_dataset.py` / `train_surrogate.py` / `check_gradients.py` / `invert.py` / `osse_stats.py` / `osse_engine.py` | 命令行入口 |

## 扩展参数或场景

在 `hsml/params.py` 的 `SPACES` 里加一个新空间（不要改已有空间，否则旧数据与模型对不上），并在 `apply_to_project` / `apply_to_meteo` 写入方式；观测向量在 `hsml/engine.py` 的 `LEAF_FIELDS` / `GROUND_FIELDS`。引擎内敏感度目前支持的参数见 `hsml/engine.py` 的 `ENGINE_SENSITIVITY`（对应引擎 `control.facetSensitivity`：`leaf.vcmax`、`leaf.ballBerrySlope`、`ground.surfaceResistance`、`ground.thermalConductivity`）。
