"""OSSE：用独立测试集里的引擎结果当“真值”，加噪声生成观测，检验代理反演 + 引擎校正的可恢复性。

  python invert.py --case <work>/cases/feb_crop/input --model <work>/surrogate --test <data>/test.jsonl \
      --index 5 --obs temperature --noise 0.1 --out <work>/osse_5_temperature.json

--obs temperature：只用类别温度（叶片 T/Tsun/Tsh、地面 T，所有节点），对应热红外/LSCT 类观测；
--obs flux：只用通量（LE/H/Rn/G），对应涡动/通量塔类观测；--obs all：两者都用。
已知强迫（d_ta、u_scale、rin_scale）取真值，待估参数为 vcmax、ball_berry_m、soil_smc、soil_rss（可用 --free 改）。
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from hsml import params as P
from hsml import surrogate as S
from hsml.engine import FacetEngine, observation_sigma_hint
from hsml.inversion import Problem, laplace, map_estimate, refine_with_engine


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--test", required=True)
    ap.add_argument("--index", type=int, default=0, help="测试集中的行号（作为真值）")
    ap.add_argument("--obs", choices=["temperature", "flux", "all"], default="temperature")
    ap.add_argument("--noise", type=float, default=1.0, help="观测噪声倍率（1 = 温度 0.1 K、通量 1 W/m2）")
    ap.add_argument("--free", default=None, help="逗号分隔；缺省为参数空间里全部 state 参数")
    ap.add_argument("--prior-sd", type=float, default=0.5, help="先验标准差（占参数量程的比例）")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--refine-iters", type=int, default=5)
    ap.add_argument("--radius", type=float, default=0.2, help="引擎校正的初始信赖域半径（归一化坐标）")
    ap.add_argument("--include-unconverged", action="store_true", help="测试集保留未全部收敛的运行（日循环）")
    ap.add_argument("--lean", action="store_true", help="引擎只写能量过程文件（日循环建议开启）")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    model = S.Surrogate.load(args.model)
    meta = json.loads((Path(args.model) / "surrogate.json").read_text(encoding="utf-8"))
    te = S.load_jsonl(args.test, include_unconverged=args.include_unconverged)
    names = model.names
    u_true = te["u"][args.index]
    y_true = te["y"][args.index]

    is_temp = np.array([n.endswith((".T", ".Tsun", ".Tsh")) for n in names])
    sel = np.where({"temperature": is_temp, "flux": ~is_temp, "all": np.ones_like(is_temp)}[args.obs])[0]
    rng = np.random.default_rng(args.seed)
    sigma = np.array([observation_sigma_hint(names[i]) for i in sel]) * args.noise
    y_obs = y_true[sel] + rng.normal(0.0, sigma)
    # 代理误差：缺省用训练集交叉验证残差的完整协方差（train_surrogate.py 生成 cv_residuals.npy；
    # 统计 OSSE 表明它略保守），没有时退回测试集 RMSE（对角，会偏窄）。
    cv_file = Path(args.model) / "cv_residuals.npy"
    if cv_file.exists():
        cov = S.model_error_covariance(np.load(cv_file))
        model_error = {"cov_model": cov[np.ix_(sel, sel)]}
        print(f"代理误差：交叉验证残差协方差（相关矩阵收缩强度 {S.model_error_covariance.last_shrinkage:.2f}）")
    else:
        print("提示：模型目录没有 cv_residuals.npy，代理误差退回测试集 RMSE（对角，后验可能偏窄）")
        model_error = {"sigma_model": np.array([meta["metrics"]["rmse"][names[i]] for i in sel])}

    free = args.free.split(",") if args.free else [p.name for p in P.PARAMS if p.role == "state"]
    prior_u = P.to_unit(P.defaults())
    free_idx = [P.NAMES.index(n) for n in free]
    prob = Problem(model, sel, y_obs, sigma, free, u_fixed=u_true.copy(), prior_u=prior_u[free_idx],
                   prior_sd_u=np.full(len(free), args.prior_sd), **model_error)

    print(f"真值 θ：{ {k: round(v, 3) for k, v in P.from_unit(u_true).items()} }")
    print(f"观测 {len(sel)} 个（{args.obs}），噪声倍率 {args.noise}，待估 {free}")

    # 1) 只用代理的 MAP
    m = map_estimate(prob, prior_u[free_idx])
    u_s = m["u"]
    lap = laplace(prob, u_s)
    print("\n[代理 MAP]")
    rows = []
    for j, n in enumerate(free):
        p = P.BY_NAME[n]
        est, tru = float(p.from_unit(u_s[j])), float(p.from_unit(u_true[P.NAMES.index(n)]))
        z = (u_s[j] - u_true[P.NAMES.index(n)]) / lap["sd_u"][j]
        rows.append({"param": n, "true": tru, "estimate": est, "sd": float(lap["sd_theta"][j]),
                     "shrinkage": float(lap["shrinkage"][j]), "z": float(z)})
        print(f"  {n:13s} 真值 {tru:9.3f}  估计 {est:9.3f} ± {lap['sd_theta'][j]:.3f}  后验/先验sd {lap['shrinkage'][j]:.2f}  z={z:+.2f}")
    print(f"  数据信息矩阵特征值（噪声归一化）：{np.round(lap['info_eigenvalues'], 2).tolist()}")
    if lap["at_bound"]:
        print(f"  贴边参数（线性高斯近似不可靠）：{lap['at_bound']}")

    # 2) 引擎校正迭代
    engine = FacetEngine(Path(args.case), work_dir=Path(args.case).parent / "runs_osse", lean_outputs=args.lean,
                         timeout_s=3600)
    count = {"n": 0}

    def run_engine(u_free):
        count["n"] += 1
        u = u_true.copy()
        u[free_idx] = u_free
        return np.array(engine.run(P.from_unit(u), f"o{count['n']:04d}")["y"])

    print("\n[引擎校正迭代]")
    u_r, hist = refine_with_engine(prob, run_engine, u_s, max_iter=args.refine_iters, radius=args.radius)
    rows_r = []
    for j, n in enumerate(free):
        p = P.BY_NAME[n]
        est, tru = float(p.from_unit(u_r[j])), float(p.from_unit(u_true[P.NAMES.index(n)]))
        rows_r.append({"param": n, "true": tru, "estimate": est})
        print(f"  {n:13s} 真值 {tru:9.3f}  估计 {est:9.3f}")
    print(f"  引擎运行 {count['n']} 次；观测项 χ²/2：{[round(c, 2) for c in hist.chi2]}（观测数 {len(sel)}，期望约 {len(sel) / 2:.0f}）")

    # 3) 输出一致性：引擎在估计参数下的全部输出 vs 真值引擎输出（含未被观测的量）
    ye = hist.y_engine[-1]
    target = np.where(is_temp, 0.1, 0.25)
    err = ye - y_true
    print("\n[输出一致性：估计参数的引擎输出 − 真值引擎输出]")
    print(f"  温度类 最大 |差| {np.abs(err[is_temp]).max():.3f} K（目标 0.1 K，仅 {args.obs} 观测约束）")
    print(f"  通量类 最大 |差| {np.abs(err[~is_temp]).max():.3f} W/m2（目标 0.25）")
    print(f"  未被观测的输出里，达标比例：{(np.abs(err) <= target)[np.setdiff1d(np.arange(len(names)), sel)].mean() if len(sel) < len(names) else float('nan'):.0%}")
    if args.out:
        Path(args.out).write_text(json.dumps({
            "args": vars(args), "true_theta": P.from_unit(u_true), "surrogate_map": rows, "refined": rows_r,
            "engine_runs": count["n"], "chi2_history": hist.chi2, "cost_history": hist.cost,
            "output_error": {n: float(e) for n, e in zip(names, err)},
            "info_eigenvalues": lap["info_eigenvalues"].tolist()}, ensure_ascii=False, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
