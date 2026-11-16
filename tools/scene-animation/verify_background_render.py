"""Verify GPU centre-ray temperature and uniform-region radiance against source facets."""
import argparse
import json
import math
from pathlib import Path

import numpy as np

from prepare_thermal_case import sha,require


def planck(wavelength,temperature):
    return 1.191043934e8/(wavelength**5*np.expm1(14388.29104/(wavelength*temperature)))


def verify(run):
    manifest=json.loads((run/'run-manifest.json').read_text(encoding='utf-8'))
    require(manifest['status']=='complete' and manifest['backgroundField']['coverageFraction']==1,'Incomplete background render')
    replay_path=run/'provenance/replay.json';scene=json.loads(replay_path.read_text(encoding='utf-8'))
    ref=scene['backgroundField'];field_path=replay_path.parent/ref['path'];require(sha(field_path)==ref['sha256'],'Metadata SHA256 mismatch')
    metadata=json.loads(field_path.read_text(encoding='utf-8'));binary=field_path.parent/metadata['field']['path'];require(sha(binary)==metadata['field']['sha256'],'Field SHA256 mismatch')
    records=np.fromfile(binary,dtype='<f4').reshape(-1,12)
    for source in metadata['sources']:require(sha(field_path.parent/source['path'])==source['sha256'],'Source SHA256 mismatch')
    width,height=manifest['width'],manifest['height'];views={}
    delta=np.zeros(len(scene['instances']))
    for item in scene.get('thermalPerturbations',[]):delta[next(i for i,x in enumerate(scene['instances']) if x['id']==item['instance'])]=item['deltaKelvin']
    for camera in manifest['cameras']:
        view=camera['id'];hits=np.fromfile(run/(view+'_hits.f32'),dtype='<f4').reshape(height,width,8)
        temperatures=np.fromfile(run/(view+'_temperatureK.f32'),dtype='<f4').reshape(height,width)
        radiance=np.fromfile(run/(view+'_centerRadiance.f32'),dtype='<f4').reshape(height,width)
        require(np.isfinite(hits).all() and np.isfinite(temperatures).all() and np.isfinite(radiance).all(),'Nonfinite GPU fields')
        require(np.array_equal(temperatures,hits[:,:,5]),'Temperature export disagrees with hit data')
        mask=hits[:,:,0]>=0;index=hits[:,:,7].astype(np.int64);require(np.all((index[mask]>=0)&(index[mask]<len(records))),'Triangle index out of range')
        instance=hits[:,:,6].astype(np.int64);require(np.all((instance[mask]>=0)&(instance[mask]<len(scene['instances']))),'Instance index out of range')
        position=np.asarray(camera['position'],dtype=float);fwd=np.asarray(camera['target'],dtype=float)-position;fwd/=np.linalg.norm(fwd)
        right=np.cross(fwd,camera['up']);right/=np.linalg.norm(right);up=np.cross(right,fwd)
        u=(np.arange(width)+.5)/width*2-1;v=1-(np.arange(height)+.5)/height*2;tan=math.tan(math.radians(camera['fovYDegrees']/2))
        rays=fwd[None,None,:]+right[None,None,:]*u[None,:,None]*width/height*tan+up[None,None,:]*v[:,None,None]*tan
        rows=records[np.maximum(index,0)];tri=rows[:,:,:9].reshape(height,width,3,3)
        normal=np.cross(tri[:,:,1]-tri[:,:,0],tri[:,:,2]-tri[:,:,0]);front=np.sum(normal*rays,axis=2)<0
        expected=np.where(front,rows[:,:,10],rows[:,:,11])+delta[np.maximum(instance,0)]
        temperature_error=float(np.abs(temperatures[mask]-expected[mask]).max());require(temperature_error<=1e-4,f'{view} temperature mismatch: {temperature_error}')
        material=rows[:,:,9].astype(np.int64);epsilon=np.asarray(metadata['sceneBinding']['emissivities'])[material]
        expected_radiance=epsilon*planck(manifest['thermal']['wavelengthMicrometers'],expected)+(1-epsilon)*planck(manifest['thermal']['wavelengthMicrometers'],manifest['thermal']['skyK'])
        radiance_error=float(np.abs(radiance[mask]-expected_radiance[mask]).max());require(radiance_error<3e-5,f'{view} radiance mismatch: {radiance_error}')
        views[view]=dict(surfacePixels=int(mask.sum()),centerRayRadiancePixels=int(mask.sum()),maxTemperatureErrorK=temperature_error,
                         maxRadianceErrorW_m2_sr_um=radiance_error,minVisibleSurfaceK=float(temperatures[mask].min()),maxVisibleSurfaceK=float(temperatures[mask].max()))
    result=dict(status='passed',timestamp=metadata['timestamp'],sourceMapping=metadata.get('mappingVerification'),views=views,fieldValidation=False,
                validationScope='GPU/file/geometry/numerical correspondence; no independent field or radiometric validation')
    (run/'background-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    return result


def main():
    p=argparse.ArgumentParser();p.add_argument('run',type=Path);a=p.parse_args();print(json.dumps(verify(a.run.resolve()),indent=2))


if __name__=='__main__':main()
