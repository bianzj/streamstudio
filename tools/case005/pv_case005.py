# ==========================================================================
# 文件：pv_case005.py
# 功能：案例005（理想化光伏草场）的准备与运行：生成光伏板/草叶网格、位置文件、理想化日变化气象，
#       基于模板工程派生“无光伏草场”和“光伏草场”两个 eFacetEB 工程，并用原生 histream.exe 运行。
# 主要模块/步骤：
#   1. 常量：ROOT（案例目录）、SOURCE（模板工程）、ENGINE（引擎可执行文件）；
#   2. dump/sha/obj：写 JSON、算 SHA-256、把三角形列表写成 OBJ（含坐标系换算）；
#   3. prepare()：建目录，生成 panels.obj、grass.obj、origin.txt、49 时次气象，改写模板工程，
#      写两个场景工程、provenance/design.json 与实施记录；
#   4. run()：依次运行 grass、pv_grass，记录引擎与输入哈希及起止时间到 run-manifest.json。
# 用法：python pv_case005.py prepare 或 python pv_case005.py run（入口按命令行第一个参数调用同名函数）。
# 数据流：模板工程 + 设计参数 → ROOT/{models,positions,meteorology,scenarios} → histream.exe
#         → ROOT/runs/<场景>/output/faceteb.json 等。
# 与其他文件的关系：ROOT、dump、sha 被本目录其他脚本引入；export_case005.mjs 渲染本脚本产出的状态。
# 约定：设计坐标为（东, 北, 上），单位 m；写入 OBJ 时换算为 HiStream 坐标（+X 北、+Y 上、+Z 东）；
#       理想化案例：固定气温（无气温响应）、草叶为水平叶片面元（LAI=1，高 0.25 m）、无独立实测验证。
# ==========================================================================

"""Case 005: reproducible, idealized photovoltaic grassland experiment."""
import copy, datetime, hashlib, json, math, pathlib, subprocess, sys
import numpy as np

ROOT = pathlib.Path('D:/cases/StreamStudio/case005')  # ASCII alias to canonical D: case; native Windows path encoding.
SOURCE = pathlib.Path('D:/cases/StreamStudio/发布整理归档/20261101-开发与研究资料/案例/培训案例/倾斜光伏板/project.json')
if (ROOT/'provenance/source-template.json').exists(): SOURCE=ROOT/'provenance/source-template.json'
ENGINE = pathlib.Path('C:/work/streamstudio/models/bin_x64/Release/histream.exe')
# 作用：写 JSON（UTF-8，缩进 2，保留中文），必要时先建父目录。
def dump(p, v):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(v, ensure_ascii=False, indent=2), encoding='utf8')
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
# 作用：把三角形列表写成单对象 OBJ（每个三角形独立 3 个顶点，无共享顶点）。
# 输入：triangles 为 [[(x,y,z) x3], ...]，设计坐标（东, 北, 上），单位 m。
def obj(path, name, triangles):
    lines = ['o '+name]
    for tri in triangles:
        # HiStream: +X north, +Y up, +Z east; design coordinates: east,north,up.
        lines.extend('v '+' '.join(f'{v:.6f}' for v in (pt[1],pt[2],pt[0])) for pt in tri)
    for i in range(len(triangles)): lines.append(f'f {3*i+1} {3*i+2} {3*i+3}')
    path.write_text('\n'.join(lines)+'\n', encoding='utf8')
