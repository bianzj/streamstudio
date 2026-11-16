"""Engine checks for motionBackend impact-v1 and the S3/S4/S6 performance paths.

impact-v1 (empirical event model, generic example parameters):
* event.json receipt, impactModel kind/label, physicsValidated=false, frames.csv physical time column;
* conservation/consistency: fragment kinetic energy within the blast share, impulse balance by construction,
  released blocks = rigid bodies, heat closure of the injected heat share in the transport grid;
* monotonicity in the charge mass (failed blocks, fragment speed, fireball diameter, heat source);
* camera shake starts at the Kingery-Bulmash arrival time at the camera (blastArrival=blast);
* mutually exclusive inputs are refused.
Performance paths:
* S3 instancedGeometry renders the same images within a small tolerance (opt-in for authored);
* S4 additionalCameras: the primary camera output is byte-identical to a single-camera run;
* S6 checkpoints: a partial run resumed from a checkpoint is byte-identical to the full run.

usage: test_impact_v1.py ENGINE SEQUENCE OUTPUT   (SEQUENCE = rigid_collision_20261108/sequence.json)
"""
import argparse, copy, hashlib, json, shutil, subprocess
from pathlib import Path

import numpy as np
from PIL import Image

PHYSICS = dict(
    projectile=dict(massKg=250, diameterM=.34, lengthM=3.0, speedMetersPerSecond=250, noseCoefficientN=.85),
    charge=dict(massKg=100, tntEquivalence=1.0),
    fuze=dict(mode='delay', delaySeconds=.005),
    targetMaterial=dict(wall=dict(type='concrete', thicknessM=.3, youngS=.9, criticalImpulsePaS=2000)),
    fragments=dict(minimumMassKg=2, minimumThicknessM=.05),
    energySplit=dict(blast=.6, heat=.3),
    presentationTimeScale=.1)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


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
        if not ok:
            shutil.rmtree(out, ignore_errors=True)
        return out

    def impact(charge=100, **extra):
        d = copy.deepcopy(b); s = d['sequence']; s.pop('impactTime'); s['motionBackend'] = 'impact-v1'
        s['impactPhysics'] = copy.deepcopy(PHYSICS); s['impactPhysics']['charge']['massKg'] = charge; s.update(extra)
        return d

    # --- impact-v1 contract ------------------------------------------------------------------------
    out = run('impact', impact())
    m = json.loads((out / 'run-manifest.json').read_text()); e = json.loads((out / 'event.json').read_text())
    assert m['status'] == 'complete' and m['physicsValidated'] is False and m['impactModel']['kind'] == 'impact-v1'
    assert m['impactModel']['predictive'] is False and 'not independently validated' in m['impactModel']['label']
    assert m['impactModel']['presentation']['presentationTimeScale'] == .1 and abs(m['impactModel']['presentation']['equivalentSlowMotionFactor'] - 10) < 1e-9
    rows = (out / 'frames.csv').read_text().splitlines()
    assert rows[0] == 'frame,presentation_time_seconds,physical_time_seconds,png,triangles', rows[0]
    f24 = rows[-1].split(','); assert abs(float(f24[2]) - float(f24[1]) * .1) < 1e-9, f24
    t = m['timeline']; assert t['motionBackend'] == 'impact-v1'
    assert t['fragmentCount'] == e['failure']['releasedRigid'] and t['failedBlocks'] == e['failure']['failed'] > 0
    assert t['intactBlocks'] + t['failedBlocks'] == t['presetBlocks'] == len(e['blocks'])
    # projectile at the physical speed: first contact after path / speed
    tc = e['timeline']['physicalContactSeconds']; assert abs(e['timeline']['physicalBurstSeconds'] - tc - PHYSICS['fuze']['delaySeconds']) < 1e-12
    # energy / impulse bookkeeping
    fr, en = e['fragments'], e['energy']
    assert fr['kineticEnergyJ'] <= en['blastJ'] and fr['kineticWithinBlastBudget'] is True
    assert abs(en['chargeEnergyJ'] - 100 * 4.184e6) < 1 and abs(en['blastJ'] + en['heatToAirJ'] + en['unmodelledJ'] - en['chargeEnergyJ']) < 1e-3
    failed = [x for x in e['blocks'] if x['failed']]
    mv = sum(x['massKg'] * np.linalg.norm(x['launchVelocityMps']) for x in failed)
    assert abs(mv - fr['impulseBalanceNs']) <= 1e-6 * max(1, mv), (mv, fr['impulseBalanceNs'])
    for x in e['blocks']:
        if not x['failed']:
            assert x['launchVelocityMps'] == [0, 0, 0]
        if x['failed'] and not x['pulverised'] and x['criterion'].startswith('reflected'):
            assert x['reflectedImpulsePaS'] > PHYSICS['targetMaterial']['wall']['criticalImpulsePaS']
    heat = m['impactHeatClosure']; assert .2 < heat['ratio'] < 1.05, heat
    pt = m['projectile']['temperaturesK']; assert 300 < pt['body'] < 330 and pt['nose'] > pt['body'], pt   # 250 m/s, 288 K ambient
    assert (out / 'frames' / 'frame_0024.png').exists()

    # --- monotonicity in charge mass (event receipts) --------------------------------------------
    receipts = []
    for w in (10, 40, 160):
        o = run(f'charge_{w}', impact(w, firstFrame=1, lastFrame=1, exportFrames=[1], sensor=False))
        receipts.append(json.loads((o / 'event.json').read_text()))
    for lo, hi in zip(receipts, receipts[1:]):
        assert hi['failure']['failed'] >= lo['failure']['failed'], 'more charge must not fail fewer blocks'
        assert hi['fragments']['maxSpeedMps'] >= lo['fragments']['maxSpeedMps']
        assert hi['fireball']['diameterM'] > lo['fireball']['diameterM'] and hi['energy']['heatToAirJ'] > lo['energy']['heatToAirJ']
        for bl, bh in zip(lo['blocks'], hi['blocks']):
            assert bh['reflectedImpulsePaS'] >= bl['reflectedImpulsePaS'] * (1 - 1e-9) or bh['clamped'], 'per-block impulse grows with charge'

    # --- shake at blast arrival ---------------------------------------------------------------------
    obs = impact(optical=True, cameraObservation=dict(enabled=True, jitterOnImpact=True, impactShakeRadians=.004,
                                                     ir=dict(width=48, height=27), rgb=dict(width=80, height=45)))
    out = run('impact_observe', obs)
    m = json.loads((out / 'run-manifest.json').read_text()); motion = m['cameraObservation']['impactMotion']; arr = motion['arrival']
    burst = json.loads((out / 'event.json').read_text())['timeline']['physicalBurstSeconds']
    assert motion['blastArrival'] == 'blast' and arr['incidentOverpressurePa'] > 0 and 0 < arr['shakeAmplitudeScale'] <= 1
    assert abs(motion['startSeconds'] - burst - arr['arrivalSeconds']) < 1e-9 and 0 < arr['arrivalSeconds'] < arr['cameraDistanceMeters'] / 300
    assert abs(motion['impactShakeRadians'] - .004 * arr['shakeAmplitudeScale']) < 1e-12

    # --- refused inputs -------------------------------------------------------------------------------
    for key, value in (('impactTime', .5), ('flightSpeedMetersPerSecond', 200), ('fragmentRelease', dict(mode='distance', propagationMetersPerSecond=300))):
        bad = impact(); bad['sequence'][key] = value; run('bad_' + key, bad, ok=False, contains='mutually exclusive')
    bad = copy.deepcopy(b); bad['sequence']['impactPhysics'] = PHYSICS; run('bad_backend', bad, ok=False, contains='requires motionBackend')
    bad = impact(); bad['sequence']['impactPhysics']['charge']['massKg'] = -1; run('bad_charge', bad, ok=False, contains='charge.massKg')
    bad = impact(); bad['sequence']['impactPhysics']['warhead'] = {}; run('bad_field', bad, ok=False, contains='Unknown impactPhysics')
    bad = copy.deepcopy(b); bad['sequence'].update(optical=True, cameraObservation=dict(enabled=True, jitterOnImpact=True, blastArrival='blast', ir=dict(width=48, height=27), rgb=dict(width=80, height=45)))
    run('bad_blast_authored', bad, ok=False, contains='requires motionBackend impact-v1')
    print('PASS impact-v1: receipt, labels, physical time, energy/impulse budget, heat closure, charge monotonicity, blast-arrival shake, refused inputs')

    # --- S3 instanced geometry: same images within tolerance -------------------------------------
    authored = copy.deepcopy(b); authored['sequence']['motionBackend'] = 'authored'
    plain = run('authored_plain', authored)
    inst = copy.deepcopy(authored); inst['sequence']['instancedGeometry'] = True
    fast = run('authored_instanced', inst)
    assert 'instanced' in json.loads((fast / 'run-manifest.json').read_text())['geometryUpdate']
    worst = 0.
    for n in (1, 6, 12, 18, 24):
        x = np.asarray(Image.open(plain / 'fixed_frames' / f'frame_{n:04d}.png'), dtype=float)
        y = np.asarray(Image.open(fast / 'fixed_frames' / f'frame_{n:04d}.png'), dtype=float)
        worst = max(worst, np.abs(x - y).mean())
    assert worst < 1.0, f'instanced geometry differs by {worst:.3f} grey levels on average'
    print(f'PASS S3 instanced geometry: mean |diff| <= {worst:.4f} grey levels')

    # --- S4 multi-camera: primary output byte-identical ----------------------------------------------
    multi = copy.deepcopy(authored); multi['sequence']['additionalCameras'] = [dict(id='street', camera='street'), dict(id='roof', camera='roof', lastFrame=12)]
    both = run('authored_multi', multi)
    for folder in ('frames', 'fixed_frames'):
        for f in sorted((plain / folder).glob('*.png')):
            assert digest(f) == digest(both / folder / f.name), f'primary camera changed: {folder}/{f.name}'
    for name in ('frames.csv', 'transport.csv', 'sensor.csv'):
        assert digest(plain / name) == digest(both / name), name
    assert len(list((both / 'cameras' / 'street' / 'frames').glob('*.png'))) == 24
    assert len(list((both / 'cameras' / 'roof' / 'frames').glob('*.png'))) == 12
    single = copy.deepcopy(authored); single['sequence']['camera'] = 'street'
    alone = run('authored_street', single)
    for f in sorted((alone / 'fixed_frames').glob('*.png')):
        assert digest(f) == digest(both / 'cameras' / 'street' / 'fixed_frames' / f.name), f'additional camera differs from its own run: {f.name}'
    print('PASS S4 additionalCameras: primary and additional cameras byte-identical to single-camera runs')

    # --- S6 checkpoints: resumed partial run byte-identical ------------------------------------------
    cache = (a.output / 'checkpoints').resolve()
    ck = copy.deepcopy(obs); ck['sequence']['checkpoints'] = dict(directory=str(cache), everyFrames=8)
    full = run('ckpt_full', ck)
    assert json.loads((full / 'run-manifest.json').read_text())['checkpoints']['written'] == 2
    part = copy.deepcopy(ck); part['sequence'].update(firstFrame=19, exportFrames=[24])
    resumed = run('ckpt_resumed', part)
    c = json.loads((resumed / 'run-manifest.json').read_text())['checkpoints']; assert c['resumedFromFrame'] == 16, c
    for folder in ('frames', 'fixed_frames', 'optical_frames', 'observed_ir', 'observed_optical', 'detector_ir'):
        for n in range(19, 25):
            name = f'frame_{n:04d}.png'
            assert digest(full / folder / name) == digest(resumed / folder / name), f'resume differs: {folder}/{name}'
    assert not (resumed / 'frames' / 'frame_0018.png').exists()
    print('PASS S6 checkpoints: resumed frames 19-24 byte-identical (display, optical, observed IR/RGB, detector codes)')


if __name__ == '__main__':
    main()
