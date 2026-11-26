# ==========================================================================
# 文件：pv_media005.py
# 功能：案例005（光伏草场）原生结果的分析与基础媒体生成：校验 FacetEB 运行结果，统计板温/草叶温度日变化，
#       由传感器 TIFF 计算多角度亮温，画 01–05 号基础图，并写 README、index.html 与 PDF 报告。
#       只用引擎原生状态与 StreamStudio 渲染的 TIFF，不引入替代温度模型。
# 主要模块/步骤：
#   1. readtif()：读 96 x 96 亮温 TIFF 并转 °C（被其他脚本引用）；
#   2. main() 步骤 1：读两场景的 faceteb.json 与逐节点能量状态，校验收敛、面积、板法线；
#   3. 步骤 2：统计第二天逐时草叶/板温，写 tables/temperature_diurnal.csv，画图 01；
#   4. 步骤 3：12:00 逐面元温度与配对温差图 02；
#   5. 步骤 4：多角度亮温（先平均辐亮度再反演），写 thermal_directionality.csv，画图 03；
#   6. 步骤 5：天底热红外对照图 04、光伏能量分配图 05；
#   7. 步骤 6：写 summary.json、native-audit.json，生成 README.md、index.html、PDF，复制脚本，
#      若存在细网格结果则调用 pv_layout005.main 覆盖为修订版排版与空间图。
# 数据流：ROOT/runs/<场景>/output、ROOT/sensor/<场景>/{nadir,angular} → figures/、tables/、qa/。
# 与其他文件的关系：ROOT、dump、sha 来自 pv_case005.py；被 compact_promotion005.py、finalize_case005.py、
#       pv_layout005.py 引入 readtif；结束时回调 pv_layout005.main。
# 约定：温度单位 °C（状态为 K，减 273.15）；面元编号：前 648 个为草叶（18 x 18 x 2 个三角形），
#       648–695 为光伏板（48 个三角形，96 m2）；HiStream 坐标 +X 北、+Y 上、+Z 东，场景原点在中心，
#       故设计坐标 = 坐标 + 9 m；10.5 μm 辐亮度用 Planck 公式（c1=1.19104e8, c2=14387.7，波长单位 μm）。
# ==========================================================================

