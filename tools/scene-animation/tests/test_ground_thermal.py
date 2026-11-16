"""Ground shade must change physical IR surfaceK while preserving RGB/geometry."""
import argparse,copy,hashlib,json,subprocess
from pathlib import Path
import numpy as np
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser()
    for k in ['engine','case','output']:p.add_argument(k,type=Path)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);scene=json.loads((a.case/'sequence.json').read_text());scene.update(width=192,height=108,samples=1)
    scene['sequence'].update(firstFrame=1,lastFrame=1,exportFrames=[1]);c=scene['sequence']['cameraObservation'];c['ir'].update(width=64,height=36);c['rgb'].update(width=192,height=108)
    ref=scene['sequence']['groundThermal'];params=json.loads(Path(ref['path']).read_text());params.update(originMeters=[-45,-40],width=100,height=100,cellMeters=1,viewSamples=16)
    h=params['history'][-1];params['history']=[dict(h,seconds=0),dict(h,seconds=3600)];f=a.output/'ground.json';f.write_text(json.dumps(params),encoding='utf-8');ref.update(path=str(f.resolve()),sha256=sha(f))
    def run(name,j,ok=True):
        path=a.output/(name+'.json');path.write_text(json.dumps(j),encoding='utf-8');out=a.output/name;r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(path),str(out)],capture_output=True);(a.output/(name+'.log')).write_bytes(r.stdout+r.stderr)
        assert (r.returncode==0)==ok,(name,r.returncode,(r.stdout+r.stderr)[-1800:]);return out
    on=run('shade',scene);m=json.loads((on/'run-manifest.json').read_text());assert m.get('groundThermal',{}).get('enabled'),'Ground thermal coupling missing'
    off=copy.deepcopy(scene);off['sequence']['groundThermal']['enabled']=False;baseline=run('off',off)
    for folder in ['optical_frames','observed_optical','geometry']:
        for f in (baseline/folder).glob('*'):assert f.read_bytes()==(on/folder/f.name).read_bytes(),('Ground EB changed RGB/geometry',str(f))
    x=np.fromfile(on/'radiance/frame_0001_surfaceK.f32',dtype='<f4');y=np.fromfile(baseline/'radiance/frame_0001_surfaceK.f32',dtype='<f4');delta=x-y
    assert np.isfinite(delta).all() and (np.abs(delta)>.05).sum()>5,('No physical ground temperature change',delta.min())
    assert m['groundThermal']['minDeltaK']<-.1,'Paired canopy control did not cool in daylight'
    assert (on/'observed_ir/frame_0001.png').read_bytes()!=(baseline/'observed_ir/frame_0001.png').read_bytes()
    bad=copy.deepcopy(scene);bad['sequence']['groundThermal']['sha256']='0'*64;run('bad_hash',bad,False)
    receipt=dict(status='passed',rgbExact=True,geometryAppearanceExact=True,minSurfaceDeltaK=float(delta.min()),changedSurfacePixels=int((np.abs(delta)>.05).sum()),observedIrChanged=True,invalidHashRejected=True,fieldValidation=False)
    (a.output/'verification.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8');print(json.dumps(receipt))


if __name__=='__main__':main()
