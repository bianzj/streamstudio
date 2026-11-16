"""Prepare a full-day native FacetEB experiment on the open airfield.

Four matched runs share geometry, materials, thermal parameters, wind, air temperature and sky longwave
forcing and differ in exactly two switches:

    sun    : the shortwave column of the forcing is kept (sunlit) or set to zero (nosun)
    payload: every working prop's internal heat source is on (powered) or off (unpowered)

The day is simulated at a uniform 30 minute node width. Several identical spin-up days precede the output
day so that the deep ground reservoirs and prop skins reach a periodic state; closure between the last two
days is measured later, not assumed. Native FacetEB owns every temperature; nothing is painted here.
"""
import argparse
import copy
import json
import math
import shutil
from pathlib import Path

import numpy as np

from coupled_town_prepare import clip_triangle, subdivide_triangle, thermal_properties, sha, read, write
from prepare_thermal_case import load_world
from integrity import require

GROUND = ('runway_road_field', 'airfield_infill', 'scenery_expanded_ground')
FAR_GROUND = 'scenery_expanded_ground'
RUNS = {  # name -> (sun on, payload on)
    'sunlit_powered': (True, True),
    'sunlit_unpowered': (True, False),
    'nosun_powered': (False, True),
    'nosun_unpowered': (False, False),
}
NODE_SECONDS = 1800
FINAL_DOY = 179


def edge_limit(instance_name, material_id, near_ground_m):
    if instance_name == FAR_GROUND:
        return 20.0
    if instance_name == 'runway_road_field' and material_id.startswith(('runway', 'new_')):
        return near_ground_m
    return 4.0


