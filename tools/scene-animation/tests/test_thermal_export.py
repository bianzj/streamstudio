"""Production guards remain active under python -O; original run is read-only."""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path
import importlib.util


def main():
    p=argparse.ArgumentParser();p.add_argument('native',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    source=a.native.resolve();a.output.mkdir(parents=True,exist_ok=False);clone=a.output/'native-fixture';clone.mkdir()
    module_path=Path(__file__).resolve().parents[1]/'prepare_thermal_case.py'
    spec=importlib.util.spec_from_file_location('prepare',module_path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);sha=module.sha
    for name in ['project.json','meteorology.txt','forcing-and-history.json','mesh-mapping.json','mesh-mapping.bin','solver-source-receipt.json','engine.exe','preparation-receipt.json','run-receipt.json']:
        shutil.copy2(source/name,clone/name)
    (clone/'output/.facet_steps').mkdir(parents=True)
    for name in ['faceteb.json','faceteb_run.json']:shutil.copy2(source/'output'/name,clone/'output'/name)
    index=json.loads((source/'output/faceteb_run.json').read_text());node=next(x for x in index['completed'] if x['node']==18)
    filename='energy_T='+node['token']+'.bin';shutil.copy2(source/'output/.facet_steps'/filename,clone/'output/.facet_steps'/filename)
    project=json.loads((clone/'project.json').read_text());project['configuration']['meteo']['path']=str(clone/'meteorology.txt')
    (clone/'project.json').write_text(json.dumps(project,indent=2),encoding='utf-8')
    history=json.loads((clone/'forcing-and-history.json').read_text());history['nativeProjectSha256']=sha(clone/'project.json');(clone/'forcing-and-history.json').write_text(json.dumps(history),encoding='utf-8')
    receipt=json.loads((clone/'run-receipt.json').read_text())
    for role,name in [('project','project.json'),('forcing','meteorology.txt'),('history','forcing-and-history.json')]:receipt['inputs'][role]=dict(path=str(clone/name),sha256=sha(clone/name))
    (clone/'run-receipt.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    exporter=module_path.parent/'export_background_field.py'
    def call(name,ok,error=''):
        out=a.output/name;result=subprocess.run([sys.executable,'-O','-X','utf8',str(exporter),str(clone),str(out),'--nodes','18'],capture_output=True)
        text=(result.stdout+result.stderr).decode(errors='replace');(a.output/(name+'.log')).write_text(text,encoding='utf-8')
        assert (result.returncode==0)==ok and error.lower() in text.lower(),(name,result.returncode,text[-1500:])
        if not ok:assert not out.exists(),'Validation failure left export artifacts'
    call('valid_optimized',True)
    forcing=clone/'meteorology.txt';saved=forcing.read_bytes();rows=forcing.read_text().splitlines();row=rows[19].split();row[1]=str(float(row[1])+2);row[5]=str(float(row[5])+50);row[6]=str(float(row[6])+20);rows[19]=' '.join(row);forcing.write_text('\n'.join(rows)+'\n')
    call('changed_forcing_optimized',False,'changed after native solve');forcing.write_bytes(saved)
    old_index=(clone/'output/faceteb_run.json').read_bytes();bad=json.loads(old_index);bad['status']='non_converged';(clone/'output/faceteb_run.json').write_text(json.dumps(bad))
    call('unconverged_optimized',False,'incomplete');(clone/'output/faceteb_run.json').write_bytes(old_index)
    mapping=clone/'mesh-mapping.bin';saved=mapping.read_bytes();mapping.write_bytes(b'\0\0\x80?'+saved[4:])
    call('changed_mapping_optimized',False,'SHA256');mapping.write_bytes(saved)
    assert sha(source/'meteorology.txt')==json.loads((source/'run-receipt.json').read_text())['inputs']['forcing']['sha256']
    print('PASS: optimized Python rejects changed forcing, unconverged states and changed mapping before output; original inputs preserved')


if __name__=='__main__':main()
