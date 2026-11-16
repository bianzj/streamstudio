"""Verify sustained-fire/tree delivery, provenance and unchanged rigid dynamics."""
import argparse, csv, json
from pathlib import Path
import numpy as np
from PIL import Image
from prepare_thermal_case import require, sha
from make_fire_trees_case import validate_tree_clearance


def read(path):return json.loads(path.read_text(encoding='utf-8'))


def main():
    p=argparse.ArgumentParser()
    for name in ['case','reference','repo']:p.add_argument(name,type=Path)
    a=p.parse_args();case,reference,repo=a.case.resolve(),a.reference.resolve(),a.repo.resolve()
    run,old=case/'result_final',reference/'result_final';m,prior=read(run/'run-manifest.json'),read(old/'run-manifest.json')
    require(m['status']=='complete' and m['frameCount']==288 and m['fps']==24,'Full native sequence incomplete')
    require(m['timeline']==prior['timeline'],'Tree/fire appearance changed rigid timeline')
    require(sha(run/'rigid.csv')==sha(old/'rigid.csv'),'Rigid diagnostics changed')
    background_files=0
    for f in (old/'provenance/background').glob('*'):
        require(sha(f)==sha(run/'provenance/background'/f.name),'Original FacetEB background changed');background_files+=1
    require(m['scenery']['temperatureSource']=='prescribed supplemental materials','Tree thermal provenance incorrect')
    tree_meta=read(case/'inputs/trees.json');validate_tree_clearance(read(case/'sequence.json'),tree_meta['trees'])
    require(tree_meta['treeCount']==40 and m['scenery']['provenance']['treeCount']==40,'Missing trees')
    require(m['sustainedFire']['enabled'] and len(m['sustainedFire']['sources'])==3,'Missing sustained sources')
    for f in (run/'radiance').glob('*.f32'):
        values=np.fromfile(f,dtype='<f4');require(values.size==m['width']*m['height'] and np.isfinite(values).all() and values.min()>0,'Invalid raw IR/temperature')
    def image(folder,frame):return np.asarray(Image.open(folder/f'optical_frames/frame_{frame:04d}.png').convert('RGB'),dtype=float)
    def green(im):return (im[:,:,1]>im[:,:,0]*1.1+5)&(im[:,:,1]>im[:,:,2]*1.1+3)
    tree_pixels=int(np.count_nonzero(green(image(run,1))));old_tree_pixels=int(np.count_nonzero(green(image(old,1))))
    require(tree_pixels>old_tree_pixels+100,'Trees absent from final RGB')
    def warm_region(im):
        h,w=im.shape[:2];q=im[int(.3*h):int(.85*h),int(.4*w):int(.6*w)]
        return int(np.count_nonzero((q[:,:,0]>q[:,:,1]*1.15+15)&(q[:,:,1]>q[:,:,2]+5)))
    flame_pixels=warm_region(image(run,288));prior_flame_pixels=warm_region(image(old,288))
    require(flame_pixels>prior_flame_pixels+20,'Persistent flame absent from late frame')
    protected=read(case/'protected.json')
    for path,digest in protected.items():require(sha(repo/path)==digest,'Protected repository file changed')
    frozen_source=read(case/'frozen-source.json')
    for path,digest in frozen_source.items():require(sha(reference/'source'/path)==digest,'Prior source snapshot changed')
    frozen_output=read(case/'frozen-output.json')
    for path,digest in frozen_output.items():require(sha(reference/path)==digest,'Prior output changed')
    frozen_native=read(case/'frozen-native.json')['nativeFrameSha256']
    for path,digest in frozen_native.items():require(sha(old/path)==digest,'Prior native frame changed')
    sources=read(case/'source-sha256.json')
    for path,digest in sources.items():require(sha(repo/path)==digest and sha(case/'source'/path)==digest,'Current source/snapshot mismatch')
    engine=repo/'build/histream-animation/runtime/Release/histream.exe'
    require(sha(engine)==sha(run/'provenance/histream.exe'),'Engine changed after rendering')
    require(sha(run/'provenance/sequence.comp.spv')==sha(repo/'build/histream-animation/runtime/Release/shader/animation/mesh/sequence_fire.comp.spv'),'Wrong fire shader provenance')
    require(read(case/'tests_final/verification.json')['status']=='passed','Fire/scenery tests incomplete')
    require(read(case/'default-isolation.json')['status']=='passed','Default regression incomplete')
    require((case/'tests-camera.log').read_text().strip().startswith('PASS:'),'Camera tests incomplete')
    video=read(case/'video/verification.json');require(video['status']=='passed' and all(v['decodedFrames']==288 for v in video['videos'].values()),'Video decode incomplete')
    aux=case/'onboard_ir';aux_manifest=read(aux/'result_final/run-manifest.json');require(aux_manifest['status']=='complete' and aux_manifest['frameCount']==1,'Onboard still incomplete')
    require(sha(aux/'onboard_ir.png')==sha(aux/'result_final/observed_ir/frame_0025.png'),'Onboard image differs from native result')
    require(sha(aux/'result_final/provenance/histream.exe')==sha(engine),'Onboard engine mismatch')
    stats=list(csv.DictReader((run/'transport.csv').open()));before=list(csv.DictReader((old/'transport.csv').open()))
    receipt=dict(status='passed',device=m['device'],wallSeconds=m['wallSeconds'],treeCount=tree_meta['treeCount'],supplementalTriangles=m['scenery']['triangleCount'],
                 fireSourceCount=len(m['sustainedFire']['sources']),lateWarmPixels=flame_pixels,previousLateWarmPixels=prior_flame_pixels,treeGreenPixels=tree_pixels,previousTreeGreenPixels=old_tree_pixels,
                 rigidTimelineExact=True,rigidCsvExact=True,originalBackgroundFilesExact=background_files,treeTemperatureSource=m['scenery']['temperatureSource'],
                 finalSmokeIntegral=float(stats[-1]['smoke_integral']),previousFinalSmokeIntegral=float(before[-1]['smoke_integral']),
                 currentSourceFiles=len(sources),protectedRepositoryFiles=len(protected),previousSourceFiles=len(frozen_source),previousPackagedFiles=len(frozen_output),previousNativeFrames=len(frozen_native),
                 videos=video['videos'],onboardIR=str(aux/'onboard_ir.png'),calibratedDeviceValidation=False,ignitionPrediction=False,vegetationEnergyBalance=False,structuralPrediction=False)
    (case/'final-verification.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8');print(json.dumps(receipt,indent=2))


if __name__=='__main__':main()
