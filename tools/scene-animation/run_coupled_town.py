"""Run paired native coupled EB experiments with an immutable input receipt."""
import argparse,json,math,os,subprocess,time
from datetime import datetime,timezone
from pathlib import Path
from coupled_town_prepare import read,write,sha
from integrity import IntegrityError, require

SURFACE_RESIDUAL_LIMIT_WM2 = 2.0


def native_inputs(project, root=None):
    project=Path(project).resolve();config=read(project)['configuration'];paths=[project]
    # Default to the case directory for the canonical case/native/<run>/project.json layout.
    # Relative keys keep receipts portable; files outside the case stay absolute.
    root=(Path(root).resolve() if root else (project.parents[2].resolve() if project.parent.parent.name=='native' else project.parent.resolve()))
    def resolve(value):
        p=Path(value)
        return p.resolve() if p.is_absolute() else (project.parent/p).resolve()
    paths.append(resolve(config['meteo']['path']))
    for item in config['objects']['items']:
        paths.extend([resolve(item['fileName']),resolve(item['positionFile'])])
    def key(path):
        try:return path.relative_to(root).as_posix()
        except ValueError:return str(path)
    return {key(p):sha(p) for p in paths}


def canonical_inputs(mapping, case):
    """Map relative or legacy absolute receipt keys to case-relative keys."""
    case=Path(case).resolve();out={}
    for name,value in mapping.items():
        p=Path(name)
        if not p.is_absolute():p=case/p
        try:
            out[p.resolve().relative_to(case).as_posix()]=value
            continue
        except ValueError:
            pass
        # A moved legacy case kept absolute paths from its old location; recover the
        # native/<run>/... suffix when the same file exists under the new case root.
        parts=p.parts
        if 'native' in parts:
            suffix=Path(*parts[parts.index('native'):])
            if (case/suffix).is_file():out[suffix.as_posix()]=value;continue
        out[str(p.resolve())]=value
    return out


def acceptance(receipt, expected_nodes=None):
    """Return (ok, issues) for the native convergence/acceptance rules."""
    issues=[]
    if receipt.get('status')!='completed':issues.append('status is not completed')
    if expected_nodes is not None and receipt.get('nodes')!=expected_nodes:
        issues.append(f"nodes {receipt.get('nodes')!r} != expected {expected_nodes}")
    if receipt.get('allNodesConverged') is not True:issues.append('not all nodes converged')
    residual=receipt.get('maxCoupledSurfaceResidualWm2')
    if (not isinstance(residual,(int,float)) or isinstance(residual,bool) or
            not math.isfinite(residual) or not residual<SURFACE_RESIDUAL_LIMIT_WM2):
        issues.append(f'surface residual {residual!r} is not < {SURFACE_RESIDUAL_LIMIT_WM2:g} W/m2')
    return (not issues,issues)


def runtime_shaders(engine):
    parent=Path(engine).resolve().parent
    return {str(p.relative_to(parent)):sha(p) for p in (parent/'shader').rglob('*.spv')}


def snapshot_runtime(case,engine,sources,resume):
    case=Path(case);engine=Path(engine).resolve();provenance=case/'runtime'
    hashes={Path(p).name:sha(p) for p in sources};shaders=runtime_shaders(engine);engine_hash=sha(engine)
    if provenance.exists():
        if not resume:raise FileExistsError('Existing runtime snapshot')
        original=read(provenance/'receipt.json')
        if original.get('engineSha256')!=engine_hash or original.get('runtimeShaders')!=shaders or original.get('sourceSnapshots')!=hashes:
            raise ValueError('Resume requires identical runtime, shaders and source snapshots')
        if sha(provenance/'histream.exe')!=engine_hash or any(sha(case/'source'/name)!=h for name,h in hashes.items()) or any(sha(provenance/name)!=h for name,h in shaders.items()):
            raise ValueError('Preserved runtime/source snapshot changed')
        return original
    provenance.mkdir();(case/'source').mkdir(exist_ok=True)
    (provenance/'histream.exe').write_bytes(engine.read_bytes())
    for name,h in shaders.items():
        target=provenance/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes((engine.parent/name).read_bytes())
        if sha(target)!=h:raise ValueError('Shader changed while snapshotting')
    for p in sources:(case/'source'/Path(p).name).write_bytes(Path(p).read_bytes())
    receipt=dict(engine=str(engine),engineSha256=engine_hash,runtimeShaders=shaders,sourceSnapshots=hashes,startedUtc=datetime.now(timezone.utc).isoformat())
    write(provenance/'receipt.json',receipt)
    return receipt


