"""Exact prior natural-v2/legacy-camera regression against frozen native files."""
import argparse,json,subprocess
from pathlib import Path
from hashlib import sha256


def main():
    p=argparse.ArgumentParser()
    for k in ['engine','reference','case']:p.add_argument(k,type=Path)
    a=p.parse_args();prior=a.reference/'tests_complete/natural';out=a.case/'compatibility_native'
    r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(a.reference/'tests_complete/natural.json'),str(out)],capture_output=True)
    (a.case/'compatibility.log').write_bytes(r.stdout+r.stderr);assert r.returncode==0,(r.stdout+r.stderr)[-1600:]
    count=0
    for folder in ['frames','fixed_frames','optical_frames','observed_ir','observed_optical','detector_ir','radiance','geometry']:
        for f in (prior/folder).glob('*'):
            if f.is_file():
                assert sha256(f.read_bytes()).digest()==sha256((out/folder/f.name).read_bytes()).digest(),str(f);count+=1
    for name in ['transport.csv','observation.csv']:
        assert (prior/name).read_bytes()==(out/name).read_bytes(),name
    receipt=dict(status='passed',frozenNativeFilesExact=count,transportCsvExact=True,observationCsvExact=True,scope='Prior natural-v2 with optional style/regional/impact settings absent; complete reduced native fixture')
    (a.case/'compatibility.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8');print('PASS: prior natural-v2/camera and transport exactly match frozen fixture:',count,'files')


if __name__=='__main__':main()
