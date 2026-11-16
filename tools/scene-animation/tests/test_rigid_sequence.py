"""Native geometric contact and rigid sequence integration."""
import argparse, copy, json, subprocess, csv
from pathlib import Path
import numpy as np


def main():
    p=argparse.ArgumentParser()
    for key in ['engine','sequence','output']:p.add_argument(key,type=Path)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    b=json.loads(a.sequence.read_text(encoding='utf-8'));b.update(width=192,height=108)
    b['sequence'].update(fps=8,frameCount=24,firstFrame=1,lastFrame=24,impactTime=.5,exportFrames=[1,6,24])
    def run(name,data,ok=True,contains=''):
        src=a.output/(name+'.json');src.write_text(json.dumps(data),encoding='utf-8');out=a.output/name
        r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(src),str(out)],capture_output=True)
        (a.output/(name+'.log')).write_bytes(r.stdout+r.stderr)
        assert (r.returncode==0)==ok and contains.lower() in (r.stdout+r.stderr).decode(errors='replace').lower(),(name,r.returncode,(r.stdout+r.stderr)[-1800:])
        return out
    out=run('full',b);m=json.loads((out/'run-manifest.json').read_text());t=m['timeline']
    assert t.get('motionBackend')=='rigid-v1','Missing actual rigid-body solver'
    assert t['firstContact']['computed'] is True and t['firstContact']['timeSeconds']>.5
    assert t['structuralResponseComputed'] is False and t['solver']['computeDevice']=='CPU'
    assert t['solver']['staticContactPointSamples']>0 and t['solver']['fragmentContactPointSamples']>0
    assert t['solver']['staticClosedConvexParts']>0,'Canonical static solids must retain volume'
    assert t['maxGroundPenetrationM']<.02,'Thin fragments must not tunnel through ground'
    assert t['solver']['maxContactPenetrationM']<.02,'Check every physics substep, not just exported video frames'
    rows=list(csv.DictReader((out/'rigid.csv').open()));assert len(rows)==24
    # Energy sanity: after release only gravity acts, so speed must stay near the free-fall bound.
    s=t['solver'];bound=t['freeFallSpeedBoundMetersPerSecond'];limit=t['speedGuardLimitMetersPerSecond']
    assert 5<bound<100 and abs(limit-1.25*bound)<1e-6,'Speed guard must follow the free-fall bound'
    assert s['maximumSpeedMPerSecond']<=limit*(1+1e-9),'Speed guard failed to bound fragment speed'
    assert s['rawPeakSpeedMPerSecond']>=s['maximumSpeedMPerSecond']>0 and s['staticCollider']=='closedConvexParts'
    assert s['speedGuardBodyCount']<=0.05*s['bodyCount'],'Speed guard acting on many bodies hides a solver problem'
    assert 'sliversUnder5cm' in t['fragmentCensus'] and t['outsideGroundCoverageSamples']>=0
    assert t['breach']['contactInsideAuthoredBreach'] is True,'Nominal flight must hit inside the authored breach'
    nominal=np.linalg.norm(np.array(b['sequence']['sourcePoint'])-b['sequence']['projectileStart'])/b['sequence']['impactTime']
    assert abs(t['flight']['speedMetersPerSecond']-nominal)<1e-3 and 'derived' in t['flight']['speedSource']
    # Explicit flight speed replaces impactTime; both together are ambiguous.
    fast=copy.deepcopy(b);fast['sequence'].pop('impactTime');fast['sequence']['flightSpeedMetersPerSecond']=20
    fo=run('speed',fast);ft=json.loads((fo/'timeline.json').read_text())
    assert abs(ft['flight']['speedMetersPerSecond']-20)<1e-3 and 'supplied' in ft['flight']['speedSource']
    assert abs(ft['firstContact']['requestedTimeSeconds']-nominal*b['sequence']['impactTime']/20)<1e-3,'Requested contact time must follow the supplied speed'
    both=copy.deepcopy(b);both['sequence']['flightSpeedMetersPerSecond']=20;run('speed_both',both,False,'mutually exclusive')
    slow=copy.deepcopy(fast);slow['sequence']['flightSpeedMetersPerSecond']=.5;run('speed_low',slow,False,'flightSpeedMetersPerSecond')
    bad=copy.deepcopy(b);bad['sequence']['rigidSpeedGuardFactor']=.5;run('guard_factor',bad,False,'rigidSpeedGuardFactor')
    bad=copy.deepcopy(b);bad['sequence']['rigidRequireSolidStatic']='yes';run('solid_type',bad,False,'rigidRequireSolidStatic')
    bad=copy.deepcopy(b);bad['sequence']['requireContactInBreach']='yes';run('breach_type',bad,False,'requireContactInBreach')
    assert t['effectOrientation']['outwardAzimuthRadians']==0 and t['fragmentRelease']['mode']=='table','Facade hit must keep the default effect direction and preset delays'
    # Optional disturbance-front release: delay grows with distance from the contact point.
    rel=copy.deepcopy(b);rel['sequence']['fragmentRelease']=dict(mode='distance',propagationMetersPerSecond=60,onsetSeconds=.02)
    ro=run('release',rel);rt=json.loads((ro/'timeline.json').read_text())['fragmentRelease']
    assert rt['mode']=='distance' and abs(rt['minimumDelaySeconds']-.02)<.03 and .02<rt['maximumDelaySeconds']<=1 and rt['maximumDelaySeconds']>rt['minimumDelaySeconds']+.02
    for name,value in [('release_mode',dict(mode='random',propagationMetersPerSecond=60)),('release_speed',dict(mode='distance',propagationMetersPerSecond=1)),('release_slow',dict(mode='distance',propagationMetersPerSecond=10,onsetSeconds=.5))]:
        bad=copy.deepcopy(b);bad['sequence']['fragmentRelease']=value;run(name,bad,False,'fragmentRelease')
    for n in [1,6,24]:
        values=np.fromfile(out/'radiance'/f'frame_{n:04d}.f32',dtype='<f4');assert np.isfinite(values).all() and values.min()>0
    # Rigid track cache: record once, replay with an identical hash. Everything the cache stores must match a live solve.
    cached=copy.deepcopy(b);cached['sequence']['rigidCache']=dict(directory=str((a.output/'track_cache').resolve()))
    rec=run('cache_record',cached);rep=run('cache_replay',cached)
    tr=json.loads((rec/'timeline.json').read_text())['solver']['trackCache'];tp=json.loads((rep/'timeline.json').read_text())['solver']['trackCache']
    assert tr['mode']=='record' and tp['mode']=='replay' and tr['key']==tp['key'] and len(tr['key'])==32,(tr,tp)
    assert (rec/'rigid.csv').read_bytes()==(rep/'rigid.csv').read_bytes(),'Replayed rigid statistics differ from the recording run'
    assert (out/'rigid.csv').read_bytes()==(rec/'rigid.csv').read_bytes(),'Recording changed the solver statistics'
    for n in [1,6,12,24]:
        name=f'frame_{n:04d}.png'
        assert (out/'frames'/name).read_bytes()==(rec/'frames'/name).read_bytes()==(rep/'frames'/name).read_bytes(),('cache changed frame',n)
    assert len(list((a.output/'track_cache').glob('rigid_*.track')))==1,'One physical input must map to exactly one track file'
    # Any change of the physical input must miss the cache; appearance-only changes must hit it.
    changed=copy.deepcopy(cached);changed['sequence']['rigidSpeedGuardFactor']=1.5;miss=run('cache_miss',changed)
    assert json.loads((miss/'timeline.json').read_text())['solver']['trackCache']['mode']=='record'
    look=copy.deepcopy(cached);look['sequence']['sensor']=False;look['sequence']['exportFrames']=[1];hit=run('cache_hit_look',look)
    assert json.loads((hit/'timeline.json').read_text())['solver']['trackCache']['mode']=='replay','Appearance-only change must reuse the physics track'
    # A truncated or corrupted track is ignored and re-recorded rather than replayed.
    track=next((a.output/'track_cache').glob('rigid_*.track'));data=track.read_bytes();track.write_bytes(data[:len(data)//2])
    healed=run('cache_healed',cached);assert json.loads((healed/'timeline.json').read_text())['solver']['trackCache']['mode']=='record'
    assert (healed/'rigid.csv').read_bytes()==(rec/'rigid.csv').read_bytes()
    bad=copy.deepcopy(b);bad['sequence']['rigidCache']=dict(directory=5);run('cache_type',bad,False,'rigidCache')
    # Threaded PNG encoding must not change a byte.
    sync=copy.deepcopy(b);sync['sequence']['outputThreads']=0;so=run('threads0',sync)
    for n in [1,12,24]:
        name=f'frame_{n:04d}.png'
        assert (out/'frames'/name).read_bytes()==(so/'frames'/name).read_bytes(),('thread count changed frame',n)
    bad=copy.deepcopy(b);bad['sequence']['outputThreads']=99;run('threads_bad',bad,False,'outputThreads')
    profile=m['profileSeconds'];assert profile['rigidSample']>=0 and 'pngSubmit' in profile and 'readbackTonemap' in profile
    seek=copy.deepcopy(b);seek['sequence'].update(firstFrame=24,lastFrame=24,exportFrames=[24]);s=run('seek',seek)
    assert (out/'frames/frame_0024.png').read_bytes()==(s/'frames/frame_0024.png').read_bytes()
    side=copy.deepcopy(b);side['sequence'].update(projectileStart=[-20,-2,13],sourcePoint=[10,-.35,12],impactTime=1,firstFrame=1,lastFrame=12,exportFrames=[1,12])
    sideout=run('side_contact',side);c=json.loads((sideout/'timeline.json').read_text())['firstContact']
    direction=np.array(side['sequence']['sourcePoint'])-side['sequence']['projectileStart'];direction/=np.linalg.norm(direction)
    assert np.linalg.norm(np.array(c['flightDirection'])-direction)<1e-6
    assert np.linalg.norm(np.cross(np.array(c['positionMeters'])-c['projectileOriginMeters'],direction))>.02,'Fixture must exercise side/fin contact'
    st=json.loads((sideout/'timeline.json').read_text());n=np.array(c['normal']);h=float(np.hypot(n[0],n[1]))
    expected=float(np.arctan2(n[0]/h,-n[1]/h)) if h>=.5 else 0.
    assert abs(st['effectOrientation']['outwardAzimuthRadians']-expected)<1e-6,'Effect direction must follow the horizontal contact normal'
    assert st['breach']['contactInsideAuthoredBreach'] in (True,False) and 'note' in st['breach']
    bad=copy.deepcopy(b);bad['sequence']['motionBackend']='unknown';run('bad_motion',bad,False,'motionBackend')
    run('full',b,False,'already exists')
    print('PASS: first geometric contact, rigid contacts, finite IR, deterministic seeking and input guards')


if __name__=='__main__':main()
