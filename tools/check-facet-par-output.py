# ==========================================================================
# 文件：check-facet-par-output.py
# 功能：检查 FacetRT/FacetEB 运行导出的光合有效辐射（PAR）过程数据的格式约定与物理边界。
# 用法：python tools/check-facet-par-output.py <运行输出目录>（其下应有 process/facetpar_T=*.json）。
# 检查步骤：
#   1. 至少存在一个 facetpar_T=*.json 节点文件
#   2. 元数据约定：每条记录 2 个 float、surfaceCount = 2 × facetCount（正反面）、
#      字段依次为 incidentParPhotons / absorbedParPhotons、单位 umol photons m-2 s-1
#   3. 二进制文件大小 = surfaceCount × 8 字节（每面 2 个小端 float32）
#   4. 数值：有限值、不为明显负数（容差 1e-5）、吸收量不超过入射量（相对/绝对容差）
#   5. 太阳天顶角 ≥ 90°（夜间）时入射 PAR 峰值必须接近 0
# 输出：全部通过打印 PASS 与节点数；任何断言失败直接抛 AssertionError。
# 与其他文件：读取引擎 facetrt 写出的 PAR 过程文件（格式定义见引擎侧，待核对）。
# ==========================================================================

"""Check the exported PAR data contract and physical bounds for a run."""
import json
import math
import struct
import sys
from pathlib import Path

root = Path(sys.argv[1])
paths = sorted((root / 'process').glob('facetpar_T=*.json'))
assert paths, 'Missing facet PAR process outputs'
# 逐节点检查：元数据 → 二进制大小 → 逐面数值边界 → 夜间零值。
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
        # 吸收不应超过入射；留 max(1e-4, 1e-5·入射) 的浮点容差。
        assert absorbed <= incident + max(1e-4, incident * 1e-5)
        peak = max(peak, incident)
    # 太阳在地平线以下（夜间）：入射 PAR 峰值应为 0（容差 1e-4）。
    if m['solarZenith'] >= 90:
        assert peak < 1e-4, 'Night-time PAR must be zero'
print(f'PASS: {len(paths)} PAR nodes, geometry, units, binary sizes and physical bounds')
