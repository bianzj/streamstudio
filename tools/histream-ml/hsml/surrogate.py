"""神经代理：把 FacetEB 的“参数 → 类别均值观测向量”映射学成一个可微的小 MLP 集成。

结构：y = A·u + MLP(u)（线性旁路 + SiLU MLP，输出已按训练集标准化）。线性旁路让少量样本下的
一阶响应更稳；SiLU 处处可微，反演的雅可比不会出现 ReLU 的折点。K 个成员用不同初始化与自助重采样，
成员间离散度是认知不确定度的粗略指标（不是校准过的预测区间，真实误差以独立测试集为准）。

用途与边界：
- 允许：大规模参数扫描、反演的梯度与初值、不确定度传播、预览；
- 不允许：替代引擎给出最终产品。反演结果必须回到引擎复核（inversion.refine_with_engine）。
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import torch
from torch import nn

from . import params as P


def load_jsonl(path: str | Path, include_unconverged: bool = False) -> dict:
    """include_unconverged：保留整次运行未全部收敛的样本。日循环里下午少数节点在 60 次（引擎上限 100 次）
    耦合迭代内不收敛，原因是个别面元在 2 W m-2 阈值附近振荡；实测 60 与 100 次的类别均值差 < 5e-5 K、
    < 5e-4 W m-2，所以这类样本对类别均值观测是可用的。"""
    rows = [json.loads(l) for l in Path(path).read_text(encoding="utf-8-sig").splitlines() if l.strip()]
    if not include_unconverged:
        rows = [r for r in rows if r.get("converged", True)]   # 未收敛点不进训练/测试
    rows.sort(key=lambda r: r["id"])
    P.set_space_for(list(rows[0]["theta"].keys()))             # 数据记录了它所用的参数空间
    return {
        "u": np.array([r["u"] for r in rows], dtype=np.float64),
        "y": np.array([r["y"] for r in rows], dtype=np.float64),
        "names": rows[0]["names"],
        "ids": [r["id"] for r in rows],
    }


class Net(nn.Module):
    def __init__(self, d_in: int, d_out: int, width: int = 96, depth: int = 3):
        super().__init__()
        layers: list[nn.Module] = []
        d = d_in
        for _ in range(depth):
            layers += [nn.Linear(d, width), nn.SiLU()]
            d = width
        layers.append(nn.Linear(d, d_out))
        self.mlp = nn.Sequential(*layers)
        self.skip = nn.Linear(d_in, d_out, bias=False)

    def forward(self, u):                       # u ∈ [0,1]^d
        z = 2.0 * u - 1.0
        return self.skip(z) + self.mlp(z)


@dataclass
class Surrogate:
    """K 个成员 + 输出标准化。predict 返回物理单位。"""
    nets: list[Net]
    y_mean: torch.Tensor
    y_std: torch.Tensor
    names: list[str]
    param_names: list[str]

    def standardized(self, u: torch.Tensor) -> torch.Tensor:
        return torch.stack([n(u) for n in self.nets], dim=0)           # (K, ..., m)

    def mean_fn(self, u: torch.Tensor) -> torch.Tensor:
        """集成均值，物理单位，对 u 可微。"""
        return self.standardized(u).mean(0) * self.y_std + self.y_mean

    def predict(self, u: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        with torch.no_grad():
            s = self.standardized(torch.as_tensor(np.atleast_2d(u), dtype=self.y_mean.dtype))
            mean = s.mean(0) * self.y_std + self.y_mean
            std = s.std(0, unbiased=False) * self.y_std
        return mean.numpy(), std.numpy()

    def to(self, dtype):
        self.nets = [n.to(dtype) for n in self.nets]
        self.y_mean, self.y_std = self.y_mean.to(dtype), self.y_std.to(dtype)
        return self

    # -- 保存/读取 ----------------------------------------------------------------------------
    def save(self, directory: str | Path, metrics: dict | None = None, extra: dict | None = None) -> None:
        directory = Path(directory)
        directory.mkdir(parents=True, exist_ok=True)
        torch.save({"states": [n.state_dict() for n in self.nets], "y_mean": self.y_mean, "y_std": self.y_std,
                    "shape": [self.nets[0].skip.in_features, self.nets[0].skip.out_features,
                              self.nets[0].mlp[0].out_features, (len(self.nets[0].mlp) - 1) // 2]},
                   directory / "surrogate.pt")
        meta = {"names": self.names, "param_names": self.param_names,
                "params": [{"name": p.name, "lo": p.lo, "hi": p.hi, "log": p.log, "unit": p.unit, "role": p.role}
                           for p in P.PARAMS if p.name in self.param_names],
                "members": len(self.nets), "metrics": metrics or {}, **(extra or {})}
        (directory / "surrogate.json").write_text(json.dumps(meta, ensure_ascii=False, indent=1), encoding="utf-8")

    @staticmethod
    def load(directory: str | Path) -> "Surrogate":
        directory = Path(directory)
        blob = torch.load(directory / "surrogate.pt", map_location="cpu", weights_only=False)
        d_in, d_out, width, depth = blob["shape"]
        nets = []
        for state in blob["states"]:
            net = Net(d_in, d_out, width, depth)
            net.load_state_dict(state)
            net.eval()
            nets.append(net)
        meta = json.loads((directory / "surrogate.json").read_text(encoding="utf-8"))
        P.set_space_for(meta["param_names"])
        return Surrogate(nets, blob["y_mean"], blob["y_std"], meta["names"], meta["param_names"])


def train(u: np.ndarray, y: np.ndarray, names: list[str], members: int = 5, epochs: int = 6000,
          lr: float = 5e-3, weight_decay: float = 1e-4, width: int = 96, depth: int = 3, seed: int = 0,
          bootstrap: bool = True, verbose: bool = True) -> Surrogate:
    """全批量 Adam + 余弦退火。样本只有几百个，一个成员在 CPU 上几十秒。"""
    torch.set_num_threads(4)
    ut = torch.as_tensor(u, dtype=torch.float32)
    y_mean = torch.as_tensor(y.mean(0), dtype=torch.float32)
    y_std = torch.as_tensor(np.maximum(y.std(0), 1e-6), dtype=torch.float32)
    yt = (torch.as_tensor(y, dtype=torch.float32) - y_mean) / y_std
    nets = []
    n = ut.shape[0]
    for k in range(members):
        g = torch.Generator().manual_seed(seed * 1000 + k)
        torch.manual_seed(seed * 1000 + k)
        idx = torch.randint(0, n, (n,), generator=g) if bootstrap and k > 0 else torch.arange(n)
        net = Net(ut.shape[1], yt.shape[1], width, depth)
        opt = torch.optim.Adam(net.parameters(), lr=lr, weight_decay=weight_decay)
        sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=epochs, eta_min=lr * 1e-3)
        for ep in range(epochs):
            opt.zero_grad(set_to_none=True)
            loss = torch.mean((net(ut[idx]) - yt[idx]) ** 2)
            loss.backward()
            opt.step()
            sched.step()
        net.eval()
        nets.append(net)
        if verbose:
            with torch.no_grad():
                print(f"  成员 {k + 1}/{members}：训练集标准化 MSE {torch.mean((net(ut) - yt) ** 2).item():.3e}", flush=True)
    return Surrogate(nets, y_mean, y_std, names, list(P.NAMES))


def evaluate(model: Surrogate, u: np.ndarray, y: np.ndarray) -> dict:
    """独立测试集上的误差，物理单位。目标：温度 0.1 K，通量 0.25 W m-2（类别均值口径）。"""
    mean, spread = model.predict(u)
    err = mean - y
    names = model.names
    is_temp = np.array([n.endswith((".T", ".Tsun", ".Tsh")) for n in names])
    target = np.where(is_temp, 0.1, 0.25)
    rmse = np.sqrt((err ** 2).mean(0))
    return {
        "n_test": int(len(u)),
        "rmse": {n: float(v) for n, v in zip(names, rmse)},
        "max_abs": {n: float(v) for n, v in zip(names, np.abs(err).max(0))},
        "ensemble_spread_mean": {n: float(v) for n, v in zip(names, spread.mean(0))},
        "target": {n: float(v) for n, v in zip(names, target)},
        "within_target_fraction": {n: float((np.abs(err[:, i]) <= target[i]).mean()) for i, n in enumerate(names)},
        "summary": {
            "rmse_temperature_median_K": float(np.median(rmse[is_temp])),
            "rmse_temperature_max_K": float(rmse[is_temp].max()),
            "rmse_flux_median_W_m2": float(np.median(rmse[~is_temp])),
            "rmse_flux_max_W_m2": float(rmse[~is_temp].max()),
        },
    }


def cross_validated_residuals(u: np.ndarray, y: np.ndarray, names: list[str], folds: int = 5, members: int = 3,
                              seed: int = 0, **train_kwargs) -> np.ndarray:
    """K 折交叉验证的样本外残差（预测 − 引擎），形状同 y。只用训练集，不碰测试集。

    用它估计代理误差的协方差（反演似然里的模型误差项）。每折用 members 个成员的小集成，
    比最终 5 成员模型略差，所以估计偏保守。"""
    n = len(u)
    order = np.random.default_rng(seed).permutation(n)
    residuals = np.zeros_like(y)
    for k, hold in enumerate(np.array_split(order, folds)):
        keep = np.setdiff1d(np.arange(n), hold)
        m = train(u[keep], y[keep], names, members=members, seed=seed + 17 * (k + 1), verbose=False, **train_kwargs)
        residuals[hold] = m.predict(u[hold])[0] - y[hold]
    return residuals


def model_error_covariance(residuals: np.ndarray, shrink: bool = True) -> np.ndarray:
    """代理误差协方差 E[r rᵀ]（不减均值：系统偏差也算误差）。

    shrink=True 时把相关矩阵向单位阵收缩（Schäfer & Strimmer 2005 的解析强度）：样本数少于或接近输出
    维数时（日循环 576 维、百余样本）样本协方差秩亏且噪声大，收缩后仍正定、对角（各输出方差）不变；
    样本充足时强度自动趋近 0。"""
    n, m = residuals.shape
    second = residuals.T @ residuals / n
    if not shrink or n < 3:
        return second
    sd = np.sqrt(np.maximum(np.diag(second), 1e-30))
    x = residuals / sd                                     # 按 RMS 标准化（与不减均值的口径一致）
    corr = x.T @ x / n
    # Var(r_ij) 的无偏估计：w_kij = x_ki x_kj，Var = n/(n-1)^3 Σ_k (w_kij − w̄_ij)²
    sum_w2 = (x ** 2).T @ (x ** 2)
    var_r = n / (n - 1) ** 3 * (sum_w2 - n * corr ** 2)
    off = ~np.eye(m, dtype=bool)
    denom = float((corr[off] ** 2).sum())
    lam = 1.0 if denom <= 0 else float(np.clip(var_r[off].sum() / denom, 0.0, 1.0))
    shrunk = corr * (1.0 - lam)
    np.fill_diagonal(shrunk, 1.0)
    model_error_covariance.last_shrinkage = lam            # 供报告读取
    return shrunk * np.outer(sd, sd)


model_error_covariance.last_shrinkage = float("nan")


def export_onnx(model: Surrogate, path: str | Path, check_u: np.ndarray | None = None) -> float:
    """导出 ONNX：输入 u (N, d) float32；输出 mean、std（集成均值与成员标准差，物理单位）。
    返回与 PyTorch 的最大绝对差（用 onnxruntime 复核）。"""

    class Wrapper(nn.Module):
        def __init__(self, m: Surrogate):
            super().__init__()
            self.nets = nn.ModuleList(m.nets)
            self.register_buffer("y_mean", m.y_mean)
            self.register_buffer("y_std", m.y_std)

        def forward(self, u):
            s = torch.stack([n(u) for n in self.nets], dim=0)
            mean = s.mean(0)
            var = ((s - mean) ** 2).mean(0)
            return mean * self.y_std + self.y_mean, torch.sqrt(var + 1e-12) * self.y_std

    wrapper = Wrapper(model).eval()
    d = len(model.param_names)
    dummy = torch.zeros(2, d, dtype=torch.float32)
    torch.onnx.export(wrapper, (dummy,), str(path), input_names=["u"], output_names=["mean", "std"],
                      dynamic_axes={"u": {0: "n"}, "mean": {0: "n"}, "std": {0: "n"}}, opset_version=17, dynamo=False)
    import onnxruntime as ort
    sess = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
    probe = (check_u if check_u is not None else np.random.default_rng(0).random((16, d))).astype(np.float32)
    o_mean, o_std = sess.run(["mean", "std"], {"u": probe})
    with torch.no_grad():
        t_mean, t_std = wrapper(torch.as_tensor(probe))
    return float(max(np.abs(o_mean - t_mean.numpy()).max(), np.abs(o_std - t_std.numpy()).max()))
