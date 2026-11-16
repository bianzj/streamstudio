"""Render the 3-5 um day of the open airfield from the four matched native FacetEB runs.

Inputs are the solved native states only: facet temperatures, local shortwave and longwave radiosity.
Imaging follows the sealed town pipeline (17-point Simpson band integral, equivalent-greybody sky and
5772 K solar spectrum, 320x180 12-bit camera with PSF/pixel averaging/noise). Display scale is one fixed
band-radiance range for every run, view and time, derived from the data and recorded in the receipt.
Intermediate video frames interpolate linearly between the 30 minute native nodes; they are not new solves.
"""
import argparse
import csv
import json
import math
import shutil
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from coupled_town_prepare import read, write, sha
from prepare_thermal_case import load_world
from prepare_runway_day import RUNS, NODE_SECONDS
from thermal_imaging import band, outgoing_band, observe_radiance, camera_rays, solar_fraction, SIGMA, BAND_POINTS, energy_record_width
from case_seal import CASE_SEAL, RENDER_SEAL, verify_seal, write_seal
from run_coupled_town import acceptance
from integrity import require
from video_encoder import pick_encoder, encoder_args, add_argument as add_encoder_argument
from steps_manifest import StepsVerifier

RUN_LABEL = {'sunlit_powered': ('有太阳', '载荷开动'), 'sunlit_unpowered': ('有太阳', '载荷关闭'),
             'nosun_powered': ('无太阳', '载荷开动'), 'nosun_unpowered': ('无太阳', '载荷关闭')}
GROUPS = {'aircraft': ['aircraft_a', 'aircraft_b'], 'tanks': ['tank_a', 'tank_b', 'tank_c'],
          'cars': ['blue_car', 'red_car', 'white_car', 'suv'], 'bus': ['city_bus']}
VIEW_SEED = {'overview': 0, 'runway_view': 1, 'vehicle_view': 2, 'high_view': 3}
STILL_HOURS = [4.0, 8.0, 12.0, 15.0, 20.0]
# Four-panel still/video layout (pixels): title band, label strip above each panel, row gap below the label.
# Recorded in render_receipt.json and read by build_runway_day_deck.crop_sky.
STILL_LAYOUT = dict(top=80, label=34, rowGap=60)
_TB_T = np.linspace(200, 600, 4001)
_TB_B = band(_TB_T)


def brightness_temperature(radiance):
    return np.interp(radiance, _TB_B, _TB_T)


def display_t(radiance, low_k, high_k):
    # gray is linear in the 3-5 um equivalent brightness temperature between two fixed bounds (display only)
    return np.rint(np.clip((brightness_temperature(radiance) - low_k) / (high_k - low_k), 0, 1) * 255).astype(np.uint8)


def display_enhanced(radiance):
    # per-picture P2-P98 stretch of the brightness temperature (display enhancement only); returns image and (low, high) K
    t = brightness_temperature(radiance)
    lo, hi = np.percentile(t, [2, 98])
    hi = max(hi, lo + 0.5)
    return np.rint(np.clip((t - lo) / (hi - lo), 0, 1) * 255).astype(np.uint8), (float(lo), float(hi))


def hour_label(hour):
    h = int(hour) % 24 if hour < 24 else 24
    m = int(round((hour - int(hour)) * 60))
    return f'{h:02d}:{m:02d}'


