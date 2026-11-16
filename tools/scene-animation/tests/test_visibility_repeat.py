"""Repeated GPU visibility must agree on depth-quantized coplanar geometry."""
import argparse,json,os,subprocess
from pathlib import Path
import numpy as np
from test_lumped_scene import fixture

p=argparse.ArgumentParser();p.add_argument('engine',type=Path);p.add_argument('output',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
previous=None;edges=None
for repeat in range(4):
 root=a.output/str(repeat);project=fixture(root,0);data=json.loads(project.read_text());c=data['configuration']
 c['meteo'].update(end=1,timeStepSeconds=[10]);c['control']['spectralAccelerationWidth']=128
 for m in c['materials']:m['params'].pop('lumpedThermal',None)
 lines=[];count=0
 for layer in range(8):
  lines.append('g hot\n')
  for i in range(16):
   for j in range(16):
    x=-1+i/8;z=-1+j/8
    for v in [(x,1,z),(x+1/8,1,z),(x+1/8,1,z+1/8),(x,1,z+1/8)]:lines.append(f'v {v[0]} {v[1]} {v[2]}\n')
    lines.extend([f'f {count+1} {count+3} {count+2}\n',f'f {count+1} {count+4} {count+3}\n']);count+=4
 (root/'world.obj').write_text(''.join(lines),encoding='utf-8');c['objects']['items'][0]['meshes']=c['objects']['items'][0]['meshes'][:1]
 project.write_text(json.dumps(data),encoding='utf-8')
 with (root/'native.log').open('wb') as log:
  result=subprocess.run([str(a.engine),'eFacetEB',str(project),str(root/'output/faceteb.json')],stdout=log,stderr=subprocess.STDOUT,env=dict(os.environ,HISTREAM_FACET_RASTER_LAYERS='64',HISTREAM_FACET_ENERGY_BACKEND='cpu'))
 assert result.returncode==0,(repeat,result.returncode)
 index=json.loads((root/'output/faceteb_run.json').read_text());token=index['completed'][0]['token'];raw=np.fromfile(root/'output/.facet_steps'/('energy_T='+token+'.bin'),dtype='<f4')
 graph=json.loads((root/'output/faceteb.json').read_text())['graph']
 if previous is not None:assert np.array_equal(raw,previous) and graph==edges,'Repeated coplanar GPU visibility differs'
 previous=raw;edges=graph
print(json.dumps(dict(status='passed',repeats=4,coplanarFacets=4096,energyAndSunlitBitIdentical=True,graph=edges)))
