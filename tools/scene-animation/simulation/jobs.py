"""Preflight and execute existing pipelines, retaining failures and provenance.

This is orchestration only: no thermal, collision, or camera formulas live here.
"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys

TOOLS = Path(__file__).resolve().parents[1]
REPO = TOOLS.parents[1]
FIELDS = {
    'native-scene': {'schemaVersion', 'pipeline', 'engine', 'scene', 'output', 'ffmpeg'},
    'coupled-town': {'schemaVersion', 'pipeline', 'engine', 'sourceCase', 'actors', 'output', 'ffmpeg'},
    # Open-airfield mid-IR day: make -> prepare -> 2 GPU geometry passes -> 4 native solves (+seal) -> render -> deck.
    'airfield-day': {'schemaVersion', 'pipeline', 'engine', 'townCase', 'actors', 'output', 'ffmpeg',
                     'spinupDays', 'parallel', 'perRunVramGb', 'videoViews', 'videoEncoder', 'deck'},
}
AIRFIELD_VIEWS = ('overview', 'runway_view', 'vehicle_view', 'high_view')


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''): h.update(block)
    return h.hexdigest()


def write(path, data):
    path = Path(path); temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def prepare_job(path):
    """Return resolved argv and frozen input hashes. Read-only; no subprocesses."""
    path = Path(path).resolve(); config = read(path)
    if not isinstance(config, dict) or type(config.get('schemaVersion')) is not int or config['schemaVersion'] != 1:
        raise ValueError('schemaVersion must be integer 1')
    pipeline = config.get('pipeline')
    if pipeline not in FIELDS: raise ValueError('Unsupported pipeline: ' + str(pipeline))
    unknown = set(config) - FIELDS[pipeline]
    if unknown: raise ValueError('Unsupported job fields: ' + ', '.join(sorted(unknown)))
    inputs = {path}

    def resolve(value, base=path.parent):
        if not isinstance(value, str) or not value.strip(): raise ValueError('Expected a nonempty path string')
        p = Path(value)
        return (base / p).resolve() if not p.is_absolute() else p.resolve()

    def file(p):
        if not p.is_file(): raise FileNotFoundError(p)
        inputs.add(p)
        return p

    def required(key):
        if key not in config: raise ValueError('Missing job field: ' + key)
        return resolve(config[key])

    def scene_inputs(scene_path):
        scene = read(file(scene_path))
        if not isinstance(scene, dict): raise ValueError('Scene must be a JSON object')
        for asset in scene.get('assets', []):
            asset_path = file(resolve(asset['path'], scene_path.parent))
            if sha(asset_path) != asset['sha256']: raise ValueError('Asset SHA256 mismatch: ' + str(asset_path))
        return scene

    engine = file(required('engine')); output = required('output')
    if output.exists(): raise FileExistsError('Output must be new: ' + str(output))
    if not output.parent.is_dir(): raise FileNotFoundError('Output parent must exist: ' + str(output.parent))
    result = output / 'result'; steps = []

    def tool(name, *args):
        script = file(TOOLS / name)
        steps.append(dict(name=script.stem, argv=[sys.executable, '-X', 'utf8', str(script), *map(str, args)]))

    ffmpeg = file(required('ffmpeg')) if 'ffmpeg' in config else None
    if pipeline == 'native-scene':
        scene_path = required('scene'); scene = scene_inputs(scene_path)
        backend = scene.get('backend', 'procedural')
        if backend not in ('mesh', 'meshSequence', 'procedural'): raise ValueError('Unsupported scene backend')
        if ffmpeg and backend == 'mesh': raise ValueError('Static mesh has no timeline to encode')
        if ffmpeg:
            timing = scene.get('sequence', {}) if backend == 'meshSequence' else scene
            if not isinstance(timing, dict): raise ValueError('Video requires a complete timeline object')
            count = timing.get('frameCount', 288)
            first, last = timing.get('firstFrame', 1), timing.get('lastFrame', count)
            if any(type(value) is not int for value in (count, first, last)) or count < 1 or first != 1 or last != count:
                raise ValueError('Video packaging requires the full sequence, firstFrame=1 and lastFrame=frameCount')
        steps.append(dict(name='eSceneAnimation', argv=[str(engine), 'eSceneAnimation', str(scene_path), str(result)]))
        if ffmpeg:
            if backend == 'meshSequence': tool('package_mesh_sequence.py', result, output / 'video', '--ffmpeg', ffmpeg)
            else: tool('render_video.py', result, '--ffmpeg', ffmpeg, '--engine', engine)
    elif pipeline == 'airfield-day':
        town = required('townCase')
        if not town.is_dir(): raise FileNotFoundError(town)
        scene_inputs(town / 'scene.json'); file(town / 'actors.json')
        actors = file(required('actors'))
        actor_config = read(actors)
        if not isinstance(actor_config, dict) or not isinstance(actor_config.get('actors'), list):
            raise ValueError('actors file must contain actors[]')
        if ffmpeg is None: raise ValueError('airfield-day requires explicit ffmpeg')

        def integer(key, default, lo, hi):
            value = config.get(key, default)
            if type(value) is not int or not lo <= value <= hi: raise ValueError(f'{key} must be an integer within {lo}..{hi}')
            return value
        spinup = integer('spinupDays', 12, 1, 60); parallel = integer('parallel', 4, 1, 4)
        vram = config.get('perRunVramGb', 14.6)
        if isinstance(vram, bool) or not isinstance(vram, (int, float)) or not 0.5 <= vram <= 200: raise ValueError('perRunVramGb must be a number within 0.5..200')
        views = config.get('videoViews', ['overview', 'vehicle_view'])
        if not isinstance(views, list) or any(v not in AIRFIELD_VIEWS for v in views) or len(set(views)) != len(views):
            raise ValueError('videoViews must be a list of distinct views from ' + ', '.join(AIRFIELD_VIEWS))
        encoder = config.get('videoEncoder', 'auto')
        if encoder not in ('auto', 'libx264', 'libopenh264', 'mpeg4'): raise ValueError('videoEncoder must be auto, libx264, libopenh264 or mpeg4')
        deck = config.get('deck', True)
        if type(deck) is not bool: raise ValueError('deck must be boolean')
        tool('make_runway_day_scene.py', town, result / 'display_scene')
        tool('prepare_runway_day.py', result / 'display_scene', actors, result, '--spinup-days', spinup)
        # Static GPU hit/visibility passes used by the renderer (not thermal solves); sealed by run_runway_day.
        steps.append(dict(name='geometry_thermal', argv=[str(engine), 'eSceneAnimation', str(result / 'thermal' / 'scene.json'), str(result / 'gpu_geometry_thermal')]))
        steps.append(dict(name='geometry_display', argv=[str(engine), 'eSceneAnimation', str(result / 'scene.json'), str(result / 'gpu_geometry_display')]))
        tool('run_runway_day.py', result, engine, '--parallel', parallel, '--per-run-vram-gb', vram)
        tool('render_runway_day.py', result, '--ffmpeg', ffmpeg, '--video-encoder', encoder, '--video-views', *views)
        if deck: tool('build_runway_day_deck.py', result, result / 'airfield_day_mwir.pptx')
        for helper in ('prepare_thermal_case.py', 'thermal_imaging.py', 'coupled_town_prepare.py', 'run_coupled_town.py', 'case_seal.py',
                       'steps_manifest.py', 'integrity.py', 'video_encoder.py'): file(TOOLS / helper)
    else:
        source = required('sourceCase')
        if not source.is_dir(): raise FileNotFoundError(source)
        scene = scene_inputs(source / 'scene.json')
        # This legacy preparer copies using the original asset path. Unlike the
        # native loader, it must only receive destinations inside the new case.
        for asset in scene.get('assets', []):
            relative = Path(asset['path'])
            if relative.is_absolute() or not (result / relative).resolve().is_relative_to(result):
                raise ValueError('Coupled asset copy requires a relative path inside the new result: ' + str(relative))
        file(source / 'actors.json'); file(source / 'SHA256SUMS.json')
        for camera in scene['cameras']:
            for suffix in ('_hits.f32', '_visible.png'): file(source / 'gpu_geometry' / (camera['id'] + suffix))
        actors = file(required('actors'))
        actor_config = read(actors)
        if not isinstance(actor_config, dict) or not isinstance(actor_config.get('actors'), list):
            raise ValueError('actors file must contain actors[]')
        if ffmpeg is None: raise ValueError('coupled-town requires explicit ffmpeg')
        tool('coupled_town_prepare.py', source, actors, result)
        tool('run_coupled_town.py', result, engine)
        tool('render_coupled_town.py', result, engine, '--ffmpeg', ffmpeg)
        for helper in ('prepare_thermal_case.py', 'thermal_imaging.py'): file(TOOLS / helper)
    # Freeze the actual orchestration and runtime, without importing scientific libraries.
    for helper in ('run_case.py', 'simulation/jobs.py', 'simulation/catalog.py', 'simulation/capabilities.json'):
        file(TOOLS / helper)
    for shader in (engine.parent / 'shader').rglob('*.spv'): file(shader)
    for source_file in inputs:
        if source_file.is_relative_to(output): raise ValueError('Output cannot contain an input: ' + str(source_file))
    return dict(schemaVersion=1, pipeline=pipeline, job=str(path), output=str(output), result=str(result),
                configuration=config, steps=steps, inputs={str(p): sha(p) for p in sorted(inputs)},
                scope='Orchestration of existing implementations; scientific validity requires native diagnostics')


def verify_inputs(inputs):
    for name, expected in inputs.items():
        p = Path(name)
        if not p.is_file() or sha(p) != expected: raise ValueError('Input changed since check: ' + name)


def verify_airfield(result, config):
    """Final airfield acceptance: both seals verify, the render passed and every video matches its receipt."""
    sys.path.insert(0, str(TOOLS))
    from case_seal import verify_seal, CASE_SEAL, RENDER_SEAL
    verify_seal(result, CASE_SEAL)
    verify_seal(result, RENDER_SEAL)
    render = read(result / 'render_receipt.json')
    if render.get('status') != 'passed': raise ValueError('render_receipt.json did not pass')
    for view in config.get('videoViews', ['overview', 'vehicle_view']):
        video = render.get('videos', {}).get(view)
        if not video or video.get('completeDecode') is not True: raise ValueError('missing or undecodable video for ' + view)
        if sha(result / video['file']) != video.get('sha256'): raise ValueError('video hash differs from render receipt: ' + video['file'])
    if config.get('deck', True) and not (result / 'airfield_day_mwir.pptx').is_file():
        raise FileNotFoundError(result / 'airfield_day_mwir.pptx')


def execute_job(plan, runner=None):
    """Run a checked job. runner is the subprocess adapter used by small tests."""
    verify_inputs(plan['inputs'])
    output = Path(plan['output']); output.mkdir(exist_ok=False)
    (output / 'logs').mkdir()
    receipt = dict(plan, status='running', startedUtc=datetime.now(timezone.utc).isoformat(), steps=[])
    write(output / 'job.json', plan['configuration']); write(output / 'run.json', receipt)
    runner = subprocess.run if runner is None else runner
    try:
        for i, step in enumerate(plan['steps']):
            verify_inputs(plan['inputs'])
            log = output / 'logs' / f'{i + 1:02d}-{step["name"]}.log'
            record = dict(step, status='running', log=str(log)); receipt['steps'].append(record)
            write(output / 'run.json', receipt)
            try:
                with log.open('wb') as stream:
                    process = runner(step['argv'], cwd=str(REPO), stdout=stream, stderr=subprocess.STDOUT, shell=False)
            except BaseException as exc:
                record.update(status='failed', error=f'{type(exc).__name__}: {exc}')
                raise
            record.update(returncode=process.returncode, status='completed' if process.returncode == 0 else 'failed')
            write(output / 'run.json', receipt)
            if process.returncode: raise RuntimeError(f'{step["name"]} failed ({process.returncode}); see {log}')
        verify_inputs(plan['inputs'])
        result = Path(plan['result'])
        proof = result / {'native-scene': 'run-manifest.json', 'coupled-town': 'native_pair.json', 'airfield-day': 'native_runs.json'}[plan['pipeline']]
        expected = 'complete' if plan['pipeline'] == 'native-scene' else 'passed'
        if read(proof).get('status') != expected: raise ValueError('Result did not report completion: ' + str(proof))
        if plan['pipeline'] == 'airfield-day':
            verify_airfield(result, plan['configuration'])
        if plan['pipeline'] == 'coupled-town':
            for name in ('coupling_feedback.json', 'video_verification.json', 'town_coupled_heat_cool.mp4'):
                if not (result / name).is_file(): raise FileNotFoundError(result / name)
            video = read(result / 'video_verification.json')
            if video.get('status') != 'passed' or video.get('completeDecode') is not True:
                raise ValueError('Coupled video did not report a successful complete decode')
            if video.get('sha256') != sha(result / 'town_coupled_heat_cool.mp4'):
                raise ValueError('Coupled video hash differs from decode receipt')
        receipt.update(status='completed', nativeReceipt=str(proof), nativeReceiptSha256=sha(proof))
    except BaseException as exc:
        receipt.update(status='failed', error=f'{type(exc).__name__}: {exc}')
        raise
    finally:
        receipt['finishedUtc'] = datetime.now(timezone.utc).isoformat()
        write(output / 'run.json', receipt)
    return receipt
