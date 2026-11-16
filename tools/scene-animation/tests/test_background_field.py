"""Native GPU file-adapter checks using explicitly synthetic field fixtures."""
import argparse
import copy
import hashlib
import importlib.util
import json
import subprocess
from pathlib import Path

import numpy as np


def main():
    p=argparse.ArgumentParser();p.add_argument('engine',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=False)
    spec=importlib.util.spec_from_file_location('assets',Path(__file__).resolve().parents[1]/'make_mesh_case.py')
    mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
    obj=mod.Obj();obj.box((0,0,0),(2,6,4),'patch');obj.save(a.output/'box.obj')
    sha=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
    scene=dict(backend='mesh',schemaVersion=1,coordinates=dict(units='m',upAxis='Z',handedness='right'),width=65,height=65,samples=1,
               materials=[dict(id='patch',color=[.6,.4,.2],emissivity=.95,temperatureK=315)],
               assets=[dict(id='box',path='box.obj',sha256=sha(a.output/'box.obj'),upAxis='Y',units='m')],
               instances=[dict(id='one',asset='box',transform=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1])],
               cameras=[dict(id='front',position=[1,-10,3],target=[1,-2,3],up=[0,0,1],fovYDegrees=40),
                        dict(id='inside',position=[1,-2,3],target=[1,-5,3],up=[0,0,1],fovYDegrees=40)])
    vertices=np.asarray(obj.vertices,dtype='<f4');vertices=vertices[:,[0,2,1]];vertices[:,1]*=-1
    triangles=vertices[np.array([f for m,f in obj.faces])-1]
    records=np.zeros((12,12),dtype='<f4');records[:,:9]=triangles.reshape(12,9);records[:,10]=310;records[:,11]=290
    (a.output/'records.bin').write_bytes(records.tobytes())
    roles=['project','forcing','history','geometry','state','runIndex','engine','solverSourceReceipt','mapping']
    sources=[]
    for role in roles:
        f=a.output/(role+'.fixture');f.write_text(json.dumps(dict(role=role,syntheticAdapterTest=True)),encoding='utf-8')
        sources.append(dict(role=role,path=f.name,sha256=sha(f)))
    def source_json(role,data):
        f=a.output/(role+'.fixture');f.write_text(json.dumps(data),encoding='utf-8')
        next(x for x in sources if x['role']==role)['sha256']=sha(f)
    source_json('project',dict(configuration=dict(meteo=dict(year=2019,timezone=8))))
    source_json('runIndex',dict(status='completed',completed=[dict(node=0,converged=True,energyFailingFacetFraction=0,julianTime=180.5)]))
    receipt=dict(status='completed',returncode=0,inputsUnchanged=True,engineSha256=sha(a.output/'engine.fixture'),
                 inputs={role:dict(sha256=sha(a.output/(role+'.fixture'))) for role in ['project','forcing','history','mapping']})
    f=a.output/'runReceipt.fixture';f.write_text(json.dumps(receipt),encoding='utf-8');sources.append(dict(role='runReceipt',path=f.name,sha256=sha(f)))
    field=dict(kind='histream-background-field',schemaVersion=1,coordinates=scene['coordinates'],temperatureUnits='K',
               timestamp='2019-06-29T12:00:00+08:00',timeConvention='native-node-instantaneous-state',
               solver=dict(engine='HiStream FacetEB',revision='synthetic-test-fixture',converged=True,node=0,note='No EB solve in this adapter test'),
               sceneBinding=dict(assets=[dict(id='box',sha256=scene['assets'][0]['sha256'])],instances=scene['instances'],materialIds=['patch'],emissivities=[.95]),
               field=dict(path='records.bin',sha256=sha(a.output/'records.bin'),recordFloats=12,triangleCount=12,layout='triangle-interleaved-a-b-c-material-frontK-backK'),
               sky=dict(longwaveWm2=5.670374419e-8*275**4,equivalentTemperatureK=275),
               sunDirectionWorld=[0,-1,1],sources=sources)

    def execute(name,config=scene,metadata=field,success=True,contains=''):
        field_path=a.output/(name+'.field.json');field_path.write_text(json.dumps(metadata),encoding='utf-8')
        cfg=copy.deepcopy(config);cfg['backgroundField']=dict(path=field_path.name,sha256=sha(field_path),expectedTimestamp=field['timestamp'])
        source=a.output/(name+'.json');source.write_text(json.dumps(cfg),encoding='utf-8')
        result=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(source.resolve()),str((a.output/name).resolve())],capture_output=True)
        (a.output/(name+'.log')).write_bytes(result.stdout+result.stderr);text=(result.stdout+result.stderr).decode(errors='replace')
        assert (result.returncode==0)==success and contains.lower() in text.lower(),(name,result.returncode,text[-1500:])
        if not success:assert not (a.output/name).exists()
        return a.output/name

    out=execute('valid')
    for view,t in [('front',310),('inside',290)]:
        hit=np.fromfile(out/(view+'_hits.f32'),dtype='<f4').reshape(65,65,8)
        assert abs(hit[32,32,5]-t)<1e-5,(view,hit[32,32])
        temperature=np.fromfile(out/(view+'_temperatureK.f32'),dtype='<f4').reshape(65,65)
        assert np.array_equal(temperature,hit[:,:,5])
        center_radiance=np.fromfile(out/(view+'_centerRadiance.f32'),dtype='<f4').reshape(65,65)
        assert np.isfinite(center_radiance).all() and center_radiance.min()>0
    m=json.loads((out/'run-manifest.json').read_text());assert m['backgroundField']['coverageFraction']==1 and m['backgroundField']['temperatureUnits']=='K'
    cfg=copy.deepcopy(scene);cfg['thermalPerturbations']=[dict(instance='one',deltaKelvin=7)]
    plus=execute('delta_once',cfg)
    for view,t in [('front',317),('inside',297)]:
        hit=np.fromfile(plus/(view+'_hits.f32'),dtype='<f4').reshape(65,65,8);assert abs(hit[32,32,5]-t)<1e-5
    assert sha(a.output/'records.bin')==field['field']['sha256'],'loaded baseline must remain immutable'
    for name,change,error in [
        ('unconverged',lambda x:x['solver'].update(converged=False),'converged'),
        ('units',lambda x:x.update(temperatureUnits='C'),'units'),
        ('axes',lambda x:x['coordinates'].update(upAxis='Y'),'coordinates'),
        ('timestamp',lambda x:x.update(timestamp='2019-06-29T14:00:00+08:00'),'timestamp'),
        ('coverage',lambda x:x['field'].update(triangleCount=11),'coverage'),
        ('hash',lambda x:x['field'].update(sha256='0'*64),'sha256'),
        ('asset_identity',lambda x:x['sceneBinding']['assets'][0].update(sha256='0'*64),'binding'),
        ('instance_identity',lambda x:x['sceneBinding']['instances'][0].update(id='other'),'binding'),
        ('material_identity',lambda x:x['sceneBinding'].update(materialIds=['other']),'binding'),
        ('missing_source',lambda x:x.update(sources=x['sources'][:-1]),'source'),
    ]:
        bad=copy.deepcopy(field);change(bad);execute('bad_'+name,metadata=bad,success=False,contains=error)
    bad=copy.deepcopy(field);broken=records.copy();broken[0,0]+=.01;(a.output/'bad-geometry.bin').write_bytes(broken.tobytes())
    bad['field'].update(path='bad-geometry.bin',sha256=sha(a.output/'bad-geometry.bin'));execute('bad_geometry',metadata=bad,success=False,contains='geometry')
    bad=copy.deepcopy(field);broken=records.copy();broken[0,10]=np.nan;(a.output/'bad-temperature.bin').write_bytes(broken.tobytes())
    bad['field'].update(path='bad-temperature.bin',sha256=sha(a.output/'bad-temperature.bin'));execute('bad_temperature',metadata=bad,success=False,contains='temperature')
    bad=copy.deepcopy(field);f=a.output/'recertified-forcing.fixture';f.write_text('changed weather after solve',encoding='utf-8')
    next(x for x in bad['sources'] if x['role']=='forcing').update(path=f.name,sha256=sha(f))
    execute('recertified_forcing',metadata=bad,success=False,contains='changed since native solve')
    bad=copy.deepcopy(field);native_index=dict(status='completed',completed=[dict(node=0,converged=False,energyFailingFacetFraction=.1,julianTime=180.5)])
    f=a.output/'unconverged-index.fixture';f.write_text(json.dumps(native_index),encoding='utf-8')
    next(x for x in bad['sources'] if x['role']=='runIndex').update(path=f.name,sha256=sha(f))
    execute('unconverged_native_index',metadata=bad,success=False,contains='converged')
    replay=out/'provenance/replay.json';replayed=a.output/'replayed'
    result=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(replay.resolve()),str(replayed.resolve())],capture_output=True)
    assert result.returncode==0,result.stderr
    assert (replayed/'front_hits.f32').read_bytes()==(out/'front_hits.f32').read_bytes()
    print('PASS: exact front/back background field, additive delta once, identity/units/axes/time/coverage/hash guards and portable replay')


if __name__=='__main__':main()
