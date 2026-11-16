"""Refine the previous synthetic case without moving trees or changing heat/rigid inputs."""
import argparse,copy,hashlib,json,random
from pathlib import Path
import numpy as np
from natural_vegetation import Mesh,build_tree
from make_fire_trees_case import validate_tree_clearance


def main():
    p=argparse.ArgumentParser();p.add_argument('reference',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    ref,out=a.reference.resolve(),a.output.resolve();inputs=out/'inputs';inputs.mkdir(parents=True,exist_ok=False)
    base=json.loads((ref/'sequence.json').read_text());tree_meta=json.loads((ref/'inputs/trees.json').read_text());validate_tree_clearance(base,tree_meta['trees'])
    mesh=Mesh();stats=[];rng=random.Random(20261109)
    for i,record in enumerate(tree_meta['trees']):
        x,y,root=record['baseMeters'];ground=root-.18;pad=1.15
        mesh.box((x-pad,y-pad,ground),(x+pad,y+pad,ground+.08),'soil');mesh.box((x-pad+.13,y-pad+.13,ground+.08),(x+pad-.13,y+pad-.13,root),'grass')
        for lo,hi in [((x-pad,y-pad,ground),(x+pad,y-pad+.12,root+.04)),((x-pad,y+pad-.12,ground),(x+pad,y+pad,root+.04)),((x-pad,y-pad,ground),(x-pad+.12,y+pad,root+.04)),((x+pad-.12,y-pad,ground),(x+pad,y+pad,root+.04))]:mesh.box(lo,hi,'curb')
        tree,s=build_tree(record,seed=20261109+i*997);mesh.merge(tree);stats.append(s)
        for _ in range(25):
            centre=[x+rng.uniform(-.9,.9),y+rng.uniform(-.9,.9),root+.045];mesh.leaf(centre,[rng.uniform(-1,1),rng.uniform(-1,1),.3],rng.random()*6.28,.10,.018,.01,'grass')
    mesh.save(inputs/'natural_trees.obj')
    old_scene=json.loads((ref/'inputs/scenery.json').read_text());materials=[m for m in old_scene['materials'] if not m['id'].startswith('leaf_')]
    palette=[[.050,.16,.028],[.065,.22,.038],[.085,.26,.045],[.055,.19,.055],[.080,.23,.040],[.10,.27,.060],[.045,.18,.030],[.07,.21,.048]]
    temps=[296.3,297,297.7,296.7]
    for i,color in enumerate(palette):materials.append(dict(id=f'leaf_{i}',color=color,emissivity=.98,temperatureK=temps[i%4],roughness=.78,metallic=0,ior=1.45,thinLeaf=True))
    scene=copy.deepcopy(old_scene);scene['materials']=materials;scene['assets']=[dict(id='natural_trees',path='natural_trees.obj',sha256=hashlib.sha256((inputs/'natural_trees.obj').read_bytes()).hexdigest(),upAxis='Z',units='m')];scene['instances'][0]['asset']='natural_trees'
    totals=dict(treeCount=len(stats),leafCount=sum(s['leafCount'] for s in stats),leafTriangles=sum(s['leafTriangles'] for s in stats),woodTriangles=sum(s['woodTriangles'] for s in stats),triangleCount=len(mesh.faces),vertexNormals=len(mesh.normals))
    scene['provenance'].update(assetOrigin='Original explicitly curved leaf meshes, smooth tapered wood branches and sparse grass blades; no closed crown solids',seed=20261109,**totals)
    sf=inputs/'scenery.json';sf.write_text(json.dumps(scene,indent=2),encoding='utf-8');scenery=dict(enabled=True,path=str(sf),sha256=hashlib.sha256(sf.read_bytes()).hexdigest())
    tree_meta.update(seed=20261109,geometry=totals,perTreeGeometry=stats,canopy='Porous actual leaf surfaces; static geometry, prescribed temperatures')
    (inputs/'trees.json').write_text(json.dumps(tree_meta,indent=2),encoding='utf-8')
    base['samples']=2;base['sequence'].update(scenery=scenery,appearanceVersion='natural-v2');(out/'sequence.json').write_text(json.dumps(base,indent=2),encoding='utf-8')
    aux=out/'onboard_ir';aux.mkdir();j=json.loads((ref/'onboard_ir/sequence.json').read_text());j['samples']=2;j['sequence'].update(scenery=scenery,appearanceVersion='natural-v2');(aux/'sequence.json').write_text(json.dumps(j,indent=2),encoding='utf-8');(aux/'pose.json').write_text((ref/'onboard_ir/pose.json').read_text(),encoding='utf-8')
    (inputs/'asset-license.json').write_text(json.dumps(dict(origin=scene['provenance']['assetOrigin'],license='CC0-1.0',generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),geometryGeneratorSha256=hashlib.sha256((Path(__file__).parent/'natural_vegetation.py').read_bytes()).hexdigest(),assetSha256=scene['assets'][0]['sha256'],statistics=totals),indent=2),encoding='utf-8')
    print(json.dumps(totals))


if __name__=='__main__':main()
