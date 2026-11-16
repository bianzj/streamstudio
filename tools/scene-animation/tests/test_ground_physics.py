"""Actual opaque shade + implicit GPU surface EB vs independent CPU bisection."""
import argparse,copy,hashlib,json,math,shutil,subprocess,sys
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from make_mesh_case import Obj


def main():
    p=argparse.ArgumentParser()
    for k in ['fixture','shader','output']:p.add_argument(k,type=Path)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
    for name,lo,hi,mat in [('floor',(-4,-4,-.5),(4,4,0),'ground'),('canopy',(-50,-50,3),(50,50,3.1),'canopy')]:
        obj=Obj();obj.box(lo,hi,mat);obj.save(a.output/(name+'.obj'))
    shared=dict(backend='mesh',schemaVersion=1,coordinates=dict(units='m',upAxis='Z',handedness='right'),width=64,height=64,samples=1,thermal=dict(wavelengthMicrometers=10,displayMinK=280,displayMaxK=330,skyK=275),materials=[dict(id=n,color=[.2,.2,.2],emissivity=1,temperatureK=300) for n in ['ground','canopy']],cameras=[dict(id='view',position=[0,-4,3],target=[0,0,0],up=[0,0,1],fovYDegrees=55)])
    for name in ['floor','canopy']:
        j=copy.deepcopy(shared);j['assets']=[dict(id=name,path=name+'.obj',sha256=sha(a.output/(name+'.obj')),units='m',upAxis='Z')];j['instances']=[dict(id=name,asset=name,transform=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1])];(a.output/(name+'.json')).write_text(json.dumps(j),encoding='utf-8')
    params=dict(kind='histream-ground-shadow-delta',schemaVersion=1,originMeters=[-1,-1],width=4,height=4,cellMeters=.5,maxReceiverZMeters=.6,initialK=300,deepK=300,stepSeconds=60,shadowIntervalSeconds=600,viewSamples=32,directFraction=.8,canopyMaterials=['canopy'],receiverMaterials=dict(ground=dict(albedo=.2,heatCapacity=60000,groundConductance=5)),sourceFiles=[])
    sun=[.2,.3,math.sqrt(.87)];lw=350;wind=2.5;air=300;dt=60;C=60000;k=5;sig=5.670374419e-8;h=5.8+4.1*math.sqrt(wind)
    def cpu(sw,lw):
        t=300
        for _ in range(60):
            lo,hi=220.,400.
            for _ in range(80):
                mid=(lo+hi)/2;res=.8*sw+lw-sig*mid**4-h*(mid-air)-k*(mid-300)-C*(mid-t)/dt
                if res>0:lo=mid
                else:hi=mid
            t=(lo+hi)/2
        return t
    forcing=a.output/'forcing.fixture';forcing.write_text('Synthetic constant forcing fixture',encoding='utf-8');params['sourceFiles']=[dict(role='fixtureForcing',path=str(forcing.resolve()),sha256=sha(forcing))];results={}
    for name,sw,canopies in [('day',800,['canopy']),('night',0,['canopy']),('no_canopy',800,[])]:
        j=copy.deepcopy(params);j['canopyMaterials']=canopies;j['history']=[dict(seconds=t,sun=sun,airK=air,wind=wind,shortwave=sw,longwave=lw) for t in [0,3600]]
        path=a.output/(name+'.json');path.write_text(json.dumps(j),encoding='utf-8');out=a.output/name
        background='canopy.json' if canopies else 'floor.json'
        r=subprocess.run([str(a.fixture.resolve()),str((a.output/'floor.json').resolve()),str((a.output/background).resolve()),str(path.resolve()),str(a.shader.resolve()),str(out.resolve())],capture_output=True);(a.output/(name+'.log')).write_bytes(r.stdout+r.stderr);assert r.returncode==0,(name,(r.stdout+r.stderr)[-2000:])
        cells=np.fromfile(out/'cells.f32',dtype='<f4').reshape(16,20);assert np.isfinite(cells).all() and np.all(cells[:,3]>=0)
        if canopies:
            expectedWithout=cpu(sw,lw);expectedWith=cpu(0,sig*300**4);expected=expectedWith-expectedWithout;error=float(np.max(np.abs(cells[:,18]-expected)));assert error<.002,(name,error,expected,cells[:,18])
            assert np.max(np.abs(cells[:,16]-expectedWithout))<.002 and np.max(np.abs(cells[:,17]-expectedWith))<.002,'Absolute temperature states differ from independent roots'
            assert np.all(cells[:,4]>.999) and np.all(cells[:,5]<.001),'Sky view geometry wrong'
            if name=='day':assert np.all(cells[:,12]>.99) and np.all(cells[:,13]<.01) and expected<-.5
            else:assert expected>.5
        else:expected=0.;error=float(np.max(np.abs(cells[:,18])));assert error==0,'No-canopy delta not exactly zero'
        results[name]=dict(expectedDeltaK=expected,maxGpuDeltaErrorK=error,maxResidualWm2=float(np.abs(cells[:,19]).max()))
    # Move the ground snapshot and remove its original forcing temporarily.
    moved=a.output/'relocated';shutil.copytree(a.output/'day',moved);held=forcing.with_suffix('.held');forcing.rename(held)
    try:
        replay=json.loads((moved/'replay-inputs.json').read_text())
        for s in replay['sourceFiles']:assert not Path(s['path']).is_absolute() and sha(moved/s['path'])==s['sha256']
        r=subprocess.run([str(a.fixture.resolve()),str((a.output/'floor.json').resolve()),str((a.output/'canopy.json').resolve()),str((moved/'replay-inputs.json').resolve()),str(a.shader.resolve()),str((a.output/'replayed').resolve())],capture_output=True)
        (a.output/'replay.log').write_bytes(r.stdout+r.stderr);assert r.returncode==0,(r.stdout+r.stderr)[-1500:]
        assert (a.output/'replayed/cells.f32').read_bytes()==(a.output/'day/cells.f32').read_bytes(),'Relocated ground replay differs'
    finally:held.rename(forcing)
    unsupported=json.loads((a.output/'floor.json').read_text());other=json.loads((a.output/'canopy.json').read_text());unsupported['assets']+=other['assets'];unsupported['instances']+=other['instances'];bad=a.output/'unsupported.json';bad.write_text(json.dumps(unsupported),encoding='utf-8')
    r=subprocess.run([str(a.fixture.resolve()),str(bad.resolve()),str((a.output/'canopy.json').resolve()),str((a.output/'day.json').resolve()),str(a.shader.resolve()),str((a.output/'unsupported').resolve())],capture_output=True);(a.output/'unsupported.log').write_bytes(r.stdout+r.stderr)
    assert r.returncode!=0 and b'canopy occluders must be supplemental' in r.stdout+r.stderr,'Missing base canopy guard'
    (a.output/'verification.json').write_text(json.dumps(dict(status='passed',cases=results,absoluteTemperaturesVerified=True,relocatedReplayExact=True,baseCanopyRejected=True,dayShadeCools=True,nightCanopyLongwaveWarms=True,noCanopyDeltaExactZero=True,fieldValidation=False),indent=2),encoding='utf-8');print(json.dumps(results,indent=2))


if __name__=='__main__':main()
