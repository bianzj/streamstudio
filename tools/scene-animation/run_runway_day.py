"""Run the four matched native FacetEB day experiments of the open-airfield case.

Matched design (checked before anything starts, and again after the runs):
  * payload pair  X_powered / X_unpowered : identical forcing and parameters except every powerW
  * sun pair      sunlit_Y / nosun_Y      : identical parameters except the shortwave column of the forcing and the
                                            declared initial deep-ground temperature (periodic-mean guess)
The four native solves run concurrently as separate processes. Serial equivalence is a separate
trial recorded in the README, not re-established by this script.
"""
import argparse
import copy
import json
import os
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

from coupled_town_prepare import read, write, sha
from run_coupled_town import (native_inputs, runtime_shaders, snapshot_runtime,
                              canonical_inputs, acceptance, verify_source_inputs,
                              SURFACE_RESIDUAL_LIMIT_WM2)
from prepare_runway_day import RUNS
from case_seal import CASE_SEAL, CaseSealError, verify_seal, write_seal
from integrity import IntegrityError, require
from steps_manifest import STEPS_MANIFEST, write_manifest, load_manifest

ENVIRONMENT = dict(HISTREAM_FACET_RASTER_LAYERS='256', HISTREAM_FACET_ENERGY_BACKEND='cpu')
# Measured peak GPU memory of one native airfield solve (README: 4 x ~14.6 GB).
PER_RUN_VRAM_GB = 14.6


