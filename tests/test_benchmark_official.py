#!/usr/bin/env python3
"""Process/artifact regressions using synthetic programs; no real routing."""
from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("benchmark_official", ROOT / "scripts/benchmark_official.py")
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)

ROUTER = """#!/usr/bin/env python3
import json, pathlib, sys, time
args = sys.argv[1:]
mode = MODE
if mode == 'timeout': time.sleep(5)
if mode == 'nonzero': sys.exit(7)
if mode == 'missing': sys.exit(0)
value = lambda option: args[args.index(option) + 1]
if mode == 'tamper': pathlib.Path(value('--def')).write_text('modified copy')
pathlib.Path(value('--output')).write_text('n\\n(\\n0 0 1 1 metal1\\n)\\n')
stats = {'scoring_model': 'official-2026', 'metrics': {'wirelength_score': 80,
 'congestion_score': 70, 'thermal_score': 60, 'quality': .715},
 'peak_rss_kb': 100, 'elapsed_seconds': 2, 'counters': {'rrr_accepted': 3},
 'options': {'time_limit_seconds': float(value('--time-limit'))}}
if mode == 'malformed': stats['metrics']['quality'] = 'invalid'
if mode.startswith('schema:'):
 field, replacement = mode.split(':')[1:]
 stats[field] = json.loads(replacement)
if mode == 'overflow-json': stats['policy'] = {'bad': 1e999}
pathlib.Path(value('--stats')).write_text(json.dumps(stats).replace('Infinity', '1e999'))
"""
VALIDATOR = """#!/usr/bin/env python3
import json, pathlib, sys
args = sys.argv[1:]
action = ACTION
guide = pathlib.Path(args[args.index('--guide') + 1])
if action != 'none':
 artifact, operation = action.split(':')
 target = guide if artifact == 'guide' else guide.parent / 'stats.json'
 if operation == 'delete': target.unlink()
 else: target.write_text('changed by synthetic validator')
report = pathlib.Path(args[args.index('--report') + 1])
report.write_text(json.dumps({'status': STATUS, 'checked': {'nets': 1}}))
sys.exit(CODE)
"""


class BenchmarkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="benchmark-official-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.case = self.root / "open_3d_case005"
        self.case.mkdir()
        for name in ("open_3d_case005.input.lef", "open_3d_case005.input.def",
                     "open_3d_case003.thermal.input.json"):
            (self.case / name).write_text("untouched " + name)
        self.runtime = self.root / "runtime"
        self.runtime.mkdir()
        (self.runtime / "libflute.so").write_bytes(b"synthetic library")
        self.binary = self.root / "router"
        self.validator = self.root / "validator"
        self.output = self.root / "run"
        self.baseline = self.root / "baseline.json"
        self.baseline.write_text(json.dumps({'scoring_model': 'official-2026',
            'metrics': {'wirelength_score': 70, 'congestion_score': 70, 'thermal_score': 50, 'quality': .645},
            'peak_rss_kb': 120, 'elapsed_seconds': 4, 'counters': {'rrr_accepted': 2}}))

    def executable(self, path, source):
        path.write_text(source)
        path.chmod(0o755)

    def run_benchmark(self, mode="pass", extra=(), validator_status="PASS", validator_code=0,
                      validator_action="none"):
        self.executable(self.binary, ROUTER.replace("MODE", repr(mode)))
        self.executable(self.validator, VALIDATOR.replace("STATUS", repr(validator_status))
            .replace("CODE", str(validator_code)).replace("ACTION", repr(validator_action)))
        arguments = ["--case-dir", str(self.case), "--binary", str(self.binary),
                     "--official-runtime", str(self.runtime), "--heat-weight", "2",
                     "--time-limit", "15", "--output-dir", str(self.output), *extra]
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            status = benchmark.main(arguments)
        manifest_path = self.output / "run.json"
        return status, json.loads(manifest_path.read_text()) if manifest_path.is_file() else None

    def test_freeze_input_basename_hashes_stats_comparison_and_validator(self):
        originals = benchmark.directory_hashes(self.case)
        status, manifest = self.run_benchmark(extra=("--validator", str(self.validator),
            "--baseline-stats", str(self.baseline), "--tsv-resource-mode", "projected",
            "--", "--rrr-rounds", "1"))
        self.assertEqual(status, 0)
        self.assertEqual(manifest["status"], "PASS")
        self.assertFalse(manifest["official_evaluator_run"])
        self.assertEqual(Path(manifest["command"][0]), (self.output / "eda_router").resolve())
        self.assertEqual(manifest["binary"]["source"], str(self.binary.resolve()))
        self.assertEqual(manifest["binary_sha256"], benchmark.sha256(self.binary))
        self.assertEqual(manifest["binary"]["frozen_sha256"], benchmark.sha256(self.output / "eda_router"))
        self.assertEqual(manifest["guide_sha256"], benchmark.sha256(self.output / "result.guide"))
        self.assertEqual(manifest["stats_sha256"], benchmark.sha256(self.output / "stats.json"))
        self.assertEqual(manifest["artifact_hashes_after"]["stats.json"], manifest["stats_sha256"])
        self.assertEqual(manifest["artifact_hashes_after"]["result.guide"], manifest["guide_sha256"])
        self.assertEqual(manifest["changed_artifacts"], [])
        self.assertIn("src/main.cpp", manifest["source_files_before"])
        self.assertEqual(manifest["comparison"]["metrics"]["wirelength_score"]["delta"], 10)
        self.assertAlmostEqual(manifest["comparison"]["metrics"]["quality"]["delta"], .07)
        self.assertEqual(manifest["comparison"]["resources"]["peak_rss_kb"]["delta"], -20)
        self.assertEqual(manifest["comparison"]["counters"]["rrr_accepted"]["delta"], 1)
        self.assertEqual(manifest["validation_report"]["status"], "PASS")
        self.assertIn("projected", manifest["validation_execution"]["command"])
        self.assertEqual(benchmark.directory_hashes(self.case), originals)
        self.assertTrue(manifest["source_inputs_unchanged"])
        self.assertTrue(manifest["runtime_unchanged"])
        self.assertEqual(manifest["changed_input_copies"], [])
        copied_def = next(item for item in manifest["input_copies"] if item["kind"] == "def")
        copied_thermal = next(item for item in manifest["input_copies"] if item["kind"] == "thermal")
        self.assertEqual(Path(copied_def["copy"]).name, "open_3d_case005.input.def")
        self.assertEqual(Path(copied_thermal["copy"]).name, "open_3d_case003.thermal.input.json")
        self.assertNotEqual(copied_def["source"], copied_def["copy"])

    def test_mixed_score_contracts_are_rejected(self):
        baseline={'scoring_model':'documented-20261020','rules_contract':'documented-rules-v1',
                  'resource_profile':'documented','thermal_gain':20,'metrics':{'quality':0.5}}
        for key,value in [('scoring_model','official-2026'),('rules_contract','historical'),
                          ('resource_profile','platform-20261024'),('thermal_gain',3)]:
            with self.subTest(key=key), self.assertRaisesRegex(ValueError,key):
                benchmark.comparison(baseline,dict(baseline,**{key:value}))

    def test_nonzero_and_timeout_never_pass(self):
        for mode in ("nonzero", "timeout"):
            with self.subTest(mode=mode):
                self.output = self.root / mode
                status, manifest = self.run_benchmark(mode, ("--wall-timeout", "0.1"))
                self.assertEqual(status, 1)
                self.assertEqual(manifest["status"], "TIMEOUT" if mode == "timeout" else "FAIL")
                self.assertLess(manifest["seconds"], 2)
                self.assertTrue(manifest["source_inputs_unchanged"])

    def test_missing_or_malformed_stats_are_error(self):
        for mode in ("missing", "malformed"):
            with self.subTest(mode=mode):
                self.output = self.root / mode
                status, manifest = self.run_benchmark(mode)
                self.assertEqual(status, 1)
                self.assertEqual(manifest["status"], "ERROR")
                if mode == "malformed":
                    self.assertIn("finite numeric", manifest["error"])
                    self.assertEqual(manifest["guide_sha256"], benchmark.sha256(self.output / "result.guide"))

    def test_validator_nonzero_or_fail_report_never_pass(self):
        for report, code in (("FAIL", 0), ("PASS", 1)):
            with self.subTest(report=report, code=code):
                self.output = self.root / (report + str(code))
                status, manifest = self.run_benchmark(extra=("--validator", str(self.validator)),
                    validator_status=report, validator_code=code)
                self.assertEqual(status, 1)
                self.assertEqual(manifest["status"], "VALIDATION_FAIL")

    def test_malformed_optional_stats_objects_finalize_as_error(self):
        for field in ("counters", "policy", "options", "initial_metrics",
                      "pre_recovery_metrics", "pre_tsv_local_metrics"):
            for replacement in ("null", "[]"):
                with self.subTest(field=field, replacement=replacement):
                    self.output = self.root / (field + replacement)
                    status, manifest = self.run_benchmark(f"schema:{field}:{replacement}",
                        extra=("--baseline-stats", str(self.baseline)))
                    self.assertEqual(status, 1)
                    self.assertEqual(manifest["status"], "ERROR")
                    self.assertIn(f"stats field {field} must be an object", manifest["error"])
                    self.assertIn("finished_utc", manifest)
                    self.assertEqual(manifest["returncode"], 0)
                    self.assertEqual(manifest["changed_artifacts"], [])
                    self.assertTrue(manifest["source_inputs_unchanged"])

    def test_nonfinite_nested_json_is_error(self):
        status, manifest = self.run_benchmark("overflow-json")
        self.assertEqual(status, 1)
        self.assertEqual(manifest["status"], "ERROR")
        self.assertIn("nonfinite JSON", manifest["error"])
        self.assertIn("finished_utc", manifest)

    def test_malformed_baseline_objects_are_rejected_before_run(self):
        valid = json.loads(self.baseline.read_text())
        for field in ("counters", "policy", "options"):
            for replacement in (None, []):
                with self.subTest(field=field, replacement=replacement):
                    data = dict(valid)
                    data[field] = replacement
                    self.baseline.write_text(json.dumps(data))
                    status, manifest = self.run_benchmark(extra=("--baseline-stats", str(self.baseline)))
                    self.assertEqual(status, 1)
                    self.assertIsNone(manifest)
                    self.assertFalse(self.output.exists())

    def test_validator_rewrite_or_delete_guide_and_stats_is_integrity_failure(self):
        for artifact in ("stats", "guide"):
            for operation in ("rewrite", "delete"):
                with self.subTest(artifact=artifact, operation=operation):
                    self.output = self.root / (artifact + operation)
                    status, manifest = self.run_benchmark(extra=("--validator", str(self.validator)),
                        validator_action=f"{artifact}:{operation}")
                    filename = "stats.json" if artifact == "stats" else "result.guide"
                    hash_key = "stats_sha256" if artifact == "stats" else "guide_sha256"
                    self.assertEqual(status, 1)
                    self.assertEqual(manifest["status"], "INTEGRITY_FAIL")
                    self.assertEqual(manifest["validation_report"]["status"], "PASS")
                    self.assertEqual(manifest["validation_execution"]["returncode"], 0)
                    self.assertEqual(manifest["changed_artifacts"], [filename])
                    self.assertNotEqual(manifest[hash_key], manifest["artifact_hashes_after"][filename])
                    self.assertEqual(manifest["stats_summary"]["metrics"]["quality"], .715)
                    if operation == "delete":
                        self.assertIsNone(manifest["artifact_hashes_after"][filename])

    def test_modified_copy_is_integrity_failure_original_is_unchanged(self):
        before = benchmark.directory_hashes(self.case)
        status, manifest = self.run_benchmark("tamper")
        self.assertEqual(status, 1)
        self.assertEqual(manifest["status"], "INTEGRITY_FAIL")
        self.assertEqual(len(manifest["changed_input_copies"]), 1)
        self.assertEqual(benchmark.directory_hashes(self.case), before)

    def test_no_overwrite_protected_output_ambiguous_inputs_or_option_override(self):
        self.output.mkdir()
        sentinel = self.output / "keep.txt"
        sentinel.write_text("keep")
        status, manifest = self.run_benchmark()
        self.assertEqual(status, 1)
        self.assertIsNone(manifest)
        self.assertEqual(sentinel.read_text(), "keep")
        self.output = self.case / "output"
        self.assertEqual(self.run_benchmark()[0], 1)
        self.assertFalse(self.output.exists())
        self.output = self.root / "override"
        self.assertEqual(self.run_benchmark(extra=("--", "--output=/tmp/wrong"))[0], 1)
        self.assertFalse(self.output.exists())
        (self.case / "second.input.def").write_text("ambiguous")
        self.output = self.root / "ambiguous"
        self.assertEqual(self.run_benchmark()[0], 1)
        self.assertFalse(self.output.exists())

    def test_physical_baseline_and_invalid_budget_rejected_before_output(self):
        data = json.loads(self.baseline.read_text())
        data["scoring_model"] = "legacy-physical"
        self.baseline.write_text(json.dumps(data))
        self.assertEqual(self.run_benchmark(extra=("--baseline-stats", str(self.baseline)))[0], 1)
        self.assertFalse(self.output.exists())
        with self.assertRaises(SystemExit), redirect_stderr(io.StringIO()):
            benchmark.main(["--case-dir", str(self.case), "--binary", str(self.binary),
                "--runtime", str(self.runtime), "--heat-weight", "nan", "--time-limit", "15",
                "--output-dir", str(self.output)])
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
