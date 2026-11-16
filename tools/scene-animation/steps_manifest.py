"""Per-node array manifests for native FacetEB runs (`native/<run>/output/steps_manifest.json`).

The case seal excludes the per-node products (`.facet_steps/*.bin`, `diagnostics/*.json`;
about 25 MB per node and run) to stay small. This manifest records their SHA-256 and size
plus a Merkle-style root, and lives in `output/`, which the case seal *does* cover, so a
sealed case transitively protects every node array. The renderer checks each array and
diagnostic against it when first read (or all of them up front with --verify-steps full).

Standard library only.

usage:
  python steps_manifest.py CASE            write manifests for every completed native run
  python steps_manifest.py CASE --verify   verify every array against existing manifests
After writing manifests into an already sealed case, re-seal it: python case_seal.py CASE
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

from case_seal import CASE_SEAL, digest

STEPS_MANIFEST = 'steps_manifest.json'
FORMAT = 1


class StepsManifestError(RuntimeError):
    """A per-node array is missing, unlisted or differs from its manifest."""


def collect(output: Path):
    output = Path(output)
    files = sorted(list((output / '.facet_steps').glob('*.bin')) + list((output / 'diagnostics').glob('*.json')))
    return [(p.relative_to(output).as_posix(), p) for p in files]


def merkle_root(entries: dict) -> str:
    """SHA-256 over the sorted `path:sha256:bytes` lines (order independent of the filesystem)."""
    h = hashlib.sha256()
    for relative in sorted(entries):
        meta = entries[relative]
        h.update(f'{relative}:{meta["sha256"]}:{meta["bytes"]}\n'.encode('utf-8'))
    return h.hexdigest()


def write_manifest(output) -> dict:
    output = Path(output).resolve()
    entries = {relative: {'sha256': digest(path), 'bytes': path.stat().st_size} for relative, path in collect(output)}
    if not entries:
        raise StepsManifestError('no per-node arrays found under ' + str(output))
    payload = dict(format=FORMAT, createdUtc=datetime.now(timezone.utc).isoformat(),
                   scope='native per-node arrays (.facet_steps/*.bin) and diagnostics (diagnostics/*.json); covered transitively by case_seal.json',
                   files=entries, fileCount=len(entries), totalBytes=sum(m['bytes'] for m in entries.values()), merkleRoot=merkle_root(entries))
    temporary = output / (STEPS_MANIFEST + '.tmp')
    temporary.write_text(json.dumps(payload, indent=1) + '\n', encoding='utf-8')
    temporary.replace(output / STEPS_MANIFEST)
    return payload


def load_manifest(output):
    path = Path(output) / STEPS_MANIFEST
    if not path.is_file():
        return None
    try:
        payload = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError) as exc:
        raise StepsManifestError(f'{path} is not readable JSON: {exc}') from exc
    entries = payload.get('files')
    if payload.get('format') != FORMAT or not isinstance(entries, dict) or not entries:
        raise StepsManifestError(f'{path} is malformed')
    if merkle_root(entries) != payload.get('merkleRoot'):
        raise StepsManifestError(f'{path}: merkleRoot does not match its entries')
    return payload


def verify_output(output) -> dict:
    output = Path(output).resolve()
    payload = load_manifest(output)
    if payload is None:
        raise StepsManifestError('no ' + STEPS_MANIFEST + ' in ' + str(output))
    present = {relative for relative, _ in collect(output)}
    missing = sorted(set(payload['files']) - present)
    if missing:
        raise StepsManifestError(f'{output}: {len(missing)} listed array(s) missing, e.g. {missing[0]}')
    for relative, meta in payload['files'].items():
        path = output / relative
        if path.stat().st_size != meta['bytes'] or digest(path) != meta['sha256']:
            raise StepsManifestError(f'{output}: {relative} differs from {STEPS_MANIFEST}')
    return dict(files=len(payload['files']), merkleRoot=payload['merkleRoot'], unlisted=sorted(present - set(payload['files'])))


class StepsVerifier:
    """Checks native per-node files against their run manifests (mode: lazy | full | off)."""

    def __init__(self, case_root, runs, mode='lazy'):
        if mode not in ('lazy', 'full', 'off'):
            raise ValueError('verify-steps mode must be lazy, full or off')
        self.root, self.runs, self.mode = Path(case_root).resolve(), list(runs), mode
        self._manifests, self._verified, self.legacy = {}, set(), []

    def _output(self, run):
        return self.root / 'native' / run / 'output'

    def _manifest(self, run):
        if run not in self._manifests:
            payload = load_manifest(self._output(run))
            self._manifests[run] = payload
            if payload is None and self.mode != 'off':
                self.legacy.append(run)
                print(f'WARNING: native/{run}/output has no {STEPS_MANIFEST}; its per-node arrays are not hash-checked '
                      f'(legacy case: run "python steps_manifest.py CASE" and re-seal)', flush=True)
        return self._manifests[run]

    def prepare(self, sealed: bool):
        """Called after the case seal verified: manifests must be sealed; full mode checks every file now."""
        if self.mode == 'off':
            return
        sealed_keys = set()
        if sealed:
            sealed_keys = set(json.loads((self.root / CASE_SEAL).read_text(encoding='utf-8')).get('files', {}))
        for run in self.runs:
            if self._manifest(run) is None:
                continue
            key = f'native/{run}/output/{STEPS_MANIFEST}'
            if sealed and key not in sealed_keys:
                raise StepsManifestError(f'{key} exists but is not covered by {CASE_SEAL}; re-seal the case')
            if self.mode == 'full':
                verify_output(self._output(run))
                self._verified.update((run, relative) for relative in self._manifests[run]['files'])

    def check(self, run, path):
        if self.mode == 'off':
            return
        payload = self._manifest(run)
        if payload is None:
            return
        relative = Path(path).resolve().relative_to(self._output(run)).as_posix()
        if (run, relative) in self._verified:
            return
        meta = payload['files'].get(relative)
        if meta is None:
            raise StepsManifestError(f'native/{run}/output/{relative} is not listed in {STEPS_MANIFEST}')
        path = Path(path)
        if not path.is_file() or path.stat().st_size != meta['bytes'] or digest(path) != meta['sha256']:
            raise StepsManifestError(f'native/{run}/output/{relative} differs from {STEPS_MANIFEST} (edited or damaged per-node array)')
        self._verified.add((run, relative))

    def summary(self):
        return dict(mode=self.mode, verifiedFiles=len(self._verified),
                    merkleRoots={run: (m or {}).get('merkleRoot') for run, m in self._manifests.items()},
                    legacyRunsWithoutManifest=list(self.legacy))


def main():
    p = argparse.ArgumentParser(description='Write or verify native per-node array manifests')
    p.add_argument('case', type=Path)
    p.add_argument('--verify', action='store_true')
    a = p.parse_args()
    outputs = sorted(o for o in (a.case.resolve() / 'native').glob('*/output') if (o / 'faceteb_run.json').is_file())
    if not outputs:
        print('no completed native runs under ' + str(a.case), file=sys.stderr)
        return 1
    try:
        for output in outputs:
            result = verify_output(output) if a.verify else write_manifest(output)
            print(json.dumps({'output': output.relative_to(a.case.resolve()).as_posix(), 'files': result.get('files', result.get('fileCount')),
                              'merkleRoot': result['merkleRoot']}, ensure_ascii=False))
        if not a.verify and (a.case / CASE_SEAL).is_file():
            print('note: the case was already sealed; re-seal it so the manifests are covered: python case_seal.py ' + str(a.case))
        return 0
    except (OSError, StepsManifestError) as exc:
        print(f'{type(exc).__name__}: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
