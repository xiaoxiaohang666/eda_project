#!/usr/bin/env python3
"""Wrapper process/provenance regressions; no native assets or real scoring.

The child executable is a synthetic program in a temporary directory. Its
reports test wrapper error handling and are never official scoring evidence.
"""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
WRAPPER = ROOT / "scripts/score_compat.py"
ASSETS = (
    "libflute.so",
    "tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so",
    "POWV9.dat",
    "POST9.dat",
    "asset-manifest.json",
)
FAKE_SCORER = r'''
import json
from pathlib import Path
import sys

mode = MODE
arguments = sys.argv[1:]
if "--help" in arguments:
    print("synthetic scorer help")
    raise SystemExit(0)
output = Path(arguments[arguments.index("--output-dir") + 1])
output.mkdir(parents=True, exist_ok=True)
(output / "received argv.json").write_text(json.dumps(arguments))
print("synthetic scorer stdout")
print("synthetic scorer stderr", file=sys.stderr)
if mode == "missing-report":
    raise SystemExit(0)
if mode == "malformed-report":
    (output / "evaluation_report.json").write_text("not JSON")
    raise SystemExit(0)
status = "ERROR" if mode.startswith("error") else "FAIL" if mode == "fail" else "PASS"
score = None if status == "ERROR" else 0 if status == "FAIL" else {"quality": 75}
report = {"status": status, "score": score, "compatibility": {"official_equivalence": False}}
(output / "evaluation_report.json").write_text(json.dumps(report))
if mode == "modify-input":
    Path(arguments[0]).write_text("changed by synthetic scorer")
if mode == "modify-runtime":
    runtime = Path(arguments[arguments.index("--official-runtime") + 1])
    (runtime / "POWV9.dat").write_bytes(b"changed synthetic asset")
raise SystemExit(2 if mode == "error" else 1 if mode in ("fail", "pass-nonzero") else 0)
'''


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ScoreCompatWrapperTests(unittest.TestCase):
    def setUp(self):
        work = ROOT / "output/score-compat-wrapper-tests"
        work.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="synthetic wrapper ", dir=work)
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.inputs_dir = self.work / "original input files"
        self.inputs_dir.mkdir()
        self.inputs = {
            "guide": self.inputs_dir / "route ; $(touch injected) [x].guide",
            "lef": self.inputs_dir / "technology one.lef",
            "lef2": self.inputs_dir / "technology two.lef",
            "def": self.inputs_dir / "synthetic.input.def",
            "thermal": self.inputs_dir / "temperature field.json",
        }
        for kind, path in self.inputs.items():
            path.write_bytes(f"unchanged synthetic {kind}\n".encode())
        self.runtime = self.work / "synthetic native assets"
        self.runtime.mkdir()
        for name in ASSETS:
            (self.runtime / name).write_bytes(f"synthetic asset {name}\n".encode())
        self.binary = self.work / "fake scorer executable"
        self.output = self.work / "new report directory"

    def run_wrapper(self, mode="pass", extra=(), include_runtime=True, prefix=()):
        self.binary.write_text(
            "#!" + sys.executable + "\n" + FAKE_SCORER.replace("MODE", repr(mode)),
            encoding="utf-8",
        )
        self.binary.chmod(0o755)
        forwarded = [*prefix,
            str(self.inputs["guide"]), "--lefs", str(self.inputs["lef"]),
            "--lefs", str(self.inputs["lef2"]),
            "--input-def", str(self.inputs["def"]),
            "--thermal-json", str(self.inputs["thermal"]),
            "--output-dir", str(self.output),
        ]
        if include_runtime:
            forwarded += ["--official-runtime", str(self.runtime)]
        forwarded += list(extra)
        sources = [*self.inputs.values(), *self.runtime.iterdir(), self.binary, WRAPPER]
        before = {str(path): sha256(path) for path in sources}
        process = subprocess.run(
            [sys.executable, str(WRAPPER), "--binary", str(self.binary), *forwarded],
            cwd=self.work, capture_output=True, text=True, timeout=10,
        )
        manifest_path = self.output / "run_manifest.json"
        manifest = json.loads(manifest_path.read_text()) if manifest_path.is_file() else None
        return process, manifest, before, forwarded

    def test_space_and_metacharacter_paths_remain_separate_argv(self):
        process, manifest, before, forwarded = self.run_wrapper(
            extra=("--via-cost-ratio", "1.5", "--profile", "20261020")
        )
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        received = json.loads((self.output / "received argv.json").read_text())
        self.assertEqual(received, forwarded)
        self.assertEqual(manifest["command"], [str(self.binary), *forwarded])
        self.assertFalse((self.work / "injected").exists())
        self.assertEqual(manifest["status"], "PASS")
        self.assertEqual(manifest["sha256_before"], before)
        self.assertEqual(manifest["sha256_after"], before)
        self.assertEqual(manifest["changed_files"], [])
        self.assertFalse(manifest["official_equivalence"])
        self.assertFalse(manifest["router_resources_measured_by_this_wrapper"])
        self.assertEqual(manifest["scope"], "local-compatible-evaluator-not-official")
        self.assertEqual({str(p): sha256(p) for p in map(Path, before)}, before)
        self.assertEqual({p.name for p in self.inputs_dir.iterdir()},
                         {p.name for p in self.inputs.values()})
        self.assertGreaterEqual(manifest["scorer_wall_seconds"], 0)

    def test_manifest_hashes_reports_and_combined_process_log(self):
        process, manifest, _, _ = self.run_wrapper()
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertEqual(manifest["report_sha256"],
                         sha256(self.output / "evaluation_report.json"))
        self.assertEqual(manifest["exit_code"], 0)
        self.assertEqual(set(manifest["artifacts"]),
                         {"evaluation_report.json", "score.log", "received argv.json"})
        for name, digest in manifest["artifacts"].items():
            self.assertEqual(sha256(self.output / name), digest, name)
        log = (self.output / "score.log").read_text()
        self.assertIn("synthetic scorer stdout", log)
        self.assertIn("synthetic scorer stderr", log)

    def test_error_report_is_recorded_as_error(self):
        process, manifest, before, _ = self.run_wrapper("error")
        self.assertEqual(process.returncode, 2, process.stdout + process.stderr)
        self.assertEqual(manifest["status"], "ERROR")
        self.assertEqual(manifest["exit_code"], 2)
        report = json.loads((self.output / "evaluation_report.json").read_text())
        self.assertIsNone(report["score"])
        self.assertEqual(manifest["sha256_after"], before)

    def test_error_report_with_zero_process_exit_is_not_success(self):
        process, manifest, _, _ = self.run_wrapper("error-zero")
        self.assertEqual(process.returncode, 2, process.stdout + process.stderr)
        self.assertEqual(manifest["status"], "ERROR")

    def test_nonzero_process_cannot_produce_pass_manifest(self):
        process, manifest, _, _ = self.run_wrapper("pass-nonzero")
        self.assertNotEqual(process.returncode, 0)
        self.assertNotEqual(manifest["status"], "PASS")
        self.assertEqual(manifest["exit_code"], 1)

    def test_legality_fail_stays_fail(self):
        process, manifest, _, _ = self.run_wrapper("fail")
        self.assertEqual(process.returncode, 1)
        self.assertEqual(manifest["status"], "FAIL")

    def test_nonempty_output_rejected_before_child_runs(self):
        self.output.mkdir()
        original = self.output / "evaluation_report.json"
        original.write_text('{"status": "PASS", "historical": true}\n')
        before = original.read_bytes()
        process, manifest, _, _ = self.run_wrapper()
        self.assertEqual(process.returncode, 2)
        self.assertIn("new or empty", process.stderr)
        self.assertIsNone(manifest)
        self.assertEqual(original.read_bytes(), before)
        self.assertEqual({p.name for p in self.output.iterdir()}, {original.name})

    def test_existing_empty_output_is_accepted(self):
        self.output.mkdir()
        process, manifest, _, _ = self.run_wrapper()
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertEqual(manifest["status"], "PASS")

    def test_modified_input_is_integrity_failure(self):
        process, manifest, before, _ = self.run_wrapper("modify-input")
        self.assertEqual(process.returncode, 2)
        self.assertEqual(manifest["status"], "INTEGRITY_FAIL")
        self.assertEqual(manifest["changed_files"], [str(self.inputs["guide"])])
        self.assertEqual(manifest["sha256_before"], before)
        self.assertEqual(manifest["sha256_after"][str(self.inputs["guide"])],
                         sha256(self.inputs["guide"]))

    def test_modified_runtime_is_integrity_failure(self):
        process, manifest, _, _ = self.run_wrapper("modify-runtime")
        self.assertEqual(process.returncode, 2)
        self.assertEqual(manifest["status"], "INTEGRITY_FAIL")
        self.assertEqual(manifest["changed_files"], [str(self.runtime / "POWV9.dat")])

    def test_missing_and_malformed_report_are_errors(self):
        for mode in ("missing-report", "malformed-report"):
            with self.subTest(mode=mode):
                self.output = self.work / mode
                process, manifest, _, _ = self.run_wrapper(mode)
                self.assertEqual(process.returncode, 2, process.stdout + process.stderr)
                if manifest is not None:
                    self.assertNotEqual(manifest["status"], "PASS")

    def test_default_runtime_is_forwarded_explicitly(self):
        process, manifest, _, forwarded = self.run_wrapper(include_runtime=False)
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        received = json.loads((self.output / "received argv.json").read_text())
        self.assertEqual(received[:-2], forwarded)
        self.assertEqual(received[-2:],
                         ["--official-runtime", str(ROOT / "output/runtime/official-flute")])
        self.assertEqual(manifest["command"], [str(self.binary), *received])

    def test_value_options_before_guide_are_not_mistaken_for_input(self):
        process, manifest, before, forwarded = self.run_wrapper(
            prefix=("--profile", "legacy-20261011", "--thermal-gain", "7", "--no-csv")
        )
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertEqual(json.loads((self.output / "received argv.json").read_text()),
                         forwarded)
        self.assertEqual(manifest["sha256_before"], before)
        self.assertEqual(manifest["status"], "PASS")


if __name__ == "__main__":
    unittest.main()
