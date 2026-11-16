"""Native natural-v2 selection, source isolation and deterministic observation."""
import argparse,copy,json,subprocess
from pathlib import Path
import numpy as np
from PIL import Image


def main():
    p=argparse.ArgumentParser()
    for k in ['engine','reference','output']:p.add_argument(k,type=Path)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    b=json.loads((a.reference/'tests_final/fire_trees.json').read_text());b['sequence']['appearanceVersion']='natural-v2'
    b['sequence'].update(lastFrame=33,exportFrames=[1,31,32,33])
    def run(name,j,ok=True,word=''):
        f=a.output/(name+'.json');f.write_text(json.dumps(j),encoding='utf-8');out=a.output/name
        r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(f.resolve()),str(out.resolve())],capture_output=True);(a.output/(name+'.log')).write_bytes(r.stdout+r.stderr)
        assert (r.returncode==0)==ok and word.lower() in (r.stdout+r.stderr).decode(errors='replace').lower(),(name,r.returncode,(r.stdout+r.stderr)[-1000:]);return out
    out=run('natural',b);m=json.loads((out/'run-manifest.json').read_text());assert m.get('appearanceVersion')=='natural-v2','Missing natural-v2 native shader'
    legacy=copy.deepcopy(b);legacy['sequence'].pop('appearanceVersion');old=run('legacy',legacy)
    jet_peak=float(np.fromfile(out/'radiance/frame_0001.f32',dtype='<f4').max());old_jet_peak=float(np.fromfile(old/'radiance/frame_0001.f32',dtype='<f4').max());assert jet_peak>old_jet_peak*.2,'Thin pre-contact exhaust was lost'
    assert (out/'transport.csv').read_bytes()==(old/'transport.csv').read_bytes(),'Appearance changed actual transport'
    for n in [31,32,33]:
        raw=np.fromfile(out/f'radiance/frame_{n:04d}.f32',dtype='<f4');assert np.isfinite(raw).all() and raw.min()>0
    def im(folder,n):return np.asarray(Image.open(folder/f'optical_frames/frame_{n:04d}.png').convert('RGB'),dtype=float)
    assert np.mean(np.abs(im(out,33)-im(old,33)))>.01,'New flame/lighting has no effect'
    assert np.count_nonzero(np.max(np.abs(im(out,31)-im(out,33)),axis=-1)>10)>5,'Flame detail is frozen'
    seek=copy.deepcopy(b);seek['sequence'].update(firstFrame=33,exportFrames=[33]);s=run('seek',seek)
    for folder in ['observed_ir','observed_optical','detector_ir']:assert (out/folder/'frame_0033.png').read_bytes()==(s/folder/'frame_0033.png').read_bytes()
    for name,field,value in [('bad_version','appearanceVersion','typo'),('bad_physical','physicalAppearance',False),('bad_optical','optical',False)]:
        bad=copy.deepcopy(b);bad['sequence'][field]=value;run(name,bad,False,'appearance')
    # Isolate the thin-leaf flag with a backlit plane in an otherwise identical scene.
    background=json.loads(Path(b['backgroundField']['path']).read_text());sun=np.asarray(background['sunDirectionWorld'],dtype=float);sun/=np.linalg.norm(sun);n=-sun
    u=np.cross(n,[0,0,1] if abs(n[2])<.9 else [1,0,0]);u/=np.linalg.norm(u);v=np.cross(n,u);centre=np.array([-65.,-30.,12.]);vertices=[centre-u*.8-v*.8,centre+u*.8-v*.8,centre+u*.8+v*.8,centre-u*.8+v*.8]
    obj=a.output/'backlit_leaf.obj';obj.write_text('mtllib backlit_leaf.mtl\n'+''.join('v '+' '.join(map(str,p))+'\n' for p in vertices)+'usemtl leaf\nf 1 2 3\nf 1 3 4\n',encoding='utf-8')
    import hashlib
    scene=json.loads(Path(b['sequence']['scenery']['path']).read_text());scene['assets']=[dict(id='leaf',path=str(obj.resolve()),sha256=hashlib.sha256(obj.read_bytes()).hexdigest(),units='m',upAxis='Z')];scene['instances'][0]['asset']='leaf';scene['materials']=[dict(id='leaf',color=[.10,.32,.05],emissivity=.98,temperatureK=296.5,roughness=.8,thinLeaf=True)]
    leaf_outputs=[]
    for flag in [False,True]:
        scene['materials'][0]['thinLeaf']=flag;sf=a.output/('leaf_scene_'+str(flag)+'.json');sf.write_text(json.dumps(scene),encoding='utf-8');j=copy.deepcopy(b);j['sequence'].update(firstFrame=1,lastFrame=1,exportFrames=[1],camera='leaf_test',scenery=dict(enabled=True,path=str(sf.resolve()),sha256=hashlib.sha256(sf.read_bytes()).hexdigest()));j['cameras'].append(dict(id='leaf_test',position=(centre-sun*8).tolist(),target=centre.tolist(),up=[0,0,1],fovYDegrees=40));leaf_outputs.append(run('leaf_'+str(flag),j))
    leaf_change=float(np.mean(np.abs(im(leaf_outputs[0],1)-im(leaf_outputs[1],1))));assert leaf_change>.1,'Thin leaf tag has no backlighting effect'
    assert (leaf_outputs[0]/'radiance/frame_0001.f32').read_bytes()==(leaf_outputs[1]/'radiance/frame_0001.f32').read_bytes(),'RGB leaf transmission changed IR'
    assert json.loads((leaf_outputs[1]/'run-manifest.json').read_text())['scenery']['thinLeafTaggedTriangles']==2
    (a.output/'verification.json').write_text(json.dumps(dict(status='passed',transportExact=True,meanRgbChange=float(np.mean(np.abs(im(out,33)-im(old,33)))),leafBacklightRgbChange=leaf_change,leafIrExact=True,jetPeak=jet_peak,legacyJetPeak=old_jet_peak,physicalCalibration=False),indent=2),encoding='utf-8')
    print('PASS: natural-v2 shader, finite IR, altered/unsteady RGB, unchanged transport, seek and guards')


if __name__=='__main__':main()
