#!/usr/bin/env python3
"""Synthetic process tests of wrapper behavior, not official-score quality tests."""
from contextlib import redirect_stdout, redirect_stderr
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("evaluate_official", ROOT / "scripts/evaluate_official.py")
wrapper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wrapper)

FAKE_SCORE = """#!/usr/bin/env python3
import json, pathlib, sys, time
mode = MODE
arguments = sys.argv[1:]
if mode == 'timeout':
    time.sleep(5)
input_def = pathlib.Path(arguments[arguments.index('--input-def') + 1])
(input_def.parent / 'net_to_die.json').write_text('{}')
output = pathlib.Path(arguments[arguments.index('--output-dir') + 1])
output.mkdir()
if mode == 'missing':
    sys.exit(0)
if mode == 'malformed':
    (output / 'evaluation_report.json').write_text('broken')
    sys.exit(0)
report = {'status': 'PASS', 'score': {
    'wirelength_score': 70.0, 'congestion_score': 80.0, 'thermal_score': 90.0,
    'weights': {'wirelength': .45, 'congestion': .30, 'thermal_tsv': .25}}}
if mode == 'fail':
    report = {'status': 'FAIL', 'score': 0.0, 'rule': 'synthetic failure'}
(output / 'evaluation_report.json').write_text(json.dumps(report))
if mode == 'tamper':
    (pathlib.Path(__file__).parent / 'README.md').write_text('tampered by synthetic scorer')
sys.exit(1 if mode in ('fail', 'nonzero') else 0)
"""


class WrapperTests(unittest.TestCase):
    def setUp(self):
        work = ROOT / "output" / "wrapper-tests"
        work.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="official-", dir=work)
        self.root = Path(self.temporary.name)
        self.package = self.root / "package"
        self.package.mkdir()
        for name in ("POST9.dat", "POWV9.dat", "README.md"):
            (self.package / name).write_bytes(name.encode())
        self.sources = self.root / "sources"
        self.sources.mkdir()
        self.inputs = {name: self.sources / name for name in ("guide", "lef", "def", "thermal")}
        for name, path in self.inputs.items():
            path.write_bytes(('synthetic-' + name).encode())
        self.output = self.root / "output"

    def tearDown(self):
        self.temporary.cleanup()

    def run_wrapper(self, mode='pass', extra=()):
        binary = self.package / "score"
        binary.write_text(FAKE_SCORE.replace('MODE', repr(mode)))
        binary.chmod(0o755)
        before = wrapper.package_hashes(self.package)
        args = [str(self.inputs['guide']), '--score-dir', str(self.package),
                '--lefs', str(self.inputs['lef']), '--input-def', str(self.inputs['def']),
                '--thermal-json', str(self.inputs['thermal']), '--output-dir', str(self.output),
                *extra]
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            result = wrapper.main(args)
        manifest = json.loads((self.output / 'run_manifest.json').read_text())
        return result, manifest, before

    def test_pass_copies_inputs_and_keeps_sources_unchanged(self):
        result, manifest, before = self.run_wrapper(extra=('--via-cost-ratio', '1.5'))
        self.assertEqual(result, 0)
        self.assertEqual(manifest['status'], 'PASS')
        self.assertEqual(wrapper.package_hashes(self.package), before)
        self.assertTrue(manifest['source_inputs_unchanged'])
        self.assertEqual(sorted(p.name for p in self.sources.iterdir()), sorted(self.inputs))
        for record in manifest['input_copies']:
            self.assertEqual(wrapper.sha256(Path(record['copy'])), record['sha256'])
            self.assertNotEqual(record['copy'], record['source'])
        self.assertEqual(manifest['summary']['weighted_score_derived_from_official_components'], 78)
        self.assertIn('--via-cost-ratio', manifest['execution']['command'])
        self.assertTrue(any(self.output.rglob('net_to_die.json')))

    def test_official_fail_and_zero_score_remain_fail(self):
        result, manifest, _ = self.run_wrapper('fail')
        self.assertEqual(result, 1)
        self.assertEqual(manifest['status'], 'FAIL')
        self.assertEqual(manifest['summary']['official_score'], 0)

    def test_nonzero_process_is_never_pass(self):
        result, manifest, _ = self.run_wrapper('nonzero')
        self.assertEqual(result, 1)
        self.assertEqual(manifest['status'], 'FAIL')
        self.assertEqual(manifest['summary']['official_status'], 'PASS')

    def test_timeout_is_recorded_and_process_killed(self):
        result, manifest, _ = self.run_wrapper('timeout', ('--timeout', '0.05'))
        self.assertEqual(result, 1)
        self.assertEqual(manifest['status'], 'TIMEOUT')
        self.assertTrue(manifest['execution']['timeout'])
        self.assertLess(manifest['execution']['seconds'], 2)

    def test_missing_report_is_error(self):
        result, manifest, _ = self.run_wrapper('missing')
        self.assertEqual(result, 1)
        self.assertEqual(manifest['status'], 'ERROR')

    def test_malformed_report_is_error(self):
        result, manifest, _ = self.run_wrapper('malformed')
        self.assertEqual(result, 1)
        self.assertEqual(manifest['status'], 'ERROR')
        self.assertTrue(manifest['official_package_unchanged'])

    def test_detects_package_mutation(self):
        result, manifest, _ = self.run_wrapper('tamper')
        self.assertEqual(result, 1)
        self.assertEqual(manifest['status'], 'INTEGRITY_FAIL')
        self.assertEqual(manifest['changed_official_files'], ['README.md'])

    def test_protected_output_path_is_rejected(self):
        binary = self.package / 'score'
        binary.write_text(FAKE_SCORE.replace('MODE', repr('pass')))
        binary.chmod(0o755)
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            result = wrapper.main([str(self.inputs['guide']), '--score-dir', str(self.package),
                '--lefs', str(self.inputs['lef']), '--input-def', str(self.inputs['def']),
                '--thermal-json', str(self.inputs['thermal']), '--output-dir', str(self.package / 'out')])
        self.assertEqual(result, 1)
        self.assertFalse((self.package / 'out').exists())


if __name__ == '__main__':
    unittest.main()
