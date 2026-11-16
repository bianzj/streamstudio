"""GPU numerical fixtures for animation transport, independent of image styling."""
import argparse
import csv
import json
import subprocess
from pathlib import Path


def main():
    p=argparse.ArgumentParser()
    p.add_argument('engine',type=Path)
    p.add_argument('output',type=Path)
    a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=False)
    base=dict(width=96,height=64,fps=4,frameCount=9,impactTime=1,
              renderMode='thermal',physicsMode='transport',samples=1)
    def run(name,**options):
        data=dict(base,**options)
        path=a.output/(name+'.json')
        path.write_text(json.dumps(data),encoding='utf-8')
        out=a.output/name
        r=subprocess.run([str(a.engine.resolve()),'eSceneAnimation',str(path.resolve()),str(out.resolve())],capture_output=True)
        (a.output/(name+'.log')).write_bytes(r.stdout+r.stderr)
        assert r.returncode==0,(name,r.stdout.decode(errors='replace'),r.stderr.decode(errors='replace'))
        manifest=json.loads((out/'run-manifest.json').read_text(encoding='utf-8'))
        assert manifest.get('physicsMode')=='transport','native GPU transport mode is missing'
        assert manifest['transport']['solver']=='incompressible-projection'
        assert (out/'provenance'/'transport.comp.spv').is_file()
        rows=list(csv.DictReader((out/'transport.csv').open(encoding='utf-8')))
        assert len(rows)==data.get('lastFrame',data['frameCount'])-data.get('firstFrame',1)+1
        return out,[{k:float(v) for k,v in row.items()} for row in rows]
    _,ambient=run('ambient',transportScenario='ambient',transportWindX=0,transportWindY=0)
    for row in ambient:
        assert abs(row['air_max_K']-300)<1e-4
        assert row['smoke_integral']<1e-8
        assert row['max_speed_m_s']<1e-6
        assert row['column_probe_solid']==1 and row['slab_probe_solid']==1
        assert row['interior_probe_solid']==0
    _,pulse=run('passive',transportScenario='passivePulse',transportWindX=2,transportWindY=0)
    movement=pulse[-1]['smoke_centroid_x_m']-pulse[0]['smoke_centroid_x_m']
    assert 3.0<movement<5.0,('2 m/s for 2 s',movement)
    drift=abs(pulse[-1]['smoke_integral']/pulse[0]['smoke_integral']-1)
    assert drift<.05,('passive tracer drift',drift)
    assert pulse[-1]['smoke_variance_m2']>pulse[0]['smoke_variance_m2']
    out,plume=run('plume',transportScenario='heatedPlume',frameCount=25,impactTime=1)
    active=[row for row in plume if row['smoke_integral']>.01]
    assert active[-1]['smoke_centroid_z_m']>active[0]['smoke_centroid_z_m']+.3
    assert max(row['surface_max_K'] for row in plume)>350
    assert plume[-1]['surface_max_K']<max(row['surface_max_K'] for row in plume)
    for row in plume+pulse+ambient:
        assert row['invalid_cells']==0
        assert row['smoke_min']>=0 and row['air_min_K']>=299.99
        assert abs(row['pressure_residual_rms']-row['divergence_after_rms'])<1e-5, 'D/G must match the solved Poisson operator'
    projected=[row for row in plume if row['divergence_before_rms']>1e-5]
    assert projected[-1]['divergence_after_rms']<projected[-1]['divergence_before_rms']
    partial,_=run('seek',transportScenario='heatedPlume',frameCount=25,impactTime=1,firstFrame=25,lastFrame=25)
    assert (partial/'frames'/'frame_0025.png').read_bytes()==(out/'frames'/'frame_0025.png').read_bytes()
    report=dict(status='passed',passiveMovementMeters=movement,passiveTracerRelativeDrift=drift,
        plumeRiseMeters=active[-1]['smoke_centroid_z_m']-active[0]['smoke_centroid_z_m'],
        finalDivergenceBefore=projected[-1]['divergence_before_rms'],
        finalDivergenceAfter=projected[-1]['divergence_after_rms'],
        scientificValidation=False)
    (a.output/'verification.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    main()
