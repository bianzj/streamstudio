"""Native mesh adapter checks: analytic hits, coordinate conversion and failure paths."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image


def main():
    p = argparse.ArgumentParser()
    p.add_argument('engine', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    # OBJ Y-up -> world Z-up: (x,y,z) -> (x,-z,y). Distinct 2/4/6 m sides.
    from importlib.util import spec_from_file_location, module_from_spec
    spec = spec_from_file_location('assets', Path(__file__).resolve().parents[1] / 'make_mesh_case.py')
    assets = module_from_spec(spec)
    spec.loader.exec_module(assets)
    obj = assets.Obj()
    obj.box((0, 0, 0), (2, 6, 4), 'patch')
    # World roof is the OBJ +Y face after the Y-up conversion.
    for i in [8,9]:obj.faces[i]=('top_patch',obj.faces[i][1])
    for i in [6,7]:obj.faces[i]=('side_patch',obj.faces[i][1])
    obj.save(a.output / 'fixture.obj')
    config = {
        'backend': 'mesh', 'schemaVersion': 1,
        'coordinates': {'units': 'm', 'upAxis': 'Z', 'handedness': 'right'},
        'width': 65, 'height': 65, 'samples': 1,
        'materials': [{'id': name, 'color': color, 'emissivity': 0.95, 'temperatureK': t} for name,color,t in [
            ('patch',[.7,.2,.1],315),('side_patch',[.2,.7,.1],305),('top_patch',[.1,.2,.7],295)]],
        'assets': [{'id': 'box', 'path': 'fixture.obj', 'sha256': hashlib.sha256((a.output/'fixture.obj').read_bytes()).hexdigest(), 'upAxis': 'Y', 'units': 'm'}],
        'instances': [{'id': 'one', 'asset': 'box', 'transform': [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]}],
        'cameras': [
            {'id': 'front', 'position': [1,-10,3], 'target': [1,-2,3], 'up': [0,0,1], 'fovYDegrees': 40},
            {'id': 'side', 'position': [10,-2,3], 'target': [1,-2,3], 'up': [0,0,1], 'fovYDegrees': 40},
            {'id': 'top', 'position': [1,-2,15], 'target': [1,-2,3], 'up': [0,1,0], 'fovYDegrees': 40}],
    }

    def run(name, data, ok=True, contains=''):
        source = a.output / (name + '.json')
        source.write_text(json.dumps(data), encoding='utf-8')
        result = subprocess.run([str(a.engine.resolve()), 'eSceneAnimation', str(source.resolve()), str((a.output/name).resolve())], capture_output=True)
        (a.output/(name+'.log')).write_bytes(result.stdout+result.stderr)
        text = (result.stdout+result.stderr).decode(errors='replace')
        assert (result.returncode == 0) == ok and contains.lower() in text.lower(), (name, result.returncode, text[-1500:])
        if not ok and name!='valid':assert not (a.output/name).exists(),(name,'invalid input created output')
        return a.output/name

    out = run('valid', config)
    m = json.loads((out/'run-manifest.json').read_text())
    assert m['sourceSha256']==hashlib.sha256((a.output/'valid.json').read_bytes()).hexdigest()
    assert m['backend'] == 'mesh' and m['triangleCount'] == 12
    assert m['worldBounds'] == [[0,-4,0],[2,0,6]], m['worldBounds']
    for name,distance,normal,material_id,temperature in [('front',6,[0,-1,0],0,315), ('side',8,[1,0,0],1,305), ('top',9,[0,0,1],2,295)]:
        hits = np.fromfile(out/(name+'_hits.f32'), dtype='<f4').reshape(65,65,8)
        center = hits[32,32]
        assert abs(center[0]-distance)<1e-4, (name, center)
        assert np.max(np.abs(center[2:5]-normal))<1e-5, (name, center)
        assert center[1] == material_id and center[5] == temperature, (name, center)
        assert np.isfinite(hits).all()
        radiance = np.fromfile(out/(name+'_radiance.f32'),dtype='<f4').reshape(65,65)
        expected = 0.95*1.191043934e8/(10**5*np.expm1(14388.29104/(10*temperature))) + 0.05*1.191043934e8/(10**5*np.expm1(14388.29104/(10*275)))
        assert abs(radiance[32,32]-expected)<3e-5, (radiance[32,32],expected)
        with Image.open(out/(name+'_thermal.png')) as im:
            im.verify()
    run('valid', config, False, 'already exists')
    bad=json.loads(json.dumps(config));bad['assets'][0]['path']='missing.obj'
    run('missing_asset', bad, False, 'asset')
    bad=json.loads(json.dumps(config));bad['materials'][0]['id']='unmapped'
    run('missing_material', bad, False, 'material')
    bad=json.loads(json.dumps(config));bad['assets'][0]['sha256']='0'*64
    run('hash_mismatch', bad, False, 'sha256')
    bad=json.loads(json.dumps(config));bad['instances'][0]['transform'][0]=0
    run('singular_transform', bad, False, 'transform')
    bad=json.loads(json.dumps(config));bad['cameras'][0]['target']=bad['cameras'][0]['position']
    run('invalid_camera', bad, False, 'camera')
    bad=json.loads(json.dumps(config));bad['cameras'][1]['id']='Front'
    run('camera_filename_collision', bad, False, 'camera')
    # Affine transform is applied after Y-up conversion, including normal inverse transpose.
    transformed=json.loads(json.dumps(config))
    transformed['instances'][0]['transform']=[0,-2,0,3, 1,0,0,5, 0,0,.5,1, 0,0,0,1]
    transformed['cameras']=[{'id':'rotated','position':[7,-5,2.5],'target':[7,6,2.5],'up':[0,0,1],'fovYDegrees':40}]
    moved=run('transformed',transformed)
    moved_hits=np.fromfile(moved/'rotated_hits.f32',dtype='<f4').reshape(65,65,8)
    assert abs(moved_hits[32,32,0]-10)<1e-4 and np.max(np.abs(moved_hits[32,32,2:5]-[0,-1,0]))<1e-5
    replay_input=out/'provenance/replay.json'
    result=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(replay_input.resolve()),str((a.output/'replayed').resolve())],capture_output=True)
    assert result.returncode==0,result.stderr
    assert (a.output/'replayed/front_hits.f32').read_bytes()==(out/'front_hits.f32').read_bytes()
    print('PASS: mesh GPU analytic hits, Y/Z axes, stride/index/normal/material/radiance, immutable outputs and input guards')


if __name__ == '__main__':
    main()
