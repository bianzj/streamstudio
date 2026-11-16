"""Native GPU IR camera-display checks, independent of explosion appearance."""
import argparse,json,subprocess
from pathlib import Path
import numpy as np
from PIL import Image,ImageChops


def main():
    p=argparse.ArgumentParser();p.add_argument('engine',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=False)
    base=dict(width=192,height=108,fps=2,frameCount=9,impactTime=2,renderMode='thermal',
              thermalBandMode='lwir8_14',sceneStyle='detailed',thermalSensor=True,samples=1,
              sensorNoiseKelvin=0,sensorFixedPatternKelvin=0,exportRadianceFrames=[1,9])
    invalid=a.output/'invalid-export.json'
    invalid.write_text(json.dumps(dict(base,lastFrame=5)),encoding='utf-8')
    r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(invalid.resolve()),str((a.output/'invalid_export').resolve())],capture_output=True)
    assert r.returncode!=0 and b'exportRadianceFrames' in r.stderr,'radiance exports outside executed frames must fail'
    def run(name,**options):
        data=dict(base,**options);config=a.output/(name+'.json');config.write_text(json.dumps(data),encoding='utf-8')
        out=a.output/name
        r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(config.resolve()),str(out.resolve())],capture_output=True)
        (a.output/(name+'.log')).write_bytes(r.stdout+r.stderr)
        assert r.returncode==0,(name,r.stdout.decode(errors='replace'),r.stderr.decode(errors='replace'))
        return out,json.loads((out/'run-manifest.json').read_text(encoding='utf-8'))
    full,m=run('full')
    assert m.get('sensor',{}).get('enabled') is True,'native sensor pipeline is missing'
    assert m['sensor']['processingDomain']=='linear radiance before display mapping'
    assert 'AGC' in m['thermal']['displayStretch'] and m['thermal']['configuredDisplayBoundsUsed'] is False
    assert (full/'provenance'/'sensor.comp.spv').is_file()
    assert len(list((full/'frames').glob('*.png')))==9
    for n in [1,9]:
        raw=np.fromfile(full/'radiance'/f'frame_{n:04d}.f32',dtype='<f4')
        assert raw.size==192*108 and np.isfinite(raw).all() and raw.min()>=0
        with Image.open(full/'frames'/f'frame_{n:04d}.png') as im:
            r,g,b=im.convert('RGB').split()
            assert ImageChops.difference(r,g).getbbox() is None and ImageChops.difference(g,b).getbbox() is None
    seek,_=run('seek',firstFrame=9,lastFrame=9)
    assert (seek/'frames'/'frame_0009.png').read_bytes()==(full/'frames'/'frame_0009.png').read_bytes(),'AGC warmup must be reproducible'
    sharp,_=run('sharp',firstFrame=9,lastFrame=9,sensorBlurSigmaPixels=0,sensorBinning=1)
    noisy,_=run('noisy',firstFrame=9,lastFrame=9,sensorNoiseKelvin=.5,sensorFixedPatternKelvin=.2)
    def pixels(folder):
        with Image.open(folder/'frames'/'frame_0009.png') as image:return np.asarray(image.convert('L'),dtype=float)
    ref=pixels(full);clear=pixels(sharp);noise=pixels(noisy)
    assert np.mean(np.abs(ref-clear))>.1,'optical/detector filtering must affect edges'
    assert np.mean(np.abs(ref-noise))>.1,'configured noise must affect display'
    report=dict(status='passed',finiteRadiance=True,grayscale=True,warmedSeeking=True,
                blurMeanAbsDn=float(np.mean(np.abs(ref-clear))),noiseMeanAbsDn=float(np.mean(np.abs(ref-noise))),
                radiometricValidation=False)
    (a.output/'verification.json').write_text(json.dumps(report,indent=2),encoding='utf-8');print(json.dumps(report,indent=2))


if __name__=='__main__':main()
