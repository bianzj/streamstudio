"""Unified entry contracts; no large scene or GPU needed for this suite."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
ENTRY = TOOLS / 'run_case.py'


class SimulationEntryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='simulation-entry-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.asset = self.root / 'with spaces.obj'
        self.asset.write_text('v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n')
        self.scene = self.root / 'scene.json'
        self.scene.write_text(json.dumps(dict(backend='mesh', assets=[dict(
            path=self.asset.name, sha256=hashlib.sha256(self.asset.read_bytes()).hexdigest())])))
        self.job = dict(schemaVersion=1, pipeline='native-scene', engine=sys.executable,
                        scene='scene.json', output='new result')
        self.job_path = self.root / 'job.json'

    def save(self, job=None):
        self.job_path.write_text(json.dumps(self.job if job is None else job), encoding='utf-8')
        return self.job_path

    def plan(self):
        from simulation.jobs import prepare_job
        return prepare_job(self.save())

    def coupled_job(self):
        source = self.root / 'town'; source.mkdir()
        (source / self.asset.name).write_bytes(self.asset.read_bytes())
        scene = json.loads(self.scene.read_text()); scene['cameras'] = [dict(id='road')]
        (source / 'scene.json').write_text(json.dumps(scene))
        (source / 'actors.json').write_text('{"actors":[]}')
        (source / 'SHA256SUMS.json').write_text('{}')
        (source / 'gpu_geometry').mkdir()
        for name in ('road_hits.f32', 'road_visible.png'):
            (source / 'gpu_geometry' / name).write_bytes(b'input fixture')
        self.job = dict(schemaVersion=1, pipeline='coupled-town', engine=sys.executable,
                        sourceCase='town', actors='town/actors.json', ffmpeg=sys.executable,
                        output='new result')
        return source

    def complete(self, argv, **kwargs):
        target = Path(argv[-1]); target.mkdir()
        (target / 'run-manifest.json').write_text('{"status":"complete"}')
        return subprocess.CompletedProcess(argv, 0)

    def test_entry_and_catalog_exist(self):
        self.assertTrue(ENTRY.is_file(), 'Unified code entry has not been implemented')
        from simulation.catalog import load_catalog, validate_catalog
        catalog = load_catalog()
        self.assertGreaterEqual(len(catalog['features']), 12)
        self.assertEqual(validate_catalog(), [])
        coupled = next(f for f in catalog['features'] if f['id'] == 'coupled-thermal')
        self.assertIn('CPU', coupled['execution'])
        self.assertIn('集中参数', coupled['limitations'])

    def test_check_is_read_only_and_resolves_relative_paths(self):
        before = set(self.root.iterdir()); self.save(); before.add(self.job_path)
        p = self.plan()
        self.assertEqual(Path(p['output']), self.root / 'new result')
        self.assertEqual(p['steps'][0]['argv'][2], str(self.scene.resolve()))
        self.assertEqual(set(self.root.iterdir()), before)

    def test_unknown_option_and_schema_rejected(self):
        from simulation.jobs import prepare_job
        for key, value in [('unimplementedAtmosphere', True), ('schemaVersion', 2), ('schemaVersion', True)]:
            data = copy.deepcopy(self.job); data[key] = value
            with self.assertRaises(ValueError): prepare_job(self.save(data))

    def test_existing_output_is_preserved(self):
        from simulation.jobs import prepare_job
        out = self.root / 'new result'; out.mkdir(); keep = out / 'keep'; keep.write_text('original')
        with self.assertRaises(FileExistsError): prepare_job(self.save())
        self.assertEqual(keep.read_text(), 'original')

    def test_output_cannot_contain_input(self):
        from simulation.jobs import prepare_job
        data = copy.deepcopy(self.job); data['output'] = '.'
        with self.assertRaises((ValueError, FileExistsError)): prepare_job(self.save(data))

    def test_asset_hash_checked_before_any_output(self):
        from simulation.jobs import prepare_job
        self.asset.write_text('changed')
        with self.assertRaisesRegex(ValueError, 'SHA256'): prepare_job(self.save())
        self.assertFalse((self.root / 'new result').exists())

    def test_static_scene_does_not_accept_video(self):
        from simulation.jobs import prepare_job
        self.job['ffmpeg'] = sys.executable
        with self.assertRaisesRegex(ValueError, 'static|mesh'): prepare_job(self.save())

    def test_video_requires_full_sequence_in_preflight(self):
        self.job['ffmpeg'] = sys.executable
        for backend in ('procedural', 'meshSequence'):
            for first, last in ((2, 3), (1, 2)):
                scene = json.loads(self.scene.read_text()); scene['backend'] = backend
                timing = dict(frameCount=3, firstFrame=first, lastFrame=last)
                if backend == 'meshSequence': scene['sequence'] = timing
                else: scene.update(timing)
                self.scene.write_text(json.dumps(scene))
                with self.subTest(backend=backend, first=first, last=last):
                    with self.assertRaisesRegex(ValueError, 'full|complete'): self.plan()
                    self.assertFalse((self.root / 'new result').exists())

    def test_partial_native_without_video_remains_available(self):
        scene = json.loads(self.scene.read_text())
        scene.update(backend='procedural', frameCount=3, firstFrame=2, lastFrame=3)
        self.scene.write_text(json.dumps(scene))
        self.assertEqual(len(self.plan()['steps']), 1)

    def test_success_records_commands_hashes_and_actual_result(self):
        from simulation.jobs import execute_job
        p = self.plan(); receipt = execute_job(p, runner=self.complete)
        self.assertEqual(receipt['status'], 'completed')
        saved = json.loads((Path(p['output']) / 'run.json').read_text())
        self.assertEqual(saved['steps'][0]['returncode'], 0)
        self.assertIn(str(self.asset.resolve()), saved['inputs'])
        self.assertTrue((Path(p['output']) / 'result/run-manifest.json').is_file())

    def test_input_change_after_check_prevents_execution(self):
        from simulation.jobs import execute_job
        p = self.plan(); self.asset.write_text('changed')
        with self.assertRaisesRegex(ValueError, 'changed'): execute_job(p, runner=self.complete)
        self.assertFalse(Path(p['output']).exists())

    def test_change_during_external_process_is_reported(self):
        from simulation.jobs import execute_job
        p = self.plan()
        def mutate(argv, **kwargs):
            self.complete(argv, **kwargs); self.asset.write_text('changed during run')
            return subprocess.CompletedProcess(argv, 0)
        with self.assertRaisesRegex(ValueError, 'changed'): execute_job(p, runner=mutate)
        receipt = json.loads((Path(p['output']) / 'run.json').read_text())
        self.assertEqual(receipt['status'], 'failed')

    def test_external_failure_saved_with_log(self):
        from simulation.jobs import execute_job
        p = self.plan()
        with self.assertRaisesRegex(RuntimeError, '7'):
            execute_job(p, runner=lambda argv, **kw: subprocess.CompletedProcess(argv, 7))
        receipt = json.loads((Path(p['output']) / 'run.json').read_text())
        self.assertEqual(receipt['status'], 'failed')
        self.assertEqual(receipt['steps'][0]['returncode'], 7)

    def test_process_start_exception_marks_step_failed(self):
        from simulation.jobs import execute_job
        p = self.plan()
        def cannot_start(*args, **kwargs): raise OSError('fixture launch denied')
        with self.assertRaises(OSError): execute_job(p, runner=cannot_start)
        receipt = json.loads((Path(p['output']) / 'run.json').read_text())
        self.assertEqual(receipt['status'], 'failed')
        self.assertEqual(receipt['steps'][0]['status'], 'failed')
        self.assertIn('fixture launch denied', receipt['steps'][0]['error'])

    def test_zero_exit_without_native_product_is_failure(self):
        from simulation.jobs import execute_job
        p = self.plan()
        with self.assertRaises((ValueError, FileNotFoundError)):
            execute_job(p, runner=lambda argv, **kw: subprocess.CompletedProcess(argv, 0))
        self.assertEqual(json.loads((Path(p['output']) / 'run.json').read_text())['status'], 'failed')

    def test_features_cli_requires_no_optional_scientific_packages(self):
        self.assertTrue(ENTRY.is_file(), 'Unified code entry has not been implemented')
        p = subprocess.run([sys.executable, '-S', str(ENTRY), 'features', '--json'], capture_output=True)
        self.assertEqual(p.returncode, 0, p.stderr.decode(errors='replace'))
        self.assertEqual(json.loads(p.stdout)['schemaVersion'], 1)

    def test_coupled_plan_has_ordered_existing_tools_and_no_execution(self):
        self.coupled_job(); p = self.plan()
        self.assertEqual([s['name'] for s in p['steps']], [
            'coupled_town_prepare', 'run_coupled_town', 'render_coupled_town'])
        self.assertFalse(Path(p['output']).exists())
        self.assertTrue(all(Path(s['argv'][3]).is_file() for s in p['steps']))

    def test_invalid_actors_type_has_clear_preflight_failure(self):
        source = self.coupled_job(); (source / 'actors.json').write_text('[]')
        with self.assertRaisesRegex(ValueError, 'actors'): self.plan()

    def test_coupled_copy_destinations_cannot_escape_new_result(self):
        source = self.coupled_job(); scene_path = source / 'scene.json'
        for asset_path in ('../with spaces.obj', str(self.asset.resolve())):
            scene = json.loads(scene_path.read_text()); scene['assets'][0]['path'] = asset_path
            scene_path.write_text(json.dumps(scene))
            with self.subTest(asset_path=asset_path):
                with self.assertRaisesRegex(ValueError, 'copy|relative|escape'): self.plan()
                self.assertFalse((self.root / 'new result').exists())
                self.assertEqual(self.asset.read_text(), 'v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n')

    def test_coupled_video_needs_successful_complete_decode(self):
        from simulation.jobs import execute_job
        self.coupled_job(); p = self.plan(); result = Path(p['result'])
        def stages(argv, **kwargs):
            name = Path(argv[3]).stem
            if name == 'coupled_town_prepare': result.mkdir()
            if name == 'run_coupled_town': (result / 'native_pair.json').write_text('{"status":"passed"}')
            if name == 'render_coupled_town':
                (result / 'coupling_feedback.json').write_text('{}')
                (result / 'video_verification.json').write_text('{"status":"failed","completeDecode":false}')
                (result / 'town_coupled_heat_cool.mp4').write_bytes(b'incomplete')
            return subprocess.CompletedProcess(argv, 0)
        with self.assertRaisesRegex(ValueError, 'video|decode'): execute_job(p, runner=stages)
        self.assertEqual(json.loads((Path(p['output']) / 'run.json').read_text())['status'], 'failed')

    def airfield_job(self):
        town = self.root / 'airtown'; town.mkdir()
        (town / self.asset.name).write_bytes(self.asset.read_bytes())
        (town / 'scene.json').write_text(self.scene.read_text())
        (town / 'actors.json').write_text('{"actors":[]}')
        actors = self.root / 'runway.actors.json'; actors.write_text('{"actors":[{"id":"tank_a"}]}')
        self.job = dict(schemaVersion=1, pipeline='airfield-day', engine=sys.executable, townCase='airtown',
                        actors=actors.name, ffmpeg=sys.executable, output='new result', videoViews=['overview'])
        return town

    def test_airfield_plan_covers_the_whole_chain(self):
        self.airfield_job(); p = self.plan()
        self.assertEqual([s['name'] for s in p['steps']], ['make_runway_day_scene', 'prepare_runway_day', 'geometry_thermal', 'geometry_display',
                                                         'run_runway_day', 'render_runway_day', 'build_runway_day_deck'])
        run = next(s for s in p['steps'] if s['name'] == 'run_runway_day')['argv']
        self.assertEqual(run[run.index('--parallel') + 1], '4')
        render = next(s for s in p['steps'] if s['name'] == 'render_runway_day')['argv']
        self.assertEqual(render[render.index('--video-encoder') + 1], 'auto')
        self.assertFalse(Path(p['output']).exists())
        no_deck = dict(self.job, deck=False, parallel=2, videoEncoder='libopenh264')
        self.save(no_deck)
        from simulation.jobs import prepare_job
        q = prepare_job(self.job_path)
        self.assertNotIn('build_runway_day_deck', [s['name'] for s in q['steps']])

    def test_airfield_options_are_validated(self):
        self.airfield_job()
        from simulation.jobs import prepare_job
        for key, value in [('parallel', 5), ('parallel', True), ('spinupDays', 0), ('videoViews', ['nope']), ('videoEncoder', 'h265'),
                           ('deck', 'yes'), ('perRunVramGb', 0)]:
            with self.subTest(key=key, value=value):
                self.save(dict(self.job, **{key: value}))
                with self.assertRaises(ValueError): prepare_job(self.job_path)

    def test_airfield_execution_requires_seals_and_matching_videos(self):
        from simulation.jobs import execute_job
        from case_seal import write_seal
        self.airfield_job(); p = self.plan(); result = Path(p['result'])
        def stages(argv, tamper=False, **kwargs):
            name = Path(argv[3]).stem if len(argv) > 3 and argv[3].endswith('.py') else ''
            if name == 'make_runway_day_scene': (result / 'display_scene').mkdir(parents=True)
            if name == 'run_runway_day':
                (result / 'native_runs.json').write_text('{"status":"passed"}'); write_seal(result)
            if name == 'render_runway_day':
                video = result / 'airfield_day_overview.mp4'; video.write_bytes(b'video')
                (result / 'render_receipt.json').write_text(json.dumps(dict(status='passed', videos=dict(overview=dict(
                    file=video.name, completeDecode=True, sha256=hashlib.sha256(b'video').hexdigest())))))
                write_seal(result, kind='render')
            if name == 'build_runway_day_deck': (result / 'airfield_day_mwir.pptx').write_bytes(b'deck')
            return subprocess.CompletedProcess(argv, 0)
        receipt = execute_job(p, runner=stages)
        self.assertEqual(receipt['status'], 'completed')
        # Second job: the video is changed after the render seal -> rejected.
        self.job['output'] = 'second result'; q = self.plan(); result = Path(q['result'])
        def tampered(argv, **kwargs):
            r = stages(argv, **kwargs)
            if Path(argv[3]).stem == 'build_runway_day_deck' if len(argv) > 3 else False:
                (result / 'airfield_day_overview.mp4').write_bytes(b'other')
            return r
        with self.assertRaises((ValueError, RuntimeError)): execute_job(q, runner=tampered)
        self.assertEqual(json.loads((Path(q['output']) / 'run.json').read_text())['status'], 'failed')

    def test_check_cli_from_another_working_directory(self):
        self.save()
        p = subprocess.run([sys.executable, '-S', str(ENTRY), 'check', str(self.job_path)],
                           cwd=self.root.parent, capture_output=True)
        self.assertEqual(p.returncode, 0, p.stderr.decode(errors='replace'))
        self.assertEqual(json.loads(p.stdout)['steps'][0]['argv'][2], str(self.scene.resolve()))
        self.assertFalse((self.root / 'new result').exists())


if __name__ == '__main__': unittest.main()
