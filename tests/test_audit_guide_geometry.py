#!/usr/bin/env python3
"""Independent known-rule audit regressions; no official scorer dependency."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/audit_guide_geometry.py"
spec = importlib.util.spec_from_file_location("audit_guide_geometry", SCRIPT)
auditor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(auditor)


class GuideAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        folder = ROOT / "output" / "audit-guide-geometry-tests"
        folder.mkdir(parents=True, exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(dir=folder)
        cls.folder = Path(cls.temporary.name)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_audit(self, rectangles, definition=None, technology=None, raw=False):
        definition = definition or (
            "UNITS DISTANCE MICRONS 2000 ; DIEAREA ( 0 0 ) ( 764200 751900 ) ; "
            "GCELLGRID X 0 DO 256 STEP 3000 ; GCELLGRID Y 0 DO 252 STEP 3000 ;")
        technology = technology or (
            "LAYER metal11 TYPE ROUTING ; DIRECTION VERTICAL ; WIDTH 0.05 ; END metal11\n"
            "LAYER metal12 TYPE ROUTING ; DIRECTION HORIZONTAL ; WIDTH 0.05 ; END metal12\n")
        self.definition = self.folder / "fixture.def"
        self.lef = self.folder / "fixture.lef"
        self.guide = self.folder / "fixture.guide"
        self.definition.write_text(definition)
        self.lef.write_text(technology)
        self.guide.write_text(rectangles if raw else "net1364\n(\n" + rectangles + "\n)\n")
        return auditor.audit(self.definition, [self.lef], self.guide)

    def test_platform_counterexample_is_rejected(self):
        report = self.run_audit("151190 432901 151290 433001 metal11")
        self.assertEqual(report["exit_code"], 1)
        self.assertEqual(report["issue_counts"], {"transverse_width": 1})
        self.assertEqual(report["errors"][0]["width"], 100)
        self.assertEqual(report["errors"][0]["expected_width"], 3000)
        self.assertEqual(report["errors"][0]["net"], "net1364")
        self.assertEqual(report["errors"][0]["line"], 3)
        self.assertFalse(report["official_scorer_run"])
        self.assertEqual(report["rule_provenance"], "platform_log_20261024")
        self.assertNotIn("score", report)
        self.assertEqual(report["guide_sha256"], hashlib.sha256(self.guide.read_bytes()).hexdigest())

    def test_horizontal_narrow_rejected(self):
        report = self.run_audit("151190 432901 235243 433001 metal12")
        self.assertEqual(report["issue_counts"], {"transverse_width": 1})
        self.assertEqual(report["errors"][0]["direction"], "HORIZONTAL")

    def test_full_cell_runs_are_clear(self):
        report = self.run_audit("150000 432000 153000 435000 metal11\n234000 375000 237000 435000 metal11\n150000 432000 237000 435000 metal12")
        self.assertEqual(report["status"], "KNOWN_CHECKS_CLEAR")
        self.assertEqual(report["summary"]["width_checked"], 3)
        self.assertEqual(report["exit_code"], 0)

    def test_overwide_rejected(self):
        report = self.run_audit("150000 432000 156000 435000 metal11")
        self.assertEqual(report["errors"][0]["width"], 6000)

    def test_only_transverse_dimension_is_constrained(self):
        report = self.run_audit("150000 432001 153000 432101 metal11")
        self.assertEqual(report["exit_code"], 0)

    def test_do_not_invent_grid_alignment_requirement(self):
        report = self.run_audit("150001 432001 153001 435002 metal11")
        self.assertEqual(report["exit_code"], 0)

    def test_clipped_high_boundary_is_unresolved(self):
        report = self.run_audit("762000 432000 764200 435000 metal11")
        self.assertEqual(report["exit_code"], 2)
        self.assertEqual(report["issue_counts"], {"boundary_trim": 1})

    def test_clipped_low_boundary_is_unresolved(self):
        report = self.run_audit("100 3000 3000 6000 metal11", definition=
            "DIEAREA ( 100 0 ) ( 9000 9000 ) ; GCELLGRID X 0 DO 4 STEP 3000 ; GCELLGRID Y 0 DO 4 STEP 3000 ;")
        self.assertEqual(report["exit_code"], 2)

    def test_aligned_boundary_does_not_exempt_narrow_rect(self):
        report = self.run_audit("0 3000 100 6000 metal11")
        self.assertEqual(report["issue_counts"], {"transverse_width": 1})

    def test_multiple_grid_declarations_are_unresolved(self):
        report = self.run_audit("3000 3000 6000 6000 metal11", definition=
            "DIEAREA ( 0 0 ) ( 9000 9000 ) ; GCELLGRID X 0 DO 3 STEP 3000 ; GCELLGRID X 6000 DO 3 STEP 1500 ; GCELLGRID Y 0 DO 4 STEP 3000 ;")
        self.assertEqual(report["exit_code"], 2)
        self.assertEqual(report["issue_counts"], {"multiple_grid_declarations": 1})

    def test_bad_format_and_coordinate_are_errors(self):
        for row in ["0 0 3000 metal11", "0 0 nan 3000 metal11", "0 0 3000.5 3000 metal11"]:
            with self.subTest(row=row):
                self.assertEqual(self.run_audit(row)["exit_code"], 1)

    def test_unknown_and_nonpositive_geometry(self):
        for row, code in [("0 0 3000 3000 bogus", "unknown_layer"),
                          ("0 0 0 3000 metal11", "nonpositive_rectangle"),
                          ("-1 0 2999 3000 metal11", "engineering_outside_die")]:
            with self.subTest(row=row):
                self.assertIn(code, self.run_audit(row)["issue_counts"])

    def test_unterminated_block(self):
        report = self.run_audit("n\n(\n0 0 3000 3000 metal11\n", raw=True)
        self.assertEqual(report["issue_counts"], {"guide_syntax": 1})

    def test_comment_grid_is_ignored(self):
        report = self.run_audit("0 0 3000 3000 metal11", definition=
            "# GCELLGRID X 0 DO 20 STEP 100 ;\nDIEAREA ( 0 0 ) ( 9000 9000 ) ; GCELLGRID X 0 DO 4 STEP 3000 ; GCELLGRID Y 0 DO 4 STEP 3000 ;")
        self.assertEqual(report["exit_code"], 0)

    def test_diagnostics_are_bounded(self):
        report = self.run_audit("\n".join(["0 0 100 100 metal11"] * 50))
        self.assertEqual(report["summary"]["errors"], 50)
        self.assertEqual(len(report["errors"]), auditor.LIMIT)

    def test_cli_reports_and_exit_codes(self):
        for row, expected in [("0 0 3000 3000 metal11", 0),
                              ("0 0 100 100 metal11", 1),
                              ("762000 0 764200 3000 metal11", 2)]:
            self.run_audit(row)
            report_file = self.folder / "cli-report.json"
            completed = subprocess.run([sys.executable, str(SCRIPT), "--def", str(self.definition),
                "--lef", str(self.lef), "--guide", str(self.guide), "--report", str(report_file)],
                capture_output=True, text=True)
            self.assertEqual(completed.returncode, expected, completed.stderr)
            report = json.loads(report_file.read_text())
            self.assertEqual(report["exit_code"], expected)
            self.assertFalse(report["official_scorer_run"])
            json.loads(completed.stdout)


if __name__ == "__main__":
    unittest.main()
