"""Check actual ground-ray coverage and preservation of the native core scene."""
import argparse,copy,csv,json,hashlib
from pathlib import Path
import numpy as np
from PIL import Image

def read(p):return json.loads(p.read_text(encoding='utf-8'))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def require(ok,msg):
    if not ok:raise ValueError(msg)

def obj_mesh(path):
    vertices=[];faces=[];materials=[];material=None
    for line in path.read_text(encoding='utf-8').splitlines():
        p=line.split()
        if not p:continue
        if p[0]=='v':vertices.append([float(x) for x in p[1:4]])
        elif p[0]=='usemtl':material=p[1]
        elif p[0]=='f':
            require(len(p)==4,'Ground asset must be triangulated')
            faces.append([int(x.split('/')[0])-1 for x in p[1:]]);materials.append(material)
    v=np.asarray(vertices);f=np.asarray(faces);require(np.isfinite(v).all() and f.min()>=0 and f.max()<len(v),'Invalid ground OBJ')
    return v[f],materials

def area(poly):
    if len(poly)<3:return 0.
    p=np.asarray(poly);return abs(float(np.sum(p[:,0]*np.roll(p[:,1],-1)-p[:,1]*np.roll(p[:,0],-1))))*.5

def clip(poly,fn):
    if not len(poly):return []
    out=[];last=np.asarray(poly[-1]);a=fn(last)
    for p in poly:
        p=np.asarray(p);b=fn(p)
        if (a>=0)!=(b>=0):out.append(last+(p-last)*(a/(a-b)))
        if b>=0:out.append(p)
        last,a=p,b
    return out

