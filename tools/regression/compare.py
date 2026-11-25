# ==========================================================================
# 文件：compare.py
# 功能：回归测试第 3 步——对比两个变体（如 base 与 new 引擎）在每个共同算例上的运行结果与耗时，
#       并可用 --tolerance 作为“纯性能改动不得改变结果”的门禁（超限时以状态 1 退出）。
# 主要模块：
#   1. stats / Variant：数组统计量（均值、中位数、p99、最大值、非有限值个数）与变体目录访问；
#   2. tiffs：各 TIFF 每个波段的统计（反射率波段另数大于 1 的像元数）；
#   3. voxeleb：VoxelEB 按体元类型（土壤/植被/建筑/水体）平均的 T、H、LE、G、SW、Rn 与迭代数；
#   4. facet_convergence：从 stdout.txt 的 FACET_CONVERGENCE 行取 FacetEB 每节点收敛信息；
#   5. facetrt：FacetRT 各波段 bandRadiosity 统计与图边数；
#   6. summary / relative_change / main：按模式汇总、计算相对变化并打印、写 --json、判定门禁。
# 数据流：<workDir>/runs/<变体>/<算例>/{result.json, stdout.txt, out/*} → 屏幕报告（可选 JSON）。
# 与其他文件的关系：用 rasterio_lite.read_tiff 读 TIFF；result.json 由 run.mjs 写；
#       算例名与模式来自 cases.mjs（经 result.json 的 mode 字段）。
# 约定：容差是“被比较的均值”的最大相对变化 |b-a|/max(|a|,1e-12)；仅 TIFF 均值、VoxelEB 的 T 与 Rn、
#       FacetRT 波段均值参与门禁，FacetEB 收敛信息、迭代数和耗时只打印、不判定。
# 用法：python -I tools/regression/compare.py <workDir> <A> <B> [--json out.json] [--tolerance 1e-4]
# ==========================================================================

"""Compare two regression variants over every prepared case.

usage: python -I tools/regression/compare.py <workDir> <variantA> <variantB> [--json out.json] [--tolerance 1e-3]

Prints, per case: exit codes and run time, per-band means/p99 of every TIFF,
VoxelEB per-type energy terms and iteration counts, FacetEB convergence lines,
FacetRT per-band statistics. With --tolerance the script exits with status 1
when any compared mean changes by more than that relative amount (use it for
changes that must not alter results, e.g. pure performance work).
"""
import argparse, glob, json, os, re, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rasterio_lite import read_tiff  # noqa: E402

# 体元类型编号（voxel_scene_links.bin 第 5 列）对应的名称。
TYPE_NAMES = {0: 'null', 1: 'soil', 2: 'veg', 3: 'bldg', 4: 'water'}


# 作用：有限值的 mean/p50/p99/max 及非有限值个数；全部非有限时只返回个数。
def stats(a):
    a = np.asarray(a, np.float64)
    finite = np.isfinite(a)
    if not finite.any():
        return {'n': int(a.size), 'nonfinite': int((~finite).sum())}
    v = a[finite]
    return {'mean': float(v.mean()), 'p50': float(np.median(v)), 'p99': float(np.percentile(v, 99)),
            'max': float(v.max()), 'nonfinite': int((~finite).sum())}


# 一个变体的运行根目录 <workDir>/runs/<name>；result(case) 读 run.mjs 写的 result.json。
class Variant:
    def __init__(self, work, name):
        self.root = os.path.join(work, 'runs', name)

    def path(self, case, *parts):
        return os.path.join(self.root, case, *parts)

    def result(self, case):
        p = self.path(case, 'result.json')
        return json.load(open(p)) if os.path.isfile(p) else None


# 作用：统计算例 out/ 下每个 TIFF 的每个波段；键为“文件名前 40 字符|波段名”。
# 波段名缺失时用 b0、b1…；名称含 Reflectance 的波段额外统计 > 1 的像元数（反射率不应超过 1）。
def tiffs(v, case):
    out = {}
    for p in sorted(glob.glob(v.path(case, 'out', '*.tif'))):
        bands, names, _ = read_tiff(p)
        for i, n in enumerate(names or [f'b{i}' for i in range(bands.shape[0])]):
            e = stats(bands[i])
            if 'Reflectance' in n:
                e['gt1'] = int((bands[i] > 1).sum())
            out[os.path.basename(p)[:40] + '|' + n] = e
    return out


