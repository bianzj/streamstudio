"""Run existing native FacetEB with input hashes captured before and after solving."""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import subprocess

from prepare_thermal_case import sha,require


def run(case,engine,layers=256):
    receipt=case/'run-receipt.json'
    require(not receipt.exists(),'Native run receipt already exists; use a new prepared case')
    require(not (case/'output').exists(),'Native output already exists; use a new prepared case')
    prepared_path=case/'preparation-receipt.json'
    require(prepared_path.is_file(),'Missing pre-solve preparation receipt; prepare a fresh case')
    prepared=json.loads(prepared_path.read_text(encoding='utf-8'))
    inputs=dict(prepared['inputs']);inputs['preparation']=dict(path=str(prepared_path),sha256=sha(prepared_path))
    for role,item in inputs.items():require(sha(Path(item['path']))==item['sha256'],'Pre-solve input changed: '+role)
    engine_hash=sha(engine);snapshot=case/'engine.exe';snapshot.write_bytes(engine.read_bytes());require(sha(snapshot)==engine_hash,'Engine changed while snapshotting')
    value=dict(kind='native-faceteb-input-receipt',schemaVersion=1,status='running',startedUtc=dt.datetime.now(dt.timezone.utc).isoformat(),
               engine=str(engine),engineSha256=engine_hash,inputs=inputs,environment=dict(HISTREAM_FACET_RASTER_LAYERS=str(layers)))
    with receipt.open('x',encoding='utf-8') as f:json.dump(value,f,indent=2)
    with (case/'native-run.log').open('wb') as log:
        result=subprocess.run([str(engine),'eFacetEB',str(case/'project.json'),str(case/'output/faceteb.json')],stdout=log,stderr=subprocess.STDOUT,
                              env=dict(os.environ,HISTREAM_FACET_RASTER_LAYERS=str(layers)))
    value['returncode']=result.returncode;value['finishedUtc']=dt.datetime.now(dt.timezone.utc).isoformat()
    value['inputsUnchanged']=all(sha(Path(item['path']))==item['sha256'] for item in inputs.values()) and sha(engine)==engine_hash
    native_index=case/'output/faceteb_run.json'
    native_completed=native_index.is_file() and json.loads(native_index.read_text(encoding='utf-8'))['status']=='completed'
    value['status']='completed' if result.returncode==0 and value['inputsUnchanged'] and native_completed else 'failed'
    receipt.write_text(json.dumps(value,indent=2),encoding='utf-8')
    require(value['status']=='completed','Native solve failed or inputs changed; preserved run-receipt.json and log')
    print('PASS native FacetEB; pre/post input hashes unchanged',flush=True)


def main():
    p=argparse.ArgumentParser();p.add_argument('case',type=Path);p.add_argument('engine',type=Path);p.add_argument('--layers',type=int,choices=[64,128,256],default=256);a=p.parse_args()
    run(a.case.resolve(),a.engine.resolve(),a.layers)


if __name__=='__main__':main()
