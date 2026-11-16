"""Reject stale scene/OBJ receipts even when declared coverage is unchanged."""
import argparse,copy,json,subprocess,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from verify_expanded_ground_case import read,sha

def save(p,x):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(x,indent=2),encoding='utf-8')
def main():
    p=argparse.ArgumentParser();p.add_argument('case',type=Path);p.add_argument('repo',type=Path);p.add_argument('output',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);verifier=a.repo/'tools/scene-animation/verify_expanded_ground_case.py'
    for name in ['obj_hash','scene_hash','hole_geometry']:
        fixture=a.output/name;inputs=fixture/'inputs';inputs.mkdir(parents=True);obj=inputs/'expanded_ground.obj';obj.write_bytes((a.case/'inputs/expanded_ground.obj').read_bytes());scene=read(a.case/'inputs/scenery.json');scene['assets'][-1]['path']=str(obj.resolve())
        if name=='hole_geometry':
            text=obj.read_text();text=text.replace('v -190.000000 -160.000000 -0.030000','v 0.000000 0.000000 -0.030000',1);obj.write_text(text,encoding='utf-8')
        scene['assets'][-1]['sha256']=sha(obj);scenery=inputs/'scenery.json';save(scenery,scene)
        for config in ['sequence.json','observer/sequence.json']:
            j=read(a.case/config);j['sequence']['scenery']=dict(enabled=True,path=str(scenery.resolve()),sha256=sha(scenery));save(fixture/config,j)
        extent=copy.deepcopy(read(a.case/'extent.json'));extent.update(expandedObjSha256=sha(obj),expandedScenerySha256=sha(scenery))
        if name=='obj_hash':extent['expandedObjSha256']='0'*64
        if name=='scene_hash':extent['expandedScenerySha256']='0'*64
        save(fixture/'extent.json',extent)
        r=subprocess.run([sys.executable,'-X','utf8',str(verifier),str(fixture),str(a.repo),'--coverage-only'],capture_output=True);(a.output/(name+'.log')).write_bytes(r.stdout+r.stderr)
        assert r.returncode!=0,f'Invalid coverage receipt accepted: {name}'
        assert b'FileNotFoundError' not in r.stderr,'Fixture failed before exercising the binding'
    (a.output/'verification.json').write_text(json.dumps(dict(status='passed',rejected=['obj_hash','scene_hash','hole_geometry']),indent=2),encoding='utf-8');print('PASS: stale OBJ/scene hashes and actual hole corruption rejected')
if __name__=='__main__':main()
