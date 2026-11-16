"""Verified scientific references and existing public software entry points."""
import html,json
from pv_case005 import ROOT,dump
REFERENCES=[
 dict(id=1,authors='Armstrong, A., Ostle, N. J., & Whitaker, J.',year=2016,title='Solar park microclimate and vegetation management effects on grassland carbon cycling',journal='Environmental Research Letters, 11(7), 074016',doi='10.1088/1748-9326/11/7/074016',source='https://iopscience.iop.org/article/10.1088/1748-9326/11/7/074016',note='草地光伏场中板下、行间与对照区的微气候研究，用于理解场景对照；文献实测值不等同于本案例草叶温度。'),
 dict(id=2,authors='Barron-Gafford, G. A., et al.',year=2019,title='Agrivoltaics provide mutual benefits across the food–energy–water nexus in drylands',journal='Nature Sustainability, 2, 848–855',doi='10.1038/s41893-019-0364-5',source='https://www.nature.com/articles/s41893-019-0364-5',note='光伏与植被耦合的研究背景；不能把特定干旱区作物试验外推为本案例的产草量结论。'),
 dict(id=3,authors='Faiman, D.',year=2008,title='Assessing the outdoor operating temperature of photovoltaic modules',journal='Progress in Photovoltaics: Research and Applications, 16(4), 307–315',doi='10.1002/pip.813',source='https://onlinelibrary.wiley.com/doi/10.1002/pip.813',note='用于理解辐照、环境气温与风速对板温的控制；本案例求解面元能量平衡，没有用该经验式替代板温。'),
 dict(id=4,authors='Cao, B., Liu, Q., Du, Y., et al.',year=2019,title='A review of earth surface thermal radiation directionality observing and modeling: Historical development, current status and perspectives',journal='Remote Sensing of Environment, 232, 111304',doi='10.1016/j.rse.2019.111304',source='https://repository.library.noaa.gov/view/noaa/45209',note='用于热辐射方向性、可见组分和方向亮温的解释；本案例没有验证实测的板面角度发射率。'),
 dict(id=5,authors='Barron-Gafford, G. A., Minor, R. L., Allen, N. A., et al.',year=2016,title='The Photovoltaic Heat Island Effect: Larger solar power plants increase local temperatures',journal='Scientific Reports, 6, 35070',doi='10.1038/srep35070',source='https://www.nature.com/articles/srep35070',note='用于区分草叶/板面温度与电站近地气温效应；本案例没有求解气温响应，不能据此判定热岛强弱。')]
LINKS=[
 dict(name='GitHub 源代码',url='https://github.com/bianzj/streamstudio',detail='StreamStudio 软件源码仓库'),
 dict(name='GitHub 程序发布',url='https://github.com/bianzj/streamstudio/releases/tag/v2.0.1',detail='v2.0.1：Windows安装程序、手册与发布资料'),
 dict(name='发布版源码 ZIP',url='https://github.com/bianzj/streamstudio/releases/download/v2.0.1/StreamStudio-2.0.1-source.zip',detail='与v2.0.1发布对应的源码包'),
 dict(name='百度网盘',url='https://pan.baidu.com/s/1iYGVT5aC67xQ9T5A00A_YA?pwd=uvx2',detail='程序与资料分享入口；提取码：uvx2')]
def reference_html():
    refs=[]
    for r in REFERENCES:
        refs.append(f'<li id="ref-{r["id"]}"><span class="ref-index">[{r["id"]}]</span><div><p>{html.escape(r["authors"])} ({r["year"]}). <a href="https://doi.org/{r["doi"]}" target="_blank" rel="noopener">{html.escape(r["title"])}</a>. <em>{html.escape(r["journal"])}</em>.</p><p class="ref-purpose">{r["note"]}</p><a class="ref-doi" href="https://doi.org/{r["doi"]}" target="_blank" rel="noopener">doi:{r["doi"]}</a></div></li>')
    return '<section id="references" class="wrap section reference-section"><div class="section-heading"><div><p class="sub">参考文献</p><h2>从草场微气候，到热辐射方向性</h2></div><p>文献提供研究背景与物理解释。<br>本文数值来自原生模拟，不作为文献实测复现。</p></div><ol class="references">'+''.join(refs)+'</ol></section>'
