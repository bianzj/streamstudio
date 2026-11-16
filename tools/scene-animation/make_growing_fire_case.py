"""Keep the latest camera/scene settings, add independent supplied growth times."""
import argparse,copy,json
from pathlib import Path


def main():
    p=argparse.ArgumentParser();p.add_argument('reference',type=Path);p.add_argument('case',type=Path);a=p.parse_args()
    def save(path,data):
        if path.exists():raise FileExistsError(path)
        path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(data,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    scene=json.loads((a.reference/'sequence.json').read_text())
    for source,seconds in zip(scene['sequence']['sustainedFire']['sources'],[5,6.5,4]):source['growthSeconds']=seconds
    save(a.case/'sequence.json',scene)
    aux=json.loads((a.reference/'onboard_ir/sequence.json').read_text());aux['sequence']['sustainedFire']=copy.deepcopy(scene['sequence']['sustainedFire']);save(a.case/'onboard_ir/sequence.json',aux)
    print('Created separate growing fire inputs; previous camera, scene and collision settings retained')


if __name__=='__main__':main()
