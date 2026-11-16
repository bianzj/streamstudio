"""Export the actual mesh scene into a native FacetEB project, without solving."""
import argparse
import datetime as dt
import hashlib
import json
import math
from pathlib import Path

import numpy as np


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def require(condition,message):
    if not condition:raise ValueError(message)


def load_world(scene_path):
    scene=json.loads(scene_path.read_text(encoding='utf-8'));assets={};names=[m['id'] for m in scene['materials']]
    for asset in scene['assets']:
        path=scene_path.parent/asset['path'];require(sha(path)==asset['sha256'],'Asset SHA256 mismatch: '+str(path))
        vertices=[];faces=[];materials=[];material=None
        for line in path.read_text(encoding='utf-8').splitlines():
            parts=line.split()
            if not parts:continue
            if parts[0]=='v':vertices.append(list(map(float,parts[1:4])))
            elif parts[0]=='usemtl':material=parts[1]
            elif parts[0]=='f':
                if len(parts)!=4:raise ValueError('Thermal exporter requires the already triangulated reference OBJ')
                face=[int(i.split('/')[0]) for i in parts[1:]];faces.append([i-1 if i>0 else len(vertices)+i for i in face]);materials.append(names.index(material))
        v=np.asarray(vertices,dtype=np.float32)
        if asset['upAxis']=='Y':v=v[:,[0,2,1]];v[:,1]*=-1
        elif asset['upAxis']!='Z':raise ValueError('Unknown asset axis')
        assets[asset['id']]=(v[np.asarray(faces)],np.asarray(materials,dtype=np.int32))
    triangles=[];ids=[];instances=[]
    for index,instance in enumerate(scene['instances']):
        positions,materials=assets[instance['asset']];matrix=np.asarray(instance['transform'],dtype=np.float32).reshape(4,4)
        transformed=np.concatenate([positions,np.ones((*positions.shape[:2],1),dtype=np.float32)],axis=2)@matrix.T
        triangles.append(transformed[:,:,:3]);ids.append(materials);instances.extend([index]*len(positions))
    return scene,np.concatenate(triangles).astype('<f4'),np.concatenate(ids),np.asarray(instances,dtype='<u4')