def validate_pair(case):
    import copy
    case=Path(case).resolve();normalized=[];common=[];frozen={}
    for name in ['on','off']:
        project=Path(case)/'native'/name/'project.json';c=copy.deepcopy(read(project)['configuration'])
        fingerprints=native_inputs(project)
        frozen[name]=fingerprints
        project_key=project.relative_to(case).as_posix()
        common.append(sorted((Path(p).name,h) for p,h in fingerprints.items() if p!=project_key))
        c['meteo']['path']='@forcing'
        for item in c['objects']['items']:item.update(fileName='@geometry',positionFile='@placements')
        for material in c['materials']:
            h=material.get('params',{}).get('lumpedThermal')
            if h is not None:h['powerW']=0
        normalized.append(c)
    if normalized[0]!=normalized[1] or common[0]!=common[1]:
        raise ValueError('Paired runs require identical geometry/forcing/parameters except heat power')
    return frozen


def verify_pair_fingerprints(case,frozen):
    for label in ['on','off']:
        if native_inputs(Path(case)/'native'/label/'project.json')!=frozen[label]:
            raise ValueError('Paired input changed since initial validation: '+label)


def verify_source_inputs(case):
    case=Path(case).resolve()
    for name,h in read(case/'source_inputs.json').items():
        p=Path(name);p=p if p.is_absolute() else case/p
        if not p.is_file() or sha(p)!=h:raise ValueError('Protected source changed: '+str(name))


def summarize_output(output,index):
    """Convergence summary derived from the solver index and per-node diagnostics."""
    diagnostics=[read(p) for p in (Path(output)/'diagnostics').glob('facet_physics_node=*.json')]
    return dict(nodes=len(index['completed']),allNodesConverged=all(n['converged'] for n in index['completed']),
        maxCoreResidualW=max(d['lumpedThermalMaxCoreResidual_W'] for d in diagnostics),
        maxConductiveExchangeMismatchW=max(d['lumpedThermalMaxExchangeMismatch_W'] for d in diagnostics),
        maxCoupledSurfaceResidualWm2=max(d['lumpedThermalMaxSurfaceResidual_W_m2'] for d in diagnostics),
        maxEnergyResidualWm2=max(n['energyResidual'] for n in index['completed']))


