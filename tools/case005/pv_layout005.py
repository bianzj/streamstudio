# ==========================================================================
# 文件：pv_layout005.py
# 功能：案例005（光伏草场）的排版与 0.25 m 空间温差图：用细网格补算结果画 12:00 草叶温度/温差图，
#       再生成交互网页 index.html 和 PDF 报告。
# 主要模块/步骤：
#   1. spatial()：读 refinement-025m 细网格运行结果，取第二天 12:00 的草叶温度，画
#      02_草叶空间温差.png/.pdf，写 tables/spatial_noon_025m.csv 与 qa/spatial-refinement-audit.json；
#   2. website()：导出逐时天底热红外帧 PNG，把 case005.html 模板的占位符替换为数值与参考文献；
#   3. report()：用 reportlab 画 960 x 720 的报告页（前 9 页手工排版，之后由 add_pdf_pages 追加）；
#   4. main()：先备份旧件，再依次 save_resources → spatial → website → report，
#      补充 README 记录并复制本目录脚本到 reproduce/。
# 数据流：ROOT/refinement-025m/{grass,pv_grass}/output + tables/summary.json + sensor 下天底 TIFF
#         → figures/、assets/、index.html、案例005_光伏草场热环境.pdf。
# 与其他文件的关系：ROOT、dump、sha 来自 pv_case005.py；readtif 来自 pv_media005.py；
#       参考文献、下载片段与附加页来自 case005_resources.py；网页模板为同目录 case005.html/css/js。
# 约定：温度单位 °C（引擎状态为 K，减 273.15）；温差 = 光伏场景减对照；空间图 72 x 72 格、每格 0.25 m，
#       东向为 x、北向为 y；PDF 坐标原点在页面左下角（y 向上，与 compact_promotion005.py 不同）。
# 注意：正文里的收敛节点数（98、74）等是手写常量，温度与温差来自 summary.json 与细网格结果。
# ==========================================================================

