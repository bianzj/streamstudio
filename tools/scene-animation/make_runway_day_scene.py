"""Create the open-airfield display scene: aircraft, tanks and cars on the runway, no town.

Starts from the sealed town scene, keeps every original asset file byte-for-byte, drops the town
(office/neighbour blocks, trees and the town ground slab), fills the slab's hole in the far ground with
a flat patch of the same material, moves the road vehicles onto the runway and re-aims the four cameras.
The source case is never modified.
"""
import argparse
import copy
import hashlib
import json
import math
import shutil
from pathlib import Path

import numpy as np
from integrity import require

TOWN_INSTANCES = ['base_target', 'base_ground', 'scenery_trees'] + [f'base_neighbor_{i}' for i in range(10)]
TOWN_ASSETS = ['base_office', 'base_neighbor', 'base_ground', 'scenery_natural_trees']
HOLE = (-95.0, -80.0, 95.0, 85.0)          # exact hole of scenery_expanded_ground (checked in the asset)
GROUND_Z = -0.03                            # z of scenery_expanded_ground
INFILL_MATERIAL = 'scenery_context_asphalt'
RUNWAY = dict(x=(-120.0, 140.0), y=(-125.0, -95.0), top_z=0.03)
# The road connector (z 0.21) crosses the runway at x 91..105; nothing may stand on it.
CONNECTOR = dict(x=(91.0, 105.0), margin=2.0)
# New world positions (x, y) on the runway; z is derived from the runway surface. Aircraft keep their poses.
PLACEMENT = {
    'tank_a': (-72.0, -100.5), 'tank_b': (-4.0, -100.5), 'tank_c': (78.0, -100.5),
    'blue_car': (-70.0, -120.0), 'red_car': (-10.0, -120.0), 'white_car': (14.0, -120.0), 'suv': (84.0, -120.0),
    'city_bus': (122.0, -110.0),
}
CAMERAS = [
    dict(id='overview', position=[-135, -235, 100], target=[10, -108, 1], up=[0, 0, 1], fovYDegrees=62),
    dict(id='runway_view', position=[-118, -158, 30], target=[8, -108, 2], up=[0, 0, 1], fovYDegrees=66),
    dict(id='vehicle_view', position=[-40, -150, 15], target=[0, -108, 2], up=[0, 0, 1], fovYDegrees=64),
    dict(id='high_view', position=[8, -108, 190], target=[8, -108, 0], up=[0, 1, 0], fovYDegrees=70),
]


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1 << 22), b''):
            h.update(block)
    return h.hexdigest()


def read(p):
    return json.loads(Path(p).read_text(encoding='utf-8-sig'))