def run(case,engine,resume=False):
    case=Path(case).resolve();engine=Path(engine).resolve()
    if (case/'SHA256SUMS.json').exists():raise FileExistsError('A sealed case is immutable; use a new run')
    prepared=read(case/'preparation.json');require(prepared['status']=='passed', "check failed: prepared['status']=='passed'");expected_nodes=prepared['nodes']
    verify_source_inputs(case)
    repo=Path(__file__).resolve().parents[2]
    snapshot=snapshot_runtime(case,engine,[Path(__file__),Path(__file__).with_name('coupled_town_prepare.py'),repo/'models/histream/src/facetrt/facetrtcore.cpp',repo/'models/histream/src/facetrt/lumpedthermal.h',
        repo/'models/histream/src/facetrt/facetrt_vulkan.cpp',repo/'models/histream/src/facetrt/energyphysics.h',
        repo/'models/histream/shader/faceteb/direct_resolve.comp',repo/'models/histream/shader/faceteb/visibility_resolve.comp'],resume)
    engine_hash=snapshot['engineSha256'];shader_hashes=snapshot['runtimeShaders']
    frozen_pair=validate_pair(case)
    results={}
    for label in ['on','off']:
        root=case/'native'/label;config=root/'project.json';output=root/'output'
        config_hash=sha(config);actual_inputs=native_inputs(config)
        verify_pair_fingerprints(case,frozen_pair)
        if runtime_shaders(engine)!=shader_hashes:raise RuntimeError('Runtime shader changed before native run')
        if output.exists():
            if not resume:raise FileExistsError(output)
            old=read(root/'run_receipt.json')
            require(old['configSha256']==config_hash and old['engineSha256']==engine_hash, 'Resume requires the same project and engine')
            require(canonical_inputs(old.get('actualInputHashes',{}),case)==actual_inputs, 'Resume requires unchanged actual dependencies')
            index=read(output/'faceteb_run.json');require(index['status']=='completed', f'Resume refused: native index of {label} is not completed')
            # Recompute the convergence summary from the solver's index/diagnostics rather than trusting stored numbers.
            fresh=summarize_output(output,index)
            stale=[k for k,v in fresh.items() if k in old and old[k]!=v]
            if stale:raise IntegrityError(f'Resume refused: run_receipt.json of {label} disagrees with its diagnostics for '+', '.join(stale))
            ok,issues=acceptance(dict(old,status='completed',**fresh),expected_nodes)
            if not ok:raise ValueError('Resume refused: previous run does not satisfy the convergence rules ('+'; '.join(issues)+'); start a new run')
            results[label]=dict(old,**fresh);print('Resume verified completed native '+label+' (summary recomputed from diagnostics)',flush=True);continue
        receipt=dict(status='running',configSha256=config_hash,engineSha256=engine_hash,actualInputHashes=actual_inputs,startedUtc=datetime.now(timezone.utc).isoformat(),
            environment=dict(HISTREAM_FACET_RASTER_LAYERS='256',HISTREAM_FACET_ENERGY_BACKEND='cpu'),scope='Native GPU radiation and CPU coupled core/surface EB')
        write(root/'run_receipt.json',receipt)
        started=time.monotonic();print('Start native coupled '+label,flush=True)
        with (root/'native.log').open('wb') as log:
            process=subprocess.run([str(engine),'eFacetEB',str(config),str(output/'faceteb.json')],stdout=log,stderr=subprocess.STDOUT,
                env=dict(os.environ,**receipt['environment']))
        receipt.update(returncode=process.returncode,seconds=time.monotonic()-started,finishedUtc=datetime.now(timezone.utc).isoformat())
        receipt['inputsUnchanged']=native_inputs(config)==actual_inputs and sha(engine)==engine_hash and runtime_shaders(engine)==shader_hashes
        index=read(output/'faceteb_run.json') if (output/'faceteb_run.json').exists() else {}
        complete=process.returncode==0 and receipt['inputsUnchanged'] and index.get('status')=='completed'
        if complete:
            try:
                receipt.update(summarize_output(output,index))
            except (OSError,KeyError,ValueError,IntegrityError) as exc:
                complete=False;receipt['summaryError']=f'{type(exc).__name__}: {exc}'
        receipt['status']='completed' if complete else 'failed'
        ok,issues=acceptance(receipt,expected_nodes) if complete else (False,['native run did not complete'])
        receipt['accepted']=ok;receipt['acceptanceIssues']=issues
        receipt['status']='completed' if complete and ok else 'failed'
        write(root/'run_receipt.json',receipt)
        print(json.dumps(dict(label=label,**receipt),ensure_ascii=False),flush=True)
        if not ok:raise RuntimeError('Native run failed the acceptance rules: '+'; '.join(issues)+'; inspect '+str(root/'native.log'))
        results[label]=receipt
    verify_source_inputs(case)
    verify_pair_fingerprints(case,frozen_pair)
    write(case/'native_pair.json',dict(status='passed',runs=results,frozenPairInputs=frozen_pair,sourceInputsUnchanged=True,
        acceptanceRule=f'all {expected_nodes} nodes converged and max surface residual < {SURFACE_RESIDUAL_LIMIT_WM2:g} W/m2',
        scientificFieldValidation=False))


def main():
    p=argparse.ArgumentParser();p.add_argument('case',type=Path);p.add_argument('engine',type=Path);p.add_argument('--resume-completed',action='store_true');a=p.parse_args()
    run(a.case,a.engine,a.resume_completed)


if __name__=='__main__':main()