"""Editorial layout and native 0.25 m spatial figure for Case 005."""
import csv,html,json,pathlib,shutil,sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]))
from facet_document import read_document  # JSON / binary facet result reader (tools/facet_document.py)
import numpy as np
from PIL import Image
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap,TwoSlopeNorm
from matplotlib.patches import Rectangle
from matplotlib import font_manager,colormaps
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas
from reportlab.lib.colors import HexColor
from reportlab.platypus import Paragraph
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.utils import ImageReader
from pv_case005 import ROOT,dump,sha
from pv_media005 import readtif
from case005_resources import reference_html,software_html,add_pdf_pages,save_resources
HERE=pathlib.Path(__file__).parent
F=ROOT/'figures';INK='#19364b';MUTED='#637887';BLUE='#245e8a';GREEN='#267861'
# 模块：0.25 m 细网格的 12:00 草叶空间温差图。
# 输入：refinement-025m/<场景>/output 下的 faceteb_run.json、faceteb.json 与第 36 节点的能量状态；
# 输出：两张图（PNG 与矢量 PDF）、spatial_noon_025m.csv、spatial-refinement-audit.json；返回 audit。
# 步骤：1 校验运行与几何；2 取温度并按方格平均；3 画三联图；4 写审计与 CSV。
def spatial():
    fields={};audit={};count=2*72*72
    for s in ['grass','pv_grass']:
        # 步骤 1：每个场景须完成 37 个节点（第 0–36 节点，保留到第二天 12 时）且全部收敛，
        # 草场面积须为 324 m2；count = 2 x 72 x 72 为单个场景的草叶三角面元数（每个场景前 count 个面元为草叶，
        # 其余为光伏板，待核对）。
        folder=ROOT/'refinement-025m'/s
        run=json.loads((folder/'output/faceteb_run.json').read_text(encoding='utf8'))
        assert len(run['completed'])==37 and all(n['converged'] for n in run['completed'])
        meta=read_document(folder/'output/faceteb.json',only={'vertexPositions'})  # JSON or binary result
        v=np.asarray(meta['vertexPositions']).reshape(-1,3,3);area=np.linalg.norm(np.cross(v[:,1]-v[:,0],v[:,2]-v[:,0]),axis=1)/2
        assert abs(area[:count].sum()-324)<1e-5
        # 步骤 2：取第 36 节点（第二天 12:00；前 24 个节点为预热）的能量状态二进制，
        # 按 float32、每行 17 列读取，第 0 列为温度（K）转 °C；b[::2] 的取法（待核对）。
        n=next(n for n in run['completed'] if n['node']==36)
        b=np.fromfile(folder/f'output/.facet_steps/energy_T={n["token"]}.bin','<f4').reshape(-1,17)
        values=b[::2,0][:count].astype(float)-273.15
        # 每个 0.25 m 方格由两个原生三角面元组成，只对这两个值取平均，不做空间插值。
        # Only average the two native triangles of each actual 0.25 m square.
        # No spatial interpolation, smoothing or synthetic temperatures.
        fields[s]=values.reshape(72,72,2).mean(axis=2)
        audit[s]=dict(nodes=37,converged_nodes=37,grass_cell_m=.25,grass_facets=count,grass_area_m2=float(area[:count].sum()),mean_grass_C=float(values.mean()),max_energy_residual_W_m2=max(t['energyResidual'] for t in run['completed']),native_geometry_sha256=sha(folder/'output/faceteb.bin' if (folder/'output/faceteb.bin').exists() else folder/'output/faceteb.json'),native_noon_state_sha256=sha(folder/f'output/.facet_steps/energy_T={n["token"]}.bin'))
    # 步骤 3：画三联图——无光伏温度、光伏草叶温度、配对温差；
    # 温度图共用色标（至少 26–42 °C，必要时取整外扩），温差图色标以 0 为中心对称。
    delta=fields['pv_grass']-fields['grass']
    font_manager.fontManager.addfont('C:/Windows/Fonts/msyh.ttc')
    plt.rcParams.update({'font.family':'Microsoft YaHei','font.size':10,'axes.unicode_minus':False,'axes.spines.top':False,'axes.spines.right':False})
    fig=plt.figure(figsize=(14,5.25),facecolor='white');gs=fig.add_gridspec(2,3,height_ratios=[1,.035],left=.055,right=.97,top=.82,bottom=.20,wspace=.23,hspace=.30)
    axes=[fig.add_subplot(gs[0,i]) for i in range(3)]
    thermal=LinearSegmentedColormap.from_list('grass_temperature',['#d9e9f1','#afcbd8','#f2eddc','#dfb873','#bb6351'])
    difference=LinearSegmentedColormap.from_list('paired_change',['#165f8b','#7eb5cd','#fbfcfc','#dfb396','#ad594c'])
    edges=np.arange(73)*.25
    vmax=max(42,np.ceil(max(f.max() for f in fields.values())));vmin=min(26,np.floor(min(f.min() for f in fields.values())))
    for i,s in enumerate(['grass','pv_grass']):
        mesh=axes[i].pcolormesh(edges,edges,fields[s],cmap=thermal,vmin=vmin,vmax=vmax,shading='flat',linewidth=0,antialiased=False,rasterized=True)
    lim=np.ceil(np.max(np.abs(delta)))
    change=axes[2].pcolormesh(edges,edges,delta,cmap=difference,norm=TwoSlopeNorm(vmin=-lim,vcenter=0,vmax=lim),shading='flat',linewidth=0,antialiased=False,rasterized=True)
    for i,a in enumerate(axes):
        a.set(xlim=(0,18),ylim=(0,18),aspect='equal',xticks=[0,6,12,18],yticks=[0,6,12,18],xlabel='东向距离 / m',ylabel='北向距离 / m' if i==0 else '')
        a.set_facecolor('white');a.tick_params(length=3,color='#acb8c0',labelcolor=INK)
        for spine in ['left','bottom']:a.spines[spine].set_color('#c3cdd3')
        a.set_title(['无光伏草场','光伏草场 · 草叶温度','温差 · 光伏减去对照'][i],pad=14,color=INK,fontsize=12)
        if i>0:
            for y in [3,7,11,15]:a.add_patch(Rectangle((3,y),12,np.sqrt(3),fill=False,edgecolor='#526a78',lw=.65,ls=(0,(4,3)),alpha=.7))
    cb=fig.colorbar(mesh,cax=fig.add_subplot(gs[1,:2]),orientation='horizontal');cb.set_label('两幅温度图共用色标 / °C',fontsize=10,labelpad=7);cb.outline.set_visible(False);cb.ax.tick_params(length=2,labelsize=9)
    cb=fig.colorbar(change,cax=fig.add_subplot(gs[1,2]),orientation='horizontal');cb.set_label('配对温差 / K',fontsize=10,labelpad=7);cb.outline.set_visible(False);cb.ax.tick_params(length=2,labelsize=9)
    fig.text(.055,.975,'12:00  草叶空间温度与配对变化',fontsize=15,color=INK,va='top')
    fig.text(.055,.02,'0.25 m 原生网格补算；每格平均两片原生三角面元，不做空间插值。虚线为板的垂直投影，包含板下草叶。',fontsize=9,color=MUTED)
    fig.savefig(F/'02_草叶空间温差.png',dpi=240,facecolor='white');fig.savefig(F/'02_草叶空间温差_矢量.pdf',dpi=300,facecolor='white');plt.close(fig)
    # 步骤 4：写审计 JSON（含平均温差）与逐格 CSV（格心坐标 east_m/north_m 及两场景温度、温差）。
    audit['difference_mean_K']=float(delta.mean());audit['display']='0.25 m cell area mean of two native triangles; no spatial interpolation';dump(ROOT/'qa/spatial-refinement-audit.json',audit)
    table=[]
    for y in range(72):
        for x in range(72):table.append(dict(east_m=(x+.5)*.25,north_m=(y+.5)*.25,open_C=float(fields['grass'][y,x]),pv_C=float(fields['pv_grass'][y,x]),difference_K=float(delta[y,x])))
    with (ROOT/'tables/spatial_noon_025m.csv').open('w',encoding='utf-8-sig',newline='') as f:
        w=csv.DictWriter(f,fieldnames=table[0].keys());w.writeheader();w.writerows(table)
    return audit
