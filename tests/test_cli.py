"""CLI contract regression tests; no contest binary dependencies."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

BINARY = pathlib.Path(sys.argv.pop(1)).resolve()
ROOT = pathlib.Path(__file__).resolve().parents[1]
CASE = ROOT / "benchmarks/sample001"
INPUTS = [
    "--input-def", str(CASE / "sample001.input.def"),
    "--lefs", str(CASE / "sample001.input.lef"),
    "--thermal-json", str(CASE / "sample001.thermal.input.json"),
]


class CliTests(unittest.TestCase):
    def run_cli(self, args, success=False):
        result = subprocess.run([str(BINARY), *args], capture_output=True, text=True, timeout=30)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("[SUCCESS]", result.stdout)
        return result

    def test_help(self):
        self.run_cli(["--help"], success=True)

    def test_invalid_arguments(self):
        for args in [[], ["--unknown"], ["--input-def"], ["--input-def", "--lefs", "a"],
                     [*INPUTS, "-o", ""], [*INPUTS, "--output", "--help"]]:
            with self.subTest(args=args):
                self.run_cli(args)

    def test_missing_file(self):
        self.run_cli([*INPUTS, "--input-def", "def-does-not-exist"])

    def test_no_input_overwrite(self):
        for path in [INPUTS[1], INPUTS[3], INPUTS[5]]:
            before = pathlib.Path(path).read_bytes()
            self.run_cli([*INPUTS, "-o", path])
            self.assertEqual(pathlib.Path(path).read_bytes(), before)

    def test_deterministic_output_and_failures(self):
        with tempfile.TemporaryDirectory(prefix="eda-cli-", dir=BINARY.parent.parent) as folder:
            output = pathlib.Path(folder) / "nested" / "result.guide"
            self.run_cli([*INPUTS, "-o", str(output)], success=True)
            first = output.read_bytes()
            self.run_cli([*INPUTS, "-o", str(output)], success=True)
            self.assertEqual(output.read_bytes(), first)
            self.run_cli([*INPUTS, "-o", "/dev/full"])
            self.run_cli([*INPUTS, "-o", str(output / "impossible.guide")])
            link = pathlib.Path(folder) / "input-link"
            link.symlink_to(INPUTS[1])
            self.run_cli([*INPUTS, "-o", str(link)])


if __name__ == "__main__":
    unittest.main()
