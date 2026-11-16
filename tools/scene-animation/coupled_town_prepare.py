"""Prepare the original display town and a separately mapped thermal mesh.

All material/operating parameters are declared game assumptions. Native FacetEB
owns the coupled temperatures; no temperature or display delta is painted here.
"""
import argparse,copy,hashlib,json,math,shutil
from pathlib import Path
import numpy as np
from prepare_thermal_case import load_world
from integrity import require


def sha(p):
    h=hashlib.sha256()
    with Path(p).open('rb') as f:
        for b in iter(lambda:f.read(4*1024*1024),b''):h.update(b)
    return h.hexdigest()


def read(p):return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def write(p,obj):Path(p).write_text(json.dumps(obj,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')


def clip_triangle(triangle,roi):
    polygon=[np.asarray(v,float) for v in triangle]
    for axis,value,sign in [(0,roi[0],1),(1,roi[1],1),(0,roi[2],-1),(1,roi[3],-1)]:
        output=[]
        for j,a in enumerate(polygon):
            b=polygon[(j+1)%len(polygon)]
            ia=sign*(a[axis]-value)>=-1e-10;ib=sign*(b[axis]-value)>=-1e-10
            if ia:output.append(a)
            if ia!=ib:
                t=(value-a[axis])/(b[axis]-a[axis]);output.append(a+t*(b-a))
        polygon=output
        if not polygon:return np.empty((0,3,3),float)
    parts=np.asarray([[polygon[0],polygon[j],polygon[j+1]] for j in range(1,len(polygon)-1)],float)
    if not len(parts):return np.empty((0,3,3),float)
    keep=np.linalg.norm(np.cross(parts[:,1]-parts[:,0],parts[:,2]-parts[:,0]),axis=1)>1e-9
    return parts[keep]


def subdivide_triangle(triangle,max_edge):
    t=np.asarray(triangle,float)
    n=max(1,int(math.ceil(max(np.linalg.norm(t[(i+1)%3]-t[i]) for i in range(3))/max_edge)))
    if n==1:return t[None].copy()
    a,b,c=t;u=(b-a)/n;v=(c-a)/n;parts=[]
    for i in range(n):
        for j in range(n-i):
            p=a+i*u+j*v;parts.append([p,p+u,p+v])
            if i+j<n-1:parts.append([p+u,p+u+v,p+v])
    return np.asarray(parts)


def thermal_properties(material):
    name=material['id'].lower()
    if any(x in name for x in ['asphalt','tarmac','runway','road']):return .12,160000.
    if any(x in name for x in ['glass','window','windshield']):return .18,12000.
    if any(x in name for x in ['leaf','foliage','canopy','crown']):return .18,1500.
    if any(x in name for x in ['wood','bark','trunk']):return .22,60000.
    if any(x in name for x in ['brick','concrete','stone','wall']):return .27,180000.
    # Explicit appearance-derived fallback, not measured broadband albedo.
    return float(np.clip(.08+.35*np.mean(material['color']),.08,.55)),60000.


def prepare(source,actors_path,output):
    source=Path(source).resolve();output=Path(output).resolve();actors_path=Path(actors_path).resolve()
    if output.exists():raise FileExistsError('Preserve old cases; output must be new: '+str(output))
    original,tri,mids,iids=load_world(source/'scene.json');config=read(actors_path)
    source_hashes={str(p):sha(p) for p in [source/'scene.json',source/'actors.json',source/'SHA256SUMS.json',actors_path]}
    for a in original['assets']:source_hashes[str(source/a['path'])]=sha(source/a['path'])
    output.mkdir();(output/'assets').mkdir();(output/'thermal/assets').mkdir(parents=True);(output/'source').mkdir()
    scene=copy.deepcopy(original)
    for a in scene['assets']:
        src=source/a['path'];target=output/a['path'];target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,target)
    write(output/'scene.json',scene);write(output/'actors.json',config)
    np.savez_compressed(output/'source_geometry.npz',triangles=tri,material=mids,instance=iids)
    names=[x['id'] for x in scene['instances']]
    near=iids!=names.index('scenery_expanded_ground')
    xy=tri[near,:,:2].reshape(-1,2)
    lo=np.floor(xy.min(0)/10)*10-30;hi=np.ceil(xy.max(0)/10)*10+30
    roi=[float(lo[0]),float(lo[1]),float(hi[0]),float(hi[1])]
    inside=np.all((tri[:,:,0]>=lo[0])&(tri[:,:,0]<=hi[0])&(tri[:,:,1]>=lo[1])&(tri[:,:,1]<=hi[1]),axis=1)
    raw=[];rm=[];ri=[];rs=[]
    raw.append(tri[inside]);rm.append(mids[inside]);ri.append(iids[inside]);rs.append(np.flatnonzero(inside))
    for j in np.flatnonzero(~inside):
        clipped=clip_triangle(tri[j],roi)
        if len(clipped):
            raw.append(clipped);rm.append(np.full(len(clipped),mids[j]));ri.append(np.full(len(clipped),iids[j]));rs.append(np.full(len(clipped),j))
    t=np.concatenate(raw);m=np.concatenate(rm);inst=np.concatenate(ri);source_index=np.concatenate(rs)
    normal=np.cross(t[:,1]-t[:,0],t[:,2]-t[:,0]);norm=np.linalg.norm(normal,axis=1)
    ground_ids=[names.index(n) for n in ['base_ground','scenery_expanded_ground','runway_road_field']]
    isground=np.isin(inst,ground_ids);flat=np.abs(normal[:,2])>.99*norm
    edge=np.maximum.reduce([np.linalg.norm(t[:,1]-t[:,0],axis=1),np.linalg.norm(t[:,2]-t[:,1],axis=1),np.linalg.norm(t[:,0]-t[:,2],axis=1)])
    limit=np.where(inst==names.index('scenery_expanded_ground'),20.,4.)
    refine=isground&flat&(edge>limit)
    groups=[t[~refine]];gm=[m[~refine]];gi=[inst[~refine]];gs=[source_index[~refine]]
    for j in np.flatnonzero(refine):
        parts=subdivide_triangle(t[j],float(limit[j]));groups.append(parts)
        gm.append(np.full(len(parts),m[j]));gi.append(np.full(len(parts),inst[j]));gs.append(np.full(len(parts),source_index[j]))
    t=np.concatenate(groups).astype('<f4');m=np.concatenate(gm).astype(int);inst=np.concatenate(gi).astype(int);source_index=np.concatenate(gs).astype(int)
    area=np.linalg.norm(np.cross(t[:,1]-t[:,0],t[:,2]-t[:,0]),axis=1)/2
    centres=t.mean(1);actor_by_id={a['id']:a for a in config['actors']};groups_meta={};binned=np.zeros(len(t),dtype=int)
    for iid in np.unique(inst):
        name=names[iid];mask=inst==iid
        if name in actor_by_id:
            actor=actor_by_id[name];capacity=actor['heated_mass_kg']*config['environment']['specific_heat_j_kg_k']
            inv=np.linalg.inv(np.asarray(scene['instances'][iid]['transform']).reshape(4,4))
            vertices=np.c_[tri[iids==iid].reshape(-1,3),np.ones(int((iids==iid).sum())*3)]@inv.T
            low,high=vertices[:,:3].min(0),vertices[:,:3].max(0)
            local=np.c_[centres[mask],np.ones(int(mask.sum()))]@inv.T;points=(local[:,:3]-low)/(high-low)
            shape=config['heat_shape'];w=np.full(len(points),shape['body_weight'])
            for hot in actor.get('hotspots',shape['hotspots']):w+=hot['strength']*np.exp(-.5*np.sum(((points-hot['center'])/hot['radius'])**2,axis=1))
            w=np.clip(w,0,1)
            for k,mid in enumerate(m[mask]):
                if any(token in scene['materials'][mid]['id'].lower() for token in ['window','glass','wheel','black']):w[k]*=.2
            binned[mask]=np.maximum(1,np.rint(w*8)).astype(int)
            raw_area=float(area[mask].sum());bounds=(t[mask].reshape(-1,3).max(0)-t[mask].reshape(-1,3).min(0))
            effective=float(min(raw_area,2*(bounds[0]*bounds[1]+bounds[0]*bounds[2]+bounds[1]*bounds[2])))
            groups_meta[name]=dict(group=name,capacityJPerK=.3*capacity,conductanceWPerK=actor['power_w']/12.,airConductanceWPerK=0,
                powerW=actor['power_w'],startSeconds=18*7200,stopSeconds=18*7200+600,initialK=298.15,
                maxSurfaceK=500,skinCapacityPerArea=.7*capacity/raw_area,convectiveScale=.5*effective/raw_area,kind='active-prop')
        elif iid in ground_ids:
            total=float(area[mask].sum())
            groups_meta[name]=dict(group=name,capacityJPerK=1e6*total,conductanceWPerK=4*total,airConductanceWPerK=0,
                powerW=0,startSeconds=0,stopSeconds=18*7200+1200,initialK=298.15,
                maxSurfaceK=360,skinCapacityPerArea=120000.,convectiveScale=.5,kind='lumped-deep-ground-reservoir')
            binned[mask]=8
    # Stable material grouping keeps the native and rendering triangle orders identical.
    keys=np.stack([m,inst,np.where(binned>0,binned,0)],axis=1)
    unique,new_m=np.unique(keys,axis=0,return_inverse=True);materials=[];physical=[]
    for mid,iid,level in unique:
        mat=copy.deepcopy(scene['materials'][mid]);name=names[iid];alpha,cap=thermal_properties(mat)
        mat['id']=f'm{mid}_i{iid}_w{level}';mat['thermalAlbedo']=alpha;mat['heatCapacityPerArea']=cap
        params=dict(heatCapacityPerArea=cap,convectiveScale=1)
        if name in groups_meta:
            g=groups_meta[name];core={k:v for k,v in g.items() if k not in ['skinCapacityPerArea','convectiveScale','kind']}
            core['weight']=float(level)/8
            params.update(heatCapacityPerArea=g['skinCapacityPerArea'],convectiveScale=g['convectiveScale'],lumpedThermal=core)
        materials.append(mat);physical.append(dict(name=mat['id'],type='Building',energyModel='solid',params=params))
    order=np.argsort(new_m,kind='stable');t=t[order];inst=inst[order];source_index=source_index[order];new_m=new_m[order];area=area[order]
    obj=output/'thermal/assets/thermal_world.obj'
    with obj.open('w',encoding='utf-8') as f:
        # Native mesh loading supplies the scene's material table through an
        # in-memory MTL reader, activated by an OBJ mtllib declaration.
        f.write('mtllib thermal_world.mtl\n')
        for v in t.reshape(-1,3):f.write(f'v {v[0]:.9g} {v[1]:.9g} {v[2]:.9g}\n')
        previous=-1
        for j,mid in enumerate(new_m):
            if mid!=previous:f.write('g '+materials[mid]['id']+'\nusemtl '+materials[mid]['id']+'\n');previous=mid
            f.write(f'f {3*j+1} {3*j+2} {3*j+3}\n')
    proxy=copy.deepcopy(scene);proxy['assets']=[dict(id='thermal_world',path='assets/thermal_world.obj',sha256=sha(obj),upAxis='Z',units='m')]
    proxy['instances']=[dict(id='thermal_world',asset='thermal_world',transform=np.eye(4).ravel().tolist())];proxy['materials']=materials
    proxy['provenance']=dict(purpose='Separate thermal discretization of the original display scene',roi=roi,groundMaxEdgeMeters=4,farGroundMaxEdgeMeters=20,sourceScene=str(source/'scene.json'))
    write(output/'thermal/scene.json',proxy)
    np.savez_compressed(output/'thermal/mapping.npz',source_triangle=source_index,source_instance=inst,material=new_m,areas=area)
    widths=[7200]*18+[60]*20;elapsed=np.cumsum(widths);weather=[]
    for sec in elapsed:
        hour=(sec/3600)%24;sw=max(0,850*math.sin(math.pi*(hour-6)/12));ta=26+6*math.cos(2*math.pi*(hour-14)/24)
        ea=.55*6.112*math.exp(17.67*ta/(ta+243.5));lw=340+20*max(0,math.sin(math.pi*(hour-6)/12))
        weather.append([179+sec/86400,ta,ea,1013.25,2.5,sw,lw])
    write(output/'coupling.json',dict(groups=groups_meta,timeStepSeconds=widths,warmupSeconds=18*7200,heatingSeconds=600,coolingSeconds=600,
        originalSceneSha256=sha(source/'scene.json'),thermalRoi=roi,originalTriangles=len(tri),thermalTriangles=len(t),syntheticWeather=True,
        limitations=['thermal domain is an explicit town ROI; far display terrain uses a declared boundary state','ground deep reservoir is lumped, not local 3D conduction','vegetation is dry opaque surfaces, no leaf EB or transpiration','all operating/material parameters are game assumptions, not measured signatures']))
    # Native Y-up transformation, with a declared 0.6 m vertical offset.
    native=t[:,:,[0,2,1]].copy();native[:,:,1]+=.6;native[:,:,2]*=-1
    width=2*math.ceil(float(np.abs(native[:,:,0]).max())+1);depth=2*math.ceil(float(np.abs(native[:,:,2]).max())+1);height=math.ceil(float(native[:,:,1].max())+2)
    for run_name,on in [('on',True),('off',False)]:
        out=output/'native'/run_name;out.mkdir(parents=True)
        model=out/'world.obj'
        with model.open('w',encoding='utf-8') as f:
            for v in native.reshape(-1,3):f.write(f'v {v[0]:.9g} {v[1]:.9g} {v[2]:.9g}\n')
            last=-1
            for j,mid in enumerate(new_m):
                if mid!=last:f.write('g '+materials[mid]['id']+'\n');last=mid
                f.write(f'f {3*j+1} {3*j+2} {3*j+3}\n')
        (out/'origin.txt').write_text(f'{width/2} {depth/2} 0 1 0\n',encoding='utf-8')
        (out/'meteorology.txt').write_text(str(len(weather))+'\n'+''.join(' '.join(f'{v:.10f}' for v in row)+'\n' for row in weather),encoding='utf-8')
        mats=copy.deepcopy(physical)
        if not on:
            for mat in mats:
                if 'lumpedThermal' in mat['params']:mat['params']['lumpedThermal']['powerW']=0
        mats.append(dict(name='soil_boundary',type='Soil',params=dict(rss=1e9,cs=1800,rhos=1600,lambdas=1,SMC=.01)))
        c=dict(scene=dict(x=width,y=depth,height=height,voxel=max(width,depth)+1,terrain=False,
                background=dict(spectralName=materials[0]['id'],thermalName=materials[0]['id'],materialName='soil_boundary',materialType='Soil',heterogeneityEnabled=False)),
            sensor=dict(x=32,y=32,bands='10000',image=False,temperature=True,process=True,energyProcess=True,radiationProcess=False,principalPlane=False,hemisphere=False),
            control=dict(samples=8,depth=4,facetBackend='gpu',couplingIterations=100,temperatureTolerance=.05,temperatureRelaxation=.5,
                radiationSolver='traditional',spectralAccelerationWidth=500,soilTemperatureMethod=2,stabilityCorrection=False),
            meteo=dict(path=str(out/'meteorology.txt'),start=0,end=len(widths),dTime=7200,timeStepSeconds=widths,Tsold=298.15,z=35,latitude=40,longitude=120,timezone=8,year=2019),
            light=dict(zenith=30,azimuth=180,direct=.8,diffuse=.2,skyTemperature=280),atmosphere=dict(enabled=False),fluid=dict(enabled=False),
            spectra=[dict(name=m['id'],model='custom',reflectance=str(m['thermalAlbedo']),transmittance='0',refTir=1-m['emissivity'],tauTir=0) for m in materials],
            materials=mats,thermals=[dict(name=m['id'],sunlitTemperature=298.15,shadedTemperature=298.15) for m in materials],
            canopies=[dict(name='solid',structureType='solid',leafwidth=2)],
            objects=dict(count=1,names=['world'],items=[dict(name='world',type='Building',fileName=str(model),positionFile=str(out/'origin.txt'),
                meshes=[dict(name=m['id'],spectralName=m['id'],thermalName=m['id'],materialName=m['id'],canopyName='solid') for m in materials])]))
        write(out/'project.json',dict(format='histream-project',schemaVersion=1,name='Town coupled thermal '+run_name,mode='eFacetEB',configuration=c))
    require(read(output/'scene.json')['instances']==original['instances'] and read(output/'scene.json')['cameras']==original['cameras'], "check failed: read(output/'scene.json')['instances']==original['instances'] and read(output/'scene.json')['cameras']==original['cameras']")
    write(output/'source_inputs.json',source_hashes)
    write(output/'preparation.json',dict(status='passed',originalAssets=len(scene['assets']),originalInstances=len(scene['instances']),originalCamerasExact=True,
        originalMaterialsExact=scene['materials']==original['materials'],displayGeometryPreserved=True,thermalTriangles=len(t),thermalMaterials=len(materials),thermalRoi=roi,
        activeProps=len(config['actors']),coupledGroundReservoirs=sum(g['kind'].startswith('lumped-deep') for g in groups_meta.values()),thermalGroundFineMaxEdgeM=4))
    print(json.dumps(read(output/'preparation.json'),ensure_ascii=False),flush=True)


def main():
    p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('actors',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    prepare(a.source,a.actors,a.output)


if __name__=='__main__':main()