def free_vram_gb():
    """Largest free memory (GB) over the NVIDIA GPUs reported by nvidia-smi, or None if unavailable."""
    try:
        r = subprocess.run(['nvidia-smi', '--query-gpu=memory.free', '--format=csv,noheader,nounits'], capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    values = []
    for line in r.stdout.splitlines():
        try:
            values.append(float(line.strip()) / 1024)
        except ValueError:
            continue
    return max(values) if values else None


def plan_parallel(requested, per_run_gb, free_gb):
    """Return (parallel, note): clamp the concurrent native solves to the free GPU memory."""
    if free_gb is None:
        return requested, f'free GPU memory unknown (nvidia-smi unavailable); keeping --parallel {requested}; each run needs ~{per_run_gb:g} GB'
    fit = int(free_gb // per_run_gb)
    if fit < 1:
        raise IntegrityError(f'only {free_gb:.1f} GB GPU memory free; one native airfield run needs ~{per_run_gb:g} GB')
    if fit < requested:
        return fit, f'--parallel {requested} needs ~{requested * per_run_gb:.1f} GB but only {free_gb:.1f} GB is free; running {fit} at a time'
    return requested, f'{free_gb:.1f} GB GPU memory free; --parallel {requested} needs ~{requested * per_run_gb:.1f} GB'


def normalized(project_path, zero_power=True, drop_initial=False):
    c = copy.deepcopy(read(project_path)['configuration'])
    c['meteo']['path'] = '@forcing'
    if drop_initial:
        c['meteo']['Tsold'] = '@initial'
    for item in c['objects']['items']:
        item.update(fileName='@geometry', positionFile='@placement')
    for material in c['materials']:
        h = material.get('params', {}).get('lumpedThermal')
        if h is None:
            continue
        if zero_power:
            h['powerW'] = 0
        if drop_initial and h['group'] in ('runway_road_field', 'airfield_infill', 'scenery_expanded_ground'):
            h['initialK'] = '@initial'
    return c


def forcing(case, run):
    return np.loadtxt(Path(case) / 'native' / run / 'meteorology.txt', skiprows=1)


def validate_design(case):
    """Return the frozen input fingerprints after proving the 2 x 2 design is exactly what it claims to be."""
    case = Path(case)
    frozen, geometry = {}, {}
    for run in RUNS:
        project = case / 'native' / run / 'project.json'
        frozen[run] = native_inputs(project)
        geometry[run] = sorted((Path(p).name, h) for p, h in frozen[run].items() if Path(p).name in ('world.obj', 'origin.txt'))
    require(len({tuple(g) for g in geometry.values()}) == 1, 'All runs must share identical geometry')
    for sun in ('sunlit', 'nosun'):
        on, off = f'{sun}_powered', f'{sun}_unpowered'
        require(normalized(case / 'native' / on / 'project.json') == normalized(case / 'native' / off / 'project.json'), lambda: f'{sun}: payload pair differs beyond power')
        require(np.array_equal(forcing(case, on), forcing(case, off)), lambda: f'{sun}: payload pair must share the forcing exactly')
    for payload in ('powered', 'unpowered'):
        a, b = f'sunlit_{payload}', f'nosun_{payload}'
        require(normalized(case / 'native' / a / 'project.json', drop_initial=True) == normalized(case / 'native' / b / 'project.json', drop_initial=True), lambda: f'{payload}: sun pair differs beyond the switches')
        fa, fb = forcing(case, a), forcing(case, b)
        require(fa.shape == fb.shape and np.array_equal(np.delete(fa, 5, axis=1), np.delete(fb, 5, axis=1)), lambda: f'{payload}: only the shortwave column may differ')
        require(np.all(fb[:, 5] == 0) and fa[:, 5].max() > 800, lambda: f'{payload}: sun-off must have zero shortwave and sunlit a real daytime peak')
    return frozen


def verify_unchanged(case, frozen):
    for run in RUNS:
        if native_inputs(Path(case) / 'native' / run / 'project.json') != frozen[run]:
            raise ValueError('A native input changed since validation: ' + run)


def summarize(root, nodes_expected):
    index = read(root / 'output/faceteb_run.json')
    diagnostics = [read(p) for p in (root / 'output/diagnostics').glob('facet_physics_node=*.json')]
    require(index['status'] == 'completed' and len(index['completed']) == nodes_expected, "check failed: index['status'] == 'completed' and len(index['completed']) == nodes_expected")
    return dict(nodes=len(index['completed']), allNodesConverged=all(n['converged'] for n in index['completed']),
                maxCoreResidualW=max(d['lumpedThermalMaxCoreResidual_W'] for d in diagnostics),
                maxConductiveExchangeMismatchW=max(d['lumpedThermalMaxExchangeMismatch_W'] for d in diagnostics),
                maxCoupledSurfaceResidualWm2=max(d['lumpedThermalMaxSurfaceResidual_W_m2'] for d in diagnostics),
                maxEnergyResidualWm2=max(n['energyResidual'] for n in index['completed']),
                maxIterations=max(n['iterations'] for n in index['completed']))


def run(case, engine, resume=False, parallel=4, per_run_gb=PER_RUN_VRAM_GB):
    case, engine = Path(case).resolve(), Path(engine).resolve()
    if (case / 'SHA256SUMS.json').exists():
        raise FileExistsError('A sealed case is immutable; use a new run')
    if (case / CASE_SEAL).is_file():
        try:
            verify_seal(case)
        except CaseSealError as exc:
            raise ValueError('Existing case seal does not match the case: ' + str(exc)) from exc
        sealed = True
    else:
        sealed = False
        verify_source_inputs(case)
    prepared = read(case / 'preparation.json')
    require(prepared['status'] == 'passed', "check failed: prepared['status'] == 'passed'")
    expected_nodes = prepared['nodes']
    repo = Path(__file__).resolve().parents[2]
    snapshot = snapshot_runtime(case, engine, [Path(__file__), Path(__file__).with_name('prepare_runway_day.py'), Path(__file__).with_name('make_runway_day_scene.py'),
        repo / 'models/histream/src/facetrt/facetrtcore.cpp', repo / 'models/histream/src/facetrt/lumpedthermal.h', repo / 'models/histream/src/facetrt/facetrt_vulkan.cpp',
        repo / 'models/histream/src/facetrt/energyphysics.h', repo / 'models/histream/shader/faceteb/direct_resolve.comp', repo / 'models/histream/shader/faceteb/visibility_resolve.comp'], resume)
    engine_hash, shader_hashes = snapshot['engineSha256'], snapshot['runtimeShaders']
    frozen = validate_design(case)
    pending, results = [], {}
    for run_name in RUNS:
        root = case / 'native' / run_name
        output = root / 'output'
        config_hash = sha(root / 'project.json')
        if output.exists():
            if not resume:
                raise FileExistsError(output)
            old = read(root / 'run_receipt.json')
            require(old['configSha256'] == config_hash and old['engineSha256'] == engine_hash, 'Resume requires the same project and engine')
            require(canonical_inputs(old.get('actualInputHashes', {}), case) == native_inputs(root / 'project.json'), 'Resume requires unchanged dependencies')
            index = read(output / 'faceteb_run.json')
            require(index['status'] == 'completed', f'Resume refused: native index of {run_name} is not completed')
            # Re-derive the convergence summary from the solver's own index/diagnostics instead of
            # trusting the stored receipt numbers, then apply the acceptance rule to the fresh values.
            fresh = summarize(root, expected_nodes)
            stale = [k for k, v in fresh.items() if k in old and old[k] != v]
            if stale:
                raise IntegrityError(f'Resume refused: run_receipt.json of {run_name} disagrees with its diagnostics for ' + ', '.join(stale))
            ok, issues = acceptance(dict(old, status='completed', **fresh), expected_nodes)
            if not ok:
                raise ValueError('Resume refused: previous run does not satisfy the convergence rules (' + '; '.join(issues) + '); start a new run')
            if load_manifest(output) is None:
                write_manifest(output)   # legacy run: record its per-node arrays before the case is (re)sealed
            results[run_name] = dict(old, **fresh)
            print('Resume verified completed native ' + run_name + ' (summary recomputed from diagnostics)', flush=True)
            continue
        pending.append(run_name)
    if runtime_shaders(engine) != shader_hashes:
        raise RuntimeError('Runtime shader changed before native run')
    if pending:
        planned, note = plan_parallel(parallel, per_run_gb, free_vram_gb())
        print('GPU memory: ' + note, flush=True)
        parallel = planned
    else:
        note = None
    active = {}
    queue = list(pending)

    def launch(run_name):
        root = case / 'native' / run_name
        receipt = dict(status='running', configSha256=sha(root / 'project.json'), engineSha256=engine_hash, actualInputHashes=native_inputs(root / 'project.json'),
                       startedUtc=datetime.now(timezone.utc).isoformat(), environment=ENVIRONMENT, scope='Native GPU radiation and CPU coupled core/surface EB')
        write(root / 'run_receipt.json', receipt)
        log = (root / 'native.log').open('wb')
        process = subprocess.Popen([str(engine), 'eFacetEB', str(root / 'project.json'), str(root / 'output/faceteb.json')], stdout=log, stderr=subprocess.STDOUT, env=dict(os.environ, **ENVIRONMENT))
        active[run_name] = (process, log, receipt, time.monotonic())
        print('Start native ' + run_name, flush=True)

    while queue or active:
        while queue and len(active) < parallel:
            launch(queue.pop(0))
        for run_name in list(active):
            process, log, receipt, began = active[run_name]
            code = process.poll()
            if code is None:
                continue
            log.close()
            root = case / 'native' / run_name
            receipt.update(returncode=code, seconds=time.monotonic() - began, finishedUtc=datetime.now(timezone.utc).isoformat())
            receipt['inputsUnchanged'] = native_inputs(root / 'project.json') == receipt['actualInputHashes'] and sha(engine) == engine_hash and runtime_shaders(engine) == shader_hashes
            index_path = root / 'output/faceteb_run.json'
            index = read(index_path) if index_path.exists() else {}
            complete = code == 0 and receipt['inputsUnchanged'] and index.get('status') == 'completed'
            if complete:
                try:
                    receipt.update(summarize(root, expected_nodes))
                except (OSError, KeyError, ValueError, IntegrityError) as exc:
                    complete = False
                    receipt['summaryError'] = f'{type(exc).__name__}: {exc}'
            receipt['status'] = 'completed' if complete else 'failed'
            ok, issues = acceptance(receipt, expected_nodes) if complete else (False, ['native run did not complete'])
            receipt['accepted'] = ok
            receipt['acceptanceIssues'] = issues
            receipt['status'] = 'completed' if complete and ok else 'failed'
            write(root / 'run_receipt.json', receipt)
            print(json.dumps(dict(run=run_name, **receipt), ensure_ascii=False), flush=True)
            del active[run_name]
            if not ok:
                raise RuntimeError('Native run failed the acceptance rules (' + run_name + '): ' + '; '.join(issues) + '; inspect ' + str(root / 'native.log'))
            manifest = write_manifest(root / 'output')
            print(f'{run_name}: {STEPS_MANIFEST} {manifest["fileCount"]} files, root {manifest["merkleRoot"][:16]}', flush=True)
            results[run_name] = receipt
        time.sleep(5)
    if not sealed:
        verify_source_inputs(case)
    verify_unchanged(case, frozen)
    write(case / 'native_runs.json', dict(status='passed', runs=results, frozenInputs=frozen, sourceInputsUnchanged=True,
        acceptanceRule=f'all {expected_nodes} nodes converged and max surface residual < {SURFACE_RESIDUAL_LIMIT_WM2:g} W/m2',
        scientificFieldValidation=False, designChecked='payload pairs differ only in power; sun pairs differ only in shortwave and declared initial deep-ground temperature',
        parallelRuns=parallel, gpuMemoryNote=note,
        stepsManifests={r: load_manifest(case / 'native' / r / 'output')['merkleRoot'] for r in RUNS}))
    seal = write_seal(case)
    print(f'case seal: {len(seal["files"])} files', flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('case', type=Path)
    p.add_argument('engine', type=Path)
    p.add_argument('--resume-completed', action='store_true')
    p.add_argument('--parallel', type=int, default=4, help='concurrent native solves (1..4); lowered automatically to the free GPU memory')
    p.add_argument('--per-run-vram-gb', type=float, default=PER_RUN_VRAM_GB, help='GPU memory of one native solve used for the pre-check')
    a = p.parse_args()
    if not 1 <= a.parallel <= 4:
        p.error('--parallel must be within 1..4')
    if not 0.5 <= a.per_run_vram_gb <= 200:
        p.error('--per-run-vram-gb must be within 0.5..200')
    run(a.case, a.engine, a.resume_completed, a.parallel, a.per_run_vram_gb)


if __name__ == '__main__':
    main()
