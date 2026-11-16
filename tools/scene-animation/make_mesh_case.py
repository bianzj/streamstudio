"""Create original reference meshes in metres; these are synthetic architectural assets.

No weapon, blast, penetration, damage or structural mechanics are calculated.
"""
import argparse
import hashlib
import json
from pathlib import Path


class Obj:
    def __init__(self):
        self.vertices = []
        self.faces = []

    def box(self, lower, upper, material):
        x,y,z=lower; X,Y,Z=upper
        assert X>x and Y>y and Z>z
        b=len(self.vertices)
        self.vertices.extend([(x,y,z),(X,y,z),(X,Y,z),(x,Y,z),(x,y,Z),(X,y,Z),(X,Y,Z),(x,Y,Z)])
        # Consistent outward normals on all six sides.
        for face in [(0,3,2,1),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)]:
            self.faces.extend([(material,[b+face[i]+1 for i in t]) for t in [(0,1,2),(0,2,3)]])

    def save(self, path):
        names=sorted({m for m,f in self.faces})
        path.with_suffix('.mtl').write_text(''.join(f'newmtl {m}\nKd 0.5 0.5 0.5\n' for m in names),encoding='utf-8')
        lines=[f'mtllib {path.with_suffix(".mtl").name}\n']
        lines += [f'v {x:.5f} {y:.5f} {z:.5f}\n' for x,y,z in self.vertices]
        current=None
        for m,f in self.faces:
            if m!=current: lines.append(f'usemtl {m}\n');current=m
            lines.append('f '+' '.join(map(str,f))+'\n')
        path.write_text(''.join(lines),encoding='utf-8')


def building(floors=7, width=21, depth=15):
    o=Obj(); h=3.5; height=floors*h
    # Slabs, thick perimeter columns, recessed window panels and spandrel bands.
    for level in range(floors+1):
        z=level*h
        o.box((0,0,z),(width,depth,z+0.25),'concrete')
        if level==floors: continue
        # Internal rooms/partitions and core are actual mesh volumes.
        o.box((width*.45,depth*.3,z+.25),(width*.55,depth*.7,z+h),'interior')
        o.box((0,depth*.52,z+.25),(width*.45,depth*.52+.16,z+h),'interior')
        for side in range(4):
            length=width if side<2 else depth
            bays=7 if side<2 else 5
            for k in range(bays):
                lo=k*length/bays;hi=(k+1)*length/bays
                if side<2:
                    y=0 if side==0 else depth-.35
                    o.box((lo,y,z+.25),(lo+.3,y+.35,z+h),'concrete')
                    o.box((lo+.3,y+.08,z+.35),(hi-.06,y+.27,z+1.02),'brick' if side==0 else 'concrete')
                    # Thin glass occupies a real opening, recessed behind frame.
                    for pane in range(2):
                        x=lo+.37+pane*(hi-lo-.48)/2
                        xe=lo+.37+(pane+1)*(hi-lo-.48)/2-.045
                        o.box((x,y+.12,z+1.12),(xe,y+.19,z+h-.12),'glass_warm' if (level+k+side)%4==0 else 'glass')
                    o.box((lo+.31,y+.05,z+1.03),(hi-.06,y+.30,z+1.11),'metal')
                    o.box(((lo+hi)/2,y+.05,z+1.11),((lo+hi)/2+.045,y+.30,z+h-.05),'metal')
                else:
                    x=0 if side==2 else width-.35
                    o.box((x,lo,z+.25),(x+.35,lo+.3,z+h),'concrete')
                    o.box((x+.08,lo+.3,z+.35),(x+.27,hi-.06,z+1.02),'brick')
                    o.box((x+.12,lo+.37,z+1.12),(x+.19,hi-.12,z+h-.12),'glass')
                    o.box((x+.05,lo+.3,z+1.03),(x+.30,hi-.06,z+1.11),'metal')
    # Roof parapets, lift overrun, ducts, plant, antenna base and entry canopy.
    for lo,hi in [((0,0,height),(width,.3,height+1)),((0,depth-.3,height),(width,depth,height+1)),((0,0,height),(.3,depth,height+1)),((width-.3,0,height),(width,depth,height+1))]:o.box(lo,hi,'concrete')
    o.box((7,5,height+.25),(13,10,height+3.4),'roof')
    for x in [2,15,18]:
        o.box((x,3,height+.25),(x+1.4,5,height+1.5),'plant')
        for zz in range(5):o.box((x,2.97,height+.4+zz*.18),(x+1.4,3.02,height+.46+zz*.18),'metal')
    o.box((7,-2,3.25),(14,.5,3.45),'metal')
    for x in [7.2,13.5]:o.box((x,-1.7,0),(x+.18,-1.52,3.25),'metal')
    return o