class Case:
    def __init__(self, case, verify_steps='lazy'):
        self.root = Path(case).resolve()
        # Per-node arrays are outside the case seal (size); steps_manifest.json, which IS sealed,
        # carries their SHA-256 and every array/diagnostic is checked when first read.
        self.steps = StepsVerifier(self.root, RUNS, verify_steps)
        self.scene = read(self.root / 'scene.json')
        self.proxy, self.tri, self.mids, _ = load_world(self.root / 'thermal/scene.json')
        self.n = len(self.tri)
        mapping = np.load(self.root / 'thermal/mapping.npz')
        self.instance, self.areas, self.source_triangle = mapping['source_instance'], mapping['areas'], mapping['source_triangle']
        self.ids = [i['id'] for i in self.scene['instances']]
        geometry = np.load(self.root / 'source_geometry.npz')
        names = np.array([m['id'] for m in self.scene['materials']])
        self.source_material = names[geometry['material'][self.source_triangle]]
        self.normals = np.cross(self.tri[:, 1] - self.tri[:, 0], self.tri[:, 2] - self.tri[:, 0])
        self.eps = np.asarray([m['emissivity'] for m in self.proxy['materials']])[self.mids]
        self.albedo = np.asarray([m['thermalAlbedo'] for m in self.proxy['materials']])[self.mids]
        self.nodes = read(self.root / 'timeline_nodes.json')
        self.coupling = read(self.root / 'coupling.json')
        self.days = max(n['dayIndex'] for n in self.nodes) + 1
        self.per_day = 86400 // NODE_SECONDS
        self.indices = {r: read(self.root / 'native' / r / 'output/faceteb_run.json') for r in RUNS}
        widths = {r: energy_record_width(read(self.root / 'native' / r / 'project.json')) for r in RUNS}
        if len(set(widths.values())) != 1:
            raise ValueError('Native runs use different energy record layouts: ' + repr(widths))
        self.record_width = next(iter(widths.values()))
        self.weather = {r: np.loadtxt(self.root / 'native' / r / 'meteorology.txt', skiprows=1) for r in RUNS}
        self.masks = {}
        for group, ids in GROUPS.items():
            self.masks[group] = np.isin(self.instance, [self.ids.index(i) for i in ids])
        for actor in self.coupling['groups']:
            if self.coupling['groups'][actor]['kind'] == 'active-prop':
                self.masks[actor] = self.instance == self.ids.index(actor)
        runway = self.instance == self.ids.index('runway_road_field')
        self.masks['runway_pavement'] = runway & np.char.startswith(self.source_material, 'runway_')
        self.masks['grass_field'] = runway & np.char.startswith(self.source_material, 'field_')
        self.masks['far_ground'] = self.instance == self.ids.index('scenery_expanded_ground')
        self.ground = np.isin(self.instance, [self.ids.index(x) for x in ('runway_road_field', 'airfield_infill', 'scenery_expanded_ground')])
        # far-ground facets lying under the 2 cm higher runway field / infill patch are shaded by them and are never seen;
        # the far-field state beyond the ROI is estimated from the exposed ones only
        centroid = self.tri.mean(1)
        covered = np.zeros(self.n, bool)
        for name in ('runway_road_field', 'airfield_infill'):
            xy = self.tri[self.instance == self.ids.index(name)][:, :, :2].reshape(-1, 2)
            lo, hi = xy.min(0), xy.max(0)
            covered |= np.all((centroid[:, :2] >= lo) & (centroid[:, :2] <= hi), axis=1)
        self.masks['far_ground_exposed'] = self.masks['far_ground'] & ~covered
        require(self.masks['far_ground_exposed'].any(), 'no exposed far-ground facets')
        self.active = np.zeros(self.n, bool)
        for g in GROUPS:
            self.active |= self.masks[g]

    def first_output_node(self):
        return (self.days - 1) * self.per_day

    def records(self, run, node):
        info = self.indices[run]['completed'][node]
        path = self.root / 'native' / run / 'output/.facet_steps' / ('energy_T=' + info['token'] + '.bin')
        self.steps.check(run, path)
        return np.fromfile(path, dtype='<f4').reshape(self.n + 2, 2, self.record_width)[:self.n]

    def diagnostic(self, run, node):
        path = self.root / 'native' / run / 'output/diagnostics' / f'facet_physics_node={node}.json'
        self.steps.check(run, path)
        return read(path)


