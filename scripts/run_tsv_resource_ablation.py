#!/usr/bin/env python3
"""Run same-binary TSV-resource ablations on benchmark-host.

Routing completes before validation/scoring starts, so an independent scorer
never competes with a timed router. Output directories must be new. This is an
offline experiment, not an official submission or equivalence certification.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time
import traceback

SOURCE = Path(__file__).resolve().parents[1]
CACHE = (Path.home() / ".cache" / "eda-official-evaluations")
VARIANTS = {'B0': (0, 0, 0), 'A1': (1, 0, 0),
            'A2': (1, 1, 0), 'A3': (1, 1, 1)}


def utc():
    return datetime.now(timezone.utc).isoformat()


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def files_hash(paths):
    return {str(path): sha(path) for path in sorted(set(paths))}


def source_hashes():
    files = [SOURCE / 'CMakeLists.txt', SOURCE / 'AGENTS.md']
    for rel in ('src', 'scripts', 'tests', 'tools/scorer_20261027'):
        files.extend(p for p in (SOURCE / rel).rglob('*')
                     if p.is_file() and '__pycache__' not in p.parts)
    return {str(p.relative_to(SOURCE)): sha(p) for p in sorted(set(files))}


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def finite(value):
    return isinstance(value, (float, int)) and not isinstance(value, bool) and math.isfinite(value)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--job-root', type=Path, default=SOURCE.parent)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--budget', type=float, default=1800)
    parser.add_argument('--variants', nargs='+', choices=tuple(VARIANTS), default=['A3', 'B0'])
    parser.add_argument('--cases', nargs='+', choices=('001', '003', '005'), default=['001', '003', '005'])
    parser.add_argument('--workers', type=int, choices=(1, 2, 3, 6, 9, 12), default=2)
    parser.add_argument('--evaluation-workers', type=int, choices=(1, 2, 3), default=2)
    parser.add_argument('--case-root', type=Path, default=CACHE / 'full-suite-20261018-04ab4298/cases')
    parser.add_argument('--runtime', type=Path, default=CACHE / 'community040-fixed-20261024-341df8d5/submission/runtime/official-flute')
    parser.add_argument('--dependency-root', type=Path, default=CACHE / 'community042-audit-dff686f959/package')
    args = parser.parse_args(argv)
    if not math.isfinite(args.budget) or not 30 <= args.budget <= 43200:
        parser.error('--budget must be finite and between 30 and 43200 seconds')
    if len(args.cases) != len(set(args.cases)) or len(args.variants) != len(set(args.variants)):
        parser.error('cases and variants must not contain duplicates')
    return args


class Experiment:
    def __init__(self, args):
        self.args = args
        self.job = args.job_root.resolve()
        self.out = args.output_dir.resolve()
        self.binary = self.job / 'build-release/bin/eda_router'
        self.validator = self.job / 'build-release/bin/eda_validate_guide'
        self.runtime = args.runtime.resolve()
        self.package = args.dependency_root.resolve()
        self.lock = threading.RLock()
        if os.uname().nodename != 'benchmark-host':
            raise ValueError('This experiment must execute on benchmark-host')
        if self.out.exists():
            raise ValueError('Output directory already exists; choose a new path')
        if self.out == Path(self.out.anchor) or SOURCE == self.out or SOURCE.is_relative_to(self.out):
            raise ValueError('Output must be a dedicated child directory, not a source ancestor')
        for f in (self.binary, self.validator, SOURCE / 'scripts/run_v9.sh',
                  SOURCE / 'scripts/score_20261027.py', SOURCE / 'scripts/audit_guide_geometry.py'):
            if not f.is_file():
                raise ValueError(f'Missing program: {f}')
        if not self.runtime.is_dir() or not self.package.is_dir():
            raise ValueError('Existing runtime and scorer dependency directories are required')
        self.inputs = {}
        for case in args.cases:
            folder = args.case_root.resolve() / ('open_3d_case' + case)
            found = {}
            for key, pattern in (('def', '*.input.def'), ('lef', '*.input.lef'),
                                 ('thermal', '*.thermal.input.json')):
                matches = sorted(p.resolve() for p in folder.glob(pattern) if p.is_file())
                if (key != 'lef' and len(matches) != 1) or not matches:
                    raise ValueError(f'{folder}: ambiguous or missing {key}')
                found[key] = matches
            self.inputs[case] = found
        paths = [p for case in self.inputs.values() for values in case.values() for p in values]
        self.inputs_before = files_hash(paths)
        self.sources_before = source_hashes()
        self.programs_before = files_hash([self.binary, self.validator])
        self.runtime_before = files_hash(p for p in self.runtime.rglob('*') if p.is_file())
        self.out.mkdir(parents=True, exist_ok=False)
        self.state = {'schema_version': 1, 'status': 'PREPARING', 'started_utc': utc(),
                      'host': os.uname().nodename, 'pid': os.getpid(), 'source': str(SOURCE),
                      'output_dir': str(self.out), 'budget_seconds': args.budget,
                      'workers': args.workers, 'evaluation_workers': args.evaluation_workers, 'variants': {v: list(VARIANTS[v]) for v in args.variants},
                      'cases': args.cases, 'internal_thermal_gain': 20,
                      'external_thermal_gain': 'scorer archived-content identity, explicitly unverified',
                      'official_platform_submission': False, 'official_equivalence': False,
                      'inputs_before': self.inputs_before, 'source_files_before': self.sources_before,
                      'programs_before': self.programs_before, 'runtime_before': self.runtime_before,
                      'package': str(self.package), 'runs': {}}
        for variant in args.variants:
            for case in args.cases:
                name = case + '-' + variant
                folder = self.out / name
                folder.mkdir()
                self.state['runs'][name] = {'case': case, 'variant': variant,
                                           'status': 'PENDING', 'folder': str(folder), 'steps': {}}
        self.publish()

    def publish(self):
        with self.lock:
            tmp = self.out / 'status.tmp'
            tmp.write_text(json.dumps(self.state, indent=2, allow_nan=False) + '\n', encoding='utf-8')
            tmp.replace(self.out / 'status.json')

    def update(self, record, **kwargs):
        with self.lock:
            record.update(kwargs)
            self.publish()

    @staticmethod
    def environment(route=False, runtime=None, binary=None):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('PYTHON', 'LD_'))}
        env.update(OMP_NUM_THREADS='1', OPENBLAS_NUM_THREADS='1', PYTHONDONTWRITEBYTECODE='1')
        if route:
            env['LD_LIBRARY_PATH'] = str(runtime.parent.parent / 'lib') + ':' + str(runtime)
            env['EDA_ROUTER_BINARY'] = str(binary)
            env['EDA_OFFICIAL_RUNTIME'] = str(runtime)
        return env

    def execute(self, name, step, command, timeout, env, allowed=(0,)):
        record = self.state['runs'][name]
        folder = Path(record['folder'])
        log = folder / (step + '.log')
        entry = {'command': list(map(str, command)), 'status': 'RUNNING',
                 'timeout_seconds': timeout, 'started_utc': utc(), 'log': str(log)}
        with self.lock:
            record['steps'][step] = entry
            record['active_step'] = step
            self.publish()
        started = time.monotonic()
        process = None
        try:
            with log.open('x', encoding='utf-8') as stream:
                process = subprocess.Popen(entry['command'], cwd=folder, env=env,
                                           stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    rc = process.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
                    entry['timeout'] = True
                    rc = process.returncode
            elapsed = time.monotonic() - started
            with self.lock:
                entry.update(returncode=rc, seconds=elapsed,
                             finished_utc=utc(), status='PASS' if rc in allowed and not entry.get('timeout') else 'FAIL')
                self.publish()
            if entry['status'] != 'PASS':
                raise RuntimeError(f'{name} {step} failed (rc={rc}); see {log}')
            return entry
        except Exception as error:
            with self.lock:
                if entry['status'] == 'RUNNING':
                    entry.update(status='FAIL', returncode=process.poll() if process else None,
                                 seconds=time.monotonic() - started, finished_utc=utc(), error=str(error))
                    self.publish()
            raise
        finally:
            if process is not None and process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()

    def run_route(self, name):
        record = self.state['runs'][name]
        folder = Path(record['folder'])
        case = self.inputs[record['case']]
        flags = VARIANTS[record['variant']]
        self.update(record, status='ROUTING')
        command = ['bash', SOURCE / 'scripts/run_v9.sh', '--time-limit', str(self.args.budget),
                   '--thermal-gain', '20', '--def', case['def'][0], '--thermal-report', case['thermal'][0],
                   '--output', folder / 'result.guide', '--stats', folder / 'stats.json']
        for lef in case['lef']:
            command += ['--lef', lef]
        for flag, value in zip(('--tsv-track-demand', '--tsv-landing-pricing', '--tsv-landing-rrr'), flags):
            command += [flag, str(value)]
        self.execute(name, 'route', command, self.args.budget + 300,
                     self.environment(True, self.runtime, self.binary))
        guide = folder / 'result.guide'
        stats = read_json(folder / 'stats.json')
        if not guide.is_file() or not guide.stat().st_size:
            raise ValueError(f'{name}: no nonempty exported Guide')
        measured = stats.get('metrics', {})
        if not all(finite(measured.get(k)) for k in ('wirelength_score', 'congestion_score', 'thermal_score', 'correlation')):
            raise ValueError(f'{name}: incomplete router metrics')
        self.update(record, status='ROUTED', active_step=None, guide_sha256=sha(guide),
                    stats_sha256=sha(folder / 'stats.json'), router_metrics=measured,
                    counters=stats.get('counters'), options=stats.get('options'),
                    wirelength_breakdown={k: v for k, v in stats.get('wirelength_breakdown', {}).items() if k != 'nets'},
                    initial_metrics=stats.get('initial_metrics'),
                    rules_contract=stats.get('rules_contract'),
                    thermal_gain=stats.get('thermal_gain'))
        print(name, 'ROUTED', round(record['steps']['route']['seconds'], 2), flush=True)

    def run_evaluation(self, name):
        record = self.state['runs'][name]
        if record['status'] != 'ROUTED':
            return
        folder = Path(record['folder'])
        case = self.inputs[record['case']]
        guide = folder / 'result.guide'
        env = self.environment(True, self.runtime, self.binary)
        self.update(record, status='VALIDATING')
        command = [self.validator, '--model', 'official', '--score-profile', '20261020',
                   '--tsv-resources', 'global-grid', '--def', case['def'][0],
                   '--thermal', case['thermal'][0], '--guide', guide, '--report', folder / 'engineering.json']
        for lef in case['lef']:
            command += ['--lef', lef]
        self.execute(name, 'engineering', command, 1800, env)
        engineering = read_json(folder / 'engineering.json')
        if engineering.get('status') != 'PASS':
            raise ValueError(f'{name}: engineering validation did not pass')
        command = [sys.executable, SOURCE / 'scripts/audit_guide_geometry.py',
                   '--def', case['def'][0], '--guide', guide, '--report', folder / 'geometry.json']
        for lef in case['lef']:
            command += ['--lef', lef]
        self.execute(name, 'geometry', command, 1800, self.environment(), allowed=(0, 2))
        geometry = read_json(folder / 'geometry.json')
        if geometry.get('summary', {}).get('errors') != 0:
            raise ValueError(f'{name}: geometry audit errors')
        self.update(record, status='SCORING', engineering_report=engineering,
                    geometry_report=geometry)
        command = [sys.executable, SOURCE / 'scripts/score_20261027.py', guide,
                   '--input-def', case['def'][0], '--thermal-json', case['thermal'][0],
                   '--output-dir', folder / 'score', '--dependency-root', self.package]
        command += ['--lefs'] + case['lef']
        self.execute(name, 'score', command, 1800, self.environment())
        report = read_json(folder / 'score/evaluation_report.json')
        if not report.get('valid_score') or report.get('evaluation_outcome') != 'SCORED':
            raise ValueError(f'{name}: scorer did not produce a valid score')
        score = report.get('score', {})
        if not all(finite(score.get(k)) for k in ('wirelength_score', 'congestion_score', 'thermal_score', 'quality')):
            raise ValueError(f'{name}: nonfinite or incomplete score')
        rho = report.get('thermal_tsv', {}).get('correlation_above_ambient')
        if not finite(rho):
            raise ValueError(f'{name}: external raw correlation missing')
        t20 = 50 * (1 + math.tanh(20 * rho))
        row = {'name': name, 'case': record['case'], 'variant': record['variant'],
               'W': score['wirelength_score'], 'C': score['congestion_score'],
               'T': score['thermal_score'], 'S': score['quality'], 'rho': rho,
               'Q': (100 / score['congestion_score']) ** 2 - 1,
               'T_gain20': t20, 'S_gain20': .45 * score['wirelength_score'] + .30 * score['congestion_score'] + .25 * t20,
               'overflow': report.get('congestion', {}).get('total_overflow'),
               'route_seconds': record['steps']['route']['seconds'],
               'score_seconds': record['steps']['score']['seconds'],
               'guide_sha256': record['guide_sha256'],
               'geometry_unresolved': geometry.get('summary', {}).get('unresolved')}
        if sha(guide) != record['guide_sha256'] or sha(folder / 'stats.json') != record['stats_sha256']:
            raise ValueError(f'{name}: Guide or stats changed during evaluation')
        self.update(record, status='COMPLETE', active_step=None, report=report, row=row)
        print(name, 'COMPLETE', 'S=', round(row['S'], 6), flush=True)

    def guarded(self, function, name):
        try:
            function(name)
        except Exception as error:
            self.update(self.state['runs'][name], status='ERROR', active_step=None,
                        error=str(error), traceback=traceback.format_exc())
            print(name, 'ERROR', str(error), flush=True)

    def phase(self, function, names, workers):
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futures = [pool.submit(self.guarded, function, name) for name in names]
            for future in as_completed(futures):
                future.result()

    def run(self):
        try:
            names = list(self.state['runs'])
            self.update(self.state, status='ROUTING')
            self.phase(self.run_route, names, self.args.workers)
            self.update(self.state, status='EVALUATING')
            self.phase(self.run_evaluation, names, self.args.evaluation_workers)
            inputs_after = files_hash(Path(p) for p in self.inputs_before)
            sources_after = source_hashes()
            programs_after = files_hash(Path(p) for p in self.programs_before)
            runtime_after = files_hash(Path(p) for p in self.runtime_before)
            integrity = {'inputs_unchanged': inputs_after == self.inputs_before,
                         'source_unchanged': sources_after == self.sources_before,
                         'programs_unchanged': programs_after == self.programs_before,
                         'runtime_unchanged': runtime_after == self.runtime_before}
            rows = [r['row'] for r in self.state['runs'].values() if r['status'] == 'COMPLETE']
            complete = len(rows) == len(names) and all(integrity.values())
            self.update(self.state, status='COMPLETE' if complete else 'ERROR',
                        finished_utc=utc(), integrity=integrity, rows=rows,
                        all_requested_runs_completed=complete)
            summary = {'status': self.state['status'], 'budget_seconds': self.args.budget,
                       'official_equivalence': False, 'integrity': integrity, 'rows': rows,
                       'means': {v: {key: sum(r[key] for r in rows if r['variant'] == v) / len(self.args.cases)
                                      for key in ('W', 'C', 'T', 'S', 'route_seconds')}
                                 for v in self.args.variants
                                 if sum(r['variant'] == v for r in rows) == len(self.args.cases)}}
            (self.out / 'summary.json').write_text(json.dumps(summary, indent=2, allow_nan=False) + '\n')
            table = ['| case | variant | W | C | T | S | rho | route s | score s |',
                     '|---|---|---:|---:|---:|---:|---:|---:|---:|']
            for r in sorted(rows, key=lambda r: (r['case'], r['variant'])):
                table.append('| {case} | {variant} | {W:.4f} | {C:.4f} | {T:.4f} | {S:.4f} | {rho:.7f} | {route_seconds:.2f} | {score_seconds:.2f} |'.format(**r))
            (self.out / 'summary.md').write_text('\n'.join(table) + '\n')
            return 0 if complete else 1
        except Exception as error:
            self.update(self.state, status='ERROR', error=str(error),
                        traceback=traceback.format_exc(), finished_utc=utc())
            raise


def main(argv=None):
    args = parse_args(argv)
    try:
        return Experiment(args).run()
    except Exception as error:
        print('Experiment ERROR:', str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
