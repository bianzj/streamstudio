"""Link a paired local ground EB correction to the existing immutable history."""
import argparse,copy,json,math
from pathlib import Path
import numpy as np
from prepare_thermal_case import sha


def main():
    p=argparse.ArgumentParser();p.add_argument('reference',type=Path);p.add_argument('case',type=Path);a=p.parse_args();out=a.case/'inputs';out.mkdir(parents=True,exist_ok=False)
    scene=json.loads((a.reference/'sequence.json').read_text());ref=scene['backgroundField'];field=Path(ref['path']);meta=json.loads(field.read_text());sources={s['role']:field.parent/s['path'] for s in meta['sources']}
    for s in meta['sources']:assert sha(field.parent/s['path'])==s['sha256']
    project=json.loads(sources['project'].read_text())['configuration'];history=json.loads(sources['history'].read_text());index=json.loads(sources['runIndex'].read_text());weather=np.loadtxt(sources['forcing'],skiprows=1)
    target=meta['solver']['node'];samples=[]
    for node in index['completed']:
        n=node['node']
        if n>target:continue
        z=math.radians(node['solarZenith']);az=math.radians(node['solarAzimuth']);w=weather[n]
        samples.append(dict(seconds=n*project['meteo']['dTime'],sun=[math.sin(z)*math.cos(az),-math.sin(z)*math.sin(az),math.cos(z)],airK=float(w[1]+273.15),wind=float(w[4]),shortwave=float(w[5]),longwave=float(w[6])))
    receivers={}
    for name in ['asphalt','pavement','marking']:
        receivers[name]=dict(albedo=history['albedos'][name],heatCapacity=history['heatCapacityPerArea_J_m2_K'][name],groundConductance=5)
    receivers.update(scenery_soil=dict(albedo=.16,heatCapacity=100000,groundConductance=5),scenery_grass=dict(albedo=.22,heatCapacity=60000,groundConductance=5),scenery_curb=dict(albedo=.35,heatCapacity=80000,groundConductance=5))
    inputs=dict(kind='histream-ground-shadow-delta',schemaVersion=1,temperatureMapping='absolute-local-ground',originMeters=[-95,-80],width=380,height=330,cellMeters=.5,maxReceiverZMeters=.6,initialK=history['initialTemperatureK'],deepK=history['initialTemperatureK'],stepSeconds=60,shadowIntervalSeconds=600,viewSamples=32,directFraction=.8,canopyMaterials=['scenery_bark']+[f'scenery_leaf_{i}' for i in range(8)],receiverMaterials=receivers,history=samples,
        sourceFiles=[dict(role='background',path=str(field),sha256=sha(field))]+[dict(role=k,path=str(v),sha256=sha(v)) for k,v in sources.items()],
        scope='Absolute local ground EB with paired without-canopy control; original FacetEB building/background files retained; fixed current occluder temperatures; no ET/leaf solve/multiple scattering/air feedback',fieldValidation=False)
    params=out/'ground-thermal.json';params.write_text(json.dumps(inputs,indent=2),encoding='utf-8')
    scene['sequence']['groundThermal']=dict(enabled=True,path=str(params.resolve()),sha256=sha(params));(a.case/'sequence.json').write_text(json.dumps(scene,indent=2),encoding='utf-8')
    aux=json.loads((a.reference/'onboard_ir/sequence.json').read_text());aux['sequence']['groundThermal']=copy.deepcopy(scene['sequence']['groundThermal']);(a.case/'onboard_ir').mkdir();(a.case/'onboard_ir/sequence.json').write_text(json.dumps(aux,indent=2),encoding='utf-8')
    print('Prepared ground grid and immutable synthetic history:',len(samples),'nodes',samples[-1]['seconds']/3600,'hours')


if __name__=='__main__':main()
