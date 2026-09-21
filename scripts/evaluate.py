"""Run the unchanged package scorer with fresh reports and reproducible hashes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]


def check_guide_syntax(path):
    """Reject syntax the permissive package parser otherwise silently skips."""
    state, names, rects = "name", set(), 0
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        if state == "name":
            if line in ("(", ")") or len(line.split()) != 1 or line in names:
                raise ValueError(f"{path}:{number}: invalid/duplicate net name")
            names.add(line)
            state, rects = "open", 0
        elif state == "open":
            if line != "(":
                raise ValueError(f"{path}:{number}: expected '('")
            state = "rect"
        elif line == ")":
            if not rects:
                raise ValueError(f"{path}:{number}: empty net")
            state = "name"
        else:
            fields = line.split()
            if len(fields) != 5 or not re.fullmatch(r"metal(?:[1-9]|1[0-9]|20)", fields[4]):
                raise ValueError(f"{path}:{number}: malformed rectangle/layer")
            x1, y1, x2, y2 = map(int, fields[:4])
            if x1 >= x2 or y1 >= y2:
                raise ValueError(f"{path}:{number}: rectangle must have positive area")
            rects += 1
    if state != "name" or not names:
        raise ValueError(f"{path}: empty or unterminated guide")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("guide", type=Path)
    parser.add_argument("--lefs", type=Path, action="append", required=True)
    parser.add_argument("--input-def", type=Path, required=True)
    parser.add_argument("--thermal-json", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        out = args.output_dir.resolve()
        if any((out / name).exists() for name in ("evaluation_report.json", "run_manifest.json")):
            raise ValueError("output contains a previous evaluation; choose a new directory")
        inputs = [args.guide, *args.lefs, args.input_def, args.thermal_json]
        for path in inputs:
            if not path.is_file():
                raise ValueError(f"missing input: {path}")
        check_guide_syntax(args.guide)
        out.mkdir(parents=True, exist_ok=True)
        command = [sys.executable, str(ROOT / "evaluator/score.py"), str(args.guide.resolve())]
        for lef in args.lefs:
            command += ["--lefs", str(lef.resolve())]
        command += ["--input-def", str(args.input_def.resolve()),
                    "--thermal-json", str(args.thermal_json.resolve()), "--output-dir", str(out)]
        env = os.environ.copy()
        for key in ("LD_LIBRARY_PATH", "PYTHONPATH"):
            env[key] = str(ROOT / "evaluator") + (os.pathsep + env[key] if env.get(key) else "")
        versions = list((ROOT / "evaluator").glob("*.py")) + list((ROOT / "evaluator").glob("*.so*"))
        sources = sorted((ROOT / "src").rglob("*.[ch]pp"))
        sources += sorted((ROOT / "scripts").glob("*.py")) + sorted((ROOT / "scripts").glob("*.sh"))
        sources += [ROOT / "CMakeLists.txt"]
        manifest = {
            "started_utc": datetime.now(timezone.utc).isoformat(),
            "python": platform.python_version(), "command": command, "status": "RUNNING",
            "sha256": {str(p.resolve()): sha256(p) for p in inputs + versions + sources},
        }
        manifest_path = out / "run_manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        result = subprocess.run(command, cwd=ROOT, env=env)
        manifest["returncode"] = result.returncode
        manifest["status"] = "FAIL"
        if result.returncode == 0:
            report = json.loads((out / "evaluation_report.json").read_text(encoding="utf-8"))
            if report.get("status") == "PASS":
                manifest["status"] = "PASS"
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        if manifest["status"] != "PASS":
            raise ValueError(f"evaluation failed; see {manifest_path}")
        print(f"Verified report: {out / 'evaluation_report.json'}")
        print(f"Input/evaluator/source hashes: {manifest_path}")
        return 0
    except (OSError, ValueError) as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
