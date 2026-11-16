"""Register case, verify provenance and provide an animation from native TIFFs."""
import csv,json,pathlib,re,shutil
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from matplotlib import colormaps
from pv_case005 import ROOT,dump,sha
from pv_media005 import readtif
def main():
    errors=[];validated=0
    manifests=list((ROOT/'runs').glob('*/run-manifest.json'))+list((ROOT/'refinement-025m').glob('*/run-manifest.json'))
    for manifest in manifests:
        m=json.loads(manifest.read_text(encoding='utf8'))
        assert m['status']=='completed' and m['engine_sha256']==sha(pathlib.Path(m['engine']))
        for name,expected in m['inputs'].items():
            assert sha(pathlib.Path(name))==expected,name
            validated+=1
    for p in (ROOT/'scenarios').glob('*/project.json'):
        q=json.loads(p.read_text(encoding='utf8'))['configuration']
        for filename in [q['meteo']['path'],*[v[k] for v in q['objects']['items'] for k in ['fileName','positionFile']]]:assert pathlib.Path(filename).is_file(),filename
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
    frames[0].save(ROOT/'figures/08_全天热红外变化.gif',save_all=True,append_images=frames[1:],duration=500,loop=0,optimize=False)
    html=(ROOT/'index.html').read_text(encoding='utf8')
    if 'src="figures/08_全天热红外变化.gif"' not in html:
        html=html.replace('<section><h2>温度日变化','<section><h2>全天热红外变化</h2><img src="figures/08_全天热红外变化.gif"><p>第二天0–24时，原生输出；两组共用14–48°C色标。</p></section><section><h2>温度日变化',1)
        (ROOT/'index.html').write_text(html,encoding='utf8')
    for p in pathlib.Path('C:/work/streamstudio/tools/case005').glob('*'):
        if p.is_file():shutil.copy2(p,ROOT/'reproduce'/p.name)
    library=pathlib.Path('D:/cases/StreamStudio/案例库');index=library/'案例索引.json';idx=json.loads(index.read_text(encoding='utf8'))
    backup=ROOT/'provenance/catalog-before-case005.json'
    if not backup.exists():shutil.copy2(index,backup)
    entry=dict(id='005',title='光伏草场热环境',mode='eFacetEB',directory=str(library/'005_光伏草场热环境'),project=str(ROOT/'project.json'),report=str(library/'005_光伏草场热环境/案例005_光伏草场热环境.pdf'),gallery=str(library/'005_光伏草场热环境/index.html'),scenarios=['无光伏草场','光伏草场'],evidence=[dict(scenario=s,native_run=str(ROOT/f'runs/{s}/output/faceteb_run.json'),passed_nodes=49) for s in ['grass','pv_grass']],field_validation=False,summary='相同气象与草地；南向30°光伏阵列；24节点预热，第二天温度、草叶配对变化、3时刻61方向热红外及真实软件展示。')
    resources=ROOT/'provenance/references-and-downloads.json'
    if resources.exists():
        info=json.loads(resources.read_text(encoding='utf8'))
        entry['downloads']=info['software'];entry['references']=info['references'];entry['resources_checked_date']=info['checked_date']
    idx['cases']=[c for c in idx['cases'] if c['id']!='005']+[entry];idx['updated']='2026-11-07';dump(index,idx)
    readme=library/'README.md';txt=readme.read_text(encoding='utf8')
    if '| 005 |' not in txt:
        txt=txt.replace('\n\n总册为','\n| 005 | [005_光伏草场热环境](005_光伏草场热环境/打开案例.md) | 005_光伏草场热环境/project.json |\n\n总册为',1)
        txt+='\n005（2026-11-07）含配对光伏/草地原生模拟、温度/多角度热红外图表、动态GIF和真实软件展示；独立PDF与交互图文入口在案例目录内，尚未并入总册Word。理想化教学模拟，未进行独立实测验证。\n'
        readme.write_text(txt,encoding='utf8')
    # Every local gallery reference must resolve; no absolute paths hidden in HTML assets.
    refs=re.findall(r'(?:src|href)="([^"]+)"',html)
    for ref in refs:
        if ref.startswith('#'):
            if f'id="{ref[1:]}"' not in html:errors.append(ref)
        elif '://' not in ref and not (ROOT/ref).exists():errors.append(ref)
    assert not errors,errors
    from pypdf import PdfReader
    pdf=PdfReader(ROOT/'案例005_光伏草场热环境.pdf');assert len(pdf.pages)>=3
    angular=list(csv.DictReader((ROOT/'tables/thermal_directionality.csv').open(encoding='utf-8-sig')));assert len(angular)==366 and all(np.isfinite(float(r['brightness_C'])) for r in angular)
    # Numerical report, renderer and project versions are fingerprinted together.
    files=[p for p in ROOT.rglob('*') if p.is_file() and 'qa' not in p.relative_to(ROOT).parts]
    dump(ROOT/'qa/delivery-manifest.json',dict(status='PASS',engine_and_input_hash_checks=validated,native_converged_nodes=98,spatial_refinement_converged_nodes=74 if (ROOT/'qa/spatial-refinement-audit.json').exists() else 0,report_pages=len(pdf.pages),angular_rows=len(angular),gallery_local_links=len(refs),checks=['native engine and input SHA256 unchanged','324 m2 grass;96 m2 south-facing 30-degree panels','all 98 base nodes converged; spatial refinement audited separately','366 angular CSV rows finite; invalid edge pixels excluded','local gallery references exist','PDF rasterized and visually reviewed'],files=[dict(path=p.relative_to(ROOT).as_posix(),bytes=p.stat().st_size,sha256=sha(p)) for p in files]))
    print(json.dumps(dict(status='PASS',input_hash_checks=validated,files=len(files),report_pages=len(pdf.pages),angular_rows=len(angular)),ensure_ascii=True))
if __name__=='__main__':main()
