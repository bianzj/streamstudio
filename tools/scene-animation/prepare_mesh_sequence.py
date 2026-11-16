"""Create original preset fracture assets for the canonical synthetic building.

The cut volume is an authored animation preset, not a predicted damage radius.
"""
import argparse
import copy
import json
import math
from pathlib import Path

import numpy as np

from prepare_thermal_case import load_world,sha,require
from make_mesh_case import Obj


FACES=[(0,3,2,1),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)]


def subtract(lo,hi,cut_lo,cut_hi):
    a=np.maximum(lo,cut_lo);b=np.minimum(hi,cut_hi)
    if np.any(b-a<1e-6):return [(lo,hi)],None
    pieces=[];lower=lo.copy();upper=hi.copy()
    for axis in range(3):
        if a[axis]>lower[axis]+1e-6:
            end=upper.copy();end[axis]=a[axis];pieces.append((lower.copy(),end));lower[axis]=a[axis]
        if b[axis]<upper[axis]-1e-6:
            start=lower.copy();start[axis]=b[axis];pieces.append((start,upper.copy()));upper[axis]=b[axis]
    return pieces,(a,b)


def prepare(source,output,disjoint=False,min_thickness=0.0):
    scene,world,material_ids,instances=load_world(source)
    target=next(i for i,x in enumerate(scene['instances']) if x['id']=='target')
    require(scene['instances'][target]['transform']==[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],'Preset generator requires the canonical identity target')
    asset=next(x for x in scene['assets'] if x['id']==scene['instances'][target]['asset'])
    path=source.parent/asset['path'];require(asset['upAxis']=='Z','Preset target must be Z-up')
    verts=[];face_materials=[];active=None
    for line in path.read_text(encoding='utf-8').splitlines():
        p=line.split()
        if not p:continue
        if p[0]=='v':verts.append(list(map(float,p[1:4])))
        elif p[0]=='usemtl':active=p[1]
        elif p[0]=='f':face_materials.append(active)
    vertices=np.asarray(verts);require(len(vertices)%8==0 and len(face_materials)==len(vertices)//8*12,'Only original box-constructed reference asset is supported')
    output.mkdir(parents=True,exist_ok=False)
    remaining=Obj();fragments=Obj();links=[[],[]];chunks=[];occupied=[]
    offset=int(np.flatnonzero(instances==target)[0]);serial=0;kept_slivers=0
    cuts=[([6.4,-.8,8.25],[14.7,4.8,17.2]),([5.9,-.7,10.2],[6.5,1.5,15.1]),([14.6,-.7,9.5],[15.3,1.8,15.8]),([8.2,-.7,17.15],[12.8,2.8,17.8])]

    def emit(obj,lo,hi,parent_lo,parent_hi,material,parent_index,fragment):
        start=len(obj.faces);obj.box(lo,hi,material)
        for face_id,face in enumerate(FACES):
            axis=[2,2,1,0,1,0][face_id];side=[0,1,0,1,1,0][face_id]
            plane=lo[axis] if side==0 else hi[axis]
            original=abs(plane-(parent_lo[axis] if side==0 else parent_hi[axis]))<1e-5
            flag=(2 if fragment>=0 else 0)|(0 if original else 1)
            for k in range(2):
                if not original:obj.faces[start+face_id*2+k]=('fracture',obj.faces[start+face_id*2+k][1])
                # Remeshed face inherits the original per-triangle mean at its centroid.
                points=np.asarray([obj.vertices[i-1] for i in obj.faces[start+face_id*2+k][1]])
                centroid=points.mean(axis=0);chosen=k
                if original:
                    for candidate in [0,1]:
                        tri=world[offset+parent_index*12+face_id*2+candidate].astype(float)
                        u,v,p=tri[1]-tri[0],tri[2]-tri[0],centroid-tri[0]
                        d00=np.dot(u,u);d01=np.dot(u,v);d11=np.dot(v,v);d20=np.dot(p,u);d21=np.dot(p,v);den=d00*d11-d01*d01
                        aa=(d11*d20-d01*d21)/den;bb=(d00*d21-d01*d20)/den
                        if aa>=-1e-6 and bb>=-1e-6 and aa+bb<=1.000001:chosen=candidate;break
                links[1 if fragment>=0 else 0].append([offset+parent_index*12+face_id*2+chosen,fragment,flag])

    for parent in range(len(vertices)//8):
        box=vertices[parent*8:(parent+1)*8];lo=box.min(axis=0);hi=box.max(axis=0);material=face_materials[parent*12]
        pieces=[(lo,hi)];removed=[]
        for cut_lo,cut_hi in cuts:
            next_pieces=[]
            for a,b in pieces:
                left,cut=subtract(a,b,np.asarray(cut_lo),np.asarray(cut_hi));next_pieces.extend(left)
                if cut is not None:removed.append(cut)
            pieces=next_pieces
        for a,b in pieces:emit(remaining,a,b,lo,hi,material,parent,-1)
        disjoint_removed=[]
        for a,b in removed:
            parts=[(a,b)]
            if disjoint:
                for old_lo,old_hi in occupied:
                    next_parts=[]
                    for lower,upper in parts:next_parts.extend(subtract(lower,upper,old_lo,old_hi)[0])
                    parts=next_parts
                occupied.append((a,b))
            disjoint_removed.extend(parts)
        for a,b in disjoint_removed:
            if min_thickness>0 and float(np.min(b-a))<min_thickness:
                # Thin sliver: keep it as static structure instead of a rigid body pinched by heavy neighbours.
                emit(remaining,a,b,lo,hi,material,parent,-1);kept_slivers+=1;continue
            count=np.maximum(1,np.ceil((b-a)/np.array([1.5,1.6,2.4])).astype(int))
            for ix in range(count[0]):
                for iy in range(count[1]):
                    for iz in range(count[2]):
                        n=np.array([ix,iy,iz]);lower=a+(b-a)*n/count;upper=a+(b-a)*(n+1)/count
                        first=len(fragments.faces);emit(fragments,lower,upper,lo,hi,material,parent,serial)
                        seed=(math.sin((serial+1)*78.233)*43758.5453)%1
                        chunks.append(dict(id=serial,firstTriangle=first,triangleCount=12,center=((lower+upper)/2).tolist(),size=(upper-lower).tolist(),
                            launchDelay=.015+.23*seed,exitDuration=.18+.055*float((lower[1]+upper[1])/2),velocity=[(seed-.5)*4,-3.5-3*seed,1.5+2*seed],seed=seed))
                        serial+=1
    remaining.save(output/'remainder.obj');fragments.save(output/'fragments.obj')
    # Thin low-mass slivers between tonne-scale blocks are what pushes a rigid-body contact
    # solver into non-physical speeds; report them so a preset with many can be revised.
    minimum=[float(min(c['size'])) for c in chunks];slivers=sum(1 for m in minimum if m<.05)
    if slivers:print(f'WARNING: {slivers} of {len(chunks)} fragments are thinner than 5 cm (minimum {min(minimum)*100:.1f} cm); rigid-v1 would rely on its speed guard for them; rerun with --min-fragment-thickness 0.05 to keep them as static structure')
    for index,name in enumerate(['remainder','fragments']):
        (output/(name+'-links.json')).write_text(json.dumps(links[index],separators=(',',':')),encoding='utf-8')
    (output/'chunks.json').write_text(json.dumps(chunks,indent=2),encoding='utf-8')
    fracture=dict(id='fracture',color=[.58,.55,.49],emissivity=.94,temperatureK=300)
    def file(name):return dict(path=str((output/name).resolve()),sha256=sha(output/name))
    scene=copy.deepcopy(scene)
    for a in scene['assets']:a['path']=str((source.parent/a['path']).resolve())
    if 'backgroundField' in scene:scene['backgroundField']['path']=str((source.parent/scene['backgroundField']['path']).resolve())
    scene['backend']='meshSequence';scene['width']=1280;scene['height']=720;scene['samples']=1
    scene['sequence']=dict(fps=24,frameCount=288,impactTime=3,camera='street',targetInstance='target',sourcePoint=[10.2,-.35,12],projectileStart=[27,-24,23],
        fractureMaterial=fracture,remainder=file('remainder.obj'),remainderLinks=file('remainder-links.json'),fragments=file('fragments.obj'),fragmentLinks=file('fragments-links.json'),chunks=file('chunks.json'),
        sensor=True,media=True,exportFrames=[1,70,76,85,121,181,288],flowOrigin=[-22,-28,0],wind=[.8,.15],
        fixedDisplayMinK=280,fixedDisplayMaxK=480,scope='authored fracture partition, kinematic chunks and supplied heat/tracer sources; no blast or structural solver')
    scene['sequence']['landingArea']=dict(lower=[-5,-24],upper=[27,-6.7])
    (output/'sequence.json').write_text(json.dumps(scene,indent=2),encoding='utf-8')
    (output/'asset-record.json').write_text(json.dumps(dict(origin='Original synthetic preset assets',license='CC0-1.0',sourceScene=str(source),sourceSceneSha256=sha(source),
        targetTriangleCount=int((instances==target).sum()),remainderTriangles=len(remaining.faces),fragmentTriangles=len(fragments.faces),fragmentCount=len(chunks),
        cutBoxes=[dict(lower=a,upper=b) for a,b in cuts],damageIsPredicted=False,fragmentMinimumDimensionM=min(minimum),fragmentsUnder5cm=slivers,minFragmentThicknessM=min_thickness,sliversKeptAsStaticStructure=kept_slivers,
        breachAuthoredForSourcePoint=scene['sequence']['sourcePoint'],breachNote='opening, fireball and debris directions are authored for this flight path and a -Y facade; they do not follow other contact points'),indent=2),encoding='utf-8')
    if disjoint:
        receipt=json.loads((output/'asset-record.json').read_text(encoding='utf-8'));receipt['partition']='disjoint closed boxes of removed-volume union; source overlap assigned to first parent'
        (output/'asset-record.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    return output/'sequence.json'


def main():
    p=argparse.ArgumentParser();p.add_argument('scene',type=Path);p.add_argument('output',type=Path);p.add_argument('--disjoint',action='store_true');p.add_argument('--min-fragment-thickness',type=float,default=0.0,help='metres; removed pieces thinner than this stay as static structure (0 keeps every piece)');a=p.parse_args()
    if not 0<=a.min_fragment_thickness<=0.2:p.error('--min-fragment-thickness must be within 0..0.2 m')
    print(prepare(a.scene.resolve(),a.output.resolve(),a.disjoint,a.min_fragment_thickness))


if __name__=='__main__':main()
