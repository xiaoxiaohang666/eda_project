#!/usr/bin/env python3
"""Compare a frozen v1 binary with v2 ablations using the unchanged evaluator.

PASS means package-proxy validation and measurement parity, not detailed-route
DRC or a final competition score. All commands, artifacts and hashes are kept.
"""
import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

from run_algorithm_comparison import check_parity, invoke, sha256

ROOT = Path(__file__).resolve().parents[1]
CASE = ROOT / "benchmarks/sample001"


def disable_tsv_local(binary):
    """Preserve v2 semantics on newer binaries, while accepting frozen v1/v2."""
    result = subprocess.run([str(binary.resolve()), "--help"], capture_output=True,
                            text=True, timeout=30)
    if result.returncode:
        raise ValueError(f"unable to inspect binary options: {binary}")
    return ["--tsv-local", "0"] if "--tsv-local" in result.stdout else []


def comparison_key(metrics):
    return (metrics["guide_overflow"], metrics["physical_overflow"],
            -metrics["quality"], metrics["physical_length_dbu"], metrics["ordinary_vias"])


def nonworsening(after, before):
    """Use the router's complete ordering, including its quality tolerance."""
    for key in ("guide_overflow", "physical_overflow"):
        if after[key] != before[key]:
            return after[key] < before[key]
    if abs(after["quality"] - before["quality"]) > 1e-6:
        return after["quality"] > before["quality"]
    if abs(after["physical_length_dbu"] - before["physical_length_dbu"]) > 1e-6:
        return after["physical_length_dbu"] < before["physical_length_dbu"]
    return after["ordinary_vias"] <= before["ordinary_vias"]


