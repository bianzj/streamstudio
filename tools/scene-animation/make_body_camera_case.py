"""Configure a body camera on the unchanged timeline; never mutate its source."""
import argparse,copy,json,math
from pathlib import Path
from prepare_thermal_case import sha

def main():
    p=argparse.ArgumentParser();p.add_argument('reference',type=Path);p.add_argument('case',type=Path);a=p.parse_args()
    ref,out=a.reference.resolve(),a.case.resolve();out.mkdir(parents=True,exist_ok=True)
    if (out/'sequence.json').exists():raise FileExistsError('Case configuration already exists')
    m=json.loads((ref/'result_final/run-manifest.json').read_text(encoding='utf-8'))
    j=json.loads((ref/'sequence.json').read_text(encoding='utf-8'));s=j['sequence'];fps=s['fps'];last=math.ceil(m['impactTime']*fps)
    camera=copy.deepcopy(next(c for c in j['cameras'] if c['id']==s['camera']));camera.update(id='body_nose',fovYDegrees=55);j['cameras'].append(camera)
    s.update(camera='body_nose',cameraMount=dict(body='projectile',offsetMeters=[0,0,.025]),firstFrame=1,lastFrame=last,exportFrames=sorted(set([1,min(25,last),min(49,last),last])))
    (out/'sequence.json').write_text(json.dumps(j,indent=2),encoding='utf-8')
    receipt=dict(reference=str(ref),referenceManifestSha256=sha(ref/'result_final/run-manifest.json'),frameCount=m['timelineFrameCount'],fps=fps,validFrames=last,lastSampleSeconds=(last-1)/fps,firstContactSeconds=m['impactTime'],cameraMount=s['cameraMount'],synchronization='same frame numbers/presentation time; no retiming; first post-contact displayed sample marks signal lost',scope='Camera on existing authored body motion; no guidance/attitude dynamics; original scene/thermal/transport unchanged')
    (out/'case.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8');print(json.dumps(receipt,indent=2))
if __name__=='__main__':main()
