"""Assemble a Stream Studio user delivery directory D:/releases/streamstudio/<version> (not yet used; for a future release).

Copies the new installer, the two manual PDFs and LICENSE, carries the examples of the previous delivery
over unchanged, adds new examples, and writes 案例目录.json, 交付清单.json (all files except itself),
SHA256SUMS.txt (all files) and 开始使用.txt. Never overwrites an existing delivery directory.

usage: prepare_user_release.py --version X.Y.Z --previous D:/releases/streamstudio/2.0.1
           --installer release/Stream-Studio-Setup-X.Y.Z-x64-slim.exe --out D:/releases/streamstudio/X.Y.Z
           --start-text FILE [--add-example NAME=DIR=CASE_JSON ...]
CASE_JSON is a JSON file with the 案例目录 entry of the added example (name/project are filled in).
"""
import argparse, datetime, hashlib, json, shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--version', required=True); p.add_argument('--previous', type=Path, required=True)
    p.add_argument('--installer', type=Path, required=True); p.add_argument('--out', type=Path, required=True)
    p.add_argument('--add-example', action='append', default=[]); p.add_argument('--start-text', type=Path, required=True)
    a = p.parse_args()
    if a.out.exists():
        raise SystemExit(f'delivery directory already exists: {a.out}')
    if not a.installer.is_file() or a.version not in a.installer.name:
        raise SystemExit(f'installer missing or not for {a.version}: {a.installer}')
    previous = json.loads((a.previous / '案例目录.json').read_text(encoding='utf-8'))
    a.out.mkdir(parents=True)
    shutil.copy2(a.installer, a.out / a.installer.name)
    shutil.copy2(ROOT / 'LICENSE.txt' if (ROOT / 'LICENSE.txt').exists() else a.previous / 'LICENSE.txt', a.out / 'LICENSE.txt')
    shutil.copy2(ROOT / 'docs' / 'Stream_Studio_User_Manual_CN.pdf', a.out / f'StreamStudio-{a.version}-用户使用手册.pdf')
    shutil.copy2(ROOT / 'docs' / 'Stream_Studio_Theory_Manual_CN.pdf', a.out / f'StreamStudio-{a.version}-模型理论与方法.pdf')
    # Previous examples unchanged (built-in examples and user data are kept).
    shutil.copytree(a.previous / 'examples', a.out / 'examples')
    cases = previous['cases']
    for item in a.add_example:
        name, folder, entry = item.split('=', 2)
        target = a.out / 'examples' / name
        if target.exists():
            raise SystemExit(f'example name already used: {name}')
        shutil.copytree(folder, target, ignore=shutil.ignore_patterns('__pycache__', 'output', '*.log'))
        record = json.loads(Path(entry).read_text(encoding='utf-8')); record['name'] = name
        record['project'] = f"examples/{name}/{record.pop('input')}"
        record['projectSha256'] = sha(a.out / record['project'])
        cases.append(record)
    shutil.copy2(a.start_text, a.out / '开始使用.txt')
    now = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='milliseconds').replace('+00:00', 'Z')
    catalog = dict(version=a.version, checkedAt=now, requiredRunLocation=previous.get('requiredRunLocation'),
                   scope=previous.get('scope', '') + ' Scene-animation example: run check with the packaged engine, see its acceptance field.',
                   carriedOverFrom=str(a.previous).replace('\\', '/'), cases=cases)
    (a.out / '案例目录.json').write_text(json.dumps(catalog, ensure_ascii=False, indent=2), encoding='utf-8')
    files = sorted(f for f in a.out.rglob('*') if f.is_file())
    listing = [dict(path=f.relative_to(a.out).as_posix(), bytes=f.stat().st_size, sha256=sha(f)) for f in files]
    manifest = dict(kind='streamstudio-user-delivery', version=a.version, preparedAt=now, installerUnchanged=False, newSoftwareRelease=True,
                    caseCount=len(cases), caseCheck=catalog['scope'], files=listing)
    (a.out / '交付清单.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    files = sorted(f for f in a.out.rglob('*') if f.is_file() and f.name != 'SHA256SUMS.txt')
    (a.out / 'SHA256SUMS.txt').write_text(''.join(f'{sha(f)}  {f.relative_to(a.out).as_posix()}\n' for f in files), encoding='utf-8')
    print(f'{a.out}: {len(files) + 1} files, {len(cases)} cases')


if __name__ == '__main__':
    main()
