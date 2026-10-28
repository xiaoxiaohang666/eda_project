#!/usr/bin/env python3
"""Documented V9 integration, budget scheduling and cost decomposition."""
import json,os,subprocess,sys,tempfile,unittest
from pathlib import Path
from test_official_cli import ASSETS,design,technology
ROUTER,SCORER,VALIDATOR,RUNTIME=map(lambda p:Path(p).resolve(),sys.argv[1:5]);del sys.argv[1:5]
ROOT=Path(__file__).resolve().parents[1]
class V9Tests(unittest.TestCase):
 def test_invalid(self):
  base=['--model','official','--unified-budget','1']
  cases=[(['--v9-incremental-rrr','1'],'v9 requires'),
   (base+['--v9-incremental-rrr','1'],'requires --v8-rrr'),
   (base+['--v9-subtree-recovery','1'],'requires --large-recovery'),
   (base+['--v9-access-rematch','1'],'requires --v8-tsv-rematch'),
   (base+['--v9-tsv-coverage','1'],'requires --v8-tsv-dynamic'),
   (base+['--v9-checkpoint-groups','257'],'out of range'),
   (base+['--v9-checkpoint-seconds','nan'],'out of range'),
   (base+['--v9-rematch-edges','12'],'budget requires'),
   (base+['--v9-rematch-access-seconds','nan'],'out of range'),
   (base+['--v9-rematch-access-seconds','.1'],'budget requires'),
   (base+['--v9-subtree-boundary','16'],'budgets require')]
  for flags,error in cases:
   with self.subTest(flags=flags):
    p=subprocess.run([str(ROUTER),*flags],capture_output=True,text=True,timeout=10)
    self.assertNotEqual(p.returncode,0);self.assertIn(error,p.stderr)
 def test_schedules_serialized_geometry_and_costs(self):
  if any(not(RUNTIME/a).is_file() for a in ASSETS):self.skipTest('FLUTE unavailable')
  with tempfile.TemporaryDirectory(prefix='v9 cli ') as tmp:
   d=Path(tmp);lef=d/'input.lef';df=d/'input.def';heat=d/'heat.json'
   lef.write_text(technology());df.write_text(design())
   heat.write_text(json.dumps({'meta':{'ambientTemp':25},'nodes':[[5+10*c,5+10*r,0,30+2*c+r] for r in range(4) for c in range(4)]}))
   env=dict(os.environ,EDA_ROUTER_BINARY=str(ROUTER),EDA_OFFICIAL_RUNTIME=str(RUNTIME))
   def run(cmd):
    p=subprocess.run(list(map(str,cmd)),cwd=d,env=env,capture_output=True,text=True,timeout=90)
    self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   for adaptive in (0,1):
    guide=d/f'{adaptive}.guide';stats=d/f'{adaptive}.json'
    run(['bash',ROOT/'scripts/run_v9.sh','--def',df,'--lef',lef,'--thermal-json',heat,'--output',guide,'--stats',stats,
     '--time-limit','10','--thermal-gain','20','--rrr-rounds','2','--v9-adaptive-budget',adaptive,
     '--large-recovery-seconds','1','--v8-tsv-rematch-seconds','1','--v7-tsv-seconds','1'])
    data=json.loads(stats.read_text());self.assertEqual(data['rules_contract'],'documented-rules-v1')
    for name in ('v9_incremental_rrr','v9_subtree_recovery','v9_access_rematch','v9_tsv_coverage','v9_thermal_regions'):
     self.assertTrue(data['options'][name])
    self.assertEqual(data['options']['v9_adaptive_budget'],bool(adaptive))
    self.assertEqual(data['options']['budget_origin'],'process_start')
    costs=data['wirelength_breakdown'];self.assertEqual(len(costs['nets']),2)
    self.assertAlmostEqual(costs['total_dbu'],sum(costs[k] for k in ('planar_dbu','ordinary_via_dbu','tsv_dbu')),places=7)
    self.assertAlmostEqual(costs['total_dbu'],data['metrics']['guide_mst_dbu'],places=7)
    for row in costs['nets']:
     self.assertAlmostEqual(row['total_dbu'],row['planar_dbu']+row['ordinary_via_dbu']+row['tsv_dbu'],places=7)
    self.assertGreaterEqual(data['metrics']['quality']+1e-9,data['initial_metrics']['quality'])
    if adaptive:
     for name in ('v9_rrr_budget','v9_tsv_budget','v9_rematch_budget','v9_recovery_budget'):
      self.assertGreater(data['counters'][name],0)
    run([VALIDATOR,'--def',df,'--lef',lef,'--thermal',heat,'--guide',guide,'--model','official',
      '--score-profile','20261020','--tsv-resources','global-grid','--report',d/f'valid{adaptive}.json'])
    run([SCORER,guide,'--input-def',df,'--lefs',lef,'--thermal-json',heat,'--official-runtime',RUNTIME,
     '--pin-layer-policy','shape','--deduplicate-tracks','1','--thermal-gain','20','--no-csv','--output-dir',d/f'score{adaptive}'])
    scored=json.loads((d/f'score{adaptive}/evaluation_report.json').read_text())
    self.assertEqual(scored['status'],'PASS')
    for name in ('wirelength_score','congestion_score','thermal_score'):
     self.assertAlmostEqual(data['metrics'][name],scored['score'][name],places=7)
if __name__=='__main__':unittest.main()