# 作用：读 VoxelEB 的过程输出，按体元类型对每个节点取平均。
# 输入：process/voxel_scene_links.bin（int32 x 6，第 5 列为类型）、voxeleb_T=*.bin/json（能量项）、
#       voxelrt_T=*.bin（辐射项，8 列）、diagnostics/energy_balance_node=*.json（迭代数）。
# 输出：{节点标记: {类型: {T,H,LE,G,SW,Rn,G/Rn}, _iter, _outside}}。
def voxeleb(v, case):
    base = v.path(case, 'out')
    links_path = os.path.join(base, 'process', 'voxel_scene_links.bin')
    if not os.path.isfile(links_path):
        return {}
    typ = np.fromfile(links_path, dtype='<i4').reshape(-1, 6)[:, 4]
    res = {}
    for meta in sorted(glob.glob(os.path.join(base, 'process', 'voxeleb_T=*.json'))):
        tok = re.search(r'T=(.*)\.json', meta).group(1)
        # （下面按元数据取记录宽度 recordFloats（缺省 18）和温度列偏移；缺省温度列取最后一列。）
        # Record width depends on the soil method (method 2 appends the
        # layer temperatures); read it and the temperature offset from metadata.
        info = json.load(open(meta, encoding='utf-8'))
        width = int(info.get('recordFloats', 18))
        tcol = next((f['offset'] for f in info.get('fields', []) if f.get('id') == 'temperature'), width - 1)
        eb = np.fromfile(os.path.join(base, 'process', f'voxeleb_T={tok}.bin'), dtype='<f4').reshape(-1, width)
        rt = np.fromfile(os.path.join(base, 'process', f'voxelrt_T={tok}.bin'), dtype='<f4').reshape(-1, 8)
        node = {}
        for tid in sorted(set(typ.tolist())):
            m = typ == tid
            row = {k: float(np.nanmean(a[m, col])) for k, a, col in
                   (('T', eb, tcol), ('H', eb, 4), ('LE', eb, 3), ('G', eb, 5), ('SW', rt, 3), ('Rn', rt, 5))}
            row['G/Rn'] = row['G'] / row['Rn'] if abs(row['Rn']) > 1 else None
            node[TYPE_NAMES.get(tid, str(tid))] = row
        diag = glob.glob(os.path.join(base, 'diagnostics', f'energy_balance_node=*_T={tok}.json'))
        if diag:
            d = json.load(open(diag[0], encoding='utf-8'))
            node['_iter'] = d.get('temperatureIterations')
            node['_outside'] = d.get('budgetStatesOutsideTolerance')
        res[tok] = node
    return res


# 作用：解析 stdout.txt 中以 FACET_CONVERGENCE 开头的行，字段依次为节点、是否收敛、迭代数、
# 最大温度变化 dT、最大残差 maxRes、等效温差 maxEqK（列含义按引擎输出，待核对）。
def facet_convergence(v, case):
    p = v.path(case, 'stdout.txt')
    rows = []
    if os.path.isfile(p):
        for line in open(p, encoding='utf-8', errors='replace'):
            if line.startswith('FACET_CONVERGENCE'):
                f = line.split()
                rows.append({'node': int(f[1]), 'conv': int(f[2]), 'iter': int(f[3]), 'dT': float(f[4]),
                             'maxRes': float(f[5]), 'maxEqK': float(f[6])})
    return rows


# 作用：读 FacetRT 的 facetrt.json：有向边数与各波段 bandRadiosity 的统计
# （数组按 2 x facetCount 个面元记录重排，每个波段一行）。
def facetrt(v, case):
    p = v.path(case, 'out', 'facetrt.json')
    if not os.path.isfile(p):
        return {}
    d = json.load(open(p, encoding='utf-8'))
    out = {'graphEdges': d.get('graph', {}).get('directedEdges')}
    if 'bandRadiosity' in d:
        n = 2 * d['facetCount']
        br = np.asarray(d['bandRadiosity'], np.float64).reshape(-1, n)
        for i, w in enumerate(d['wavelengths']):
            out[f'band{w}'] = stats(br[i]) | {'gt1': int((br[i] > 1).sum())}
    return out


# 作用：按计算模式选择需要汇总的内容（TIFF / VoxelEB / FacetEB / FacetRT）；无 out 目录则只含 run。
def summary(v, case, mode):
    s = {'run': v.result(case)}
    if not os.path.isdir(v.path(case, 'out')):
        return s
    if mode in ('eRaytracing', 'eVoxelRT', 'eVoxelEB'):
        s['tif'] = tiffs(v, case)
    if mode == 'eVoxelEB':
        s['voxeleb'] = voxeleb(v, case)
    if mode == 'eFacetEB':
        s['conv'] = facet_convergence(v, case)
    if mode == 'eFacetRT':
        s['facetrt'] = facetrt(v, case)
    return s


