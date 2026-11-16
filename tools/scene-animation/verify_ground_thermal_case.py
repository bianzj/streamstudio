"""Verify actual ground energy solutions and preservation of independent channels."""
import argparse,copy,csv,json,math
from pathlib import Path
import numpy as np
from PIL import Image
from prepare_thermal_case import require,sha


def read(p):return json.loads(p.read_text(encoding='utf-8'))
def rows(p):return list(csv.DictReader(p.open(encoding='utf-8')))


def main():
    p=argparse.ArgumentParser()
    for k in ['case','reference','repo']:p.add_argument(k,type=Path)
    a=p.parse_args();case,old,repo=a.case.resolve(),a.reference.resolve(),a.repo.resolve();run,prior=case/'result_final',old/'result_final';m,b=read(run/'run-manifest.json'),read(prior/'run-manifest.json')
    require(m['status']=='complete' and m['frameCount']==288 and m['fps']==24,'Full native run incomplete')
    require(m['timeline']==b['timeline'] and m['cameraObservation']==b['cameraObservation'] and m['scenery']==b['scenery'] and m['sustainedFire']==b['sustainedFire'],'Unrelated scene/camera changed')
    source=read(case/'sequence.json');copySource=copy.deepcopy(source);copySource['sequence'].pop('groundThermal');require(copySource==read(old/'sequence.json'),'Unexpected input changes')
    for name in ['rigid.csv','transport.csv']:require(sha(run/name)==sha(prior/name),'Independent simulation changed: '+name)
    rgb=0
    for folder in ['optical_frames','observed_optical','geometry']:
        for f in (prior/folder).glob('*'):
            if f.is_file():require(sha(f)==sha(run/folder/f.name),'RGB/geometry appearance changed');rgb+=1
    backgrounds=list((prior/'provenance/background').glob('*'))
    for f in backgrounds:require(sha(f)==sha(run/'provenance/background'/f.name),'Original background file changed')
    require([(x['id'],x['sha256']) for x in m['assets']]==[(x['id'],x['sha256']) for x in b['assets']],'Original geometry asset changed')
    g=m['groundThermal'];require(g['enabled'] and g['model']=='paired-local-ground-energy-balance' and not g['fullyCoupledFacetEB'],'Ground model missing/misattributed')
    nx,ny=g['gridPixels'];values=np.fromfile(run/'ground_thermal/cells.f32',dtype='<f4').reshape(ny,nx,20);valid=values[:,:,3]>=0
    require(np.isfinite(values).all() and int(valid.sum())==g['validReceivers'] and np.abs(values[:,:,19][valid]).max()<2,'Invalid ground EB state')
    require(np.allclose(values[:,:,18][valid],values[:,:,17][valid]-values[:,:,16][valid],atol=1e-5),'Paired temperature delta inconsistent')
    require(sha(run/'ground_thermal/cells.f32')==g['fieldSha256'],'Ground field hash mismatch')
    original=read(run/'ground_thermal/inputs.json');replay=read(run/'provenance/replay.json');ref=replay['sequence']['groundThermal'];require(not Path(ref['path']).is_absolute(),'Ground replay ref absolute')
    rp=(run/'provenance'/ref['path']).resolve();require(sha(rp)==ref['sha256'],'Ground replay parameters hash mismatch');replayInput=read(rp)
    for s in replayInput['sourceFiles']:
        require(not Path(s['path']).is_absolute(),'Ground replay source absolute');f=(rp.parent/s['path']).resolve();require(f.is_relative_to(run) and sha(f)==s['sha256'],'Ground replay source not bundled/changed')
    # Check the production history against its source data, not just linked hashes.
    field=Path(read(old/'sequence.json')['backgroundField']['path']);native=read(field);paths={s['role']:field.parent/s['path'] for s in native['sources']};weather=np.loadtxt(paths['forcing'],skiprows=1);project=read(paths['project'])['configuration'];index=read(paths['runIndex']);nodes={x['node']:x for x in index['completed']}
    require(len(original['history'])==native['solver']['node']+1,'Ground history coverage differs')
    for i,h in enumerate(original['history']):
        w=weather[i];n=nodes[i];z=math.radians(n['solarZenith']);az=math.radians(n['solarAzimuth']);expected=[math.sin(z)*math.cos(az),-math.sin(z)*math.sin(az),math.cos(z)]
        require(abs(h['seconds']-i*project['meteo']['dTime'])<1e-6 and np.allclose(h['sun'],expected,atol=1e-7),'Solar/time history differs')
        require(np.allclose([h['airK'],h['wind'],h['shortwave'],h['longwave']],[w[1]+273.15,w[4],w[5],w[6]],atol=1e-7),'Meteorology history differs')
    before=rows(run/'observation.csv');require(len(before)==288,'Observation rows missing');pre=[r for r in before if float(r['presentationTimeSeconds'])<=m['impactTime']]
    require(all(float(r[k])==0 for r in pre for k in ['jitterYawRadians','jitterPitchRadians','jitterRollRadians']),'Pre-contact jitter changed')
    camera=m['cameraObservation'];lo,hi=camera['ir']['adcSignalRange'];step=(hi-lo)/16383;error=0
    for n in range(1,289):
        codes=np.asarray(Image.open(run/f'detector_ir/frame_{n:04d}.png'));require(codes.shape==(216,384) and codes.max()<=16383,'Invalid IR ADC');x=np.sort(codes.ravel().astype(float));low=lo+x[int(.02*(len(x)-1))]*step;high=max(lo+x[int(.98*(len(x)-1))]*step,low+step)
        error=max(error,abs(float(before[n-1]['irAgcLow'])-low),abs(float(before[n-1]['irAgcHigh'])-high))
    require(error<1e-4,'IR2% display endpoints changed')
    a0=np.fromfile(run/'radiance/frame_0001_surfaceK.f32',dtype='<f4');b0=np.fromfile(prior/'radiance/frame_0001_surfaceK.f32',dtype='<f4');require(np.isfinite(a0).all() and np.count_nonzero(np.abs(a0-b0)>.1)>10,'No actual surfaceK change')
    for receipt in ['physics_final/verification.json','tests_final/verification.json','compatibility.json']:
        require(read(case/receipt)['status']=='passed','Native checks incomplete: '+receipt)
    require(read(case/'physics_final/verification.json')['relocatedReplayExact'],'Relocation check incomplete')
    for f,h in read(case/'protected.json').items():require(sha(repo/f)==h,'Protected file changed')
    for f,h in read(case/'frozen-source.json').items():require(sha(old/'source'/f)==h,'Previous source changed')
    for f,h in read(case/'frozen-output.json').items():require(sha(old/f)==h,'Previous packaged file changed')
    previousNative=read(case/'frozen-native.json')['nativeFrameSha256']
    for f,h in previousNative.items():require(sha(prior/f)==h,'Previous native image changed')
    sources=read(case/'source-sha256.json')
    for f,h in sources.items():require(sha(repo/f)==h and sha(case/'source'/f)==h,'Current source snapshot differs: '+f)
    runtime=repo/'build/histream-animation/runtime/Release';require(sha(runtime/'histream.exe')==sha(run/'provenance/histream.exe'),'Engine mismatch')
    require(sha(runtime/'shader/animation/ground_thermal.comp.spv')==sha(run/'ground_thermal/ground_thermal.comp.spv') and sha(runtime/'shader/animation/mesh/sequence_ground.comp.spv')==sha(run/'provenance/sequence.comp.spv'),'Ground shader mismatch')
    video=read(case/'video/verification.json');cmp=read(case/'comparison/verification.json');require(video['status']=='passed' and cmp['status']=='passed','Videos incomplete')
    for v in video['videos'].values():require(v['decodedFrames']==288 and sha(Path(v['path']))==v['sha256'],'Video hash/count differs')
    stats=read(case/'diagnostics/shade-statistics.json');require(stats['currentCanopyShadowCells']>0 and stats['currentShadowPairedMeanDeltaK']<0,'No daytime canopy cooling in matched model')
    aux=case/'onboard_ir';require(read(aux/'result_final/run-manifest.json')['status']=='complete' and sha(aux/'onboard_ir.png')==sha(aux/'result_final/observed_ir/frame_0025.png'),'Auxiliary image incomplete')
    require(sha(aux/'result_final/ground_thermal/cells.f32')==sha(run/'ground_thermal/cells.f32'),'Physical ground field depends on camera')
    receipt=dict(status='passed',frames=288,seconds=12,wallSeconds=m['wallSeconds'],groundModel=g,shadeStatistics=stats,actualGroundSurfaceChangedPixels=int((np.abs(a0-b0)>.1).sum()),rgbAndGeometryAppearanceFilesExact=rgb,geometryBindingsExact=True,rigidExact=True,transportExact=True,cameraSettingsExact=True,originalBackgroundFilesExact=len(backgrounds),maxIrPercentileError=error,historySourceVerified=True,groundReplayBundled=True,physics=read(case/'physics_final/verification.json'),sourceFiles=len(sources),previousNativeFramesPreserved=len(previousNative),videos=video['videos'],fullyCoupledFacetEB=False,fieldValidation=False)
    (case/'final-verification.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8');print(json.dumps(receipt,indent=2))


if __name__=='__main__':main()