def prepare(scene_path,output,nodes=25):
    scene,triangles,materials,instances=load_world(scene_path)
    require(nodes>0 and nodes<=1000,'Node count outside 1..1000')
    output.mkdir(parents=True,exist_ok=False)
    (output/'models').mkdir();(output/'positions').mkdir()
    # Bake transforms, preserve triangle order, map right-handed Z-up to Y-up.
    native=triangles[:,:,[0,2,1]].copy();native[:,:,1]+=.6;native[:,:,2]*=-1
    width=2*math.ceil(float(np.abs(native[:,:,0]).max())+1)
    depth=2*math.ceil(float(np.abs(native[:,:,2]).max())+1)
    height=math.ceil(float(native[:,:,1].max())+2)
    lines=[]
    for v in native.reshape(-1,3):lines.append('v '+' '.join(f'{float(x):.9g}' for x in v)+'\n')
    active=-1
    for i,m in enumerate(materials):
        if m!=active:lines.append(f'g {scene["materials"][m]["id"]}\n');active=m
        lines.append(f'f {3*i+1} {3*i+2} {3*i+3}\n')
    model=output/'models/world.obj';model.write_text(''.join(lines),encoding='utf-8')
    positions=output/'positions/origin.txt';positions.write_text(f'{width/2} {depth/2} 0 1 0\n',encoding='utf-8')
    records=np.concatenate([triangles.reshape(-1,9),materials[:,None].astype('<f4'),instances[:,None].astype('<f4')],axis=1).astype('<f4')
    (output/'mesh-mapping.bin').write_bytes(records.tobytes())
    binding=dict(assets=[dict(id=x['id'],sha256=x['sha256']) for x in scene['assets']],instances=scene['instances'],
                 materialIds=[x['id'] for x in scene['materials']],emissivities=[x['emissivity'] for x in scene['materials']])
    mapping=dict(kind='animation-faceteb-map',schemaVersion=1,scenePath=str(scene_path.resolve()),sceneSha256=sha(scene_path),
                 sceneBinding=binding,coordinates=scene['coordinates'],nativeToWorld=dict(formula='(x,y,z)->(x,-z,y-0.6)',upAxis='Y'),
                 nativeExtraBoundary=dict(groundHeightWorldM=-.61,facetCount=2,role='below-mesh soil boundary required by native FacetEB'),
                 triangleCount=len(triangles),dataFile='mesh-mapping.bin',recordFloats=11,dataSha256=sha(output/'mesh-mapping.bin'))
    (output/'mesh-mapping.json').write_text(json.dumps(mapping,indent=2),encoding='utf-8')
    weather=[]
    for i in range(nodes):
        hour=(2*i)%24;sw=max(0,850*math.sin(math.pi*(hour-6)/12));ta=26+6*math.cos(2*math.pi*(hour-14)/24)
        ea=.55*6.112*math.exp(17.67*ta/(ta+243.5));lw=340+20*max(0,math.sin(math.pi*(hour-6)/12))
        weather.append([179+i/12,ta,ea,1013.25,2.5,sw,lw])
    meteo=output/'meteorology.txt';meteo.write_text(str(nodes)+'\n'+''.join(' '.join(f'{v:.9f}' for v in row)+'\n' for row in weather),encoding='utf-8')
    albedos={'concrete':.35,'brick':.25,'interior':.4,'glass':.18,'glass_warm':.18,'metal':.3,'roof':.3,'plant':.28,'asphalt':.12,'pavement':.35,'marking':.7}
    capacities={'concrete':80000,'brick':100000,'interior':120000,'glass':15000,'glass_warm':15000,'metal':20000,'roof':90000,'plant':60000,'asphalt':120000,'pavement':100000,'marking':120000}
    spectra=[];physical=[];thermals=[];meshes=[]
    for m in scene['materials']:
        name=m['id'];spectra.append(dict(name=name,model='custom',reflectance=str(albedos[name]),transmittance='0',refTir=1-m['emissivity'],tauTir=0))
        physical.append(dict(name=name,type='Building',energyModel='solid',params=dict(heatCapacityPerArea=capacities[name],convectiveScale=1)))
        thermals.append(dict(name=name,sunlitTemperature=298.15,shadedTemperature=298.15))
        meshes.append(dict(name=name,spectralName=name,thermalName=name,materialName=name,canopyName='solid'))
    physical.append(dict(name='soil_boundary',type='Soil',params=dict(rss=1e9,cs=1800,rhos=1600,lambdas=1,SMC=.1)))
    config=dict(outDir=str((output/'output').resolve()),scene=dict(x=width,y=depth,height=height,voxel=max(width,depth)+1,terrain=False,
                background=dict(spectralName='asphalt',thermalName='asphalt',materialName='soil_boundary',materialType='Soil',heterogeneityEnabled=False)),
                sensor=dict(x=32,y=32,bands='10000',image=False,temperature=True,process=True,energyProcess=True,radiationProcess=False,principalPlane=False,hemisphere=False),
                control=dict(samples=8,depth=4,facetBackend='gpu',couplingIterations=100,temperatureTolerance=.05,temperatureRelaxation=.5,
                             radiationSolver='traditional',spectralAccelerationWidth=500,soilTemperatureMethod=0,stabilityCorrection=False),
                meteo=dict(path=str(meteo.resolve()),start=0,end=nodes,dTime=7200,Tsold=298.15,z=35,latitude=40,longitude=120,timezone=8,year=2019),
                light=dict(zenith=30,azimuth=180,direct=.8,diffuse=.2,skyTemperature=280),atmosphere=dict(enabled=False),fluid=dict(enabled=False),
                spectra=spectra,materials=physical,thermals=thermals,canopies=[dict(name='solid',structureType='solid',leafwidth=2)],
                objects=dict(count=1,names=['world'],items=[dict(name='world',type='Building',fileName=str(model.resolve()),positionFile=str(positions.resolve()),meshes=meshes)]))
    project=dict(format='histream-project',schemaVersion=1,name='Mesh background thermal field',mode='eFacetEB',configuration=config)
    (output/'project.json').write_text(json.dumps(project,indent=2),encoding='utf-8')
    (output/'forcing-and-history.json').write_text(json.dumps(dict(syntheticWeather=True,fieldValidation=False,year=2019,latitude=40,longitude=120,timezoneHours=8,
        startLocal='2019-06-28T00:00:00+08:00',stepSeconds=7200,nodeCount=nodes,warmupHours=24,initialTemperatureK=298.15,
        weatherColumns=['DOY_local','air_C','vapor_hPa','pressure_hPa','wind_m_s','SW_W_m2','LW_W_m2'],
        albedos=albedos,emissivities={m['id']:m['emissivity'] for m in scene['materials']},heatCapacityPerArea_J_m2_K=capacities,
        limitations=['idealized opaque two-sided facet thermal storage; no multilayer wall conduction','no measured weather or materials','2-hour time step and 24-hour initial warmup are demonstration settings'],
        sourceSceneSha256=sha(scene_path),nativeProjectSha256=sha(output/'project.json')),indent=2),encoding='utf-8')
    inputs=dict(project=output/'project.json',forcing=meteo,history=output/'forcing-and-history.json',mapping=output/'mesh-mapping.json',
                mappingData=output/'mesh-mapping.bin',model=model,placements=positions,sourceScene=scene_path)
    for asset in scene['assets']:inputs['asset_'+asset['id']]=scene_path.parent/asset['path']
    (output/'preparation-receipt.json').write_text(json.dumps(dict(kind='thermal-preparation-inputs',schemaVersion=1,
        inputs={role:dict(path=str(path.resolve()),sha256=sha(path)) for role,path in inputs.items()}),indent=2),encoding='utf-8')
    return output/'project.json'


def main():
    p=argparse.ArgumentParser();p.add_argument('scene',type=Path);p.add_argument('output',type=Path);p.add_argument('--nodes',type=int,default=25);a=p.parse_args()
    print(prepare(a.scene.resolve(),a.output.resolve(),a.nodes))


if __name__=='__main__':main()
