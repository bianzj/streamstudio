# ==========================================================================
# 文件：compare-facet-outer-stomata.py
# 功能：对比同一个 144 株作物场景的多次完整 FacetEB 运行（不同外层气孔/收敛判据变体），
#       核对收敛标记并汇总正午中心 9 株的温度、能量残差和 PAR 差异；从不采信未收敛结果。
# 用法：python tools/compare-facet-outer-stomata.py <根目录> <变体名> [<变体名> ...]
#   每个 <根目录>/<变体>/run.json 须含 outputDir（该次运行的输出目录）；第一个变体为基线。
# 主要步骤（对每个变体）：
#   1. 读取 run-manifest.json、faceteb_run.json、faceteb.json；要求运行已结束、
#      叶面元数 = 144 × 958、几何与基线逐位相同
#   2. 汇总用时、已完成节点数、收敛节点数、总迭代数
#   3. 对每个节点按其历史上声明的外层收敛判据重新计算“应有”的收敛标记，与记录值比对
#   4. 读取 process/faceteb_T=*.bin（float32，形状 [面元, 2 面, 7 字段]）并要求全为有限值
#   5. 正午节点（token = DOY214_12-20）：统计中心 9 株叶温（℃）的最小/最大/面积加权均值、
#      指定下部面元的通量；与基线对比 PAR 最大绝对差与叶温差
# 输出：<根目录>/comparison.json，并在标准输出打印不含逐节点明细的摘要。
# 约定：温度 K 转 ℃ 减 273.15；通量 W m-2；叶面元按 2 面记录，H/LE 为整叶值在两面重复，不可求和。
# 注意：脚本含硬编码的场景常量（958 面元/株、中心 9 株编号、面元 62764、正午 token）。
# ==========================================================================

"""Compare actual full-scene FacetEB runs; never promote unconverged fields."""
import argparse
import json
from datetime import datetime
from pathlib import Path

import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument('root', type=Path)
parser.add_argument('variants', nargs='+')
args = parser.parse_args()

def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))

report = {}
baseline_data = baseline_par = baseline_vertices = None
# 中心 9 株（编号 52-54、64-66、76-78，推测为 12×12 株阵列的中央 3×3，待核对）
# 的全部叶面元编号；每株 958 个叶面元。
ids = np.concatenate([np.arange(i * 958, (i + 1) * 958)
                      for i in [52, 53, 54, 64, 65, 66, 76, 77, 78]])
