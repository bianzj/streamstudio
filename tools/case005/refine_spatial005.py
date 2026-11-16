"""Independent 0.25 m grass mesh check for the midday spatial figure."""
import copy,datetime,json,pathlib,subprocess
import numpy as np
from pv_case005 import ROOT,ENGINE,obj,dump,sha
R=ROOT/'refinement-025m'
def main():
    R.mkdir(exist_ok=True);(R/'models').mkdir(exist_ok=True)
    triangles=[];step=.25
    for y in np.arange(0,18,step):
        for x in np.arange(0,18,step):
            a=(x,y,.25);b=(x+step,y,.25);c=(x+step,y+step,.25);d=(x,y+step,.25)
            triangles.extend([[a,b,c],[a,c,d]])
    mesh=R/'models/grass-025m.obj'
    if not mesh.exists():obj(mesh,'grass_leaf',triangles)
    for s in ['grass','pv_grass']:
        run=R/s;run.mkdir(exist_ok=True);manifest=run/'run-manifest.json'
        if manifest.exists():
            assert json.loads(manifest.read_text(encoding='utf8'))['status']=='completed'
            continue
        p=json.loads((ROOT/f'scenarios/{s}/project.json').read_text(encoding='utf8'));p['name']+=' 0.25m午间空间补算'
        c=p['configuration'];c['meteo']['end']=37;c['outDir']=(run/'output').as_posix()
        for item in c['objects']['items']:
            if item['name']=='grass':item['fileName']=mesh.as_posix()
        dump(run/'project.json',p);(run/'output').mkdir(exist_ok=True)
        files=[run/'project.json',mesh,ROOT/'models/panels.obj',ROOT/'positions/origin.txt',ROOT/'meteorology/idealized_diurnal.txt']
        m=dict(status='running',purpose='midday spatial figure only; original diurnal and directional products retained',grass_cell_m=.25,grass_facets=len(triangles),engine=str(ENGINE),engine_sha256=sha(ENGINE),inputs={str(f):sha(f) for f in files},started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
        dump(manifest,m)
        with (run/'native-run.log').open('w',encoding='utf8') as log:
            result=subprocess.run([str(ENGINE),'eFacetEB',str(run/'project.json'),str(run/'output/faceteb.json')],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
        m.update(status='completed' if result.returncode==0 else 'failed',returncode=result.returncode,finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat());dump(manifest,m)
        print(s,m['status'],flush=True)
        assert result.returncode==0
if __name__=='__main__':main()
