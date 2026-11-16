"""Geometry contract for actual leaves and smooth branches, rather than solid crowns."""
import importlib.util, sys, tempfile
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))


def main():
    assert importlib.util.find_spec('natural_vegetation') is not None,'Missing porous leaf geometry generator'
    from natural_vegetation import build_tree
    record=dict(baseMeters=[0,0,.2],heightMeters=7,crownGeneratorRadiusMeters=1.8,conservativeCanopyRadiusMeters=2.88)
    mesh,stats=build_tree(record,seed=20261108)
    positions=np.asarray(mesh.vertices);normals=np.asarray(mesh.normals);indices=np.asarray([f for _,f in mesh.faces])
    assert len(positions)==len(normals) and np.isfinite(positions).all() and np.isfinite(normals).all()
    assert np.max(np.abs(np.linalg.norm(normals,axis=1)-1))<1e-5
    triangles=positions[indices];cross=np.cross(triangles[:,1]-triangles[:,0],triangles[:,2]-triangles[:,0]);area=np.linalg.norm(cross,axis=1)
    assert area.min()>1e-8 and np.min(np.sum(cross*normals[indices].mean(axis=1),axis=1))>0,'Winding disagrees with normals'
    assert stats['leafCount']>=2000 and stats['leafTriangles']==stats['leafCount']*4
    assert len(indices)<20000 and stats['woodTriangles']>200
    assert np.linalg.norm(positions[:,:2],axis=1).max()<=record['conservativeCanopyRadiusMeters']
    # Downward rays through the upper canopy must contain both foliage and holes.
    leaf=np.array([f for m,f in mesh.faces if m.startswith('leaf_')]);tri=positions[leaf];a,b,c=tri[:,0],tri[:,1],tri[:,2]
    e=b[:,:2]-a[:,:2];f=c[:,:2]-a[:,:2];det=e[:,0]*f[:,1]-e[:,1]*f[:,0];valid=np.abs(det)>1e-10
    hits=0;total=0
    for x in np.linspace(-2.4,2.4,36):
        for y in np.linspace(-2.4,2.4,36):
            if x*x+y*y>2.4**2:continue
            q=np.array([x,y])-a[:,:2];s=np.divide(q[:,0]*f[:,1]-q[:,1]*f[:,0],det,out=np.zeros_like(det),where=valid);t=np.divide(e[:,0]*q[:,1]-e[:,1]*q[:,0],det,out=np.zeros_like(det),where=valid)
            hits+=bool(np.any(valid&(s>=0)&(t>=0)&(s+t<=1)));total+=1
    fraction=hits/total;assert .2<fraction<.9,('Canopy should retain visible openings',fraction)
    with tempfile.TemporaryDirectory() as d:
        path=Path(d)/'tree.obj';mesh.save(path);text=path.read_text();assert '\nvn ' in text and '//' in text
    print('PASS: leaf geometry, normalized smooth normals, winding/area, canopy bounds, porous coverage',fraction,stats)


if __name__=='__main__':main()
