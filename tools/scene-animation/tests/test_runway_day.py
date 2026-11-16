import copy,json,tempfile,unittest
from pathlib import Path
import numpy as np
from make_runway_day_scene import build,TOWN_INSTANCES,PLACEMENT,RUNWAY
from prepare_runway_day import prepare,RUNS,time_axis,forcing,NODE_SECONDS
from run_runway_day import validate_design,acceptance
from run_coupled_town import canonical_inputs,native_inputs,sha
from integrity import IntegrityError

TOWN_CASE=Path(r'D:\cases\StreamStudio\supplemental\impact_gpu_20261107\mwir_town_coupled_v5_20261109')
ACTORS=Path(__file__).resolve().parents[1]/'samples/runway-day.actors.json'


class RunwayDayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.case=Path(cls.temp.name)/'case';cls.case.mkdir()
        cls.report=build(TOWN_CASE,cls.case/'display_scene')
        prepare(cls.case/'display_scene',ACTORS,cls.case,spinup_days=3)
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def test_town_is_removed_and_props_stay_on_the_runway(self):
        scene=json.loads((self.case/'display_scene/scene.json').read_text(encoding='utf-8'))
        ids=[i['id'] for i in scene['instances']]
        self.assertFalse(set(ids)&set(TOWN_INSTANCES))
        self.assertEqual(sorted(i for i in ids if i in PLACEMENT),sorted(PLACEMENT))
        self.assertIn('airfield_infill',ids);self.assertGreaterEqual(self.report['minimumVehicleGapM'],2.5)
        for name,(x0,y0,x1,y1,_) in ((k,v) for k,v in self.report['footprints'].items()):
            self.assertTrue(RUNWAY['x'][0]<=x0 and x1<=RUNWAY['x'][1] and RUNWAY['y'][0]<=y0 and y1<=RUNWAY['y'][1],name)
    def test_source_case_assets_are_copied_unchanged(self):
        for p in (self.case/'display_scene/assets').glob('*.obj'):
            source=TOWN_CASE/'assets'/p.name
            if source.exists():self.assertEqual(p.read_bytes(),source.read_bytes(),p.name)
    def test_time_axis_is_uniform_and_ends_on_the_output_day(self):
        nodes=time_axis(3)
        self.assertEqual(len(nodes),4*48);self.assertEqual({b['elapsedSeconds']-a['elapsedSeconds'] for a,b in zip(nodes,nodes[1:])},{NODE_SECONDS})
        self.assertEqual(sum(n['output'] for n in nodes),48);self.assertTrue(all(n['output'] for n in nodes[-48:]))
        self.assertAlmostEqual(nodes[-1]['julianDay'],180.0);self.assertAlmostEqual(nodes[-1]['clockHour'],0.0)
    def test_forcing_switch_changes_only_the_shortwave_column(self):
        for node in time_axis(3):
            a,b=forcing(node,True),forcing(node,False)
            self.assertEqual(a[:5]+a[6:],b[:5]+b[6:]);self.assertEqual(b[5],0.0)
        noon=next(n for n in time_axis(3) if n['output'] and abs(n['clockHour']-12)<1e-9)
        self.assertAlmostEqual(forcing(noon,True)[5],850.0)
    def test_design_is_exactly_a_two_by_two(self):
        frozen=validate_design(self.case);self.assertEqual(set(frozen),set(RUNS))
    def test_design_check_rejects_a_hidden_difference(self):
        for label,mutate in {
            'payload pair wind':lambda root:self._edit_forcing(root/'native/sunlit_unpowered/meteorology.txt',4,3.0),
            'payload pair material':lambda root:self._edit_project(root/'native/nosun_powered/project.json',lambda c:c['materials'][0].update(extra=1)),
            'sun pair air temperature':lambda root:self._edit_forcing(root/'native/nosun_unpowered/meteorology.txt',1,1.0),
            'sun-off keeps shortwave':lambda root:self._edit_forcing(root/'native/nosun_powered/meteorology.txt',5,100.0),
        }.items():
            with self.subTest(label),tempfile.TemporaryDirectory() as d:
                copy_root=Path(d)/'c';(copy_root/'native').mkdir(parents=True)
                for run in RUNS:
                    target=copy_root/'native'/run;target.mkdir()
                    for p in (self.case/'native'/run).iterdir():
                        if p.is_file():
                            data=p.read_text(encoding='utf-8').replace(str(self.case),str(copy_root)) if p.suffix in ('.json','.txt') else p.read_bytes()
                            (target/p.name).write_text(data,encoding='utf-8') if isinstance(data,str) else (target/p.name).write_bytes(data)
                validate_design(copy_root)
                mutate(copy_root)
                with self.assertRaises(IntegrityError):validate_design(copy_root)
    def test_only_working_payloads_change_power(self):
        for run,(sun,payload) in RUNS.items():
            c=json.loads((self.case/'native'/run/'project.json').read_text(encoding='utf-8'))['configuration']
            powers=[m['params']['lumpedThermal']['powerW'] for m in c['materials'] if m.get('params',{}).get('lumpedThermal') and m['params']['lumpedThermal']['group'] in ('tank_a','aircraft_a','city_bus')]
            self.assertTrue(powers)
            self.assertTrue(all(p>0 for p in powers) if payload else all(p==0 for p in powers),run)
    def test_source_inputs_are_case_relative_and_match_the_copies(self):
        hashes=json.loads((self.case/'source_inputs.json').read_text(encoding='utf-8'))
        self.assertTrue(hashes)
        self.assertTrue(all(not Path(name).is_absolute() for name in hashes),hashes)
        for name,expected in hashes.items():self.assertEqual(sha(self.case/name),expected,name)
    def test_resume_acceptance_uses_convergence_rules(self):
        good=dict(status='completed',nodes=192,allNodesConverged=True,maxCoupledSurfaceResidualWm2=1.999)
        self.assertEqual(acceptance(good,192),(True,[]))
        for change,needle in (({'maxCoupledSurfaceResidualWm2':2.0},'residual'),({'allNodesConverged':False},'converged'),({'nodes':191},'nodes')):
            bad=dict(good,**change);ok,issues=acceptance(bad,192)
            self.assertFalse(ok,change);self.assertIn(needle,' '.join(issues).lower(),change)
    def test_legacy_absolute_receipt_keys_are_canonicalized(self):
        project=self.case/'native'/'sunlit_powered'/'project.json'
        relative=native_inputs(project)
        key='native/sunlit_powered/project.json'
        self.assertIn(key,relative)
        legacy={str(project.resolve()):relative[key]}
        self.assertEqual(canonical_inputs(legacy,self.case),{key:relative[key]})
        moved={str(Path(r'D:\old\case')/key):relative[key]}
        self.assertEqual(canonical_inputs(moved,self.case),{key:relative[key]})
    @staticmethod
    def _edit_forcing(path,column,value):
        rows=[l.split() for l in path.read_text(encoding='utf-8').splitlines()[1:]];rows[7][column]=str(value)
        path.write_text(str(len(rows))+'\n'+''.join(' '.join(r)+'\n' for r in rows),encoding='utf-8')
    @staticmethod
    def _edit_project(path,change):
        p=json.loads(path.read_text(encoding='utf-8'));change(p['configuration']);path.write_text(json.dumps(p),encoding='utf-8')


if __name__=='__main__':unittest.main()
