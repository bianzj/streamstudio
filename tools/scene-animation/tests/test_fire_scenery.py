"""Persistent supplied fire, independent scenery, provenance and default isolation."""
import argparse, copy, csv, hashlib, json, subprocess
from pathlib import Path
import numpy as np
from PIL import Image
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from make_mesh_case import Obj


def main():
    p=argparse.ArgumentParser()
    for k in ['engine','sequence','output']:p.add_argument(k,type=Path)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    b=json.loads(a.sequence.read_text(encoding='utf-8'));b.update(width=192,height=108)
    b['sequence'].update(motionBackend='authored',fps=4,frameCount=33,firstFrame=1,lastFrame=33,impactTime=.5,exportFrames=[1,3,17,33])
    c=b['sequence']['cameraObservation'];c['ir'].update(width=64,height=36);c['rgb'].update(width=128,height=72)
    tree=Obj();tree.box((-3,-4,.2),(-2.6,-3.6,7),'bark');tree.box((-4.5,-5.5,4),(-1,-2,8),'leaves');tree.save(a.output/'tree.obj')
    scenery=copy.deepcopy(b)
    for key in ['sequence','backgroundField','thermalPerturbations']:scenery.pop(key,None)
    scenery['materials']=[dict(id='bark',color=[.2,.09,.035],emissivity=.96,temperatureK=301),dict(id='leaves',color=[.08,.28,.04],emissivity=.98,temperatureK=296.5)]
    scenery['assets']=[dict(id='tree',path='tree.obj',sha256=hashlib.sha256((a.output/'tree.obj').read_bytes()).hexdigest(),upAxis='Z',units='m')]
    scenery['instances']=[dict(id='tree',asset='tree',transform=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1])]
    sf=a.output/'scenery.json';sf.write_text(json.dumps(scenery),encoding='utf-8')
    b['sequence']['scenery']=dict(enabled=True,path=str(sf.resolve()),sha256=hashlib.sha256(sf.read_bytes()).hexdigest())
    b['sequence']['sustainedFire']=dict(enabled=True,sources=[dict(offsetMeters=[0,-1.2,-1.2],radiusMeters=1.2,heightMeters=3.5,onsetSeconds=.3,durationSeconds=15,fadeSeconds=2,temperatureK=1120,heatKelvinPerSecond=140,tracerRate=2)])
    def run(name,data,ok=True,contains=''):
        src=a.output/(name+'.json');src.write_text(json.dumps(data),encoding='utf-8');out=a.output/name
        r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(src.resolve()),str(out.resolve())],capture_output=True)
        (a.output/(name+'.log')).write_bytes(r.stdout+r.stderr)
        assert (r.returncode==0)==ok and contains.lower() in (r.stdout+r.stderr).decode(errors='replace').lower(),(name,r.returncode,(r.stdout+r.stderr)[-1400:]);return out
    out=run('fire_trees',b);m=json.loads((out/'run-manifest.json').read_text())
    assert m.get('sustainedFire',{}).get('enabled') is True,'Missing sustained fire model'
    assert m.get('scenery',{}).get('temperatureSource')=='prescribed supplemental materials','Tree temperatures must not be relabeled FacetEB'
    assert m['scenery']['triangleCount']==24 and m['backgroundField']['coverageFraction']==1
    off=copy.deepcopy(b);off['sequence']['sustainedFire']['enabled']=False;no=run('no_fire',off)
    for folder in ['frames','fixed_frames','optical_frames','observed_ir','observed_optical','detector_ir']:
        assert (out/folder/'frame_0001.png').read_bytes()==(no/folder/'frame_0001.png').read_bytes(),('Fire before ignition',folder)
    def rgb(folder,n):return np.asarray(Image.open(folder/f'optical_frames/frame_{n:04d}.png').convert('RGB'),dtype=float)
    hot=rgb(out,33);cold=rgb(no,33)
    def orange(im):return (im[:,:,0]>im[:,:,1]*1.15+15)&(im[:,:,1]>im[:,:,2]+5)
    assert np.count_nonzero(orange(hot))>=np.count_nonzero(orange(cold))+8,'Persistent flame missing at8s'
    raw=np.fromfile(out/'radiance/frame_0033.f32',dtype='<f4');baseline=np.fromfile(no/'radiance/frame_0033.f32',dtype='<f4')
    assert np.isfinite(raw).all() and np.mean(np.abs(raw-baseline))>1e-4,'Fire not represented in linear IR'
    def last_stats(folder):return list(csv.DictReader((folder/'transport.csv').open()))[-1]
    assert float(last_stats(out)['smoke_integral'])>float(last_stats(no)['smoke_integral']),'Persistent smoke not transported'
    bare=copy.deepcopy(off);bare['sequence']['scenery']['enabled']=False;empty=run('no_trees',bare)
    assert np.count_nonzero(np.max(np.abs(rgb(no,1)-rgb(empty,1)),axis=-1)>10)>10,'Scenery not visible'
    temp=np.fromfile(no/'radiance/frame_0001_surfaceK.f32',dtype='<f4');assert np.count_nonzero(np.abs(temp-296.5)<1e-4)>10,'Supplemental tree temperature lost'
    seek=copy.deepcopy(b);seek['sequence'].update(firstFrame=33,lastFrame=33,exportFrames=[33]);so=run('seek',seek)
    assert (out/'observed_ir/frame_0033.png').read_bytes()==(so/'observed_ir/frame_0033.png').read_bytes()
    replay=json.loads((out/'provenance/replay.json').read_text());replay['sequence'].update(firstFrame=33,lastFrame=33,exportFrames=[33])
    # Resolve the portable snapshot through its own directory, without original scenery paths.
    rp=out/'provenance/replay_test.json';rp.write_text(json.dumps(replay),encoding='utf-8');ro=a.output/'replay';r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(rp.resolve()),str(ro.resolve())],capture_output=True);(a.output/'replay.log').write_bytes(r.stdout+r.stderr);assert r.returncode==0,(r.stdout+r.stderr)[-1400:]
    assert (out/'observed_optical/frame_0033.png').read_bytes()==(ro/'observed_optical/frame_0033.png').read_bytes()
    remote=copy.deepcopy(b);remote['sequence'].update(lastFrame=1,exportFrames=[1]);extra=copy.deepcopy(remote['sequence']['sustainedFire']['sources'][0]);extra['offsetMeters']=[20,20,0];remote['sequence']['sustainedFire']['sources'].append(extra);remote_out=run('remote_before_ignition',remote)
    for folder in ['observed_ir','observed_optical']:assert (out/folder/'frame_0001.png').read_bytes()==(remote_out/folder/'frame_0001.png').read_bytes(),'Inactive remote fire changed volume bounds'
    short=copy.deepcopy(b);short['sequence'].update(lastFrame=1,exportFrames=[1]);short['sequence']['sustainedFire']['sources'][0]['durationSeconds']=1;short['sequence']['sustainedFire']['sources'][0].pop('fadeSeconds');short_out=run('short_default_fade',short);assert json.loads((short_out/'run-manifest.json').read_text())['sustainedFire']['sources'][0]['fadeSeconds']==1
    for name,mutate in [('bad_duration',lambda v:v['sequence']['sustainedFire']['sources'][0].update(durationSeconds=-1)),('bad_domain',lambda v:v['sequence']['sustainedFire']['sources'][0].update(offsetMeters=[1000,0,0])),('bad_media',lambda v:v['sequence'].update(media=False)),('bad_scenery_hash',lambda v:v['sequence']['scenery'].update(sha256='0'*64))]:
        v=copy.deepcopy(b);mutate(v);run(name,v,False,'scenery' if 'scenery' in name else 'fire')
    (a.output/'verification.json').write_text(json.dumps(dict(status='passed',lateFlamePixels=int(np.count_nonzero(orange(hot))),baselineLateFlamePixels=int(np.count_nonzero(orange(cold))),rawIrMeanDifference=float(np.mean(np.abs(raw-baseline))),treeTemperaturePixels=int(np.count_nonzero(np.abs(temp-296.5)<1e-4)),physicalCalibration=False),indent=2),encoding='utf-8')
    print('PASS: sustained RGB/IR fire, transported smoke, scenery temperatures, pre-ignition isolation, seeking/replay and input guards')


if __name__=='__main__':main()