def write(p, obj):
    Path(p).write_text(json.dumps(obj, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def load_obj_vertices(path):
    return np.array([list(map(float, line.split()[1:4])) for line in Path(path).read_text(encoding='utf-8').splitlines() if line.startswith('v ')])


def world_footprint(asset, instance, root):
    """Axis-aligned world footprint (xmin, ymin, xmax, ymax) of an instance, honouring the Y-up conversion."""
    v = load_obj_vertices(root / asset['path'])
    if asset['upAxis'] == 'Y':
        v = v[:, [0, 2, 1]]
        v[:, 1] *= -1
    m = np.asarray(instance['transform'], float).reshape(4, 4)
    w = np.c_[v, np.ones(len(v))] @ m.T
    return w[:, 0].min(), w[:, 1].min(), w[:, 0].max(), w[:, 1].max(), w[:, 2].min()


def gap(a, b):
    dx = max(a[0] - b[2], b[0] - a[2], 0.0)
    dy = max(a[1] - b[3], b[1] - a[3], 0.0)
    return math.hypot(dx, dy)


def build(source, output):
    source, output = Path(source).resolve(), Path(output).resolve()
    if output.exists():
        raise FileExistsError('output must be new: ' + str(output))
    scene = read(source / 'scene.json')
    assets = {a['id']: a for a in scene['assets']}
    keep_assets = [a for a in scene['assets'] if a['id'] not in TOWN_ASSETS]
    (output / 'assets').mkdir(parents=True)
    for a in keep_assets:
        shutil.copyfile(source / a['path'], output / a['path'])
    # Flat infill with the far-ground material: exactly the hole, two triangles, z = ground plane.
    x0, y0, x1, y1 = HOLE
    infill = ['# Flat infill of the far-ground hole that held the town slab; same material as scenery_expanded_ground\n',
              'mtllib airfield_infill.mtl\n']
    for x, y in [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]:
        infill.append(f'v {x:.6f} {y:.6f} {GROUND_Z:.6f}\n')
    infill += [f'usemtl {INFILL_MATERIAL}\n', 'f 1 2 3\n', 'f 1 3 4\n']
    (output / 'assets/airfield_infill.obj').write_text(''.join(infill), encoding='utf-8')
    (output / 'assets/airfield_infill.mtl').write_text(f'newmtl {INFILL_MATERIAL}\n', encoding='utf-8')
    new = copy.deepcopy(scene)
    new['assets'] = [dict(a) for a in keep_assets] + [dict(id='airfield_infill', path='assets/airfield_infill.obj', sha256=sha(output / 'assets/airfield_infill.obj'), upAxis='Z', units='m')]
    instances = [copy.deepcopy(i) for i in scene['instances'] if i['id'] not in TOWN_INSTANCES]
    identity = np.eye(4).ravel().tolist()
    instances.insert(1, dict(id='airfield_infill', asset='airfield_infill', transform=identity))
    for inst in instances:
        if inst['id'] in PLACEMENT:
            t = np.asarray(inst['transform'], float).reshape(4, 4)
            t[0, 3], t[1, 3] = PLACEMENT[inst['id']]
            t[2, 3] = RUNWAY['top_z'] + 0.04 + (t[2, 3] - 0.24 if inst['id'].startswith(('tank', 'blue', 'red', 'white', 'suv', 'city')) else 0)
            inst['transform'] = t.ravel().tolist()
    new['instances'] = instances
    new['cameras'] = CAMERAS
    props = [i for i in instances if i['id'].startswith(('aircraft', 'tank', 'blue', 'red', 'white', 'suv', 'city'))]
    footprints = {i['id']: world_footprint(assets[i['asset']], i, output) for i in props}
    for i in props:
        fp = footprints[i['id']]
        require(RUNWAY['x'][0] <= fp[0] and fp[2] <= RUNWAY['x'][1] and RUNWAY['y'][0] <= fp[1] and fp[3] <= RUNWAY['y'][1], lambda: ('not fully on the runway', i['id'], fp))
        require(fp[2] < CONNECTOR['x'][0] - CONNECTOR['margin'] or fp[0] > CONNECTOR['x'][1] + CONNECTOR['margin'], lambda: ('standing on the road connector', i['id'], fp))
    pairs = [(a, b, gap(footprints[a], footprints[b])) for n, a in enumerate(footprints) for b in list(footprints)[n + 1:]]
    worst = min(pairs, key=lambda p: p[2])
    require(worst[2] >= 2.5, lambda: ('vehicles too close', worst))
    new['provenance'] = dict(purpose='Open airfield: aircraft, tanks and cars on the runway, town removed',
                             sourceScene=str(source / 'scene.json'), removedInstances=TOWN_INSTANCES,
                             infill=dict(asset='airfield_infill', rectangle=list(HOLE), material=INFILL_MATERIAL),
                             movedToRunway=PLACEMENT, aircraftPoseUnchanged=True, camerasRedefined=True)
    write(output / 'scene.json', new)
    actors = read(source / 'actors.json')
    (output / 'source_actors_reference.json').write_text(json.dumps(actors, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    files = {str(p.relative_to(output)).replace('\\', '/'): dict(sha256=sha(p)) for p in sorted(output.rglob('*')) if p.is_file() and p.name != 'SHA256SUMS.json'}
    write(output / 'SHA256SUMS.json', dict(files=files))
    report = dict(status='passed', instances=len(instances), assets=len(new['assets']), minimumVehicleGapM=worst[2], closestPair=worst[:2],
                  footprints={k: [round(float(x), 2) for x in v] for k, v in footprints.items()})
    write(output / 'scene_build.json', report)
    return report


def main():
    p = argparse.ArgumentParser()
    p.add_argument('source', type=Path, help='sealed town case directory (scene.json, assets/, actors.json)')
    p.add_argument('output', type=Path, help='new directory for the open-airfield display scene')
    a = p.parse_args()
    print(json.dumps(build(a.source, a.output), ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
