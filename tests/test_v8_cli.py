#!/usr/bin/env python3
"""V8 contracts and serialized Guide score roundtrip."""
import json,subprocess,sys,tempfile,os,unittest
from pathlib import Path
from test_official_cli import ASSETS,design,technology
ROUTER,SCORER,VALIDATOR,RUNTIME=map(lambda p:Path(p).resolve(),sys.argv[1:5]);del sys.argv[1:5]
ROOT=Path(__file__).resolve().parents[1]
class V8Tests(unittest.TestCase):
 def test_invalid(self):
  base=['--model','official','--score-profile','20261020']
  cases=[(['--v8-rrr','1'],'requires --model'),
   (base+['--v8-rrr','1'],'requires --rrr-score-aware'),
   (base+['--v8-recovery','1'],'requires --large-recovery'),
   (base+['--v8-tsv-dynamic','1'],'requires --tsv-resource-mode'),
   (base+['--v8-tsv-rematch-group-size','9'],'must be 2..8'),
   (base+['--congestion-price-cap','nan'],'out of range'),
   (base+['--score-unit-cost','1'],'require --official-score-pricing'),
   (base+['--v8-tsv-probe-candidates','2'],'probe budget requires'),
   (base+['--v8-tsv-rematch-seconds','1'],'rematch budgets require')]
  for flags,error in cases:
   with self.subTest(flags=flags):
    p=subprocess.run([str(ROUTER),*flags],capture_output=True,text=True,timeout=10)
    self.assertNotEqual(p.returncode,0);self.assertIn(error,p.stderr)
 def test_roundtrip(self):
  if any(not(RUNTIME/a).is_file() for a in ASSETS):self.skipTest('FLUTE unavailable')
  with tempfile.TemporaryDirectory(prefix='v8 cli ') as tmp:
   d=Path(tmp);lef=d/'input.lef';df=d/'input.def';heat=d/'heat.json'
   lef.write_text(technology());df.write_text(design())
   heat.write_text(json.dumps({'meta':{'ambientTemp':25},'nodes':[[5+10*c,5+10*r,0,30+2*c+r] for r in range(4) for c in range(4)]}))
   env=dict(os.environ,EDA_ROUTER_BINARY=str(ROUTER),EDA_OFFICIAL_RUNTIME=str(RUNTIME))
   def run(cmd):
    p=subprocess.run(list(map(str,cmd)),cwd=d,env=env,capture_output=True,text=True,timeout=90)
    self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   def unique(pairs):
    out={}
    for k,v in pairs:self.assertNotIn(k,out);out[k]=v
    return out
   for score_units in (0,1):
    guide=d/f'route{score_units}.guide';stats=d/f'stats{score_units}.json'
    run(['bash',ROOT/'scripts/run_v8.sh','--def',df,'--lef',lef,'--thermal-json',heat,
     '--output',guide,'--stats',stats,'--time-limit','15','--rrr-rounds','2',
     '--large-recovery-seconds','1','--v8-tsv-rematch-seconds','1','--v7-tsv-seconds','1',
     '--score-unit-cost',score_units,'--congestion-price-cap','100000'])
    data=json.loads(stats.read_text(),object_pairs_hook=unique)
    for key in ('v8_rrr','v8_tsv_dynamic','v8_tsv_rematch','v8_recovery','unified_budget'):
     self.assertTrue(data['options'][key],key)
    self.assertEqual(data['options']['score_unit_cost'],bool(score_units))
    self.assertEqual(data['options']['congestion_price_cap'],100000)
    self.assertGreaterEqual(data['metrics']['quality']+1e-10,data['initial_metrics']['quality'])
    run([VALIDATOR,'--def',df,'--lef',lef,'--thermal',heat,'--guide',guide,
     '--model','official','--score-profile','20261020','--tsv-resources','global-grid','--report',d/f'validation{score_units}.json'])
    run([SCORER,guide,'--input-def',df,'--lefs',lef,'--thermal-json',heat,'--official-runtime',RUNTIME,
     '--resource-profile','platform-20261024','--output-dir',d/f'score{score_units}','--no-csv'])
    scored=json.loads((d/f'score{score_units}/evaluation_report.json').read_text())
    self.assertEqual(scored['status'],'PASS')
    for k in ('wirelength_score','congestion_score','thermal_score'):self.assertAlmostEqual(data['metrics'][k],scored['score'][k],places=7)
if __name__=='__main__':unittest.main()
