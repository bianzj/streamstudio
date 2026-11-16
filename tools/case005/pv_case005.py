"""Case 005: reproducible, idealized photovoltaic grassland experiment."""
import copy, datetime, hashlib, json, math, pathlib, subprocess, sys
import numpy as np

ROOT = pathlib.Path('D:/cases/StreamStudio/case005')  # ASCII alias to canonical D: case; native Windows path encoding.
SOURCE = pathlib.Path('D:/cases/StreamStudio/发布整理归档/20261101-开发与研究资料/案例/培训案例/倾斜光伏板/project.json')
if (ROOT/'provenance/source-template.json').exists(): SOURCE=ROOT/'provenance/source-template.json'
ENGINE = pathlib.Path('C:/work/streamstudio/models/bin_x64/Release/histream.exe')
def dump(p, v):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(v, ensure_ascii=False, indent=2), encoding='utf8')
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def obj(path, name, triangles):
    lines = ['o '+name]
    for tri in triangles:
        # HiStream: +X north, +Y up, +Z east; design coordinates: east,north,up.
        lines.extend('v '+' '.join(f'{v:.6f}' for v in (pt[1],pt[2],pt[0])) for pt in tri)
    for i in range(len(triangles)): lines.append(f'f {3*i+1} {3*i+2} {3*i+3}')
    path.write_text('\n'.join(lines)+'\n', encoding='utf8')
