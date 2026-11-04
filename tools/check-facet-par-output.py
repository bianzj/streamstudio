"""Check the exported PAR data contract and physical bounds for a run."""
import json
import math
import struct
import sys
from pathlib import Path

root = Path(sys.argv[1])
paths = sorted((root / 'process').glob('facetpar_T=*.json'))
assert paths, 'Missing facet PAR process outputs'
for path in paths:
    m = json.loads(path.read_text(encoding='utf-8'))
    assert m['recordFloats'] == 2
    assert m['surfaceCount'] == 2 * m['facetCount']
    assert [f['id'] for f in m['fields']] == ['incidentParPhotons', 'absorbedParPhotons']
    assert m['units'] == 'umol photons m-2 s-1'
    data = (path.parent / m['dataFile']).read_bytes()
    assert len(data) == m['surfaceCount'] * 8
    peak = 0.0
    for incident, absorbed in struct.iter_unpack('<ff', data):
        assert math.isfinite(incident) and math.isfinite(absorbed)
        assert incident >= -1e-5 and absorbed >= -1e-5
        assert absorbed <= incident + max(1e-4, incident * 1e-5)
        peak = max(peak, incident)
    if m['solarZenith'] >= 90:
        assert peak < 1e-4, 'Night-time PAR must be zero'
print(f'PASS: {len(paths)} PAR nodes, geometry, units, binary sizes and physical bounds')
