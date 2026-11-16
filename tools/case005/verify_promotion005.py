"""Bounded checks for the promotional-material commit, without simulations."""
import hashlib,json,pathlib,re,subprocess
from pypdf import PdfReader
REPO=pathlib.Path('C:/work/streamstudio');DEST=REPO/'docs/cases/005-photovoltaic'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
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
    credential=re.compile(r'gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{35,}|sk-[A-Za-z0-9]{32,}|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----')
    for p in DEST.rglob('*'):
        if p.is_file():
            assert p.stat().st_size<100_000_000,p.name
            if p.suffix in ['.md','.html','.css','.js','.json','.csv']:assert not credential.search(p.read_text(encoding='utf8')),p.name
    tracked=subprocess.check_output(['git','diff','--cached','--name-only','-z'],cwd=REPO).decode('utf8').rstrip('\0').split('\0')
    allowed={'README.md','docs/cases/README.md','docs/cases/catalog.json','docs/cases/manifest.json'}
    assert all(p in allowed or p.startswith('docs/cases/005-photovoltaic/') for p in tracked),tracked
    changed=[REPO/'docs/cases/README.md',REPO/'docs/cases/catalog.json',*[p for p in DEST.rglob('*') if p.is_file()]]
    for p in changed:
        row=m['files'][p.relative_to(REPO).as_posix()];assert sha(p)==row['sha256'],p
    print(json.dumps(dict(status='PASS',promotional_files=len(data['published_files'])+1,staged_files=len(tracked),report_pages=3,credential_scan='PASS',project_links_removed=True,scope='promotional_materials_only'),ensure_ascii=True))
if __name__=='__main__':main()
