"""Extend authored CGI translation before the old start; retain fixed optics."""
import argparse,copy,json,math
from pathlib import Path
import numpy as np
from prepare_thermal_case import require,sha

def read(p):return json.loads(p.read_text(encoding='utf-8'))
def save(p,v):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(v,indent=2),encoding='utf-8')
def rebase(v,base):
    if isinstance(v,dict):
        for k,x in v.items():
            if k=='path' and isinstance(x,str) and not Path(x).is_absolute():v[k]=str((base/x).resolve())
            else:rebase(x,base)
    elif isinstance(v,list):
        for x in v:rebase(x,base)

def main():
    p=argparse.ArgumentParser();p.add_argument('parent',type=Path);p.add_argument('case',type=Path);p.add_argument('--finalize',action='store_true');a=p.parse_args();parent,case=a.parent.resolve(),a.case.resolve()
    if a.finalize:
        m=read(case/'observer/result_final/run-manifest.json');body=read(case/'result_final/run-manifest.json');require(m['status']==body['status']=='complete','Both native runs must complete');require(m['impactTime']==body['impactTime'],'Native contact mismatch')
        save(case/'case.json',dict(reference=str(case/'observer'),referenceManifestSha256=sha(case/'observer/result_final/run-manifest.json'),frameCount=m['frameCount'],fps=m['fps'],validFrames=body['frameCount'],lastSampleSeconds=(body['lastFrame']-1)/body['fps'],firstContactSeconds=m['impactTime'],cameraMount=read(case/'sequence.json')['sequence']['cameraMount'],synchronization='common native extended timeline, original24fps; no digital zoom/retiming; body signal lost after first contact',scope='Authored CGI translation pre-roll; no flight/attitude/guidance dynamics'))
        print('Finalized native shared contact and synchronization metadata');return
    require(not (case/'sequence.json').exists() and not (case/'observer/sequence.json').exists(),'Case already configured')
    body=read(parent/'sequence.json');observerRef=Path(read(parent/'case.json')['reference']);observer=read(observerRef/'sequence.json');oldManifest=read(parent/'result_final/run-manifest.json');rebase(body,parent);rebase(observer,observerRef)
    fps=body['sequence']['fps'];extra=9;shift=extra*fps;start=np.array(body['sequence']['projectileStart']);target=np.array(body['sequence']['sourcePoint']);duration=body['sequence']['impactTime'];far=start-(target-start)/duration*extra;count=body['sequence']['frameCount']+shift;estimated=oldManifest['impactTime']+extra;last=math.ceil(estimated*fps)
    for j in [body,observer]:j['sequence'].update(frameCount=count,impactTime=duration+extra,projectileStart=far.tolist(),firstFrame=1)
    body['sequence'].update(lastFrame=last,exportFrames=sorted(set([1,73,145,217,265,last])))
    observer['sequence'].update(lastFrame=count,exportFrames=sorted(set([1,217,last,last+1,last+12,last+48,count])))
    save(case/'sequence.json',body);save(case/'observer/sequence.json',observer)
    pilot=copy.deepcopy(body);pilot.update(width=384,height=216,samples=1);pilot['sequence'].pop('groundThermal');pilot['sequence'].update(firstFrame=1,lastFrame=1,exportFrames=[1]);pilot['sequence']['cameraObservation']['ir'].update(width=128,height=72);pilot['sequence']['cameraObservation']['rgb'].update(width=384,height=216);save(case/'pilot_first.json',pilot)
    save(case/'approach.json',dict(parent=str(parent),oldObserver=str(observerRef),prerollSeconds=extra,prerollFrames=shift,nominalStartDistanceM=float(np.linalg.norm(target-far)),oldNominalStartDistanceM=float(np.linalg.norm(target-start)),oldStart=start.tolist(),newStart=far.tolist(),nominalImpactSeconds=duration+extra,estimatedContactSeconds=estimated,expectedBodyFrames=last,expectedObserverFrames=count,fps=fps,fovYDegrees=55,digitalZoom=False,scope='CGI authored translation only; finite existing smoke domain retained'))
    print('Prepared',last,'body and',count,'external native frames; start distance',float(np.linalg.norm(target-far)),'m')
if __name__=='__main__':main()