# 模块：生成交互网页。导出每个场景 0–24 时的天底亮温帧（inferno，14–48 °C，NaN 按 14 °C），
# 再把 case005.html 里的 __KEY__ 占位符替换为 summary.json 与细网格结果的数值，并加参考文献引用。
# 输出：assets/frames/*.png、index.html、assets/case005.css|js。
def website(summary,sp):
    assets=ROOT/'assets';(assets/'frames').mkdir(parents=True,exist_ok=True)
    for s in ['grass','pv_grass']:
        for hour in range(25):
            token=f'DOY180_{hour:02d}-00' if hour<24 else 'DOY181_00-00'
            a=readtif(next((ROOT/f'sensor/{s}/nadir').glob(f'T={token}*.tif')))
            rgb=(colormaps['inferno'](np.clip((np.nan_to_num(a,nan=14)-14)/34,0,1))[:,:,:3]*255).astype('uint8')
            Image.fromarray(rgb).save(assets/f'frames/{s}-{hour:02d}.png')
    noon=summary['noon'];text=(ROOT/'README.md').read_text(encoding='utf8')
    technical=''.join('<h3>'+html.escape(p[3:])+'</h3>' if p.startswith('## ') else '<p>'+html.escape(p).replace('\n','<br>')+'</p>' for p in text.split('\n\n'))
    replacements={'PANEL':f'{noon["panel_C"]:.1f}','OPEN':f'{noon["grass_open_C"]:.1f}','PV':f'{noon["grass_pv_C"]:.1f}','DELTA':f'{noon["grass_difference_K"]:+.2f}','RANGE':f'{summary["noon_directional_range_K"]:.2f}','TECHNICAL':technical,'SPATIAL_CAPTION':'0.25 m 草叶网格独立补算；两幅温度图共用色标，温差图以零为中心。每个方格是两片原生三角面元的面积均值，未作空间插值。','SPATIAL_TEXT':f'细网格的午间草叶均温由 {sp["grass"]["mean_grass_C"]:.2f}°C 变为 {sp["pv_grass"]["mean_grass_C"]:.2f}°C，配对变化 {sp["difference_mean_K"]:+.2f} K。本节使用独立细网格结果；日变化与方向性保持原有 1 m 草叶网格结果。'}
    page=(HERE/'case005.html').read_text(encoding='utf8')
    for k,v in replacements.items():page=page.replace('__'+k+'__',v)
    page=page.replace('__REFERENCES__',reference_html()).replace('__SOFTWARE_LINKS__',software_html())
    # 在固定语句后插入文献角标链接（句子须与模板文字完全一致，否则不会被替换）。
    citation_map={
        '白天遮阴改变短波吸收，板面长波与天空遮挡也参与交换。':'白天遮阴改变短波吸收，板面长波与天空遮挡也参与交换。<sup><a href="#ref-1">[1]</a><a href="#ref-2">[2]</a></sup>',
        '同一个温度场，可以形成不同的场景热红外亮温。':'同一个温度场，可以形成不同的场景热红外亮温。<sup><a href="#ref-4">[4]</a></sup>',
        '发电耗能已参与原生板面能量平衡。':'发电耗能已参与原生板面能量平衡。板温影响因素可参阅文献<sup><a href="#ref-3">[3]</a></sup>。',
        '方向曲线包含随视角变化的组分与边界效应；未引入实测的角度依赖板面发射率。':'方向曲线包含随视角变化的组分与边界效应；未引入实测的角度依赖板面发射率。电站气温效应的研究背景见文献<sup><a href="#ref-5">[5]</a></sup>。'}
    for old,new in citation_map.items():page=page.replace(old,new)
    assert '__SPATIAL' not in page
    (ROOT/'index.html').write_text(page,encoding='utf8')
    for name in ['case005.css','case005.js']:shutil.copy2(HERE/name,assets/name)
