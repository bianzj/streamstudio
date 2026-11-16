"""Verify gradual supplied fire growth, retained camera and frozen previous case."""
import argparse,copy,csv,json
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
    require(m['timeline']==previous['timeline'] and m['scenery']==previous['scenery'] and sha(run/'rigid.csv')==sha(prior/'rigid.csv'),'Rigid/scenery changed')
    require(m['cameraObservation']==previous['cameraObservation'],'Camera/display settings changed')
    config=read(case/'sequence.json');baseline=read(old/'sequence.json');without=copy.deepcopy(config)
    for s in without['sequence']['sustainedFire']['sources']:s.pop('growthSeconds')
    require(without==baseline,'Unexpected configuration changes')
    require([s['growthSeconds'] for s in m['sustainedFire']['sources']]==[5,6.5,4],'Growth schedule differs')
    geometry=0
    for f in (prior/'geometry').glob('*'):
        if f.is_file():require(sha(f)==sha(run/'geometry'/f.name),'Surface geometry changed');geometry+=1
    bg=list((prior/'provenance/background').glob('*'))
    for f in bg:require(sha(f)==sha(run/'provenance/background'/f.name),'Original background changed')
    transport=rows(run/'transport.csv');require(len(transport)==288 and all(np.isfinite(float(v)) for r in transport for v in r.values()),'Invalid transport diagnostics')
    require(all(float(r['invalid_cells'])==0 and float(r['smoke_min'])>=-1e-6 for r in transport),'Invalid/negative smoke cells')
    require(sha(run/'transport.csv')!=sha(prior/'transport.csv'),'Growing heat/tracer sources did not affect transport')
    obs=rows(run/'observation.csv');require(len(obs)==288,'Observation rows missing');before=[r for r in obs if float(r['presentationTimeSeconds'])<=m['impactTime']]
    require(all(float(r[k])==0 for r in before for k in ['jitterYawRadians','jitterPitchRadians','jitterRollRadians']),'Pre-impact camera jitter')
    previousObs=rows(prior/'observation.csv')
    for r,s in zip(obs,previousObs):
        for k in ['jitterYawRadians','jitterPitchRadians','jitterRollRadians','regionalActivation','regionalCenterX','regionalCenterY']:require(r[k]==s[k],'Camera timing/path changed')
    c=m['cameraObservation'];lo,hi=c['ir']['adcSignalRange'];step=(hi-lo)/16383;error=0
    for n in range(1,289):
        v=np.asarray(Image.open(run/f'detector_ir/frame_{n:04d}.png'));require(v.shape==(216,384) and v.max()<=16383,'Invalid native IR ADC')
        v=np.sort(v.ravel().astype(float));low=lo+v[int(.02*(len(v)-1))]*step;high=max(lo+v[int(.98*(len(v)-1))]*step,low+step)
        error=max(error,abs(float(obs[n-1]['irAgcLow'])-low),abs(float(obs[n-1]['irAgcHigh'])-high))
    require(error<1e-4,'IR2% display endpoints changed')
    for f in (run/'radiance').glob('*.f32'):
        v=np.fromfile(f,dtype='<f4');require(v.size==m['width']*m['height'] and np.isfinite(v).all() and v.min()>0,'Invalid IR radiance')
    for f,h in read(case/'protected.json').items():require(sha(repo/f)==h,'Protected repository file changed')
    for f,h in read(case/'frozen-source.json').items():require(sha(old/'source'/f)==h,'Previous source changed')
    for f,h in read(case/'frozen-output.json').items():require(sha(old/f)==h,'Previous packaged file changed')
    native=read(case/'frozen-native.json')['nativeFrameSha256']
    for f,h in native.items():require(sha(prior/f)==h,'Previous native frame changed')
    sources=read(case/'source-sha256.json')
    for f,h in sources.items():require(sha(repo/f)==h and sha(case/'source'/f)==h,'Current source snapshot differs: '+f)
    runtime=repo/'build/histream-animation/runtime/Release';require(sha(runtime/'histream.exe')==sha(run/'provenance/histream.exe'),'Engine mismatch')
    require(sha(runtime/'shader/animation/mesh/sequence_varied.comp.spv')==sha(run/'provenance/sequence.comp.spv'),'Growth render shader mismatch')
    require(sha(runtime/'shader/animation/mesh/transport.comp.spv')==sha(run/'provenance/transport.comp.spv'),'Growth transport shader mismatch')
    require(read(case/'tests_complete/verification.json')['status']=='passed' and read(case/'growth_gpu/verification.json')['status']=='passed' and read(case/'compatibility.json')['status']=='passed','Scoped tests incomplete')
    video=read(case/'video/verification.json');comparison=read(case/'comparison/verification.json');require(video['status']=='passed' and comparison['status']=='passed','Videos incomplete')
    for v in video['videos'].values():require(v['decodedFrames']==288 and sha(Path(v['path']))==v['sha256'],'Video hash/count differs')
    require(comparison['frames']==288 and sha(Path(comparison['path']))==comparison['sha256'],'Comparison differs')
    aux=case/'onboard_ir';require(read(aux/'result_final/run-manifest.json')['status']=='complete' and sha(aux/'onboard_ir.png')==sha(aux/'result_final/observed_ir/frame_0025.png'),'Auxiliary IR incomplete')
    result=dict(status='passed',nativeFrames=288,seconds=12,wallSeconds=m['wallSeconds'],growthSeconds=[5,6.5,4],growthTest=read(case/'tests_complete/verification.json'),gpuGrowthTest=read(case/'growth_gpu/verification.json'),rigidExact=True,geometryFilesExact=geometry,backgroundFilesExact=len(bg),cameraSettingsExact=True,cameraMotionExact=True,preContactZeroJitterFrames=len(before),maxIrPercentileError=error,transportChangedAsIntended=True,finalNormalizedSmoke=float(transport[-1]['smoke_integral']),sourceFiles=len(sources),previousNativeFramesPreserved=len(native),videos=video['videos'],comparison=comparison,combustionPrediction=False,physicalCalibration=False)
    (case/'final-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result,indent=2))


if __name__=='__main__':main()
