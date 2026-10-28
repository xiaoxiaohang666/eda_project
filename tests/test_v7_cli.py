#!/usr/bin/env python3
"""Exercise V7 option contracts and full serialized quality/legality integration."""
import json,os
from pathlib import Path
import subprocess,sys,tempfile,unittest
from test_official_cli import ASSETS,design,technology
ROUTER,SCORER,VALIDATOR,RUNTIME=map(lambda p:Path(p).resolve(),sys.argv[1:5]);del sys.argv[1:5]
RUN_V7=Path(__file__).resolve().parents[1]/'scripts/run_v7.sh'
class V7CliTests(unittest.TestCase):
    def test_invalid_options(self):
        base=['--model','official','--score-profile','20261020']
        cases=[(['--v7-rrr','1'],'require --model official'),
               (['--v7-topology','1'],'require --model official'),
               (base+['--v7-rrr','1'],'requires --rrr-score-aware'),
               (base+['--v7-rrr-net-limit','32'],'budgets require'),
               (base+['--v7-rrr-astar-seconds','nan'],'must be'),
               (base+['--v7-tsv','1'],'requires --tsv-resource-mode'),
               (base+['--v7-tsv-seconds','10'],'budgets require'),
               (base+['--resource-profile','invalid'],'invalid resource profile'),
               (['--resource-profile','platform-20261024'],'require --model official')]
        for flags,error in cases:
            with self.subTest(flags=flags):
                p=subprocess.run([str(ROUTER),*flags],capture_output=True,text=True,timeout=10)
                self.assertNotEqual(p.returncode,0);self.assertIn(error,p.stderr)
    def test_full_v7_serialized_roundtrip(self):
        if any(not(RUNTIME/a).is_file() for a in ASSETS):self.skipTest('FLUTE unavailable')
        with tempfile.TemporaryDirectory(prefix='v7 cli ') as temp:
            d=Path(temp);lef=d/'case input.lef';ddef=d/'case input.def';thermal=d/'case heat.json'
            lef.write_text(technology());ddef.write_text(design())
            thermal.write_text(json.dumps({'meta':{'ambientTemp':25},'nodes':[[5+10*c,5+10*r,0,30+2*c+r] for r in range(4) for c in range(4)]}))
            # Exercise quoted, relative environment paths and caller-relative I/O.
            (d/'router binary').symlink_to(ROUTER)
            (d/'runtime assets').symlink_to(RUNTIME,target_is_directory=True)
            env=dict(os.environ,EDA_ROUTER_BINARY='./router binary',EDA_OFFICIAL_RUNTIME='./runtime assets')
            guide=d/'result guide.guide';stats=d/'stats.json'
            def run(cmd):
                p=subprocess.run(list(map(str,cmd)),cwd=d,env=env,capture_output=True,text=True,timeout=60)
                self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            def unique_pairs(pairs):
                result={}
                for k,v in pairs:
                    self.assertNotIn(k,result,'duplicate JSON key');result[k]=v
                return result
            def load_stats(path):
                return json.loads(path.read_text(),object_pairs_hook=unique_pairs)
            common=[RUN_V7,'--def',ddef.name,'--lef',lef.name,'--thermal-json',thermal.name]
            run(common+['--output','preset guide.guide','--stats','preset.json'])
            preset=load_stats(d/'preset.json')
            expected={'time_limit_seconds':1800,'rrr_rounds':8,'rrr_net_limit':512,
                      'rrr_score_aware':True,'official_score_pricing':True,'v7_rrr':True,
                      'v7_rrr_astar_net_limit':32,'v7_rrr_astar_seconds':.03,
                      'v7_tsv':True,'v7_tsv_nets':512,'v7_tsv_candidates':6,'v7_tsv_seconds':45,
                      'v7_cell_cost':True,'v7_topology':True}
            for key,value in expected.items():
                self.assertEqual(preset['options'][key],value,key)
            self.assertEqual(preset['resource_profile'],'documented')
            self.assertEqual(preset['scoring_model'],'documented-20261020')
            self.assertEqual(preset['policy']['pairing'],'global-grid')
            for key,value in {'assignment_polish_heat_weight':2,'assignment_polish_candidates':32,
                              'assignment_polish_passes_limit':8,'assignment_polish_bidirectional':True}.items():
                self.assertEqual(preset['policy'][key],value,key)
            # Reuse the same full roundtrip with tail overrides, including stage
            # ablations; do not duplicate validator/scorer integration cases.
            run(common+['--output',guide.name,'--stats',stats.name,
                        '--time-limit','20','--rrr-rounds','0','--tsv-rounds','0',
                        '--v7-rrr-astar-net-limit','2','--v7-rrr-astar-expansions','8',
                        '--v7-rrr-astar-seconds','0.001','--v7-tsv-nets','2',
                        '--v7-tsv-candidates','2','--v7-tsv-seconds','1',
                        '--v7-cell-cost','0','--v7-topology','0'])
            routed=load_stats(stats)
            overridden={'time_limit_seconds':20,'rrr_rounds':0,'tsv_rounds':0,
                        'v7_rrr_astar_net_limit':2,'v7_rrr_astar_expansions':8,
                        'v7_rrr_astar_seconds':.001,'v7_tsv_nets':2,'v7_tsv_candidates':2,
                        'v7_tsv_seconds':1,'v7_cell_cost':False,'v7_topology':False}
            for key,value in overridden.items():
                self.assertEqual(routed['options'][key],value,key)
            print('run_v7 effective options: '+json.dumps({
                'preset':{k:preset['options'][k] for k in expected},
                'overrides':{k:routed['options'][k] for k in overridden}},sort_keys=True))
            run([VALIDATOR,'--def',ddef,'--lef',lef,'--thermal',thermal,'--guide',guide,'--model','official',
                 '--score-profile','20261020','--tsv-resources','global-grid','--report',d/'validation.json'])
            run([SCORER,guide,'--input-def',ddef,'--lefs',lef,'--thermal-json',thermal,'--official-runtime',RUNTIME,
                 '--pin-layer-policy','shape','--deduplicate-tracks','1','--thermal-gain','20','--output-dir',d/'score','--no-csv'])
            scored=json.loads((d/'score/evaluation_report.json').read_text())
            self.assertEqual(scored['status'],'PASS')
            for name in ['wirelength_score','congestion_score','thermal_score']:
                self.assertAlmostEqual(routed['metrics'][name],scored['score'][name],places=7)
            for result in [preset,routed]:
                self.assertGreaterEqual(result['metrics']['quality']+1e-9,result['initial_metrics']['quality'])
                self.assertFalse(result.get('official_scorer_run',False))
if __name__=='__main__':unittest.main()
