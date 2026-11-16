"""Four coordinated social/editorial cards; only uses existing native outputs."""
import csv,hashlib,html,json,pathlib,re,shutil,subprocess
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.colors import LinearSegmentedColormap
from matplotlib.patches import Rectangle
from reportlab.pdfgen import canvas
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import Paragraph
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.colors import HexColor
from reportlab.lib.utils import ImageReader
from pv_case005 import ROOT,sha,dump
from case005_resources import REFERENCES,LINKS
REPO=pathlib.Path('C:/work/streamstudio');DEST=REPO/'docs/cases/005-photovoltaic'
OUT=ROOT/'publication/promotion';INK='#19364b';MUTED='#637887';BLUE='#245e8a';GREEN='#267861'
CARD_COUNT=3
def csvread(name):return list(csv.DictReader((ROOT/'tables'/name).open(encoding='utf-8-sig')))
def plots():
    font_manager.fontManager.addfont('C:/Windows/Fonts/msyh.ttc');plt.rcParams.update({'font.family':'Microsoft YaHei','font.size':16,'axes.unicode_minus':False,'axes.spines.top':False,'axes.spines.right':False,'figure.facecolor':'white','axes.facecolor':'white'})
    rows=csvread('spatial_noon_025m.csv');g=np.array([float(r['open_C']) for r in rows]).reshape(72,72);p=np.array([float(r['pv_C']) for r in rows]).reshape(72,72)
    cm=LinearSegmentedColormap.from_list('temps',['#d9e9f1','#afcbd8','#f2eddc','#dfb873','#bb6351'])
    fig,axs=plt.subplots(1,2,figsize=(9.5,4.5),layout='constrained')
    for i,(a,v,t) in enumerate(zip(axs,[g,p],['无光伏草场','光伏草场（含板下草叶）'])):
        im=a.imshow(v,origin='lower',extent=[0,18,0,18],cmap=cm,vmin=26,vmax=42,interpolation='nearest');a.set(title=t,xticks=[0,9,18],yticks=[0,9,18],xlabel='东向 / m',ylabel='北向 / m' if i==0 else '')
        if i:
            for y in [3,7,11,15]:a.add_patch(Rectangle((3,y),12,np.sqrt(3),fill=False,edgecolor='#526a78',lw=.8,ls='--'))
    fig.colorbar(im,ax=axs,orientation='horizontal',fraction=.09,pad=.03,label='12:00 草叶物理温度 / °C',ticks=[26,30,34,38,42]);fig.savefig(OUT/'assets/maps.png',dpi=120);plt.close(fig)
    rows=csvread('temperature_diurnal.csv');fig,ax=plt.subplots(figsize=(9.5,3.6),layout='constrained')
    for key,label,color in [('panel_C','光伏板',BLUE),('grass_open_C','无光伏草叶','#b9853e'),('grass_pv_C','有光伏草叶',GREEN)]:ax.plot([float(r['hour']) for r in rows],[float(r[key]) for r in rows],color=color,lw=2.8,label=label)
    ax.set(xlabel='第二天当地时间 / h',ylabel='物理温度 / °C',xticks=[0,6,12,18,24]);ax.legend(loc='upper left',fontsize=13,ncol=1,frameon=False);ax.grid(alpha=.12);fig.savefig(OUT/'assets/diurnal.png',dpi=120);plt.close(fig)
    rows=csvread('thermal_directionality.csv');fig,ax=plt.subplots(figsize=(9.5,3.9),layout='constrained')
    for scenario,label,color in [('grass','无光伏草场',GREEN),('pv_grass','光伏草场',BLUE)]:
        pts=sorted([(float(r['vza_deg'])*(1 if float(r['vaa_deg'])==180 else -1),float(r['brightness_C'])) for r in rows if r['scenario']==scenario and int(r['hour'])==12 and float(r['vaa_deg']) in [0,180]])
        ax.plot(*zip(*pts),label=label,color=color,lw=2.8)
    ax.set(xlabel='观测天顶角 / °（负：北向，正：南向）',ylabel='10.5 μm 亮温 / °C',xticks=[-75,-50,-25,0,25,50,75]);ax.legend(fontsize=13,frameon=False);ax.grid(alpha=.12);fig.savefig(OUT/'assets/angular.png',dpi=120);plt.close(fig)
    from pv_media005 import readtif
    from matplotlib import colormaps
    from PIL import Image
    for z,az in [(60,0),(0,0),(60,180)]:
        pattern=f'T=DOY180_12-00_VZA={z:.2f}_VAA={az:.2f}*.tif' if z else 'T=DOY180_12-00_VZA=0.00_*.tif'
        path=next((ROOT/'sensor/pv_grass/angular').glob(pattern));a=readtif(path);rgb=(colormaps['inferno'](np.clip((np.nan_to_num(a,nan=22)-22)/26,0,1))[:,:,:3]*255).astype('uint8');Image.fromarray(rgb).save(OUT/f'assets/view-{z}-{az}.png')
