"""Verify the native natural-appearance case and preserve its exact provenance."""
import argparse,json
from pathlib import Path
import numpy as np
from prepare_thermal_case import require,sha
from make_fire_trees_case import validate_tree_clearance


def read(p):return json.loads(p.read_text(encoding='utf-8'))


def main():
    p=argparse.ArgumentParser()
    for key in ['case','reference','repo']:p.add_argument(key,type=Path)
    a=p.parse_args();case,reference,repo=a.case.resolve(),a.reference.resolve(),a.repo.resolve();run,old=case/'result_final',reference/'result_final';m,prior=read(run/'run-manifest.json'),read(old/'run-manifest.json')
    require(m['status']=='complete' and m['appearanceVersion']=='natural-v2','Natural render incomplete')
    require(m['frameCount']==288 and m['fps']==24 and m['samplesPerPixel']==4,'Unexpected frame/sampling profile')
    require(m['timeline']==prior['timeline'],'Appearance changed rigid timeline')
    for file in ['rigid.csv','transport.csv']:require(sha(run/file)==sha(old/file),'Appearance changed physics diagnostics: '+file)
    background=0
    for f in (old/'provenance/background').glob('*'):require(sha(f)==sha(run/'provenance/background'/f.name),'Native background changed');background+=1
    current=read(case/'current-inputs.json');tree=read(case/current['treeMetadata']);validate_tree_clearance(read(case/'sequence.json'),tree['trees']);geometry=tree['geometry']
    require(geometry['treeCount']==40 and geometry['leafCount']==132000 and geometry['triangleCount']==589280,'Missing refined tree geometry')
    require(m['scenery']['thinLeafTaggedTriangles']==geometry['leafTriangles'],'Leaf tag/material mapping mismatch')
    for record,stats in zip(tree['trees'],tree['perTreeGeometry']):require(stats['actualRadialBoundMeters']<=record['conservativeCanopyRadiusMeters'],'Leaf geometry violates clearance bound')
    require(read(case/'tests_complete/verification.json')['status']=='passed' and read(case/'compatibility.json')['status']=='passed','Native tests incomplete')
    require((case/'geometry-v2.log').read_text().startswith('PASS:'),'Geometry test incomplete')
    for f in (run/'radiance').glob('*.f32'):
        values=np.fromfile(f,dtype='<f4');require(values.size==m['width']*m['height'] and np.isfinite(values).all() and values.min()>0,'Invalid IR/temperature samples')
    protected=read(case/'protected.json')
    for f,h in protected.items():require(sha(repo/f)==h,'Protected repository file changed')
    frozen=read(case/'frozen-source.json')
    for f,h in frozen.items():require(sha(reference/'source'/f)==h,'Prior source snapshot changed')
    outputs=read(case/'frozen-output.json')
    for f,h in outputs.items():require(sha(reference/f)==h,'Prior packaged artifact changed')
    native=read(case/'frozen-native.json')['nativeFrameSha256']
    for f,h in native.items():require(sha(old/f)==h,'Prior native image changed')
    sources=read(case/'source-sha256.json')
    for f,h in sources.items():require(sha(repo/f)==h and sha(case/'source'/f)==h,'Current source/snapshot mismatch')
    runtime=repo/'build/histream-animation/runtime/Release';require(sha(runtime/'histream.exe')==sha(run/'provenance/histream.exe'),'Engine mismatch')
    require(sha(runtime/'shader/animation/mesh/sequence_natural.comp.spv')==sha(run/'provenance/sequence.comp.spv'),'Natural shader mismatch')
    video=read(case/'video/verification.json');comparison=read(case/'comparison/verification.json');require(video['status']=='passed' and all(v['decodedFrames']==288 for v in video['videos'].values()) and comparison['status']=='passed' and comparison['frames']==288,'Video decoding incomplete')
    aux=case/'onboard_ir';require(read(aux/'result_final/run-manifest.json')['status']=='complete' and sha(aux/'onboard_ir.png')==sha(aux/'result_final/observed_ir/frame_0025.png'),'Onboard still incomplete')
    require(sha(aux/'result_final/provenance/histream.exe')==sha(runtime/'histream.exe'),'Onboard engine mismatch')
    receipt=dict(status='passed',device=m['device'],wallSeconds=m['wallSeconds'],treeGeometry=geometry,rigidTimelineExact=True,rigidCsvExact=True,transportCsvExact=True,originalBackgroundFilesExact=background,nativeTests=read(case/'tests_complete/verification.json'),compatibility=read(case/'compatibility.json'),sourceFiles=len(sources),protectedFiles=len(protected),previousSourceFiles=len(frozen),previousPackagedFiles=len(outputs),previousNativeFrames=len(native),videos=video['videos'],comparison=comparison,physicalCalibration=False,combustionPrediction=False,vegetationEnergyBalance=False)
    (case/'final-verification.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8');print(json.dumps(receipt,indent=2))


if __name__=='__main__':main()
