"""IR-only 2/98-percentile linear display; keep native ADC and RGB unchanged."""
import argparse,copy,json,subprocess
from pathlib import Path


def main():
    p=argparse.ArgumentParser()
    for k in ['engine','reference','output']:p.add_argument(k,type=Path)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    b=json.loads((a.reference/'tests_final/varied.json').read_text());b['sequence'].update(firstFrame=1,lastFrame=1,exportFrames=[1]);b['sequence']['cameraObservation']['irDisplay']='linear-percent-2'
    def run(name,j,ok=True):
        f=a.output/(name+'.json');f.write_text(json.dumps(j),encoding='utf-8');out=a.output/name
        r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(f),str(out)],capture_output=True);(a.output/(name+'.log')).write_bytes(r.stdout+r.stderr)
        assert (r.returncode==0)==ok,(name,r.returncode,(r.stdout+r.stderr)[-1400:]);return out
    new=run('percent2',b);m=json.loads((new/'run-manifest.json').read_text())
    assert m['cameraObservation'].get('irDisplay',{}).get('mode')=='linear-percent-2','Native 2% IR display missing'
    legacy=copy.deepcopy(b);legacy['sequence']['cameraObservation'].pop('irDisplay');old=run('legacy',legacy)
    for folder in ['frames','fixed_frames','optical_frames','radiance','geometry','observed_optical','detector_ir']:
        for f in (new/folder).glob('*'):
            if f.is_file():assert f.read_bytes()==(old/folder/f.name).read_bytes(),(folder,f.name)
    assert (new/'observed_ir/frame_0001.png').read_bytes()!=(old/'observed_ir/frame_0001.png').read_bytes(),'IR display unchanged'
    bad=copy.deepcopy(b);bad['sequence']['cameraObservation']['irDisplay']='typo';run('bad_display',bad,False)
    (a.output/'verification.json').write_text(json.dumps(dict(status='passed',onlyIrDisplayChanged=True,rawAdcExact=True,rgbExact=True,invalidModeRejected=True),indent=2),encoding='utf-8');print('PASS: native IR-only percent stretch, unchanged ADC/RGB and invalid mode guard')


if __name__=='__main__':main()
