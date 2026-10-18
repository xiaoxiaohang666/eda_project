#!/usr/bin/env python3
"""Official-mode CLI regressions with owned synthetic inputs.

Usage: python3 tests/test_official_cli.py build/bin/eda_router output/runtime/official-flute
This tests the router's official metric integration, not the contest checker.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
ASSETS = (
    "libflute.so",
    "tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so",
    "POWV9.dat",
    "POST9.dat",
)
ROUTER = None
RUNTIME = None


def technology():
    layers = [
        f"LAYER metal{layer} TYPE ROUTING ; DIRECTION "
        f"{'HORIZONTAL' if layer % 2 else 'VERTICAL'} ; WIDTH 0.2 ; END metal{layer}"
        for layer in range(1, 21)
    ]
    layers.extend([
        "MACRO RESOURCE_SLOT_bottom SIZE 7 BY 7 ; END RESOURCE_SLOT_bottom",
        "MACRO RESOURCE_SLOT_top SIZE 7 BY 7 ; END RESOURCE_SLOT_top",
        "LAYER tsv TYPE CUT ; END tsv",
        "VIA tsv_v DEFAULT LAYER metal10 ; RECT -2.5 -2.5 2.5 2.5 ; "
        "LAYER tsv ; RECT -2.5 -2.5 2.5 2.5 ; "
        "LAYER metal11 ; RECT -2.5 -2.5 2.5 2.5 ; END tsv_v",
        "MACRO TSV_BOTTOM SIZE 7 BY 7 ; PIN TSV_PORT PORT LAYER metal10 ; "
        "RECT 1 1 6 6 ; END END TSV_PORT END TSV_BOTTOM",
        "MACRO TSV_TOP SIZE 7 BY 7 ; PIN TSV_PORT PORT LAYER metal11 ; "
        "RECT 1 1 6 6 ; END END TSV_PORT END TSV_TOP",
        # At FS placement (195,195), p's transformed center is (225,225).
        # The historical untransformed checker coordinate would be (225,205).
        "MACRO gate CLASS BLOCK ; SIZE 4 BY 4 ; PIN p PORT LAYER metal1 ; "
        "RECT 2.8 0.8 3.2 1.2 ; END END p "
        "OBS LAYER metal1 ; RECT 0 0 4 4 ; END END gate",
    ])
    return "\n".join(layers) + "\n"


def design():
    tracks = " ".join(
        f"TRACKS {axis} 5 DO 40 STEP 10 LAYER metal{layer} ;"
        for layer in range(1, 21) for axis in ("X", "Y")
    )
    # Declared grid extends beyond DIE. Cell (2,2) intersects DIE only on
    # [200,235] x [200,235]; its emitted rectangles must be clipped.
    return (
        "UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 235 235 ) ; "
        "GCELLGRID X 0 DO 5 STEP 100 ; GCELLGRID Y 0 DO 5 STEP 100 ; "
        + tracks +
        " COMPONENTS 3 ; "
        "- b RESOURCE_SLOT_bottom + PLACED ( 100 100 ) N ; "
        "- t RESOURCE_SLOT_top + PLACED ( 100 100 ) N ; "
        "- u gate + PLACED ( 195 195 ) FS ; END COMPONENTS "
        "PINS 3 ; "
        "- lower + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 25 25 ) N ; "
        "- upper + LAYER metal12 ( -1 -1 ) ( 1 1 ) + FIXED ( 215 25 ) N ; "
        "- border + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 225 215 ) N ; "
        "END PINS NETS 2 ; "
        "- cross ( PIN lower ) ( PIN upper ) ; "
        "- edge ( u p ) ( PIN border ) ; END NETS END DESIGN\n"
    )


def read_guides(path):
    result = {}
    name = None
    for line in path.read_text().splitlines():
        fields = line.split()
        if not fields:
            continue
        if line.strip() == "(":
            continue
        if line.strip() == ")":
            name = None
        elif len(fields) == 1:
            name = fields[0]
            if name in result:
                raise AssertionError(f"duplicate Guide net: {name}")
            result[name] = []
        elif len(fields) == 5 and name is not None:
            result[name].append((*map(int, fields[:4]), fields[4]))
        else:
            raise AssertionError(f"invalid Guide line: {line!r}")
    return result


class OfficialCliTests(unittest.TestCase):
    def setUp(self):
        work = ROOT / "output" / "official-cli-tests"
        work.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="synthetic-", dir=work)
        self.work = Path(self.temporary.name)
        self.lef = self.work / "synthetic.lef"
        self.def_file = self.work / "synthetic.input.def"
        self.thermal = self.work / "synthetic.thermal.json"
        self.lef.write_text(technology())
        self.def_file.write_text(design())
        # Coordinates are in microns. Sample every declared GCell, including
        # those outside DIE, so this does not depend on missing-cell fill.
        nodes = [[5 + 10 * col, 5 + 10 * row, 0, 30 + 2 * col + row]
                 for row in range(4) for col in range(4)]
        self.thermal.write_text(json.dumps({"meta": {"ambientTemp": 25}, "nodes": nodes}))
        self.input_bytes = {p: p.read_bytes() for p in (self.lef, self.def_file, self.thermal)}

    def tearDown(self):
        self.temporary.cleanup()

    def require_native(self):
        missing = [name for name in ASSETS if not (RUNTIME / name).is_file()]
        if missing:
            self.skipTest(f"native official runtime unavailable at {RUNTIME}: {', '.join(missing)}")

    def run_router(self, stem, *extra):
        guide, stats = self.work / f"{stem}.guide", self.work / f"{stem}.json"
        command = [str(ROUTER), "--def", str(self.def_file), "--lef", str(self.lef),
                   "--thermal-report", str(self.thermal), "--output", str(guide),
                   "--stats", str(stats), "--model", "official",
                   "--official-runtime", str(RUNTIME), "--time-limit", "20", *extra]
        process = subprocess.run(command, cwd=self.work, capture_output=True, text=True, timeout=30)
        return process, guide, stats

    def assert_success(self, result):
        process, guide, stats = result
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertTrue(guide.is_file(), process.stdout + process.stderr)
        self.assertTrue(stats.is_file(), process.stdout + process.stderr)
        for path, data in self.input_bytes.items():
            self.assertEqual(path.read_bytes(), data, f"input changed: {path}")
        report = json.loads(stats.read_text())
        self.assertEqual(report["scoring_model"], "official-2026")
        self.assertEqual(report["guide_encoding"], "global-gcell-regions")
        return guide, report

    def assert_geometry(self, guide):
        nets = read_guides(guide)
        self.assertEqual(set(nets), {"cross", "edge"})
        for rectangles in nets.values():
            self.assertTrue(rectangles)
            for x1, y1, x2, y2, layer in rectangles:
                self.assertTrue(0 <= x1 < x2 <= 235 and 0 <= y1 < y2 <= 235,
                                f"outside DIE: {(x1, y1, x2, y2, layer)}")
        for net, x, y, layer in (
            ("cross", 25, 25, "metal1"), ("cross", 215, 25, "metal12"),
            ("edge", 225, 225, "metal1"), ("edge", 225, 215, "metal1"),
        ):
            self.assertTrue(any(l == layer and x1 <= x <= x2 and y1 <= y <= y2
                                for x1, y1, x2, y2, l in nets[net]),
                            f"actual Pin uncovered: {net} {(x, y, layer)}")
        # The actual oriented Pin lies inside gate's metal1 OBS [195,235]^2.
        # Its successful overlapping Guide demonstrates abstract OBS semantics.
        self.assertTrue(any(l == "metal1" and min(x2, 235) > max(x1, 195)
                            and min(y2, 235) > max(y1, 195)
                            for x1, y1, x2, y2, l in nets["edge"]))

    def test_invalid_model_reports_error_without_output(self):
        process, guide, stats = self.run_router("invalid", "--model", "invalid")
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("invalid model", process.stderr)
        self.assertFalse(guide.exists())
        self.assertFalse(stats.exists())

    def test_heat_weight_requires_enabled_polish_without_output(self):
        for extra in (
            ("--tsv-polish-heat-weight", "32"),
            ("--tsv-polish-heat-weight", "32", "--tsv-assignment-polish", "0"),
            ("--tsv-assignment-polish", "1", "--tsv-polish-heat-weight", "32",
             "--tsv-assignment-polish", "0"),
        ):
            with self.subTest(options=extra):
                process, guide, stats = self.run_router("disabled-heat", *extra)
                self.assertNotEqual(process.returncode, 0)
                self.assertIn("requires --tsv-assignment-polish 1", process.stderr)
                self.assertFalse(guide.exists())
                self.assertFalse(stats.exists())

    def test_heat_weight_option_order_preserves_behavior(self):
        self.require_native()
        guides = []
        for index, extra in enumerate((
            ("--tsv-polish-heat-weight", "32", "--tsv-assignment-polish", "1"),
            ("--tsv-assignment-polish", "1", "--tsv-polish-heat-weight", "32"),
        )):
            guide, report = self.assert_success(self.run_router(f"heat-order-{index}", *extra))
            self.assert_geometry(guide)
            self.assertEqual(report["policy"]["assignment_polish_heat_weight"], 32)
            guides.append(guide.read_bytes())
        self.assertEqual(guides[0], guides[1])

    def test_invalid_polish_budgets_without_output(self):
        for extra in (
            ("--tsv-polish-passes", "0"),
            ("--tsv-polish-passes", "65"),
            ("--tsv-polish-candidates", "65537"),
            ("--tsv-polish-bidirectional", "2"),
            ("--fast-quality", "2"),
            ("--tsv-polish-candidates", "16", "--tsv-assignment-polish", "0"),
        ):
            with self.subTest(options=extra):
                process, guide, stats = self.run_router("invalid-budget", *extra)
                self.assertNotEqual(process.returncode, 0)
                self.assertFalse(guide.exists())
                self.assertFalse(stats.exists())

    def test_invalid_round_two_options_without_output(self):
        for extra in (
            ("--rrr-score-aware", "2"),
            ("--rrr-net-limit", "0"),
            ("--rrr-score-aware", "1", "--rrr-net-limit", "8193"),
            ("--rrr-net-limit", "512"),
            ("--rrr-score-aware", "1", "--model", "physical"),
            ("--official-score-pricing", "2"),
            ("--official-score-pricing", "1", "--model", "physical"),
            ("--fast-layer-pruning", "2"),
            ("--tsv-polish-thermal-candidates", "1"),
            ("--tsv-assignment-polish", "1", "--tsv-polish-thermal-candidates", "2"),
            ("--tsv-assignment-polish", "1", "--tsv-polish-thermal-candidates", "1",
             "--tsv-polish-thermal", "pareto"),
        ):
            with self.subTest(options=extra):
                process, guide, stats = self.run_router("invalid-round-two", *extra)
                self.assertNotEqual(process.returncode, 0)
                self.assertFalse(guide.exists())
                self.assertFalse(stats.exists())

    def test_round_two_options_export_and_geometry(self):
        self.require_native()
        guide, report = self.assert_success(self.run_router(
            "round-two", "--rrr-score-aware", "1", "--rrr-net-limit", "512",
            "--official-score-pricing", "1", "--tsv-assignment-polish", "1",
            "--tsv-polish-thermal-candidates", "1", "--tsv-resource-mode", "projected"))
        self.assert_geometry(guide)
        self.assertTrue(report["options"]["rrr_score_aware"])
        self.assertEqual(report["options"]["rrr_net_limit"], 512)
        self.assertTrue(report["options"]["official_score_pricing"])
        self.assertTrue(report["options"]["fast_layer_pruning"])
        self.assertGreater(report["options"]["congestion_price_updates"], 0)
        self.assertTrue(report["policy"]["assignment_polish_thermal_candidates_enabled"])

    def test_invalid_large_recovery_budgets_without_output(self):
        for extra in (
            ("--large-recovery", "2"),
            ("--large-recovery", "-1"),
            ("--large-recovery-nets", "0"),
            ("--large-recovery-nets", "4097"),
            ("--large-recovery-nets", "1.5"),
            ("--large-recovery-rounds", "0"),
            ("--large-recovery-rounds", "3"),
            ("--large-recovery-branches", "-1"),
            ("--large-recovery-branches", "9"),
            ("--large-recovery-seconds", "0"),
            ("--large-recovery-seconds", "-1"),
            ("--large-recovery-seconds", "nan"),
            ("--large-recovery-seconds", "inf"),
            ("--large-recovery-seconds", "3600.0001"),
            ("--large-recovery-seconds", "1e309"),
            ("--large-recovery-seconds", "1second"),
            ("--large-recovery-seconds",),
        ):
            with self.subTest(options=extra):
                process, guide, stats = self.run_router(
                    "invalid-large-recovery-budget", "--large-recovery", "1", *extra)
                self.assertNotEqual(process.returncode, 0)
                self.assertFalse(guide.exists())
                self.assertFalse(stats.exists())

    def test_large_recovery_requires_official_and_enabled_stage_without_output(self):
        for extra, diagnostic in (
            (("--large-recovery", "1", "--model", "physical"),
             "large recovery requires --model official"),
            (("--large-recovery-nets", "1024"),
             "large recovery budgets require --large-recovery 1"),
            (("--large-recovery-rounds", "1"),
             "large recovery budgets require --large-recovery 1"),
            (("--large-recovery-seconds", "300"),
             "large recovery budgets require --large-recovery 1"),
            (("--large-recovery-branches", "0"),
             "large recovery budgets require --large-recovery 1"),
            (("--large-recovery", "0", "--large-recovery-nets", "1"),
             "large recovery budgets require --large-recovery 1"),
            (("--large-recovery", "1", "--large-recovery-branches", "8",
              "--large-recovery", "0"),
             "large recovery budgets require --large-recovery 1"),
        ):
            with self.subTest(options=extra):
                process, guide, stats = self.run_router("invalid-large-recovery-mode", *extra)
                self.assertNotEqual(process.returncode, 0)
                self.assertIn(diagnostic, process.stderr)
                self.assertFalse(guide.exists())
                self.assertFalse(stats.exists())

    def test_large_recovery_boundary_values_parse_before_native_initialization(self):
        # Deliberately stop at the existing missing-runtime guard. Valid upper
        # limits must be accepted without routing 4096 nets or waiting an hour,
        # and these parser regressions must run when native assets are absent.
        missing = self.work / "missing-large-recovery-runtime"
        for extra in (
            ("--large-recovery", "0"),
            ("--large-recovery-nets", "1", "--large-recovery", "1"),
            ("--large-recovery", "1", "--large-recovery-nets", "4096"),
            ("--large-recovery", "1", "--large-recovery-rounds", "1"),
            ("--large-recovery-rounds", "2", "--large-recovery", "1"),
            ("--large-recovery", "1", "--large-recovery-branches", "0"),
            ("--large-recovery-branches", "8", "--large-recovery", "1"),
            ("--large-recovery", "1", "--large-recovery-seconds", "1e-9"),
            ("--large-recovery-seconds", "3600", "--large-recovery", "1"),
            ("--large-recovery-nets", "4096", "--large-recovery-rounds", "2",
             "--large-recovery-seconds", "3600", "--large-recovery-branches", "8",
             "--large-recovery", "1"),
        ):
            with self.subTest(options=extra):
                process, guide, stats = self.run_router(
                    "large-recovery-valid-boundary", "--official-runtime", str(missing), *extra)
                self.assertNotEqual(process.returncode, 0)
                self.assertIn("missing original official FLUTE runtime asset", process.stderr)
                self.assertFalse(guide.exists())
                self.assertFalse(stats.exists())
                for path, data in self.input_bytes.items():
                    self.assertEqual(path.read_bytes(), data, f"input changed: {path}")

    def test_polish_budget_order_and_stats(self):
        self.require_native()
        results = []
        for index, extra in enumerate((
            ("--tsv-polish-candidates", "16", "--tsv-polish-passes", "4",
             "--tsv-polish-bidirectional", "1", "--tsv-assignment-polish", "1"),
            ("--tsv-assignment-polish", "1", "--tsv-polish-bidirectional", "1",
             "--tsv-polish-passes", "4", "--tsv-polish-candidates", "16"),
        )):
            guide, report = self.assert_success(self.run_router(
                f"budget-order-{index}", "--tsv-resource-mode", "projected", *extra))
            self.assert_geometry(guide)
            policy = report["policy"]
            self.assertEqual(policy["assignment_polish_candidates"], 16)
            self.assertEqual(policy["assignment_polish_passes_limit"], 4)
            self.assertTrue(policy["assignment_polish_bidirectional"])
            self.assertLessEqual(policy["assignment_polish_passes"], 4)
            self.assertTrue(report["options"]["fast_quality"])
            results.append(guide.read_bytes())
        self.assertEqual(*results)

    def test_invalid_thermal_rule_combinations_without_output(self):
        for extra, error in (
            (("--tsv-polish-thermal", "invalid"), "invalid TSV polish thermal rule"),
            (("--tsv-polish-thermal", "pareto"), "requires --tsv-assignment-polish 1"),
            (("--tsv-assignment-polish", "1", "--tsv-polish-thermal", "pareto",
              "--model", "physical"), "requires --model official"),
            (("--tsv-assignment-polish", "1", "--tsv-polish-thermal", "pareto",
              "--tsv-polish-heat-weight", "2"), "requires weighted thermal rule"),
        ):
            with self.subTest(options=extra):
                process, guide, stats = self.run_router("invalid-thermal-rule", *extra)
                self.assertNotEqual(process.returncode, 0)
                self.assertIn(error, process.stderr)
                self.assertFalse(guide.exists())
                self.assertFalse(stats.exists())

    def test_pareto_thermal_rule_matches_exported_guide_correlation(self):
        self.require_native()
        guide, report = self.assert_success(self.run_router(
            "pareto", "--tsv-assignment-polish", "1", "--tsv-polish-thermal", "pareto",
            "--tsv-resource-mode", "projected", "--tsv-rounds", "0", "--tsv-local", "0"))
        self.assert_geometry(guide)
        policy = report["policy"]
        self.assertEqual(policy["assignment_polish_thermal_rule"], "pearson-pareto")
        self.assertGreaterEqual(policy["assignment_polish_correlation_after"],
                                policy["assignment_polish_correlation_before"])
        self.assertAlmostEqual(policy["assignment_polish_correlation_after"],
                               report["metrics"]["correlation"], places=12)

    def test_missing_runtime_reports_error_without_output(self):
        missing = self.work / "missing-native-runtime"
        process, guide, stats = self.run_router("missing", "--official-runtime", str(missing))
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("missing original official FLUTE runtime asset", process.stderr)
        self.assertFalse(guide.exists())
        self.assertFalse(stats.exists())

    def test_initial_boundary_cells_obs_and_actual_pins(self):
        self.require_native()
        guide, report = self.assert_success(self.run_router(
            "initial", "--rrr-rounds", "0", "--tsv-rounds", "0",
            "--wirelength-rounds", "0", "--tsv-local", "0", "--pin-local", "0"))
        self.assert_geometry(guide)
        # With direct Pin-local routing disabled, the partial border cell is
        # represented by a clipped cell Guide, not a thin terminal corridor.
        self.assertIn((200, 200, 235, 235, "metal1"), read_guides(guide)["edge"])
        self.assertEqual(report["metrics"], report["initial_metrics"])

    def test_default_optimization_keeps_official_quality(self):
        self.require_native()
        before_assets = {name: hashlib.sha256((RUNTIME / name).read_bytes()).hexdigest()
                         for name in ASSETS}
        guide, report = self.assert_success(self.run_router("optimized"))
        self.assert_geometry(guide)
        stages = [report[key] for key in ("initial_metrics", "pre_recovery_metrics",
                                         "pre_tsv_local_metrics", "metrics")]
        for stage in stages:
            self.assertTrue(math.isfinite(stage["quality"]))
            self.assertGreaterEqual(stage["quality"], 0)
            self.assertLessEqual(stage["quality"], 1)
            self.assertEqual(stage["zero_capacity_cells"], 0)
            expected = (.45 * stage["wirelength_score"] + .30 * stage["congestion_score"]
                        + .25 * stage["thermal_score"]) / 100
            self.assertAlmostEqual(stage["quality"], expected, places=12)
        for before, after in zip(stages, stages[1:]):
            self.assertGreaterEqual(after["quality"] + 1e-10, before["quality"])
        self.assertGreater(report["counters"]["wirelength_trials"], 0)
        self.assertEqual(before_assets,
                         {name: hashlib.sha256((RUNTIME / name).read_bytes()).hexdigest()
                          for name in ASSETS}, "native runtime assets changed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("router", type=Path, help="built eda_router executable")
    parser.add_argument("runtime", type=Path, help="prepared official FLUTE assets directory")
    arguments = parser.parse_args()
    ROUTER, RUNTIME = arguments.router.resolve(), arguments.runtime.resolve()
    if not ROUTER.is_file():
        parser.error(f"router does not exist: {ROUTER}")
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(OfficialCliTests))
    raise SystemExit(0 if result.wasSuccessful() else 1)
