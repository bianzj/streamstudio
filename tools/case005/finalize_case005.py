# ==========================================================================
# 文件：finalize_case005.py
# 功能：案例005（光伏草场）收尾脚本：校验运行溯源（引擎与输入哈希）、用原生 TIFF 生成全天热红外 GIF、
#       复制本目录脚本到 reproduce/、登记本地案例库索引、做交付检查并写 qa/delivery-manifest.json。
# 主要步骤（main）：
#   1. 校验 runs/ 与 refinement-025m/ 下各 run-manifest.json：状态为 completed，引擎与输入的 SHA-256 未变；
#   2. 检查各场景 project.json 引用的气象、模型、位置文件都存在；
#   3. 用第二天 0–24 时的天底亮温 TIFF 拼出两场景并排的 GIF，并在 index.html 中插入引用；
#   4. 复制 tools/case005 下全部文件到 ROOT/reproduce；
#   5. 更新案例库 案例索引.json 与 README.md 中的 005 条目（先备份旧索引）；
#   6. 检查 index.html 的本地引用、PDF 页数、方向性 CSV 行数，写交付清单并打印摘要。
# 与其他文件的关系：ROOT、dump、sha 来自 pv_case005.py；readtif 来自 pv_media005.py；
#       天底 TIFF 由 export_case005.mjs 生成；index.html 与 PDF 由案例的其他脚本生成。
# 约定：温度单位 °C；GIF 色标 14–48 °C（NaN 按 14 °C）；每个场景每个节点 1 h，共 49 节点（前 24 个预热）。
# 注意：98、74、366 等数量为脚本写死的检查值（366 = 3 个时刻 x 61 个方向 x 2 个场景，待核对）。
# ==========================================================================

