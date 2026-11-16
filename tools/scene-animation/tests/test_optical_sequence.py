"""Visible output, volume response and independent infrared regression checks."""
import argparse,copy,json,subprocess
from pathlib import Path
import numpy as np
from PIL import Image


def main():
    p=argparse.ArgumentParser()
    for name in ['engine','sequence','output']:p.add_argument(name,type=Path)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    b=json.loads(a.sequence.read_text(encoding='utf-8'));b.update(width=192,height=108)
    # Rendering fixture: no expensive rigid recalculation is needed for optics.
    b['sequence'].update(motionBackend='authored',fps=8,frameCount=16,firstFrame=1,lastFrame=16,impactTime=.5,exportFrames=[1,3,7,16],optical=True)
    def run(name,data,ok=True,contains=''):
        src=a.output/(name+'.json');src.write_text(json.dumps(data),encoding='utf-8');out=a.output/name
        r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(src),str(out)],capture_output=True)
        (a.output/(name+'.log')).write_bytes(r.stdout+r.stderr)
        assert (r.returncode==0)==ok and contains.lower() in (r.stdout+r.stderr).decode(errors='replace').lower(),(name,r.returncode,(r.stdout+r.stderr)[-1600:])
        return out
    out=run('optical',b);m=json.loads((out/'run-manifest.json').read_text())
    assert m.get('optical',{}).get('enabled') is True,'Missing visible sequence output'
    assert len(list((out/'optical_frames').glob('*.png')))==16
    def rgb(folder,n):return np.asarray(Image.open(folder/f'optical_frames/frame_{n:04d}.png').convert('RGB'),dtype=float)
    image=rgb(out,7);assert image.shape==(108,192,3) and np.max(np.abs(image[:,:,0]-image[:,:,2]))>30
    off=copy.deepcopy(b);off['sequence']['optical']=False;ir=run('infrared',off)
    assert not (ir/'optical_frames').exists()
    for kind in ['frames','fixed_frames','radiance','geometry']:
        for f in (ir/kind).glob('*'):assert f.read_bytes()==(out/kind/f.name).read_bytes(),('Optics changed IR/geometry',str(f))
    nomedia=copy.deepcopy(b);nomedia['sequence']['media']=False;n=run('no_media',nomedia)
    assert np.count_nonzero(np.max(np.abs(image-rgb(n,7)),axis=-1)>10)>30,'Visible fire/smoke must affect RGB pixels'
    seek=copy.deepcopy(b);seek['sequence'].update(firstFrame=16,lastFrame=16,exportFrames=[16]);s=run('seek',seek)
    assert (out/'optical_frames/frame_0016.png').read_bytes()==(s/'optical_frames/frame_0016.png').read_bytes()
    bad=copy.deepcopy(b);bad['sequence']['optical']='yes';run('bad_optical',bad,False,'optical')
    print('PASS: RGB frames, visible media response, exact IR/geometry preservation, deterministic seek and boolean guard')


if __name__=='__main__':main()
