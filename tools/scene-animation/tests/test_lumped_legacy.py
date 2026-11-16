"""A pre-change native no-feature result is the legacy numerical reference."""
import argparse,json,os,subprocess
from pathlib import Path
from test_lumped_scene import fixture

p=argparse.ArgumentParser();p.add_argument('engine',type=Path);p.add_argument('reference',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
project=fixture(a.output,0);data=json.loads(project.read_text())
for m in data['configuration']['materials']:m['params'].pop('lumpedThermal',None)
data['configuration']['meteo'].pop('timeStepSeconds')
project.write_text(json.dumps(data),encoding='utf-8')
with (a.output/'native.log').open('wb') as log:
 r=subprocess.run([str(a.engine),'eFacetEB',str(project),str(a.output/'output/faceteb.json')],stdout=log,stderr=subprocess.STDOUT,
   env=dict(os.environ,HISTREAM_FACET_RASTER_LAYERS='64',HISTREAM_FACET_ENERGY_BACKEND='cpu'))
assert r.returncode==0,r.returncode
index=json.loads((a.output/'output/faceteb_run.json').read_text())
assert index['status']=='completed' and not index['lumpedThermalBodies']
old=json.loads((a.reference/'output/faceteb_run.json').read_text())
assert [(n['iterations'],n['energyResidual']) for n in index['completed']]==[(n['iterations'],n['energyResidual']) for n in old['completed']]
count=0
for f in (a.reference/'output/.facet_steps').glob('energy_T=*.bin'):
 assert f.read_bytes()==(a.output/'output/.facet_steps'/f.name).read_bytes(),f.name
 count+=1
print(json.dumps(dict(status='passed',checks=2,legacyEnergyBinariesIdentical=count,nodes=len(index['completed']))))
