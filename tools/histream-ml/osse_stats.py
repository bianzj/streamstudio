"""统计 OSSE：多个真值点 × 多个噪声样本，检验代理反演的 Laplace 后验是否“说真话”（覆盖率）。

  python osse_stats.py --model <work>/surrogate2 --train <data>/train.jsonl --test <data>/test.jsonl \
      --case <work>/cases/feb_crop/input --seeds 3 --refine 24 --out <work>/osse_stats.json

做法：
- 真值取独立测试集（引擎结果），观测 = 真值输出 + 高斯噪声（温度 0.1 K、通量 1 W m-2 × --noise）；
- 先验：缺省参数值、标准差 = 量程 × --prior-sd；强迫取真值（已知）；
- 代理误差口径（似然里的模型误差项），逐一比较：
    none     不计代理误差；
    diag_test 测试集 RMSE（旧做法；测试集同时被用来检验，有泄漏，仅作对照）；
    diag_cv  训练集 5 折交叉验证 RMSE（对角）；
    full_cv  交叉验证残差的完整协方差（输出间相关）；
- 指标：z = (估计 − 真值)/后验 sd（归一化坐标），1σ 覆盖率应≈68%，1.96σ≈95%，E[z²]≈1；
  贴边（估计落在量程端点）的样本单独计数，不进覆盖率（线性高斯近似在边界上无效）；
- --refine N：对前 N 个组合（full_cv）再做引擎校正迭代，校正后在最终点用观测噪声重算 Laplace，比较覆盖率。
"""
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import numpy as np
import torch

from hsml import params as P
from hsml import surrogate as S
from hsml.engine import FacetEngine, observation_sigma_hint
from hsml.inversion import Problem, laplace, map_estimate, refine_with_engine

VARIANTS = ("none", "diag_test", "diag_cv", "full_cv")


def cv_covariance(model_dir: Path, train_file: str, include_unconverged: bool) -> tuple[np.ndarray, np.ndarray]:
    cache = model_dir / "cv_residuals.npy"
    if cache.exists():
        res = np.load(cache)
    else:
        tr = S.load_jsonl(train_file, include_unconverged=include_unconverged)
        t0 = time.perf_counter()
        res = S.cross_validated_residuals(tr["u"], tr["y"], tr["names"], members=5)
        np.save(cache, res)
        print(f"交叉验证残差：{res.shape}，耗时 {time.perf_counter() - t0:.0f} s（已缓存 {cache.name}）", flush=True)
    return res, S.model_error_covariance(res)


def summarize(records: list[dict], free: list[str]) -> dict:
    out = {}
    for j, name in enumerate(free):
        z = np.array([r["z"][j] for r in records if not r["at_bound"][j]])
        err = np.array([r["est_theta"][j] - r["true_theta"][j] for r in records])
        sd = np.array([r["sd_theta"][j] for r in records])
        out[name] = {
            "n": len(records), "n_interior": int(len(z)), "at_bound_fraction": float(1 - len(z) / max(len(records), 1)),
            "coverage_1sigma": float(np.mean(np.abs(z) <= 1.0)) if len(z) else float("nan"),
            "coverage_95": float(np.mean(np.abs(z) <= 1.96)) if len(z) else float("nan"),
            "mean_z2": float(np.mean(z ** 2)) if len(z) else float("nan"),
            "rmse_theta": float(np.sqrt(np.mean(err ** 2))), "median_sd_theta": float(np.median(sd)),
            "shrinkage_median": float(np.median([r["shrinkage"][j] for r in records])),
        }
    zs = np.concatenate([[r["z"][j] for j in range(len(free)) if not r["at_bound"][j]] for r in records])
    # 数据有约束的样本（后验/先验 sd < 0.3）：不可辨识参数的后验≈宽先验，会把覆盖率抬高，单独看这一部分。
    zi = np.concatenate([[r["z"][j] for j in range(len(free)) if not r["at_bound"][j] and r["shrinkage"][j] < 0.3]
                         for r in records])
    out["_pooled"] = {"coverage_1sigma": float(np.mean(np.abs(zs) <= 1)), "coverage_95": float(np.mean(np.abs(zs) <= 1.96)),
                      "mean_z2": float(np.mean(zs ** 2)), "n": int(len(zs))}
    out["_informative"] = {"coverage_1sigma": float(np.mean(np.abs(zi) <= 1)) if len(zi) else float("nan"),
                           "coverage_95": float(np.mean(np.abs(zi) <= 1.96)) if len(zi) else float("nan"),
                           "mean_z2": float(np.mean(zi ** 2)) if len(zi) else float("nan"), "n": int(len(zi))}
    return out


