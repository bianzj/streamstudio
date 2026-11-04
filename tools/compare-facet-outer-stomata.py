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
ids = np.concatenate([np.arange(i * 958, (i + 1) * 958)
                      for i in [52, 53, 54, 64, 65, 66, 76, 77, 78]])
for variant in args.variants:
    folder = Path(read(args.root / variant / 'run.json')['outputDir'])
    manifest, index = read(folder / 'run-manifest.json'), read(folder / 'faceteb_run.json')
    assert manifest['status'] != 'running', f'{variant} still running'
    geometry = read(folder / 'faceteb.json')
    vertices = np.asarray(geometry['vertexPositions']).reshape(-1, 3, 3)
    area = np.linalg.norm(np.cross(vertices[:, 1] - vertices[:, 0],
                                   vertices[:, 2] - vertices[:, 0]), axis=1) / 2
    assert geometry['leafFacetCount'] == 144 * 958, 'Not the original 144-plant scene'
    if baseline_vertices is None:
        baseline_vertices = vertices
    else:
        assert np.array_equal(vertices, baseline_vertices), 'Scene geometry changed'
    t = lambda key: datetime.fromisoformat(manifest[key].replace('Z', '+00:00'))
    result = dict(run=str(folder), status=manifest['status'],
                  elapsed_seconds=(t('finishedAt') - t('startedAt')).total_seconds(),
                  completed=len(index['completed']),
                  converged=sum(n['converged'] for n in index['completed']),
                  total_iterations=sum(n['iterations'] for n in index['completed']), nodes=[])
    for node in index['completed']:
        diag = read(folder / f"diagnostics/facet_physics_node={node['node']}.json")
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
        if node['token'] == 'DOY214_12-20':
            par = np.fromfile(folder / f"process/facetpar_T={node['token']}.bin", dtype='<f4').reshape(-1, 2, 2)
            leaf = data[:, 0]  # Whole-leaf H/LE duplicated on the two sides: do not sum.
            temperature = leaf[ids, 3].astype(float) - 273.15
            result['noon'] = dict(converged=expected,
                central9_temperature_C=dict(min=float(temperature.min()), max=float(temperature.max()),
                    area_mean=float(np.average(temperature, weights=area[ids]))),
                lower_facet_62764={name: float(leaf[62764, column] - (273.15 if column == 3 else 0))
                    for name, column in [('T_C', 3), ('LE_W_m2', 0), ('H_W_m2', 1), ('G_W_m2', 2), ('Rn_W_m2', 4)]})
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
