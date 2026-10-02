#!/usr/bin/env python3
"""Reproduce first-version ablations with the unchanged package evaluator.

Each invocation creates a fresh run directory. Requires the authorized Python
3.10 evaluator .so dependencies; unit/CTest regressions do not require them.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
CASE = ROOT / "benchmarks/sample001"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def invoke(command, log, timeout):
    started = time.monotonic()
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=ROOT, stdout=stream,
                                stderr=subprocess.STDOUT, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"command returned {result.returncode}; see {log}")
    return time.monotonic() - started


def check_parity(stats, report):
    """Compare C++ measurements to a separately executed, unmodified scorer."""
    measured = stats["metrics"]
    expected = {
        "guide_overflow": report["congestion"]["total_overflow"],
        "guide_mst_dbu": report["wirelength"]["total_dbu"],
        "reference_dbu": report["wirelength"]["hpwl_reference_dbu"],
        "normalized_wirelength": report["wirelength"]["normalized"],
        "correlation": report["thermal_tsv"]["correlation_above_ambient"],
        "evaluator_tsv_count": report["thermal_tsv"]["tsv_points"],
        "quality": (0.45 * report["score"]["wirelength_quality"]
                    + 0.30 * report["score"]["congestion_quality"]
                    + 0.25 * report["score"]["thermal_quality"]),
    }
    for key, value in expected.items():
        if not math.isclose(measured[key], value, rel_tol=1e-8, abs_tol=1e-7):
            raise ValueError(f"metric mismatch {key}: router={measured[key]}, evaluator={value}")
    if measured["physical_tsv_count"] != measured["evaluator_tsv_count"]:
        raise ValueError("planned/evaluator TSV count mismatch")
    return expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/bin/eda_router")
    parser.add_argument("--def", dest="input_def", type=Path, default=CASE / "sample001.input.def")
    parser.add_argument("--lef", dest="lefs", action="append", type=Path)
    parser.add_argument("--thermal", type=Path, default=CASE / "sample001.thermal.input.json")
    parser.add_argument("--reference", type=Path, default=CASE / "sample001.output.guide")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--time-limit", type=float, default=120)
    args = parser.parse_args()
    args.lefs = args.lefs or [CASE / "sample001.input.lef"]
    if not math.isfinite(args.time_limit) or args.time_limit <= 0:
        parser.error("--time-limit must be finite and positive")
    tracked = [args.binary, args.input_def, *args.lefs, args.thermal, args.reference]
    tracked += sorted((ROOT / "evaluator").glob("*.py"))
    tracked += sorted((ROOT / "evaluator").glob("*.so*"))
    for path in tracked:
        if not path.is_file():
            parser.error(f"missing file: {path}")
    parent = ROOT / "output/runs"
    parent.mkdir(parents=True, exist_ok=True)
    out = args.output_dir.resolve() if args.output_dir else Path(
        tempfile.mkdtemp(prefix="algorithm-comparison-", dir=parent))
    if out.exists() and any(out.iterdir()):
        parser.error(f"output directory is not empty; choose a fresh path: {out}")
    out.mkdir(parents=True, exist_ok=True)
    summary = {
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "status": "RUNNING",
        "description": "same-input package-proxy ablation; not detailed-route DRC or final ranking",
        "sha256": {str(path.resolve()): sha256(path) for path in tracked},
        "runs": [],
    }
    summary_path = out / "comparison.json"

    def save():
        summary_path.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n",
                                encoding="utf-8")

    save()
    variants = [
        ("reference", None),
        ("baseline", ["--router", "baseline"]),
        ("initial", ["--router", "negotiated", "--rrr-rounds", "0", "--tsv-rounds", "0"]),
        ("rrr", ["--router", "negotiated", "--tsv-rounds", "0"]),
        ("tsv_no_local", ["--router", "negotiated", "--local-repair", "0"]),
        ("full", ["--router", "negotiated"]),
    ]
    try:
        for name, options in variants:
            folder = out / name
            folder.mkdir()
            guide = args.reference.resolve() if options is None else folder / "router.guide"
            item = {"name": name, "guide": str(guide), "status": "RUNNING"}
            summary["runs"].append(item)
            save()
            if options is not None:
                command = [str(args.binary.resolve()), "--def", str(args.input_def.resolve()),
                           "--thermal-report", str(args.thermal.resolve()), "--output", str(guide),
                           "--time-limit", str(args.time_limit), *options]
                for lef in args.lefs:
                    command += ["--lef", str(lef.resolve())]
                if name != "baseline":
                    command += ["--stats", str(folder / "router_stats.json")]
                item["router_command"] = command
                item["router_seconds"] = invoke(command, folder / "router.log", args.time_limit + 90)
            command = [sys.executable, str(ROOT / "scripts/evaluate.py"), str(guide),
                       "--input-def", str(args.input_def.resolve()), "--thermal-json",
                       str(args.thermal.resolve()), "--output-dir", str(folder)]
            for lef in args.lefs:
                command += ["--lefs", str(lef.resolve())]
            item["evaluator_seconds"] = invoke(command, folder / "evaluation.log", 180)
            report = json.loads((folder / "evaluation_report.json").read_text(encoding="utf-8"))
            if report.get("status") != "PASS":
                raise ValueError(f"{name}: evaluator did not PASS")
            item["status"] = "PASS"
            item["guide_sha256"] = sha256(guide)
            item["wirelength"] = report["wirelength"]
            item["congestion"] = report["congestion"]
            item["thermal_tsv"] = report["thermal_tsv"]
            item["score"] = report["score"]
            if name not in ("reference", "baseline"):
                stats = json.loads((folder / "router_stats.json").read_text(encoding="utf-8"))
                item["verified_metrics"] = check_parity(stats, report)
                item["router_metrics"] = stats["metrics"]
                item["counters"] = stats["counters"]
                item["policy"] = stats["policy"]
                item["assignment"] = stats["assignment"]
            save()
            print(f"{name:14s} PASS  overflow={report['congestion']['total_overflow']:4d} "
                  f"WL={report['score']['wirelength_score']:.4f} "
                  f"congestion={report['score']['congestion_score']:.4f} "
                  f"thermal={report['score']['thermal_score']:.4f} "
                  f"TSVs={report['thermal_tsv']['tsv_points']}", flush=True)
        summary["status"] = "PASS"
        save()
        print(f"Reports, commands, hashes and ablations: {summary_path}")
        return 0
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as error:
        summary["status"] = "FAIL"
        summary["error"] = str(error)
        save()
        print(f"[ERROR] {error}; partial evidence preserved in {out}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())