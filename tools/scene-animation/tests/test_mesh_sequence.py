"""Small native sequence checks; full video is verified separately."""
import argparse,json,subprocess,copy,csv
from pathlib import Path
import numpy as np
from PIL import Image


def main():
    p=argparse.ArgumentParser();p.add_argument('engine',type=Path);p.add_argument('sequence',type=Path);p.add_argument('output',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    base=json.loads(a.sequence.read_text(encoding='utf-8'));base.update(width=160,height=96)
    base['sequence'].update(fps=4,frameCount=12,impactTime=.5,exportFrames=[1,4,12],sensor=True)
    def run(name,data,ok=True,contains=''):
        source=a.output/(name+'.json');source.write_text(json.dumps(data),encoding='utf-8');out=a.output/name
        r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(source.resolve()),str(out.resolve())],capture_output=True);text=(r.stdout+r.stderr).decode(errors='replace');(a.output/(name+'.log')).write_text(text,encoding='utf-8')
        assert (r.returncode==0)==ok and contains.lower() in text.lower(),(name,r.returncode,text[-1400:]);return out
    out=run('full',base);m=json.loads((out/'run-manifest.json').read_text());assert m['backend']=='meshSequence' and m['status']=='complete'
    assert len(list((out/'frames').glob('*.png')))==12 and len(list((out/'fixed_frames').glob('*.png')))==12
    for n in [1,4,12]:
        f=np.fromfile(out/'radiance'/f'frame_{n:04d}.f32',dtype='<f4');assert f.size==160*96 and np.isfinite(f).all() and f.min()>0
    with Image.open(out/'frames/frame_0012.png') as im:assert im.size==(160,96)
    assert m['timeline']['fragmentCount']>0 and m['timeline']['structuralResponseComputed'] is False
    receipt=json.loads((out/'timeline.json').read_text());assert receipt['maxGroundPenetrationM']<1e-4
    assert receipt['settledOverlapPairs']==0 and receipt['maxUnsupportedRestGapM']<1e-4
    diagnostics={int(float(row['frame'])):row for row in csv.DictReader((out/'transport.csv').open())}
    for n in [4,12]:
        surface=np.fromfile(out/'radiance'/f'frame_{n:04d}_surfaceK.f32',dtype='<f4')
        limit=m['backgroundField']['temperatureMaxK']+max(100,float(diagnostics[n]['surface_max_K'])-300)
        assert surface.max()<=limit+1e-3,'Hot air must not be sampled as wall-layer temperature'
    seek=copy.deepcopy(base);seek['sequence'].update(firstFrame=12,lastFrame=12,exportFrames=[12]);s=run('seek',seek)
    assert (s/'frames/frame_0012.png').read_bytes()==(out/'frames/frame_0012.png').read_bytes()
    unaligned=copy.deepcopy(base);unaligned['sequence'].update(impactTime=.51,frameCount=8,exportFrames=[1,8]);run('unaligned',unaligned)
    boundary=copy.deepcopy(base);boundary['sequence'].update(fps=60,impactTime=.05,frameCount=8,exportFrames=[1,8]);run('float_boundary',boundary)
    bad=copy.deepcopy(base);bad['sequence']['fps']=0;run('bad_fps',bad,False,'fps')
    bad=copy.deepcopy(base);bad['sequence']['remainder']['sha256']='0'*64;run('bad_hash',bad,False,'sha256')
    run('full',base,False,'already exists')
    print('PASS: sequence frames, radiance, geometry/contact receipt, deterministic warm seeking, input and no-overwrite guards')


if __name__=='__main__':main()
