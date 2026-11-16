"""Growing supplied flame size, source schedule, packet packing and legacy guard."""
import argparse,copy,csv,json,subprocess
from pathlib import Path
import numpy as np
from PIL import Image


def main():
    p=argparse.ArgumentParser()
    for k in ['engine','reference','output']:p.add_argument(k,type=Path)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    b=json.loads((a.reference.parent/'varied_fire_smoke_20261108/tests_final/varied.json').read_text());b['sequence'].update(firstFrame=1,lastFrame=33,exportFrames=[1,17,33])
    # Delay this isolated test fire until the shared impact flash has subsided.
    s=b['sequence']['sustainedFire']['sources'][0];s.update(growthSeconds=5,onsetSeconds=2.5,tracerRate=0,heatKelvinPerSecond=0)
    def run(name,j,ok=True):
        path=a.output/(name+'.json');path.write_text(json.dumps(j),encoding='utf-8');out=a.output/name
        r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(path),str(out)],capture_output=True);(a.output/(name+'.log')).write_bytes(r.stdout+r.stderr)
        assert (r.returncode==0)==ok,(name,r.returncode,(r.stdout+r.stderr)[-1600:]);return out
    out=run('growing',b);m=json.loads((out/'run-manifest.json').read_text());assert m['sustainedFire']['sources'][0].get('growthSeconds')==5,'Native fire growth missing'
    full=copy.deepcopy(b);full['sequence']['sustainedFire']['sources'][0].pop('growthSeconds');old=run('full_size',full)
    def orange(root,n):
        im=np.asarray(Image.open(root/f'optical_frames/frame_{n:04d}.png').convert('RGB'),dtype=float)
        return int(np.count_nonzero((im[:,:,0]>100)&(im[:,:,0]>im[:,:,1]*1.25)&(im[:,:,1]>im[:,:,2]*1.5)))
    early,late,adult,adult_late=orange(out,17),orange(out,33),orange(old,17),orange(old,33)
    assert early<adult*.8 and late>early*1.4 and adult_late>0 and late>=adult_late*.8,('Fire did not grow visibly',early,late,adult,adult_late)
    assert (out/'optical_frames/frame_0033.png').read_bytes()==(old/'optical_frames/frame_0033.png').read_bytes(),'Mature source does not converge to adult appearance'
    assert (out/'transport.csv').read_bytes()==(old/'transport.csv').read_bytes(),'Zero source rates changed transport'
    z=copy.deepcopy(full);z['sequence']['sustainedFire']['sources'][0]['growthSeconds']=0;z['sequence'].update(firstFrame=17,lastFrame=17,exportFrames=[17]);zero=run('zero_growth',z)
    for folder in ['observed_optical','observed_ir','detector_ir','optical_frames']:assert (zero/folder/'frame_0017.png').read_bytes()==(old/folder/'frame_0017.png').read_bytes(),folder
    supplied=copy.deepcopy(b);supplied['sequence']['sustainedFire']['sources'][0].update(tracerRate=7,heatKelvinPerSecond=140);small=run('growing_smoke',supplied)
    supplied['sequence']['sustainedFire']['sources'][0].pop('growthSeconds');large=run('full_smoke',supplied)
    def smoke(root):return list(csv.DictReader((root/'transport.csv').open()))
    assert float(smoke(small)[16]['smoke_integral'])<float(smoke(large)[16]['smoke_integral']),'Smoke source did not grow with fire'
    assert float(smoke(small)[16]['air_excess_heat_J'])<float(smoke(large)[16]['air_excess_heat_J']),'Heat source did not grow with fire'
    assert float(smoke(small)[-1]['smoke_integral'])>float(smoke(small)[16]['smoke_integral']),'Growing smoke did not accumulate'
    pack=copy.deepcopy(b);pack['sequence'].update(firstFrame=1,lastFrame=1,exportFrames=[1]);codes=[.1,2.5,5,12.8,6.5,9,20,25.5]
    pack['sequence']['sustainedFire']['sources']=[dict(copy.deepcopy(s),growthSeconds=v) for v in codes];po=run('eight_sources',pack)
    got=[s['growthSeconds'] for s in json.loads((po/'run-manifest.json').read_text())['sustainedFire']['sources']];assert np.allclose(got,codes),got
    for i,value in enumerate([-1,.01,26]):
        j=copy.deepcopy(pack);j['sequence']['sustainedFire']['sources'][0]['growthSeconds']=value;run('invalid_'+str(i),j,False)
    result=dict(status='passed',earlyOrangePixels=early,lateOrangePixels=late,adultEarlyOrangePixels=adult,adultLateOrangePixels=adult_late,matureCanonicalExact=True,zeroGrowthExact=True,transportGrowth=True,heatGrowth=True,eightSourceDurations=got,invalidDurationsRejected=True,combustionPrediction=False)
    (a.output/'verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print('PASS: growing flame area, transported source schedule, header packing, legacy zero and guards')


if __name__=='__main__':main()
