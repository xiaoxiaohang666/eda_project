#!/usr/bin/env python3
"""Route untouched public inputs, then validate the reloaded exported guides.

PASS means the project's engineering export checks passed, not official DRC or
an official quality score. Outputs are confined to a new or empty directory.
"""
import argparse
from collections import Counter, deque
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import time


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def package_hashes(root):
    return {str(path.relative_to(root)): sha256(path)
            for path in sorted(root.rglob("*")) if path.is_file()}


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
                    encoding="utf-8")


def execute(command, log, cwd, timeout):
    start = time.monotonic()
    record = {"command": command, "timeout_seconds": timeout, "log": str(log)}
    try:
        with log.open("w", encoding="utf-8") as output:
            process = subprocess.run(command, cwd=cwd, stdout=output,
                                     stderr=subprocess.STDOUT, timeout=timeout)
        record["returncode"] = process.returncode
    except subprocess.TimeoutExpired:
        record.update(returncode=None, timeout=True)
    except OSError as error:
        record.update(returncode=None, error=str(error))
    record["seconds"] = round(time.monotonic() - start, 3)
    return record


def route_failure(log, inputs):
    if not log.exists():
        return "route", []
    tail = deque(maxlen=12)
    kind = "route"
    with log.open(encoding="utf-8", errors="replace") as source:
        for line in source:
            tail.append(line.rstrip())
            if any(f"[ERROR] {path}:" in line for path in inputs):
                kind = "parse"
            elif "TIME_BUDGET" in line or "time limit exceeded" in line.lower():
                kind = "timeout"
    return kind, list(tail)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--validator", required=True, type=Path)
    parser.add_argument("--cases-root", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--time-limit", type=float, default=1800)
    parser.add_argument("--rrr-rounds", type=int, default=0)
    args = parser.parse_args()
    root, output = args.cases_root.resolve(), args.output_dir.resolve()
    binary, validator = args.binary.resolve(), args.validator.resolve()
    if not root.is_dir() or not binary.is_file() or not validator.is_file():
        parser.error("cases-root must be a directory; binary and validator must be existing files")
    if args.time_limit <= 0 or args.time_limit != args.time_limit or args.time_limit == float("inf") or args.rrr_rounds < 0:
        parser.error("time-limit must be finite and positive; rrr-rounds must be nonnegative")
    if output.is_relative_to(root):
        parser.error("output-dir must be outside the protected public package")
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        parser.error("output-dir must be new or empty; existing results are never overwritten")
    cases = [directory for directory in sorted(root.iterdir()) if directory.is_dir()
             and any(any(directory.glob(pattern)) for pattern in
                     ("*.input.lef", "*.input.def", "*.thermal.input.json"))]
    if not cases:
        parser.error("no immediate case directories containing LEF, DEF or thermal input files were found")
    before = package_hashes(root)
    output.mkdir(parents=True, exist_ok=True)
    save(output / "protected_inputs_before.json", before)
    # Graph construction and final export are outside the router's internal
    # optimization clock; the process still has a finite wall-clock bound.
    process_timeout = args.time_limit + 180
    manifest = {"started_utc": datetime.now(timezone.utc).isoformat(),
                "scope": "project-engineering-export-checks", "official_evaluator_run": False,
                "cases_root": str(root), "output_directory": str(output),
                "router": {"path": str(binary), "sha256": sha256(binary)},
                "validator": {"path": str(validator), "sha256": sha256(validator)},
                "routing_time_limit_seconds": args.time_limit, "rrr_rounds": args.rrr_rounds,
                "process_wall_timeout_seconds": process_timeout, "case_results": []}
    save(output / "run_manifest.json", manifest)
    interrupted = False
    try:
        for case in cases:
            work = output / case.name
            work.mkdir()
            inputs = {name: sorted(case.glob(pattern)) for name, pattern in
                      (("lef", "*.input.lef"), ("def", "*.input.def"),
                       ("thermal", "*.thermal.input.json"))}
            result = {"case": case.name, "status": "FAIL",
                      "inputs": {key: [str(path) for path in paths] for key, paths in inputs.items()}}
            manifest["case_results"].append(result)
            if not inputs["lef"] or len(inputs["def"]) != 1 or len(inputs["thermal"]) != 1:
                result.update(failure_kind="parse", phase="input_discovery",
                              diagnostic="require at least one LEF, exactly one DEF and one thermal JSON")
            else:
                lef_args = [value for path in inputs["lef"] for value in ("--lef", str(path))]
                guide, stats = work / "result.guide", work / "stats.json"
                command = [str(binary), *lef_args, "--def", str(inputs["def"][0]),
                           "--thermal-report", str(inputs["thermal"][0]), "--output", str(guide),
                           "--stats", str(stats), "--time-limit", str(args.time_limit),
                           "--rrr-rounds", str(args.rrr_rounds)]
                print(f"[{case.name}] routing", flush=True)
                result["route"] = execute(command, work / "route.log", work, process_timeout)
                if result["route"].get("timeout"):
                    result.update(failure_kind="timeout", phase="route")
                elif result["route"]["returncode"] != 0 or not guide.is_file() or not stats.is_file():
                    kind, tail = route_failure(work / "route.log", [path for paths in inputs.values() for path in paths])
                    result.update(failure_kind=kind, phase="route", diagnostic_tail=tail)
                else:
                    command = [str(validator), *lef_args, "--def", str(inputs["def"][0]),
                               "--thermal", str(inputs["thermal"][0]), "--guide", str(guide),
                               "--strict-obs", "1", "--report", str(work / "validation.json")]
                    result["phase"] = "validation"
                    result["validation"] = execute(command, work / "validation.log", work, process_timeout)
                    validation_report = work / "validation.json"
                    if validation_report.is_file():
                        report = json.loads(validation_report.read_text(encoding="utf-8"))
                        for key in ("checked", "errors", "guide_envelope_obs_intersections", "pin_port_precedence_rectangles"):
                            if key in report:
                                result["validation_" + key] = report[key]
                    if result["validation"].get("timeout"):
                        result.update(failure_kind="timeout", phase="validation")
                    elif result["validation"]["returncode"] != 0:
                        result.update(failure_kind="validation", phase="validation")
                    else:
                        report = json.loads((work / "validation.json").read_text(encoding="utf-8"))
                        if report.get("status") == "PASS" and report.get("strict_obs_enforced") is True:
                            result.update(status="PASS", validation_checked=report["checked"])
                        else:
                            result.update(failure_kind="validation", phase="validation",
                                          diagnostic="validator success did not provide a strict PASS report")
            save(work / "run_manifest.json", result)
            save(output / "run_manifest.json", manifest)
            print(f"[{case.name}] {result['status']} ({result.get('failure_kind', 'engineering checks')})", flush=True)
    except KeyboardInterrupt:
        interrupted = True
    except Exception as error:
        manifest["runner_error"] = str(error)
        if manifest["case_results"]:
            result = manifest["case_results"][-1]
            result["failure_kind"] = "validation" if result.get("phase") == "validation" else "route"
    finally:
        try:
            after = package_hashes(root)
        except OSError as error:
            after = {}
            manifest["integrity_error"] = str(error)
        save(output / "protected_inputs_after.json", after)
        changed = sorted(name for name in before.keys() | after.keys() if before.get(name) != after.get(name))
        results = manifest["case_results"]
        counts = Counter(result.get("failure_kind", "route") for result in results if result["status"] != "PASS")
        summary = {"status": "PASS" if not changed and not interrupted and not manifest.get("runner_error")
                   and len(results) == len(cases) and all(result["status"] == "PASS" for result in results) else "FAIL",
                   "scope": "strict-reloaded-export-engineering-validation", "official_evaluator_run": False,
                   "full_drc_certified": False, "public_package_unchanged": not changed,
                   "protected_file_count": len(before), "changed_public_files": changed,
                   "interrupted": interrupted, "expected_cases": len(cases), "cases": results,
                   "passed_cases": sum(result["status"] == "PASS" for result in results),
                   "failure_counts": dict(counts), "runner_error": manifest.get("runner_error"),
                   "integrity_error": manifest.get("integrity_error")}
        manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
        save(output / "run_manifest.json", manifest)
        save(output / "summary.json", summary)
        print(json.dumps({key: summary[key] for key in
                          ("status", "passed_cases", "expected_cases", "public_package_unchanged", "failure_counts")},
                         ensure_ascii=False), flush=True)
    return 130 if interrupted else (0 if summary["status"] == "PASS" else 1)


if __name__ == "__main__":
    raise SystemExit(main())
