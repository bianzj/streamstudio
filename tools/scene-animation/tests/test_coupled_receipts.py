import json,tempfile,unittest
from pathlib import Path
from run_coupled_town import native_inputs,snapshot_runtime,validate_pair,verify_pair_fingerprints


class CoupledReceiptTests(unittest.TestCase):
    def test_off_forcing_cannot_change_between_paired_runs(self):
        with tempfile.TemporaryDirectory() as d:
            r=Path(d)
            for label in ['on','off']:
                p=r/'native'/label;p.mkdir(parents=True)
                for name in ['world.obj','origin.txt','weather.txt']:(p/name).write_text(name)
                (p/'project.json').write_text(json.dumps(dict(configuration=dict(meteo=dict(path='weather.txt'),materials=[],objects=dict(items=[dict(fileName='world.obj',positionFile='origin.txt')])))))
            frozen=validate_pair(r)
            (r/'native/off/weather.txt').write_text('changed between on and off')
            with self.assertRaises(ValueError):verify_pair_fingerprints(r,frozen)
    def test_fingerprint_contains_actual_referenced_files(self):
        with tempfile.TemporaryDirectory() as d:
            r=Path(d)
            for n in ['world.obj','weather.txt','origin.txt']:(r/n).write_text(n)
            p=r/'project.json';p.write_text(json.dumps(dict(configuration=dict(meteo=dict(path='weather.txt'),objects=dict(items=[dict(fileName='world.obj',positionFile='origin.txt')])))))
            inputs=native_inputs(p)
            self.assertEqual(len(inputs),4)
            old=inputs['weather.txt']
            (r/'weather.txt').write_text('changed')
            self.assertNotEqual(old,native_inputs(p)['weather.txt'])
    def test_resume_preserves_receipt_and_source_snapshot(self):
        with tempfile.TemporaryDirectory() as d:
            r=Path(d);engine=r/'engine.exe';source=r/'source.cpp';engine.write_bytes(b'engine');source.write_text('v1');case=r/'case';case.mkdir()
            snapshot_runtime(case,engine,[source],False)
            old=(case/'runtime/receipt.json').read_bytes();original=(case/'source/source.cpp').read_bytes()
            snapshot_runtime(case,engine,[source],True)
            self.assertEqual(old,(case/'runtime/receipt.json').read_bytes())
            self.assertEqual(original,(case/'source/source.cpp').read_bytes())
            source.write_text('v2')
            with self.assertRaises(ValueError):snapshot_runtime(case,engine,[source],True)
            self.assertEqual(old,(case/'runtime/receipt.json').read_bytes())
            self.assertEqual(original,(case/'source/source.cpp').read_bytes())
    def test_fresh_run_rejects_existing_runtime(self):
        with tempfile.TemporaryDirectory() as d:
            r=Path(d);e=r/'e.exe';s=r/'s.cpp';e.write_bytes(b'e');s.write_text('v1');case=r/'case';case.mkdir()
            snapshot_runtime(case,e,[s],False)
            with self.assertRaises(FileExistsError):snapshot_runtime(case,e,[s],False)


if __name__=='__main__':unittest.main()
