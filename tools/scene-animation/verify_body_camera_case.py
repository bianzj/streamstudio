"""Verify camera parenting, timestamps, actual field preservation and artifacts."""
import argparse,copy,csv,json
from pathlib import Path
import numpy as np
from PIL import Image
from prepare_thermal_case import require,sha

def read(p):return json.loads(p.read_text(encoding='utf-8'))
def rows(p):return list(csv.DictReader(p.open(encoding='utf-8')))
def main():
    p=argparse.ArgumentParser();p.add_argument('case',type=Path);p.add_argument('repo',type=Path);a=p.parse_args();case,repo=a.case.resolve(),a.repo.resolve()
    spec=read(case/'case.json');ref=Path(spec['reference']);run,old=case/'result_final',ref/'result_final';m,b=read(run/'run-manifest.json'),read(old/'run-manifest.json');count=spec['validFrames'];fps=spec['fps'];impact=m['impactTime']
    require(m['status']=='complete' and m['frameCount']==count and m['firstFrame']==1 and m['lastFrame']==count and m['timelineFrameCount']==spec['frameCount'],'Incomplete mounted sequence')
    require(sha(old/'run-manifest.json')==spec['referenceManifestSha256'] and impact==spec['firstContactSeconds'],'Reference/contact drift')
    j,parent=read(case/'sequence.json'),read(ref/'sequence.json');unchanged=copy.deepcopy(j);unchanged['cameras'].pop()
    for k in ['camera','exportFrames','firstFrame','lastFrame','cameraMount']:
        unchanged['sequence'].pop(k,None)
        if k in parent['sequence']:unchanged['sequence'][k]=parent['sequence'][k]
    require(unchanged==parent,'Unrelated scene or model input changed')
    for key in ['cameraObservation','scenery','sustainedFire','assets']:
        require(m[key]==b[key],'Unrelated manifest state differs: '+key)
    ground,priorGround=copy.deepcopy(m['groundThermal']),copy.deepcopy(b['groundThermal'])
    # The full source JSON hash includes the intentionally changed camera.
    require(ground.pop('baseSceneSha256')==m['sourceSha256'] and priorGround.pop('baseSceneSha256')==b['sourceSha256'],'Ground source provenance mismatch')
    require(ground==priorGround,'Physical ground configuration/state differs')
    require(sha(run/'ground_thermal/cells.f32')==sha(old/'ground_thermal/cells.f32'),'Physical field depends on camera')
    for name in ['rigid.csv','transport.csv']:
        x=(run/name).read_bytes().splitlines();y=(old/name).read_bytes().splitlines();require(len(x)==count+1 and x==y[:len(x)],'Simulation prefix changed: '+name)
    # Independent body transform from original start and actual contact origin.
    start=np.array(parent['sequence']['projectileStart']);end=np.array(b['timeline']['firstContact']['projectileOriginMeters']);forward=(end-start)/np.linalg.norm(end-start);right=np.cross(forward,[0,0,1]);right/=np.linalg.norm(right);up=np.cross(right,forward);offset=np.array(j['sequence']['cameraMount']['offsetMeters']);worldOffset=right*offset[0]+up*offset[1]+forward*offset[2]
    samples=rows(run/'camera.csv');require(len(samples)==count*3,'Missing shutter poses');poseError=0;forwardError=0;canonical=[];exposure=j['sequence']['cameraObservation']['exposureSeconds']
    for n in range(1,count+1):
        group=samples[(n-1)*3:n*3];t=(n-1)/fps
        require([int(x['canonical']) for x in group]==[0,0,1] and all(int(x['frame'])==n for x in group),'Shutter sample grouping')
        for i,row in enumerate(group):
            s=float(row['sampleTimeSeconds']);require(abs(s-max(0,t-exposure*(1-i*.5)))<1e-8 and s<impact,'Shutter pose time differs')
            require(abs(float(row['presentationTimeSeconds'])-t)<1e-8,'Presentation timestamp differs')
            body=start+(end-start)*(s/impact);expected=body+worldOffset
            poseError=max(poseError,float(np.max(np.abs(expected-np.array([float(row[k]) for k in ['x','y','z']])))))
            poseError=max(poseError,float(np.max(np.abs(body-np.array([float(row['body'+k]) for k in 'XYZ'])))))
            forwardError=max(forwardError,float(np.max(np.abs(forward-np.array([float(row['forward'+k]) for k in 'XYZ'])))))
            require(np.max(np.abs(up-np.array([float(row['up'+k]) for k in 'XYZ'])))<1e-6 and float(row['fovYDegrees'])==55,'Mounted axes/FOV drift')
        canonical.append([float(group[-1][k]) for k in ['x','y','z']])
    require(poseError<1e-5 and forwardError<1e-6,'Independent body transform mismatch')
    require((count-1)/fps<impact<=count/fps and np.linalg.norm(np.array(canonical[-1])-canonical[0])>20,'Wrong contact boundary/static camera')
    observations=rows(run/'observation.csv');require(len(observations)==count,'Missing camera observations');lo,hi=m['cameraObservation']['ir']['adcSignalRange'];step=(hi-lo)/16383;agcError=0
    for n,row in enumerate(observations,1):
        require(int(row['frame'])==n and abs(float(row['presentationTimeSeconds'])-(n-1)/fps)<1e-8,'Observation timestamp mismatch')
        require(all(float(row[k])==0 for k in ['jitterYawRadians','jitterPitchRadians','jitterRollRadians']),'Pre-contact angular jitter')
        codes=np.asarray(Image.open(run/f'detector_ir/frame_{n:04d}.png'));require(codes.shape==(216,384) and codes.max()<=16383,'IR codes/shape invalid')
        x=np.sort(codes.ravel().astype(float));lower=lo+x[int(.02*(len(x)-1))]*step;upper=max(lo+x[int(.98*(len(x)-1))]*step,lower+step);agcError=max(agcError,abs(lower-float(row['irAgcLow'])),abs(upper-float(row['irAgcHigh'])))
    require(agcError<1e-4,'2% display stretch changed')
    video=read(case/'video/verification.json');require(video['status']=='passed','Packaging failed')
    for f,h in video['nativeFrameSha256'].items():require(sha(run/f)==h,'Native image changed')
    for name,v in video['videos'].items():
        require(v['decodedFrames']==(spec['frameCount'] if name=='synchronized' else count) and sha(Path(v['path']))==v['sha256'],'Video count/hash mismatch')
    sync=read(case/'video/synchronization.json');require(len(sync)==spec['frameCount'],'Sync timeline incomplete')
    for n,row in enumerate(sync,1):require(row==dict(frame=n,presentationTimeSeconds=(n-1)/fps,bodyObservationValid=n<=count,bodyFrame=n if n<=count else None),'Wrong sync or fabricated post-contact observation')
    for f,h in read(case/'frozen-source.json').items():require(sha(ref/'source'/f)==h,'Previous source snapshot changed')
    for f,h in read(case/'frozen-output.json').items():require(sha(ref/f)==h,'Previous output changed')
    for f,h in read(case/'frozen-native.json')['nativeFrameSha256'].items():require(sha(old/f)==h,'Previous native frame changed')
    for f,h in read(case/'protected.json').items():require(sha(repo/f)==h,'Protected source changed')
    source=read(case/'source-sha256.json')
    for f,h in source.items():require(sha(repo/f)==h and sha(case/'source'/f)==h,'Current source snapshot mismatch: '+f)
    runtime=repo/'build/histream-animation/runtime/Release';require(sha(runtime/'histream.exe')==sha(run/'provenance/histream.exe')==read(case/'source-version.json')['engineSha256'],'Engine mismatch')
    require(sha(run/'provenance/sequence.comp.spv')==sha(old/'provenance/sequence.comp.spv'),'Rendering shader changed')
    require(read(case/'tests_final/verification.json')['status']=='passed' and read(case/'compatibility.json')['status']=='passed','Native checks incomplete')
    receipt=dict(status='passed',validBodyCameraFrames=count,fullSynchronizedFrames=spec['frameCount'],fps=fps,firstContactSeconds=impact,lastBodySampleSeconds=(count-1)/fps,poseSamples=len(samples),maxIndependentPositionErrorMeters=poseError,maxIndependentForwardError=forwardError,maxIrPercentileError=agcError,sourceFiles=len(source),previousSourceFilesPreserved=len(read(case/'frozen-source.json')),previousNativeFramesPreserved=len(read(case/'frozen-native.json')['nativeFrameSha256']),physicalGroundFieldExact=True,rigidAndTransportPrefixExact=True,legacyCompatibility=read(case/'compatibility.json'),videos=video['videos'],scope='Camera synchronization on unchanged authored trajectory; no new guidance, flight dynamics or measured sensor validation')
    (case/'final-verification.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8');print(json.dumps(receipt,indent=2))
if __name__=='__main__':main()
