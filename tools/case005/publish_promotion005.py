# ==========================================================================
# 文件：publish_promotion005.py
# 功能：案例005（光伏草场）第一版“仅宣传材料”发布脚本：把 D:/cases/StreamStudio/case005 中的图件、
#       PDF、两张 CSV 表和网页，复制/改写到仓库 docs/cases/005-photovoltaic，并登记到案例目录与各清单。
#       不发布工程输入、原生状态和模型源码，也不重跑模型。
# 主要步骤（main）：
#   1. 目标目录必须不存在（防止覆盖），逐文件复制并校验 SHA-256，记录映射；
#   2. 改写 index.html 里指向工程文件的链接与技术说明，生成发布版网页；
#   3. 由 references-and-downloads.json 与图件清单拼出 README.md（正文数值为手写常量）；
#   4. 在 docs/cases/catalog.json、docs/cases/README.md、仓库根 README.md 中登记案例 005；
#   5. 校验网页内所有相对链接，写 promotion-manifest.json，刷新 docs/cases/manifest.json。
# 与其他文件的关系：之后由 compact_promotion005.py 把本脚本发布的 8 图/11 页版本压缩为 3 张推广图（CARD_COUNT=3，
#       report.pdf 也随之变为 3 页），并在 publish() 中重写同一批清单与 README（它自己的 copywriting() 另生成 README.md，
#       catalog 里的“7图 + 动态图”被替换为“3张融合推广图”）。因此本脚本写入的 README 里“11页 / 7图”等描述只是第一版的
#       状态，最终发布状态以 compact_promotion005.py 的输出为准；本脚本的字符串不再改动，以免与已提交的第一版文件不一致。
# 注意：98、74 个收敛节点，−2.37 K、−2.32 K、5.65 K 等数值为手写常量，不从结果文件读取。
# ==========================================================================

