"""Separate clear-optical / mildly blurred IR case, with display-only2% stretch."""
import argparse,copy,json
from pathlib import Path


def main():
    p=argparse.ArgumentParser();p.add_argument('reference',type=Path);p.add_argument('case',type=Path);a=p.parse_args()
    j=json.loads((a.reference/'sequence.json').read_text());c=j['sequence']['cameraObservation']
    c['irDisplay']='linear-percent-2';c['exposureSeconds']=1e-5
    c['rgb'].update(width=j['width'],height=j['height'],psfSigmaPixels=0,responseSeconds=0)
    c['ir']['psfSigmaPixels']=.65
    c['regionalBlur'].update(irSigmaSourcePixels=.8,rgbSigmaSourcePixels=0,irEdgeSigmaSourcePixels=.35,rgbEdgeSigmaSourcePixels=0)
    def save(path,data):
        if path.exists():raise FileExistsError(path)
        path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(data,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    save(a.case/'sequence.json',j)
    aux=json.loads((a.reference/'onboard_ir/sequence.json').read_text());aux['sequence']['cameraObservation']=copy.deepcopy(c);save(a.case/'onboard_ir/sequence.json',aux)
    print('Created separate clear RGB / mildly blurred IR2%-display inputs; native14bit IR retained')


if __name__=='__main__':main()