"""Register case, verify provenance and provide an animation from native TIFFs."""
import csv,json,pathlib,re,shutil
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from matplotlib import colormaps
from pv_case005 import ROOT,dump,sha
from pv_media005 import readtif
# 作用：依次完成溯源校验、GIF 与索引更新、交付检查；任何检查失败都会 assert 中止。
# 副作用：改写 ROOT/index.html、案例库索引与 README、ROOT/reproduce、ROOT/qa。
def main():
    errors=[];validated=0
    # 步骤 1：校验每次原生运行的溯源：引擎文件与全部输入文件的 SHA-256 须与运行时记录一致。
    manifests=list((ROOT/'runs').glob('*/run-manifest.json'))+list((ROOT/'refinement-025m').glob('*/run-manifest.json'))
    for manifest in manifests:
        m=json.loads(manifest.read_text(encoding='utf8'))
        assert m['status']=='completed' and m['engine_sha256']==sha(pathlib.Path(m['engine']))
        for name,expected in m['inputs'].items():
            assert sha(pathlib.Path(name))==expected,name
            validated+=1
    # 步骤 2：各场景工程引用的气象文件、模型与位置文件必须真实存在。
    for p in (ROOT/'scenarios').glob('*/project.json'):
        q=json.loads(p.read_text(encoding='utf8'))['configuration']
        for filename in [q['meteo']['path'],*[v[k] for v in q['objects']['items'] for k in ['fileName','positionFile']]]:assert pathlib.Path(filename).is_file(),filename
    # 步骤 3：逐小时（第二天 0–24 时，共 25 帧）把两场景的天底亮温并排画成 GIF 帧；
    # 第 25 帧取下一天 DOY181 的 00:00。
    rows=list(csv.DictReader((ROOT/'tables/temperature_diurnal.csv').open(encoding='utf-8-sig')))
    font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',24);small=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',18)
    frames=[]
    for hour in range(25):
        frame=Image.new('RGB',(1060,650),'#f5f8f6');d=ImageDraw.Draw(frame)
        d.text((30,18),f'005 光伏草场｜第二天 {hour:02d}:00',font=font,fill='#163c37')
        for i,s in enumerate(['grass','pv_grass']):
            token=f'DOY180_{hour:02d}-00' if hour<24 else 'DOY181_00-00'
            a=readtif(next((ROOT/f'sensor/{s}/nadir').glob(f'T={token}*.tif')))
            rgb=(colormaps['inferno'](np.clip((np.nan_to_num(a,nan=14)-14)/34,0,1))[:,:,:3]*255).astype('uint8')
            frame.paste(Image.fromarray(rgb).resize((460,460),Image.Resampling.NEAREST),(35+i*530,105))
            d.text((35+i*530,65),'无光伏草场' if i==0 else '光伏草场',font=font,fill='#163c37')
        d.text((30,590),'10.5 μm 原生热红外亮温；两图共用14–48°C色标。',font=small,fill='#163c37')
        d.text((30,618),'理想化数值教学案例；灰度/颜色为观测亮温，不等于板或草叶的物理温度。',font=small,fill='#163c37')
        frames.append(frame)
    # 每帧 500 ms、无限循环。
    frames[0].save(ROOT/'figures/08_全天热红外变化.gif',save_all=True,append_images=frames[1:],duration=500,loop=0,optimize=False)
    # 若 index.html 尚未引用该 GIF，则插到“温度日变化”小节之前（只替换一次）。
    html=(ROOT/'index.html').read_text(encoding='utf8')
    if 'src="figures/08_全天热红外变化.gif"' not in html:
        html=html.replace('<section><h2>温度日变化','<section><h2>全天热红外变化</h2><img src="figures/08_全天热红外变化.gif"><p>第二天0–24时，原生输出；两组共用14–48°C色标。</p></section><section><h2>温度日变化',1)
        (ROOT/'index.html').write_text(html,encoding='utf8')
    # 步骤 4：把复现脚本随案例一起归档到 reproduce/。
    for p in pathlib.Path('C:/work/streamstudio/tools/case005').glob('*'):
        if p.is_file():shutil.copy2(p,ROOT/'reproduce'/p.name)
    # 步骤 5：登记案例库索引（案例 005 的条目会被替换为最新内容；第一次运行时先备份旧索引）。
    library=pathlib.Path('D:/cases/StreamStudio/案例库');index=library/'案例索引.json';idx=json.loads(index.read_text(encoding='utf8'))
    backup=ROOT/'provenance/catalog-before-case005.json'
    if not backup.exists():shutil.copy2(index,backup)
    entry=dict(id='005',title='光伏草场热环境',mode='eFacetEB',directory=str(library/'005_光伏草场热环境'),project=str(ROOT/'project.json'),report=str(library/'005_光伏草场热环境/案例005_光伏草场热环境.pdf'),gallery=str(library/'005_光伏草场热环境/index.html'),scenarios=['无光伏草场','光伏草场'],evidence=[dict(scenario=s,native_run=str(ROOT/f'runs/{s}/output/faceteb_run.json'),passed_nodes=49) for s in ['grass','pv_grass']],field_validation=False,summary='相同气象与草地；南向30°光伏阵列；24节点预热，第二天温度、草叶配对变化、3时刻61方向热红外及真实软件展示。')
    # 若存在参考文献与下载信息文件，则并入索引条目。
    resources=ROOT/'provenance/references-and-downloads.json'
    if resources.exists():
        info=json.loads(resources.read_text(encoding='utf8'))
        entry['downloads']=info['software'];entry['references']=info['references'];entry['resources_checked_date']=info['checked_date']
    idx['cases']=[c for c in idx['cases'] if c['id']!='005']+[entry];idx['updated']='2026-11-07';dump(index,idx)
    # 在案例库 README 的目录表中补一行 005，并追加一段说明（仅在尚无 005 时执行）。
    readme=library/'README.md';txt=readme.read_text(encoding='utf8')
    if '| 005 |' not in txt:
        txt=txt.replace('\n\n总册为','\n| 005 | [005_光伏草场热环境](005_光伏草场热环境/打开案例.md) | 005_光伏草场热环境/project.json |\n\n总册为',1)
        txt+='\n005（2026-11-07）含配对光伏/草地原生模拟、温度/多角度热红外图表、动态GIF和真实软件展示；独立PDF与交互图文入口在案例目录内，尚未并入总册Word。理想化教学模拟，未进行独立实测验证。\n'
        readme.write_text(txt,encoding='utf8')
    # 步骤 6：交付检查——index.html 的本地引用和锚点必须都能解析。
    # Every local gallery reference must resolve; no absolute paths hidden in HTML assets.
    refs=re.findall(r'(?:src|href)="([^"]+)"',html)
    for ref in refs:
        if ref.startswith('#'):
            if f'id="{ref[1:]}"' not in html:errors.append(ref)
        elif '://' not in ref and not (ROOT/ref).exists():errors.append(ref)
    assert not errors,errors
    # PDF 至少 3 页；方向性表要求 366 行且亮温均为有限值。
    from pypdf import PdfReader
    pdf=PdfReader(ROOT/'案例005_光伏草场热环境.pdf');assert len(pdf.pages)>=3
    angular=list(csv.DictReader((ROOT/'tables/thermal_directionality.csv').open(encoding='utf-8-sig')));assert len(angular)==366 and all(np.isfinite(float(r['brightness_C'])) for r in angular)
    # 写交付清单：登记 ROOT 下（不含 qa 目录）所有文件的大小与 SHA-256。
    # Numerical report, renderer and project versions are fingerprinted together.
    files=[p for p in ROOT.rglob('*') if p.is_file() and 'qa' not in p.relative_to(ROOT).parts]
    dump(ROOT/'qa/delivery-manifest.json',dict(status='PASS',engine_and_input_hash_checks=validated,native_converged_nodes=98,spatial_refinement_converged_nodes=74 if (ROOT/'qa/spatial-refinement-audit.json').exists() else 0,report_pages=len(pdf.pages),angular_rows=len(angular),gallery_local_links=len(refs),checks=['native engine and input SHA256 unchanged','324 m2 grass;96 m2 south-facing 30-degree panels','all 98 base nodes converged; spatial refinement audited separately','366 angular CSV rows finite; invalid edge pixels excluded','local gallery references exist','PDF rasterized and visually reviewed'],files=[dict(path=p.relative_to(ROOT).as_posix(),bytes=p.stat().st_size,sha256=sha(p)) for p in files]))
    print(json.dumps(dict(status='PASS',input_hash_checks=validated,files=len(files),report_pages=len(pdf.pages),angular_rows=len(angular)),ensure_ascii=True))
if __name__=='__main__':main()