# 作用：相对变化 |b-a| / max(|a|, 1e-12)；任一方缺失按 0 处理（不触发门禁）。
def relative_change(a, b):
    if a is None or b is None:
        return 0.0
    return abs(b - a) / max(abs(a), 1e-12)


# 作用：对两个变体共有的算例逐一打印对比；worst 记录门禁指标的最大相对变化。
# 退出状态：指定 --tolerance 且 worst 超限时为 1，否则 0。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('work'); parser.add_argument('a'); parser.add_argument('b')
    parser.add_argument('--json'); parser.add_argument('--tolerance', type=float)
    args = parser.parse_args()
    A, B = Variant(args.work, args.a), Variant(args.work, args.b)
    # 只比较两个变体都有运行目录的算例。
    cases = sorted(c for c in set(os.listdir(A.root)) & set(os.listdir(B.root)) if os.path.isdir(os.path.join(A.root, c)))
    worst = 0.0
    report = {}
    # 模式从 result.json 的 mode 取（A 优先，缺失用 B）。
    for case in cases:
        mode = (A.result(case) or {}).get('mode') or (B.result(case) or {}).get('mode')
        a, b = summary(A, case, mode), summary(B, case, mode)
        report[case] = {args.a: a, args.b: b}
        ra, rb = a['run'] or {}, b['run'] or {}
        print(f"\n## {case} [{mode}]: exit {ra.get('exitCode')} -> {rb.get('exitCode')}, time {ra.get('elapsedSec')} s -> {rb.get('elapsedSec')} s")
        for key, x in a.get('tif', {}).items():
            y = b.get('tif', {}).get(key, {})
            worst = max(worst, relative_change(x.get('mean'), y.get('mean')))
            print(f"  {key:78s} mean {x.get('mean', float('nan')):.5f} -> {y.get('mean', float('nan')):.5f}  p99 {x.get('p99', float('nan')):.5f} -> {y.get('p99', float('nan')):.5f}  gt1 {x.get('gt1', '')} -> {y.get('gt1', '')}")
        for tok, x in a.get('voxeleb', {}).items():
            y = b.get('voxeleb', {}).get(tok, {})
            print(f"  node {tok}: iterations {x.get('_iter')} -> {y.get('_iter')}, states outside {x.get('_outside')} -> {y.get('_outside')}")
            for t, row in x.items():
                if t.startswith('_'):
                    continue
                other = y.get(t, {})
                for k in ('T', 'Rn'):
                    worst = max(worst, relative_change(row.get(k), other.get(k)))
                print(f"    {t:5s} " + ' '.join(f"{k} {row[k]:.3f}->{other.get(k, float('nan')):.3f}" if row[k] is not None and other.get(k) is not None else f"{k} -" for k in ('T', 'H', 'LE', 'G', 'Rn', 'G/Rn')))
        for x, y in zip(a.get('conv', []), b.get('conv', [])):
            print(f"  node {x['node']}: converged {x['conv']}->{y['conv']} iterations {x['iter']}->{y['iter']} maxRes {x['maxRes']:.3f}->{y['maxRes']:.3f} maxEqK {x['maxEqK']:.3f}->{y['maxEqK']:.3f}")
        fa, fb = a.get('facetrt', {}), b.get('facetrt', {})
        if fa:
            print(f"  graph edges {fa.get('graphEdges')} -> {fb.get('graphEdges')}")
        for key, x in fa.items():
            if key.startswith('band'):
                y = fb.get(key, {})
                worst = max(worst, relative_change(x.get('mean'), y.get('mean')))
                print(f"  {key}: mean {x['mean']:.5f} -> {y.get('mean', float('nan')):.5f} p99 {x['p99']:.5f} -> {y.get('p99', float('nan')):.5f} gt1 {x['gt1']} -> {y.get('gt1')}")
    if args.json:
        json.dump(report, open(args.json, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
    print(f"\nlargest relative change of a compared mean: {worst:.3e}")
    if args.tolerance is not None and worst > args.tolerance:
        print(f"FAIL: exceeds tolerance {args.tolerance:g}")
        sys.exit(1)


if __name__ == '__main__':
    main()
