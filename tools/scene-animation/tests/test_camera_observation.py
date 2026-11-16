"""Native observation outputs, ideal-channel isolation and deterministic seeking."""
import argparse,copy,json,subprocess,csv
from pathlib import Path
import numpy as np
from PIL import Image

def main():
    p=argparse.ArgumentParser()
    for k in ['engine','sequence','output']:p.add_argument(k,type=Path)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);b=json.loads(a.sequence.read_text(encoding='utf-8'));b.update(width=192,height=108)
    b['sequence'].update(motionBackend='authored',fps=8,frameCount=12,firstFrame=1,lastFrame=12,impactTime=.5,exportFrames=[1,7,12])
    c=b['sequence']['cameraObservation'];c['ir'].update(width=64,height=36);c['rgb'].update(width=128,height=72)
    def run(name,data,ok=True,contains=''):
        src=a.output/(name+'.json');src.write_text(json.dumps(data),encoding='utf-8');out=a.output/name;r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(src),str(out)],capture_output=True);(a.output/(name+'.log')).write_bytes(r.stdout+r.stderr)
        assert (r.returncode==0)==ok and contains.lower() in (r.stdout+r.stderr).decode(errors='replace').lower(),(name,r.returncode,(r.stdout+r.stderr)[-1400:]);return out
    out=run('observed',b);m=json.loads((out/'run-manifest.json').read_text());assert m.get('cameraObservation',{}).get('enabled') is True,'Missing physical camera pipeline'
    for folder in ['observed_ir','observed_optical','detector_ir']:
        assert len(list((out/folder).glob('*.png')))==12
    with Image.open(out/'detector_ir/frame_0007.png') as im:codes=np.asarray(im);assert im.size==(64,36) and codes.max()<=16383
    off=copy.deepcopy(b);off['sequence']['cameraObservation']['enabled']=False;offout=run('ideal',off)
    for folder in ['frames','fixed_frames','radiance','geometry','optical_frames']:
        for f in (offout/folder).glob('*'):assert f.read_bytes()==(out/folder/f.name).read_bytes(),('Ideal channel changed',str(f))
    seek=copy.deepcopy(b);seek['sequence'].update(firstFrame=12,lastFrame=12,exportFrames=[12]);s=run('seek',seek)
    for folder in ['observed_ir','observed_optical','detector_ir']:assert (s/folder/'frame_0012.png').read_bytes()==(out/folder/'frame_0012.png').read_bytes()
    rows=list(csv.DictReader((out/'observation.csv').open()));assert len(rows)==12
    assert len({r['jitterYawRadians'] for r in rows})>1
    edge=copy.deepcopy(b);edge['sequence'].update(lastFrame=1,exportFrames=[1]);edge['sequence']['cameraObservation']['ir'].update(gainFpn=.1,responseSeconds=.1,apertureFraction=.1);run('legal_float_boundaries',edge)
    bad=copy.deepcopy(b);bad['sequence']['cameraObservation']['exposureSeconds']=1;run('bad_exposure',bad,False,'exposure')
    print('PASS: camera observations, detector codes, ideal-channel isolation, jitter, deterministic seek and exposure guard')

if __name__=='__main__':main()
