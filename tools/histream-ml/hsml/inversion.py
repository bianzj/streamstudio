"""基于神经代理的可微 MAP 反演、Laplace 后验、引擎校正迭代和梯度核对。

记号：u 为归一化参数（params.py），y_s(u) 为代理预测，y_e(u) 为引擎结果。
代价（高斯假设）：J(u) = ½ ‖W (ŷ(u) − y_obs)‖² + ½ Σ ((u − u_prior)/σ_prior)²，u 限制在 [0,1] 盒内；
W 是 (观测噪声 + 代理误差协方差) 的 Cholesky 逆（白化），代理误差取训练集交叉验证残差（surrogate.py）。

梯度来源（见 docs/differentiable-inversion-and-surrogate.md）：
1. 代理自动微分（map_estimate / laplace）：快，只在训练域内，幅值受代理误差限制；
2. 引擎内收敛点切线敏感度（engine.FacetEngine.run(sensitivity=...)，gauss_newton_engine）：不用代理，
   节点间热历史冻结；
3. 引擎外部中心差分（finite_difference_jacobian）：慢，且受求解器停止误差污染，只作核对。

两条反演路线：
- A：代理 MAP → refine_with_engine。每步用引擎在当前点的残差把代理平移
  ŷ(u) = y_s(u) + (y_e(u_k) − y_s(u_k))，在信赖域内解 MAP；代理只提供雅可比。不动点处引擎输出与观测的失配
  满足 MAP 驻点条件（雅可比取代理值），代理的偏差不再进入结果。
- B：gauss_newton_engine，只用引擎输出与引擎雅可比的 Levenberg–Marquardt。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable

import numpy as np
import torch
from scipy.optimize import least_squares

from . import params as P
from .surrogate import Surrogate


@dataclass
class Problem:
    model: Surrogate                      # 代理（会转成 float64 使用）
    obs_index: np.ndarray                 # 使用的观测在输出向量中的下标
    y_obs: np.ndarray                     # 对应的观测值
    sigma_obs: np.ndarray                 # 观测标准差
    free: list[str]                       # 待估参数名
    u_fixed: np.ndarray                   # 全参数向量里固定部分的取值（free 位置的值被忽略）
    prior_u: np.ndarray                   # free 的先验均值（归一化坐标）
    prior_sd_u: np.ndarray                # free 的先验标准差（归一化坐标）
    sigma_model: np.ndarray | None = None  # 代理误差标准差（与 obs_index 对应，对角），None 表示不计入
    cov_model: np.ndarray | None = None    # 代理误差协方差（m×m，与 obs_index 对应）；给出时优先于 sigma_model

    def __post_init__(self):
        self.model = self.model.to(torch.float64)
        self.free_idx = np.array([P.NAMES.index(n) for n in self.free])
        C = np.diag(self.sigma_obs ** 2)
        if self.cov_model is not None:
            C = C + self.cov_model
        elif self.sigma_model is not None:
            C = C + np.diag(self.sigma_model ** 2)
        self.sigma = np.sqrt(np.diag(C))
        # 白化矩阵 W = L⁻¹（C = L Lᵀ）：残差 W (ŷ − y_obs) 的各分量独立、单位方差。
        self.W = np.linalg.inv(np.linalg.cholesky(C))
        self.W_obs = np.diag(1.0 / self.sigma_obs)

    def full_u(self, u_free) -> torch.Tensor:
        base = torch.as_tensor(self.u_fixed, dtype=torch.float64)
        idx = torch.as_tensor(self.free_idx)
        return base.index_put((idx,), u_free) if isinstance(u_free, torch.Tensor) else \
            base.index_put((idx,), torch.as_tensor(u_free, dtype=torch.float64))

    def predict_full(self, u_free) -> torch.Tensor:
        """代理对全部输出的预测（物理单位）。"""
        return self.model.mean_fn(self.full_u(u_free))


def _residual_fn(prob: Problem, offset: np.ndarray | None, obs_only: bool = False):
    """obs_only=True 只用观测噪声（引擎校正阶段：偏差已由引擎残差消除）。"""
    W = torch.as_tensor(prob.W_obs if obs_only else prob.W, dtype=torch.float64)
    y_obs = torch.as_tensor(prob.y_obs, dtype=torch.float64)
    prior = torch.as_tensor(prob.prior_u, dtype=torch.float64)
    prior_sd = torch.as_tensor(prob.prior_sd_u, dtype=torch.float64)
    sel = torch.as_tensor(prob.obs_index)
    off = None if offset is None else torch.as_tensor(offset, dtype=torch.float64)

    def residual(u_free: torch.Tensor) -> torch.Tensor:
        y = prob.predict_full(u_free)
        if off is not None:
            y = y + off
        return torch.cat([W @ (y[sel] - y_obs), (u_free - prior) / prior_sd])
    return residual


def map_estimate(prob: Problem, u0: np.ndarray, offset: np.ndarray | None = None,
                 obs_only: bool = False, bounds: tuple[np.ndarray, np.ndarray] | None = None) -> dict:
    """Gauss–Newton 型信赖域（scipy trf）+ torch 精确雅可比；盒约束缺省 [0,1]，bounds 可收紧（引擎校正的信赖域）。"""
    res = _residual_fn(prob, offset, obs_only)
    lo, hi = (np.zeros_like(u0), np.ones_like(u0)) if bounds is None else bounds

    def f(x):
        with torch.no_grad():
            return res(torch.as_tensor(x, dtype=torch.float64)).numpy()

    def jac(x):
        return torch.autograd.functional.jacobian(res, torch.as_tensor(x, dtype=torch.float64)).numpy()

    # scipy 要求初值严格在盒内
    span = np.maximum(hi - lo, 1e-12)
    x0 = np.clip(u0, lo + 1e-6 * span, hi - 1e-6 * span)
    sol = least_squares(f, x0, jac=jac, bounds=(lo, hi), method="trf",
                        x_scale=1.0, xtol=1e-10, ftol=1e-12, gtol=1e-10, max_nfev=200)
    return {"u": sol.x, "cost": float(sol.cost), "nfev": int(sol.nfev), "status": int(sol.status)}


def laplace(prob: Problem, u_hat: np.ndarray, offset: np.ndarray | None = None, obs_only: bool = False) -> dict:
    """在 u_hat 处线性化的后验协方差 (JᵀJ + Σp⁻¹)⁻¹，以及数据信息矩阵的特征谱（可辨识性）。

    注意：边界处（u=0 或 1）线性高斯近似不再可靠；报告里标出贴边参数。"""
    res = _residual_fn(prob, offset, obs_only)
    J = torch.autograd.functional.jacobian(res, torch.as_tensor(u_hat, dtype=torch.float64)).numpy()
    m = len(prob.obs_index)
    Jd, Jp = J[:m], J[m:]
    info_data = Jd.T @ Jd
    H = info_data + Jp.T @ Jp
    cov = np.linalg.inv(H)
    sd_u = np.sqrt(np.diag(cov))
    # 转成物理单位的近似标准差：dθ/du 在 u_hat 处的局部斜率
    theta = []
    for j, name in enumerate(prob.free):
        p = P.BY_NAME[name]
        theta.append(abs(float(p.from_unit(min(u_hat[j] + 1e-4, 1.0)) - p.from_unit(max(u_hat[j] - 1e-4, 0.0)))
                         / (min(u_hat[j] + 1e-4, 1.0) - max(u_hat[j] - 1e-4, 0.0))))
    sd_theta = sd_u * np.array(theta)
    corr = cov / np.outer(sd_u, sd_u)
    w, v = np.linalg.eigh(info_data)
    order = np.argsort(w)[::-1]
    return {
        "sd_u": sd_u, "sd_theta": sd_theta, "corr": corr,
        "info_eigenvalues": w[order], "info_eigenvectors": v[:, order],
        "prior_sd_u": prob.prior_sd_u,
        "shrinkage": sd_u / prob.prior_sd_u,                 # 后验 sd / 先验 sd：越小数据信息越多
        "at_bound": [name for name, x in zip(prob.free, u_hat) if x < 1e-3 or x > 1 - 1e-3],
        "jacobian_data": Jd,
    }


@dataclass
class RefineHistory:
    u: list[np.ndarray] = field(default_factory=list)
    cost: list[float] = field(default_factory=list)       # 引擎评价的代价（含先验项）
    chi2: list[float] = field(default_factory=list)       # 只含观测项
    alpha: list[float] = field(default_factory=list)
    y_engine: list[np.ndarray] = field(default_factory=list)


def refine_with_engine(prob: Problem, run_engine: Callable[[np.ndarray], np.ndarray], u0: np.ndarray,
                       max_iter: int = 6, tol: float = 5e-3, radius: float = 0.2,
                       verbose: bool = True) -> tuple[np.ndarray, RefineHistory]:
    """引擎校正迭代（代理雅可比 + 引擎残差）。run_engine(u_free) 返回引擎的全部输出向量。

    观测噪声 σ 只取观测不确定度（不再叠加代理误差）：校正项已在当前点消除了代理的偏差。
    信赖域：校正后的代理只在 u ± radius（归一化坐标）的盒子里被信任，在盒内求 MAP；引擎代价下降且
    步长顶到盒边时半径加倍（上限 1），代价不降（或引擎在试探点失败）时半径减半、重解，最多 4 次。
    代理较弱时（例如样本少的日循环代理），无约束的整步会在量程里来回跳。"""
    hist = RefineHistory()
    y_obs = prob.y_obs
    sel = prob.obs_index
    prior, psd = prob.prior_u, prob.prior_sd_u

    def engine_cost(u, ye):
        chi2 = 0.5 * float((((ye[sel] - y_obs) / prob.sigma_obs) ** 2).sum())
        return chi2 + 0.5 * float((((u - prior) / psd) ** 2).sum()), chi2

    u = np.clip(u0, 0.0, 1.0)
    ye = run_engine(u)
    cost, chi2 = engine_cost(u, ye)
    hist.u.append(u.copy()); hist.cost.append(cost); hist.chi2.append(chi2); hist.alpha.append(radius); hist.y_engine.append(ye)
    if verbose:
        print(f"  迭代 0：引擎代价 {cost:.2f}（观测项 {chi2:.2f}）", flush=True)
    r = radius
    for it in range(1, max_iter + 1):
        with torch.no_grad():
            ys = prob.predict_full(torch.as_tensor(u, dtype=torch.float64)).numpy()
        offset = ye - ys
        accepted, converged = False, False
        u_try, ye_try, c_try, chi_try, moved = u, ye, np.inf, np.inf, 0.0
        for _ in range(4):
            box = (np.maximum(u - r, 0.0), np.minimum(u + r, 1.0))
            u_try = map_estimate(prob, u, offset, obs_only=True, bounds=box)["u"]
            moved = float(np.abs(u_try - u).max())
            if moved < tol:
                converged = True
                break
            try:
                ye_try = run_engine(u_try)
                c_try, chi_try = engine_cost(u_try, ye_try)
            except RuntimeError as exc:           # 引擎在个别试探点可能自检失败（EngineError 继承 RuntimeError）：当作坏点
                c_try, chi_try = np.inf, np.inf
                if verbose:
                    print(f"    试探点引擎失败（{str(exc).splitlines()[0][:80]}），缩小信赖域", flush=True)
            if c_try <= cost + 1e-9:
                accepted = True
                break
            r *= 0.5
        if converged:
            if verbose:
                print(f"  迭代 {it}：信赖域（半径 {r:.3f}）内的校正 MAP 步长 {moved:.4f} < {tol}，视为收敛", flush=True)
            break
        if verbose:
            print(f"  迭代 {it}：信赖域半径 {r:.3f}，最大 |Δu| {moved:.4f}，引擎代价 {c_try:.2f}（观测项 {chi_try:.2f}）"
                  f"{'' if accepted else '，未下降，停止'}", flush=True)
        if not accepted:
            break
        u, ye, cost, chi2 = u_try, ye_try, c_try, chi_try
        hist.u.append(u.copy()); hist.cost.append(cost); hist.chi2.append(chi2); hist.alpha.append(r); hist.y_engine.append(ye)
        if moved >= 0.9 * r:
            r = min(2.0 * r, 1.0)
    return u, hist


def finite_difference_jacobian(run_engine: Callable[[np.ndarray], np.ndarray], u: np.ndarray, free_idx: list[int],
                               h: float = 0.03) -> np.ndarray:
    """引擎的中心差分雅可比 ∂y/∂u（物理输出对归一化参数）。引擎对同一输入逐位重复（已实测），
    所以差分只受离散化/收敛误差影响；步长 h 取量程的 3%，贴边时改单边。"""
    cols = []
    for j in free_idx:
        up, um = u.copy(), u.copy()
        up[j] = min(u[j] + h, 1.0)
        um[j] = max(u[j] - h, 0.0)
        cols.append((run_engine(up) - run_engine(um)) / (up[j] - um[j]))
    return np.stack(cols, axis=1)


def surrogate_jacobian(model: Surrogate, u: np.ndarray, free_idx: list[int]) -> np.ndarray:
    m64 = model.to(torch.float64)
    x = torch.as_tensor(u, dtype=torch.float64)
    J = torch.autograd.functional.jacobian(m64.mean_fn, x).numpy()
    return J[:, free_idx]


@dataclass
class EngineGNResult:
    u: np.ndarray
    cov_u: np.ndarray                      # Laplace 后验协方差（归一化坐标，引擎雅可比）
    sd_u: np.ndarray
    shrinkage: np.ndarray
    info_eigenvalues: np.ndarray
    history: list[dict]
    engine_runs: int
    wall_s: float
    y: np.ndarray


def gauss_newton_engine(evaluate: Callable[[np.ndarray, bool], tuple[np.ndarray, np.ndarray | None, int]],
                        obs_index: np.ndarray,
                        y_obs: np.ndarray, sigma_obs: np.ndarray, prior_u: np.ndarray, prior_sd_u: np.ndarray,
                        u0: np.ndarray, max_iter: int = 8, tol: float = 2e-3, verbose: bool = True) -> EngineGNResult:
    """只用引擎（不用代理）的 Levenberg–Marquardt / MAP，盒约束 [0,1]。

    evaluate(u_free, need_jacobian) -> (y 全部输出, J = ∂y/∂u_free 或 None, 本次引擎运行数)。J 由引擎内切线
    敏感度给出（必要时个别列用引擎差分）。试探步只要 y（一次普通运行）；步长被接受后才求该点的雅可比。"""
    import time as _time
    t0 = _time.perf_counter()
    W = np.diag(1.0 / sigma_obs)

    def cost_of(u, y):
        r = W @ (y[obs_index] - y_obs)
        p = (u - prior_u) / prior_sd_u
        return 0.5 * float(r @ r + p @ p), 0.5 * float(r @ r)

    u = np.clip(u0, 0.0, 1.0)
    y, J, runs = evaluate(u, True)
    cost, chi2 = cost_of(u, y)
    lam = 1e-3
    history = [{"u": u.tolist(), "cost": cost, "chi2": chi2, "lambda": lam}]
    if verbose:
        print(f"  GN 0：代价 {cost:.3f}（观测项 {chi2:.3f}）", flush=True)
    for it in range(1, max_iter + 1):
        Jw = W @ J[obs_index]
        r = W @ (y[obs_index] - y_obs)
        H = Jw.T @ Jw + np.diag(1.0 / prior_sd_u ** 2)
        g = Jw.T @ r + (u - prior_u) / prior_sd_u ** 2
        accepted = False
        c_try = chi_try = float("inf")
        u_try, y_try = u, y
        for _ in range(5):
            step = -np.linalg.solve(H + lam * np.diag(np.diag(H)), g)
            u_try = np.clip(u + step, 0.0, 1.0)
            try:
                y_try, _, n = evaluate(u_try, False)
            except RuntimeError as exc:
                if verbose:
                    print(f"    试探点引擎失败（{str(exc).splitlines()[0][:80]}），增大阻尼", flush=True)
                lam *= 10
                continue
            runs += n
            c_try, chi_try = cost_of(u_try, y_try)
            if c_try <= cost + 1e-9:
                accepted = True
                break
            lam *= 10
        moved = float(np.abs(u_try - u).max()) if accepted else 0.0
        if verbose:
            print(f"  GN {it}：λ={lam:.0e}，最大 |Δu| {moved:.4f}，代价 {c_try:.3f}（观测项 {chi_try:.3f}）"
                  f"{'' if accepted else '，未下降，停止'}", flush=True)
        if not accepted:
            break
        u, y, cost, chi2 = u_try, y_try, c_try, chi_try
        lam = max(lam / 10, 1e-6)
        history.append({"u": u.tolist(), "cost": cost, "chi2": chi2, "lambda": lam})
        if moved < tol:
            break
        _, J, n = evaluate(u, True)
        runs += n
    Jw = W @ J[obs_index]
    info = Jw.T @ Jw
    cov = np.linalg.inv(info + np.diag(1.0 / prior_sd_u ** 2))
    sd = np.sqrt(np.diag(cov))
    w = np.sort(np.linalg.eigvalsh(info))[::-1]
    return EngineGNResult(u, cov, sd, sd / prior_sd_u, w, history, runs, _time.perf_counter() - t0, y)