def build_view_cache(case, engine_geometry):
    cache = {}
    scene = case.scene
    width, height = scene['width'], scene['height']
    for camera in scene['cameras']:
        view = camera['id']
        hit = np.fromfile(engine_geometry / (view + '_hits.f32'), dtype='<f4').reshape(height, width, 8)
        display_hit = np.fromfile(case.root / 'gpu_geometry_display' / (view + '_hits.f32'), dtype='<f4').reshape(height, width, 8)
        valid, display_valid = hit[:, :, 0] >= 0, display_hit[:, :, 0] >= 0
        triangle = np.clip(hit[:, :, 7].astype(int), 0, case.n - 1)
        rays = camera_rays(camera, width, height)
        side = (np.sum(case.normals[triangle] * rays, axis=-1) >= 0).astype(int)
        both = valid & display_valid
        cache[view] = dict(valid=valid, triangle=triangle, side=side, far=display_valid & ~valid, sky=~display_valid,
                           maxHitDistanceDifferenceM=float(np.max(np.abs(hit[:, :, 0][both] - display_hit[:, :, 0][both]))),
                           farBoundaryPixels=int((display_valid & ~valid).sum()), skyPixels=int((~display_valid).sum()))
    return cache


def verify_case(case, allow_unsealed=False):
    """Refuse to render stale or edited native results.

    A case seal covers inputs/configuration/GPU geometry with relative paths. Legacy
    (unsealed) cases are still checked against native receipts and source_inputs when
    --allow-unsealed is used.
    """
    root = case.root
    sealed = (root / CASE_SEAL).is_file()
    if sealed:
        summary = verify_seal(root)
        print(f'case seal verified: {summary["files"]} files, {summary["bytes"] / 1e9:.3f} GB', flush=True)
        if summary['extras']:
            print(f'note: {len(summary["extras"])} file(s) not covered by the case seal', flush=True)
    elif allow_unsealed:
        print('WARNING: no case_seal.json; checking native receipts and source_inputs only', flush=True)
    else:
        raise RuntimeError('case_seal.json is missing; run "python case_seal.py CASE" or pass --allow-unsealed')
    native = read(root / 'native_runs.json')
    if native.get('status') != 'passed':
        raise ValueError('native_runs.json is not passed')
    expected = read(root / 'preparation.json')['nodes']
    for run_name in RUNS:
        receipt = native['runs'][run_name]
        project = root / 'native' / run_name / 'project.json'
        if receipt.get('configSha256') != sha(project):
            raise ValueError('native project changed since the solve: ' + run_name)
        ok, issues = acceptance(receipt, expected)
        if not ok:
            raise ValueError('native receipt does not satisfy the acceptance rules (' + run_name + '): ' + '; '.join(issues))
        index = read(root / 'native' / run_name / 'output' / 'faceteb_run.json')
        if index.get('status') != 'completed' or len(index.get('completed', [])) != expected:
            raise ValueError('native index is incomplete for ' + run_name)
    case.steps.prepare(sealed)
    if not sealed:
        for name, expected_hash in read(root / 'source_inputs.json').items():
            path = Path(name)
            path = path if path.is_absolute() else root / path
            if not path.is_file() or sha(path) != expected_hash:
                raise ValueError('source input changed: ' + name)
        display = root / 'display_scene' / 'scene.json'
        if display.is_file() and sha(display) != sha(root / 'scene.json'):
            raise ValueError('case scene.json differs from display_scene/scene.json')


