"""只用引擎（不用代理）的 OSSE 反演：雅可比来自引擎内切线敏感度（control.facetSensitivity），
不支持的参数（土壤导热率等）用引擎中心差分补列。与 osse_stats.py 用相同的真值、噪声种子与先验，可逐条对比。

  python osse_engine.py --case <work>/cases/feb_crop/input --test <data>/test.jsonl --truths 10 \
      --compare <work>/osse_stats2.json --out <work>/osse_engine.json [--engine-dir <dir>]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from hsml import params as P
from hsml import surrogate as S
from hsml.engine import ENGINE_SENSITIVITY, FacetEngine, observation_sigma_hint
from hsml.inversion import gauss_newton_engine


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", required=True)
    ap.add_argument("--test", required=True)
    ap.add_argument("--truths", type=int, default=10, help="每种观测取测试集前 N 个真值（噪声种子 0）")
    ap.add_argument("--obs", default="temperature,all")
    ap.add_argument("--noise", type=float, default=1.0)
    ap.add_argument("--prior-sd", type=float, default=0.5)
    ap.add_argument("--fd-step", type=float, default=0.03, help="差分列的步长（归一化坐标）")
    ap.add_argument("--engine-dir", default=None)
    ap.add_argument("--compare", default=None, help="osse_stats.py 的输出（含 --refine 结果），逐条对比")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    te = S.load_jsonl(args.test)
    free = [p.name for p in P.PARAMS if p.role == "state"]
    free_idx = [P.NAMES.index(n) for n in free]
    native = [n for n in free if n in ENGINE_SENSITIVITY]
    fd_cols = [n for n in free if n not in ENGINE_SENSITIVITY]
    print(f"待估 {free}；引擎内导数 {native}；引擎差分列 {fd_cols}")
    engine = FacetEngine(Path(args.case), Path(args.engine_dir) if args.engine_dir else None,
                         work_dir=Path(args.out).parent / "runs_osse_engine")
    prior_u = P.to_unit(P.defaults())[free_idx]
    names = None
    compare = json.loads(Path(args.compare).read_text(encoding="utf-8")) if args.compare else None
    out = {"args": vars(args), "free": free, "native": native, "fd": fd_cols}
    for obs_name in args.obs.split(","):
        rows = []
        for i in range(args.truths):
            counter = {"n": 0}

            def evaluate(u_free, need_jacobian, i=i):
                u = te["u"][i].copy()
                u[free_idx] = u_free
                counter["n"] += 1
                if not need_jacobian:
                    return np.array(engine.run(P.from_unit(u), f"e{counter['n']:03d}")["y"]), None, 1
                r = engine.run(P.from_unit(u), f"e{counter['n']:03d}", sensitivity=native)
                J = np.zeros((len(r["y"]), len(free)))
                for k, n in enumerate(native):
                    J[:, free.index(n)] = r["jacobian"][:, k]
                runs = 1
                for n in fd_cols:                       # 中心差分补列
                    j = P.NAMES.index(n)
                    up, um = u.copy(), u.copy()
                    up[j] = min(u[j] + args.fd_step, 1.0)
                    um[j] = max(u[j] - args.fd_step, 0.0)
                    yp = np.array(engine.run(P.from_unit(up), "fd_p")["y"])
                    ym = np.array(engine.run(P.from_unit(um), "fd_m")["y"])
                    J[:, free.index(n)] = (yp - ym) / (up[j] - um[j])
                    runs += 2
                return np.array(r["y"]), J, runs

            nonlocal_names = te["names"]
            is_temp = np.array([n.endswith((".T", ".Tsun", ".Tsh")) for n in nonlocal_names])
            sel = {"temperature": np.where(is_temp)[0], "flux": np.where(~is_temp)[0],
                   "all": np.arange(len(nonlocal_names))}[obs_name]
            sigma = np.array([observation_sigma_hint(nonlocal_names[q]) for q in sel]) * args.noise
            rng = np.random.default_rng(1000 * i + 0)               # 与 osse_stats.py 相同的噪声
            y_obs = te["y"][i][sel] + rng.normal(0.0, sigma)
            print(f"[{obs_name}] 真值 {i}", flush=True)
            res = gauss_newton_engine(evaluate, sel, y_obs, sigma, prior_u, np.full(len(free), args.prior_sd),
                                      prior_u.copy(), verbose=False)
            true_u = te["u"][i][free_idx]
            z = (res.u - true_u) / res.sd_u
            rows.append({
                "truth": i, "est_theta": [float(P.BY_NAME[n].from_unit(res.u[j])) for j, n in enumerate(free)],
                "true_theta": [float(P.BY_NAME[n].from_unit(true_u[j])) for j, n in enumerate(free)],
                "z": z.tolist(), "shrinkage": res.shrinkage.tolist(),
                "at_bound": [bool(x < 1e-3 or x > 1 - 1e-3) for x in res.u],
                "engine_runs": res.engine_runs, "wall_s": res.wall_s, "iterations": len(res.history) - 1,
                "chi2": res.history[-1]["chi2"], "n_obs": int(len(sel))})
            print(f"   GN {len(res.history) - 1} 步，引擎 {res.engine_runs} 次，{res.wall_s:.0f} s；"
                  + "  ".join(f"{n} {rows[-1]['est_theta'][j]:.3g}/{rows[-1]['true_theta'][j]:.3g}" for j, n in enumerate(free)),
                  flush=True)
        zs = np.array([r["z"] for r in rows])
        bound = np.array([r["at_bound"] for r in rows])
        cov1 = {n: float(np.mean(np.abs(zs[~bound[:, j], j]) <= 1)) for j, n in enumerate(free)}
        pooled = float(np.mean(np.abs(zs[~bound]) <= 1))
        rmse = {n: float(np.sqrt(np.mean([(r["est_theta"][j] - r["true_theta"][j]) ** 2 for r in rows]))) for j, n in enumerate(free)}
        summary = {"coverage_1sigma": cov1, "coverage_1sigma_pooled": pooled, "rmse_theta": rmse,
                   "engine_runs_mean": float(np.mean([r["engine_runs"] for r in rows])),
                   "wall_s_mean": float(np.mean([r["wall_s"] for r in rows]))}
        print(f"[{obs_name}] 引擎 GN：1σ 覆盖 {pooled:.0%} {cov1}；RMSE {rmse}；平均引擎 {summary['engine_runs_mean']:.1f} 次，"
              f"{summary['wall_s_mean']:.0f} s")
        if compare and "refined" in compare and obs_name in compare.get("refined", {}):
            ref = compare[obs_name]["_records_full_cv"]           # 代理 MAP（full_cv），按 (真值, 种子) 顺序
            seeds = compare["args"]["seeds"]
            sur = [ref[i * seeds] for i in range(args.truths)]
            rmse_s = {n: float(np.sqrt(np.mean([(r["est_theta"][j] - r["true_theta"][j]) ** 2 for r in sur]))) for j, n in enumerate(free)}
            summary["surrogate_map_rmse_theta_same_cases"] = rmse_s
            print(f"   同样个例的代理 MAP RMSE：{rmse_s}")
        out[obs_name] = {"summary": summary, "rows": rows}
    Path(args.out).write_text(json.dumps(out, ensure_ascii=False, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