def record(prob: Problem, u_hat, lap, u_true, free_idx, free) -> dict:
    true_u = u_true[free_idx]
    return {
        "z": ((u_hat - true_u) / lap["sd_u"]).tolist(),
        "at_bound": [bool(x < 1e-3 or x > 1 - 1e-3) for x in u_hat],
        "est_theta": [float(P.BY_NAME[n].from_unit(u_hat[j])) for j, n in enumerate(free)],
        "true_theta": [float(P.BY_NAME[n].from_unit(true_u[j])) for j, n in enumerate(free)],
        "sd_theta": lap["sd_theta"].tolist(), "shrinkage": lap["shrinkage"].tolist(),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", required=True)
    ap.add_argument("--train", required=True)
    ap.add_argument("--test", required=True)
    ap.add_argument("--case", default=None, help="引擎校正需要（--refine > 0）")
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--obs", default="temperature,all")
    ap.add_argument("--noise", type=float, default=1.0)
    ap.add_argument("--prior-sd", type=float, default=0.5)
    ap.add_argument("--free", default=None, help="缺省为全部 state 参数")
    ap.add_argument("--refine", type=int, default=0)
    ap.add_argument("--include-unconverged", action="store_true")
    ap.add_argument("--lean", action="store_true")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    torch.set_num_threads(4)

    model_dir = Path(args.model)
    model = S.Surrogate.load(model_dir)
    meta = json.loads((model_dir / "surrogate.json").read_text(encoding="utf-8"))
    names = model.names
    te = S.load_jsonl(args.test, include_unconverged=args.include_unconverged)
    cv_res, cv_cov = cv_covariance(model_dir, args.train, args.include_unconverged)
    cv_rmse = np.sqrt(np.diag(cv_cov))
    test_rmse = np.array([meta["metrics"]["rmse"][n] for n in names])
    print("代理误差：交叉验证 RMSE / 测试集 RMSE（中位）：", f"{np.median(cv_rmse / test_rmse):.2f}")

    free = args.free.split(",") if args.free else [p.name for p in P.PARAMS if p.role == "state"]
    free_idx = [P.NAMES.index(n) for n in free]
    prior_u = P.to_unit(P.defaults())[free_idx]
    is_temp = np.array([n.endswith((".T", ".Tsun", ".Tsh")) for n in names])
    obs_sets = {"temperature": np.where(is_temp)[0], "flux": np.where(~is_temp)[0], "all": np.arange(len(names))}

    results: dict = {"args": vars(args), "free": free, "surrogate": {"cv_rmse": dict(zip(names, cv_rmse.tolist())),
                                                                     "test_rmse": dict(zip(names, test_rmse.tolist()))}}
    combos = []
    t0 = time.perf_counter()
    for obs_name in args.obs.split(","):
        sel = obs_sets[obs_name]
        sigma = np.array([observation_sigma_hint(names[i]) for i in sel]) * args.noise
        per_variant = {v: [] for v in VARIANTS}
        for i in range(len(te["u"])):
            for s in range(args.seeds):
                rng = np.random.default_rng(1000 * i + s)
                y_obs = te["y"][i][sel] + rng.normal(0.0, sigma)
                combos.append((obs_name, i, s, y_obs))
                for v in VARIANTS:
                    kw = {"none": {}, "diag_test": {"sigma_model": test_rmse[sel]},
                          "diag_cv": {"sigma_model": cv_rmse[sel]}, "full_cv": {"cov_model": cv_cov[np.ix_(sel, sel)]}}[v]
                    prob = Problem(model, sel, y_obs, sigma, free, u_fixed=te["u"][i].copy(), prior_u=prior_u,
                                   prior_sd_u=np.full(len(free), args.prior_sd), **kw)
                    u_hat = map_estimate(prob, prior_u)["u"]
                    per_variant[v].append(record(prob, u_hat, laplace(prob, u_hat), te["u"][i], free_idx, free))
        results[obs_name] = {v: summarize(recs, free) for v, recs in per_variant.items()}
        results[obs_name]["_records_full_cv"] = per_variant["full_cv"]
        print(f"[{obs_name}] {len(te['u'])} 真值 × {args.seeds} 噪声，用时 {time.perf_counter() - t0:.0f} s", flush=True)
        for v in VARIANTS:
            pooled = results[obs_name][v]["_pooled"]
            inf = results[obs_name][v]["_informative"]
            per = "  ".join(f"{n}:{results[obs_name][v][n]['coverage_1sigma']:.0%}" for n in free)
            print(f"  {v:9s} 1σ覆盖 {pooled['coverage_1sigma']:.0%}  95%覆盖 {pooled['coverage_95']:.0%}  "
                  f"E[z²] {pooled['mean_z2']:.2f} | 有约束 n={inf['n']} 1σ {inf['coverage_1sigma']:.0%} 95% {inf['coverage_95']:.0%} | {per}", flush=True)

    # 引擎校正后的覆盖率（full_cv 的 MAP 作起点）
    if args.refine:
        engine = FacetEngine(Path(args.case), work_dir=Path(args.out).parent / "runs_osse_stats", lean_outputs=args.lean,
                             timeout_s=3600)
        refined = {}
        obs_names = args.obs.split(",")
        per_obs = max(1, args.refine // len(obs_names))
        picks = [c for o in obs_names for c in [c for c in combos if c[2] == 0 and c[0] == o][:per_obs]]
        for k, (obs_name, i, s, y_obs) in enumerate(picks):
            sel = obs_sets[obs_name]
            sigma = np.array([observation_sigma_hint(names[q]) for q in sel]) * args.noise
            prob = Problem(model, sel, y_obs, sigma, free, u_fixed=te["u"][i].copy(), prior_u=prior_u,
                           prior_sd_u=np.full(len(free), args.prior_sd), cov_model=cv_cov[np.ix_(sel, sel)])
            u_s = map_estimate(prob, prior_u)["u"]
            count = {"n": 0}

            def run_engine(u_free, i=i):
                count["n"] += 1
                u = te["u"][i].copy()
                u[free_idx] = u_free
                return np.array(engine.run(P.from_unit(u), f"r{k:03d}_{count['n']:02d}")["y"])

            try:
                u_r, hist = refine_with_engine(prob, run_engine, u_s, max_iter=4, verbose=False)
            except RuntimeError as exc:
                print(f"  校正 {k} 失败：{str(exc)[:120]}")
                continue
            with torch.no_grad():
                ys = prob.predict_full(torch.as_tensor(u_r, dtype=torch.float64)).numpy()
            offset = hist.y_engine[-1] - ys
            lap_obs = laplace(prob, u_r, offset, obs_only=True)
            lap_full = laplace(prob, u_r, offset, obs_only=False)
            r_obs = record(prob, u_r, lap_obs, te["u"][i], free_idx, free)
            r_full = record(prob, u_r, lap_full, te["u"][i], free_idx, free)
            r_obs["engine_runs"] = r_full["engine_runs"] = count["n"]
            refined.setdefault(obs_name, {"obs_only": [], "with_model_error": []})
            refined[obs_name]["obs_only"].append(r_obs)
            refined[obs_name]["with_model_error"].append(r_full)
            print(f"  校正 {k + 1}/{len(picks)}（{obs_name}，真值 {i}）：引擎 {count['n']} 次", flush=True)
        results["refined"] = {o: {kind: summarize(recs, free) for kind, recs in d.items()} for o, d in refined.items()}
        for o, d in results["refined"].items():
            for kind, summ in d.items():
                pooled = summ["_pooled"]
                per = "  ".join(f"{n}:{summ[n]['coverage_1sigma']:.0%}" for n in free)
                print(f"[校正后 {o} / {kind}] 1σ覆盖 {pooled['coverage_1sigma']:.0%}  95% {pooled['coverage_95']:.0%}  "
                      f"E[z²] {pooled['mean_z2']:.2f} | {per}")
    Path(args.out).write_text(json.dumps(results, ensure_ascii=False, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
