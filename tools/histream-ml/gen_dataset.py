"""生成代理训练/测试数据：Sobol 采样参数空间 → 逐点运行 FacetEB → 追加写入 JSONL（可断点续跑）。

示例：
  python gen_dataset.py --case <work>/cases/feb_crop/input --out <work>/data/train.jsonl --n 256 --seed 1
  python gen_dataset.py --case ... --out <work>/data/test.jsonl  --n 64  --seed 99      # 独立测试集
  python gen_dataset.py --case ... --out <work>/data/repeat.jsonl --repeat 6            # 同一点重复，量化引擎自身的重复性

JSONL 每行：{"id", "theta"(物理量), "u"(归一化), "y", "names", "converged", "iterations", "residual_W_m2", "wall_s"}。
"""
from __future__ import annotations

import argparse
import json
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

from hsml import params as P
from hsml.engine import EngineError, FacetEngine


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--case", required=True, help="冻结输入目录（含 project.json、meteo.txt）")
    ap.add_argument("--out", required=True, help="输出 JSONL")
    ap.add_argument("--n", type=int, default=0, help="Sobol 样本数（建议 2 的幂）")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--repeat", type=int, default=0, help="改为在默认参数点重复运行 N 次")
    ap.add_argument("--workers", type=int, default=2, help="并行引擎进程数（共用一块 GPU）")
    ap.add_argument("--engine-dir", default=None)
    ap.add_argument("--coupling-iterations", type=int, default=60)
    ap.add_argument("--space", default="v1", help="参数空间（hsml/params.py 的 SPACES）")
    ap.add_argument("--lean", action="store_true", help="关闭影像与辐射过程输出（只留能量过程文件），日循环建议开启")
    args = ap.parse_args()
    P.set_space(args.space)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    engine = FacetEngine(Path(args.case), Path(args.engine_dir) if args.engine_dir else None,
                         work_dir=out.parent / "runs", coupling_iterations=args.coupling_iterations,
                         lean_outputs=args.lean, timeout_s=3600)
    if args.repeat:
        us = np.tile(P.to_unit(P.defaults()), (args.repeat, 1))
    elif args.n:
        us = P.sobol_unit(args.n, args.seed)
    else:
        ap.error("需要 --n 或 --repeat")

    done = set()
    if out.exists():
        for line in out.read_text(encoding="utf-8").splitlines():
            if line.strip():
                done.add(json.loads(line)["id"])
    lock = threading.Lock()
    stats = {"ok": 0, "failed": 0}

    def work(i: int) -> None:
        if i in done:
            return
        theta = P.from_unit(us[i])
        try:
            r = engine.run(theta, f"{out.stem}_{i:05d}")
        except (EngineError, Exception) as exc:   # noqa: BLE001  记录失败点，继续其余样本
            with lock:
                stats["failed"] += 1
                print(f"[{i}] 失败：{str(exc)[:300]}", flush=True)
            return
        row = {"id": i, "theta": theta, "u": us[i].tolist(), **r}
        with lock:
            with out.open("a", encoding="utf-8") as f:
                f.write(json.dumps(row, ensure_ascii=False) + "\n")
            stats["ok"] += 1
            if stats["ok"] % 10 == 0:
                print(f"已完成 {stats['ok']} / {len(us) - len(done)}，失败 {stats['failed']}，最近一次 {r['wall_s']:.1f} s", flush=True)

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        list(pool.map(work, range(len(us))))
    print(f"完成：成功 {stats['ok']}，失败 {stats['failed']}，已有 {len(done)}，输出 {out}")
    return 1 if stats["failed"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
