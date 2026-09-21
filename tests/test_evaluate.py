"""Wrapper failure paths must not turn an old report into a new PASS."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("evaluate", ROOT / "scripts/evaluate.py")
evaluate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evaluate)


class EvaluationTests(unittest.TestCase):
    def test_strict_guide_syntax(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "input.guide"
            valid = "n\n(\n0 0 10 10 metal1\n)\n"
            path.write_text(valid)
            evaluate.check_guide_syntax(path)
            for invalid in ["", valid[:-2], valid + valid, "n\n(\n)\n",
                            valid.replace("metal1", "metal21"), valid.replace("10 10", "0 10"),
                            valid.replace("(\n", ""), valid.replace("0 0 10", "garbage 0 10")]:
                with self.subTest(invalid=invalid):
                    path.write_text(invalid)
                    with self.assertRaises(ValueError):
                        evaluate.check_guide_syntax(path)

    def test_old_report_is_preserved_and_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            report = Path(folder) / "evaluation_report.json"
            content = json.dumps({"status": "PASS", "old": True})
            report.write_text(content)
            result = subprocess.run([
                sys.executable, str(ROOT / "scripts/evaluate.py"), "missing.guide",
                "--lefs", "missing.lef", "--input-def", "missing.def",
                "--thermal-json", "missing.json", "--output-dir", folder,
            ], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("previous evaluation", result.stderr)
            self.assertEqual(report.read_text(), content)


if __name__ == "__main__":
    unittest.main()