# 模块：用 reportlab 手工排版 960 x 720 的报告页（前 9 页），其余页由 add_pdf_pages 追加。
# 坐标原点在页面左下角；rect/label/para/picture/header/end 为绘制原语。
# 正文数值来自 summary（12 时板温、草叶温度、方向亮温极差）与 sp（细网格结果）。
def report(summary,sp):
    pdfmetrics.registerFont(TTFont('YaHei','C:/Windows/Fonts/msyh.ttc'));pdfmetrics.registerFont(TTFont('YaHeiBold','C:/Windows/Fonts/msyhbd.ttc'))
    c=canvas.Canvas(str(ROOT/'案例005_光伏草场热环境.pdf'),pagesize=(960,720));c.setTitle('005 光伏草场热环境 | StreamStudio');c.setAuthor('StreamStudio')
    def rect(x,y,w,h,color):c.setFillColor(HexColor(color));c.rect(x,y,w,h,fill=1,stroke=0)
    def label(t,x,y,size=11,color=MUTED,bold=False):
        c.setFont('YaHeiBold' if bold else 'YaHei',size);c.setFillColor(HexColor(color));c.drawString(x,y,t)
    def para(t,x,y,w,size=11,color=MUTED):
        p=Paragraph(t,ParagraphStyle('p',fontName='YaHei',fontSize=size,leading=size*1.85,textColor=HexColor(color),wordWrap='CJK'));_,h=p.wrap(w,600);p.drawOn(c,x,y-h);return h
    def picture(name,x,y,w,h):
        im=ImageReader(str(F/name));iw,ih=im.getSize();scale=min(w/iw,h/ih);c.drawImage(im,x+(w-iw*scale)/2,y+(h-ih*scale)/2,width=iw*scale,height=ih*scale,mask='auto')
    def header(num,title,sub):
        rect(0,0,960,720,'#ffffff');label('StreamStudio',48,680,13,INK,True);label('光伏草场热环境 / 005',735,681,10);rect(48,661,864,1,'#dce5ea');label(sub,48,630,11,GREEN);label(title,48,588,29,INK,True)
        label('理想化数值案例 · 原生状态与传感器观测分别解释',48,28,9);label(f'{num:02d}',885,28,10,BLUE)
    def end():c.showPage()
    n=summary['noon']
    header(1,'光伏之下，草场如何受热？','光伏 × 草场')
    para('板面在升温，板下的草叶却可能更凉。<br/>从一天的温度变化，看到一片草场中的热量分配。',48,550,650,13)
    # 第 1 页：软件截图按场景视口裁切（偏移与缩放比 290/755 为经验值，待核对）。
    # Clip the actual app screenshot to its scene viewport for the cover.
    box=(48,190,864,290);rect(*box,'#f1f5f9');c.saveState()
    im=ImageReader(str(F/'06_StreamStudio场景.png'));iw,ih=im.getSize();scale=290/755
    viewport_x=48+(864-1366*scale)/2
    p=c.beginPath();p.rect(viewport_x,190,1366*scale,290);c.clipPath(p,stroke=0)
    c.drawImage(im,viewport_x-244*scale,190-(ih-843)*scale,width=iw*scale,height=ih*scale);c.restoreState()
    for i,(v,t) in enumerate([(f'{n["panel_C"]:.1f}°C','12时板平均温度'),(f'{n["grass_difference_K"]:+.2f} K','12时草叶均温变化'),(f'{summary["noon_directional_range_K"]:.2f} K','12时场景方向亮温极差')]):
        x=48+i*294;label(v,x,125,30,GREEN if i==1 else BLUE);label(t,x,96,11)
    label('18 × 18 m 配对场景   /   南向30°板阵列   /   96 m²组件',48,59,10);end()
    # 第 2 页：场景与参数（光伏板参数、气象峰值为手写文字，待核对与工程一致）。
    header(2,'同一片草地，加入四行光伏板','场景与对照')
    picture('06_StreamStudio场景.png',48,205,600,340)
    for i,(a,b) in enumerate([('草场范围','18 × 18 m'),('草叶高度 / LAI','0.25 m / 1'),('板阵列','4行，南向30°'),('板总面积','96 m²'),('风速 / 短波峰值','2.5 m/s / 850 W/m²')]):
        y=520-i*55;label(a,680,y,10);label(b,680,y-22,13,INK)
    para('两组共用气象、草地、土壤与初始化。基础计算49个整点，前24个节点预热，展示第二天0–24时。第24时是下一天0时，不重复计入日平均。',48,162,840,12)
    para('光伏板：25°C效率0.21，相对温度系数−0.0035 K⁻¹，面积热容量12000 J/(m²·K)。',48,93,840,10);end()
    # 第 3 页：板温与草叶温度的日变化。
    header(3,'热的板，较凉的草','一天中的温度变化')
    picture('01_温度日变化.png',40,220,880,325)
    for i,(title,body) in enumerate([('板面升温',f'日最高约{n["panel_C"]:.1f}°C。辐照、对流、发电耗能与面积热容量共同参与控制。'),('草叶响应',f'12时草叶均温由{n["grass_open_C"]:.1f}°C变为{n["grass_pv_C"]:.1f}°C，配对变化{n["grass_difference_K"]:+.2f} K。'),('位置不同，受热不同','固定板投影下、行间与外围草叶分别统计；投影分组不等于每时刻的实际阴影。')]):
        x=48+i*294;rect(x,192,260,2,'#dce5ea');label(title,x,164,14,INK,True);para(body,x,144,260,11)
    label('草场与光伏共存的研究背景见参考文献[1,2]；本页数值来自原生模拟。',48,82,10)
    label('基础日变化：1 m草叶面元；物理温度按面元面积统计。气温曲线为共同驱动。',48,59,10);end()
    # 第 4 页：0.25 m 空间温差图。
    header(4,'把温差放回草场中','12:00 草叶空间对照')
    picture('02_草叶空间温差.png',30,195,900,370)
    para(f'0.25 m网格独立补算：无光伏草叶均温{sp["grass"]["mean_grass_C"]:.2f}°C，有光伏{sp["pv_grass"]["mean_grass_C"]:.2f}°C，配对变化{sp["difference_mean_K"]:+.2f} K。',48,169,860,13,INK)
    para('两幅温度图共用色标；温差图以零为中心，蓝色表示降低。虚线表示板的垂直投影，图中完整显示板下草叶。每格是两片原生三角面元的面积均值，未做空间插值。',48,113,860,11)
    label('本页为细网格空间诊断；全天与方向性沿用原有1 m草叶面元结果。',48,59,10);end()
    # 第 5 页：热辐射方向性。
    header(5,'换个方向，看到不同的亮温','热辐射方向性')
    picture('03_热辐射方向性.png',40,228,880,318)
    para('09、12、15时分别观测61个方向。天顶角−75°至75°，5°步长；南北平面正角朝南、负角朝北，东西平面正角朝东、负角朝西。',48,192,850,12,INK)
    para('观测改变板面、草叶、土壤的可见比例及遮挡。每幅图先平均有效像元辐亮度再反演10.5 μm亮温；曲线包含投影边界变化，不作为固定足迹的卫星角度校正。',48,128,850,11)
    label('方向性解释参考[4]。本案例板发射率取0.90，未引入实测角度依赖发射率；亮温不同于物理温度。',48,59,10);end()
    # 第 6 页：09、12、15 时天底热红外对照。
    header(6,'暖板与阴影，共同进入画面','天底热红外影像')
    picture('04_热红外影像.png',135,108,690,450)
    label('09、12、15时；统一22–48°C色标，10.5 μm。暖板与较冷草叶共同进入传感器观测。',48,71,11);end()
    # 第 7 页：光伏板温度与能量分配。
    header(7,'进入板面的能量，去了哪里？','光伏温度特性')
    picture('05_光伏温度与能量.png',40,232,880,320)
    para('吸收的辐射通过发电、显热、长波交换与储热分配，发电耗能参与原生板面能量平衡。功率随入射辐照和板温共同变化。',48,198,850,13,INK)
    para('曲线描述小时尺度的升温与冷却。每小时采样不足以可靠识别分钟级热惯性或估计时间常数；组件直流输出未计逆变器和线路损失。',48,122,850,11)
    label('板温影响因素的相关研究见[3]；本案例采用面元能量平衡，没有用经验板温式替代原生求解。',48,59,10);end()
    # 第 8 页：软件中的展现。
    header(8,'从三维场景，到可读的结果','StreamStudio中的展现')
    picture('07_StreamStudio热红外结果.png',48,190,600,355)
    for i,(t,b) in enumerate([('打开工程','project.json；面元能量平衡Facet EB。'),('查看过程','原生过程文件包含温度、能量通量和光伏功率。'),('对照结果','查看逐时热红外图像，或切换无光伏草场工程。')]):
        y=520-i*105;label(t,680,y,15,INK,True);para(b,680,y-16,220,11)
    para('软件显示热红外结果单位K，报告图转换为°C。三维场景预览用于展示对象、几何与光照，不能代替计算温度。',48,141,850,12)
    label('英文入口 D:/cases/StreamStudio/case005 指向正式D盘案例目录，以兼容当前引擎路径编码。',48,70,10);end()
    # 第 9 页：方法、参数与证据边界（左右两栏；x 坐标 48 与 500，y 从 544 向下排）。
    header(9,'保留每一条结果的来处','方法、参数与证据边界')
    left=[('计算与强迫','理想化晴夏日重复驱动；北纬40°，东经120°，UTC+8。基础场景49时次×2组，98节点全部收敛；空间细网格37时次×2组，74节点全部收敛。'),('网格与观测','全天温度与方向性：1 m草叶网格。空间图：0.25 m草叶网格独立补算。传感器96×96像元，10.5 μm；对角度图的无效边界像元剔除后再平均辐亮度。'),('数值判据','原生求解采用温差和能量残差等效温差等组合判据，temperatureTolerance=0.02 K不代表导出的最大迭代温差都小于0.02 K。实际残差、状态和文件哈希保留在qa及运行索引。')]
    right=[('解释范围','水平草叶是简化冠层；没有模拟生长、产草量或土壤水分长期变化。两组气温由相同气象指定，没有启用流体耦合，不能据此声称降低近地气温或区域热岛。'),('空间精细化','细网格没有改变草场面积、LAI、板阵列、气象与初始化。图中只平均同一0.25 m方格内两片三角面元，未用插值或平滑产生新温度。细化是空间诊断，不构成独立观测验证。'),('文件与复现','project.json为展示工程；scenarios为基础输入；runs保留原生状态；refinement-025m保留细网格输入与输出；tables为可复核表格；provenance和qa保留版本及哈希。完整说明见README.md。')]
    for x,items in [(48,left),(500,right)]:
        y=544
        for t,b in items:label(t,x,y,15,INK,True);used=para(b,x,y-20,410,11);y-=used+52
    label('电站近地气温效应的研究背景见[5]；本案例不模拟该响应。',48,82,10)
    label('本案例为理想化数值教学对照，未完成独立光伏电站实测验证。',48,59,11,GREEN);end()
    # 附加页（参考文献与下载入口等）由 case005_resources.add_pdf_pages 绘制。
    add_pdf_pages(c,header,para,label,end)
    c.save()