"""Prepare only Case005 promotional documents; no model/code publication."""
import hashlib,json,pathlib,re,shutil,html
SOURCE=pathlib.Path('D:/cases/StreamStudio/case005')
REPO=pathlib.Path('C:/work/streamstudio')
DEST=REPO/'docs/cases/005-photovoltaic'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def dump(p,v):p.write_text(json.dumps(v,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
# 作用：一次性生成并登记案例 005 的宣传材料发布目录。
# 副作用：新建 DEST，改写 docs/cases 下的 catalog.json、README.md、manifest.json 与仓库根 README.md。
def main():
    # 步骤 1：目标目录已存在则中止，避免覆盖已发布的材料。
    assert not DEST.exists(),'Inspect existing destination before overwriting'
    DEST.mkdir(parents=True)
    mapping=[]
    # 复制并校验：源与目标的 SHA-256 必须一致，并把映射记入 mapping（写进 promotion-manifest.json）。
    def cp(src,dst):
        a=SOURCE/src;b=DEST/dst;b.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(a,b)
        assert sha(a)==sha(b);mapping.append(dict(source=src,published=dst,sha256=sha(a),bytes=b.stat().st_size))
    # 复制 figures/（PNG、GIF、PDF）与 assets/ 下全部文件，以及逐时/方向性两张表和 summary.json。
    for p in (SOURCE/'figures').iterdir():
        if p.is_file() and p.suffix in ['.png','.gif','.pdf']:cp(p.relative_to(SOURCE).as_posix(),p.relative_to(SOURCE).as_posix())
    for p in (SOURCE/'assets').rglob('*'):
        if p.is_file():cp(p.relative_to(SOURCE).as_posix(),p.relative_to(SOURCE).as_posix())
    for name in ['temperature_diurnal.csv','thermal_directionality.csv','summary.json']:
        cp('tables/'+name,'tables/'+name)
    # PDF 与参考文献文件改用仓库内的英文文件名。
    cp('案例005_光伏草场热环境.pdf','report.pdf')
    cp('参考文献与下载.md','references-and-downloads.md')
    # 步骤 2：改写网页——PDF 改名为 report.pdf；去掉指向工程文件的链接（仓库只发布宣传材料）；
    # 用固定文字替换“技术说明”折叠区（其中 49 时次、37 时次等数字为手写，待核对）。
    page=(SOURCE/'index.html').read_text(encoding='utf8').replace('案例005_光伏草场热环境.pdf','report.pdf')
    page=page.replace('href="project.json">工程文件 project.json','href="#software">软件源码与下载')
    page=page.replace('href="scenarios/grass/project.json">无光伏草场工程','href="#compare">查看有、无光伏对照')
    page=re.sub(r'<div class="technical">.*?</div></details>', '<div class="technical"><p>理想化晴夏日、共同气象与初值；基础温度及方向性使用1 m草叶网格，49时次×2组全部收敛。空间图使用0.25 m草叶网格独立补算，37时次×2组全部收敛。</p><p>物理温度与10.5 μm亮温分别解释。草叶温差不代表近地气温或热岛变化。本目录为图文宣传材料，不包含工程输入与完整原生状态。</p><p>方法边界与参考文献见PDF报告和README。</p></div></details>',page,flags=re.S)
    page=page.replace('本案例复现脚本位于本地 reproduce/；软件公开下载包内容以对应发布页或网盘为准。','本目录为案例宣传材料；软件公开下载包内容以对应发布页或网盘为准。')
    (DEST/'index.html').write_text(page,encoding='utf8')
    # 步骤 3：参考文献条目来自 references-and-downloads.json；figures 列出 README 画廊里的 7 张图与标题。
    resources=json.loads((SOURCE/'provenance/references-and-downloads.json').read_text(encoding='utf8'))
    reference_md='\n\n'.join(f'[{r["id"]}] {r["authors"]} ({r["year"]}). {r["title"]}. *{r["journal"]}*. [DOI](https://doi.org/{r["doi"]}).' for r in resources['references'])
    figures=[('01_温度日变化.png','一天中的表面温度变化'),('02_草叶空间温差.png','午间草叶空间温差：0.25 m原生网格'),('03_热辐射方向性.png','不同方向看到的热红外亮温'),('04_热红外影像.png','09、12、15时天底热红外对照'),('05_光伏温度与能量.png','板温、发电与能量分配'),('06_StreamStudio场景.png','StreamStudio中的草地与光伏阵列'),('07_StreamStudio热红外结果.png','软件中的原生热红外结果')]
    gallery='\n\n'.join(f'### {title}\n\n![{title}](figures/{name})' for name,title in figures)
    # README 正文（f 字符串）：场景、关键数字、全天热红外 GIF、图件画廊、参考文献与下载入口。
    # 其中温度、变化量与节点数均为手写常量。
    readme=f'''# 案例005：光伏草场热环境

光伏之下，草场如何受热？板面在升温，板下的草叶却可能更凉。本案例用 StreamStudio 展示草场对照、板温日变化和热辐射方向性。

[完整图文PDF（11页）](report.pdf) · [交互网页文件（下载本目录后打开）](index.html) · [参考文献与下载地址](references-and-downloads.md)

## 场景与主要结果

18 × 18 m 草场，4行南向30°光伏阵列，组件总面积96 m²。两组使用相同气象、草地、土壤与初始条件，前24个节点用于预热。

- 基础网格12时：板平均物理温度 **46.2°C**；无光伏草叶均温 **37.6°C**，有光伏 **35.2°C**，配对变化 **−2.37 K**。
- 草叶空间图采用 **0.25 m原生网格**独立补算：无光伏 **37.60°C**，有光伏 **35.28°C**，变化 **−2.32 K**。每格只平均两片原生三角面元，没有空间插值。
- 09、12、15时各61个观测方向；12时有光伏场景10.5 μm亮温方向极差约 **5.65 K**。亮温与板的物理温度不同。
- 基础计算98节点、空间细化74节点全部通过引擎原生收敛判据。基础日变化与方向性沿用1 m草叶网格，空间图使用0.25 m网格，两者分别标明。

这是理想化数值教学对照，**未完成独立实测验证**。气温由共同气象指定，没有模拟近地气温响应、区域热岛、草地生长或产草量。参考文献提供研究背景，不能当作本案例的独立验证。

## 全天热红外对照

![全天热红外变化](figures/08_全天热红外变化.gif)

第二天0–24时，两组共用14–48°C色标。交互网页可拖动时间轴；GitHub展示本页图件和动态图。

## 图件

{gallery}

## 参考文献

{reference_md}

## 代码与程序下载

- [GitHub源代码](https://github.com/bianzj/streamstudio)
- [GitHub程序发布 v2.0.1](https://github.com/bianzj/streamstudio/releases/tag/v2.0.1)
- [v2.0.1发布版源码ZIP](https://github.com/bianzj/streamstudio/releases/download/v2.0.1/StreamStudio-2.0.1-source.zip)
- [百度网盘](https://pan.baidu.com/s/1iYGVT5aC67xQ9T5A00A_YA?pwd=uvx2)，提取码 **uvx2**。

以上是软件现有公开入口，下载包内容以对应发布页或网盘为准。

## 材料范围

本次仅同步宣传材料：PDF、图文页面、图件、逐时/多角度图表数据、参考文献及软件下载链接。工程输入、完整原生运行状态和模型源码改动不包含在本次提交中。场景草地纹理只影响预览，不改变原生温度结果。本次上传没有重新运行模型，也没有升级程序或修改Release。

[温度图表数据](tables/temperature_diurnal.csv) · [方向性图表数据](tables/thermal_directionality.csv) · [材料哈希清单](promotion-manifest.json)
'''
    (DEST/'README.md').write_text(readme,encoding='utf8')
    # 步骤 4：登记到案例目录 catalog.json（案例 005 必须尚未存在）并更新 docs/cases/README.md 与根 README。
    # The five-case navigation describes the differing publication scopes.
    catalog_path=REPO/'docs/cases/catalog.json';cat=json.loads(catalog_path.read_text(encoding='utf8'))
    assert all(c['id']!='005' for c in cat['cases'])
    cat['cases'].append(dict(id='005',title='光伏草场热环境',page='005-photovoltaic/README.md',figures=8,report='005-photovoltaic/report.pdf',interactive_page='005-photovoltaic/index.html',scope='promotional_materials_only',limitations='理想化草场配对模拟。基础温度与方向性使用1 m网格，空间图独立补算0.25 m网格；没有气温响应或产草量求解，未进行独立实测验证。',model_rerun=False,projects=[]))
    cat['updated']='2026-11-07';dump(catalog_path,cat)
    p=REPO/'docs/cases/README.md';text=p.read_text(encoding='utf8').replace('四个案例的图文报告、原始图件、当前工程与所需输入。','五个研究案例的图文报告与图件。001–004含可迁移工程与输入；005当前仅发布宣传材料。')
    text=text.replace('| 004 | [延庆山地光热与风场](004-yanqing/README.md) | 5 |','| 004 | [延庆山地光热与风场](004-yanqing/README.md) | 5 |\n| 005 | [光伏草场热环境](005-photovoltaic/README.md) | 7图 + 动态图 |')
    p.write_text(text,encoding='utf8')
    p=REPO/'README.md';text=p.read_text(encoding='utf8').replace('新增研究案例：玉米、雨林、城市街区、延庆山地','新增研究案例：玉米、雨林、城市街区、延庆山地、光伏草场').replace('含报告、图件与可迁移工程输入','含图文报告；001–004附工程输入，005为宣传材料')
    p.write_text(text,encoding='utf8')
    # 步骤 5：检查网页中每个相对链接（含页内锚点）都能解析到发布目录内的文件。
    # Verify every relative URL, including navigation anchors, before staging.
    refs=re.findall(r'(?:src|href)="([^"]+)"',page)
    for ref in refs:
        if ref.startswith('#'):assert f'id="{ref[1:]}"' in page,ref
        elif '://' not in ref:assert (DEST/ref).is_file(),ref
    allfiles=[p for p in DEST.rglob('*') if p.is_file()]
    # 单文件须小于 100 MB（GitHub 单文件大小限制）。
    assert all(p.stat().st_size<100_000_000 for p in allfiles)
    # 写本次发布的哈希清单（含逐文件复制映射）。
    dump(DEST/'promotion-manifest.json',dict(date='2026-11-07',scope='promotional_materials_only',model_rerun_during_publication=False,base_native_converged_nodes=98,spatial_refinement_converged_nodes=74,references=len(resources['references']),files_copied_unchanged=mapping,published_files=[dict(path=p.relative_to(DEST).as_posix(),bytes=p.stat().st_size,sha256=sha(p)) for p in allfiles]))
    # 把新增/改动的文件登记进仓库级 manifest.json（大小与 SHA-256）。
    manifest_path=REPO/'docs/cases/manifest.json';m=json.loads(manifest_path.read_text(encoding='utf8'))
    for p in [REPO/'README.md',REPO/'docs/cases/README.md',catalog_path,*[p for p in DEST.rglob('*') if p.is_file()]]:
        m['files'][p.relative_to(REPO).as_posix()]=dict(bytes=p.stat().st_size,sha256=sha(p))
    dump(manifest_path,m)
    print(json.dumps(dict(files=len(allfiles)+1,bytes=sum(p.stat().st_size for p in DEST.rglob('*') if p.is_file()),local_urls_verified=len(refs),destination=str(DEST)),ensure_ascii=True))
if __name__=='__main__':main()
