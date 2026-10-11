#!/usr/bin/env python3
"""Compare a frozen v2 binary against v3 disabled/default with a fixed evaluator.

The frozen executable is never rebuilt. PASS covers package-proxy validation,
measurement parity, replay and nonregression, not detailed-route DRC or ranking.
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
NONINCREASING = ("guide_overflow", "physical_overflow", "guide_mst_dbu",
                 "physical_length_dbu", "ordinary_vias")


def write_report(out, summary):
    lines = ["# Router v2/v3 comparison", "", f"Status: {summary['status']}", "",
             "Fixed inputs and unchanged package evaluator; PASS is structural/proxy evidence, "
             "not detailed-route DRC or final competition ranking.", "",
             "Physical length is explicit resource/access geometry accounting. The unmodified "
             "package evaluator independently recomputes Guide MST, Guide HPWL, overflow and "
             "thermal correlation. Guide HPWL is representation dependent; acceptance also "
             "requires raw Guide MST and physical length to stay nonincreasing.", "",
             "| Variant | Q | Guide overflow | Physical overflow | Physical DBU | Guide MST DBU | Guide HPWL DBU | Ordinary vias | TSV | Router s |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for item in summary["runs"]:
        if "router_metrics" not in item:
            continue
        m = item["router_metrics"]
        lines.append(f"| {item['name']} | {m['quality']:.9f} | {m['guide_overflow']} | "
                     f"{m['physical_overflow']} | {m['physical_length_dbu']:.3f} | "
                     f"{m['guide_mst_dbu']:.3f} | {m['reference_dbu']:.3f} | "
                     f"{m['ordinary_vias']} | {m['evaluator_tsv_count']} | "
                     f"{item['router_seconds']:.3f} |")
    for title, key in (("Checks", "checks"), ("Observed improvements", "observations")):
        if summary.get(key):
            lines.extend(["", title + ":", ""])
            lines.extend(f"- {name}: {result}" for name, result in summary[key].items())
    lines.extend(["", "Commands and per-run reports:", ""])
    for item in summary["runs"]:
        name = item["name"]
        lines.append(f"- [{name} evaluator report]({name}/evaluation_report.json), "
                     f"[router stats]({name}/router_stats.json), [commands]({name}/commands.json)")
    if "error" in summary:
        lines.extend(["", "Error: " + summary["error"]])
    lines.extend(["", "See comparison.json for exact command arrays, input/binary/evaluator "
                  "SHA-256 hashes, metric deltas, counters, TSV assignments and output hashes.", ""])
    (out / "comparison.md").write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--v2-binary", type=Path, required=True,
                        help="previously frozen v2 executable; never rebuilt by this script")
    parser.add_argument("--v3-binary", type=Path, default=ROOT / "build/bin/eda_router")
    parser.add_argument("--def", dest="input_def", type=Path, default=CASE / "sample001.input.def")
    parser.add_argument("--lef", dest="lefs", action="append", type=Path)
    parser.add_argument("--thermal", type=Path, default=CASE / "sample001.thermal.input.json")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--time-limit", type=float, default=120)
    args = parser.parse_args()
    args.lefs = args.lefs or [CASE / "sample001.input.lef"]
    if not math.isfinite(args.time_limit) or args.time_limit <= 0:
        parser.error("--time-limit must be finite and positive")
    tracked = [args.v2_binary, args.v3_binary, args.input_def, *args.lefs, args.thermal,
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
        out = Path(tempfile.mkdtemp(prefix="router-v2-v3-", dir=parent))
    hashes = {str(path.resolve()): sha256(path) for path in tracked}
    summary = {"started_utc": datetime.now(timezone.utc).isoformat(), "status": "RUNNING",
               "description": "same-input fixed-evaluator proxy comparison; not final ranking or DRC",
               "sha256": hashes, "runs": [], "checks": {}}
    summary_path = out / "comparison.json"

    def save():
        summary_path.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        write_report(out, summary)

    variants = [("frozen_v2", args.v2_binary, []),
                ("v3_disabled", args.v3_binary, ["--tsv-local", "0"]),
                ("v3_default", args.v3_binary, [])]
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
            for key in ("wirelength", "congestion", "thermal_tsv", "score"):
                item[key] = report[key]
            for key in ("initial_metrics", "pre_recovery_metrics", "pre_tsv_local_metrics", "counters",
                        "policy", "assignment", "options", "elapsed_seconds", "peak_rss_kb"):
                if key in stats:
                    item[key] = stats[key]
            item["guide_sha256"] = sha256(guide)
            item["output_sha256"] = {p.name: sha256(p) for p in (guide, folder / "router_stats.json",
                                                               folder / "evaluation_report.json")}
            item["status"] = "PASS"
            save()
            m = item["router_metrics"]
            print(f"{name:12s} PASS Q={m['quality']:.9f} guide_overflow={m['guide_overflow']} "
                  f"physical={m['physical_length_dbu']:.1f} MST={m['guide_mst_dbu']:.1f} "
                  f"vias={m['ordinary_vias']} TSV={m['evaluator_tsv_count']}", flush=True)
        runs = {item["name"]: item for item in summary["runs"]}
        old, disabled, final_run = (runs[name] for name in ("frozen_v2", "v3_disabled", "v3_default"))
        checks = summary["checks"]
        checks["disabled_guide_is_byte_identical_to_frozen_v2"] = old["guide_sha256"] == disabled["guide_sha256"]
        checks["disabled_metrics_equal_frozen_v2"] = all(disabled["router_metrics"].get(k) == v
                                                      for k, v in old["router_metrics"].items())
        for key in ("initial_metrics", "pre_recovery_metrics", "assignment"):
            checks["disabled_" + key + "_equal_frozen_v2"] = old.get(key) == disabled.get(key)
        checks["disabled_switch_zero_and_default_switch_one"] = (disabled["options"]["tsv_local"] == 0
                                                               and final_run["options"]["tsv_local"] == 1)
        checks["disabled_tsv_local_counters_zero"] = all(disabled["counters"][key] == 0 for key in
            ("tsv_local_candidates", "tsv_local_trials", "tsv_local_accepted"))
        final, baseline = final_run["router_metrics"], old["router_metrics"]
        for key in NONINCREASING:
            checks["default_" + key + "_nonincreasing"] = final[key] <= baseline[key] + 1e-6
        checks["default_quality_nondecreasing"] = final["quality"] + 1e-12 >= baseline["quality"]
        checks["default_assignment_preserved"] = final_run["assignment"] == old["assignment"]
        for key in ("physical_tsv_count", "evaluator_tsv_count", "correlation"):
            checks["default_" + key + "_preserved"] = final[key] == baseline[key]
        summary["full_minus_v2"] = {key: final[key] - value for key, value in baseline.items()}
        summary["observations"] = {
            "physical_length_reduced": final["physical_length_dbu"] < baseline["physical_length_dbu"],
            "raw_guide_mst_reduced": final["guide_mst_dbu"] < baseline["guide_mst_dbu"],
            "ordinary_vias_reduced": final["ordinary_vias"] < baseline["ordinary_vias"],
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