# 作用：生成案例的全部输入。已存在 project.json 时拒绝重复准备。
# 输出：models/*.obj、positions/origin.txt、meteorology/idealized_diurnal.txt、两个场景工程、
#       provenance/design.json 与 implementation-plan.md。
def prepare():
    if (ROOT/'project.json').exists(): raise RuntimeError('Case already prepared')
    for d in ['models','positions','meteorology','provenance','reproduce','figures','tables','qa']:
        (ROOT/d).mkdir(parents=True,exist_ok=True)
    # 步骤 1：光伏板——4 行（y=3,7,11,15 m），每行 x 向 3–15 m 共 12 m，斜面长 2 m、倾角 30°，
    # 低边高 1.5 m、高边高 2.5 m；每个 2 m x 2 m 组件拆成两个三角形，法线朝上偏南。
    # z-up, normal points upward and south (-y); 4 rows, each 12 x 2 m.
    pv=[]
    for y in [3,7,11,15]:
        for x in range(3,15,2):
            a=(x,y,1.5); b=(x+2,y,1.5); c=(x+2,y+2*math.cos(math.pi/6),2.5); d=(x,y+2*math.cos(math.pi/6),2.5)
            pv.extend([[a,b,c],[a,c,d]])
    grass=[]
    # 步骤 2：草叶——18 x 18 个 1 m 水平方块（各 2 个三角形），位于 0.25 m 高度，代表 LAI=1 的草冠。
    # Explicit horizontal leaf tiles: LAI=1, height=.25 m, 1 m resolution.
    for y in range(18):
        for x in range(18):
            a=(x,y,.25);b=(x+1,y,.25);c=(x+1,y+1,.25);d=(x,y+1,.25)
            grass.extend([[a,b,c],[a,c,d]])
    obj(ROOT/'models/panels.obj','pv_module',pv)
    obj(ROOT/'models/grass.obj','grass_leaf',grass)
    (ROOT/'positions/origin.txt').write_text('0 0 0 1 0\n',encoding='utf8')
    # 步骤 3：理想化 49 时次（两天加 1 个端点，1 h 间隔）气象；DOY 从 179 起。
    # 短波：日出 6 时、日落 18 时的正弦（峰值 850 W m-2，夜间 0）；气温：23 ± 7 ℃，14 时最高；
    # 水汽压 ea 取相对湿度 65% 与 Magnus 型饱和水汽压（hPa）；其余为常数（气压 1000、风速 2.5、
    # 长波 350）。各列顺序与单位以 meteorology 读入规则为准（待核对）。
    rows=[]
    for i in range(49):
        h=i%24; sw=max(0,850*math.sin(math.pi*(h-6)/12)); t=23+7*math.cos(2*math.pi*(h-14)/24)
        ea=.65*6.112*math.exp(17.67*t/(t+243.5))
        rows.append([179+i/24,t,ea,1000,2.5,sw,350])
    met=ROOT/'meteorology/idealized_diurnal.txt'
    met.write_text('49\n'+'\n'.join(' '.join(f'{v:.9f}' for v in r) for r in rows)+'\n',encoding='utf8')
    # 步骤 4：以模板工程为基础改写配置：模式 eFacetEB、场景 18 x 18 x 6 m、96 x 96 像元传感器、
    # 采样/耦合迭代控制（最多 150 次，温度容差 0.02）、关闭流体与大气、只保留需要的光谱与材料。
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
    # 草叶材料：优先沿用模板里的 Vegetation 材料，没有则用缺省光合/气孔参数新建（待核对参数含义）。
    leaf=next((m for m in c['materials'] if m['type']=='Vegetation'),None)
    if leaf is None:
        leaf={'name':'grass','type':'Vegetation','params':{'Vcmax':60,'m':8,'BallBerry':.01,'heatCapacityPerArea':1000}};c['materials'].append(leaf)
    leaf['name']='grass';leaf['label']='草叶';leaf['energyModel']='vegetation'
    # 热状态初值统一 290.15 K；冠层参数：草 LAI=1、高 0.25 m；刚体（光伏板）为 solid。
    c['thermals']=[{'name':n,'sunlitTemperature':290.15,'shadedTemperature':290.15} for n in ['pv_temperature','vegetation_temperature','soil_temperature']]
    c['canopies']=[{'name':'grass_canopy','structureType':'canopy','lai':1,'hc':.25,'leafwidth':.02,'G':.5}, {'name':'rigid_body','structureType':'solid','leafwidth':2}]
    # 作用：构造一个场景对象（OBJ、位置文件、材料/光谱/热状态/冠层绑定），单实例、关闭镜面反射。
    def item(name,typ,mesh,mat,spec,therm,canopy):
        return dict(name=name,type=typ,fileName=(ROOT/f'models/{name}.obj').as_posix(),positionFile=(ROOT/'positions/origin.txt').as_posix(),materialName=mat,canopyName=canopy,instanceCount=1,meshes=[dict(name=mesh,spectralName=spec,thermalName=therm,materialName=mat,canopyName=canopy,specularEnabled=False)])
    g=item('grass','Vegetation','grass_leaf','grass','green_leaf','vegetation_temperature','grass_canopy')
    panel=item('panels','Building','pv_module','pv_panel','pv_opaque','pv_temperature','rigid_body')
    # 步骤 5：对照两个场景——grass 仅有草；pv_grass 为草加光伏板；其余设置完全相同。
    # pv_grass 同时写成案例根目录的 project.json。
    for scenario, items in [('grass',[g]),('pv_grass',[g,panel])]:
        q=copy.deepcopy(p);q['name']='案例005 '+('光伏草场' if scenario=='pv_grass' else '无光伏草场')
        q['configuration']['objects']={'count':len(items),'names':[v['name'] for v in items],'items':items}
        q['configuration']['outDir']=(ROOT/f'runs/{scenario}/output').as_posix()
        dump(ROOT/f'scenarios/{scenario}/project.json',q)
        if scenario=='pv_grass': dump(ROOT/'project.json',q)
    # 写设计与局限记录：覆盖率 = 板水平投影面积 / 草场面积（96·cos30°/324）；
    # 报告节点为预热后的第 24–48 个节点（第二天）。
    dump(ROOT/'provenance/design.json',dict(idealized=True,field_validation=False,grass_representation='explicit horizontal leaf tiles, LAI 1, height 0.25 m; no grass growth or yield',domain_m=[18,18,6],panel_area_m2=96,panel_tilt_deg=30,panel_normal_azimuth_deg=180,ground_cover_fraction=96*math.cos(math.pi/6)/324,warmup_nodes=24,report_nodes=list(range(24,49)),forcing='repeated idealized clear summer day, NOT observed meteorology',limitations=['fixed ambient air; no air-temperature/UHI claim','horizontal grass tiles are an idealization','thermal directionality from geometry, temperature mixture, occlusion and reflected sky; no measured angle-dependent panel emissivity'],source_template=str(SOURCE),source_template_sha256=sha(SOURCE)))
    (ROOT/'provenance/implementation-plan.md').write_text('# 案例005实施记录\n\n1. 建立同气象、同草地的有/无光伏配对工程。\n2. 用原生 eFacetEB 完成49时次，前24时次预热。\n3. 核验收敛、能量残差、文件与输入哈希；原生多角度产品分析。\n4. 制作温度、方向性、空间分布图与可打开的案例说明，并登记案例库。\n\n直接执行用户要求；无模型源码修改。\n',encoding='utf8')
    print('prepared',ROOT,flush=True)
# 作用：逐场景运行原生引擎（stdout/stderr 写入 native-run.log）。
# 溯源：运行前记录引擎与全部输入的 SHA-256；已完成的场景跳过，未完成的旧记录要求人工检查后再重跑。
# 失败时抛出 RuntimeError。
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
