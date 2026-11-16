"""Integrity helpers added after the 2026-11-12 review: require (-O safe), GPU-memory plan,
per-node steps manifests and ffmpeg encoder selection."""
import json, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
from integrity import IntegrityError, require
from run_runway_day import plan_parallel
from steps_manifest import (StepsManifestError, StepsVerifier, load_manifest, merkle_root,
                            verify_output, write_manifest, STEPS_MANIFEST)
from case_seal import write_seal, verify_seal
import video_encoder

FFMPEGS = [Path(r'C:\Users\jiank\AppData\Local\kzip_sogou\ffmpeg.exe'), Path(r'C:\work\miniconda\Library\bin\ffmpeg.exe')]


class RequireTests(unittest.TestCase):
    def test_require_raises_with_lazy_message(self):
        calls = []
        require(True, lambda: calls.append(1))
        self.assertEqual(calls, [])
        with self.assertRaisesRegex(IntegrityError, 'boom 3'):
            require(False, lambda: f'boom {1 + 2}')
        with self.assertRaisesRegex(IntegrityError, r"\('tuple', 1\)"):
            require(0, ('tuple', 1))
        self.assertTrue(issubclass(IntegrityError, RuntimeError))

    def test_require_survives_python_O(self):
        code = 'import sys; sys.path.insert(0, sys.argv[1]); from integrity import require\nassert False\nrequire(False, "still checked")'
        r = subprocess.run([sys.executable, '-O', '-c', code, str(HERE)], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('still checked', r.stderr)

    def test_production_scripts_have_no_statement_asserts(self):
        import ast
        for name in ('run_runway_day.py', 'render_runway_day.py', 'run_coupled_town.py', 'render_coupled_town.py', 'finalize_coupled_town.py',
                     'render_video.py', 'prepare_runway_day.py', 'make_runway_day_scene.py', 'coupled_town_prepare.py'):
            tree = ast.parse((HERE / name).read_text(encoding='utf-8'))
            self.assertFalse([n.lineno for n in ast.walk(tree) if isinstance(n, ast.Assert)], name)


class ParallelPlanTests(unittest.TestCase):
    def test_clamps_to_free_memory(self):
        self.assertEqual(plan_parallel(4, 14.6, 60)[0], 4)
        self.assertEqual(plan_parallel(4, 14.6, 47)[0], 3)
        self.assertEqual(plan_parallel(4, 14.6, 23)[0], 1)
        self.assertIn('unknown', plan_parallel(4, 14.6, None)[1])
        self.assertEqual(plan_parallel(2, 14.6, None)[0], 2)
        with self.assertRaises(IntegrityError):
            plan_parallel(4, 14.6, 10)


class StepsManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.case = Path(self.temp.name) / 'case'
        self.output = self.case / 'native' / 'run_a' / 'output'
        (self.output / '.facet_steps').mkdir(parents=True)
        (self.output / 'diagnostics').mkdir()
        for k in range(3):
            (self.output / '.facet_steps' / f'energy_T=node{k}.bin').write_bytes(bytes([k]) * 1000)
            (self.output / 'diagnostics' / f'facet_physics_node={k}.json').write_text(json.dumps({'node': k}), encoding='utf-8')
        (self.output / 'faceteb_run.json').write_text('{}', encoding='utf-8')
        (self.case / 'input.txt').write_text('x', encoding='utf-8')

    def tearDown(self):
        self.temp.cleanup()

    def test_manifest_is_sealed_and_detects_tampering(self):
        payload = write_manifest(self.output)
        self.assertEqual(payload['fileCount'], 6)
        self.assertEqual(payload['merkleRoot'], merkle_root(payload['files']))
        seal = write_seal(self.case)
        self.assertIn(f'native/run_a/output/{STEPS_MANIFEST}', seal['files'])
        self.assertFalse(any('.facet_steps' in k for k in seal['files']), 'arrays stay outside the case seal')
        verify_output(self.output)
        verifier = StepsVerifier(self.case, ['run_a'], 'lazy')
        verifier.prepare(sealed=True)
        verifier.check('run_a', self.output / '.facet_steps' / 'energy_T=node1.bin')
        self.assertEqual(verifier.summary()['verifiedFiles'], 1)
        target = self.output / '.facet_steps' / 'energy_T=node2.bin'
        target.write_bytes(bytes([9]) * 1000)          # same size, different content
        with self.assertRaises(StepsManifestError):
            verifier.check('run_a', target)
        with self.assertRaises(StepsManifestError):
            verify_output(self.output)
        verify_seal(self.case)                          # the seal alone cannot see the array edit

    def test_full_mode_and_legacy_and_unsealed_manifest(self):
        verifier = StepsVerifier(self.case, ['run_a'], 'full')
        verifier.prepare(sealed=False)
        self.assertEqual(verifier.summary()['legacyRunsWithoutManifest'], ['run_a'])
        write_seal(self.case)
        write_manifest(self.output)                     # written after sealing -> refused
        with self.assertRaises(StepsManifestError):
            StepsVerifier(self.case, ['run_a'], 'full').prepare(sealed=True)
        write_seal(self.case)
        full = StepsVerifier(self.case, ['run_a'], 'full')
        full.prepare(sealed=True)
        self.assertEqual(full.summary()['verifiedFiles'], 6)

    def test_edited_manifest_is_rejected(self):
        write_manifest(self.output)
        path = self.output / STEPS_MANIFEST
        data = json.loads(path.read_text(encoding='utf-8'))
        first = next(iter(data['files']))
        data['files'][first]['sha256'] = '0' * 64
        path.write_text(json.dumps(data), encoding='utf-8')
        with self.assertRaises(StepsManifestError):
            load_manifest(self.output)


class EncoderTests(unittest.TestCase):
    def test_encoder_arguments(self):
        self.assertEqual(video_encoder.encoder_args('libx264', 17, 'medium'), ['-c:v', 'libx264', '-preset', 'medium', '-crf', '17'])
        self.assertNotIn('-crf', video_encoder.encoder_args('libopenh264'))
        self.assertEqual(video_encoder.encoder_args('mpeg4')[:2], ['-c:v', 'mpeg4'])
        with self.assertRaises(video_encoder.EncoderError):
            video_encoder.encoder_args('h265')

    def test_probe_picks_a_working_encoder_or_explains(self):
        found = [f for f in FFMPEGS if f.is_file()]
        if not found:
            self.skipTest('no local ffmpeg')
        for ffmpeg in found:
            name = video_encoder.pick_encoder(ffmpeg)
            self.assertIn(name, video_encoder.PREFERENCE)
            with self.assertRaises(video_encoder.EncoderError):
                video_encoder.pick_encoder(ffmpeg, 'h265')
        missing = Path(tempfile.gettempdir()) / 'no_such_ffmpeg.exe'
        with self.assertRaises((video_encoder.EncoderError, OSError)):
            video_encoder.pick_encoder(missing)


if __name__ == '__main__':
    unittest.main()