def software_html():
    entries=''.join(f'<a href="{html.escape(l["url"],quote=True)}" target="_blank" rel="noopener"><strong>{l["name"]}</strong><span>{l["detail"]}</span><small>{html.escape(l["url"])}</small></a>' for l in LINKS)
    return '<section id="software" class="software-links-section"><div class="wrap section"><div class="section-heading"><div><p class="sub">代码与程序</p><h2>获取 StreamStudio</h2></div><p>现有公开源码与程序发布入口。<br>百度网盘提取码：<b>uvx2</b></p></div><div class="software-links">'+entries+'</div><p class="figure-caption">本案例复现脚本位于本地 reproduce/；软件公开下载包内容以对应发布页或网盘为准。</p></div></section>'
def add_pdf_pages(c,header,para,label,end):
    header(10,'从草场微气候，到热辐射方向性','参考文献')
    y=546
    for r in REFERENCES:
        label(f'[{r["id"]}]',48,y-11,11,'#245e8a',True)
        citation=f'{html.escape(r["authors"])} ({r["year"]}). {html.escape(r["title"])}. {html.escape(r["journal"])}. <link href="https://doi.org/{r["doi"]}" color="#245e8a">doi:{r["doi"]}</link>'
        h=para(citation,84,y,824,10,'#19364b');y-=h+4
        h=para(r['note'],84,y,824,9,'#637887');y-=h+17
    assert y>50,('Reference page overflow',y)
    label('文献用于研究背景与物理解释；本案例数值为原生模拟，不作为文献实测结果的复现。',48,52,10,'#267861');end()
    header(11,'获取源代码与程序','StreamStudio 公开资源')
    y=535
    for l in LINKS:
        label(l['name'],48,y,15,'#19364b',True)
        para(l['detail'],48,y-16,850,10)
        para(f'<link href="{html.escape(l["url"],quote=True)}" color="#245e8a">{html.escape(l["url"])}</link>',48,y-43,850,10,'#245e8a')
        y-=105
    para('本案例复现脚本位于本地 reproduce/。以上是软件现有公开入口，公开下载包是否包含本次新增案例以对应发布页或网盘内容为准。',48,103,850,10)
    label('百度网盘提取码：uvx2   |   链接核对日期：2026-11-07',48,51,11,'#267861');end()
def save_resources():
    dump(ROOT/'provenance/references-and-downloads.json',dict(checked_date='2026-11-07',references=REFERENCES,software=LINKS,download_verification={'github_repository':'public page opened','github_release':'v2.0.1 public page and asset names checked through GitHub API','baidu':'share opened; STREAMSTUDIO folder visible, extraction code uvx2; binaries not downloaded'},scientific_boundary='references supply background only; native modeled numbers are not independent field validation'))
    (ROOT/'参考文献与下载.md').write_text('# 参考文献与软件下载\n\n'+'\n\n'.join(f'[{r["id"]}] {r["authors"]} ({r["year"]}). {r["title"]}. {r["journal"]}. https://doi.org/{r["doi"]}\n\n{r["note"]}' for r in REFERENCES)+'\n\n## 代码与程序\n\n'+'\n\n'.join(f'[{l["name"]}]({l["url"]})\n\n{l["detail"]}' for l in LINKS)+'\n\n核对日期：2026-11-07。文献为背景参考，不是本案例独立验证。\n',encoding='utf8')
    for name in ['README.md','打开案例.md']:
        p=ROOT/name;text=p.read_text(encoding='utf8');marker='## 参考文献与代码程序下载'
        if marker not in text:
            p.write_text(text+'\n\n'+marker+'\n\n完整条目及对应解释见 [参考文献与下载.md](参考文献与下载.md)。\n\n'+'\n\n'.join(f'- [{l["name"]}]({l["url"]})：{l["detail"]}' for l in LINKS)+'\n\n场景草地外观已启用Three.js草地预览纹理，仅影响场景展示；模拟输入及物理参数保留。\n',encoding='utf8')