def cards():
    pdfmetrics.registerFont(TTFont('YaHei','C:/Windows/Fonts/msyh.ttc'));pdfmetrics.registerFont(TTFont('YaHeiBold','C:/Windows/Fonts/msyhbd.ttc'))
    c=canvas.Canvas(str(OUT/'report.pdf'),pagesize=(1080,1440));c.setTitle('STREAMSTUDIO | 光伏草场热环境 · 案例005');c.setAuthor('StreamStudio')
    def text(s,x,y,size=28,color=INK,bold=False):c.setFillColor(HexColor(color));c.setFont('YaHeiBold' if bold else 'YaHei',size);c.drawString(x,1440-y-size,s)
    def para(s,x,y,w,size=25,color=MUTED):
        p=Paragraph(s,ParagraphStyle('p',fontName='YaHei',fontSize=size,leading=size*1.6,textColor=HexColor(color),wordWrap='CJK'));_,h=p.wrap(w,1300);p.drawOn(c,x,1440-y-h);return h
    def box(x,y,w,h,col):c.setFillColor(HexColor(col));c.rect(x,1440-y-h,w,h,fill=1,stroke=0)
    def image(p,x,y,w,h):
        im=ImageReader(str(p));iw,ih=im.getSize();sc=min(w/iw,h/ih);c.drawImage(im,x+(w-iw*sc)/2,1440-y-h+(h-ih*sc)/2,width=iw*sc,height=ih*sc,mask='auto')
    def scene(y,h):
        x=65;w=950;box(x,y,w,h,'#f1f5f9');im=ImageReader(str(ROOT/'figures/06_StreamStudio场景.png'));iw,ih=im.getSize();sc=min(w/1240,h/695);vx=x+(w-1240*sc)/2
        viewport_y=1440-y-h+(h-695*sc)/2
        c.saveState();p=c.beginPath();p.rect(vx,viewport_y,1240*sc,695*sc);c.clipPath(p,stroke=0);c.drawImage(im,vx-300*sc,viewport_y-(ih-840)*sc,width=iw*sc,height=ih*sc);c.restoreState()
    def top(n,tag):text('StreamStudio',65,48,25,INK,True);text('案例005 · '+tag,680,52,18,MUTED);box(65,96,950,1,'#dce5ea')
    def foot(n):
        box(65,1325,950,1,'#dce5ea')
        if n==CARD_COUNT:
            text('代码与程序：github.com/bianzj/streamstudio',65,1341,15,MUTED)
            text('百度网盘：pan.baidu.com/s/1iYGVT5aC67xQ9T5A00A_YA?pwd=uvx2   提取码：uvx2',65,1366,15,MUTED)
            c.linkURL(LINKS[0]['url'],(65,1440-1362,1015,1440-1340));c.linkURL(LINKS[3]['url'],(65,1440-1388,1015,1440-1366))
        text('理想化数值案例 · 未完成独立实测验证',65,1400,14,MUTED);text(f'{n}/{CARD_COUNT}',970,1400,14,MUTED)
        c.showPage()
    top(1,'光伏与草场热环境')
    image(REPO/'design/branding/stream-studio-logo-concept.png',65,145,105,105)
    text('STREAMSTUDIO',192,158,75,INK,True);text('光伏 × 草场',65,273,78,GREEN,True)
    para('三维场景中的温度变化与热辐射方向性',65,390,950,31,BLUE)
    box(65,475,950,605,'#f1f5f9')
    image(ROOT/'figures/06_StreamStudio场景.png',65,475,950,605)
    text('StreamStudio 软件界面 · 光伏阵列与草场',65,1090,21,MUTED)
    for i,label in enumerate(['有、无光伏对照','板温与草叶温度','多角度热红外']):
        x=65+i*325;box(x,1130,285,2,'#dce5ea');text(label,x,1151,28,INK,True)
    para('基于三维辐射传输与能量平衡，展示光伏阵列与草场的热环境。<br/>18 × 18 m草场 · 南向30°阵列 · 原生数值案例 [1,2]',65,1230,950,23)
    foot(1)
    top(2,'草场对照与日变化');text('同一片草场，',65,145,62,INK,True);text('板下与行间怎样变化？',65,227,62,INK,True)
    image(OUT/'assets/maps.png',55,333,970,435)
    text('午间草叶均温变化 −2.32 K',65,785,35,GREEN,True)
    para('0.25 m原生网格独立补算；虚线是板的垂直投影。<br/>每格平均两片原生三角面元，没有空间插值。',65,840,950,23)
    image(OUT/'assets/diurnal.png',55,945,970,285)
    para('上图是0.25 m午间空间诊断；全天曲线沿用1 m草叶网格。<br/>前24个节点预热。草叶温差不能直接解释为气温变化。[1,2,5]',65,1245,950,20)
    foot(2)
    top(3,'热辐射方向性');text('换一个角度看，',65,135,59,INK,True);text('热红外亮温也会变',65,215,59,INK,True)
    para('同一温度场 · 12:00 · 10.5 μm<br/>板面、草叶与土壤的可见比例随方向改变。[4]',65,315,950,24)
    image(OUT/'assets/angular.png',55,402,970,300)
    text('61个方向，场景亮温极差约 5.65 K',65,718,32,BLUE,True)
    for i,(z,az,name) in enumerate([(60,0,'北向60°'),(0,0,'天底0°'),(60,180,'南向60°')]):
        image(OUT/f'assets/view-{z}-{az}.png',65+i*325,786,300,190);text(name,130+i*325,987,21,INK)
    para('同一22–48°C色标；曲线示南北平面。亮温不同于物理温度；<br/>板发射率固定取0.90，未验证角度依赖发射率。',65,1033,950,18)
    text('参考文献（完整条目见配套文案）',65,1102,21,INK,True)
    short=['Armstrong et al. (2016), ERL 11:074016','Barron-Gafford et al. (2019), Nature Sustainability 2:848–855','Faiman (2008), Progress in Photovoltaics 16:307–315','Cao et al. (2019), Remote Sensing of Environment 232:111304','Barron-Gafford et al. (2016), Scientific Reports 6:35070']
    y=1144
    for r,title in zip(REFERENCES,short):
        citation=f'[{r["id"]}] {title}. <link href="https://doi.org/{r["doi"]}" color="#245e8a">doi:{r["doi"]}</link>'
        y+=para(citation,65,y,950,14,INK)+6
    assert y<1315,('References exceed footer',y)
    foot(3);c.save()
