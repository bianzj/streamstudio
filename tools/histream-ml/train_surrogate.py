"""训练神经代理并在独立测试集上评估，导出 ONNX。

  python train_surrogate.py --train <data>/train.jsonl --test <data>/test.jsonl --out <work>/surrogate

测试集不参与任何超参数选择（超参数固定在代码默认值里）；学习曲线（训练样本数 → 测试误差）用 --curve 输出。
"""
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import numpy as np

from hsml import surrogate as S


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--train", required=True, nargs="+")
    ap.add_argument("--test", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--members", type=int, default=5)
    ap.add_argument("--epochs", type=int, default=6000)
    ap.add_argument("--curve", action="store_true", help="另外训练 1/4、1/2 样本量，给出学习曲线")
    ap.add_argument("--no-cv", action="store_true", help="不算交叉验证残差（反演的代理误差协方差需要它）")
    ap.add_argument("--include-unconverged", action="store_true",
                    help="保留未全部收敛的运行（日循环：个别面元在阈值附近振荡，类别均值已稳定）")
    args = ap.parse_args()

    inc = args.include_unconverged
    tr, te = S.load_jsonl(args.train[0], include_unconverged=inc), S.load_jsonl(args.test, include_unconverged=inc)
    for extra in args.train[1:]:                     # 多个训练文件（不同 Sobol 种子）合并
        more = S.load_jsonl(extra, include_unconverged=inc)
        assert more["names"] == tr["names"]
        tr["u"] = np.concatenate([tr["u"], more["u"]])
        tr["y"] = np.concatenate([tr["y"], more["y"]])
    assert tr["names"] == te["names"]
    print(f"训练 {len(tr['u'])} 点，测试 {len(te['u'])} 点，输出 {len(tr['names'])} 维")

    curve = {}
    if args.curve:
        for frac in (0.25, 0.5):
            m = int(len(tr["u"]) * frac)
            sub = S.train(tr["u"][:m], tr["y"][:m], tr["names"], members=3, epochs=args.epochs, verbose=False)
            s = S.evaluate(sub, te["u"], te["y"])["summary"]
            curve[str(m)] = s
            print(f"  学习曲线 n={m}: {s}")

    t0 = time.perf_counter()
    model = S.train(tr["u"], tr["y"], tr["names"], members=args.members, epochs=args.epochs)
    print(f"训练耗时 {time.perf_counter() - t0:.0f} s")
    metrics = S.evaluate(model, te["u"], te["y"])
    metrics["learning_curve"] = curve
    out = Path(args.out)
    model.save(out, metrics, extra={"train_file": [str(f) for f in args.train], "test_file": str(args.test), "n_train": len(tr["u"])})
    diff = S.export_onnx(model, out / "surrogate.onnx", te["u"][:16])
    metrics["onnx_max_abs_diff"] = diff
    if not args.no_cv:
        # 代理误差协方差的来源（invert.py / osse_stats.py 读取）：只用训练集，5 折、每折 5 成员。
        t1 = time.perf_counter()
        residuals = S.cross_validated_residuals(tr["u"], tr["y"], tr["names"], members=args.members, epochs=args.epochs)
        np.save(out / "cv_residuals.npy", residuals)
        cv_rmse = np.sqrt((residuals ** 2).mean(0))
        metrics["cv_rmse"] = dict(zip(tr["names"], cv_rmse.tolist()))
        test_rmse = np.array([metrics["rmse"][n] for n in tr["names"]])
        metrics["cv_over_test_rmse_median"] = float(np.median(cv_rmse / np.maximum(test_rmse, 1e-12)))
        print(f"交叉验证残差已保存（{time.perf_counter() - t1:.0f} s）；交叉验证 RMSE / 测试集 RMSE 中位 "
              f"{metrics['cv_over_test_rmse_median']:.2f}")
    spread = np.array([metrics["ensemble_spread_mean"][n] for n in tr["names"]])
    rmse = np.array([metrics["rmse"][n] for n in tr["names"]])
    metrics["rmse_over_ensemble_spread_median"] = float(np.median(rmse / np.maximum(spread, 1e-12)))
    (out / "metrics.json").write_text(json.dumps(metrics, ensure_ascii=False, indent=1), encoding="utf-8")
    model.save(out, metrics, extra={"train_file": [str(f) for f in args.train], "test_file": str(args.test), "n_train": len(tr["u"])})

    print(f"ONNX 与 PyTorch 最大绝对差：{diff:.2e}")
    print(f"测试集汇总：{metrics['summary']}")
    print(f"集成离散度偏小：测试集 RMSE / 成员离散度 中位 {metrics['rmse_over_ensemble_spread_median']:.2f}"
          "（离散度不能直接当代理误差）")
    if len(tr["names"]) > 48:                        # 日循环：按量汇总，不逐个输出
        groups = {}
        for n in tr["names"]:
            groups.setdefault(n.split(".", 1)[1], []).append(n)
        print(f"{'量':12s} {'RMSE 中位':>9s} {'RMSE 最大':>9s} {'达标比例':>8s}")
        for g, members in groups.items():
            r = np.array([metrics["rmse"][n] for n in members])
            f = np.mean([metrics["within_target_fraction"][n] for n in members])
            print(f"{g:12s} {np.median(r):9.4f} {r.max():9.4f} {f:8.0%}")
        return 0
    print(f"{'输出':14s} {'RMSE':>9s} {'最大误差':>9s} {'集成离散':>9s} {'目标':>6s} {'达标比例':>8s}")
    for n in tr["names"]:
        print(f"{n:14s} {metrics['rmse'][n]:9.4f} {metrics['max_abs'][n]:9.4f} {metrics['ensemble_spread_mean'][n]:9.4f} "
              f"{metrics['target'][n]:6.2f} {metrics['within_target_fraction'][n]:8.0%}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
