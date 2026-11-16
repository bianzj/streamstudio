"""Original branch/leaf geometry with explicit normals and real canopy openings."""
import math, random
from pathlib import Path
import numpy as np


def unit(v):
    v=np.asarray(v,dtype=float);length=np.linalg.norm(v)
    if not np.isfinite(length) or length<1e-10:raise ValueError('Zero/invalid vegetation direction')
    return v/length


class Mesh:
    def __init__(self):self.vertices=[];self.normals=[];self.faces=[]
    def add(self,vertices,normals,faces,material):
        first=len(self.vertices);self.vertices.extend(tuple(v) for v in vertices);self.normals.extend(tuple(unit(n)) for n in normals)
        self.faces.extend((material,[first+i for i in face]) for face in faces)
    def merge(self,other):
        first=len(self.vertices);self.vertices.extend(other.vertices);self.normals.extend(other.normals);self.faces.extend((m,[first+i for i in f]) for m,f in other.faces)
    def box(self,lo,hi,material):
        x,y,z=lo;X,Y,Z=hi;p=np.array([(x,y,z),(X,y,z),(X,Y,z),(x,Y,z),(x,y,Z),(X,y,Z),(X,Y,Z),(x,Y,Z)])
        for face in [(0,3,2,1),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)]:
            v=p[list(face)];n=unit(np.cross(v[1]-v[0],v[2]-v[0]));self.add(v,[n]*4,[(0,1,2),(0,2,3)],material)
    def tube(self,start,end,r0,r1,material='bark',sectors=7):
        start,end=np.asarray(start,dtype=float),np.asarray(end,dtype=float);d=unit(end-start);side=unit(np.cross(d,[0,0,1] if abs(d[2])<.9 else [1,0,0]));up=np.cross(d,side)
        directions=[side*math.cos(k*2*math.pi/sectors)+up*math.sin(k*2*math.pi/sectors) for k in range(sectors)]
        normals=[unit(v+d*(r0-r1)/np.linalg.norm(end-start)) for v in directions]
        vertices=[p+r*v for p,r in [(start,r0),(end,r1)] for v in directions];faces=[]
        for k in range(sectors):
            j=(k+1)%sectors;faces.extend([(k,j,k+sectors),(j,j+sectors,k+sectors)])
        self.add(vertices,normals*2,faces,material)
        for p,r,n,reverse in [(start,r0,-d,True),(end,r1,d,False)]:
            v=[p]+[p+r*q for q in directions];f=[]
            for k in range(sectors):
                j=(k+1)%sectors;f.append((0,j+1,k+1) if reverse else (0,k+1,j+1))
            self.add(v,[n]*(sectors+1),f,material)
    def leaf(self,centre,normal,angle,length,width,bend,material):
        n=unit(normal);a=unit(np.cross([0,0,1] if abs(n[2])<.9 else [1,0,0],n));b=np.cross(n,a);u=a*math.cos(angle)+b*math.sin(angle);v=np.cross(n,u);p=np.asarray(centre)
        vertices=np.array([p+n*bend,p+u*length*.5,p+v*width*.5-u*length*.08,p-u*length*.5,p-v*width*.5-u*length*.08]);faces=[(0,1,2),(0,2,3),(0,3,4),(0,4,1)]
        normals=np.zeros((5,3))
        for f in faces:
            normal=np.cross(vertices[f[1]]-vertices[f[0]],vertices[f[2]]-vertices[f[0]])
            for k in f:normals[k]+=normal
        self.add(vertices,normals,faces,material)
    def save(self,path):
        path=Path(path);names=sorted({m for m,_ in self.faces});path.with_suffix('.mtl').write_text(''.join(f'newmtl {m}\nKd .5 .5 .5\n' for m in names),encoding='utf-8')
        with path.open('w',encoding='utf-8',newline='\n') as f:
            f.write(f'mtllib {path.with_suffix(".mtl").name}\n')
            for v in self.vertices:f.write('v '+' '.join(f'{q:.7f}' for q in v)+'\n')
            for n in self.normals:f.write('vn '+' '.join(f'{q:.8f}' for q in n)+'\n')
            previous=None
            for material,face in self.faces:
                if material!=previous:f.write(f'usemtl {material}\n');previous=material
                f.write('f '+' '.join(f'{q+1}//{q+1}' for q in face)+'\n')


def build_tree(record,seed=20261108):
    rng=random.Random(seed);mesh=Mesh();base=np.asarray(record['baseMeters'],dtype=float);height=record['heightMeters'];radius=record['crownGeneratorRadiusMeters'];sprays=[]
    trunk=[base+np.array([.025*height*math.sin(k*.9),.014*height*math.sin(k*1.3),height*z]) for k,z in enumerate([0,.26,.49,.75])]
    for k in range(3):mesh.tube(trunk[k],trunk[k+1],.17*(1-k*.24),.17*(1-(k+1)*.24),'bark',10)
    for i in range(5):
        angle=i*2.39996+rng.uniform(-.25,.25);root=base+np.array([0,0,height*rng.uniform(.30,.47)]);end=base+np.array([math.cos(angle)*radius*.7,math.sin(angle)*radius*.7,height*rng.uniform(.67,.78)])
        middle=root*.45+end*.55;middle[2]-=height*.04;mesh.tube(root,middle,.075,.043);mesh.tube(middle,end,.043,.023)
        for j in range(3):
            phi=angle+(j-1)*.48+rng.uniform(-.2,.2);r=radius*rng.uniform(.68,1.0);tip=base+np.array([math.cos(phi)*r,math.sin(phi)*r,height*rng.uniform(.64,.90)])
            mesh.tube(middle,tip,.025,.009,'bark',6)
            for k in range(2):
                theta=phi+(k-.5)*.8;leaf_tip=tip+np.array([math.cos(theta)*radius*.10,math.sin(theta)*radius*.10,height*rng.uniform(-.01,.03)])
                mesh.tube(tip,leaf_tip,.011,.004,'bark',5);sprays.append(leaf_tip)
    sprays.extend(base+np.array([math.cos(k*2.1)*radius*.22,math.sin(k*2.1)*radius*.22,height*.90]) for k in range(3))
    wood_triangles=len(mesh.faces);count=0
    for spray in sprays:
        for _ in range(100):
            z=rng.uniform(-1,1);phi=rng.uniform(0,2*math.pi);r=rng.random()**(1/3);q=np.array([math.sqrt(1-z*z)*math.cos(phi),math.sqrt(1-z*z)*math.sin(phi),z])*r
            centre=spray+q*np.array([radius*.38,radius*.33,height*.075]);yaw=rng.uniform(0,2*math.pi);tilt=rng.uniform(.25,1.25)
            normal=[math.sin(tilt)*math.cos(yaw),math.sin(tilt)*math.sin(yaw),math.cos(tilt)];length=rng.uniform(.17,.29);width=length*rng.uniform(.55,.80)
            mesh.leaf(centre,normal,rng.uniform(0,2*math.pi),length,width,length*rng.uniform(.025,.075),'leaf_'+str(rng.randrange(8)));count+=1
    positions=np.asarray(mesh.vertices);radial=np.linalg.norm(positions[:,:2]-base[:2],axis=1).max()
    if radial>record['conservativeCanopyRadiusMeters']:raise ValueError('Generated leaves exceed canopy clearance bound')
    return mesh,dict(leafCount=count,leafTriangles=count*4,woodTriangles=wood_triangles,triangleCount=len(mesh.faces),actualRadialBoundMeters=float(radial))
