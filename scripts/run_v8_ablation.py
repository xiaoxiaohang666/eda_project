#!/usr/bin/env python3
"""Same-binary, same-ceiling serial V8 experiments. Never submits to a platform."""
import argparse,hashlib,json,os,signal,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
VARIANTS={
 'A0':[],
 'A1':['--large-recovery','1'],
 'A1e':['--large-recovery','1','--v8-recovery','1'],
 'A2':['--large-recovery','1','--v8-tsv-rematch','1'],
 'A3':['--large-recovery','1','--v8-tsv-rematch','1','--v8-rrr','1'],
 'A4cap':['--large-recovery','1','--v8-tsv-rematch','1','--v8-rrr','1','--congestion-price-cap','100000'],
 'A4score':['--large-recovery','1','--v8-tsv-rematch','1','--v8-rrr','1','--score-unit-cost','1'],
 'A5':['--large-recovery','1','--v8-tsv-rematch','1','--v8-rrr','1','--v8-tsv-dynamic','1'],
 'V8':['--large-recovery','1','--large-recovery-nets','4096','--large-recovery-rounds','2','--v8-recovery','1','--v8-tsv-rematch','1','--v8-rrr','1','--v8-tsv-dynamic','1'],
}
def sha(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
 return h.hexdigest()
def write(p,x):p.write_text(json.dumps(x,ensure_ascii=False,indent=2,allow_nan=False)+'\n')
def run(cmd,log,timeout):
 begin=time.monotonic()
 with log.open('w') as f:
  p=subprocess.Popen(list(map(str,cmd)),cwd=ROOT,stdout=f,stderr=subprocess.STDOUT,start_new_session=True)
  try:code=p.wait(timeout=timeout)
  except subprocess.TimeoutExpired:
   os.killpg(p.pid,signal.SIGTERM)
   try:p.wait(timeout=10)
   except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
   code=124
 return {'command':list(map(str,cmd)),'exit_code':code,'seconds':time.monotonic()-begin}
def main():
 ap=argparse.ArgumentParser(description=__doc__)
 ap.add_argument('--binary',type=Path,required=True)
 ap.add_argument('--output-dir',type=Path,required=True)
 ap.add_argument('--cases',nargs='+',default=['001','003','005'],choices=['toy','001','003','005'])
 ap.add_argument('--variants',nargs='+',default=['A0','A1','A2','A3','A4cap','A4score','A5','V8'],choices=list(VARIANTS))
 ap.add_argument('--time-limit',type=float,default=1800)
 ap.add_argument('--recovery-seconds',type=float,default=60)
 ap.add_argument('--rematch-seconds',type=float,default=30)
 ap.add_argument('--tsv-seconds',type=float,default=45)
 ap.add_argument('--external-score',action='store_true')
 ap.add_argument('--resume',action='store_true')
 a=ap.parse_args();a.binary=a.binary.resolve();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
 identity={'binary':str(a.binary),'sha256':sha(a.binary),'time_limit':a.time_limit,
  'recovery_seconds':a.recovery_seconds,'rematch_seconds':a.rematch_seconds,'tsv_seconds':a.tsv_seconds}
 validator=a.binary.parent/'eda_validate_guide';records=[]
 for case in a.cases:
  name='toy' if case=='toy' else 'open_3d_case'+case
  folder=ROOT/'公开测试用例1011/公开测试用例'/name
  inputs={k:next(folder.glob(pattern)) for k,pattern in [('def','*.def'),('lef','*.lef'),('heat','*.json')]}
  hashes={k:sha(p) for k,p in inputs.items()}
  for variant in a.variants:
   d=out/(case+'-'+variant);record_path=d/'record.json'
   if d.exists():
    if not a.resume:raise RuntimeError('existing output: '+str(d))
    record=json.loads(record_path.read_text())
    if record['identity']!=identity or record['input_sha256']!=hashes or record['flags']!=VARIANTS[variant]:raise RuntimeError('resume identity mismatch')
    if record.get('complete'):records.append(record);continue
    raise RuntimeError('incomplete output requires a fresh directory: '+str(d))
   d.mkdir();guide=d/'route.guide';stats=d/'stats.json'
   flags=['--unified-budget','1','--time-limit',str(a.time_limit),'--v7-tsv-seconds',str(a.tsv_seconds)]+VARIANTS[variant]
   if variant!='A0':flags+=['--large-recovery-seconds',str(a.recovery_seconds)]
   if '--v8-tsv-rematch' in flags:flags+=['--v8-tsv-rematch-seconds',str(a.rematch_seconds)]
   command=['bash',str(ROOT/'scripts/run_v7.sh'),'--def',str(inputs['def']),'--lef',str(inputs['lef']),
    '--thermal-json',str(inputs['heat']),'--output',str(guide),'--stats',str(stats)]+flags
   # Preserve run_v7 defaults while selecting exactly one frozen executable.
   record={'case':case,'variant':variant,'identity':identity,'flags':VARIANTS[variant],'input_sha256':hashes,'complete':False}
   write(record_path,record);print('START',case,variant,flush=True)
   env=os.environ.copy();os.environ['EDA_ROUTER_BINARY']=str(a.binary)
   record['route']=run(command,d/'route.log',a.time_limit+300)
   os.environ.clear();os.environ.update(env)
   if record['route']['exit_code']==0:
    data=json.loads(stats.read_text());record['metrics']=data['metrics'];record['counters']=data['counters']
    record['recovery']=data['budgeted_wirelength_recovery'];record['guide_sha256']=sha(guide)
    record['engineering']=run([validator,'--model','official','--score-profile','20261020','--tsv-resources','global-grid',
      '--def',inputs['def'],'--lef',inputs['lef'],'--thermal',inputs['heat'],'--guide',guide,'--report',d/'engineering.json'],d/'engineering.log',600)
    record['geometry']=run(['python3',ROOT/'scripts/audit_guide_geometry.py','--def',inputs['def'],'--lef',inputs['lef'],
      '--guide',guide,'--report',d/'geometry.json'],d/'geometry.log',600)
    if a.external_score:
     record['external_score']=run([ROOT/'tools/民间评估器3/score',guide,'--input-def',inputs['def'],'--lefs',inputs['lef'],
      '--thermal-json',inputs['heat'],'--output-dir',d/'platform','--mode','platform'],d/'platform.log',1800)
     report=d/'platform/evaluation_report.json'
     if report.exists():record['external_report']=json.loads(report.read_text())
   record['inputs_unchanged']=hashes=={k:sha(p) for k,p in inputs.items()}
   record['binary_unchanged']=identity['sha256']==sha(a.binary)
   record['complete']=True;write(record_path,record);records.append(record)
   write(out/'summary.json',records)
   print('DONE',case,variant,record.get('metrics'),flush=True)
if __name__=='__main__':main()