# 入口：先备份将被改写的旧件（仅首次），再重新生成空间图、网页、PDF，补写 README 记录，
# 并把本目录脚本复制到 ROOT/reproduce。
def main():
    backup=ROOT/'qa/layout-before-20261107';backup.mkdir(exist_ok=True)
    for rel in ['index.html','案例005_光伏草场热环境.pdf','figures/02_草叶空间温差.png','qa/delivery-manifest.json']:
        dest=backup/pathlib.Path(rel).name
        if not dest.exists():shutil.copy2(ROOT/rel,dest)
    summary=json.loads((ROOT/'tables/summary.json').read_text(encoding='utf8'))
    save_resources()
    sp=spatial();website(summary,sp);report(summary,sp)
    add='\n\n## 2026-11-07 空间图细化与排版\n草叶空间图已改用 refinement-025m 中0.25 m草叶网格独立补算（两组各37时次，前24时次预热，保留至第二天12时）。每格只平均两片原生三角面元，不进行空间插值。基础日变化、方向性和摘要数值仍采用原有1 m草叶网格；图中及PDF已分别标明。细网格表见tables/spatial_noon_025m.csv，核验见qa/spatial-refinement-audit.json。网页增加逐时热红外对照滑块，图像均来自原生TIFF。\n'
    readme=ROOT/'README.md';text=readme.read_text(encoding='utf8')
    if '## 2026-11-07 空间图细化与排版' not in text:readme.write_text(text+add,encoding='utf8')
    for p in HERE.glob('*'):
        if p.is_file():shutil.copy2(p,ROOT/'reproduce'/p.name)
    print('Layout and 0.25 m spatial figure created',json.dumps(sp,ensure_ascii=True),flush=True)
if __name__=='__main__':main()
