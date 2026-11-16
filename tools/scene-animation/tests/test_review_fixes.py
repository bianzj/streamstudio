"""Engine checks added after the 2026-11-12 review.

* rigid-v1 static collider: a non-box static part no longer turns the whole scene into a thin skin
  (layout "mixed"; the box parts keep their volume) and is reported in run-manifest warnings[];
* rigidRequireSolidStatic=true is enforced even when a recorded track would be replayed;
* the track cache reports writeOutcome before receipts and keeps an existing longer track;
* top-level physicsValidated/impactModel labels;
* authored fragmentExit plane (default output unchanged is covered by the old/new identity run);
* cameraObservation blastArrival=acoustic delays the shake by distance / sound speed.

usage: test_review_fixes.py ENGINE SEQUENCE OUTPUT
"""
import argparse, copy, hashlib, json, subprocess
from pathlib import Path

TETRA = 'mtllib tetra.mtl\nv 60 60 0\nv 61 60 0\nv 60 61 0\nv 60 60 1\nusemtl asphalt\nf 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n'


def main():
    p = argparse.ArgumentParser()
    for key in ['engine', 'sequence', 'output']:
        p.add_argument(key, type=Path)
    a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=False)
    b = json.loads(a.sequence.read_text(encoding='utf-8')); b.update(width=96, height=54)
    b['sequence'].update(fps=8, frameCount=24, firstFrame=1, lastFrame=24, impactTime=.5, exportFrames=[24])

    def run(name, data, ok=True, contains=''):
        src = a.output / (name + '.json'); src.write_text(json.dumps(data), encoding='utf-8'); out = a.output / name
        r = subprocess.run([str(a.engine.resolve()), 'eSceneAnimation', str(src), str(out)], capture_output=True)
        text = (r.stdout + r.stderr).decode(errors='replace'); (a.output / (name + '.log')).write_text(text, encoding='utf-8')
        assert (r.returncode == 0) == ok and contains.lower() in text.lower(), (name, r.returncode, text[-1500:])
        return out, text

    # --- labels on the canonical rigid scene (all closed boxes) ---------------------------------
    cache = (a.output / 'cache').resolve()
    rigid = copy.deepcopy(b); rigid['sequence']['rigidCache'] = dict(directory=str(cache))
    out, _ = run('canonical', rigid)
    m = json.loads((out / 'run-manifest.json').read_text())
    assert m['physicsValidated'] is False and m['impactModel']['kind'] == 'authored' and m['impactModel']['predictive'] is False
    assert m['warnings'] == [], m['warnings']
    s = m['timeline']['solver']
    assert s['staticCollider'] == 'closedConvexParts' and s['staticLayout']['orderedFastPath'] is True and s['staticLayout']['skinTriangles'] == 0
    assert s['trackCache']['mode'] == 'record' and s['trackCache']['writeOutcome'] == 'written', s['trackCache']
    out, _ = run('canonical_replay', rigid)
    s = json.loads((out / 'run-manifest.json').read_text())['timeline']['solver']
    assert s['trackCache']['mode'] == 'replay' and s['staticCollider'] == 'closedConvexParts'

    # --- one non-box static part: mixed layout, warning, solid parts retained -------------------
    tetra = a.output / 'tetra.obj'; tetra.write_text(TETRA, encoding='ascii')
    mixed = copy.deepcopy(rigid)
    mixed.pop('backgroundField', None)   # the FacetEB background binds the exact instance list; use prescribed material temperatures
    mixed['assets'].append(dict(id='tetra', path=str(tetra.resolve()), sha256=hashlib.sha256(tetra.read_bytes()).hexdigest(), units='m', upAxis='Z'))
    mixed['instances'].append(dict(id='tetra_0', asset='tetra', transform=[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]))
    out, text = run('mixed', mixed, contains='thin triangle-skin')
    m = json.loads((out / 'run-manifest.json').read_text()); s = m['timeline']['solver']
    assert s['staticCollider'] == 'mixed', s['staticCollider']
    assert s['staticLayout']['skinTriangles'] == 4, s['staticLayout']
    assert s['staticLayout']['convexParts'] >= 6000, 'box parts must keep their volume next to a non-box part'
    assert any('mixed' in w for w in m['warnings']), m['warnings']
    assert s['trackCache']['mode'] == 'record', 'a different static layout must not reuse the all-box track'
    assert m['timeline']['maxGroundPenetrationM'] < .02

    # --- requireSolidStatic: refused when recording AND when a recorded track exists ------------
    strict = copy.deepcopy(mixed); strict['sequence']['rigidRequireSolidStatic'] = True
    run('strict_replay', strict, ok=False, contains='rigidRequireSolidStatic')
    nocache = copy.deepcopy(strict); nocache['sequence'].pop('rigidCache')
    run('strict_record', nocache, ok=False, contains='rigidRequireSolidStatic')
    strict_ok = copy.deepcopy(rigid); strict_ok['sequence']['rigidRequireSolidStatic'] = True
    out, _ = run('strict_canonical', strict_ok)
    s = json.loads((out / 'run-manifest.json').read_text())['timeline']['solver']
    assert s['requireSolidStatic'] is True and s['trackCache']['mode'] == 'replay', 'the guard of this run is reported, not the recorded one'

    # --- a shorter recording never replaces a longer track --------------------------------------
    track = next(cache.glob('rigid_*.track')); size = track.stat().st_size
    short = copy.deepcopy(rigid); short['sequence']['rigidCache'] = dict(directory=str(cache)); short['sequence'].update(lastFrame=8, exportFrames=[8])
    out, _ = run('short_replay', short)
    s = json.loads((out / 'run-manifest.json').read_text())['timeline']['solver']
    assert s['trackCache']['mode'] == 'replay' and track.stat().st_size == size

    # --- authored fragmentExit and acoustic blast arrival ----------------------------------------
    authored = copy.deepcopy(b); authored['sequence']['motionBackend'] = 'authored'
    out, _ = run('authored_default', authored)
    t = json.loads((out / 'timeline.json').read_text())
    assert t['fragmentExit']['source'] == 'default -Y facade' and t['fragmentExit']['planeOffsetMeters'] == 2.8
    turned = copy.deepcopy(authored); turned['sequence']['fragmentExit'] = dict(normal=[1, 0], planeOffset=20)
    out, _ = run('authored_exit', turned)
    t = json.loads((out / 'timeline.json').read_text())
    assert t['fragmentExit']['source'] == 'supplied fragmentExit' and t['fragmentExit']['normal'] == [1.0, 0.0]
    for bad in (dict(normal=[0, 0], planeOffset=1), dict(normal=[1, 0]), dict(normal=[1], planeOffset=1), 'x'):
        broken = copy.deepcopy(authored); broken['sequence']['fragmentExit'] = bad
        run('exit_bad', broken, ok=False, contains='fragmentExit'); __import__('shutil').rmtree(a.output / 'exit_bad', ignore_errors=True)
    rigid_exit = copy.deepcopy(b); rigid_exit['sequence']['fragmentExit'] = dict(normal=[1, 0], planeOffset=2)
    run('rigid_exit', rigid_exit, ok=False, contains='fragmentExit')

    observe = copy.deepcopy(authored); observe['sequence'].update(optical=True, cameraObservation=dict(
        enabled=True, jitterOnImpact=True, impactShakeRadians=.004, blastArrival='acoustic', soundSpeedMetersPerSecond=340,
        ir=dict(width=48, height=27), rgb=dict(width=80, height=45)))
    observe.update(width=96, height=54)
    out, _ = run('acoustic', observe)
    motion = json.loads((out / 'run-manifest.json').read_text())['cameraObservation']['impactMotion']
    arrival = motion['arrival']
    assert motion['blastArrival'] == 'acoustic' and arrival['soundSpeedMetersPerSecond'] == 340 and arrival['cameraDistanceMeters'] > 1
    assert abs(motion['startSeconds'] - arrival['contactSeconds'] - arrival['cameraDistanceMeters'] / 340) < 1e-6
    bad = copy.deepcopy(observe); bad['sequence']['cameraObservation']['blastArrival'] = 'supersonic'
    run('acoustic_bad', bad, ok=False, contains='blastArrival')
    print('PASS: mixed static collider, requireSolidStatic on replay, cache write outcome, labels, fragmentExit, acoustic shake onset')


if __name__ == '__main__':
    main()
