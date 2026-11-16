"""Bind native FacetEB states to the exact animation triangle geometry.

This performs numerical/source validation, not field accuracy validation.
"""
import argparse
import datetime as dt
import json
import math
import hashlib
from pathlib import Path

import numpy as np

from prepare_thermal_case import sha,load_world,require


def export(native,output,node_ids):
    require(not output.exists(),'Export output already exists; use a new directory')
    require(len(node_ids)>0 and len(set(node_ids))==len(node_ids),'Select unique nonempty native node IDs')
    receipt_path=native/'run-receipt.json'
    require(receipt_path.is_file(),'Missing pre-solve native input receipt; run a freshly prepared case')
    run_receipt=json.loads(receipt_path.read_text(encoding='utf-8'))
    require(run_receipt['status']=='completed' and run_receipt['returncode']==0 and run_receipt['inputsUnchanged'],'Native receipt is not completed/converged')
    for role,item in run_receipt['inputs'].items():require(sha(Path(item['path']))==item['sha256'],'Input changed after native solve: '+role)
    mapping_path=native/'mesh-mapping.json';mapping=json.loads(mapping_path.read_text(encoding='utf-8'))
    scene_path=Path(mapping['scenePath']);require(sha(scene_path)==mapping['sceneSha256'],'Source scene SHA256 mismatch')
    scene,world,materials,instances=load_world(scene_path)
    require(len(world)==mapping['triangleCount'],'Mapping triangle count mismatch')
    raw=native/mapping['dataFile'];require(sha(raw)==mapping['dataSha256'],'Mapping SHA256 mismatch')
    original_map=np.fromfile(raw,dtype='<f4').reshape(-1,11)
    require(np.array_equal(original_map[:,:9],world.reshape(-1,9)) and np.array_equal(original_map[:,9],materials) and np.array_equal(original_map[:,10],instances),'Mapping geometry/material/instance mismatch')
    index_path=native/'output/faceteb_run.json';index=json.loads(index_path.read_text(encoding='utf-8'))
    require(index['status']=='completed' and len(index['completed'])==index['requested']['endNodeExclusive']-index['requested']['startNode'],'Native nodes incomplete')
    require(all(x['converged'] and x['energyFailingFacetFraction']<.001 for x in index['completed']),'Native node not converged')
    geometry_path=native/'output/faceteb.json';geo=json.loads(geometry_path.read_text(encoding='utf-8'))
    require(geo['converged'] and geo['graph']['fragmentOverflow']==0 and geo['graph']['hashOverflow']==0,'Unconverged native geometry/visibility graph')
    vertices=np.asarray(geo['vertexPositions'],dtype='<f4').reshape(-1,3,3)
    require(len(vertices)==len(world)+mapping['nativeExtraBoundary']['facetCount'],'Native geometry count mismatch')
    converted=vertices[:len(world),:,[0,2,1]].copy();converted[:,:,1]*=-1;converted[:,:,2]-=.6
    error=float(np.max(np.abs(converted-world)));require(error<=1e-4,'Geometry differs from rendered scene: '+str(error))
    native_normals=np.cross(converted[:,1]-converted[:,0],converted[:,2]-converted[:,0]);world_normals=np.cross(world[:,1]-world[:,0],world[:,2]-world[:,0])
    require(np.all(np.sum(native_normals*world_normals,axis=1)>0),'Native winding changed')
    project_path=native/'project.json';project=json.loads(project_path.read_text(encoding='utf-8'))['configuration']
    history_path=native/'forcing-and-history.json';history=json.loads(history_path.read_text(encoding='utf-8'));require(sha(project_path)==history['nativeProjectSha256'],'Project SHA256 mismatch')
    require(project['control']['facetBackend']=='gpu' and geo['backend'].startswith('Vulkan GPU'),'Native solve was not GPU FacetEB')
    spectra={s['name']:s for s in project['spectra']}
    for mat in scene['materials']:require(abs(1-spectra[mat['id']]['refTir']-mat['emissivity'])<1e-7,'Emissivity mismatch')
    source_path=native/'solver-source-receipt.json';source=json.loads(source_path.read_text(encoding='utf-8'))
    require(sha(native/'engine.exe')==source['engineSha256']==run_receipt['engineSha256'],'Native engine SHA256 mismatch')
    weather_path=Path(project['meteo']['path']);weather=np.loadtxt(weather_path,skiprows=1)
    band_count=len(project['sensor']['bands'].split());record_floats=11+2*band_count
    sources=dict(project=project_path,forcing=weather_path,history=history_path,geometry=geometry_path,runIndex=index_path,
                 engine=native/'engine.exe',solverSourceReceipt=source_path,mapping=mapping_path,runReceipt=receipt_path)
    receipts=[];pending=[]
    for number in node_ids:
        node=next(x for x in index['completed'] if x['node']==number)
        state=native/'output/.facet_steps'/('energy_T='+node['token']+'.bin')
        values=np.fromfile(state,dtype='<f4').reshape(len(vertices),2,record_floats)
        require(np.isfinite(values).all() and np.all((values[:,:,0]>=150)&(values[:,:,0]<=2000)),'Invalid native state temperature')
        field=np.concatenate([world.reshape(-1,9),materials[:,None],values[:len(world),:,0]],axis=1).astype('<f4')
        binary=output/(f'node_{number:02d}.f32')
        local=dt.datetime(project['meteo']['year'],1,1,tzinfo=dt.timezone(dt.timedelta(hours=project['meteo']['timezone'])))+dt.timedelta(seconds=round((float(node['julianTime'])-1)*86400))
        require(local.strftime('%H-%M')==node['token'][-5:],'Native token/time mismatch')
        timestamp=local.isoformat()
        require(abs(float(weather[number,0])-node['julianTime'])<1e-5,'Forcing/native node time mismatch')
        z=math.radians(node['solarZenith']);az=math.radians(node['solarAzimuth']);sky_k=(weather[number,6]/5.670374419e-8)**.25
        paths=dict(sources,state=state)
        metadata=dict(kind='histream-background-field',schemaVersion=1,coordinates=scene['coordinates'],temperatureUnits='K',timestamp=timestamp,timeConvention='native-node-instantaneous-state',
            solver=dict(engine='HiStream FacetEB',revision=source['baseCommit'],uncommitted=source['uncommitted'],backend=geo['backend'],node=number,
                        converged=node['converged'],criterion=index['convergence']['criterion'],energyFailingFacetFraction=node['energyFailingFacetFraction']),
            sceneBinding=mapping['sceneBinding'],field=dict(path=binary.name,sha256=hashlib.sha256(field.tobytes()).hexdigest(),recordFloats=12,triangleCount=len(world),layout='triangle-interleaved-a-b-c-material-frontK-backK'),
            sky=dict(longwaveWm2=float(weather[number,6]),equivalentTemperatureK=sky_k),sunDirectionWorld=[math.sin(z)*math.cos(az),-math.sin(z)*math.sin(az),math.cos(z)],
            sources=[dict(role=role,path=str(path.resolve()),sha256=sha(path)) for role,path in paths.items()],
            mappingVerification=dict(maxVertexErrorM=error,interpolation='none',mappedTriangles=len(world),nativeExtraBoundary=mapping['nativeExtraBoundary']),
            forcingHistory=dict(syntheticWeather=True,stepSeconds=project['meteo']['dTime'],warmupHours=history['warmupHours'],initialTemperatureK=history['initialTemperatureK'],
                                airTemperatureC=float(weather[number,1]),windMps=float(weather[number,4]),incomingShortwaveWm2=float(weather[number,5]),incomingLongwaveWm2=float(weather[number,6])),
            limitations=history['limitations']+['transferred temperature is native per-side area-mean; unresolved within-triangle sun/shade mixture','generic greybody spectral rendering is not the original FacetRT radiosity renderer'],fieldValidation=False)
        target=output/(f'node_{number:02d}.json');pending.append((binary,field.tobytes(),target,metadata))
        receipts.append(dict(node=number,timestamp=timestamp,metadata=str(target),minK=float(field[:,10:].min()),maxK=float(field[:,10:].max())))
    output.mkdir(parents=True,exist_ok=False)
    for receipt,(binary,raw,target,metadata) in zip(receipts,pending):
        binary.write_bytes(raw);target.write_text(json.dumps(metadata,indent=2),encoding='utf-8');receipt['sha256']=sha(target)
    (output/'export-verification.json').write_text(json.dumps(dict(status='passed',triangleCount=len(world),maxVertexErrorM=error,nativeNodesConverged=len(index['completed']),fields=receipts,fieldValidation=False),indent=2),encoding='utf-8')
    return receipts


def main():
    p=argparse.ArgumentParser();p.add_argument('native',type=Path);p.add_argument('output',type=Path);p.add_argument('--nodes',type=int,nargs='+',default=[18,22]);a=p.parse_args()
    print(json.dumps(export(a.native.resolve(),a.output.resolve(),a.nodes),indent=2))


if __name__=='__main__':main()
