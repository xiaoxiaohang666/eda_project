"""v2 behavioral regressions; package .so dependencies are not required.

Usage: python3 tests/test_v2.py /path/to/eda_router [--v1-binary /path/to/frozen/v1]
The frozen-v1 comparison is skipped when the optional local artifact is absent;
compare_router_versions.py always requires that independently built reference.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
CASE = ROOT / "benchmarks/sample001"
arguments = argparse.ArgumentParser(add_help=False)
arguments.add_argument("binary", type=Path)
arguments.add_argument("--v1-binary", type=Path,
                       default=ROOT / "output/runs/v2-development-20261002/eda_router_v1")
args, remaining = arguments.parse_known_args()
BINARY, V1 = args.binary.resolve(), args.v1_binary.resolve()
sys.argv = [sys.argv[0], *remaining]
sys.path.insert(0, str(ROOT / "scripts"))
from compare_router_versions import disable_tsv_local, nonworsening
# Reuse the independent Python rectangle-demand/TSV oracle. That test module
# takes its binary argument at import; isolate its argument parsing here.
with patch.object(sys, "argv", [sys.argv[0], str(BINARY)]):
    from test_algorithm import guide_accounting, parse_guide

TSV_DISABLED = disable_tsv_local(BINARY)
DISABLED = ["--pin-local", "0", "--wirelength-rounds", "0"]
INPUTS = ["--def", str(CASE / "sample001.input.def"),
          "--lef", str(CASE / "sample001.input.lef"),
          "--thermal-report", str(CASE / "sample001.thermal.input.json")]


class V2Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory(prefix="eda-v2-", dir=BINARY.parent.parent)
        cls.directory = Path(cls.folder.name)
        cls.disabled = cls.route("disabled", DISABLED)
        cls.pin_only = cls.route("pin-only", ["--pin-local", "1", "--wirelength-rounds", "0"])
        cls.recovery = cls.route("recovery", ["--pin-local", "0", "--wirelength-rounds", "3"])
        cls.full = cls.route("full", [])
        cls.repeat = cls.route("repeat", [])
        cls.frozen = cls.route("frozen-v1", [], binary=V1) if V1.is_file() else None

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    @classmethod
    def route(cls, name, options, binary=BINARY, time_limit=600):
        guide, stats = cls.directory / (name + ".guide"), cls.directory / (name + ".json")
        if binary == BINARY:
            options = [*options, *TSV_DISABLED]
        command = [str(binary), *INPUTS, "--router", "negotiated", "--time-limit", str(time_limit),
                   "--output", str(guide), "--stats", str(stats), *options]
        result = subprocess.run(command, capture_output=True, text=True, timeout=time_limit + 90)
        if result.returncode:
            raise AssertionError(" ".join(command) + "\n" + result.stdout + result.stderr)
        return guide, json.loads(stats.read_text(encoding="utf-8"))

    def assert_complete_and_accounted(self, result, expected_nets=203):
        guide, stats = result
        nets, overflow, points = guide_accounting(guide)
        self.assertEqual(len(nets), expected_nets)
        self.assertTrue(all(nets.values()))
        self.assertEqual(overflow, stats["metrics"]["guide_overflow"])
        self.assertEqual(len(points), stats["metrics"]["evaluator_tsv_count"])
        self.assertEqual(len(points), stats["metrics"]["physical_tsv_count"])
        return nets

    def test_disabled_switches_reproduce_frozen_v1(self):
        if self.frozen is None:
            self.skipTest("frozen v1 not supplied; use --v1-binary for binary-to-binary replay")
        self.assertEqual(self.frozen[0].read_bytes(), self.disabled[0].read_bytes())
        for key, value in self.frozen[1]["metrics"].items():
            self.assertEqual(value, self.disabled[1]["metrics"][key], key)
        self.assertEqual(self.frozen[1]["initial_metrics"], self.disabled[1]["initial_metrics"])
        self.assertEqual(self.frozen[1]["assignment"], self.disabled[1]["assignment"])
        self.assertEqual(self.disabled[1]["counters"]["pin_local_candidates"], 0)
        self.assertEqual(self.disabled[1]["counters"]["wirelength_trials"], 0)

    def test_full_result_is_deterministic_and_complete(self):
        self.assertEqual(self.full[0].read_bytes(), self.repeat[0].read_bytes())
        for key in ("metrics", "initial_metrics", "pre_recovery_metrics", "assignment", "counters"):
            self.assertEqual(self.full[1][key], self.repeat[1][key], key)
        self.assert_complete_and_accounted(self.full)
        self.assertEqual(self.full[1]["metrics"]["guide_overflow"], 0)
        self.assertEqual(self.full[1]["metrics"]["physical_overflow"], 0)
        self.assertEqual(self.full[1]["metrics"]["physical_tsv_count"], 3)
        if TSV_DISABLED:
            self.assertEqual(self.full[1]["options"]["tsv_local"], 0)
            for key in ("tsv_local_candidates", "tsv_local_trials", "tsv_local_accepted"):
                self.assertEqual(self.full[1]["counters"][key], 0, key)

    def test_recovery_preserves_complete_solution_order(self):
        for name, before, after in [("grid recovery", self.disabled, self.recovery),
                                    ("pin recovery", self.pin_only, self.full)]:
            with self.subTest(name=name):
                self.assertEqual(before[1]["metrics"], after[1]["pre_recovery_metrics"])
                self.assertTrue(nonworsening(after[1]["metrics"], before[1]["metrics"]))
                self.assert_complete_and_accounted(after)
        self.assertGreater(self.full[1]["counters"]["wirelength_trials"], 0)
        self.assertGreater(self.full[1]["counters"]["pin_local_candidates"], 0)

    def test_gain_is_raw_length_reduction_not_only_larger_hpwl_denominator(self):
        before, after = self.disabled[1]["metrics"], self.full[1]["metrics"]
        self.assertLess(after["physical_length_dbu"], before["physical_length_dbu"])
        self.assertLess(after["guide_mst_dbu"], before["guide_mst_dbu"])
        self.assertGreater(after["quality"], before["quality"] + 1e-6)
        self.assertLessEqual(after["guide_overflow"], before["guide_overflow"])
        self.assertLessEqual(after["physical_overflow"], before["physical_overflow"])
        self.assertEqual(after["physical_tsv_count"], before["physical_tsv_count"])
        self.assertEqual(after["evaluator_tsv_count"], before["evaluator_tsv_count"])

    def test_short_optimization_budget_retains_a_complete_result(self):
        # Calibrate against the same build's initial+v1 stages so sanitizer/slow
        # machines get sufficient time to finish a complete incumbent. Keep
        # pin-local off here to isolate the recovery transaction deadline path.
        baseline_seconds = self.disabled[1]["elapsed_seconds"]
        budget = max(0.75, baseline_seconds * 3 + 0.2)
        limited = self.route("short-budget", ["--pin-local", "0", "--wirelength-rounds", "100"],
                             time_limit=budget)
        self.assert_complete_and_accounted(limited)
        self.assertTrue(nonworsening(limited[1]["metrics"], limited[1]["pre_recovery_metrics"]))
        self.assertEqual(limited[1]["assignment"], self.disabled[1]["assignment"])
        # Tiny budgets may fail before the first solution, but must never publish
        # a truncated guide or statistics as if the task had succeeded.
        guide, stats = self.directory / "tiny.guide", self.directory / "tiny.json"
        result = subprocess.run([str(BINARY), *INPUTS, "--time-limit", "0.000001",
                                 "--output", str(guide), "--stats", str(stats)],
                                capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(guide.exists())
        self.assertFalse(stats.exists())

    def test_invalid_new_options_leave_previous_files_untouched(self):
        guide, stats = self.directory / "invalid.guide", self.directory / "invalid.json"
        guide.write_bytes(b"previous verified guide\n")
        stats.write_bytes(b"previous verified stats\n")
        for option in (["--pin-local", "2"], ["--pin-local", "true"],
                       ["--wirelength-rounds", "-1"], ["--wirelength-rounds", "1.5"]):
            with self.subTest(option=option):
                result = subprocess.run([str(BINARY), *INPUTS, "--output", str(guide),
                                         "--stats", str(stats), *option],
                                        capture_output=True, text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(guide.read_bytes(), b"previous verified guide\n")
                self.assertEqual(stats.read_bytes(), b"previous verified stats\n")

    def test_comparison_refuses_to_reuse_nonempty_output_directory(self):
        out = self.directory / "protected-comparison"
        out.mkdir()
        sentinel = out / "existing-evidence.txt"
        sentinel.write_bytes(b"keep previous evidence\n")
        result = subprocess.run([sys.executable, str(ROOT / "scripts/compare_router_versions.py"),
                                 "--v1-binary", str(BINARY), "--v2-binary", str(BINARY),
                                 "--output-dir", str(out)], capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("output directory is not empty", result.stderr)
        self.assertEqual(sentinel.read_bytes(), b"keep previous evidence\n")
        self.assertEqual(list(out.iterdir()), [sentinel])


if __name__ == "__main__":
    unittest.main()
