#!/usr/bin/env python3
"""New-profile CLI, serialized Guide legality and independent score agreement."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from test_official_cli import ASSETS, design, technology

ROUTER, SCORER, VALIDATOR, RUNTIME = map(lambda p: Path(p).resolve(), sys.argv[1:5])
del sys.argv[1:5]


class V6CliTests(unittest.TestCase):
    def test_invalid_profile_combinations(self):
        for flags, error in [
            (["--score-profile", "bogus"], "invalid score profile"),
            (["--score-profile", "20261020"], "requires --model official"),
            (["--model", "official", "--score-profile", "20261020", "--nonpreferred", "1"], "forbids"),
            (["--nonpreferred", "1", "--score-profile", "20261020", "--model", "official"], "forbids"),
        ]:
            with self.subTest(flags=flags):
                result = subprocess.run([str(ROUTER), *flags], capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(error, result.stderr)

    def test_new_guide_roundtrip_and_score(self):
        if any(not (RUNTIME / asset).is_file() for asset in ASSETS):
            self.skipTest("native FLUTE runtime unavailable")
        with tempfile.TemporaryDirectory(prefix="eda-v6-cli-") as folder:
            work = Path(folder)
            lef, ddef, thermal = (work / name for name in ("test.lef", "test.def", "test.json"))
            lef.write_text(technology())
            ddef.write_text(design())
            thermal.write_text(json.dumps({"meta": {"ambientTemp": 25}, "nodes": [
                [5 + 10 * c, 5 + 10 * r, 0, 30 + 2 * c + r] for r in range(4) for c in range(4)]}))
            inputs = {p: p.read_bytes() for p in (lef, ddef, thermal)}
            guide, stats = work / "result.guide", work / "stats.json"
            def run(command):
                result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            run([ROUTER, "--def", ddef, "--lef", lef, "--thermal-json", thermal, "--output", guide,
                 "--stats", stats, "--model", "official", "--score-profile", "20261020",
                 "--official-runtime", RUNTIME, "--tsv-resource-mode", "global-grid", "--time-limit", "20",
                 "--rrr-score-aware", "1", "--official-score-pricing", "1"])
            run([VALIDATOR, "--def", ddef, "--lef", lef, "--thermal", thermal, "--guide", guide,
                 "--model", "official", "--score-profile", "20261020", "--tsv-resources", "global-grid",
                 "--report", work / "validation.json"])
            run([SCORER, guide, "--input-def", ddef, "--lefs", lef, "--thermal-json", thermal,
                 "--official-runtime", RUNTIME, "--pin-layer-policy", "shape", "--deduplicate-tracks", "1", "--thermal-gain", "20", "--output-dir", work / "score", "--no-csv"])
            routed = json.loads(stats.read_text())
            scored = json.loads((work / "score/evaluation_report.json").read_text())
            self.assertEqual(routed["scoring_model"], "documented-20261020")
            self.assertEqual(scored["status"], "PASS")
            self.assertFalse(scored["compatibility"]["official_equivalence"])
            self.assertAlmostEqual(routed["metrics"]["quality"] * 100, scored["score"]["quality"], places=8)
            for name in ("wirelength_score", "congestion_score", "thermal_score"):
                self.assertAlmostEqual(routed["metrics"][name], scored["score"][name], places=8)
            self.assertGreaterEqual(routed["metrics"]["quality"] + 1e-10, routed["initial_metrics"]["quality"])
            self.assertEqual(inputs, {p: p.read_bytes() for p in inputs})


if __name__ == "__main__":
    unittest.main()
