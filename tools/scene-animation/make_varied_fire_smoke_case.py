"""Create a separate native case, reusing the frozen tree/background/rigid inputs."""
import argparse,copy,json
from pathlib import Path


def save(path,value):
    if path.exists():
        if json.loads(path.read_text(encoding='utf-8'))==value:return
        raise FileExistsError('Existing case inputs are immutable: '+str(path))
    path.write_text(json.dumps(value,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')


def main():
    p=argparse.ArgumentParser();p.add_argument('reference',type=Path);p.add_argument('case',type=Path);a=p.parse_args()
    a.case.mkdir(parents=True,exist_ok=True)
    scene=json.loads((a.reference/'sequence.json').read_text(encoding='utf-8'));s=scene['sequence'];s['appearanceVersion']='varied-v3'
    for source,style,seed,radius,height,rate in zip(s['sustainedFire']['sources'],['window-licking','window-billowing','ground-pool'],[202611,343817,698231],[1.05,.95,1.2],[3.5,2.7,2.4],[5.5,7.5,6.5]):
        source.update(flameStyle=style,seed=seed,radiusMeters=radius,heightMeters=height,tracerRate=rate)
    c=s['cameraObservation'];c.update(jitterOnImpact=True,impactShakeRadians=.004,regionalBlur=dict(enabled=True,onImpact=True,radiusNormalized=[.22,.32],irSigmaSourcePixels=2.3,rgbSigmaSourcePixels=3.2,irEdgeSigmaSourcePixels=1,rgbEdgeSigmaSourcePixels=1.3))
    c['ir']['psfSigmaPixels']=1.05;c['rgb'].update(width=640,height=360,psfSigmaPixels=.9)
    save(a.case/'sequence.json',scene)
    preview=copy.deepcopy(scene);preview.update(width=640,height=360);s=preview['sequence']
    # Single 8 s preview; authored poses at that time, not a new contact calculation.
    old=json.loads((a.reference/'result_final/run-manifest.json').read_text());s.update(motionBackend='authored',impactTime=old['impactTime'],sourcePoint=old['transport']['sourcePointMeters'],fps=1,frameCount=13,firstFrame=9,lastFrame=9,exportFrames=[9])
    s['cameraObservation']['ir'].update(width=192,height=108);s['cameraObservation']['rgb'].update(width=320,height=180)
    save(a.case/'preview.json',preview)
    preview_rigid=copy.deepcopy(scene);preview_rigid['sequence'].update(fps=1,frameCount=13,firstFrame=9,lastFrame=9,exportFrames=[9])
    save(a.case/'preview_rigid.json',preview_rigid)
    onboard=json.loads((a.reference/'onboard_ir/sequence.json').read_text());onboard['sequence']['appearanceVersion']='varied-v3';onboard['sequence']['cameraObservation']=copy.deepcopy(c);onboard['sequence']['sustainedFire']=copy.deepcopy(scene['sequence']['sustainedFire'])
    (a.case/'onboard_ir').mkdir(exist_ok=True);save(a.case/'onboard_ir/sequence.json',onboard)
    save(a.case/'input-provenance.json',dict(reference=str(a.reference.resolve()),geometryReused=s['scenery'],unchanged='Tree geometry, native FacetEB background binding, rigid collision inputs and timing',changed='Supplied fire profiles/tracer rates, smoke extinction, camera observation',physicalCalibration=False))
    print('Created separate varied-v3 inputs; prior assets referenced by existing hashes')


if __name__=='__main__':main()
