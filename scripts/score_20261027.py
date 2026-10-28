#!/usr/bin/env python3
"""Launch the isolated October 27 compatible scorer with pinned Python 3.10."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DEPENDENCY = ROOT / "references/community-score-0.4.2"


def main():
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument("--dependency-root", type=Path, default=DEFAULT_DEPENDENCY)
    args, _ = parser.parse_known_args()
    dependency = args.dependency_root.resolve()
    if os.environ.get("EDA_SCORER_20261027_BOOTSTRAPPED") != "1":
        loader = dependency / "runtime/lib/ld-linux-x86-64.so.2"
        interpreter = dependency / "runtime/bin/python3.10"
        if not loader.is_file() or not interpreter.is_file():
            print("Missing pinned 0.4.2 runtime; supply --dependency-root PATH", file=sys.stderr)
            return 2
        env = dict(os.environ)
        for key in ("PYTHONPATH", "PYTHONHOME", "PYTHONUSERBASE", "LD_PRELOAD", "LD_LIBRARY_PATH"):
            env.pop(key, None)
        env["EDA_SCORER_20261027_BOOTSTRAPPED"] = "1"
        os.execve(str(loader), [str(loader), "--inhibit-cache", "--library-path",
                  f"{dependency / 'vendor/unpacked'}:{dependency / 'runtime/lib'}",
                  str(interpreter), "-I", "-S", "-B", str(Path(__file__).resolve()), *sys.argv[1:]], env)
    if sys.version_info[:2] != (3, 10):
        raise RuntimeError("Pinned evaluator requires Python 3.10")
    sys.path.insert(0, str(ROOT / "tools"))
    from scorer_20261027.cli import main as evaluate
    return evaluate(default_dependency=DEFAULT_DEPENDENCY)


if __name__ == "__main__":
    raise SystemExit(main())
