"""梯度核对：代理的自动微分雅可比 vs 引擎的中心有限差分雅可比。

这是“代理可不可以当梯度来源”的直接检验。引擎对同一输入逐位重复（gen_dataset --repeat 已实测），
差分误差只来自离散化与收敛判据，因此引擎差分可视作参考。

  python check_gradients.py --case <work>/cases/feb_crop/input --model <work>/surrogate --test <data>/test.jsonl --points 3

输出：逐参数列的相对误差（‖J_s − J_e‖ / ‖J_e‖）、余弦相似度，以及步长减半的差分自洽检查。
雅可比单位：输出单位 / 参数全量程（归一化 u 的 1 个单位）。
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from hsml import params as P
from hsml import surrogate as S
from hsml.engine import FacetEngine
from hsml.inversion import finite_difference_jacobian, surrogate_jacobian


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--test", required=True)
    ap.add_argument("--points", type=int, default=3)
    ap.add_argument("--h", type=float, default=0.03)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    model = S.Surrogate.load(args.model)
    te = S.load_jsonl(args.test)
    engine = FacetEngine(Path(args.case), work_dir=Path(args.case).parent / "runs_grad")
    d = len(P.NAMES)
    counter = {"n": 0}

    def run(u):
        counter["n"] += 1
        return np.array(engine.run(P.from_unit(u), f"g{counter['n']:05d}")["y"])

    # 取测试集中最靠近量程中心的点（避免贴边的单边差分），再取其余按间隔挑选
    center = np.abs(te["u"] - 0.5).max(1)
    order = list(np.argsort(center))
    picks = order[:1] + order[len(order) // 4:len(order) // 4 + args.points - 1]
    report = []
    for pi in picks:
        u = te["u"][pi]
        Je = finite_difference_jacobian(run, u, list(range(d)), h=args.h)
        Js = surrogate_jacobian(model, u, list(range(d)))
        cols = []
        for j, name in enumerate(P.NAMES):
            ne, ns = np.linalg.norm(Je[:, j]), np.linalg.norm(Js[:, j])
            cos = float(Je[:, j] @ Js[:, j] / (ne * ns + 1e-30))
            rel = float(np.linalg.norm(Js[:, j] - Je[:, j]) / (ne + 1e-30))
            cols.append({"param": name, "engine_norm": float(ne), "surrogate_norm": float(ns), "cosine": cos, "rel_error": rel})
        # 差分自洽：步长减半后列向量变化
        Je2 = finite_difference_jacobian(run, u, [0, 3], h=args.h / 2)
        fd_consistency = [float(np.linalg.norm(Je2[:, k] - Je[:, j]) / (np.linalg.norm(Je[:, j]) + 1e-30)) for k, j in enumerate([0, 3])]
        report.append({"test_index": int(pi), "u": u.tolist(), "columns": cols, "fd_step_halving_rel_change(vcmax,soil_rss)": fd_consistency,
                       "abs_error_max_per_output": {n: float(np.abs(Js[i] - Je[i]).max()) for i, n in enumerate(model.names)}})
        print(f"\n测试点 {pi}，u={np.round(u, 2).tolist()}，引擎运行累计 {counter['n']} 次")
        print(f"{'参数':14s} {'‖J_engine‖':>11s} {'‖J_surr‖':>10s} {'cosine':>7s} {'相对误差':>8s}")
        for c in cols:
            print(f"{c['param']:14s} {c['engine_norm']:11.3f} {c['surrogate_norm']:10.3f} {c['cosine']:7.3f} {c['rel_error']:8.1%}")
        print("差分步长减半的列变化（vcmax, soil_rss）：", [f"{x:.2%}" for x in fd_consistency])
    if args.out:
        Path(args.out).write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
