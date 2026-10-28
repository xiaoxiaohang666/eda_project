#!/usr/bin/env python3
"""End-to-end checks for the independent score compatibility CLI.

Usage: test_score_compat.py BINARY REPO FIXTURE_DIR RUNTIME [--historical-toy]

The synthetic cases exercise the documented October 20 changes. An optional
frozen toy comparison checks the legacy profile against an unchanged official
report; it does not establish equivalence with the unavailable new scorer.
"""

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ARGS = None
ASSETS = (
    "libflute.so",
    "tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so",
    "POWV9.dat",
    "POST9.dat",
)
CSV_FILES = {
    "heat_gcell.csv",
    "tsv_gcell.csv",
    "thermal_node_count_per_gcell.csv",
    "tsv_points.csv",
    "overflow_gcells.csv",
}
GUIDE = "cross\n(\n40 40 60 60 metal10\n40 40 260 60 metal11\n)\n"


def technology():
    return "\n".join(
        f"LAYER metal{i} TYPE ROUTING ; DIRECTION "
        f"{'HORIZONTAL' if i % 2 else 'VERTICAL'} ; WIDTH 0.2 ; END metal{i}"
        for i in range(1, 21)
    ) + "\n"


def design(zero_capacity=False):
    # One preferred-direction track per cell. Moving M11's tracks outside the
    # routed row gives a real zero-capacity demand without malformed TRACKS.
    tracks = "\n".join(
        f"TRACKS {'Y' if i % 2 else 'X'} "
        f"{150 if zero_capacity and i == 11 else 50} DO 4 STEP 100 "
        f"LAYER metal{i} ;" for i in range(1, 21)
    )
    return (
        "UNITS DISTANCE MICRONS 10 ;\n"
        "DIEAREA ( 0 0 ) ( 400 400 ) ;\n"
        "GCELLGRID X 0 DO 5 STEP 100 ;\n"
        "GCELLGRID Y 0 DO 5 STEP 100 ;\n"
        + tracks
        + "\nCOMPONENTS 0 ; END COMPONENTS\n"
        "PINS 2 ;\n"
        "- lower + NET cross + LAYER metal10 ( -1 -1 ) ( 1 1 ) "
        "+ FIXED ( 50 50 ) N ;\n"
        "- upper + NET cross + LAYER metal11 ( -1 -1 ) ( 1 1 ) "
        "+ FIXED ( 250 50 ) N ;\n"
        "END PINS\n"
        "NETS 1 ; - cross ( PIN lower ) ( PIN upper ) ; END NETS\n"
        "END DESIGN\n"
    )


class ScoreCompatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        missing = [name for name in ASSETS if not (ARGS.runtime / name).is_file()]
        if missing:
            raise unittest.SkipTest(
                f"official FLUTE assets unavailable at {ARGS.runtime}: {missing}"
            )

    def setUp(self):
        ARGS.fixture_dir.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(
            prefix="score-compat-", dir=ARGS.fixture_dir
        )
        self.work = Path(self.temporary.name)
        self.lef = self.work / "synthetic.lef"
        self.definition = self.work / "synthetic.input.def"
        self.thermal = self.work / "synthetic.thermal.json"
        self.guide = self.work / "result.guide"
        self.lef.write_text(technology(), encoding="utf-8")
        self.definition.write_text(design(), encoding="utf-8")
        self.thermal.write_text(json.dumps({
            "meta": {"ambientTemp": 25},
            "nodes": [[5 + col * 10, 5 + row * 10, 0, 35 + col + 2 * row]
                      for row in range(4) for col in range(4)],
        }), encoding="utf-8")
        self.guide.write_text(GUIDE, encoding="utf-8")
        self.counter = 0

    def tearDown(self):
        self.temporary.cleanup()

    def run_score(self, *extra, output=None, runtime=None):
        self.counter += 1
        if output is None:
            output = self.work / f"report-{self.counter}"
        command = [
            str(ARGS.binary), str(self.guide), "--lefs", str(self.lef),
            "--input-def", str(self.definition),
            "--thermal-json", str(self.thermal), "--output-dir", str(output),
            "--official-runtime", str(runtime if runtime is not None else ARGS.runtime),
            *extra,
        ]
        inputs = {p: p.read_bytes() for p in
                  (self.lef, self.definition, self.thermal, self.guide)}
        original_names = {p.name for p in self.work.iterdir()}
        result = subprocess.run(
            command, cwd=self.work, capture_output=True, text=True, timeout=30
        )
        for path, data in inputs.items():
            self.assertEqual(path.read_bytes(), data, f"input modified: {path}")
        # In particular, no pin_positions.json or net_to_die.json side effects.
        allowed_names = original_names | {output.name}
        self.assertLessEqual({p.name for p in self.work.iterdir()}, allowed_names)
        report_path = output / "evaluation_report.json"
        report = json.loads(report_path.read_text()) if report_path.is_file() else None
        return result, output, report

    def assert_status(self, result, code, status):
        process, output, report = result
        self.assertEqual(process.returncode, code, process.stdout + process.stderr)
        self.assertIsNotNone(report, f"missing report in {output}: {process.stderr}")
        self.assertEqual(report["status"], status)
        self.assertFalse(report["compatibility"]["official_equivalence"])
        return report

    def assert_finite_score(self, report):
        for key in ("wirelength_score", "congestion_score", "thermal_score", "quality"):
            self.assertTrue(math.isfinite(report["score"][key]), key)
            self.assertGreaterEqual(report["score"][key], 0, key)
            self.assertLessEqual(report["score"][key], 100, key)

    def test_documented_profile_changes_and_pin_owner_exemption(self):
        current = self.assert_status(self.run_score(), 0, "PASS")
        legacy = self.assert_status(
            self.run_score("--profile", "legacy-20261011"), 0, "PASS"
        )
        self.assertEqual(current["compatibility"]["profile"], "20261020")
        self.assertEqual(legacy["compatibility"]["profile"], "legacy-20261011")
        self.assertEqual(current["s1"], {"status": "PASS", "value": 1})
        self.assertEqual(current["s4"]["status"], "NOT_EVALUATED")
        self.assert_finite_score(current)
        self.assert_finite_score(legacy)
        self.assertLess(current["wirelength"]["normalized"], 1)
        self.assertAlmostEqual(current["score"]["wirelength_score"], 100)
        self.assertLess(legacy["score"]["wirelength_score"], 100)
        self.assertAlmostEqual(
            current["wirelength"]["total_dbu"], legacy["wirelength"]["total_dbu"]
        )
        # New IO fixed demand is one in each pin cell; the lower pin masks both
        # its own GUIDE and TSV demand. M11's non-pin TSV cell still overflows.
        self.assertEqual(current["congestion"]["total_overflow"], 1)
        self.assertEqual(legacy["congestion"]["total_overflow"], 2)
        self.assertGreater(
            current["score"]["congestion_score"], legacy["score"]["congestion_score"]
        )
        # Four planar cells cost 400 DBU and one TSV costs 600 DBU.
        # Two pin centers are 200 DBU apart; archived FLUTE counts one native
        # via here, plus ten cross-die vias: 200 + 11*300 + 600 = 4100.
        self.assertAlmostEqual(current["wirelength"]["total_dbu"], 1000)
        self.assertAlmostEqual(current["wirelength"]["hpwl_reference_dbu"], 4100)
        self.assertAlmostEqual(legacy["score"]["wirelength_score"], 100 * 1000 / 4100)
        expected_congestion = 100 / math.sqrt(1 + 0.1**2)
        self.assertAlmostEqual(current["score"]["congestion_score"], expected_congestion)
        heat = [10 + col + 2 * row for row in range(4) for col in range(4)]
        tsv = [1] + [0] * 15
        mean_heat = sum(heat) / len(heat)
        mean_tsv = sum(tsv) / len(tsv)
        covariance = sum((h - mean_heat) * (t - mean_tsv) for h, t in zip(heat, tsv))
        variance_product = (
            sum((h - mean_heat)**2 for h in heat)
            * sum((t - mean_tsv)**2 for t in tsv)
        )
        expected_correlation = covariance / math.sqrt(variance_product)
        expected_thermal = 50 * (1 + math.tanh(20 * expected_correlation))
        self.assertAlmostEqual(
            current["thermal_tsv"]["correlation_above_ambient"], expected_correlation
        )
        self.assertAlmostEqual(current["score"]["thermal_score"], expected_thermal)
        self.assertAlmostEqual(
            current["score"]["quality"],
            45 + 0.30 * expected_congestion + 0.25 * expected_thermal,
        )

    def test_csv_outputs_describe_the_scored_cells(self):
        result = self.run_score()
        report = self.assert_status(result, 0, "PASS")
        output = result[1]
        self.assertLessEqual(CSV_FILES, {p.name for p in output.iterdir()})
        for name in ("heat_gcell.csv", "tsv_gcell.csv", "thermal_node_count_per_gcell.csv"):
            with (output / name).open(newline="", encoding="utf-8-sig") as stream:
                rows = list(csv.reader(stream))
            self.assertEqual(len(rows), 4, name)
            self.assertTrue(all(len(row) == 4 for row in rows), name)
        with (output / "tsv_gcell.csv").open(newline="") as stream:
            values = [[float(v) for v in row] for row in csv.reader(stream)]
        self.assertEqual(sum(map(sum, values)), 1)
        self.assertEqual(values[0][0], 1)
        with (output / "overflow_gcells.csv").open(
            newline="", encoding="utf-8-sig"
        ) as stream:
            overflow = list(csv.DictReader(stream))
        self.assertEqual(len(overflow), 1)
        self.assertEqual(float(overflow[0]["overflow"]), 1)
        self.assertEqual(float(overflow[0]["demand"]), 2)
        self.assertEqual(float(overflow[0]["capacity"]), 1)
        self.assertEqual(report["congestion"]["overflow_entries"], len(overflow))

    def test_no_csv_flag(self):
        result = self.run_score("--no-csv")
        self.assert_status(result, 0, "PASS")
        self.assertFalse(any(result[1].glob("*.csv")))

    def test_direction_width_rejected_only_by_new_profile(self):
        self.guide.write_text(GUIDE.replace("40 40 260 60", "40 40 260 160"))
        report = self.assert_status(self.run_score(), 1, "FAIL")
        self.assertEqual(report["s1"]["value"], 0)
        self.assertEqual(report["score"], 0)
        self.assert_status(self.run_score("--profile", "legacy-20261011"), 0, "PASS")

    def test_same_layer_transverse_touch_is_not_connected(self):
        self.guide.write_text(
            GUIDE.replace("40 40 260 60 metal11", "40 40 260 60 metal11\n240 60 260 80 metal11")
        )
        self.assert_status(self.run_score(), 1, "FAIL")
        self.assert_status(self.run_score("--profile", "legacy-20261011"), 0, "PASS")

    def test_same_layer_longitudinal_touch_is_connected(self):
        self.guide.write_text(
            GUIDE.replace("40 40 260 60 metal11", "40 40 160 60 metal11\n160 40 260 60 metal11")
        )
        self.assert_status(self.run_score(), 0, "PASS")

    def test_more_than_two_tsv_candidates_fail_s1_after_geometry_passes(self):
        self.guide.write_text(
            "cross\n(\n"
            "40 40 60 260 metal10\n"
            "40 40 260 60 metal11\n"
            "40 140 260 160 metal11\n"
            "40 240 260 260 metal11\n"
            "240 40 260 260 metal12\n)\n"
        )
        report = self.assert_status(self.run_score(), 1, "FAIL")
        self.assertEqual(report["s1"], {"status": "FAIL", "value": 0})
        self.assertIn("TSV", report["message"])

    def test_failed_thermal_mapping_does_not_claim_full_s1_pass(self):
        self.thermal.write_text('{"nodes": []}')
        report = self.assert_status(self.run_score(), 2, "ERROR")
        self.assertEqual(report["s1"], {"status": "NOT_EVALUATED", "value": None})

    def test_zero_capacity_penalty_is_finite_and_configurable(self):
        self.definition.write_text(design(zero_capacity=True))
        low = self.assert_status(
            self.run_score("--zero-capacity-penalty", "100"), 0, "PASS"
        )
        high = self.assert_status(
            self.run_score("--zero-capacity-penalty", "10000"), 0, "PASS"
        )
        self.assert_finite_score(low)
        self.assert_finite_score(high)
        self.assertGreater(
            low["score"]["congestion_score"], high["score"]["congestion_score"]
        )

    def test_routing_layer_without_any_tracks_has_zero_capacity(self):
        self.definition.write_text(
            design().replace("TRACKS Y 50 DO 4 STEP 100 LAYER metal11 ;", "")
        )
        report = self.assert_status(self.run_score(), 0, "PASS")
        self.assertEqual(report["s1"]["value"], 1)
        self.assertGreater(report["congestion"]["zero_capacity_cells"], 0)
        self.assert_finite_score(report)
        self.assertLess(report["score"]["congestion_score"], 1)

    def test_duplicate_track_policy_is_explicit(self):
        tracks = "TRACKS Y 50 DO 4 STEP 100 LAYER metal11 ;"
        self.definition.write_text(design().replace(tracks, tracks + "\n" + tracks))
        kept = self.assert_status(self.run_score("--deduplicate-tracks", "0"), 0, "PASS")
        deduplicated = self.assert_status(
            self.run_score("--deduplicate-tracks", "1"), 0, "PASS"
        )
        self.assertFalse(kept["compatibility"]["deduplicate_tracks"])
        self.assertTrue(deduplicated["compatibility"]["deduplicate_tracks"])
        self.assertEqual(kept["congestion"]["total_overflow"], 0)
        self.assertEqual(deduplicated["congestion"]["total_overflow"], 1)

    def test_fractional_tsv_demand(self):
        report = self.assert_status(
            self.run_score("--tsv-congestion-ratio", "0.5"), 0, "PASS"
        )
        self.assertAlmostEqual(report["congestion"]["total_overflow"], 0.5)

    def test_zero_gain_gives_neutral_thermal_score(self):
        report = self.assert_status(self.run_score("--thermal-gain", "0"), 0, "PASS")
        self.assertAlmostEqual(report["score"]["thermal_score"], 50)

    def test_resource_evidence_and_limits(self):
        within = self.assert_status(self.run_score(
            "--router-runtime-seconds", "43200",
            "--router-peak-memory-bytes", "128000000000",
        ), 0, "PASS")
        self.assertEqual(within["s4"]["status"], "PASS")
        over_time = self.assert_status(self.run_score(
            "--router-runtime-seconds", "43201",
            "--router-peak-memory-bytes", "1024",
        ), 1, "FAIL")
        self.assertEqual(over_time["s4"]["status"], "FAIL")
        self.assertEqual(over_time["score"], 0)
        over_memory = self.assert_status(self.run_score(
            "--router-runtime-seconds", "1",
            "--router-peak-memory-bytes", "1000000000000",
        ), 1, "FAIL")
        self.assertEqual(over_memory["s4"]["status"], "FAIL")
        self.assertEqual(over_memory["score"], 0)

    def test_zero_reference_refuses_to_invent_new_score(self):
        self.definition.write_text(design().replace("( 250 50 )", "( 50 50 )"))
        self.guide.write_text(GUIDE.replace("40 40 260 60", "40 40 60 60"))
        options = ("--tsv-cost-ratio", "0", "--via-cost-ratio", "0")
        current = self.assert_status(self.run_score(*options), 2, "ERROR")
        self.assertEqual(current["s1"]["value"], 1)
        self.assertIsNone(current["score"])
        self.assertIn("reference", current["message"].lower())
        legacy = self.assert_status(
            self.run_score("--profile", "legacy-20261011", *options), 0, "PASS"
        )
        self.assertEqual(legacy["wirelength"]["normalized"], 100)
        self.assertTrue(any("reference" in text.lower() for text in legacy["warnings"]))

    def test_no_tsv_distribution_is_error(self):
        self.definition.write_text(
            design().replace("+ LAYER metal11", "+ LAYER metal10")
            .replace("( 250 50 )", "( 50 70 )")
        )
        self.guide.write_text("cross\n(\n40 40 60 80 metal10\n)\n")
        for profile in ("20261020", "legacy-20261011"):
            with self.subTest(profile=profile):
                report = self.assert_status(self.run_score("--profile", profile), 2, "ERROR")
                self.assertEqual(report["s1"]["value"], 1)
                self.assertIsNone(report["score"])
                self.assertIn("TSV", report["message"])

    def test_high_fanout_skipped_reference_is_reported(self):
        definition = design().split("PINS 2 ;")[0]
        definition += "PINS 151 ;\n"
        for index in range(151):
            layer, x = (11, 250) if index == 0 else (10, 50)
            definition += (
                f"- p{index} + NET cross + LAYER metal{layer} "
                f"( -1 -1 ) ( 1 1 ) + FIXED ( {x} 50 ) N ;\n"
            )
        definition += "END PINS\nNETS 1 ; - cross "
        definition += " ".join(f"( PIN p{i} )" for i in range(151))
        definition += " ; END NETS\nEND DESIGN\n"
        self.definition.write_text(definition)
        report = self.assert_status(
            self.run_score("--profile", "legacy-20261011"), 0, "PASS"
        )
        self.assertEqual(report["wirelength"]["skipped_degree_limit_nets"], 1)
        self.assertTrue(any("150" in text for text in report["warnings"]))
        current = self.assert_status(self.run_score(), 2, "ERROR")
        self.assertIsNone(current["score"])
        self.assertTrue(any("150" in text for text in current["warnings"]))

    def test_missing_native_runtime_is_error(self):
        result = self.run_score(runtime=self.work / "unavailable-runtime")
        report = self.assert_status(result, 2, "ERROR")
        self.assertIsNone(report["score"])
        self.assertEqual(report["s1"], {"status": "NOT_EVALUATED", "value": None})

    def test_numeric_overflow_is_error_with_valid_json(self):
        report = self.assert_status(self.run_score("--tsv-cost-ratio", "1e308"), 2, "ERROR")
        self.assertIsNone(report["score"])
        self.assertIn("overflow", report["message"])

    def test_missing_and_unknown_guide_net_fail_s1(self):
        for contents in ("", GUIDE.replace("cross", "unknown")):
            with self.subTest(contents=contents):
                self.guide.write_text(contents)
                report = self.assert_status(self.run_score(), 1, "FAIL")
                self.assertEqual(report["s1"]["value"], 0)
                self.assertEqual(report["score"], 0)

    def test_invalid_options_fail_without_success_report(self):
        for extra in (
            ("--profile", "unknown"),
            ("--zero-capacity-penalty", "0"),
            ("--zero-capacity-penalty", "nan"),
            ("--thermal-gain", "-1"),
            ("--via-cost-ratio", "-1"),
            ("--tsv-cost-ratio", "inf"),
            ("--tsv-congestion-ratio", "-1"),
            ("--pin-layer-policy", "unknown"),
            ("--deduplicate-tracks", "2"),
            ("--router-runtime-seconds", "1"),
            ("--router-peak-memory-bytes", "1"),
        ):
            with self.subTest(extra=extra):
                process, _, report = self.run_score(*extra)
                self.assertEqual(process.returncode, 2, process.stdout + process.stderr)
                if report is not None:
                    self.assertEqual(report["status"], "ERROR")
                    self.assertIsNone(report["score"])

    def test_nonempty_output_is_preserved(self):
        output = self.work / "existing-report"
        output.mkdir()
        sentinel = output / "evaluation_report.json"
        sentinel.write_text('{"historical": true}\n')
        before = sentinel.read_bytes()
        process, _, _ = self.run_score(output=output)
        self.assertEqual(process.returncode, 2, process.stdout + process.stderr)
        self.assertEqual(sentinel.read_bytes(), before)
        self.assertEqual({p.name for p in output.iterdir()}, {sentinel.name})

    def test_empty_existing_output_is_accepted(self):
        output = self.work / "empty-report"
        output.mkdir()
        self.assert_status(self.run_score(output=output), 0, "PASS")

    def test_historical_toy_legacy_parity(self):
        if not ARGS.historical_toy:
            self.skipTest("enable --historical-toy for the frozen official report comparison")
        toy = ARGS.repo / "output/version5-20261018/global-grid-toy"
        evidence = ARGS.repo / (
            "output/version5-20261018/remote-tools/jobs/"
            "v5-global-grid-full-57dbdd9b/downloaded/results/toy/evaluation"
        )
        manifest_path = evidence / "run_manifest.json"
        if not manifest_path.is_file() or not (toy / "result.guide").is_file():
            self.skipTest("frozen toy evidence is not installed")
        manifest = json.loads(manifest_path.read_text())
        self.guide = toy / "result.guide"
        self.lef = toy / "inputs/000-lef/toy.input.lef"
        self.definition = toy / "inputs/001-def/toy.input.def"
        self.thermal = toy / "inputs/002-thermal/toy.thermal.input.json"
        by_kind = {item["kind"]: item["sha256"] for item in manifest["input_copies"]}
        for kind, path in (
            ("guide", self.guide), ("lef", self.lef),
            ("def", self.definition), ("thermal", self.thermal),
        ):
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), by_kind[kind])
        official_path = evidence / "official_report/evaluation_report.json"
        self.assertEqual(hashlib.sha256(official_path.read_bytes()).hexdigest(),
                         manifest["report_sha256"])
        official = json.loads(official_path.read_text())
        legacy = self.assert_status(
            self.run_score("--profile", "legacy-20261011", "--no-csv"), 0, "PASS"
        )
        for group, names in {
            "wirelength": ("total_dbu", "hpwl_reference_dbu", "normalized", "tsv_count"),
            "congestion": ("total_overflow", "max_overflow", "used_layer_gcells", "overflow_entries"),
            "score": ("wirelength_score", "congestion_score", "thermal_score"),
        }.items():
            for name in names:
                self.assertAlmostEqual(legacy[group][name], official[group][name],
                                       delta=1e-7, msg=f"{group}.{name}")
        self.assert_status(self.run_score("--no-csv"), 1, "FAIL")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=lambda p: Path(p).resolve())
    parser.add_argument("repo", type=lambda p: Path(p).resolve())
    parser.add_argument("fixture_dir", type=lambda p: Path(p).resolve())
    parser.add_argument("runtime", type=lambda p: Path(p).resolve())
    parser.add_argument("--historical-toy", action="store_true")
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