def copywriting():
    refs='\n\n'.join(f'[{r["id"]}] {r["authors"]} ({r["year"]}). {r["title"]}. {r["journal"]}. https://doi.org/{r["doi"]}' for r in REFERENCES)
    footer=''  # Resource entry appears once, below the third card.
    xhs='''# 光伏板下的草地，真的更凉吗？

板面很热，板下的草叶也一定更热吗？

用StreamStudio做了一组理想化对照：同一片18×18 m草场、同一气象，只增加4行南向光伏板。

① 看温度：基础网格12时，板平均温度约46.2°C，草叶均温从37.6°C变为35.2°C。
② 看位置：0.25 m网格的午间补算显示，板下与行间存在明显温度差异，全场草叶均温变化约−2.32 K。
③ 换角度：同一温度场，从61个方向观察，10.5 μm场景亮温极差约5.65 K。

三张图依次介绍：StreamStudio光伏草场封面、草场温度对照、热辐射方向性与参考文献。

注意：这是原生模型数值案例，不是电站实测验证。草叶降温不代表气温或区域热岛必然降低；方向亮温也不是板的物理温度。

参考文献见图3及references-and-downloads.md。

#光伏 #草地 #热红外遥感 #微气候 #StreamStudio #科研可视化
'''+footer
    (OUT/'小红书文案.md').write_text(xhs,encoding='utf8')
    article='''# 光伏板下的草地为何会变凉，热红外图像却仍然显热？

光伏板很热，并不意味着板下草叶以相同方式升温。草叶的物理温度、板的物理温度和传感器观测的热红外亮温，描述的是不同对象。

![场景与主要结果](promotion/page-1.png)

## 在同一气象下比较有、无光伏草场

本案例用StreamStudio建立18×18 m的草场，加入4行南向30°光伏阵列，总组件面积96 m²。两组使用相同的气象、土壤、草地与初始化。已有草地光伏场研究表明，板下、行间和对照区域的微气候响应会受到季节、地表和管理条件影响[1]；光伏与植被共存的收益也具有环境条件边界[2]。

基础网格的12时结果中，板平均物理温度约46.2°C；无光伏草叶均温约37.6°C，有光伏约35.2°C。草叶遮阴减少短波吸收，板面长波及天空遮挡也参与交换。白天的差异不能直接外推为全天或所有气候下的普遍规律。

![空间与日变化](promotion/page-2.png)

空间图另外采用0.25 m草叶网格补算，午间全场草叶均温变化约−2.32 K。图中每格只平均两片原生三角面元，没有插值。全天曲线和下述方向性结果沿用1 m网格，避免将不同网格的数值混作同一组。

## 同一温度场，为什么从不同方向看到不同亮温？

从不同方向观察时，光伏板、草叶和土壤进入视场的比例不同，遮挡和反射天空长波也会改变观测。热辐射方向性综述对此给出了系统解释[4]。

![热辐射方向性](promotion/page-3.png)

本案例在09、12、15时各生成61个方向的热红外图像。12时，有光伏场景在所采方向的10.5 μm亮温极差约5.65 K。各图先平均有效像元辐亮度，再反演亮温；结果包含随视角变化的组分和投影边界，不能直接作为固定足迹的角度校正，也没有验证真实板面的角度依赖发射率。

## 软件中如何展示？

StreamStudio把光伏阵列、草地、太阳和观测方向放在同一场景中。面元能量平衡输出板温、草叶温度和能量通量，发电耗能参与板面能量分配。辐照、气温与风速对板温的影响可参考Faiman[3]；本案例并未以该经验式替代原生求解。

基础计算98节点及空间补算74节点全部通过引擎原生收敛判据。不过，收敛不等于独立科学验证。本案例是理想化教学对照，未模拟气温响应、草地生长或产草量。电站气温效应需要另行研究[5]，不能从草叶温差推出热岛增强或减弱。

## 参考文献

'''+refs+footer
    (OUT/'知乎文章.md').write_text(article,encoding='utf8')
    readme='''# 案例005：光伏草场热环境

用3张图介绍：StreamStudio光伏草场宣传封面、草场温度对照与热辐射方向性。

[3页PDF](report.pdf) · [小红书文案](小红书文案.md) · [知乎文章](知乎文章.md) · [参考文献](references-and-downloads.md)

![场景与主要结果](promotion/page-1.png)

![空间与日变化](promotion/page-2.png)

![热辐射方向性](promotion/page-3.png)

基础温度与方向性采用1 m草叶网格；空间图独立补算0.25 m网格，二者分别标明。理想化数值教学案例，未完成独立实测验证；草叶温差不代表气温或区域热岛变化。

本次仅发布宣传材料，不包含工程输入与完整原生状态。

'''
    (OUT/'README.md').write_text(readme.rstrip()+'\n',encoding='utf8')
