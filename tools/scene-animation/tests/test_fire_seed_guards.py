"""Reject integer seed overflow before any narrowing or GPU execution."""
import argparse,copy,json,subprocess
from pathlib import Path


def main():
    p=argparse.ArgumentParser()
    for k in ['engine','sequence','output']:p.add_argument(k,type=Path)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);b=json.loads(a.sequence.read_text());b['sequence'].update(firstFrame=1,lastFrame=1,exportFrames=[1])
    for i,value in enumerate([4294967297,18446744073709551615,-4294967295,1.5,0,1000000]):
        j=copy.deepcopy(b);j['sequence']['sustainedFire']['sources'][0]['seed']=value;f=a.output/f'invalid_{i}.json';f.write_text(json.dumps(j),encoding='utf-8')
        r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(f),str(a.output/f'invalid_{i}')],capture_output=True);(a.output/f'invalid_{i}.log').write_bytes(r.stdout+r.stderr)
        assert r.returncode!=0 and b'seed' in r.stdout+r.stderr,('Invalid seed accepted',value,r.returncode)
    (a.output/'verification.json').write_text(json.dumps(dict(status='passed',invalidSeedsRejected=6)),encoding='utf-8');print('PASS: six invalid seed values rejected')


if __name__=='__main__':main()
