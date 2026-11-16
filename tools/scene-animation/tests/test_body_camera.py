"""Catch fixed/mis-parented rays, shutter-time drift and invalid mount inputs."""
import argparse,copy,csv,json,subprocess
from pathlib import Path
import numpy as np

def read(p):return json.loads(p.read_text(encoding='utf-8'))
def main():
    p=argparse.ArgumentParser()
    for k in ['engine','reference','output']:p.add_argument(k,type=Path)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    j=read(a.reference/'sequence.json');j.update(width=192,height=108,samples=1)
    s=j['sequence'];s.pop('groundThermal');s.update(firstFrame=1,lastFrame=2,exportFrames=[1,2],cameraMount=dict(body='projectile',offsetMeters=[.4,.3,.025]))
    o=s['cameraObservation'];o['ir'].update(width=64,height=36);o['rgb'].update(width=192,height=108)
    def run(name,config,ok=True):
        src=a.output/(name+'.json');src.write_text(json.dumps(config),encoding='utf-8');out=a.output/name
        r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(src),str(out)],capture_output=True)
        (a.output/(name+'.log')).write_bytes(r.stdout+r.stderr)
        assert (r.returncode==0)==ok,(name,r.returncode,(r.stdout+r.stderr)[-1000:])
        return out
    out=run('mounted',j);m=read(out/'run-manifest.json')
    assert m.get('cameraMount',{}).get('body')=='projectile','Native camera mount missing/ignored'
    poses=list(csv.DictReader((out/'camera.csv').open()))
    assert len(poses)==6 and sum(int(x['canonical']) for x in poses)==2
    contact=read(a.reference/'result_final/run-manifest.json')['timeline']['firstContact']
    start=np.array(s['projectileStart']);end=np.array(contact['projectileOriginMeters']);impact=m['impactTime'];f=(end-start)/np.linalg.norm(end-start);right=np.cross(f,[0,0,1]);right/=np.linalg.norm(right);up=np.cross(right,f);offset=right*.4+up*.3+f*.025
    expected=lambda t:start+(end-start)*(t/impact)+offset
    for row in poses:
        t=float(row['sampleTimeSeconds']);frame=int(row['frame']);presentation=(frame-1)/24
        # CSV uses10significant digits; 1/24 rounds upward by3.3e-12s.
        assert -1e-9<=t<=presentation+1e-9 and presentation-t<=o['exposureSeconds']+1e-8
        assert np.allclose([float(row[k]) for k in ['x','y','z']],expected(t),atol=1e-5)
        assert np.allclose([float(row['forward'+k]) for k in 'XYZ'],f,atol=1e-6)
        assert np.allclose([float(row['up'+k]) for k in 'XYZ'],up,atol=1e-6)
    canonical=[row for row in poses if int(row['canonical'])]
    assert np.linalg.norm([float(canonical[1][k])-float(canonical[0][k]) for k in ['x','y','z']])>.3,'Mounted camera remained static'
    # Independent static camera at t=1/24 must yield the identical actual rays.
    fixed=copy.deepcopy(j);fixed['sequence'].pop('cameraMount');fixed['sequence']['camera']='known_pose'
    pos=expected(1/24);fov=next(c['fovYDegrees'] for c in j['cameras'] if c['id']==s['camera'])
    fixed['cameras'].append(dict(id='known_pose',position=pos.tolist(),target=(pos+f*4).tolist(),up=up.tolist(),fovYDegrees=fov))
    check=run('static_equivalent',fixed)
    # Finite-float camera matrices may differ by a few ulps: compare radiance,
    # surfaceK and rendered pixels, rather than a textual pose declaration.
    from PIL import Image
    for folder in ['optical_frames','fixed_frames','geometry']:
        x=np.asarray(Image.open(out/folder/'frame_0002.png')).astype(float);y=np.asarray(Image.open(check/folder/'frame_0002.png')).astype(float)
        assert np.mean(np.abs(x-y))<.1 and np.quantile(np.abs(x-y),.999)<3,(folder,np.abs(x-y).max())
    for name in ['frame_0002.f32','frame_0002_surfaceK.f32']:
        x=np.fromfile(out/'radiance'/name,dtype='<f4');y=np.fromfile(check/'radiance'/name,dtype='<f4');assert np.mean(np.abs(x-y))<.002
    invalid=[]
    for name,patch in [('body',dict(cameraMount=dict(body='unknown',offsetMeters=[0,0,0]))),('offset',dict(cameraMount=dict(body='projectile',offsetMeters=[4,0,0]))),('conflict',dict(trackingCameraOffset=[4,0,0])),('contact',dict(lastFrame=75))]:
        bad=copy.deepcopy(j);bad['sequence'].update(patch);run('invalid_'+name,bad,False);invalid.append(name)
    result=dict(status='passed',poseSamples=6,independentPoseVerified=True,staticEquivalentRaysVerified=True,invalidMountsRejected=invalid,scope='Camera parenting and exposure timestamps only; original motion unchanged')
    (a.output/'verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result))
if __name__=='__main__':main()
