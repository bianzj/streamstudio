"""Original procedural trees + supplied sustained-fire demonstration, no raw edits."""
import argparse, copy, hashlib, json, math, random
from pathlib import Path
import numpy as np
from make_mesh_case import Obj


def tube(o, start, end, r0, r1, material, sectors=9):
    start,end=np.asarray(start),np.asarray(end);axis=end-start;axis/=np.linalg.norm(axis)
    side=np.cross(axis,[0,0,1] if abs(axis[2])<.9 else [1,0,0]);side/=np.linalg.norm(side);up=np.cross(axis,side)
    first=len(o.vertices)
    for centre,r in [(start,r0),(end,r1)]:
        for k in range(sectors):o.vertices.append(tuple(centre+r*(side*math.cos(k*2*math.pi/sectors)+up*math.sin(k*2*math.pi/sectors))))
    o.vertices.extend([tuple(start),tuple(end)])
    for k in range(sectors):
        j=(k+1)%sectors
        for face in [(k,j,k+sectors),(j,j+sectors,k+sectors),(2*sectors,j,k),(2*sectors+1,k+sectors,j+sectors)]:o.faces.append((material,[first+i+1 for i in face]))


def crown(o, centre, radii, material, rng):
    n,rings=12,5;first=len(o.vertices);cx,cy,cz=centre;rx,ry,rz=radii
    o.vertices.append((cx,cy,cz+rz))
    for j in range(1,rings+1):
        theta=j*math.pi/(rings+1)
        for k in range(n):
            phi=k*2*math.pi/n;f=rng.uniform(.86,1.13)
            o.vertices.append((cx+rx*math.sin(theta)*math.cos(phi)*f,cy+ry*math.sin(theta)*math.sin(phi)*f,cz+rz*math.cos(theta)*rng.uniform(.94,1.06)))
    bottom=len(o.vertices)-first;o.vertices.append((cx,cy,cz-rz))
    for k in range(n):
        q=(k+1)%n
        o.faces.append((material,[first+1,first+2+k,first+2+q]))
        o.faces.append((material,[first+bottom+1,first+2+(rings-1)*n+q,first+2+(rings-1)*n+k]))
    for j in range(rings-1):
        for k in range(n):
            q=(k+1)%n;a=first+1+j*n+k;b=first+1+j*n+q;c=first+1+(j+1)*n+k;d=first+1+(j+1)*n+q
            o.faces.extend([(material,[a+1,c+1,b+1]),(material,[b+1,c+1,d+1])])


def validate_tree_clearance(base, records):
    for tree in records:
        x,y,_=tree['baseMeters'];r=tree['conservativeCanopyRadiusMeters']
        assert y+r<=-28 or y-r>=-8,tree
        nearest=np.array([np.clip(x,-5,27),np.clip(y,-24,-6.7)])
        assert np.linalg.norm(np.array([x,y])-nearest)>r,tree
        for item in base['instances']:
            if item['id']=='ground':continue
            t=item['transform'];rotation=np.array([[t[0],t[1]],[t[4],t[5]]]);assert np.allclose(rotation.T@rotation,np.eye(2),atol=1e-6)
            point=rotation.T@(np.array([x,y])-np.array([t[3],t[7]]));w,d=(21,15) if item['asset']=='office' else (18,12)
            closest=np.clip(point,[0,0],[w,d]);assert np.linalg.norm(point-closest)>r,tree


