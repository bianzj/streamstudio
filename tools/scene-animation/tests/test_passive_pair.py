"""Paired experiments must be identical before working heat is enabled."""
import argparse,json
from pathlib import Path
import numpy as np

p=argparse.ArgumentParser();p.add_argument('case',type=Path);p.add_argument('--node',type=int,default=17);a=p.parse_args()
arrays=[];edges=[]
for label in ['on','off']:
 root=a.case/'native'/label
 diagnostic=json.loads((root/'output/diagnostics'/f'facet_physics_node={a.node}.json').read_text())
 assert all(b['inputPower_W']==0 for b in diagnostic['lumpedThermal'])
 index=json.loads((root/'output/faceteb_run.json').read_text());node=next(n for n in index['completed'] if n['node']==a.node)
 arrays.append(np.fromfile(root/'output/.facet_steps'/('energy_T='+node['token']+'.bin'),dtype='<f4').reshape(-1,2,13))
 edges.append(json.loads((root/'output/faceteb.json').read_text())['graph']['directedEdges'])
delta=float(np.max(np.abs(arrays[0][:,:,0]-arrays[1][:,:,0])))
assert np.array_equal(arrays[0][:,:,8],arrays[1][:,:,8]),('Passive sunlit ownership differs',delta)
assert edges[0]==edges[1],('Passive visibility graph differs',edges)
assert np.array_equal(arrays[0][:,:,[0,9,10]],arrays[1][:,:,[0,9,10]]),('Passive temperatures differ',delta)
print(json.dumps(dict(status='passed',checks=3,node=a.node,maxPassiveTemperatureDifferenceK=delta,directedEdges=edges[0])))
