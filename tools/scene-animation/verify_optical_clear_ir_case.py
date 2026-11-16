"""Check channel-specific blur, true2% display and exact preserved simulation."""
import argparse,csv,json
from pathlib import Path
import numpy as np
from PIL import Image
from prepare_thermal_case import require,sha


def read(p):return json.loads(p.read_text(encoding='utf-8'))
def rows(p):return list(csv.DictReader(p.open(encoding='utf-8')))


def main():
    p=argparse.ArgumentParser()
    for key in ['case','reference','repo']:p.add_argument(key,type=Path)
    a=p.parse_args();case,old,repo=a.case.resolve(),a.reference.resolve(),a.repo.resolve();run,prior=case/'result_final',old/'result_final';m,previous=read(run/'run-manifest.json'),read(prior/'run-manifest.json')
    require(m['status']=='complete' and m['frameCount']==288 and m['fps']==24,'Native sequence incomplete')
    require(m['timeline']==previous['timeline'] and m['scenery']==previous['scenery'] and m['sustainedFire']==previous['sustainedFire'],'Simulation/scenery changed')
    for name in ['rigid.csv','transport.csv']:require(sha(run/name)==sha(prior/name),'Simulation changed: '+name)
    canonical=0
    for folder in ['frames','fixed_frames','optical_frames','radiance','geometry']:
        for f in (prior/folder).glob('*'):
            if f.is_file():require(sha(f)==sha(run/folder/f.name),'Canonical output changed: '+str(f));canonical+=1
    background=list((prior/'provenance/background').glob('*'))
    for f in background:require(sha(f)==sha(run/'provenance/background'/f.name),'Background changed')
    c=m['cameraObservation'];r=read(case/'sequence.json')['sequence']['cameraObservation'];require(c['rgb']['pixels']==[1280,720] and c['rgb']['gaussianPsfSigmaDetectorPixels']==0 and c['rgb']['responseSeconds']==0,'Optical blur remains')
    require(r['regionalBlur']['rgbSigmaSourcePixels']==0 and r['regionalBlur']['rgbEdgeSigmaSourcePixels']==0 and c['exposureSeconds']<=1e-5,'Optical regional/motion blur remains')
    require(c['ir']['pixels']==[384,216] and c['ir']['adcBits']==14 and abs(c['ir']['gaussianPsfSigmaDetectorPixels']-.65)<1e-6,'IR sampling profile differs')
    require(c['irDisplay']['mode']=='linear-percent-2' and c['irDisplay']['percentiles']==[2,98] and c['irDisplay']['rawAdcModified'] is False,'IR display differs')
    obs=rows(run/'observation.csv');require(len(obs)==288,'Observation rows missing');before=[r for r in obs if float(r['presentationTimeSeconds'])<=m['impactTime']]
    require(all(float(r[k])==0 for r in before for k in ['jitterYawRadians','jitterPitchRadians','jitterRollRadians']),'Pre-impact jitter')
    lo,hi=c['ir']['adcSignalRange'];step=(hi-lo)/((1<<14)-1);errors=[]
    for n in range(1,289):
        codes=np.asarray(Image.open(run/f'detector_ir/frame_{n:04d}.png'));require(codes.shape==(216,384) and codes.max()<=16383,'IR ADC invalid')
        values=np.sort(codes.ravel().astype(float));low=lo+values[int(.02*(len(values)-1))]*step;high=max(lo+values[int(.98*(len(values)-1))]*step,low+step)
        error=max(abs(float(obs[n-1]['irAgcLow'])-low),abs(float(obs[n-1]['irAgcHigh'])-high));require(error<1e-4,'IR display endpoints not true2%');errors.append(error)
    for f,h in read(case/'protected.json').items():require(sha(repo/f)==h,'Protected repository file changed')
    for f,h in read(case/'frozen-source.json').items():require(sha(old/'source'/f)==h,'Old source changed')
    for f,h in read(case/'frozen-output.json').items():require(sha(old/f)==h,'Old packaged file changed')
    native=read(case/'frozen-native.json')['nativeFrameSha256']
    for f,h in native.items():require(sha(prior/f)==h,'Old native frame changed')
    sources=read(case/'source-sha256.json')
    for f,h in sources.items():require(sha(repo/f)==h and sha(case/'source'/f)==h,'Current source snapshot differs: '+f)
    runtime=repo/'build/histream-animation/runtime/Release';require(sha(runtime/'histream.exe')==sha(run/'provenance/histream.exe'),'Engine mismatch')
    for name in ['filter','integrate','detector','display','regional_filter']:require(sha(runtime/f'shader/animation/camera/{name}.comp.spv')==sha(run/f'provenance/camera/{name}.comp.spv'),'Camera shader mismatch')
    for item in ['camera_tests','tests_final']:require(read(case/item/'verification.json')['status']=='passed','Scoped tests incomplete')
    require(read(case/'compatibility.json')['status']=='passed','Legacy compatibility incomplete')
    video=read(case/'video/verification.json');comparison=read(case/'comparison/verification.json');require(video['status']=='passed' and comparison['status']=='passed','Video checks incomplete')
    for v in video['videos'].values():require(v['decodedFrames']==288 and sha(Path(v['path']))==v['sha256'],'Video hash/count differs')
    require(comparison['frames']==288 and sha(Path(comparison['path']))==comparison['sha256'],'Comparison differs')
    aux=case/'onboard_ir';require(read(aux/'result_final/run-manifest.json')['status']=='complete' and sha(aux/'onboard_ir.png')==sha(aux/'result_final/observed_ir/frame_0025.png'),'Auxiliary IR incomplete')
    result=dict(status='passed',nativeFrames=288,seconds=12,wallSeconds=m['wallSeconds'],canonicalFilesExact=canonical,transportExact=True,rigidExact=True,backgroundFilesExact=len(background),opticalGaussianBlur=False,irNativePixels=[384,216],irRawBits=14,displayBits=8,irDisplayPercentiles=[2,98],maxPercentileError=max(errors),preContactJitterZeroFrames=len(before),sourceFiles=len(sources),previousNativeFramesPreserved=len(native),videos=video['videos'],comparison=comparison,physicalCalibration=False)
    (case/'final-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result,indent=2))


if __name__=='__main__':main()
