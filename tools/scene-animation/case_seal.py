"""Portable case seals for the scene-animation native pipelines.

A seal is a SHA-256 manifest whose keys are paths relative to the case directory,
so a sealed case can be copied to another drive or machine and still verified.
The case seal covers inputs, configuration, receipts and the GPU geometry
products used by the renderer. Large per-node products (`native/*/output/.facet_steps`,
`process`, `diagnostics`) and rendered media are excluded; render products have
their own seal (`render_seal.json`).

The module is deliberately dependency-free (standard library only) so it can run
in the same minimal Python as `run_case.py features`.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

CASE_SEAL = 'case_seal.json'
RENDER_SEAL = 'render_seal.json'
FORMAT = 1

# Directories whose contents are either huge per-node products or rendered media.
EXCLUDED_DIR_NAMES = {
    '.facet_steps', 'process', 'diagnostics',
    'video_frames', 'stills', 'radiance', 'display', 'rgb',
}
# Native geometry product written next to the small `faceteb_run.json` index.
EXCLUDED_FILE_NAMES = {'faceteb.json'}
# Render/deck products are not case inputs; the render seal covers them.
EXCLUDED_CASE_PRODUCTS = {
    'render_receipt.json', 'render_seal.json', 'day_history.csv', 'day_extremes.csv',
    'closure.json', 'energy_summary.json', 'render_runway_day.log',
}
# Video/deck products are not case inputs; the render seal covers videos.
EXCLUDED_SUFFIXES = {'.mp4', '.pptx'}

RENDER_FILES = {
    'render_receipt.json', 'day_history.csv', 'day_extremes.csv',
    'closure.json', 'energy_summary.json',
}
RENDER_DIRS = ('stills', 'video_frames', 'rgb')


class CaseSealError(RuntimeError):
    """A seal is missing, malformed, or does not match the case contents."""


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1 << 22), b''):
            h.update(block)
    return h.hexdigest()


def _write_json(path: Path, value) -> None:
    path = Path(path)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    os.replace(temporary, path)


def _relative(case: Path, path: Path) -> str:
    return path.relative_to(case).as_posix()


def _iter_case_files(case: Path, seal_name: str):
    for path in sorted(case.rglob('*')):
        if not path.is_file():
            continue
        relative = _relative(case, path)
        if relative == seal_name:
            continue
        parts = Path(relative).parts
        if any(part in EXCLUDED_DIR_NAMES for part in parts):
            continue
        if path.name in EXCLUDED_FILE_NAMES or path.name in EXCLUDED_CASE_PRODUCTS or path.suffix.lower() in EXCLUDED_SUFFIXES:
            continue
        # `.track` caches are derived physics, not inputs; they may be huge.
        if path.suffix == '.track':
            continue
        yield relative, path


def _iter_render_files(case: Path, seal_name: str):
    for path in sorted(case.rglob('*')):
        if not path.is_file():
            continue
        relative = _relative(case, path)
        if relative == seal_name:
            continue
        top = relative.split('/', 1)[0]
        if relative in RENDER_FILES or top in RENDER_DIRS or path.suffix.lower() == '.mp4':
            yield relative, path


def iter_files(case, kind: str = 'case', name: str | None = None):
    case = Path(case).resolve()
    if kind == 'case':
        return _iter_case_files(case, name or CASE_SEAL)
    if kind == 'render':
        return _iter_render_files(case, name or RENDER_SEAL)
    raise ValueError('kind must be case or render: ' + str(kind))


def write_seal(case, name: str | None = None, kind: str = 'case', files=None):
    """Write a relative-path SHA-256 seal and return its payload."""
    case = Path(case).resolve()
    kind = str(kind)
    if kind not in ('case', 'render'):
        raise ValueError('kind must be case or render')
    name = name or (CASE_SEAL if kind == 'case' else RENDER_SEAL)
    entries = {}
    for relative, path in (files if files is not None else iter_files(case, kind, name)):
        entries[relative] = {'sha256': digest(path), 'bytes': path.stat().st_size}
    payload = {
        'format': FORMAT,
        'kind': kind,
        'createdUtc': datetime.now(timezone.utc).isoformat(),
        'rootName': case.name,
        'scope': ('inputs, configuration, receipts and GPU geometry products; '
                  'large per-node arrays and rendered media excluded' if kind == 'case'
                  else 'rendered products and render receipts; radiance data excluded'),
        'files': entries,
    }
    _write_json(case / name, payload)
    return payload


def _resolve(case: Path, key: str) -> Path:
    path = Path(key)
    return path if path.is_absolute() else case / path


def verify_seal(case, name: str | None = None, strict_extras: bool = False):
    """Verify a seal and return a summary; raise CaseSealError on mismatch."""
    case = Path(case).resolve()
    name = name or CASE_SEAL
    path = case / name
    if not path.is_file():
        raise CaseSealError('seal not found: ' + str(path))
    try:
        payload = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError) as exc:
        raise CaseSealError('seal is not readable JSON: ' + str(exc)) from exc
    if payload.get('format') != FORMAT:
        raise CaseSealError('unsupported seal format: ' + repr(payload.get('format')))
    kind = payload.get('kind', 'case')
    entries = payload.get('files')
    if not isinstance(entries, dict) or not entries:
        raise CaseSealError('seal has no files')
    missing, changed = [], []
    total = 0
    for relative, meta in entries.items():
        if not isinstance(meta, dict) or 'sha256' not in meta:
            raise CaseSealError('seal entry is malformed: ' + str(relative))
        file_path = _resolve(case, relative)
        if not file_path.is_file():
            missing.append(relative)
            continue
        expected = meta['sha256']
        total += file_path.stat().st_size
        if meta.get('bytes') is not None and file_path.stat().st_size != meta['bytes']:
            changed.append(relative)
            continue
        if digest(file_path) != expected:
            changed.append(relative)
    current = {relative for relative, _ in iter_files(case, kind, name)}
    extras = sorted(current - set(entries))
    if missing or changed:
        details = []
        if missing:
            details.append('missing=' + ', '.join(missing[:5]))
        if changed:
            details.append('changed=' + ', '.join(changed[:5]))
        raise CaseSealError('case seal mismatch (' + name + '): ' + '; '.join(details))
    if strict_extras and extras:
        raise CaseSealError('case seal found unsealed files: ' + ', '.join(extras[:5]))
    return {'files': len(entries), 'bytes': total, 'kind': kind,
            'extras': extras, 'missing': missing, 'changed': changed}


def main():
    parser = argparse.ArgumentParser(description='Write or verify a portable case seal')
    parser.add_argument('case', type=Path)
    parser.add_argument('--name', default=None)
    parser.add_argument('--kind', choices=('case', 'render'), default='case')
    parser.add_argument('--verify', action='store_true', help='verify an existing seal instead of writing')
    parser.add_argument('--strict-extras', action='store_true', help='also reject unsealed files')
    parser.add_argument('--json', action='store_true', help='print the full seal payload when writing')
    args = parser.parse_args()
    try:
        seal_name = args.name or (CASE_SEAL if args.kind == 'case' else RENDER_SEAL)
        if args.verify:
            summary = verify_seal(args.case, seal_name, args.strict_extras)
            print(json.dumps(summary, ensure_ascii=False, indent=2))
            return 0
        payload = write_seal(args.case, args.name, args.kind)
        if args.json:
            print(json.dumps(payload, ensure_ascii=False, indent=2))
        else:
            print(json.dumps({'status': 'sealed', 'name': seal_name,
                              'kind': args.kind, 'files': len(payload['files'])}, ensure_ascii=False))
        return 0
    except (OSError, ValueError, CaseSealError) as exc:
        print(f'{type(exc).__name__}: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