def prepare():
    if (ROOT/'project.json').exists(): raise RuntimeError('Case already prepared')
    for d in ['models','positions','meteorology','provenance','reproduce','figures','tables','qa']:
        (ROOT/d).mkdir(parents=True,exist_ok=True)
    # z-up, normal points upward and south (-y); 4 rows, each 12 x 2 m.
    pv=[]
    for y in [3,7,11,15]:
        for x in range(3,15,2):
            a=(x,y,1.5); b=(x+2,y,1.5); c=(x+2,y+2*math.cos(math.pi/6),2.5); d=(x,y+2*math.cos(math.pi/6),2.5)
            pv.extend([[a,b,c],[a,c,d]])
    grass=[]
    # Explicit horizontal leaf tiles: LAI=1, height=.25 m, 1 m resolution.
    for y in range(18):
        for x in range(18):
            a=(x,y,.25);b=(x+1,y,.25);c=(x+1,y+1,.25);d=(x,y+1,.25)
            grass.extend([[a,b,c],[a,c,d]])
    obj(ROOT/'models/panels.obj','pv_module',pv)
    obj(ROOT/'models/grass.obj','grass_leaf',grass)
    (ROOT/'positions/origin.txt').write_text('0 0 0 1 0\n',encoding='utf8')
    rows=[]
    for i in range(49):
        h=i%24; sw=max(0,850*math.sin(math.pi*(h-6)/12)); t=23+7*math.cos(2*math.pi*(h-14)/24)
        ea=.65*6.112*math.exp(17.67*t/(t+243.5))
        rows.append([179+i/24,t,ea,1000,2.5,sw,350])
    met=ROOT/'meteorology/idealized_diurnal.txt'
    met.write_text('49\n'+'\n'.join(' '.join(f'{v:.9f}' for v in r) for r in rows)+'\n',encoding='utf8')
    p=json.loads(SOURCE.read_text(encoding='utf8'));p['mode']='eFacetEB'
    c=p['configuration'];c['scene'].update(x=18,y=18,height=6,voxel=.5,terrain=False,demFile='')
    c['sensor'].update(x=96,y=96,bands='660 860 10500',principalPlane=True,hemisphere=True,positionX=9,positionY=9,height=100,process=True,temperature=True,radiationProcess=True,energyProcess=True)
    c['control'].update(samples=32,depth=6,couplingIterations=150,temperatureTolerance=.02,facetBackend='gpu',cpuThreads=4)
    c['fluid']['enabled']=False
    c['light'].update(direct=.8,diffuse=.2,skyTemperature=280)
    c['meteo'].update(path=met.as_posix(),start=0,end=49,dTime=3600,Tsold=290.15,z=2,latitude=40,longitude=120,timezone=8)
    c['atmosphere']['enabled']=False
    c['spectra']=[s for s in c['spectra'] if s['name'] in ['pv_opaque','green_leaf','soil']]
    for s in c['spectra']:
        s['label']=s['name'];s['physicalTexture']={'enabled':False};s['fileName']=''
        if s['name']=='pv_opaque':
            s.update(reflectance='0.08,0.08,0.08',transmittance='0,0,0',refTir=.1,tauTir=0)
            s['specular']['enabled']=False
    c['materials']=[m for m in c['materials'] if m['name'] in ['pv_panel','soilset','leaf']]
    leaf=next((m for m in c['materials'] if m['type']=='Vegetation'),None)
    if leaf is None:
        leaf={'name':'grass','type':'Vegetation','params':{'Vcmax':60,'m':8,'BallBerry':.01,'heatCapacityPerArea':1000}};c['materials'].append(leaf)
    leaf['name']='grass';leaf['label']='草叶';leaf['energyModel']='vegetation'
    c['thermals']=[{'name':n,'sunlitTemperature':290.15,'shadedTemperature':290.15} for n in ['pv_temperature','vegetation_temperature','soil_temperature']]
    c['canopies']=[{'name':'grass_canopy','structureType':'canopy','lai':1,'hc':.25,'leafwidth':.02,'G':.5}, {'name':'rigid_body','structureType':'solid','leafwidth':2}]
    def item(name,typ,mesh,mat,spec,therm,canopy):
        return dict(name=name,type=typ,fileName=(ROOT/f'models/{name}.obj').as_posix(),positionFile=(ROOT/'positions/origin.txt').as_posix(),materialName=mat,canopyName=canopy,instanceCount=1,meshes=[dict(name=mesh,spectralName=spec,thermalName=therm,materialName=mat,canopyName=canopy,specularEnabled=False)])
    g=item('grass','Vegetation','grass_leaf','grass','green_leaf','vegetation_temperature','grass_canopy')
    panel=item('panels','Building','pv_module','pv_panel','pv_opaque','pv_temperature','rigid_body')
    for scenario, items in [('grass',[g]),('pv_grass',[g,panel])]:
        q=copy.deepcopy(p);q['name']='案例005 '+('光伏草场' if scenario=='pv_grass' else '无光伏草场')
        q['configuration']['objects']={'count':len(items),'names':[v['name'] for v in items],'items':items}
        q['configuration']['outDir']=(ROOT/f'runs/{scenario}/output').as_posix()
        dump(ROOT/f'scenarios/{scenario}/project.json',q)
        if scenario=='pv_grass': dump(ROOT/'project.json',q)
    dump(ROOT/'provenance/design.json',dict(idealized=True,field_validation=False,grass_representation='explicit horizontal leaf tiles, LAI 1, height 0.25 m; no grass growth or yield',domain_m=[18,18,6],panel_area_m2=96,panel_tilt_deg=30,panel_normal_azimuth_deg=180,ground_cover_fraction=96*math.cos(math.pi/6)/324,warmup_nodes=24,report_nodes=list(range(24,49)),forcing='repeated idealized clear summer day, NOT observed meteorology',limitations=['fixed ambient air; no air-temperature/UHI claim','horizontal grass tiles are an idealization','thermal directionality from geometry, temperature mixture, occlusion and reflected sky; no measured angle-dependent panel emissivity'],source_template=str(SOURCE),source_template_sha256=sha(SOURCE)))
    (ROOT/'provenance/implementation-plan.md').write_text('# 案例005实施记录\n\n1. 建立同气象、同草地的有/无光伏配对工程。\n2. 用原生 eFacetEB 完成49时次，前24时次预热。\n3. 核验收敛、能量残差、文件与输入哈希；原生多角度产品分析。\n4. 制作温度、方向性、空间分布图与可打开的案例说明，并登记案例库。\n\n直接执行用户要求；无模型源码修改。\n',encoding='utf8')
    print('prepared',ROOT,flush=True)
def run():
    for scenario in ['grass','pv_grass']:
        folder=ROOT/f'runs/{scenario}';folder.mkdir(parents=True,exist_ok=True)
        manifest=folder/'run-manifest.json'
        if manifest.exists():
            if json.loads(manifest.read_text())['status']=='completed': continue
            raise RuntimeError('Inspect prior run before restarting')
        project=ROOT/f'scenarios/{scenario}/project.json'
        inputs=[project,*list((ROOT/'models').glob('*.obj')),ROOT/'positions/origin.txt',ROOT/'meteorology/idealized_diurnal.txt']
        m=dict(status='running',engine=str(ENGINE),engine_sha256=sha(ENGINE),inputs={str(x):sha(x) for x in inputs},started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
        dump(manifest,m)
        (folder/'output').mkdir(exist_ok=True)
        with (folder/'native-run.log').open('w',encoding='utf8') as log:
            r=subprocess.run([str(ENGINE),'eFacetEB',str(project),str(folder/'output/faceteb.json')],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
        m.update(status='completed' if r.returncode==0 else 'failed',returncode=r.returncode,finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat());dump(manifest,m)
        print(scenario,m['status'],flush=True)
        if r.returncode: raise RuntimeError('Native engine failed')
if __name__=='__main__': globals()[sys.argv[1]]()