# 逐变体处理（步骤 1-5）。
for variant in args.variants:
    folder = Path(read(args.root / variant / 'run.json')['outputDir'])
    manifest, index = read(folder / 'run-manifest.json'), read(folder / 'faceteb_run.json')
    assert manifest['status'] != 'running', f'{variant} still running'
    geometry = read(folder / 'faceteb.json')
    vertices = np.asarray(geometry['vertexPositions']).reshape(-1, 3, 3)
    # 面元面积 = |(v1-v0)×(v2-v0)|/2，用作温度面积加权均值的权重。
    area = np.linalg.norm(np.cross(vertices[:, 1] - vertices[:, 0],
                                   vertices[:, 2] - vertices[:, 0]), axis=1) / 2
    assert geometry['leafFacetCount'] == 144 * 958, 'Not the original 144-plant scene'
    if baseline_vertices is None:
        baseline_vertices = vertices
    else:
        assert np.array_equal(vertices, baseline_vertices), 'Scene geometry changed'
    # 解析 manifest 中的 ISO 时间（'Z' 结尾需换成 +00:00），用于计算运行耗时。
    t = lambda key: datetime.fromisoformat(manifest[key].replace('Z', '+00:00'))
    result = dict(run=str(folder), status=manifest['status'],
                  elapsed_seconds=(t('finishedAt') - t('startedAt')).total_seconds(),
                  completed=len(index['completed']),
                  converged=sum(n['converged'] for n in index['completed']),
                  total_iterations=sum(n['iterations'] for n in index['completed']), nodes=[])
    for node in index['completed']:
        # 步骤 3-4：逐节点核对收敛标记与数据有效性。
        diag = read(folder / f"diagnostics/facet_physics_node={node['node']}.json")
        # 新判据：未收敛能量面元比例须小于 1/1000（failed×1000 < total）；
        # 旧判据：温度变化、能量残差（温度当量）、稳定度变化、气孔对数失配（可由运行声明为仅诊断）、
        # 叶片能量残差（≤ 1 W m-2）全部满足。两种判据的阈值取自本脚本字面值。
        # Check each historical run against its own declared outer criterion.
        if 'maximumFailingFacetFractionExclusive' in index['convergence']:
            failed, total = node['energyFailingFacets'], node['energyTotalFacets']
            assert 0 <= failed <= total == len(vertices)
            assert failed == diag['energyFailingFacets']
            assert node['energyFailingFacetFraction'] == failed / total
            expected = total > 0 and failed * 1000 < total
        else:
            expected = (node['temperatureDelta'] <= node['temperatureTolerance'] and
                        node['energyResidualTemperatureEquivalent'] <= node['temperatureTolerance'] and
                        diag['maximumStabilityChange'] <= .001 and
                        (index['convergence'].get('stomatalMismatchDiagnosticOnly', False) or
                         diag['maximumStomatalLogMismatch'] <= .001) and
                        diag['maximumLeafEnergyResidual_W_m2'] <= 1)
        assert node['converged'] == expected, f"Incorrect convergence flag: {variant}/{node['node']}"
        # 数据布局：[面元, 正反面, 7 个字段]；字段含义仅知第 3 列为温度 K、0 LE、1 H、2 G、4 Rn（见下），
        # 其余列待核对。
        data = np.fromfile(folder / f"process/faceteb_T={node['token']}.bin", dtype='<f4').reshape(-1, 2, 7)
        assert np.isfinite(data).all(), f"Nonfinite field: {variant}/{node['node']}"
        result['nodes'].append(dict(node=node['node'], token=node['token'], converged=expected,
            iterations=node['iterations'], temperature_delta_K=node['temperatureDelta'],
            energy_residual_W_m2=node['energyResidual'],
            leaf_energy_residual_W_m2=diag['maximumLeafEnergyResidual_W_m2'],
            stomatal_log_mismatch=diag['maximumStomatalLogMismatch']))
        if 'energyFailingFacets' in node:
            result['nodes'][-1].update(failing_facets=failed, total_facets=total,
                                      failing_fraction=failed / total)
        # 步骤 5：只对正午节点做空间统计与基线对比。
        if node['token'] == 'DOY214_12-20':
            # PAR 数据布局：[面元, 正反面, (入射, 吸收)]。
            par = np.fromfile(folder / f"process/facetpar_T={node['token']}.bin", dtype='<f4').reshape(-1, 2, 2)
            leaf = data[:, 0]  # Whole-leaf H/LE duplicated on the two sides: do not sum.
            temperature = leaf[ids, 3].astype(float) - 273.15
            result['noon'] = dict(converged=expected,
                central9_temperature_C=dict(min=float(temperature.min()), max=float(temperature.max()),
                    area_mean=float(np.average(temperature, weights=area[ids]))),
                lower_facet_62764={name: float(leaf[62764, column] - (273.15 if column == 3 else 0))
                    for name, column in [('T_C', 3), ('LE_W_m2', 0), ('H_W_m2', 1), ('G_W_m2', 2), ('Rn_W_m2', 4)]})
            # 第一个变体作为基线；之后的变体与其做差。
            if baseline_data is None:
                baseline_data, baseline_par = data, par
            else:
                result['noon']['max_abs_PAR_difference'] = float(np.abs(par - baseline_par).max())
                result['noon']['central9_temperature_difference_K'] = dict(
                    min=float((data[ids, 0, 3] - baseline_data[ids, 0, 3]).min()),
                    max=float((data[ids, 0, 3] - baseline_data[ids, 0, 3]).max()),
                    area_mean=float(np.average(data[ids, 0, 3] - baseline_data[ids, 0, 3], weights=area[ids])))
    report[variant] = result
(args.root / 'comparison.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps({v: {k: x for k, x in r.items() if k not in ['nodes', 'run']}
                  for v, r in report.items()}, ensure_ascii=False, indent=2))