def write_report(out, summary):
    lines = ["# Router v1/v2 comparison", "", f"Status: {summary['status']}", "",
             "Fixed inputs and unchanged package evaluator; PASS is structural/proxy evidence, "
             "not detailed-route DRC or a final ranking.", "",
             "Physical length is the router's explicit resource/access geometry accounting. "
             "Guide MST and Guide HPWL are independently recomputed by the package evaluator. "
             "Guide HPWL can change with guide representation; it is not a fixed terminal lower bound.", "",
             "| Variant | Q | Guide overflow | Physical overflow | Physical DBU | Guide MST DBU | Guide HPWL DBU | MST/HPWL | TSV | Router s |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for item in summary["runs"]:
        if "router_metrics" not in item:
            continue
        m = item["router_metrics"]
        lines.append(f"| {item['name']} | {m['quality']:.9f} | {m['guide_overflow']} | "
                     f"{m['physical_overflow']} | {m['physical_length_dbu']:.3f} | "
                     f"{m['guide_mst_dbu']:.3f} | {m['reference_dbu']:.3f} | "
                     f"{m['normalized_wirelength']:.9f} | {m['evaluator_tsv_count']} | "
                     f"{item['router_seconds']:.3f} |")
    if summary.get("checks"):
        lines.extend(["", "Checks:", ""])
        for name, result in summary["checks"].items():
            lines.append(f"- {name}: {result}")
    lines.extend(["", "Commands and per-run reports:", ""])
    for item in summary["runs"]:
        name = item["name"]
        lines.extend([f"- [{name} evaluator report]({name}/evaluation_report.json), "
                      f"[router stats]({name}/router_stats.json), [commands]({name}/commands.json)"])
    if "error" in summary:
        lines.extend(["", "Error: " + summary["error"]])
    lines.extend(["", "See comparison.json for input, binary and evaluator SHA-256 hashes, "
                  "exact argument arrays, timings, deltas, counters and policy details.", ""])
    (out / "comparison.md").write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--v1-binary", type=Path, required=True,
                        help="previously frozen v1 executable; never rebuilt by this script")
    parser.add_argument("--v2-binary", type=Path, default=ROOT / "build/bin/eda_router")
    parser.add_argument("--def", dest="input_def", type=Path, default=CASE / "sample001.input.def")
    parser.add_argument("--lef", dest="lefs", action="append", type=Path)
    parser.add_argument("--thermal", type=Path, default=CASE / "sample001.thermal.input.json")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--time-limit", type=float, default=120)
    parser.add_argument("--wirelength-rounds", type=int, default=3)
    args = parser.parse_args()
    args.lefs = args.lefs or [CASE / "sample001.input.lef"]
    if not math.isfinite(args.time_limit) or args.time_limit <= 0:
        parser.error("--time-limit must be finite and positive")
    if args.wirelength_rounds < 0:
        parser.error("--wirelength-rounds must be nonnegative")
    tracked = [args.v1_binary, args.v2_binary, args.input_def, *args.lefs, args.thermal,
               ROOT / "scripts/evaluate.py", ROOT / "scripts/run_algorithm_comparison.py",
               Path(__file__)]
    tracked += sorted((ROOT / "evaluator").glob("*.py"))
    tracked += sorted((ROOT / "evaluator").glob("*.so*"))
    for path in tracked:
        if not path.is_file():
            parser.error(f"missing file: {path}")
    if args.output_dir:
        out = args.output_dir.resolve()
        if out.exists() and (not out.is_dir() or any(out.iterdir())):
            parser.error(f"output directory is not empty; choose a fresh path: {out}")
        out.mkdir(parents=True, exist_ok=True)
    else:
        parent = ROOT / "output/runs"
        parent.mkdir(parents=True, exist_ok=True)
        out = Path(tempfile.mkdtemp(prefix="router-v1-v2-", dir=parent))
    hashes = {str(path.resolve()): sha256(path) for path in tracked}
    summary = {"started_utc": datetime.now(timezone.utc).isoformat(), "status": "RUNNING",
               "description": "same-input fixed-evaluator proxy comparison; not final ranking or DRC",
               "sha256": hashes, "runs": [], "checks": {}}
    summary_path = out / "comparison.json"

    def save():
        summary_path.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        write_report(out, summary)

    tsv_disabled = disable_tsv_local(args.v2_binary)
    rounds = str(args.wirelength_rounds)
    variants = [
        ("frozen_v1", args.v1_binary, []),
        ("v2_disabled", args.v2_binary, ["--pin-local", "0", "--wirelength-rounds", "0", *tsv_disabled]),
        ("pin_local", args.v2_binary, ["--pin-local", "1", "--wirelength-rounds", "0", *tsv_disabled]),
        ("recovery", args.v2_binary, ["--pin-local", "0", "--wirelength-rounds", rounds, *tsv_disabled]),
        ("full", args.v2_binary, ["--pin-local", "1", "--wirelength-rounds", rounds, *tsv_disabled]),
    ]
    save()
    try:
        for name, binary, options in variants:
            folder = out / name
            folder.mkdir()
            guide = folder / "router.guide"
            command = [str(binary.resolve()), "--def", str(args.input_def.resolve()),
                       "--thermal-report", str(args.thermal.resolve()), "--output", str(guide),
                       "--stats", str(folder / "router_stats.json"), "--router", "negotiated",
                       "--time-limit", str(args.time_limit), *options]
            for lef in args.lefs:
                command += ["--lef", str(lef.resolve())]
            evaluator = [sys.executable, str(ROOT / "scripts/evaluate.py"), str(guide),
                         "--input-def", str(args.input_def.resolve()), "--thermal-json",
                         str(args.thermal.resolve()), "--output-dir", str(folder)]
            for lef in args.lefs:
                evaluator += ["--lefs", str(lef.resolve())]
            item = {"name": name, "status": "RUNNING", "guide": str(guide),
                    "router_command": command, "evaluator_command": evaluator}
            summary["runs"].append(item)
            (folder / "commands.json").write_text(json.dumps(
                {"cwd": str(ROOT), "router": command, "evaluator": evaluator}, indent=2) + "\n", encoding="utf-8")
            (folder / "commands.sh").write_text("#!/bin/sh\nset -eu\ncd " + shlex.quote(str(ROOT)) +
                                                  "\n" + shlex.join(command) + "\n" + shlex.join(evaluator) + "\n",
                                                  encoding="utf-8")
            save()
            item["router_seconds"] = invoke(command, folder / "router.log", args.time_limit + 90)
            item["evaluator_seconds"] = invoke(evaluator, folder / "evaluation.log", 180)
            report = json.loads((folder / "evaluation_report.json").read_text(encoding="utf-8"))
            stats = json.loads((folder / "router_stats.json").read_text(encoding="utf-8"))
            if report.get("status") != "PASS":
                raise ValueError(f"{name}: evaluator did not PASS")
            item["verified_metrics"] = check_parity(stats, report)
            item["router_metrics"] = stats["metrics"]
            item["zero_guide_overflow"] = stats["metrics"]["guide_overflow"] == 0
            item["zero_physical_overflow"] = stats["metrics"]["physical_overflow"] == 0
            for key in ("wirelength", "congestion", "thermal_tsv", "score"):
                item[key] = report[key]
            for key in ("initial_metrics", "counters", "policy", "assignment", "options",
                        "elapsed_seconds", "peak_rss_kb", "pre_recovery_metrics"):
                if key in stats:
                    item[key] = stats[key]
            item["guide_sha256"] = sha256(guide)
            item["status"] = "PASS"
            save()
            m = item["router_metrics"]
            print(f"{name:12s} PASS Q={m['quality']:.9f} guide_overflow={m['guide_overflow']} "
                  f"physical={m['physical_length_dbu']:.1f} MST={m['guide_mst_dbu']:.1f} "
                  f"HPWL={m['reference_dbu']:.1f} TSV={m['evaluator_tsv_count']}", flush=True)
        runs = {item["name"]: item for item in summary["runs"]}
        old, disabled = runs["frozen_v1"], runs["v2_disabled"]
        checks = summary["checks"]
        checks["disabled_guide_is_byte_identical_to_frozen_v1"] = old["guide_sha256"] == disabled["guide_sha256"]
        checks["disabled_metrics_equal_frozen_v1"] = all(disabled["router_metrics"].get(k) == v
                                                      for k, v in old["router_metrics"].items())
        checks["disabled_assignment_equal_frozen_v1"] = old["assignment"] == disabled["assignment"]
        for name, before in (("recovery", "v2_disabled"), ("full", "pin_local")):
            checks[name + "_preserves_complete_comparison_key"] = nonworsening(
                runs[name]["router_metrics"], runs[before]["router_metrics"])
        final, baseline = runs["full"]["router_metrics"], old["router_metrics"]
        summary["full_minus_v1"] = {key: final[key] - value for key, value in baseline.items()}
        summary["observations"] = {
            "physical_length_reduced": final["physical_length_dbu"] < baseline["physical_length_dbu"],
            "raw_guide_mst_reduced": final["guide_mst_dbu"] < baseline["guide_mst_dbu"],
            "guide_hpwl_changed": final["reference_dbu"] != baseline["reference_dbu"],
            "quality_improved": final["quality"] > baseline["quality"] + 1e-6,
        }
        checks["tracked_inputs_binaries_evaluator_unchanged"] = all(sha256(Path(path)) == value
                                                                  for path, value in hashes.items())
        if not all(checks.values()):
            raise ValueError("comparison invariants failed: " + ", ".join(k for k, v in checks.items() if not v))
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
