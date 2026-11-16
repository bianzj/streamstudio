"""Portable case-seal tests (standard library only, no GPU)."""
import json
import shutil
import tempfile
import unittest
from pathlib import Path

from case_seal import CASE_SEAL, RENDER_SEAL, CaseSealError, verify_seal, write_seal


class CaseSealTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.case = Path(self.temp.name) / 'case'
        (self.case / 'native' / 'run' / 'output' / '.facet_steps').mkdir(parents=True)
        (self.case / 'stills').mkdir()
        (self.case / 'video_frames' / 'overview').mkdir(parents=True)
        (self.case / 'radiance').mkdir()
        (self.case / 'rgb').mkdir()
        (self.case / 'scene.json').write_text('{"scene": 1}', encoding='utf-8')
        (self.case / 'actors.json').write_text('{"actors": []}', encoding='utf-8')
        (self.case / 'native' / 'run' / 'project.json').write_text('{"project": 1}', encoding='utf-8')
        (self.case / 'native' / 'run' / 'output' / 'faceteb_run.json').write_text('{"completed": []}', encoding='utf-8')
        (self.case / 'native' / 'run' / 'output' / 'faceteb.json').write_text('large geometry', encoding='utf-8')
        (self.case / 'native' / 'run' / 'output' / '.facet_steps' / 'energy.bin').write_bytes(b'large step')
        (self.case / 'render_receipt.json').write_text('{"status": "passed"}', encoding='utf-8')
        (self.case / 'day_history.csv').write_text('hour,value\n', encoding='utf-8')
        (self.case / 'stills' / 'a.png').write_bytes(b'png')
        (self.case / 'video_frames' / 'overview' / 'frame_0000.png').write_bytes(b'frame')
        (self.case / 'radiance' / 'view.npz').write_bytes(b'npz')
        (self.case / 'rgb' / 'overview.png').write_bytes(b'rgb')
        (self.case / 'video.mp4').write_bytes(b'mp4')

    def tearDown(self):
        self.temp.cleanup()

    def test_case_seal_is_relative_and_excludes_large_products(self):
        payload = write_seal(self.case)
        files = payload['files']
        self.assertIn('scene.json', files)
        self.assertIn('native/run/output/faceteb_run.json', files)
        self.assertNotIn('native/run/output/faceteb.json', files)
        self.assertNotIn('native/run/output/.facet_steps/energy.bin', files)
        self.assertNotIn('stills/a.png', files)
        self.assertNotIn('video.mp4', files)
        self.assertNotIn(CASE_SEAL, files)
        self.assertFalse(any(Path(k).is_absolute() or ':' in k for k in files))
        summary = verify_seal(self.case)
        self.assertEqual(summary['kind'], 'case')
        self.assertGreater(summary['files'], 3)

    def test_changed_input_is_rejected(self):
        write_seal(self.case)
        (self.case / 'scene.json').write_text('{"scene": 2}', encoding='utf-8')
        with self.assertRaises(CaseSealError) as caught:
            verify_seal(self.case)
        self.assertIn('scene.json', str(caught.exception))

    def test_missing_input_is_rejected(self):
        write_seal(self.case)
        (self.case / 'actors.json').unlink()
        with self.assertRaises(CaseSealError) as caught:
            verify_seal(self.case)
        self.assertIn('actors.json', str(caught.exception))

    def test_seal_survives_copying_the_case(self):
        write_seal(self.case)
        moved = Path(self.temp.name) / 'moved'
        shutil.copytree(self.case, moved)
        summary = verify_seal(moved)
        self.assertEqual(summary['changed'], [])
        self.assertEqual(summary['missing'], [])

    def test_unsealed_file_is_reported_and_strict_mode_rejects(self):
        write_seal(self.case)
        (self.case / 'notes.txt').write_text('new', encoding='utf-8')
        summary = verify_seal(self.case)
        self.assertIn('notes.txt', summary['extras'])
        with self.assertRaises(CaseSealError):
            verify_seal(self.case, strict_extras=True)

    def test_render_seal_covers_products_not_inputs(self):
        payload = write_seal(self.case, kind='render')
        files = payload['files']
        self.assertIn('render_receipt.json', files)
        self.assertIn('stills/a.png', files)
        self.assertIn('video.mp4', files)
        self.assertNotIn('scene.json', files)
        self.assertNotIn('radiance/view.npz', files)
        verify_seal(self.case, RENDER_SEAL)
        (self.case / 'stills' / 'a.png').write_bytes(b'changed')
        with self.assertRaises(CaseSealError):
            verify_seal(self.case, RENDER_SEAL)

    def test_missing_seal_is_clear(self):
        with self.assertRaises(CaseSealError):
            verify_seal(self.case)

    def test_cli_render_verify_defaults_to_render_seal(self):
        import contextlib
        import io
        import sys
        from case_seal import main
        write_seal(self.case, kind='render')
        old = sys.argv
        sys.argv = ['case_seal.py', str(self.case), '--kind', 'render', '--verify']
        try:
            with contextlib.redirect_stdout(io.StringIO()) as captured:
                code = main()
        finally:
            sys.argv = old
        self.assertEqual(code, 0)
        self.assertIn('"kind": "render"', captured.getvalue())


if __name__ == '__main__':
    unittest.main()
