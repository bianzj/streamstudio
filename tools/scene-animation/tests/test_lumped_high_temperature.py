"""Working props may exceed the passive-surface 360 K guard."""
import argparse,json
from pathlib import Path
from test_lumped_scene import run

p=argparse.ArgumentParser();p.add_argument('engine',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
index,diagnostics=run(a.engine,a.output,20000)
assert all(d['lumpedThermalMaxSurfaceResidual_W_m2']<2 for d in diagnostics)
assert any(d['lumpedThermal'][0]['coreTemperature_K']>360 for d in diagnostics)
print(json.dumps(dict(status='passed',checks=2,scope='high-temperature thermal source converges without changing energy thresholds')))