"""Analyze native states and sensor exports; no substituted temperature model."""
import csv,json,pathlib,re,shutil,warnings
import numpy as np
import rasterio
from rasterio.errors import NotGeoreferencedWarning
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.collections import PolyCollection
from reportlab.pdfgen import canvas
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.cidfonts import UnicodeCIDFont
from pv_case005 import ROOT,dump,sha
import sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]))
from facet_document import read_document  # JSON / binary facet result reader (tools/facet_document.py)
warnings.filterwarnings('ignore',category=NotGeoreferencedWarning)
font_manager.fontManager.addfont('C:/Windows/Fonts/msyh.ttc')
plt.rcParams.update({'font.family':'Microsoft YaHei','font.size':11,'axes.unicode_minus':False,'axes.spines.top':False,'axes.spines.right':False,'figure.facecolor':'#f8fafc','axes.facecolor':'#f8fafc'})
F=ROOT/'figures'; T=ROOT/'tables'; colors=['#15806c','#dc7d28','#315c99']
# 作用：把字典列表写成 UTF-8（带 BOM）CSV，列名取第一行的键。
def csvout(p,rows):
    with p.open('w',encoding='utf-8-sig',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
def save(fig,name):
    fig.savefig(F/(name+'.png'),dpi=170,bbox_inches='tight');plt.close(fig)
# 作用：读单波段亮温 TIFF（K）并转成 °C；断言尺寸 96 x 96、有效像元占比 > 85%、
#       亮温在 200–360 K 之间，以便尽早发现渲染异常。
def readtif(p):
    with rasterio.open(p) as f: a=f.read(1)
    assert a.shape==(96,96) and np.isfinite(a).mean()>.85 and np.nanmin(a)>200 and np.nanmax(a)<360
    return a-273.15
# 作用：完成本案例的全部分析、出图与文档生成（耗时的文件读写集中在此函数内）。
def main():
    data={};audit={};series=[]
    for s in ['grass','pv_grass']:
        # 步骤 1：逐场景读取 faceteb.json（几何）与 faceteb_run.json（节点索引），要求 49 节点全部收敛；
        # 草叶 648 个三角面元总面积须为 324 m2；光伏场景的板面元（648–695）总面积 96 m2，
        # 且法线为 (-0.5, √3/2, 0)（HiStream 坐标，即朝上偏南 30°）。
        base=ROOT/f'runs/{s}/output'; g=read_document(base/'faceteb.json',only={'vertexPositions'}); idx=json.loads((base/'faceteb_run.json').read_text())
        assert idx['status']=='completed' and len(idx['completed'])==49 and all(n['converged'] for n in idx['completed'])
        v=np.array(g['vertexPositions']).reshape(-1,3,3);a=np.linalg.norm(np.cross(v[:,1]-v[:,0],v[:,2]-v[:,0]),axis=1)/2
        assert abs(a[:648].sum()-324)<1e-4
        if s=='pv_grass':
            assert abs(a[648:696].sum()-96)<1e-3
            normal=np.cross(v[648,1]-v[648,0],v[648,2]-v[648,0]);normal/=np.linalg.norm(normal)
            assert abs(normal[0]+.5)<1e-5 and abs(normal[1]-np.sqrt(.75))<1e-5 and abs(normal[2])<1e-5
        records=[]
        for n in idx['completed']:
            # 每个节点的能量状态为 float32、17 列，行数为 2 x facetCount（每面元两行，第 0 列为温度 K；
            # 后文用 [::2] 取每面元的第一行，两行含义待核对）。
            b=np.fromfile(base/f'.facet_steps/energy_T={n["token"]}.bin','<f4').reshape(-1,17)
            assert b.shape==(2*g['facetCount'],17) and np.isfinite(b).all()
            records.append(b)
        data[s]=dict(v=v,a=a,idx=idx,b=records)
        audit[s]=dict(nodes=49,converged_nodes=49,max_residual_W_m2=max(n['energyResidual'] for n in idx['completed']),max_temperature_delta_K=max(n['temperatureDelta'] for n in idx['completed']),grass_area_m2=float(a[:648].sum()),facet_count=g['facetCount'])
    # 步骤 2：按几何划分“板投影下”草叶：用面元中心的北向/东向位置判断是否落入任一行板的水平投影
    # （北向 y..y+√3 m、东向 3–15 m），不依赖某一时刻的太阳阴影。
    c=data['pv_grass']['v'][:648].mean(axis=1)
    # Geometric footprint, independent of instantaneous solar shadow.
    under=np.zeros(648,dtype=bool)
    for y in [3,7,11,15]: under|=(c[:,0]+9>=y)&(c[:,0]+9<=y+np.sqrt(3))&(c[:,2]+9>=3)&(c[:,2]+9<=15)
    # 逐小时统计第二天（节点 24–48）：草叶温度取面元算术平均，板温取面积加权平均。
    for node in range(24,49):
        h=node-24; b=data['pv_grass']['b'][node][::2]; bg=data['grass']['b'][node][::2]
        row=dict(hour=h,grass_open_C=float(bg[:648,0].mean()-273.15),grass_pv_C=float(b[:648,0].mean()-273.15),grass_under_footprint_C=float(b[:648,0][under].mean()-273.15),grass_interrow_C=float(b[:648,0][~under].mean()-273.15),panel_C=float(np.average(b[648:696,0],weights=data['pv_grass']['a'][648:696])-273.15))
        row['grass_difference_K']=row['grass_pv_C']-row['grass_open_C'];series.append(row)
    csvout(T/'temperature_diurnal.csv',series)
    # 步骤 2b：读光伏汇总表；图 01——左为表面温度与同一气温驱动（23 ± 7 °C，与 pv_case005.py 的气象一致），右为板投影下/行间草叶。
    pv=list(csv.DictReader((ROOT/'runs/pv_grass/output/photovoltaic_summary.csv').open()))
    fig,ax=plt.subplots(1,2,figsize=(13,5),layout='constrained');h=np.arange(25)
    for k,label,col in [('grass_open_C','无光伏：草叶',colors[0]),('grass_pv_C','有光伏：草叶均值',colors[2]),('panel_C','光伏板',colors[1])]:ax[0].plot(h,[r[k] for r in series],label=label,color=col,lw=2)
    ax[0].plot(h,[23+7*np.cos(2*np.pi*((i%24)-14)/24) for i in h],'--',color='#909090',label='两组相同的气温驱动')
    ax[0].set(title='A  表面温度与气温驱动',xlabel='第二天当地时间 / h',ylabel='温度 / °C');ax[0].legend(fontsize=9)
    for k,label,col in [('grass_under_footprint_C','板投影下草叶',colors[2]),('grass_interrow_C','行间及外围草叶',colors[0])]:ax[1].plot(h,[r[k] for r in series],label=label,color=col,lw=2)
    ax[1].set(title='B  草叶位置决定受热差异',xlabel='第二天当地时间 / h',ylabel='草叶温度 / °C');ax[1].legend(fontsize=9)
    for a in ax:a.set_xticks(range(0,25,3));a.grid(alpha=.15)
    fig.suptitle('005｜光伏改变草场的温度分布',fontsize=18);save(fig,'01_温度日变化')
    # 步骤 3：图 02——12:00（节点 36）逐面元温度；右图为同一面元位置上有、无光伏之差。
    # 面元坐标 +9 m 把以场景中心为原点的坐标移到 0–18 m。
    fig,ax=plt.subplots(1,3,figsize=(14,4.8),layout='constrained')
    b=data['pv_grass']['b'][36][::2,0][:648]-273.15; bg=data['grass']['b'][36][::2,0][:648]-273.15
    v=data['pv_grass']['v'][:648,:,[2,0]]+9
    for i,(values,title,cmap,lo,hi) in enumerate([(bg,'无光伏草叶温度','turbo',min(bg.min(),b.min()),max(bg.max(),b.max())),(b,'有光伏草叶温度（透视板下）','turbo',min(bg.min(),b.min()),max(bg.max(),b.max())),(b-bg,'配对温差：有 − 无','RdBu_r',-max(abs(b-bg)),max(abs(b-bg))) ]):
        pc=PolyCollection(v,array=values,cmap=cmap,clim=(lo,hi),edgecolors='none');ax[i].add_collection(pc);ax[i].set(xlim=(0,18),ylim=(0,18),aspect='equal',xlabel='东向 / m',ylabel='北向 / m',title=title);fig.colorbar(pc,ax=ax[i],shrink=.8,label='K' if i==2 else '°C')
    fig.suptitle('12:00｜草叶物理温度；右图按同一位置逐面元相减',fontsize=17);save(fig,'02_草叶空间温差')
    # 步骤 4：多角度亮温。对每幅图：先把亮温换成 10.5 μm 辐亮度，对有效像元取平均，
    # 再反演成亮温（平均亮温另存为 mean_pixel_brightness_C 作对比）。
    angular=[]
    for s in ['grass','pv_grass']:
        man=json.loads((ROOT/f'sensor/{s}/angular/export-manifest.json').read_text())
        for n in man['products']:
            for p in n['tifPaths']:
                z,az=map(float,re.search(r'VZA=([\d.]+)_VAA=([\d.]+)',p).groups());a=readtif(p)
                # （以下三行：Planck 正向、像元平均、Planck 逆向；常数 c1=1.19104e8、c2=14387.7，波长 10.5 μm。）
                # Equal image-pixel solid footprint weighting: average radiance first.
                radiance=1.19104e8/(10.5**5*np.expm1(14387.7/(10.5*(a+273.15))))
                mean_radiance=float(np.nanmean(radiance))
                tb=14387.7/(10.5*np.log1p(1.19104e8/(10.5**5*mean_radiance)))-273.15
                angular.append(dict(scenario=s,hour=n['node']-24,vza_deg=z,vaa_deg=az,brightness_C=float(tb),mean_pixel_brightness_C=float(np.nanmean(a)),valid_pixels=int(np.isfinite(a).sum()),image=pathlib.Path(p).relative_to(ROOT).as_posix()))
    # 画图 03：南北（180°/0°）与东西（90°/270°）两个主平面，正角朝南/朝东，负角朝北/西。
    csvout(T/'thermal_directionality.csv',angular)
    fig,ax=plt.subplots(1,3,figsize=(14,4.8),layout='constrained')
    for i,hour in enumerate([9,12,15]):
        for s,style,col,label in [('grass','--',colors[0],'无光伏'),('pv_grass','-',colors[1],'有光伏')]:
            for plane,positive in [('南北',180),('东西',90)]:
                rows=[r for r in angular if r['scenario']==s and r['hour']==hour and (r['vaa_deg'] in [positive,(positive+180)%360] or r['vza_deg']==0)]
                points=sorted([((r['vza_deg'] if r['vaa_deg']==positive else -r['vza_deg']),r['brightness_C']) for r in rows])
                ax[i].plot(*zip(*points),style,color=col,alpha=1 if plane=='南北' else .45,lw=2,label=label+'·'+plane)
        ax[i].set(title=f'{hour:02d}:00',xlabel='有符号观测天顶角 / °',ylabel='10.5 μm 场景亮温 / °C');ax[i].grid(alpha=.15)
    ax[0].legend(fontsize=8);fig.suptitle('热辐射方向性｜同一温度场，改变观测方向',fontsize=18);save(fig,'03_热辐射方向性')
    # 步骤 5：图 04——两场景在 09、12、15 时的天底热红外影像，统一色标 22–48 °C。
    fig,ax=plt.subplots(2,3,figsize=(12,7),layout='constrained')
    for i,s in enumerate(['grass','pv_grass']):
        for j,hour in enumerate([9,12,15]):
            p=next((ROOT/f'sensor/{s}/nadir').glob(f'T=DOY180_{hour:02d}-00*.tif'));a=readtif(p)
            im=ax[i,j].imshow(a,cmap='inferno',vmin=22,vmax=48);ax[i,j].set(title=('无光伏' if i==0 else '有光伏')+f'  {hour:02d}:00');ax[i,j].set_axis_off()
    fig.colorbar(im,ax=ax.ravel().tolist(),shrink=.7,label='10.5 μm 亮温 / °C');fig.suptitle('天底热红外影像｜暖板与较冷草叶共同进入像元',fontsize=18);save(fig,'04_热红外影像')
    # 图 05：光伏板能量分配（按 96 m2 归一）与板温、直流功率；数据来自 photovoltaic_summary.csv 的第二天 25 行。
    fig,ax=plt.subplots(1,2,figsize=(13,5),layout='constrained');rows=pv[24:49]
    for key,label,col in [('absorbed_W','吸收短波',colors[1]),('electric_W','发电输出',colors[0]),('sensible_W','显热',colors[2]),('net_longwave_W','净长波','#9763aa')]:ax[0].plot(h,[float(r[key])/96 for r in rows],label=label,color=col,lw=2)
    ax[0].set(title='板组件能量分配（96 m² 面积归一）',xlabel='当地时间 / h',ylabel='W/m²');ax[0].legend(fontsize=9);ax[0].grid(alpha=.15)
    ax[1].plot(h,[float(r['temperature_C']) for r in rows],color=colors[1],lw=2,label='板温');a2=ax[1].twinx();a2.plot(h,[float(r['electric_W'])/1000 for r in rows],color=colors[0],lw=2,label='直流功率')
    ax[1].set(xlabel='当地时间 / h',ylabel='板温 / °C',title='发电随辐照与板温共同变化');a2.set_ylabel('阵列直流功率 / kW');ax[1].legend(loc='upper left');a2.legend(loc='upper right')
    fig.suptitle('光伏温度特性｜发电耗能参与原生能量平衡',fontsize=18);save(fig,'05_光伏温度与能量')
    # 步骤 6：汇总（12 时数值、板温峰值、方向亮温极差、白天草叶最大降温、第二天直流发电量）并写审计 JSON。
    noon=series[12];ar=[r['brightness_C'] for r in angular if r['scenario']=='pv_grass' and r['hour']==12]
    summary=dict(noon=noon,peak_panel_C=max(r['panel_C'] for r in series),peak_panel_hour=int(np.argmax([r['panel_C'] for r in series])),noon_directional_range_K=max(ar)-min(ar),max_daytime_grass_cooling_K=-min(r['grass_difference_K'] for r in series[6:19]),second_day_dc_energy_kWh=float(pv[48]['energy_kWh'])-float(pv[24]['energy_kWh']),independent_field_validation=False)
    dump(ROOT/'tables/summary.json',summary);dump(ROOT/'qa/native-audit.json',audit)
    text=f'''# 005 光伏草场热环境

光伏板如何改变草场温度？同一块草地在板下、行间和没有光伏时有什么不同？从不同方向观察，又会看到什么热红外信号？本案例用 StreamStudio 原生面元辐射传输与能量平衡展示这三个问题。

## 场景与对照
18 × 18 m 的草地；显式水平草叶面元高 0.25 m、单面叶面积指数 1。光伏场景增加4行南向30°板阵列，组件总面积96 m²，下沿1.5 m、上沿2.5 m。两组使用完全相同的土壤、草地、气象和初始化。板参数：25°C效率0.21、相对温度系数−0.0035 K⁻¹、面积热容量12000 J m⁻² K⁻¹，单面发电。

使用重复的理想化晴夏日气象：北纬40°、东经120°、UTC+8，风速2.5 m s⁻¹，短波峰值850 W m⁻²。计算49个整点，前24个节点用于预热；图表展示第二天0–24时。第24时是下一日0时，日平均应使用0–23时，不能重复计入。

## 温度变化
12时板平均物理温度为 {noon['panel_C']:.2f}°C；无光伏草叶 {noon['grass_open_C']:.2f}°C；有光伏草叶均值 {noon['grass_pv_C']:.2f}°C，配对变化 {noon['grass_difference_K']:+.2f} K。板投影下草叶 {noon['grass_under_footprint_C']:.2f}°C，行间及外围草叶 {noon['grass_interrow_C']:.2f}°C。投影分组是固定几何区域，不等于每时刻的实际阴影。

板温日最高为 {summary['peak_panel_C']:.2f}°C，出现在 {summary['peak_panel_hour']:02d}时。曲线展示辐照增强后的升温和日落后冷却；气温、辐射、对流、发电耗能和面积热容量共同控制结果。每小时采样不足以识别分钟级热惯性或可靠估计时间常数。阴影削弱草叶吸收短波，板面长波与天空遮挡也参与交换；夜间变化由原生输出判读，不能把白天遮阴效果外推为全天降温。

## 热辐射方向性
09、12、15时分别渲染61个方向（南北和东西两个垂直面，天顶角−75°至75°，步长5°，公共天底只算一次）。正角分别朝南/东，负角朝北/西。波长10.5 μm。方向性曲线按每幅正射图所有有效像元的平均辐亮度再反演亮温，包含随视角改变的可见组分与投影边界；不代表固定地面足迹的卫星角度校正。

12时有光伏场景在所采方向的亮温极差为 {summary['noon_directional_range_K']:.2f} K。板、草叶和土壤的可见比例及遮挡随方向改变，加上反射天空的长波，形成场景热红外方向性。这里的亮温是观测量，不能当作板的物理温度。板发射率按设定取0.90，没有引入实测的角度依赖发射率；该结果不是对真实组件镜面热辐射的验证。

## 在 StreamStudio 中展示
打开本目录 project.json，选择面元能量平衡 Facet EB。场景显示光伏阵列、草叶、太阳与传感器方向。草地对照工程见 scenarios/grass/project.json；光伏计算输入原件见 scenarios/pv_grass/project.json。打开结果可查看25幅第二天天底亮温图；原生 process 文件包含能量、温度及光伏功率，可用于3D过程查看。原始每时次二进制保留在 runs/*/output/.facet_steps，未被渲染操作删除。

为兼容当前 Windows 引擎路径编码，D:/cases/StreamStudio/case005 是指向本案例正式D盘目录的英文目录联接。所有工程的资源及输出应能通过该入口解析；迁移到其他电脑时需重建该入口或在软件中重设资源路径。根目录 project.json 是展示副本，模拟实际输入与哈希记录位于 scenarios 和 runs/*/run-manifest.json。

## 可复核范围
两组98个原生节点全部收敛；阈值、实际残差和温差见 qa/native-audit.json 与 faceteb_run.json。原生收敛采用温差及能量残差等效温差等组合判据；不要把temperatureTolerance=0.02 K误读为所有导出的maximumTemperatureDelta都小于0.02 K。输出由现有HiStream GPU引擎计算，没有修改模型源码；传感器图像使用现有StreamStudio渲染器从原生状态生成。

本案例为理想化数值教学对比，不是实测光伏电站验证。水平草叶是简化冠层；没有模拟生长、产草量或土壤水分长期演变。两组气温均由相同气象指定，没有启用流体耦合，不能据此声称降低近地气温或区域热岛。每日重复强迫的一天预热不代表土壤全年周期稳定。发电量为组件直流能量，未含逆变器与线路损失。

## 文件
- figures：日变化、草叶温差、方向性、热红外影像、能量分配及真实软件截图。
- tables：逐时温度、61方向×3时刻×2场景表和摘要。
- runs：引擎原始结果、日志与输入/引擎SHA-256。
- sensor：StreamStudio原生渲染结果与导出记录，96×96像元。
- reproduce：本案例生成、运行、分析与渲染脚本副本；开发源在 C:/work/streamstudio/tools/case005。
- qa：输入几何、数值与交付文件核验；初期坐标/路径排查结果已隔离，不属于正式证据。
'''
    # 写 README.md 与“打开案例.md”（后者取 README 中“在 StreamStudio 中展示”一节）。
    (ROOT/'README.md').write_text(text,encoding='utf8');(ROOT/'打开案例.md').write_text('打开 index.html 阅读完整案例；打开 project.json 进入StreamStudio。\n\n'+text.split('## 在 StreamStudio 中展示')[1].split('## 可复核范围')[0],encoding='utf8')
    # 生成网页 index.html：每张图一节，文末附 README 全文（做 HTML 转义）。
    captions=[('01_温度日变化','原生面元物理温度，按草叶/板面积统计；气温曲线为共同驱动。'),('02_草叶空间温差','12时；板下草叶在图中完整显示，未按相机遮挡剔除。'),('03_热辐射方向性','10.5 μm场景亮温；每幅图先平均辐亮度再反演亮温。'),('04_热红外影像','两组、三时刻、统一22–48°C色标；亮温并非组件温度。'),('05_光伏温度与能量','功率为组件直流输出，发电耗能参与板面能量平衡。'),('06_StreamStudio场景','真实StreamStudio界面；场景预览不能替代计算温度。'),('07_StreamStudio热红外结果','真实软件打开12时的原生渲染TIFF；软件显示单位K，报告图显示°C。')]
    body=''.join(f'<section><h2>{name[3:]}</h2><img src="figures/{name}.png"><p>{cap}</p></section>' for name,cap in captions)
    html=f'''<!doctype html><html lang="zh"><meta charset="utf-8"><title>005 光伏草场热环境</title><style>body{{margin:auto;max-width:1250px;padding:40px;font:17px/1.8 "Microsoft YaHei",sans-serif;color:#18312f;background:#f5f8f6}}h1{{font-size:42px}}section{{background:white;padding:25px;margin:30px 0;border-radius:16px}}img{{width:100%}}pre{{white-space:pre-wrap;font:inherit}}a{{color:#087763}}</style><p>STREAMSTUDIO · CASE 005</p><h1>光伏之下，草场如何受热？</h1><p>原生面元能量平衡 · 配对场景 · 多角度热红外</p><p>12时：板温 {noon['panel_C']:.1f}°C · 草叶均温变化 {noon['grass_difference_K']:+.2f} K · 61方向亮温极差 {summary['noon_directional_range_K']:.2f} K</p><p>理想化教学案例；98/98节点收敛。表面温度与热红外亮温分开解释。</p><a href="案例005_光伏草场热环境.pdf">PDF报告</a> · <a href="project.json">StreamStudio工程</a> · <a href="tables/temperature_diurnal.csv">逐时温度CSV</a> · <a href="tables/thermal_directionality.csv">方向性CSV</a>{body}<section><pre>{text.replace('&','&amp;').replace('<','&lt;')}</pre></section></html>'''
    (ROOT/'index.html').write_text(html,encoding='utf8')
    # 生成 PDF：每张图一页，再把 README 按 65 字折行、每页 31 行排成文字页。
    pdfmetrics.registerFont(UnicodeCIDFont('STSong-Light'));pdf=canvas.Canvas(str(ROOT/'案例005_光伏草场热环境.pdf'),pagesize=(960,720))
    pdf.setTitle('案例005 光伏草场热环境')
    from reportlab.lib.utils import ImageReader
    for num,(name,cap) in enumerate(captions,1):
        pdf.setFillColorRGB(.06,.22,.20);pdf.setFont('STSong-Light',23);pdf.drawString(45,670,'005 | '+name[3:])
        img=ImageReader(str(F/(name+'.png')));w,hh=img.getSize();scale=min(870/w,510/hh);pdf.drawImage(img,45+(870-w*scale)/2,120+(510-hh*scale)/2,width=w*scale,height=hh*scale)
        pdf.setFont('STSong-Light',12);pdf.drawString(45,85,cap);pdf.drawString(45,58,'理想化数值教学案例 | 原生HiStream状态与StreamStudio渲染 | 不作为独立实测验证');pdf.drawRightString(915,30,str(num));pdf.showPage()
    # Add concise readable methods pages, wrapping by available character widths.
    lines=[]
    for para in text.splitlines():
        while len(para)>65:lines.append(para[:65]);para=para[65:]
        lines.append(para)
    for start in range(0,len(lines),31):
        pdf.setFont('STSong-Light',19);pdf.drawString(45,670,'案例说明与证据边界')
        pdf.setFont('STSong-Light',12)
        for i,line in enumerate(lines[start:start+31]):pdf.drawString(45,635-i*18,line)
        pdf.showPage()
    pdf.save()
    for p in pathlib.Path('C:/work/streamstudio/tools/case005').glob('*'):
        if p.is_file():shutil.copy2(p,ROOT/'reproduce'/p.name)
    # 若已有 0.25 m 细网格结果，则由 pv_layout005 重新生成修订版排版（会覆盖上面的网页、PDF 与图 02）。
    # Keep the revised presentation and independently refined spatial figure
    # when regenerating the base plots on this case.
    if (ROOT/'refinement-025m/pv_grass/output/faceteb.json').exists():
        from pv_layout005 import main as revised_layout
        revised_layout()
    print(json.dumps(summary,ensure_ascii=True),flush=True)
if __name__=='__main__':main()
