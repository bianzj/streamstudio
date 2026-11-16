"""Verify far-to-near native movement, shared simulation and preserved cases."""
import argparse,copy,csv,json,math
from pathlib import Path
import numpy as np
from PIL import Image
from prepare_thermal_case import require,sha

def read(p):return json.loads(p.read_text(encoding='utf-8'))
def rows(p):return list(csv.DictReader(p.open(encoding='utf-8')))
def main():
    p=argparse.ArgumentParser();p.add_argument('case',type=Path);p.add_argument('repo',type=Path);p.add_argument('--duration-only',action='store_true');a=p.parse_args();case,repo=a.case.resolve(),a.repo.resolve();run=case/'result_final';m=read(run/'run-manifest.json')
    require(m['status']=='complete','Native approach render incomplete')
    require(m['impactTime']>=10 and m['frameCount']>=240,'Distant approach missing: body view is still shorter than10s')
    if a.duration_only:print('PASS: distant native duration');return
    spec=read(case/'approach.json');parent=Path(spec['parent']);observer=case/'observer/result_final';b=read(observer/'run-manifest.json');j=read(case/'sequence.json');s=j['sequence'];old=read(parent/'sequence.json');oldM=read(parent/'result_final/run-manifest.json');count=m['frameCount'];fps=m['fps'];impact=m['impactTime'];total=b['frameCount']
    require(m['status']==b['status']=='complete' and count==290 and total==504 and fps==b['fps']==24,'Incomplete native sequences')
    require(m['impactTime']==b['impactTime'] and abs(impact-oldM['impactTime']-9)<5e-5,'Contact time failed to shift9s')
    require((count-1)/fps<impact<=count/fps,'Incorrect last body frame')
    start=np.array(s['projectileStart']);end=np.array(m['timeline']['firstContact']['projectileOriginMeters']);source=np.array(s['sourcePoint']);oldStart=np.array(old['sequence']['projectileStart']);oldDistance=np.linalg.norm(source-oldStart);distance=np.linalg.norm(source-start)
    require(distance/oldDistance>3.8 and abs(distance/oldDistance-4)<1e-5,'No genuinely more distant starting position')
    require(np.max(np.abs((source-start)/s['impactTime']-(source-oldStart)/old['sequence']['impactTime']))<1e-5,'Authored visual translation rate changed')
    # Camera/model settings are exactly retained; only timeline and authored start vary.
    same=copy.deepcopy(j)
    for key in ['frameCount','impactTime','projectileStart','firstFrame','lastFrame','exportFrames']:
        same['sequence'].pop(key,None)
        if key in old['sequence']:same['sequence'][key]=old['sequence'][key]
    require(same==old,'Unrelated body input changed')
    for key in ['cameraObservation','scenery','sustainedFire','assets']:require(m[key]==b[key],'Shared metadata changed: '+key)
    for key in ['scenery','sustainedFire','assets']:require(m[key]==oldM[key],'Original scene metadata changed: '+key)
    oldCamera=copy.deepcopy(oldM['cameraObservation'])
    oldEvent=oldCamera['impactMotion']['startSeconds'];newEvent=m['cameraObservation']['impactMotion']['startSeconds']
    require(abs(newEvent-oldEvent-9)<5e-5,'Camera impact event failed to shift9s')
    oldCamera['impactMotion']['startSeconds']=newEvent
    require(oldCamera==m['cameraObservation'],'Camera settings changed beyond the expected event timestamp')
    require(sha(run/'ground_thermal/cells.f32')==sha(observer/'ground_thermal/cells.f32')==sha(parent/'result_final/ground_thermal/cells.f32'),'Ground field changed')
    for name in ['rigid.csv','transport.csv']:
        x=(run/name).read_bytes().splitlines();y=(observer/name).read_bytes().splitlines();require(x==y[:len(x)] and len(x)==count+1,'Shared simulation mismatch: '+name)
    f=(end-start)/np.linalg.norm(end-start);right=np.cross(f,[0,0,1]);right/=np.linalg.norm(right);up=np.cross(right,f);offset=np.array(s['cameraMount']['offsetMeters']);world=right*offset[0]+up*offset[1]+f*offset[2];samples=rows(run/'camera.csv');require(len(samples)==count*3,'Missing shutter samples');error=0;distances=[];exposure=s['cameraObservation']['exposureSeconds']
    for n in range(1,count+1):
        group=samples[(n-1)*3:n*3];require([int(r['canonical']) for r in group]==[0,0,1],'Shutter grouping differs')
        for index,row in enumerate(group):
            t=float(row['sampleTimeSeconds']);require(abs(t-max(0,(n-1)/fps-exposure*(1-index*.5)))<1e-7 and t<impact,'Shutter sample drift')
            expected=start+(end-start)*(t/impact)+world;actual=np.array([float(row[k]) for k in ['x','y','z']]);error=max(error,float(np.max(np.abs(actual-expected))))
            require(np.max(np.abs(np.array([float(row['forward'+k]) for k in 'XYZ'])-f))<2e-6 and float(row['fovYDegrees'])==55,'Camera zoom/orientation changed')
        distances.append(float(np.linalg.norm(end-np.array([float(group[-1][k]) for k in ['x','y','z']]))))
    require(error<3e-5 and all(x>y for x,y in zip(distances,distances[1:])) and distances[0]>120 and distances[-1]<.3,'Far-to-near path not continuous')
    obs=rows(run/'observation.csv');require(len(obs)==count,'Missing observations');lo,hi=m['cameraObservation']['ir']['adcSignalRange'];step=(hi-lo)/16383;agc=0
    for n,row in enumerate(obs,1):
        codes=np.asarray(Image.open(run/f'detector_ir/frame_{n:04d}.png'));require(codes.shape==(216,384) and codes.max()<=16383,'IR ADC changed');x=np.sort(codes.ravel().astype(float));lower=lo+x[int(.02*(len(x)-1))]*step;upper=max(lower+step,lo+x[int(.98*(len(x)-1))]*step);agc=max(agc,abs(lower-float(row['irAgcLow'])),abs(upper-float(row['irAgcHigh'])))
        require(all(float(row[k])==0 for k in ['jitterYawRadians','jitterPitchRadians','jitterRollRadians']),'Pre-contact jitter changed')
    require(agc<1e-4,'IR2% endpoints differ')
    videos=read(case/'video/verification.json');require(videos['status']=='passed','Video incomplete')
    for name,v in videos['videos'].items():require(v['decodedFrames']==(total if name=='synchronized' else count) and sha(Path(v['path']))==v['sha256'],'Video receipt mismatch')
    for f0,h in videos['nativeFrameSha256'].items():require(sha(run/f0)==h,'Body native image changed')
    sync=read(case/'video/synchronization.json');require(len(sync)==total and all(x['bodyObservationValid']==(i<count) and x['frame']==i+1 and x['presentationTimeSeconds']==i/fps for i,x in enumerate(sync)),'Synchronization mismatch')
    for f0,h in read(case/'frozen-source.json').items():require(sha(parent/'source'/f0)==h,'Old source snapshot changed')
    for f0,h in read(case/'frozen-output.json').items():require(sha(parent/f0)==h,'Old output changed')
    for f0,h in read(case/'frozen-native.json')['nativeFrameSha256'].items():require(sha(parent/'result_final'/f0)==h,'Old native frame changed')
    for f0,h in read(case/'protected.json').items():require(sha(repo/f0)==h,'Protected source changed')
    hashes=read(case/'source-sha256.json')
    for f0,h in hashes.items():require(sha(repo/f0)==h and sha(case/'source'/f0)==h,'Current source snapshot mismatch')
    require(sha(run/'provenance/histream.exe')==sha(observer/'provenance/histream.exe')==read(case/'source-version.json')['engineSha256'],'Engine mismatch')
    require(sha(run/'provenance/sequence.comp.spv')==sha(parent/'result_final/provenance/sequence.comp.spv'),'GPU renderer changed')
    receipt=dict(status='passed',bodyFrames=count,synchronizedFrames=total,fps=fps,bodySeconds=count/fps,totalSeconds=total/fps,firstContactSeconds=impact,nominalStartDistanceM=distance,oldStartDistanceM=oldDistance,initialCameraDistanceM=distances[0],finalCameraDistanceM=distances[-1],maxIndependentPoseErrorM=error,maxIrPercentileError=agc,poseSamples=len(samples),fixedFovDegrees=55,sharedGroundFieldExact=True,sharedRigidTransportPrefixExact=True,sourceFiles=len(hashes),previousSourceFilesPreserved=len(read(case/'frozen-source.json')),previousNativeFramesPreserved=len(read(case/'frozen-native.json')['nativeFrameSha256']),videos=videos['videos'],scope='Authored CGI approach extension; no flight or guidance model; no calibrated sensor accuracy')
    (case/'final-verification.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8');print(json.dumps(receipt,indent=2))
if __name__=='__main__':main()
