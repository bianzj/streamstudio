# ==========================================================================
# 文件：refine_spatial005.py
# 功能：案例005 的 0.25 m 草叶网格独立补算：只为第二天午间（12:00）的空间温差图，把草叶网格由 1 m 加密到 0.25 m，
#       用同一原生引擎重跑 grass、pv_grass 两个场景的前 37 个节点（第 0–36 节点），其余输入不变。
# 主要步骤（main）：
#   1. 生成 0.25 m 水平草叶网格 grass-025m.obj（72 x 72 方格，每格 2 个三角形，高 0.25 m）；
#   2. 对每个场景，复制其工程到 refinement-025m/<场景>，把草叶 OBJ 换成细网格、气象截止到 37 个节点；
#   3. 记录引擎与输入 SHA-256 后调用 histream.exe（eFacetEB），把状态写入 run-manifest.json。
# 数据流：ROOT/scenarios/<场景>/project.json + 原模型/位置/气象 → ROOT/refinement-025m/<场景>/output，
#         随后由 pv_layout005.py 的 spatial() 读取。
# 与其他文件的关系：ROOT、ENGINE、obj、dump、sha 来自 pv_case005.py。
# 约定：长度单位 m；前 24 个节点为预热，第 36 节点为第二天 12:00；已完成的场景会被跳过。
# 注意：原有基础日变化与方向性结果仍沿用 1 m 网格，本补算只用于空间图。
# ==========================================================================

"""Independent 0.25 m grass mesh check for the midday spatial figure."""
import copy,datetime,json,pathlib,subprocess
import numpy as np
from pv_case005 import ROOT,ENGINE,obj,dump,sha
R=ROOT/'refinement-025m'
# 作用：生成细网格并运行两个场景的补算；已有 completed 记录则跳过，失败时 assert 中止。
def main():
    R.mkdir(exist_ok=True);(R/'models').mkdir(exist_ok=True)
    # 步骤 1：18 m x 18 m 范围按 0.25 m 切成方格，每格拆成两个三角形（设计坐标：东, 北, 上）。
    triangles=[];step=.25
    for y in np.arange(0,18,step):
        for x in np.arange(0,18,step):
            a=(x,y,.25);b=(x+step,y,.25);c=(x+step,y+step,.25);d=(x,y+step,.25)
            triangles.extend([[a,b,c],[a,c,d]])
    mesh=R/'models/grass-025m.obj'
    if not mesh.exists():obj(mesh,'grass_leaf',triangles)
    # 步骤 2：逐场景派生工程。meteo.end=37 表示只算到第 36 节点（第二天 12:00），以节省时间；
    # 只替换名为 grass 的对象的网格文件，光伏板与其他设置保持不变。
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
        # 步骤 3：登记溯源并运行原生引擎（输出写入 native-run.log），完成后更新运行记录。
        files=[run/'project.json',mesh,ROOT/'models/panels.obj',ROOT/'positions/origin.txt',ROOT/'meteorology/idealized_diurnal.txt']
        m=dict(status='running',purpose='midday spatial figure only; original diurnal and directional products retained',grass_cell_m=.25,grass_facets=len(triangles),engine=str(ENGINE),engine_sha256=sha(ENGINE),inputs={str(f):sha(f) for f in files},started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
        dump(manifest,m)
        with (run/'native-run.log').open('w',encoding='utf8') as log:
            result=subprocess.run([str(ENGINE),'eFacetEB',str(run/'project.json'),str(run/'output/faceteb.json')],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
        m.update(status='completed' if result.returncode==0 else 'failed',returncode=result.returncode,finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat());dump(manifest,m)
        print(s,m['status'],flush=True)
        assert result.returncode==0
if __name__=='__main__':main()