def verify_geometry(case):
    extent=read(case/'extent.json');parent=Path(extent['parent']);original=read(parent/'sequence.json');sceneFile=case/'inputs/scenery.json';require(sha(sceneFile)==extent['expandedScenerySha256'],'Expanded scenery hash does not match extent receipt');scene=read(sceneFile)
    priorPath=Path(original['sequence']['scenery']['path']);prior=read(priorPath);require(sha(priorPath)==extent['originalScenerySha256'],'Original scenery changed')
    require(scene['materials'][:-1]==prior['materials'] and scene['instances'][:-1]==prior['instances'],'Original scenery materials/instances changed')
    require(len(scene['assets'])==len(prior['assets'])+1,'Unexpected supplemental assets')
    for a,b in zip(scene['assets'][:-1],prior['assets']):
        for key in ['id','sha256','upAxis','units']:require(a[key]==b[key],'Original scenery asset changed')
    asset=scene['assets'][-1];obj=(sceneFile.parent/asset['path']).resolve();require(sha(obj)==asset['sha256']==extent['expandedObjSha256'],'Expanded OBJ hash does not match receipts')
    tri,materials=obj_mesh(obj);require(len(tri)==extent['addedTriangles'] and set(materials)=={'context_asphalt'},'Unexpected ground geometry/material')
    groundAsset=next(x for x in original['assets'] if x['id']=='ground');groundInstance=next(x for x in original['instances'] if x['asset']=='ground');require(groundAsset['upAxis']=='Z' and groundAsset['units']=='m' and np.array_equal(np.array(groundInstance['transform']).reshape(4,4),np.eye(4)),'Coverage requires the original untransformed Z-up floor')
    oldTri,oldMaterials=obj_mesh((parent/groundAsset['path']).resolve());asphalt=oldTri[np.array(oldMaterials)=='asphalt'];z=float(asphalt[:,:,2].max());top=asphalt[np.all(np.abs(asphalt[:,:,2]-z)<1e-9,axis=1)];hole=[float(top[:,:,0].min()),float(top[:,:,0].max()),float(top[:,:,1].min()),float(top[:,:,1].max())]
    require(np.all(np.abs(tri[:,:,2]-z)<1e-9) and z==extent['floorZMeters'],'New floor is not coplanar with original')
    bounds=[float(tri[:,:,0].min()),float(tri[:,:,0].max()),float(tri[:,:,1].min()),float(tri[:,:,1].max())];require(bounds==extent['outerBoundsMeters'] and hole==extent['originalFloorBoundsMeters'],'Declared floor bounds differ from actual geometry')
    require(np.all(np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0])[:,2]>0),'Ground winding is not upward')
    for t in tri:
        p=list(t[:,:2])
        for axis,limit,sign in [(0,hole[0],1),(0,hole[1],-1),(1,hole[2],1),(1,hole[3],-1)]:p=clip(p,lambda q,axis=axis,limit=limit,sign=sign:sign*(q[axis]-limit))
        require(area(p)<1e-6,'New ground overlaps the original central hole')
    for i,t in enumerate(tri):
        for other in tri[:i]:
            p=list(t[:,:2])
            for a,b in zip(other[:,:2],np.roll(other[:,:2],-1,axis=0)):
                p=clip(p,lambda q,a=a,b=b:(b[0]-a[0])*(q[1]-a[1])-(b[1]-a[1])*(q[0]-a[0]))
            require(area(p)<1e-6,'New ground triangles overlap')
    total=sum(area(t[:,:2]) for t in tri);expected=(bounds[1]-bounds[0])*(bounds[3]-bounds[2])-(hole[1]-hole[0])*(hole[3]-hole[2]);require(abs(total-expected)<expected*1e-12,'Ground does not cover the complete annulus')
    for filename,run in [('sequence.json','result_final'),('observer/sequence.json','observer/result_final')]:
        source=case/filename;config=read(source);ref=config['sequence']['scenery'];require((source.parent/ref['path']).resolve()==sceneFile.resolve() and ref['sha256']==sha(sceneFile),'Scene configuration references different scenery')
        manifest=case/run/'run-manifest.json'
        if manifest.exists():
            m=read(manifest);require(m['sourceSha256']==sha(source)==sha(case/run/'provenance/input.json'),'Run manifest/config binding differs');require(m['scenery']['sourceSha256']==sha(sceneFile),'Rendered scenery differs from certificate');saved=next(x for x in m['scenery']['assets'] if x['id']=='expanded_ground');require(saved['sha256']==sha(obj) and saved['triangleCount']==len(tri),'Rendered ground asset differs from certificate')
    return dict(bounds=bounds,hole=hole,z=z,triangles=len(tri),objSha256=sha(obj),scenerySha256=sha(sceneFile))

def coverage(case,bounds):
    j=read(case/'sequence.json');m=read(case/'result_final/run-manifest.json') if (case/'result_final/run-manifest.json').exists() else None
    if m:contact=np.array(m['timeline']['firstContact']['projectileOriginMeters'])
    else:contact=np.array(read(Path(read(case/'extent.json')['parent'])/'result_final/run-manifest.json')['timeline']['firstContact']['projectileOriginMeters'])
    start=np.array(j['sequence']['projectileStart']);f=(contact-start)/np.linalg.norm(contact-start);right=np.cross(f,[0,0,1]);right/=np.linalg.norm(right);up=np.cross(right,f);offset=np.array(j['sequence']['cameraMount']['offsetMeters']);bodyPosition=start+right*offset[0]+up*offset[1]+f*offset[2]
    reference=read(case/'observer/sequence.json') if (case/'observer/sequence.json').exists() else read(Path(read(case/'case.json')['reference'])/'sequence.json');camera=next(c for c in reference['cameras'] if c['id']==reference['sequence']['camera'])
    forward=next(c for c in j['cameras'] if c['id']==j['sequence']['camera']);width,height=j['width'],j['height'];require((width,height)==(reference['width'],reference['height']),'View resolutions differ')
    poses=[('forward',bodyPosition,f,up,forward['fovYDegrees']),('external',np.array(camera['position']),np.array(camera['target'])-camera['position'],np.array(camera['up']),camera['fovYDegrees'])];results=[]
    x,y=np.meshgrid((np.arange(width)+.5)/width*2-1,1-(np.arange(height)+.5)/height*2)
    for name,p,d,u,fov in poses:
        d=d/np.linalg.norm(d);r=np.cross(d,u);r/=np.linalg.norm(r);u=np.cross(r,d);half=np.tan(np.deg2rad(fov/2));rays=d+x[:,:,None]*r*(width/height)*half+y[:,:,None]*u*half;rays/=np.linalg.norm(rays,axis=2)[:,:,None]
        with np.errstate(divide='ignore',invalid='ignore'):t=(-.03-p[2])/rays[:,:,2]
        ground=(rays[:,:,2]<0)&(t>0)&(t<=10000);hits=p+rays*t[:,:,None];outside=ground&((hits[:,:,0]<bounds[0])|(hits[:,:,0]>bounds[1])|(hits[:,:,1]<bounds[2])|(hits[:,:,1]>bounds[3]))
        results.append(dict(camera=name,traceableGroundRays=int(ground.sum()),outsideFloorRays=int(outside.sum())))
    return dict(boundsMeters=bounds,traceMaxMeters=10000,pixels=[width,height],views=results,scope='All opening pixel-centre rays; floor height verified against source OBJ, trace10000m is the unchanged native shader limit')

