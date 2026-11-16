"""Verify scoped appearance/camera evidence without asserting combustion accuracy."""
import argparse,csv,json
from pathlib import Path
import numpy as np
from prepare_thermal_case import require,sha


def read(p):return json.loads(p.read_text(encoding='utf-8'))
def rows(p):return list(csv.DictReader(p.open(encoding='utf-8')))


def main():
    p=argparse.ArgumentParser()
    for key in ['case','reference','repo']:p.add_argument(key,type=Path)
    a=p.parse_args();case,reference,repo=a.case.resolve(),a.reference.resolve(),a.repo.resolve();run,old=case/'result_final',reference/'result_final';m,prior=read(run/'run-manifest.json'),read(old/'run-manifest.json')
    require(m['status']=='complete' and m['appearanceVersion']=='varied-v3','Varied render incomplete')
    require(m['frameCount']==288 and m['fps']==24 and m['samplesPerPixel']==4,'Unexpected sampling profile')
    require(m['timeline']==prior['timeline'] and sha(run/'rigid.csv')==sha(old/'rigid.csv'),'Rigid trajectory changed')
    require(m['scenery']==prior['scenery'] and m['scenery']['triangleCount']==589280 and m['scenery']['thinLeafTaggedTriangles']==528000,'Native tree geometry changed')
    source=read(case/'sequence.json');previous=read(reference/'sequence.json');require(source['sequence']['scenery']==previous['sequence']['scenery'],'Tree geometry changed')
    background=0
    for f in (old/'provenance/background').glob('*'):require(sha(f)==sha(run/'provenance/background'/f.name),'Original background changed');background+=1
    require(background==12,'Missing original background files')
    fires=m['sustainedFire']['sources'];require([f['flameStyle'] for f in fires]==['window-licking','window-billowing','ground-pool'] and len({f['seed'] for f in fires})==3,'Fire profiles missing')
    transport,previousTransport=rows(run/'transport.csv'),rows(old/'transport.csv');smoke=float(transport[-1]['smoke_integral']);priorSmoke=float(previousTransport[-1]['smoke_integral'])
    require(smoke>priorSmoke,'Smoke did not increase in the actual transported field')
    require(len(transport)==288 and all(np.isfinite(float(v)) for r in transport for v in r.values()),'Incomplete/nonfinite transport diagnostics')
    require(all(float(r['invalid_cells'])==0 and float(r['smoke_min'])>=-1e-6 for r in transport),'Invalid transport cells/negative smoke')
    obs=rows(run/'observation.csv');before=[r for r in obs if float(r['presentationTimeSeconds'])<=m['impactTime']];after=[r for r in obs if float(r['presentationTimeSeconds'])>m['impactTime']]
    require(len(obs)==288 and before and after,'Incomplete observation receipt')
    require(all(float(r[k])==0 for r in before for k in ['jitterYawRadians','jitterPitchRadians','jitterRollRadians','regionalActivation']),'Motion/central blur before impact')
    require(any(abs(float(r['jitterYawRadians']))>0 for r in after) and float(after[-1]['regionalActivation'])>.99,'Post-impact effects absent')
    require(all(abs(float(r['impactTimeSeconds'])-m['impactTime'])<1e-6 for r in obs),'Trigger differs from native contact')
    for f in (run/'radiance').glob('*.f32'):
        v=np.fromfile(f,dtype='<f4');require(v.size==m['width']*m['height'] and np.isfinite(v).all() and v.min()>0,'Invalid canonical IR field')
    for receipt in ['tests_final/verification.json','camera_verified/verification.json','seed_verified/verification.json','compatibility.json']:
        require(read(case/receipt)['status']=='passed','Scoped checks incomplete: '+receipt)
    protected=read(case/'protected.json');frozen=read(case/'frozen-source.json');outputs=read(case/'frozen-output.json');native=read(case/'frozen-native.json')['nativeFrameSha256'];sources=read(case/'source-sha256.json')
    for f,h in protected.items():require(sha(repo/f)==h,'Protected file changed: '+f)
    for f,h in frozen.items():require(sha(reference/'source'/f)==h,'Previous code changed: '+f)
    for f,h in outputs.items():require(sha(reference/f)==h,'Previous artifact changed: '+f)
    for f,h in native.items():require(sha(old/f)==h,'Previous native image changed: '+f)
    for f,h in sources.items():require(sha(repo/f)==h and sha(case/'source'/f)==h,'Current code/snapshot mismatch: '+f)
    runtime=repo/'build/histream-animation/runtime/Release';require(sha(runtime/'histream.exe')==sha(run/'provenance/histream.exe'),'Engine mismatch')
    require(sha(runtime/'shader/animation/mesh/sequence_varied.comp.spv')==sha(run/'provenance/sequence.comp.spv'),'Varied shader mismatch')
    require(sha(runtime/'shader/animation/camera/regional_filter.comp.spv')==sha(run/'provenance/camera/regional_filter.comp.spv'),'Regional shader mismatch')
    video=read(case/'video/verification.json');comparison=read(case/'comparison/verification.json');require(video['status']=='passed' and all(v['decodedFrames']==288 for v in video['videos'].values()) and comparison['status']=='passed' and comparison['frames']==288,'Videos incomplete')
    for f,h in video['nativeFrameSha256'].items():require(sha(run/f)==h,'Packaged native image changed')
    aux=case/'onboard_ir';require(read(aux/'result_final/run-manifest.json')['status']=='complete' and sha(aux/'onboard_ir.png')==sha(aux/'result_final/observed_ir/frame_0025.png'),'Onboard still incomplete')
    require(sha(aux/'result_final/provenance/histream.exe')==sha(runtime/'histream.exe'),'Onboard engine mismatch')
    require(float(rows(aux/'result_final/observation.csv')[0]['jitterYawRadians'])==0,'Pre-contact onboard jitter')
    result=dict(status='passed',device=m['device'],wallSeconds=m['wallSeconds'],appearance='varied-v3',treeGeometryReused=True,rigidTimelineExact=True,originalBackgroundFilesExact=background,smokeIntegral=smoke,priorSmokeIntegral=priorSmoke,smokeRatio=smoke/priorSmoke,transportChangedAsIntended=True,actualImpactSeconds=m['impactTime'],preContactFramesWithoutJitter=len(before),postContactFrames=len(after),nativeTests=read(case/'tests_final/verification.json'),cameraTests=read(case/'camera_verified/verification.json'),seedGuards=read(case/'seed_verified/verification.json'),compatibility=read(case/'compatibility.json'),sourceFiles=len(sources),protectedFiles=len(protected),previousSourceFiles=len(frozen),previousPackagedFiles=len(outputs),previousNativeFrames=len(native),videos=video['videos'],comparison=comparison,physicalCalibration=False,combustionPrediction=False,structuralPrediction=False)
    (case/'final-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result,indent=2))


if __name__=='__main__':main()
