"""Flight tracer, body contrast, plume ablation and deterministic seek checks."""
import argparse, copy, csv, json, subprocess
from pathlib import Path
import numpy as np


def main():
    p = argparse.ArgumentParser()
    for name in ('engine', 'sequence', 'output'):
        p.add_argument(name, type=Path)
    a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=False)
    base = json.loads(a.sequence.read_text(encoding='utf-8'))
    base.update(width=320, height=180)
    base['sequence'].update(fps=8, frameCount=12, firstFrame=1, lastFrame=12,
                            impactTime=1, exportFrames=[1, 5, 12],
                            trackingCameraOffset=[-6, -8, 3])
    def run(name, data):
        source = a.output / (name + '.json'); out = a.output / name
        source.write_text(json.dumps(data), encoding='utf-8')
        r = subprocess.run([str(a.engine.resolve()), 'eSceneAnimation', str(source), str(out)], capture_output=True)
        (a.output / (name + '.log')).write_bytes(r.stdout + r.stderr)
        assert r.returncode == 0, (name, (r.stdout+r.stderr)[-1500:])
        return out
    out = run('full', base)
    rows = list(csv.DictReader((out/'transport.csv').open()))
    assert all(float(r['smoke_integral']) > 0 for r in rows[:8]), 'Missing persistent pre-impact tracer'
    assert float(rows[6]['smoke_integral']) > float(rows[0]['smoke_integral']), 'Wake must accumulate'
    assert all(float(r['invalid_cells']) == 0 for r in rows)
    surface = np.fromfile(out/'radiance/frame_0005_surfaceK.f32', dtype='<f4')
    assert ((surface >= 328) & (surface <= 345)).sum() >= 12, 'Body must have a separate cooler silhouette'
    assert ((surface >= 590) & (surface <= 610)).sum() > 0, 'Nozzle missing'
    seek = copy.deepcopy(base); seek['sequence'].update(firstFrame=5, lastFrame=5, exportFrames=[5])
    s = run('seek', seek)
    assert (out/'frames/frame_0005.png').read_bytes() == (s/'frames/frame_0005.png').read_bytes()
    off = copy.deepcopy(seek); off['sequence']['media'] = False
    n = run('no_media', off)
    on_r = np.fromfile(s/'radiance/frame_0005.f32', dtype='<f4')
    off_r = np.fromfile(n/'radiance/frame_0005.f32', dtype='<f4')
    assert np.isfinite(on_r).all() and on_r.min() > 0
    assert np.count_nonzero(np.abs(on_r-off_r) > .05) > 100, 'Plume must change actual radiance'
    # Exclude all rays within 1.5 m of the entire analytic jet (radius <= .185 m).
    # A residual volume signal here must come from the transported smoke.
    seq = base['sequence']; start = np.array(seq['projectileStart']); end = np.array(seq['sourcePoint'])
    direction = (end-start)/np.linalg.norm(end-start); pos = (start+end)/2
    camera = pos+np.array(seq['trackingCameraOffset']); forward = pos-direction*1.5-camera
    forward /= np.linalg.norm(forward); right = np.cross(forward, [0, 0, 1]); right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    x, y = np.meshgrid((np.arange(320)+.5)/320*2-1, 1-(np.arange(180)+.5)/180*2)
    rays = forward+x[..., None]*right*(320/180)*np.tan(np.deg2rad(17.5))+y[..., None]*up*np.tan(np.deg2rad(17.5))
    rays /= np.linalg.norm(rays, axis=-1)[..., None]; distance = np.full((180, 320), np.inf)
    for length in np.linspace(0, 2.1, 40):
        q = pos-direction*(2.7+length)-camera
        t = np.maximum(0, np.sum(rays*q, axis=-1))
        distance = np.minimum(distance, np.linalg.norm(q-t[..., None]*rays, axis=-1))
    delta = np.abs(on_r-off_r).reshape(180, 320)
    assert np.count_nonzero((distance > 1.5) & (delta > .02)) > 500, 'Transported smoke must affect radiance beyond the jet'
    print('PASS: persistent flight tracer, distinct body/nozzle, volume radiance and deterministic seek')


if __name__ == '__main__': main()