def main():
    p=argparse.ArgumentParser();p.add_argument('output',type=Path);a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=False)
    building().save(a.output/'office.obj')
    building(4,18,12).save(a.output/'neighbor.obj')
    ground=Obj()
    ground.box((-95,-80,-.5),(95,85,-.03),'asphalt')
    ground.box((-9,-6,-.03),(31,22,.18),'pavement')
    for x in range(-75,85,7):ground.box((x,-19,0),(x+3,-18.82,.015),'marking')
    for x in [-15,36]:ground.box((x,-70,-.02),(x+.28,65,.16),'pavement')
    ground.save(a.output/'ground.obj')
    materials=[]
    for name,color,e,t in [('concrete',[.55,.53,.49],.94,303),('brick',[.42,.25,.18],.94,307),('interior',[.48,.46,.41],.94,298),('glass',[.10,.18,.22],.84,294),('glass_warm',[.16,.22,.25],.84,298),('metal',[.12,.14,.15],.65,302),('roof',[.48,.49,.47],.93,309),('plant',[.29,.32,.32],.88,306),('asphalt',[.12,.13,.14],.96,310),('pavement',[.45,.45,.43],.94,305),('marking',[.78,.77,.66],.94,310)]:
        materials.append(dict(id=name,color=color,emissivity=e,temperatureK=t))
    assets=[dict(id=n,path=n+'.obj',sha256=hashlib.sha256((a.output/(n+'.obj')).read_bytes()).hexdigest(),upAxis='Z',units='m') for n in ['office','neighbor','ground']]
    def instance(name,asset,x,y,angle=0):
        import math
        c=math.cos(angle);s=math.sin(angle)
        return dict(id=name,asset=asset,transform=[c,-s,0,x,s,c,0,y,0,0,1,0,0,0,0,1])
    scene=dict(backend='mesh',schemaVersion=1,coordinates=dict(units='m',upAxis='Z',handedness='right'),width=1280,height=800,samples=2,
               materials=materials,assets=assets,instances=[instance('target','office',0,0),instance('ground','ground',0,0)],
               cameras=[dict(id='street',position=[43,-54,19],target=[10,7,11],up=[0,0,1],fovYDegrees=43),dict(id='roof',position=[42,-40,48],target=[9,7,12],up=[0,0,1],fovYDegrees=44),dict(id='rear',position=[-31,27,22],target=[10,7,11],up=[0,0,1],fovYDegrees=48)],
               thermal=dict(wavelengthMicrometers=10,displayMinK=280,displayMaxK=320,skyK=275),
               provenance=dict(assetOrigin='Original synthetic architectural reference generated by make_mesh_case.py',license='CC0-1.0',temperatureSource='prescribed per-material values, not energy balance'))
    (a.output/'single-building.json').write_text(json.dumps(scene,indent=2),encoding='utf-8')
    for i,(x,y,angle) in enumerate([(-39,3,0),(39,3,0),(-39,32,0),(0,34,0),(38,34,0),(-38,-48,0),(0,-49,0),(39,-48,0),(-70,10,.2),(68,8,-.15)]):
        scene['instances'].append(instance(f'neighbor_{i}','neighbor',x,y,angle))
    (a.output/'street-block.json').write_text(json.dumps(scene,indent=2),encoding='utf-8')
    (a.output/'asset-license.json').write_text(json.dumps(dict(origin='Generated original meshes; no third-party models',license='CC0-1.0',generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),assets=assets),indent=2),encoding='utf-8')


if __name__=='__main__': main()
