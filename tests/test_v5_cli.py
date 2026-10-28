#!/usr/bin/env python3
"""Reject misconfigured experimental optimizers before touching input/output."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROUTER = Path(sys.argv.pop(1)).resolve()


class V5CliTests(unittest.TestCase):
    def reject(self, options, expected):
        with tempfile.TemporaryDirectory(prefix="eda-v5-cli-") as folder:
            output = Path(folder) / "result.guide"
            result = subprocess.run(
                [str(ROUTER), "--output", str(output), *options],
                capture_output=True, text=True, timeout=10,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(expected, result.stderr)
            self.assertFalse(output.exists())

    def test_requires_official_model(self):
        for flag in ("--tsv-block-refine", "--v5-pin-rooted"):
            with self.subTest(flag=flag):
                self.reject([flag, "1"], "requires --model official")

    def test_budget_requires_enabled_optimizer_in_either_order(self):
        for budget, enable in (("--tsv-block-size", "--tsv-block-refine"),
                               ("--v5-topology-max-pins", "--v5-pin-rooted")):
            for options in ([budget, "16"], [enable, "1", budget, "16", enable, "0"]):
                with self.subTest(options=options):
                    self.reject(["--model", "official", *options], f"{enable} 1")

    def test_invalid_resource_budgets(self):
        cases = [
            (["--tsv-block-size", "2"], "must be 3..64"),
            (["--tsv-block-size", "65"], "must be 3..64"),
            (["--tsv-block-passes", "0"], "must be 1..16"),
            (["--tsv-block-max-groups", "65537"], "must be 1..65536"),
            (["--tsv-block-max-evals", "0"], "must be positive"),
            (["--tsv-block-seconds", "nan"], "must be >0 and <=600"),
            (["--tsv-block-seconds", "0"], "must be >0 and <=600"),
            (["--v5-topology-max-pins", "129"], "must be 3..128"),
        ]
        for options, expected in cases:
            with self.subTest(options=options):
                self.reject(["--model", "official", *options], expected)

    def test_global_grid_requires_official(self):
        self.reject(["--tsv-resource-mode", "global-grid"], "require --model official")


if __name__ == "__main__":
    unittest.main()
