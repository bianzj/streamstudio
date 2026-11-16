"""Extend only the supplemental floor; preserve all native cameras and motion."""
import argparse,copy,json
from pathlib import Path
import numpy as np
from make_distant_approach_case import rebase
from verify_expanded_ground_case import read,sha,require

def save(p,x):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(x,indent=2),encoding='utf-8')
def main():
    p=argparse.ArgumentParser();p.add_argument('parent',type=Path);p.add_argument('case',type=Path);p.add_argument('--finalize',action='store_true');a=p.parse_args();parent,case=a.parent.resolve(),a.case.resolve()
    if a.finalize:
        b,m=read(case/'observer/result_final/run-manifest.json'),read(case/'result_final/run-manifest.json');require(b['status']==m['status']=='complete' and b['impactTime']==m['impactTime'],'Native runs incomplete or contact differs');original=read(parent/'case.json');original.update(reference=str(case/'observer'),referenceManifestSha256=sha(case/'observer/result_final/run-manifest.json'),scope='Same four-camera native simulation with extended supplemental floor; far-floor temperature is a prescribed background proxy');save(case/'case.json',original);print('Finalized same-time four-view metadata');return
    require(not (case/'sequence.json').exists(),'Case already configured');body=read(parent/'sequence.json');observer=read(parent/'observer/sequence.json');rebase(body,parent);rebase(observer,parent/'observer');oldRef=body['sequence']['scenery'];oldPath=Path(oldRef['path']);scenery=read(oldPath);rebase(scenery,oldPath.parent)
    field=parent/'result_final/ground_thermal';meta=read(field/'verification.json');values=np.fromfile(field/'cells.f32',dtype='<f4').reshape(-1,20);material=meta['materialNames'].index('asphalt');mask=(values[:,3]==material)&(values[:,12]>.99)&(values[:,13]>.99);require(mask.sum()>100,'Missing representative sunlit asphalt samples');temperature=float(np.median(values[mask,17]))
    inputs=case/'inputs';inputs.mkdir(parents=True);obj=inputs/'expanded_ground.obj';lines=['# Supplemental coplanar floor with exact central hole','mtllib materials.mtl','usemtl context_asphalt','vn 0 0 1'];vertices=0;triangles=0;inner=[-95.,95.,-80.,85.];rings=[]
    while inner!=[-12000.,12000.,-12000.,12000.]:
        outer=[x*2 for x in inner] if max(abs(x) for x in inner)<6000 else [-12000.,12000.,-12000.,12000.]
        x0,x1,y0,y1=inner;X0,X1,Y0,Y1=outer
        quads=[[(X0,Y0),(X1,Y0),(X1,y0),(X0,y0)],[(X0,y1),(X1,y1),(X1,Y1),(X0,Y1)],[(X0,y0),(x0,y0),(x0,y1),(X0,y1)],[(x1,y0),(X1,y0),(X1,y1),(x1,y1)]]
        for q in quads:
            for x,y in q:lines.append(f'v {x:.6f} {y:.6f} -0.030000')
            indices=[vertices+i for i in [1,2,3,4]];lines.extend([f'f {indices[0]}//1 {indices[1]}//1 {indices[2]}//1',f'f {indices[0]}//1 {indices[2]}//1 {indices[3]}//1']);vertices+=4;triangles+=2
        rings.append(dict(inner=inner,outer=outer));inner=outer
    obj.write_text('\n'.join(lines)+'\n',encoding='utf-8');mat=copy.deepcopy(next(m for m in body['materials'] if m['id']=='asphalt'));mat.update(id='context_asphalt',temperatureK=temperature);scenery['materials'].append(mat);scenery['assets'].append(dict(id='expanded_ground',path=str(obj),sha256=sha(obj),upAxis='Z',units='m'));scenery['instances'].append(dict(id='expanded_ground',asset='expanded_ground',transform=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]));scenery['provenance']['groundExtension']=dict(outerBoundsMeters=inner,innerHoleMeters=[-95,95,-80,85],zMeters=-.03,triangles=triangles,temperatureSource='median parent current direct-sun asphalt local field; prescribed background proxy, not new far-field EB')
    sceneryFile=inputs/'scenery.json';save(sceneryFile,scenery);ref=dict(enabled=True,path=str(sceneryFile),sha256=sha(sceneryFile))
    for j in [body,observer]:j['sequence']['scenery']=ref
    save(case/'sequence.json',body);save(case/'observer/sequence.json',observer)
    for name,j in [('body',body),('external',observer)]:
        pilot=copy.deepcopy(j);pilot.update(width=384,height=216,samples=1);pilot['sequence'].pop('groundThermal');pilot['sequence'].update(firstFrame=1,lastFrame=1,exportFrames=[1]);pilot['sequence']['cameraObservation']['ir'].update(width=128,height=72);pilot['sequence']['cameraObservation']['rgb'].update(width=384,height=216);save(case/f'pilot_{name}.json',pilot)
    save(case/'extent.json',dict(parent=str(parent),outerBoundsMeters=inner,originalFloorBoundsMeters=[-95,95,-80,85],floorZMeters=-.03,addedTriangles=triangles,rings=rings,farGroundTemperatureK=temperature,sunlitTemperatureSamples=int(mask.sum()),originalScenerySha256=oldRef['sha256'],expandedScenerySha256=ref['sha256'],expandedObjSha256=sha(obj),temperatureScope='Core ground EB retained; appended far-floor prescribed unshaded proxy, no full-domain energy-balance claim',cameraAndTrajectoryChanged=False))
    print('Added',triangles,'floor triangles, bounds',inner,'proxyK',temperature)
if __name__=='__main__':main()
