#!/usr/bin/env python3
"""Exercise the actual submission wrapper after ZIP extraction.

The probe executable records its invocation so permission restoration, argument
forwarding and caller-relative paths are checked independently of routing.
"""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[1]
PROBE = b'''#!/usr/bin/python3
import json, os, pathlib, sys
args = sys.argv[1:]
output = pathlib.Path(args[args.index("--output") + 1])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps({"args": args, "cwd": os.getcwd(),
                            "library_path": os.environ["LD_LIBRARY_PATH"]}))
'''


class SubmissionEntrypointTests(unittest.TestCase):
    def setUp(self):
        artifacts = ROOT / "output" / "submission-entrypoint-tests"
        artifacts.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="extracted-", dir=artifacts)
        self.work = Path(self.temporary.name)
        self.package = self.work / "package with spaces"
        self.caller = self.work / "caller with spaces"
        self.caller.mkdir()
        archive = self.work / "submission.zip"
        with zipfile.ZipFile(archive, "w") as zf:
            for name, content in (("run.sh", (ROOT / "submission/run.sh").read_bytes()),
                                  ("router", PROBE)):
                entry = zipfile.ZipInfo(name)
                entry.create_system = 3
                entry.external_attr = 0o100755 << 16
                zf.writestr(entry, content)
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(self.package)
        self.arguments = ["--output", "results with spaces/route.guide",
                          "--thermal-report", "inputs with spaces/heat.json",
                          "--lef", "inputs with spaces/tech.lef",
                          "--def", "inputs with spaces/design.def"]

    def tearDown(self):
        self.temporary.cleanup()

    def invoke(self):
        result = subprocess.run(
            ["/bin/bash", str(self.package / "run.sh"), *self.arguments],
            cwd=self.caller, env={"PATH": "/usr/bin:/bin"},
            text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        received = json.loads((self.caller / self.arguments[1]).read_text())
        self.assertEqual(received["cwd"], str(self.caller))
        self.assertEqual(received["args"][-len(self.arguments):], self.arguments)
        self.assertEqual(received["args"][:4],
                         ["--model", "official", "--official-runtime",
                          str(self.package / "runtime/official-flute")])
        self.assertEqual(received["args"][4:6], ["--score-profile", "20261020"])
        self.assertEqual(received["library_path"],
                         f"{self.package}/lib:{self.package}/runtime/official-flute")

    def test_extractall_without_build_restores_execution(self):
        self.assertEqual((self.package / "router").stat().st_mode & 0o111, 0)
        self.assertFalse((self.package / "build.sh").exists())
        self.invoke()
        self.assertTrue((self.package / "router").stat().st_mode & 0o100)

    def test_existing_executable_permissions_are_preserved(self):
        router = self.package / "router"
        router.chmod(0o555)
        self.invoke()
        self.assertEqual(router.stat().st_mode & 0o777, 0o555)


if __name__ == "__main__":
    unittest.main()