def run(case_dir, ffmpeg, video_views, stills, probe_nodes=0, out_dir=None, allow_unsealed=False, video_encoder='auto', verify_steps='lazy'):
    # Probe the encoder before any expensive work: a missing libx264 used to fail only at the first video frame.
    encoder = pick_encoder(ffmpeg, video_encoder) if video_views else None
    if encoder:
        print(f'video encoder: {encoder}', flush=True)
    case = Case(case_dir, verify_steps)
    src = case.root
    if probe_nodes:
        # Test mode: treat the first 2*probe_nodes nodes as two short 'days'; results are not meaningful physics products.
        require(out_dir, 'probe mode needs --out-dir')
        case.per_day, case.days = probe_nodes, 2
    else:
        verify_case(case, allow_unsealed)
    root = Path(out_dir).resolve() if out_dir else src
    if out_dir:
        root.mkdir(parents=True, exist_ok=True)
    scene = case.scene
    width, height = scene['width'], scene['height']
    views = [c['id'] for c in scene['cameras']]
    cache = build_view_cache(case, src / 'gpu_geometry_thermal')
    visible = np.unique(np.concatenate([c['triangle'][c['valid']] for c in cache.values()]))
    compact = np.full(case.n, -1, dtype=int)
    compact[visible] = np.arange(len(visible))
    first = case.first_output_node()
    # hour 0 of the output day is the last node of the preceding day (same physical state, hour 24 of day -1)
    node_ids = [first - 1] + list(range(first, first + case.per_day))
    hours = [0.0] + [(k + 1) * NODE_SECONDS / 3600 for k in range(case.per_day)]
    fraction = solar_fraction()
    radiance = {v: {r: np.zeros((len(node_ids), height, width), dtype=np.float32) for r in RUNS} for v in views}
    history, extremes = [], []
    far_id = case.masks['far_ground_exposed']
    for r in RUNS:
        weather = case.weather[r]
        for k, node in enumerate(node_ids):
            rec = case.records(r, node)
            sky_k = float((weather[node, 6] / SIGMA) ** .25)
            sw = float(weather[node, 5])
            both = outgoing_band(rec, case.eps, case.albedo, sky_k, fraction)
            rad = both[visible]
            # declared far-field state beyond the thermal ROI: area-mean radiance of the SOLVED, exposed far-ground facets (same longwave/solar environment)
            far_r = float(np.average(both[far_id, 0], weights=case.areas[far_id]))
            for v in views:
                c = cache[v]
                jj = np.maximum(compact[c['triangle']], 0)
                img = rad[jj, c['side']]
                img[c['sky']] = float(band(sky_k))
                img[c['far']] = far_r
                radiance[v][r][k] = img
            # per-group statistics from the solved facets, side 0 (outward for closed bodies and upward ground)
            full = both[:, 0]
            for g, mask in case.masks.items():
                a = case.areas[mask]
                t_mean = float(np.average(rec[mask, 0, 0], weights=a))
                rad_mean = float(np.average(full[mask], weights=a))
                history.append(dict(run=r, node=node, outputIndex=k, hour=hours[k], group=g, skinMeanK=t_mean, skinMaxK=float(rec[mask, 0, 0].max()),
                                    bandBrightnessK=float(brightness_temperature(rad_mean)), bandRadiance=rad_mean,
                                    airC=float(weather[node, 1]), shortwaveWm2=sw, skyK=sky_k))
            extremes.append(dict(run=r, node=node, groundMaxK=float(rec[case.ground, 0, 0].max()), groundMinK=float(rec[case.ground, 0, 0].min()),
                                 activeMaxK=float(rec[case.active, 0, 0].max()), activeMinK=float(rec[case.active, 0, 0].min())))
            if (k + 1) % 12 == 0:
                print(f'processed {r} node {k + 1}/{len(node_ids)}', flush=True)
    # fixed display range from the data (sky excluded), rounded outward to 5 K
    sample = []
    for v in views:
        c = cache[v]
        keep = ~c['sky']
        for r in RUNS:
            sample.append(radiance[v][r][:, keep].ravel()[::97])
    sample = np.concatenate(sample)
    t_lo, t_hi = float(brightness_temperature(np.percentile(sample, 0.05))), float(brightness_temperature(np.percentile(sample, 99.995)))
    low_k, high_k = 5 * math.floor(t_lo / 5), 5 * math.ceil(t_hi / 5)
    low, high = float(band(low_k)), float(band(high_k))
    print(f'display range {low_k}-{high_k} K equivalent band temperature', flush=True)
    (root / 'radiance').mkdir(exist_ok=True)
    (root / 'display').mkdir(exist_ok=True)
    (root / 'rgb').mkdir(exist_ok=True)
    for v in views:
        shutil.copyfile(src / 'gpu_geometry_display' / (v + '_visible.png'), root / 'rgb' / (v + '.png'))
        for r in RUNS:
            codes = np.zeros((len(node_ids), height // 4, width // 4), dtype=np.uint16)
            for k in range(len(node_ids)):
                obs = observe_radiance(radiance[v][r][k], low, high, seed=5100 + VIEW_SEED[v] * 10 + list(RUNS).index(r), frame=k)
                codes[k] = obs['codes']
            np.savez_compressed(root / 'radiance' / f'{v}_{r}.npz', radiance=radiance[v][r].astype('<f4'), adc=codes, hours=np.array(hours), lowK=low_k, highK=high_k)
            ir_height, ir_width = codes.shape[1], codes.shape[2]
    # histories
    fields = list(history[0])
    with (root / 'day_history.csv').open('w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(history)
    with (root / 'day_extremes.csv').open('w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=list(extremes[0]))
        w.writeheader()
        w.writerows(extremes)
    closure = periodic_closure(case, node_ids)
    # solver-side energy diagnostics for the output day
    energy = energy_summary(case, first)
    write(root / 'closure.json', closure)
    write(root / 'energy_summary.json', energy)
    font = ImageFont.truetype(r'C:\Windows\Fonts\msyh.ttc', 28)
    small = ImageFont.truetype(r'C:\Windows\Fonts\msyh.ttc', 21)
    panels = {}
    for v in views:
        panels[v] = {}
    still_dir = root / 'stills'
    still_dir.mkdir(exist_ok=True)
    for v in views:
        for hour in stills:
            k = hours.index(hour)
            g = Image.new('RGB', (width * 2, (height + STILL_LAYOUT['rowGap']) * 2 + STILL_LAYOUT['top']), '#101c26')
            d = ImageDraw.Draw(g)
            ge = Image.new('RGB', (width * 2, (height + STILL_LAYOUT['rowGap']) * 2 + STILL_LAYOUT['top']), '#101c26')
            de = ImageDraw.Draw(ge)
            zen = case.indices['sunlit_powered']['completed'][node_ids[k]]['solarZenith']
            d.text((18, 8), f'{v} · 3–5 μm · {hour_label(hour)} · 太阳天顶角 {zen:.1f}°（≥90°为夜间）· 固定范围 {low_k}–{high_k} K 等效亮温', font=font, fill='white')
            d.text((18, 46), '上排有太阳，下排无太阳；左列载荷开动，右列载荷关闭；灰度对等效亮温线性，四格同一范围', font=small, fill='#c4d5df')
            for row, sun in enumerate(('sunlit', 'nosun')):
                for col, payload in enumerate(('powered', 'unpowered')):
                    r = f'{sun}_{payload}'
                    obs = observe_radiance(radiance[v][r][k], low, high, seed=5100 + VIEW_SEED[v] * 10 + list(RUNS).index(r), frame=k)
                    img = Image.fromarray(display_t(obs['radiance'], low_k, high_k)).resize((width, height), Image.Resampling.BILINEAR).convert('RGB')
                    x, y = col * width, STILL_LAYOUT['top'] + row * (height + STILL_LAYOUT['rowGap'])
                    d.text((x + 14, y + 4), ' · '.join(RUN_LABEL[r]), font=small, fill='#ffd27a')
                    g.paste(img, (x, y + STILL_LAYOUT['label']))
                    enhanced, (e_lo, e_hi) = display_enhanced(obs['radiance'])
                    ge.paste(Image.fromarray(enhanced).resize((width, height), Image.Resampling.BILINEAR).convert('RGB'), (x, y + STILL_LAYOUT['label']))
                    de.text((x + 14, y + 4), ' · '.join(RUN_LABEL[r]) + f'   本格拉伸 {e_lo:.1f}–{e_hi:.1f} K (P2–P98)', font=small, fill='#ffd27a')
            g.save(still_dir / f'{v}_{int(hour):02d}h.png')
            de.text((18, 8), f'{v} · 3–5 μm · {hour_label(hour)} · 仅显示增强（每格独立 P2–P98 拉伸，格间不可比）', font=font, fill='white')
            de.text((18, 46), '同一次求解的同一批数据；增强只改变灰度映射，不改变辐亮度或 12 bit 数字量', font=small, fill='#c4d5df')
            ge.save(still_dir / f'{v}_{int(hour):02d}h_enhanced.png')
    # videos
    video_receipts = {}
    for v in video_views:
        out = root / f'airfield_day_{v}.mp4'
        frames_dir = root / 'video_frames' / v
        frames_dir.mkdir(parents=True, exist_ok=True)
        proc = subprocess.Popen([str(ffmpeg), '-y', '-v', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f"{width * 2}x{(height + STILL_LAYOUT['rowGap']) * 2 + STILL_LAYOUT['top']}", '-r', '24', '-i', '-']
                                + encoder_args(encoder, crf=18) + ['-pix_fmt', 'yuv420p', str(out)], stdin=subprocess.PIPE, stderr=subprocess.PIPE)
        for frame in range(288):
            hour = 24 * frame / 288
            x = hour / (NODE_SECONDS / 3600)
            k0 = min(int(x), len(node_ids) - 2)
            alpha = x - k0
            g = Image.new('RGB', (width * 2, (height + STILL_LAYOUT['rowGap']) * 2 + STILL_LAYOUT['top']), '#101c26')
            d = ImageDraw.Draw(g)
            zen = (1 - alpha) * case.indices['sunlit_powered']['completed'][node_ids[k0]]['solarZenith'] + alpha * case.indices['sunlit_powered']['completed'][node_ids[k0 + 1]]['solarZenith']
            d.text((18, 8), f'{v} · 3–5 μm · 一天 24 h 压缩为 12 s · {hour_label(hour)}', font=font, fill='white')
            d.text((18, 46), f'太阳天顶角约 {zen:.0f}°；固定范围 {low_k}–{high_k} K（灰度对等效亮温线性）；节点间辐亮度线性插值，不是新增解算', font=small, fill='#c4d5df')
            for row, sun in enumerate(('sunlit', 'nosun')):
                for col, payload in enumerate(('powered', 'unpowered')):
                    r = f'{sun}_{payload}'
                    rr = (1 - alpha) * radiance[v][r][k0] + alpha * radiance[v][r][k0 + 1]
                    obs = observe_radiance(rr, low, high, seed=5100 + VIEW_SEED[v] * 10 + list(RUNS).index(r), frame=frame)
                    img = Image.fromarray(display_t(obs['radiance'], low_k, high_k)).resize((width, height), Image.Resampling.BILINEAR).convert('RGB')
                    xx, yy = col * width, STILL_LAYOUT['top'] + row * (height + STILL_LAYOUT['rowGap'])
                    d.text((xx + 14, yy + 4), ' · '.join(RUN_LABEL[r]), font=small, fill='#ffd27a')
                    g.paste(img, (xx, yy + STILL_LAYOUT['label']))
            if frame % 24 == 0:
                g.save(frames_dir / f'frame_{frame:04d}.png', compress_level=1)
            try:
                proc.stdin.write(g.tobytes())
            except (BrokenPipeError, OSError) as exc:
                try:
                    proc.stdin.close()
                except OSError:
                    pass
                error = proc.stderr.read()
                code = proc.wait()
                raise RuntimeError(f'ffmpeg failed while writing {v} frame {frame} (exit {code}): ' +
                                   error.decode(errors='replace')[:1200]) from exc
        proc.stdin.close()
        error = proc.stderr.read()
        code = proc.wait()
        require(code == 0, lambda: f'ffmpeg ({encoder}) failed for {v} (exit {code}): ' + error.decode(errors='replace')[:1200])
        decoded = subprocess.run([str(ffmpeg), '-v', 'error', '-i', str(out), '-f', 'null', '-'], capture_output=True)
        require(decoded.returncode == 0, lambda: f'{out.name} does not decode completely: ' + decoded.stderr.decode(errors='replace')[:1200])
        video_receipts[v] = dict(file=out.name, frames=288, fps=24, seconds=12, simulatedHours=24, sha256=sha(out), completeDecode=True, encoder=encoder)
        print('video', v, 'ok', flush=True)
    code_hashes = {p.name: sha(p) for p in (Path(__file__), Path(__file__).with_name('thermal_imaging.py'),
        Path(__file__).with_name('prepare_thermal_case.py'), Path(__file__).with_name('coupled_town_prepare.py'),
        Path(__file__).with_name('run_coupled_town.py'), Path(__file__).with_name('prepare_runway_day.py'),
        Path(__file__).with_name('case_seal.py'), Path(__file__).with_name('video_encoder.py'),
        Path(__file__).with_name('steps_manifest.py'), Path(__file__).with_name('integrity.py'))}
    native_receipt = read(src / 'native_runs.json')
    engine_sha = next(iter(native_receipt.get('runs', {}).values()), {}).get('engineSha256')
    write(root / 'render_receipt.json', dict(status='passed', displayLowK=low_k, displayHighK=high_k, views=views, hours=hours, stillHours=list(stills), videos=video_receipts,
        geometry={v: {k: c[k] for k in ('maxHitDistanceDifferenceM', 'farBoundaryPixels', 'skyPixels')} for v, c in cache.items()},
        intermediateFrames='linear radiance interpolation between 30 minute native nodes; not new physical solves',
        bandPoints=BAND_POINTS, irWidth=ir_width, irHeight=ir_height, stillLayout=dict(STILL_LAYOUT, panelWidth=width, panelHeight=height),
        nodeHours=NODE_SECONDS / 3600, nodesPerDay=case.per_day, videoFrames=288, videoFps=24, videoSeconds=12, adcBits=12,
        camera=f'{ir_width}x{ir_height}, 12 bit (codes linear in band radiance between the fixed bounds), PSF sigma 1.2 px, pixel averaging, generic uncalibrated noise',
        displayMapping='gray linear in 3-5 um equivalent brightness temperature between displayLowK and displayHighK, identical for every run/view/hour; *_enhanced stills stretch each picture P2-P98 and are not comparable between panels',
        farFieldState='pixels beyond the thermal ROI use the area-mean radiance of the solved far-ground facets of the same run and node that are not covered by the runway field or infill patch',
        exposedFarGroundAreaShare=float(case.areas[case.masks['far_ground_exposed']].sum() / case.areas[case.masks['far_ground']].sum()),
        rgbReference='static daylight display render of the same geometry; it is not an illumination-resolved night/day RGB product',
        caseSeal=CASE_SEAL if (src / CASE_SEAL).is_file() else None,
        renderSeal=RENDER_SEAL, nativeRunsSha256=sha(src / 'native_runs.json'), nativeEngineSha256=engine_sha,
        videoEncoder=dict(encoder=encoder, ffmpeg=str(ffmpeg)) if encoder else None,
        stepsVerification=case.steps.summary(),
        codeSha256=code_hashes))
    seal = write_seal(root, RENDER_SEAL, 'render')
    print(f'render seal: {len(seal["files"])} files', flush=True)


def periodic_closure(case, node_ids):
    """Same clock hour, consecutive days: how far from periodic is the output day?

    Facet-level maxima are dominated by a few shadow-boundary facets whose sunlit fraction flips because the sun moves
    ~0.2 deg between days, so the area-weighted mean, the 99th percentile, the area share above 1 K and the shift of the
    group MEAN temperature (what the day curves show) are reported next to the maximum.
    """
    first = case.first_output_node()
    out, attribution = {}, {}
    for r in RUNS:
        stats = {g: dict(maxAbsK=0., maxAbsHour=0., meanAbsK=0., p99AbsK=0., areaShareAbove1K=0., groupMeanShiftK=0., groupMeanShiftHour=0.) for g in case.masks}
        big_total, big_fraction_changed, pave_x, pave_y = 0, 0, [], []
        for k in range(case.per_day):
            now, before = case.records(r, first + k), case.records(r, first + k - case.per_day)
            diff = now[:, 0, 0].astype(float) - before[:, 0, 0]
            dfrac = now[:, 0, 8].astype(float) - before[:, 0, 8]
            big = np.abs(diff) > 1
            if case.weather[r][first + k, 5] > 0:   # attribution only at hours with sunshine; at night the sunlit fraction is irrelevant
                big_total += int(big.sum())
                big_fraction_changed += int((big & (np.abs(dfrac) > .05)).sum())
                pave = case.masks['runway_pavement'] & (np.abs(dfrac) > 1e-6)
                pave_x.append(dfrac[pave])
                pave_y.append(diff[pave])
            hour = (k + 1) * NODE_SECONDS / 3600
            for g, mask in case.masks.items():
                a = case.areas[mask]
                d = np.abs(diff[mask])
                cur = stats[g]
                if d.max() > cur['maxAbsK']:
                    cur.update(maxAbsK=float(d.max()), maxAbsHour=hour)
                cur['meanAbsK'] = max(cur['meanAbsK'], float(np.average(d, weights=a)))
                cur['p99AbsK'] = max(cur['p99AbsK'], float(np.percentile(d, 99)))
                cur['areaShareAbove1K'] = max(cur['areaShareAbove1K'], float(a[d > 1].sum() / a.sum()))
                shift = abs(float(np.average(now[mask, 0, 0], weights=a)) - float(np.average(before[mask, 0, 0], weights=a)))
                if shift > cur['groupMeanShiftK']:
                    cur.update(groupMeanShiftK=shift, groupMeanShiftHour=hour)
        out[r] = stats
        px, py = (np.concatenate(pave_x), np.concatenate(pave_y)) if pave_x else (np.zeros(0), np.zeros(0))
        attribution[r] = dict(facetsAbove1KOverDay=big_total, shareWithSunlitFractionChangeAbove0p05=(big_fraction_changed / big_total if big_total else None),
                              pavementSlopeKPerUnitSunlitFraction=(float(np.polyfit(px, py, 1)[0]) if len(px) > 10 else None),
                              pavementCorrelationWithSunlitFractionChange=(float(np.corrcoef(px, py)[0, 1]) if len(px) > 10 and px.std() > 0 and py.std() > 0 else None))
    return dict(attribution=attribution, attributionNote='evaluated only at hours with shortwave > 0; per-facet sunlit fraction is sampled; the sun moves ~0.05 deg in zenith and ~0.2 deg in azimuth from one calendar day to the next, which changes the sampled fraction of some facets, mostly near prop shadows',
                definition='temperature at the same clock hour on the output day minus the preceding day, side 0 of every facet, over all 48 nodes; each value is the worst over the day',
                spinupDays=case.days - 1, byRun=out)

def energy_summary(case, first):
    out = {}
    for r in RUNS:
        entries = {}
        for k in range(case.per_day):
            node = first + k
            d = case.diagnostic(r, node)
            for b in d['lumpedThermal']:
                e = entries.setdefault(b['id'], dict(inputEnergyJ=0., conductedToSkinJ=0., storageJ=0., coreResidualMaxW=0.))
                e['inputEnergyJ'] += b['inputPower_W'] * NODE_SECONDS
                e['conductedToSkinJ'] += b['conductedToSkin_W'] * NODE_SECONDS
                e['storageJ'] += b['storage_W'] * NODE_SECONDS
                e['coreResidualMaxW'] = max(e['coreResidualMaxW'], abs(b['coreResidual_W']))
        out[r] = entries
    return dict(note='output day only; core node of each lumped group: input = conducted to skin + core storage + residual', byRun=out)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('case', type=Path)
    p.add_argument('--ffmpeg', type=Path, required=True)
    p.add_argument('--video-views', nargs='*', default=['overview', 'vehicle_view'])
    p.add_argument('--closure-only', action='store_true', help='recompute closure.json from the solved nodes and exit (no imaging)')
    p.add_argument('--probe-nodes', type=int, default=0, help='test only: use the first 2*N native nodes as two short days')
    p.add_argument('--out-dir', type=Path, default=None, help='write products here instead of the case (required with --probe-nodes)')
    p.add_argument('--allow-unsealed', action='store_true', help='legacy cases only: check native receipts and source_inputs instead of case_seal.json')
    add_encoder_argument(p)
    p.add_argument('--verify-steps', choices=('lazy', 'full', 'off'), default='lazy',
                   help='per-node array/diagnostic SHA-256 check against steps_manifest.json: lazy = each file when first read, full = all files before rendering')
    a = p.parse_args()
    if a.closure_only:
        case = Case(a.case, a.verify_steps)
        verify_case(case, a.allow_unsealed)
        write(case.root / 'closure.json', periodic_closure(case, []))
        print('closure.json rewritten')
        return
    run(a.case, a.ffmpeg, a.video_views, [h for h in STILL_HOURS if not a.probe_nodes or h <= a.probe_nodes * .5] or [a.probe_nodes * .5], a.probe_nodes, a.out_dir, a.allow_unsealed,
        a.video_encoder, a.verify_steps)


if __name__ == '__main__':
    main()
