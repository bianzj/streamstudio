"""Distinct supplied fire profiles, denser smoke and contact-gated camera effects."""
import argparse,copy,csv,json,subprocess
from pathlib import Path
import numpy as np
from PIL import Image


def main():
    p=argparse.ArgumentParser()
    for k in ['engine','reference','output']:p.add_argument(k,type=Path)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    b=json.loads((a.reference/'tests_complete/natural.json').read_text());b['sequence']['appearanceVersion']='varied-v3'
    c=b['sequence']['cameraObservation'];c.update(jitterOnImpact=True,impactShakeRadians=.004,regionalBlur=dict(enabled=True,radiusNormalized=[.22,.32],irSigmaSourcePixels=2,rgbSigmaSourcePixels=2.5,irEdgeSigmaSourcePixels=.5,rgbEdgeSigmaSourcePixels=.8,onImpact=True))
    source=b['sequence']['sustainedFire']['sources'][0];source.update(flameStyle='window-licking',seed=12345,tracerRate=7)
    def run(name,j,ok=True,word=''):
        f=a.output/(name+'.json');f.write_text(json.dumps(j),encoding='utf-8');out=a.output/name;r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(f.resolve()),str(out.resolve())],capture_output=True);(a.output/(name+'.log')).write_bytes(r.stdout+r.stderr)
        assert (r.returncode==0)==ok and word.lower() in (r.stdout+r.stderr).decode(errors='replace').lower(),(name,r.returncode,(r.stdout+r.stderr)[-1200:]);return out
    out=run('varied',b);m=json.loads((out/'run-manifest.json').read_text());assert m.get('appearanceVersion')=='varied-v3','Missing varied-v3 appearance'
    assert m['sustainedFire']['sources'][0]['flameStyle']=='window-licking' and m['sustainedFire']['sources'][0]['seed']==12345
    rows=list(csv.DictReader((out/'observation.csv').open()));before=[r for r in rows if float(r['presentationTimeSeconds'])<=m['impactTime']];after=[r for r in rows if float(r['presentationTimeSeconds'])>m['impactTime']]
    assert before and after and all(float(r[k])==0 for r in before for k in ['jitterYawRadians','jitterPitchRadians','jitterRollRadians'])
    assert any(abs(float(r['jitterYawRadians']))>0 for r in after),'No post-contact motion'
    off=copy.deepcopy(b);off['sequence']['cameraObservation']['regionalBlur']['enabled']=False;no=run('no_region',off)
    for folder in ['frames','fixed_frames','optical_frames','radiance','geometry']:
        for f in (no/folder).glob('*'):assert f.read_bytes()==(out/folder/f.name).read_bytes(),('Observation changed canonical output',str(f))
    assert (no/'observed_optical/frame_0033.png').read_bytes()!=(out/'observed_optical/frame_0033.png').read_bytes(),'Regional blur inactive'
    low=copy.deepcopy(b);low['sequence']['sustainedFire']['sources'][0]['tracerRate']=1.8;lo=run('less_smoke',low)
    def stats(root):return list(csv.DictReader((root/'transport.csv').open()))[-1]
    assert float(stats(out)['smoke_integral'])>float(stats(lo)['smoke_integral'])+5,'Added smoke source not transported'
    profiles=[]
    for i,style in enumerate(['window-licking','window-billowing','ground-pool']):
        j=copy.deepcopy(b);j['sequence'].update(firstFrame=33,lastFrame=33,exportFrames=[33]);j['sequence']['sustainedFire']['sources'][0].update(flameStyle=style,seed=202611+i*101,tracerRate=0)
        root=run('profile_'+str(i),j);profiles.append(np.asarray(Image.open(root/'optical_frames/frame_0033.png').convert('RGB'),dtype=float))
    differences=[float(np.mean(np.abs(profiles[i]-profiles[j]))) for i,j in [(0,1),(0,2),(1,2)]];assert min(differences)>.015,('Profiles indistinguishable',differences)
    seek=copy.deepcopy(b);seek['sequence'].update(firstFrame=33,lastFrame=33,exportFrames=[33]);so=run('seek',seek)
    for folder in ['observed_ir','observed_optical','detector_ir']:assert (so/folder/'frame_0033.png').read_bytes()==(out/folder/'frame_0033.png').read_bytes()
    bad=copy.deepcopy(b);bad['sequence']['sustainedFire']['sources'][0]['flameStyle']='typo';run('bad_style',bad,False,'fire')
    bad=copy.deepcopy(b);bad['sequence']['cameraObservation']['regionalBlur']['rgbSigmaSourcePixels']=20;run('bad_region',bad,False,'rgbSigmaSourcePixels')
    (a.output/'verification.json').write_text(json.dumps(dict(status='passed',preContactJitterZero=True,postContactJitter=True,canonicalIsolation=True,profileMeanDifferences=differences,smokeIntegral=float(stats(out)['smoke_integral']),lowSmokeIntegral=float(stats(lo)['smoke_integral']),physicalCalibration=False),indent=2),encoding='utf-8')
    print('PASS: distinct fire profiles, transported smoke increase, impact-only shake, regional blur, canonical isolation and deterministic seek')


if __name__=='__main__':main()