def publish():
    backup=ROOT/'qa/promotion-before-compact';backup.mkdir(exist_ok=True)
    for p in [ROOT/'案例005_光伏草场热环境.pdf',ROOT/'index.html']:
        if not (backup/p.name).exists():shutil.copy2(p,backup/p.name)
    shutil.copy2(OUT/'report.pdf',ROOT/'案例005_光伏草场热环境.pdf');shutil.copy2(OUT/'report.pdf',DEST/'report.pdf')
    (DEST/'promotion').mkdir(exist_ok=True)
    for i in range(1,CARD_COUNT+1):shutil.copy2(OUT/f'page-{i}.png',DEST/f'promotion/page-{i}.png')
    for name in ['README.md','小红书文案.md','知乎文章.md']:shutil.copy2(OUT/name,DEST/name)
    # Download entry is already embedded below card 3; do not repeat it.
    images=''.join(f'<section><img src="promotion/page-{i}.png" alt="案例005推广图{i}"></section>' for i in range(1,CARD_COUNT+1))
    web='<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>005 光伏草场热环境</title><style>body{margin:0;background:#f1f5f7;color:#19364b;font:15px/1.8 "Microsoft YaHei",sans-serif}header{max-width:1080px;margin:25px auto;padding:0 20px;display:flex;justify-content:space-between;align-items:center}main{max-width:1080px;margin:auto}section{margin-bottom:28px}img{display:block;width:100%;height:auto;background:white}.resource{font-size:11px;color:#637887;margin:0;padding:9px 18px}a{color:#245e8a}h1{font-size:19px;font-weight:500}.refs{max-width:1040px;margin:30px auto;padding:0 20px;color:#637887;font-size:12px}@media(min-width:1250px){main{max-width:1200px;display:grid;grid-template-columns:1fr 1fr;gap:24px}}@media(prefers-reduced-motion:no-preference){html{scroll-behavior:smooth}}</style><header><h1>StreamStudio · 案例005</h1><a href="report.pdf">3页PDF</a></header><main>'+images+'</main><div class="refs"><a href="references-and-downloads.md">完整参考文献</a> · <a href="小红书文案.md">小红书文案</a> · <a href="知乎文章.md">知乎文章</a><p>本案例为理想化数值教学对照，未完成独立实测验证。</p></div></html>'
    (DEST/'index.html').write_text(web,encoding='utf8')
    (ROOT/'promotion').mkdir(exist_ok=True)
    for i in range(1,CARD_COUNT+1):shutil.copy2(OUT/f'page-{i}.png',ROOT/f'promotion/page-{i}.png')
    local=web.replace('href="report.pdf"','href="案例005_光伏草场热环境.pdf"').replace('references-and-downloads.md','参考文献与下载.md')
    (ROOT/'index.html').write_text(local,encoding='utf8')
    for name in ['小红书文案.md','知乎文章.md']:shutil.copy2(OUT/name,ROOT/name)
    (DEST/'.gitattributes').write_bytes(b'*.pdf binary\n')
    for path in [REPO/'docs/cases/catalog.json',REPO/'docs/cases/README.md']:
        if path.suffix=='.json':
            cat=json.loads(path.read_text(encoding='utf8'));case=next(c for c in cat['cases'] if c['id']=='005');case.update(figures=CARD_COUNT,report_pages=CARD_COUNT,promotion_platforms=['xiaohongshu','zhihu','github']);dump(path,cat)
        else:
            text=path.read_text(encoding='utf8').replace('| 005 | [光伏草场热环境](005-photovoltaic/README.md) | 7图 + 动态图 |','| 005 | [光伏草场热环境](005-photovoltaic/README.md) | 3张融合推广图 |').replace('| 4张融合推广图 |','| 3张融合推广图 |');path.write_text(text,encoding='utf8')
    files=[p for p in DEST.rglob('*') if p.is_file() and p.name!='promotion-manifest.json']
    dump(DEST/'promotion-manifest.json',dict(date='2026-11-07',scope='promotional_materials_only',model_rerun_during_publication=False,report_pages=CARD_COUNT,promotion_cards=CARD_COUNT,base_native_converged_nodes=98,spatial_refinement_converged_nodes=74,references=5,published_files=[dict(path=p.relative_to(DEST).as_posix(),bytes=p.stat().st_size,sha256=sha(p)) for p in files]))
    manifest=REPO/'docs/cases/manifest.json';m=json.loads(manifest.read_text(encoding='utf8'));m['files'].pop('README.md',None)
    m['files'].pop('docs/cases/005-photovoltaic/promotion/page-4.png',None)
    for p in [REPO/'docs/cases/README.md',REPO/'docs/cases/catalog.json',*[p for p in DEST.rglob('*') if p.is_file()]]:m['files'][p.relative_to(REPO).as_posix()]=dict(bytes=p.stat().st_size,sha256=sha(p))
    dump(manifest,m)
    print('Prepared three-page promotion for Xiaohongshu, Zhihu and GitHub',flush=True)
def main():
    previous=ROOT/'qa/previous-four-card-promotion';previous.mkdir(exist_ok=True)
    if (OUT/'report.pdf').exists() and not (previous/'report.pdf').exists():shutil.copy2(OUT/'report.pdf',previous/'report.pdf')
    for label,path in [('source',OUT/'page-4.png'),('public',DEST/'promotion/page-4.png'),('local',ROOT/'promotion/page-4.png')]:
        # Exact files from this case only; preserve the old artwork in QA.
        resolved=path.resolve()
        assert resolved.is_relative_to(ROOT.resolve()) or resolved.is_relative_to(DEST.resolve())
        if path.exists():shutil.copy2(path,previous/(label+'-page-4.png'));path.unlink()
    (OUT/'assets').mkdir(parents=True,exist_ok=True);plots();cards();copywriting()
    subprocess.run(['pdftoppm','-scale-to','1440','-png',str(OUT/'report.pdf'),str(OUT/'page')],check=True)
    publish()
if __name__=='__main__':main()