def main():
    p=argparse.ArgumentParser();p.add_argument('reference',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    reference=a.reference.resolve();out=a.output.resolve();inputs=out/'inputs';inputs.mkdir(parents=True,exist_ok=False)
    base=json.loads((reference/'sequence.json').read_text(encoding='utf-8'));rng=random.Random(20261108)
    positions=[(x,y) for x in [-6,29] for y in [-3,5,13,21]]
    positions += [(x,27.5) for x in [0,7,14,21,28]]
    positions += [(x,-32) for x in [-12,-3,6,15,24,33,44,55]]
    positions += [(61,y) for y in [-3,7,19,27,35,47,57]]
    positions += [(-46,y) for y in [-2,16,25,47,57]]
    positions += [(x,56) for x in [-40,-28,-6,8,22,42,54]]
    trees=Obj();records=[]
    for index,(x,y) in enumerate(positions):
        x+=rng.uniform(-.18,.18);y+=rng.uniform(-.18,.18);ground=.18 if -9<x<31 and -6<y<22 else -.03
        height=rng.uniform(5.2,8.2);radius=rng.uniform(1.45,2.05);pad=1.15;root=ground+.18
        trees.box((x-pad,y-pad,ground),(x+pad,y+pad,ground+.08),'soil')
        trees.box((x-pad+.13,y-pad+.13,ground+.08),(x+pad-.13,y+pad-.13,root),'grass')
        for lo,hi in [((x-pad,y-pad,ground),(x+pad,y-pad+.12,root+.04)),((x-pad,y+pad-.12,ground),(x+pad,y+pad,root+.04)),((x-pad,y-pad,ground),(x-pad+.12,y+pad,root+.04)),((x+pad-.12,y-pad,ground),(x+pad,y+pad,root+.04))]:trees.box(lo,hi,'curb')
        tube(trees,[x,y,root],[x+.08,y-.05,root+height*.76],.16,.055,'bark')
        for group in range(9):
            ring=group//5;angle=group*2.39996+rng.uniform(-.25,.25);spread=radius*(.75 if ring==0 else .42)
            tip=np.array([x+math.cos(angle)*spread,y+math.sin(angle)*spread,root+height*(.64 if ring==0 else .83)+rng.uniform(-.25,.25)])
            tube(trees,[x,y,root+height*(.42 if ring==0 else .58)],tip,.06,.014,'bark',7)
            material='leaf_'+str((index+group)%4);crown(trees,tip,(radius*.62,radius*.58,height*.20),material,rng)
        records.append(dict(id=f'tree_{index:02d}',baseMeters=[x,y,root],heightMeters=height,crownGeneratorRadiusMeters=radius,conservativeCanopyRadiusMeters=radius*1.6,thermalSource='prescribed materials',rigidObstacle=False))
    trees.save(inputs/'trees.obj')
    materials=[]
    for name,color,emissivity,temperature in [('bark',[.19,.105,.048],.96,301),('leaf_0',[.055,.21,.035],.98,296.3),('leaf_1',[.08,.27,.043],.98,297.0),('leaf_2',[.11,.30,.055],.98,297.7),('leaf_3',[.065,.23,.075],.98,296.7),('soil',[.16,.10,.055],.96,302.3),('grass',[.09,.25,.045],.98,296.8),('curb',[.43,.42,.38],.94,303.5)]:
        materials.append(dict(id=name,color=color,emissivity=emissivity,temperatureK=temperature,roughness=.9,metallic=0,ior=1.5))
    scene=copy.deepcopy(base)
    for key in ['sequence','backgroundField','thermalPerturbations']:scene.pop(key,None)
    scene.update(backend='mesh',materials=materials,assets=[dict(id='trees',path='trees.obj',sha256=hashlib.sha256((inputs/'trees.obj').read_bytes()).hexdigest(),upAxis='Z',units='m')],instances=[dict(id='trees',asset='trees',transform=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1])],provenance=dict(assetOrigin='Original procedural trunks, branches, irregular crown clusters and planters',license='CC0-1.0',treeCount=len(records),seed=20261108,temperatureSource='prescribed tree/soil/grass material temperatures; no vegetation energy-balance solve',dynamics='static scenery; no tree burning or branch motion'))
    scene_file=inputs/'scenery.json';scene_file.write_text(json.dumps(scene,indent=2),encoding='utf-8')
    scenery_ref=dict(enabled=True,path=str(scene_file),sha256=hashlib.sha256(scene_file.read_bytes()).hexdigest())
    base['sequence']['scenery']=scenery_ref
    sources=[]
    for offset,radius,height,onset,temp,heat,tracer in [([-1.1,-1.1,-1.1],1.05,3.1,.45,1120,140,1.8),([1.5,-1.0,1.0],.8,2.7,.8,1080,120,1.5),([2.0,-4.0,-11.4],.9,2.4,1.8,1050,100,1.5)]:
        sources.append(dict(offsetMeters=offset,radiusMeters=radius,heightMeters=height,onsetSeconds=onset,durationSeconds=30,fadeSeconds=3,temperatureK=temp,heatKelvinPerSecond=heat,tracerRate=tracer))
    base['sequence']['sustainedFire']=dict(enabled=True,sources=sources)
    (out/'sequence.json').write_text(json.dumps(base,indent=2),encoding='utf-8')
    validate_tree_clearance(base,records)
    (inputs/'trees.json').write_text(json.dumps(dict(seed=20261108,treeCount=len(records),trees=records,exclusion='Conservative canopy circles1.6xgeneratorRadius and planters clear rotated building footprint rectangles, main street y=-28..-8 and authored debris landing rectangle; not a scanned-asset collision/vegetation solve'),indent=2),encoding='utf-8')
    onboard=out/'onboard_ir';onboard.mkdir();aux=json.loads((reference/'onboard_ir/sequence.json').read_text());aux['sequence']['scenery']=scenery_ref;aux['sequence']['sustainedFire']=base['sequence']['sustainedFire'];(onboard/'sequence.json').write_text(json.dumps(aux,indent=2),encoding='utf-8');(onboard/'pose.json').write_text((reference/'onboard_ir/pose.json').read_text(),encoding='utf-8')
    print(json.dumps(dict(treeCount=len(records),triangles=len(trees.faces),sources=len(sources),scenery=str(scene_file))))


if __name__=='__main__':main()