def time_axis(spinup_days):
    """Uniform nodes; the last day is DOY 179. Returns per-node dicts (elapsed seconds, JD, clock hour, day)."""
    days = spinup_days + 1
    first_doy = FINAL_DOY - spinup_days
    per_day = 86400 // NODE_SECONDS
    nodes = []
    for k in range(days * per_day):
        sec = (k + 1) * NODE_SECONDS
        nodes.append(dict(node=k, elapsedSeconds=sec, julianDay=first_doy + sec / 86400.0, clockHour=(sec / 3600.0) % 24,
                          dayIndex=(sec - 1) // 86400, output=(sec - 1) // 86400 == days - 1))
    return nodes


def forcing(node, sun):
    hour = node['clockHour']
    daylight = max(0.0, math.sin(math.pi * (hour - 6) / 12))
    sw = 850.0 * daylight if sun else 0.0
    ta = 26 + 6 * math.cos(2 * math.pi * (hour - 14) / 24)
    ea = .55 * 6.112 * math.exp(17.67 * ta / (ta + 243.5))
    lw = 340 + 20 * daylight
    return [node['julianDay'], ta, ea, 1013.25, 2.5, sw, lw]


def prepare(display_case, actors_path, output, spinup_days=8, near_ground_m=2.0, ground_initial_k=None):
    display_case, output, actors_path = Path(display_case).resolve(), Path(output).resolve(), Path(actors_path).resolve()
    source = display_case
    if not (source / 'scene.json').exists():
        raise FileNotFoundError('display scene not found: ' + str(source / 'scene.json'))
    if output.exists() and any(p.name != source.name for p in output.iterdir()):
        raise FileExistsError('Preserve old cases; output must be new (or contain only the display scene): ' + str(output))
    if not 3 <= spinup_days <= 30:
        raise ValueError('spinup_days must be within 3..30')
    original, tri, mids, iids = load_world(source / 'scene.json')
    config = read(actors_path)
    names = [x['id'] for x in original['instances']]
    for actor in config['actors']:
        if actor['id'] not in names:
            raise ValueError('actor is not an instance of the display scene: ' + actor['id'])
    output.mkdir(exist_ok=True)
    for sub in ('assets', 'thermal/assets', 'source', 'native'):
        (output / sub).mkdir(parents=True, exist_ok=True)
    for a in original['assets']:
        shutil.copyfile(source / a['path'], output / a['path'])
    shutil.copyfile(source / 'scene.json', output / 'scene.json')
    write(output / 'actors.json', config)
    np.savez_compressed(output / 'source_geometry.npz', triangles=tri, material=mids, instance=iids)
    # Portable receipt: hash the copies inside the case, not the build-time absolute paths.
    source_hashes = {'scene.json': sha(output / 'scene.json'), 'actors.json': sha(output / 'actors.json')}
    for a in original['assets']:
        relative = Path(a['path']).as_posix()
        source_hashes[relative] = sha(output / relative)

    # Thermal region: every near-field instance plus 30 m; the far ground outside it is a declared boundary.
    near = iids != names.index(FAR_GROUND)
    xy = tri[near, :, :2].reshape(-1, 2)
    lo = np.floor(xy.min(0) / 10) * 10 - 30
    hi = np.ceil(xy.max(0) / 10) * 10 + 30
    roi = [float(lo[0]), float(lo[1]), float(hi[0]), float(hi[1])]
    inside = np.all((tri[:, :, 0] >= lo[0]) & (tri[:, :, 0] <= hi[0]) & (tri[:, :, 1] >= lo[1]) & (tri[:, :, 1] <= hi[1]), axis=1)
    raw, rm, ri, rs = [tri[inside]], [mids[inside]], [iids[inside]], [np.flatnonzero(inside)]
    for j in np.flatnonzero(~inside):
        clipped = clip_triangle(tri[j], roi)
        if len(clipped):
            raw.append(clipped)
            rm.append(np.full(len(clipped), mids[j]))
            ri.append(np.full(len(clipped), iids[j]))
            rs.append(np.full(len(clipped), j))
    t, m = np.concatenate(raw), np.concatenate(rm)
    inst, source_index = np.concatenate(ri), np.concatenate(rs)
    normal = np.cross(t[:, 1] - t[:, 0], t[:, 2] - t[:, 0])
    norm = np.linalg.norm(normal, axis=1)
    ground_ids = [names.index(n) for n in GROUND]
    isground = np.isin(inst, ground_ids)
    flat = np.abs(normal[:, 2]) > .99 * norm
    edge = np.maximum.reduce([np.linalg.norm(t[:, 1] - t[:, 0], axis=1), np.linalg.norm(t[:, 2] - t[:, 1], axis=1), np.linalg.norm(t[:, 0] - t[:, 2], axis=1)])
    material_ids = [x['id'] for x in original['materials']]
    limit = np.array([edge_limit(names[i], material_ids[k], near_ground_m) for i, k in zip(inst, m)])
    refine = isground & flat & (edge > limit)
    groups, gm, gi, gs = [t[~refine]], [m[~refine]], [inst[~refine]], [source_index[~refine]]
    for j in np.flatnonzero(refine):
        parts = subdivide_triangle(t[j], float(limit[j]))
        groups.append(parts)
        gm.append(np.full(len(parts), m[j]))
        gi.append(np.full(len(parts), inst[j]))
        gs.append(np.full(len(parts), source_index[j]))
    t = np.concatenate(groups).astype('<f4')
    m = np.concatenate(gm).astype(int)
    inst = np.concatenate(gi).astype(int)
    source_index = np.concatenate(gs).astype(int)
    area = np.linalg.norm(np.cross(t[:, 1] - t[:, 0], t[:, 2] - t[:, 0]), axis=1) / 2

    nodes = time_axis(spinup_days)
    total_seconds = nodes[-1]['elapsedSeconds']
    final_start = (spinup_days) * 86400
    schedule = config.get('heat_schedule', dict(mode='continuous'))
    if schedule['mode'] == 'continuous':
        heat_start, heat_stop = 0.0, float(total_seconds + 1)
    elif schedule['mode'] == 'final-day-window':
        heat_start, heat_stop = final_start + 3600.0 * schedule['startHour'], final_start + 3600.0 * schedule['stopHour']
        require(0 <= schedule['startHour'] < schedule['stopHour'] <= 24, 'invalid heat window')
    else:
        raise ValueError('heat_schedule.mode must be continuous or final-day-window')

    centres = t.mean(1)
    actor_by_id = {a['id']: a for a in config['actors']}
    groups_meta, binned = {}, np.zeros(len(t), dtype=int)
    cp = config['environment']['specific_heat_j_kg_k']
    for iid in np.unique(inst):
        name = names[iid]
        mask = inst == iid
        if name in actor_by_id:
            actor = actor_by_id[name]
            capacity = actor['heated_mass_kg'] * cp
            inv = np.linalg.inv(np.asarray(original['instances'][iid]['transform']).reshape(4, 4))
            vertices = np.c_[tri[iids == iid].reshape(-1, 3), np.ones(int((iids == iid).sum()) * 3)] @ inv.T
            low, high = vertices[:, :3].min(0), vertices[:, :3].max(0)
            local = np.c_[centres[mask], np.ones(int(mask.sum()))] @ inv.T
            points = (local[:, :3] - low) / (high - low)
            shape = config['heat_shape']
            w = np.full(len(points), shape['body_weight'])
            for hot in actor.get('hotspots', shape['hotspots']):
                w += hot['strength'] * np.exp(-.5 * np.sum(((points - hot['center']) / hot['radius']) ** 2, axis=1))
            w = np.clip(w, 0, 1)
            for k, mid in enumerate(m[mask]):
                if any(token in original['materials'][mid]['id'].lower() for token in ['window', 'glass', 'wheel', 'black']):
                    w[k] *= .2
            binned[mask] = np.maximum(1, np.rint(w * 8)).astype(int)
            raw_area = float(area[mask].sum())
            bounds = t[mask].reshape(-1, 3).max(0) - t[mask].reshape(-1, 3).min(0)
            effective = float(min(raw_area, 2 * (bounds[0] * bounds[1] + bounds[0] * bounds[2] + bounds[1] * bounds[2])))
            groups_meta[name] = dict(group=name, capacityJPerK=.3 * capacity, conductanceWPerK=actor['power_w'] / 12., airConductanceWPerK=0,
                                     powerW=actor['power_w'], startSeconds=heat_start, stopSeconds=heat_stop, initialK=298.15,
                                     maxSurfaceK=500, skinCapacityPerArea=.7 * capacity / raw_area, convectiveScale=.5 * effective / raw_area, kind='active-prop')
        elif iid in ground_ids:
            total = float(area[mask].sum())
            groups_meta[name] = dict(group=name, capacityJPerK=1e6 * total, conductanceWPerK=4 * total, airConductanceWPerK=0,
                                     powerW=0, startSeconds=0, stopSeconds=float(total_seconds + 1), initialK=298.15,
                                     maxSurfaceK=360, skinCapacityPerArea=120000., convectiveScale=.5, kind='lumped-deep-ground-reservoir')
            binned[mask] = 8

    keys = np.stack([m, inst, np.where(binned > 0, binned, 0)], axis=1)
    unique, new_m = np.unique(keys, axis=0, return_inverse=True)
    materials, physical = [], []
    for mid, iid, level in unique:
        mat = copy.deepcopy(original['materials'][mid])
        name = names[iid]
        alpha, cap = thermal_properties(mat)
        mat['id'] = f'm{mid}_i{iid}_w{level}'
        mat['thermalAlbedo'] = alpha
        mat['heatCapacityPerArea'] = cap
        params = dict(heatCapacityPerArea=cap, convectiveScale=1)
        if name in groups_meta:
            g = groups_meta[name]
            core = {k: v for k, v in g.items() if k not in ['skinCapacityPerArea', 'convectiveScale', 'kind']}
            core['weight'] = float(level) / 8
            params.update(heatCapacityPerArea=g['skinCapacityPerArea'], convectiveScale=g['convectiveScale'], lumpedThermal=core)
        materials.append(mat)
        physical.append(dict(name=mat['id'], type='Building', energyModel='solid', params=params))
    order = np.argsort(new_m, kind='stable')
    t, inst, source_index, new_m, area = t[order], inst[order], source_index[order], new_m[order], area[order]
    obj = output / 'thermal/assets/thermal_world.obj'
    with obj.open('w', encoding='utf-8') as f:
        f.write('mtllib thermal_world.mtl\n')
        for v in t.reshape(-1, 3):
            f.write(f'v {v[0]:.9g} {v[1]:.9g} {v[2]:.9g}\n')
        previous = -1
        for j, mid in enumerate(new_m):
            if mid != previous:
                f.write('g ' + materials[mid]['id'] + '\nusemtl ' + materials[mid]['id'] + '\n')
                previous = mid
            f.write(f'f {3 * j + 1} {3 * j + 2} {3 * j + 3}\n')
    proxy = copy.deepcopy(original)
    proxy['assets'] = [dict(id='thermal_world', path='assets/thermal_world.obj', sha256=sha(obj), upAxis='Z', units='m')]
    proxy['instances'] = [dict(id='thermal_world', asset='thermal_world', transform=np.eye(4).ravel().tolist())]
    proxy['materials'] = materials
    proxy['provenance'] = dict(purpose='Separate thermal discretization of the open-airfield display scene', roi=roi, nearGroundMaxEdgeMeters=near_ground_m,
                               groundMaxEdgeMeters=4, farGroundMaxEdgeMeters=20, sourceScene=str(source / 'scene.json'))
    write(output / 'thermal/scene.json', proxy)
    np.savez_compressed(output / 'thermal/mapping.npz', source_triangle=source_index, source_instance=inst, material=new_m, areas=area)

    # Native Y-up transformation with the declared 0.6 m vertical offset (identical to the town pipeline).
    native = t[:, :, [0, 2, 1]].copy()
    native[:, :, 1] += .6
    native[:, :, 2] *= -1
    width = 2 * math.ceil(float(np.abs(native[:, :, 0]).max()) + 1)
    depth = 2 * math.ceil(float(np.abs(native[:, :, 2]).max()) + 1)
    height = math.ceil(float(native[:, :, 1].max()) + 2)
    widths = [NODE_SECONDS] * len(nodes)
    ground_start = {True: ground_initial_k['sunlit'] if ground_initial_k else 305.0, False: ground_initial_k['nosun'] if ground_initial_k else 295.0}
    for run_name, (sun, powered) in RUNS.items():
        out = output / 'native' / run_name
        out.mkdir(parents=True)
        model = out / 'world.obj'
        with model.open('w', encoding='utf-8') as f:
            for v in native.reshape(-1, 3):
                f.write(f'v {v[0]:.9g} {v[1]:.9g} {v[2]:.9g}\n')
            last = -1
            for j, mid in enumerate(new_m):
                if mid != last:
                    f.write('g ' + materials[mid]['id'] + '\n')
                    last = mid
                f.write(f'f {3 * j + 1} {3 * j + 2} {3 * j + 3}\n')
        (out / 'origin.txt').write_text(f'{width / 2} {depth / 2} 0 1 0\n', encoding='utf-8')
        weather = [forcing(n, sun) for n in nodes]
        (out / 'meteorology.txt').write_text(str(len(weather)) + '\n' + ''.join(' '.join(f'{v:.10f}' for v in row) + '\n' for row in weather), encoding='utf-8')
        mats = copy.deepcopy(physical)
        for mat in mats:
            h = mat['params'].get('lumpedThermal')
            if h is None:
                continue
            if h['powerW'] == 0:
                h['initialK'] = ground_start[sun]          # deep ground reservoir starts near its expected periodic mean
            elif not powered:
                h['powerW'] = 0
        mats.append(dict(name='soil_boundary', type='Soil', params=dict(rss=1e9, cs=1800, rhos=1600, lambdas=1, SMC=.01)))
        c = dict(scene=dict(x=width, y=depth, height=height, voxel=max(width, depth) + 1, terrain=False,
                            background=dict(spectralName=materials[0]['id'], thermalName=materials[0]['id'], materialName='soil_boundary', materialType='Soil', heterogeneityEnabled=False)),
                 sensor=dict(x=32, y=32, bands='10000', image=False, temperature=True, process=True, energyProcess=True, radiationProcess=False, principalPlane=False, hemisphere=False),
                 control=dict(samples=8, depth=4, facetBackend='gpu', couplingIterations=100, temperatureTolerance=.05, temperatureRelaxation=.5,
                              radiationSolver='traditional', spectralAccelerationWidth=500, soilTemperatureMethod=2, stabilityCorrection=False),
                 meteo=dict(path=str(out / 'meteorology.txt'), start=0, end=len(widths), dTime=NODE_SECONDS, timeStepSeconds=widths, Tsold=ground_start[sun], z=35,
                            latitude=40, longitude=120, timezone=8, year=2019),
                 light=dict(zenith=30, azimuth=180, direct=.8, diffuse=.2, skyTemperature=280), atmosphere=dict(enabled=False), fluid=dict(enabled=False),
                 spectra=[dict(name=x['id'], model='custom', reflectance=str(x['thermalAlbedo']), transmittance='0', refTir=1 - x['emissivity'], tauTir=0) for x in materials],
                 materials=mats, thermals=[dict(name=x['id'], sunlitTemperature=298.15, shadedTemperature=298.15) for x in materials],
                 canopies=[dict(name='solid', structureType='solid', leafwidth=2)],
                 objects=dict(count=1, names=['world'], items=[dict(name='world', type='Building', fileName=str(model), positionFile=str(out / 'origin.txt'),
                                                                     meshes=[dict(name=x['id'], spectralName=x['id'], thermalName=x['id'], materialName=x['id'], canopyName='solid') for x in materials])]))
        write(out / 'project.json', dict(format='histream-project', schemaVersion=1, name='Airfield day ' + run_name, mode='eFacetEB', configuration=c))
    write(output / 'timeline_nodes.json', nodes)
    write(output / 'coupling.json', dict(groups=groups_meta, nodeSeconds=NODE_SECONDS, spinupDays=spinup_days, outputDayDoy=FINAL_DOY, heatSchedule=schedule,
        runs={k: dict(sun=v[0], payload=v[1]) for k, v in RUNS.items()}, thermalRoi=roi, originalTriangles=len(tri), thermalTriangles=len(t), syntheticWeather=True,
        solarWindowHours=[6, 18], peakShortwaveWm2=850.0,
        groundInitialK={'sunlit': ground_start[True], 'nosun': ground_start[False]},
        sunSwitch='shortwave column of the forcing multiplied by 0; air temperature, humidity, wind and sky longwave unchanged',
        limitations=['sun-off isolates direct/diffuse solar loading; air temperature and sky longwave are held at the sunlit-day values, so it is a controlled counterfactual, not a real overcast/night-only day',
                     'thermal domain is the airfield ROI; far display terrain uses a declared boundary state',
                     'ground deep reservoir is lumped (C = 1e6 J/m2K, G = 4 W/m2K), not local 3D conduction',
                     'forcing is the synthetic half-sine day of the town pipeline (06-18 h, 850 W/m2 peak), not measured weather',
                     'all operating/material parameters are game assumptions, not measured signatures']))
    write(output / 'source_inputs.json', source_hashes)
    report = dict(status='passed', originalAssets=len(original['assets']), originalInstances=len(original['instances']), thermalTriangles=len(t), thermalMaterials=len(materials),
                  thermalRoi=roi, activeProps=len(config['actors']), groundReservoirs=sum(g['kind'].startswith('lumped-deep') for g in groups_meta.values()),
                  nodes=len(nodes), spinupDays=spinup_days, nodeSeconds=NODE_SECONDS, nearGroundMaxEdgeM=near_ground_m, runs=list(RUNS), heatSchedule=schedule)
    write(output / 'preparation.json', report)
    print(json.dumps(report, ensure_ascii=False), flush=True)
    return report


def main():
    p = argparse.ArgumentParser()
    p.add_argument('display_scene', type=Path, help='directory with the open-airfield scene.json and assets')
    p.add_argument('actors', type=Path)
    p.add_argument('output', type=Path, help='case directory; may contain only the display scene directory')
    p.add_argument('--spinup-days', type=int, default=12)
    p.add_argument('--near-ground-edge', type=float, default=2.0)
    a = p.parse_args()
    prepare(a.display_scene, a.actors, a.output, a.spinup_days, a.near_ground_edge)


if __name__ == '__main__':
    main()