def main():
    p=argparse.ArgumentParser();p.add_argument('case',type=Path);p.add_argument('repo',type=Path);p.add_argument('--coverage-only',action='store_true');p.add_argument('--old-floor',action='store_true');a=p.parse_args();case,repo=a.case.resolve(),a.repo.resolve()
    geometry=None if a.old_floor else verify_geometry(case)
    bounds=[-95,95,-80,85] if a.old_floor else geometry['bounds'];certificate=coverage(case,bounds)
    require(all(v['outsideFloorRays']==0 for v in certificate['views']),'Finite floor boundary is visible within native primary ground-ray domain')
    if a.coverage_only:
        (case/'coverage-verification.json').write_text(json.dumps(dict(status='passed',**certificate),indent=2),encoding='utf-8');print(json.dumps(certificate));return
    spec=read(case/'extent.json');old=Path(spec['parent']);run=case/'result_final';observer=case/'observer/result_final';m,b=read(run/'run-manifest.json'),read(observer/'run-manifest.json');oldM=read(old/'result_final/run-manifest.json')
    require(m['status']==b['status']=='complete' and m['frameCount']==290 and b['frameCount']==504,'Incomplete native sequences')
    require(m['impactTime']==b['impactTime']==oldM['impactTime'],'Contact time changed')
    for filename in ['sequence.json','observer/sequence.json']:
        now=read(case/filename);before=read(old/filename);now['sequence']['scenery']=before['sequence']['scenery'];require(now==before,'Camera, trajectory or other input changed')
    for key in ['cameraObservation','assets','sustainedFire','timeline']:
        if key!='timeline':require(m[key]==oldM[key],'Original metadata changed: '+key)
    require(m['scenery']['triangleCount']==oldM['scenery']['triangleCount']+spec['addedTriangles'],'Unexpected geometry addition')
    require(sha(run/'ground_thermal/cells.f32')==sha(observer/'ground_thermal/cells.f32'),'Views use different physical ground fields')
    now=np.fromfile(run/'ground_thermal/cells.f32',dtype='<f4').reshape(-1,20);before=np.fromfile(old/'result_final/ground_thermal/cells.f32',dtype='<f4').reshape(-1,20)
    require(np.array_equal(now[:,:6],before[:,:6]) and np.array_equal(now[:,10:16],before[:,10:16]),'Receiver geometry/sky visibility/direct-solar factors changed')
    dSky=now[:,6:8]-before[:,6:8];dEmission=now[:,8:10]-before[:,8:10];changed=np.any(now!=before,axis=1);temperatureError=float(np.max(np.abs(now[:,16:18]-before[:,16:18])));noise=read(case/'sequence.json')['sequence']['cameraObservation']['ir']['noiseKelvin']
    # Existing coplanar planter soil/curb faces have an ambiguous equal-distance
    # material hit. The larger BLAS may choose the other face. Keep and report
    # the recomputed state, rather than claiming bitwise preservation.
    scenery=read(case/'inputs/scenery.json');soil=next(x for x in scenery['materials'] if x['id']=='soil');curb=next(x for x in scenery['materials'] if x['id']=='curb');slope=(curb['emissivity']*5.670374419e-8*curb['temperatureK']**4-soil['emissivity']*5.670374419e-8*soil['temperatureK']**4)/(soil['emissivity']-curb['emissivity']);mask=np.abs(dSky)>1e-7
    require(np.max(np.abs(dSky[:,0]-dSky[:,1]))<2e-7 and np.max(np.abs(dEmission[mask]/dSky[mask]-slope))<.1,'Longwave differences are not explained by the diagnosed material tie')
    require(temperatureError<noise and np.isfinite(now).all() and np.max(np.abs(now[:,19]))<2,'Core state difference exceeds this imaging-case budget')
    thermal=dict(sharedNewFieldExact=True,originalFieldBitwiseExact=bool(np.array_equal(now,before)),changedCells=int(changed.sum()),maxTemperatureDifferenceK=temperatureError,caseNoiseBudgetK=noise,pointSkyAndSolarExact=True,materialTieEmissionSlope=slope,cause='Original coplanar planter soil/curb equal-distance hits; see thermal-tie-diagnosis.json. Bound is an imaging-case comparison, not field-accuracy validation.')
    for filename in ['rigid.csv','transport.csv']:
        require(sha(run/filename)==sha(old/'result_final'/filename),'Body simulation changed')
        require(sha(observer/filename)==sha(old/'observer/result_final'/filename),'External simulation changed')
    require(sha(run/'camera.csv')==sha(old/'result_final/camera.csv'),'Mounted camera poses changed')
    four=case/'four_camera';v=read(four/'verification.json');require(v['status']=='passed' and v['decodedFrames']==504 and v['pixels']==[2560,1600] and sha(Path(v['video']))==v['sha256'],'Four-view output incomplete')
    require([x['channel'] for x in v['layout']]==['external_rgb','external_ir','forward_ir','forward_rgb'],'Wrong four-view order')
    for f,h in read(case/'frozen-native.json').items():require(sha(old/f)==h,'Previous native image changed')
    for f,h in read(case/'frozen-output.json').items():require(sha(old/f)==h,'Previous packaged output changed')
    for f,h in read(case/'frozen-source.json').items():
        before=old/'source'/f
        if not before.exists():before=old/'four_camera_combined_20261108'/Path(f).name
        require(sha(before)==h,'Previous source snapshot changed')
    for f,h in read(case/'protected.json').items():require(sha(repo/f)==h,'Protected source changed')
    sources=read(case/'source-sha256.json')
    for f,h in sources.items():require(sha(repo/f)==h and sha(case/'source'/f)==h,'Current source snapshot mismatch')
    require(sha(run/'provenance/histream.exe')==sha(observer/'provenance/histream.exe')==read(case/'source-version.json')['engineSha256'],'Engine provenance mismatch')
    require(read(case/'binding_tests_final/verification.json')['status']=='passed','Geometry binding guards not verified')
    receipt=dict(status='passed',bodyFrames=290,externalFrames=504,seconds=21,coverage=certificate,geometry=geometry,originalCameraAndTimelineExact=True,coreGroundComparison=thermal,rigidTransportExact=True,sourceFiles=len(sources),previousNativeImagesPreserved=len(read(case/'frozen-native.json')),addedTriangles=spec['addedTriangles'],farGroundTemperatureK=spec['farGroundTemperatureK'],farGroundEnergyBalanceSolved=False,video=v['video'],videoSha256=v['sha256'],scope='Extended background ground geometry; core solver/input unchanged but material-tie rerun differences reported; no full-domain outer-ground energy-balance validation')
    (case/'final-verification.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8');print(json.dumps(receipt,indent=2))
if __name__=='__main__':main()
