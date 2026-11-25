# ==========================================================================
# 文件：verify_promotion005.py
# 功能：案例005 宣传材料提交前的有限检查：不跑模拟，只核对发布目录 docs/cases/005-photovoltaic 的
#       哈希、PDF 内容、网页链接、凭据泄露和暂存区范围，任一断言失败即中止。
# 主要步骤（main）：
#   1. 去掉 docs/cases/manifest.json 中的 README.md 条目并写回（注意：该检查会修改清单）；
#   2. 按 promotion-manifest.json 复核每个发布文件的 SHA-256；
#   3. 检查 report.pdf 为 3 页；前两页没有 GitHub/网盘链接，第 3 页恰好各出现一次，且含参考文献 [1]–[5]；
#   4. 检查 index.html 不含工程文件链接、reproduce/、resource 行，并校验所有相对链接与页内锚点；
#   5. 对文本类文件做常见令牌/私钥格式的正则扫描，并要求单文件小于 100 MB；
#   6. 要求 git 暂存区只包含 docs/cases/005-photovoltaic/ 与四个允许的清单/README 文件；
#   7. 校验 manifest.json 中登记的哈希与当前文件一致，打印 PASS 摘要。
# 与其他文件的关系：检查 compact_promotion005.py / publish_promotion005.py 生成的发布物。
# 注意：第 6 步用 git diff --cached，需在已暂存改动后运行。
# ==========================================================================

"""Bounded checks for the promotional-material commit, without simulations."""
import hashlib,json,pathlib,re,subprocess
from pypdf import PdfReader
REPO=pathlib.Path('C:/work/streamstudio');DEST=REPO/'docs/cases/005-photovoltaic'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
# 作用：依次执行上面 1–7 项检查；通过时打印 JSON 摘要（status=PASS）。
def main():
    manifest=REPO/'docs/cases/manifest.json';m=json.loads(manifest.read_text(encoding='utf8'))
    m['files'].pop('README.md',None)
    manifest.write_text(json.dumps(m,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
    data=json.loads((DEST/'promotion-manifest.json').read_text(encoding='utf8'))
    for row in data['published_files']:assert sha(DEST/row['path'])==row['sha256'],row['path']
    pdf=PdfReader(DEST/'report.pdf');assert len(pdf.pages)==3
    text=[p.extract_text() for p in pdf.pages]
    assert all('github.com' not in t and 'pan.baidu.com' not in t for t in text[:2])
    assert text[2].count('github.com/bianzj/streamstudio')==1 and text[2].count('pan.baidu.com')==1
    assert all(f'[{i}]' in text[2] for i in range(1,6))
    page=(DEST/'index.html').read_text(encoding='utf8')
    assert 'href="project.json"' not in page and 'reproduce/' not in page
    assert 'class="resource"' not in page  # No repeated resource rows outside images.
    for ref in re.findall(r'(?:src|href)="([^"]+)"',page):
        if ref.startswith('#'):assert f'id="{ref[1:]}"' in page,ref
        elif '://' not in ref:assert (DEST/ref).exists(),ref
    # 凭据扫描模式：GitHub 令牌（ghp_ 等、github_pat_）、sk- 开头密钥、PEM 私钥头。
    credential=re.compile(r'gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{35,}|sk-[A-Za-z0-9]{32,}|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----')
    for p in DEST.rglob('*'):
        if p.is_file():
            assert p.stat().st_size<100_000_000,p.name
            if p.suffix in ['.md','.html','.css','.js','.json','.csv']:assert not credential.search(p.read_text(encoding='utf8')),p.name
    # 暂存区范围检查：只允许本案例目录与 README/清单这几个文件，防止误提交工程输入或其他改动。
    tracked=subprocess.check_output(['git','diff','--cached','--name-only','-z'],cwd=REPO).decode('utf8').rstrip('\0').split('\0')
    allowed={'README.md','docs/cases/README.md','docs/cases/catalog.json','docs/cases/manifest.json'}
    assert all(p in allowed or p.startswith('docs/cases/005-photovoltaic/') for p in tracked),tracked
    changed=[REPO/'docs/cases/README.md',REPO/'docs/cases/catalog.json',*[p for p in DEST.rglob('*') if p.is_file()]]
    for p in changed:
        row=m['files'][p.relative_to(REPO).as_posix()];assert sha(p)==row['sha256'],p
    print(json.dumps(dict(status='PASS',promotional_files=len(data['published_files'])+1,staged_files=len(tracked),report_pages=3,credential_scan='PASS',project_links_removed=True,scope='promotional_materials_only'),ensure_ascii=True))
if __name__=='__main__':main()
