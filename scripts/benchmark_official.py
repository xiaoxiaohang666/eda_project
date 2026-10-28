#!/usr/bin/env python3
"""Reproduce one official-model routing run with frozen programs and hashes.

This runs the project's model and an optional independent Guide validator. It
does not invoke any official scorer or select/batch public cases automatically.
Extra router options follow ``--``; managed input/output/model options cannot
be overridden. For example::

    python3 scripts/benchmark_official.py --case-dir CASE --binary build/bin/eda_router \
      --official-runtime output/runtime/official-flute --heat-weight 8192 \
      --time-limit 180 --output-dir output/runs/v4-case001 \
      --validator build/bin/eda_validate_guide --baseline-stats OLD/stats.json \
      -- --rrr-rounds 3
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

PROJECT = Path(__file__).resolve().parents[1]
MANAGED_OPTIONS = {
    "--model", "--def", "--input-def", "--lef", "--lefs", "--thermal-json",
    "--thermal-report", "--output", "-o", "--stats", "--official-runtime",
    "--time-limit", "--tsv-assignment-polish", "--tsv-polish-thermal",
    "--tsv-polish-heat-weight", "--tsv-resource-mode",
}
SCORE_FIELDS = ("wirelength_score", "congestion_score", "thermal_score", "quality")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, record, exclusive=False):
    with path.open("x" if exclusive else "w", encoding="utf-8") as stream:
        stream.write(json.dumps(record, ensure_ascii=False, indent=2, allow_nan=False) + "\n")


def read_json(path):
    def reject_constant(value):
        raise ValueError(f"nonfinite JSON value: {value}")

    def finite_float(value):
        number = float(value)
        if not math.isfinite(number):
            reject_constant(value)
        return number

    return json.loads(path.read_text(encoding="utf-8"), parse_constant=reject_constant,
                      parse_float=finite_float)


def numeric(value):
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return False
    try:
        return math.isfinite(value)
    except OverflowError:
        return False


def finite_number(value):
    number = float(value)
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError("must be finite and nonnegative")
    return number


def positive_number(value):
    number = finite_number(value)
    if number == 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def discover_inputs(case_dir):
    if not case_dir.is_dir():
        raise ValueError("case-dir must be an existing single case directory")
    lefs = sorted(path.resolve() for path in case_dir.glob("*.input.lef") if path.is_file())
    defs = sorted(path.resolve() for path in case_dir.glob("*.input.def") if path.is_file())
    thermals = sorted(path.resolve() for path in case_dir.glob("*.thermal.input.json") if path.is_file())
    if not lefs or len(defs) != 1 or len(thermals) != 1:
        raise ValueError("case-dir requires at least one *.input.lef, exactly one *.input.def "
                         "and one *.thermal.input.json (thermal basename may differ)")
    return [("lef", path) for path in lefs] + [("def", defs[0]), ("thermal", thermals[0])]


def source_hashes():
    paths = [PROJECT / "CMakeLists.txt", PROJECT / "AGENTS.md"]
    suffixes = {".cpp", ".hpp", ".h", ".py", ".sh"}
    for directory in ("src", "scripts", "tests"):
        paths += [path for path in (PROJECT / directory).rglob("*")
                  if path.is_file() and path.suffix in suffixes and "__pycache__" not in path.parts]
    return {str(path.relative_to(PROJECT)): sha256(path) for path in sorted(paths) if path.is_file()}


def file_hashes(paths):
    return {str(path): sha256(path) if path.is_file() else None for path in paths}


def directory_hashes(folder):
    return {str(path.relative_to(folder)): sha256(path)
            for path in sorted(folder.rglob("*")) if path.is_file()}


def changed(before, after):
    return sorted(key for key in before.keys() | after.keys() if before.get(key) != after.get(key))


def stats_summary(path):
    stats = read_json(path)
    if not isinstance(stats, dict) or not isinstance(stats.get("metrics"), dict):
        raise ValueError("stats must be an object containing metrics")
    if stats.get("scoring_model", "official-2026") not in ("official-2026", "documented-20261020"):
        raise ValueError("baseline and candidate must use an identified documented/legacy diagnostic model")
    if any(not numeric(stats["metrics"].get(key)) for key in SCORE_FIELDS):
        raise ValueError("stats require finite numeric W/C/T scores and quality")
    for key in ("initial_metrics", "pre_recovery_metrics", "pre_tsv_local_metrics", "budgeted_wirelength_recovery",
                "counters", "policy", "options"):
        if key in stats and not isinstance(stats[key], dict):
            raise ValueError(f"stats field {key} must be an object")
    for key in ("peak_rss_kb", "elapsed_seconds"):
        if key in stats and (not numeric(stats[key]) or stats[key] < 0):
            raise ValueError(f"stats field {key} must be finite and nonnegative")
    fields = ("scoring_model", "geometry_model", "guide_encoding", "large_design_schedule",
              "rules_contract", "resource_profile", "thermal_gain", "thermal_gain_source", "zero_capacity_routing_policy",
              "metrics", "initial_metrics", "pre_recovery_metrics", "pre_tsv_local_metrics", "budgeted_wirelength_recovery",
              "counters", "policy", "options", "peak_rss_kb", "elapsed_seconds")
    return {key: stats[key] for key in fields if key in stats}


def comparison(baseline, candidate):
    for key in ("scoring_model", "geometry_model", "rules_contract", "resource_profile", "thermal_gain", "zero_capacity_routing_policy"):
        if baseline.get(key) != candidate.get(key):
            raise ValueError(f"cannot compare different routing score contracts: {key}")
    result = {"delta_direction": "candidate minus baseline; quality is a fraction, W/C/T are scores"}
    groups = {
        "metrics": (baseline["metrics"], candidate["metrics"]),
        "resources": ({key: baseline.get(key) for key in ("peak_rss_kb", "elapsed_seconds")},
                      {key: candidate.get(key) for key in ("peak_rss_kb", "elapsed_seconds")}),
        "counters": (baseline.get("counters", {}), candidate.get("counters", {})),
    }
    for group, (old, new) in groups.items():
        result[group] = {}
        for key in sorted(old.keys() & new.keys()):
            if not numeric(old[key]) or not numeric(new[key]):
                continue
            delta = new[key] - old[key]
            if not numeric(delta):
                raise ValueError(f"nonfinite comparison delta for {group}.{key}")
            result[group][key] = {"baseline": old[key], "candidate": new[key], "delta": delta}
    result["baseline_options"] = baseline.get("options", {})
    result["candidate_options"] = candidate.get("options", {})
    return result


def stop_process(process):
    if process.poll() is None:
        try:
            if os.name == "posix":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
        except ProcessLookupError:
            pass
    process.wait()


def run_process(command, output, log, timeout):
    started = time.monotonic()
    result = {"command": command, "timeout_seconds": timeout, "log": str(log)}
    process = None
    try:
        with log.open("w", encoding="utf-8") as stream:
            process = subprocess.Popen(command, cwd=output, stdout=stream, stderr=subprocess.STDOUT,
                                       start_new_session=os.name == "posix")
            try:
                result["returncode"] = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                stop_process(process)
                result.update(returncode=process.returncode, timeout=True)
            except KeyboardInterrupt:
                stop_process(process)
                result.update(returncode=process.returncode, interrupted=True)
    except OSError as error:
        result.update(returncode=None, error=str(error))
    finally:
        if process is not None and process.poll() is None:
            stop_process(process)
        result["seconds"] = round(time.monotonic() - started, 6)
    return result


def freeze_binary(source, destination):
    before = sha256(source)
    shutil.copy2(source, destination)
    if sha256(destination) != before or sha256(source) != before:
        raise ValueError(f"program changed while freezing: {source}")
    return {"source": str(source), "source_sha256": before,
            "frozen": str(destination.resolve()), "frozen_sha256": sha256(destination)}


def benchmark(args):
    case_dir, binary, runtime = (path.resolve() for path in (args.case_dir, args.binary, args.official_runtime))
    output = args.output_dir.resolve()
    inputs = discover_inputs(case_dir)
    programs = [binary] + ([args.validator.resolve()] if args.validator else [])
    for program in programs:
        if not program.is_file() or not os.access(program, os.X_OK):
            raise ValueError(f"program must be an executable file: {program}")
    if not runtime.is_dir():
        raise ValueError("official-runtime must be an existing native FLUTE runtime directory")
    if args.heat_weight > 1000000:
        raise ValueError("heat-weight must not exceed the router limit 1000000")
    extra = args.router_args[1:] if args.router_args[:1] == ["--"] else args.router_args
    for argument in extra:
        if argument.partition("=")[0] in MANAGED_OPTIONS:
            raise ValueError(f"extra router arguments cannot override managed option: {argument}")
    protected = [case_dir, runtime] + [path.parent for _, path in inputs]
    if any(output.is_relative_to(directory) for directory in protected):
        raise ValueError("output-dir must stay outside case/input/runtime directories")
    if args.output_dir.is_symlink() or (output.exists() and (not output.is_dir() or any(output.iterdir()))):
        raise ValueError("output-dir must be new or empty; existing runs are never overwritten")
    baseline = stats_summary(args.baseline_stats.resolve()) if args.baseline_stats else None
    baseline_record = ({"path": str(args.baseline_stats.resolve()), "sha256": sha256(args.baseline_stats.resolve()),
                        "summary": baseline} if baseline else None)
    before = file_hashes([path for _, path in inputs])
    sources_before, runtime_before = source_hashes(), directory_hashes(runtime)
    output.mkdir(parents=True, exist_ok=True)
    manifest = {"scope": "single-case-own-official-model-benchmark", "status": "RUNNING",
                "started_utc": datetime.now(timezone.utc).isoformat(), "case_dir": str(case_dir),
                "output_dir": str(output), "official_evaluator_run": False, "full_drc_certified": False,
                "source_root": str(PROJECT), "source_files_before": sources_before,
                "source_inputs_before": before, "runtime": str(runtime), "runtime_before": runtime_before,
                "input_copies": [], "baseline": baseline_record}
    manifest_path = output / "run.json"
    # Claim the directory before freezing artifacts, including concurrent runs
    # that both observed the same initially empty directory.
    write_json(manifest_path, manifest, exclusive=True)
    try:
        manifest["binary"] = freeze_binary(binary, output / "eda_router")
        copied = {}
        for index, (kind, source) in enumerate(inputs):
            destination = output / "inputs" / f"{index:03d}-{kind}" / source.name
            destination.parent.mkdir(parents=True)
            shutil.copyfile(source, destination)
            digest = sha256(destination)
            if digest != before[str(source)]:
                raise ValueError(f"input changed while copying: {source}")
            copied.setdefault(kind, []).append(destination)
            manifest["input_copies"].append({"source": str(source), "copy": str(destination), "kind": kind, "sha256": digest})
        guide, stats_path = output / "result.guide", output / "stats.json"
        command = [manifest["binary"]["frozen"], "--model", "official", "--official-runtime", str(runtime),
                   "--tsv-assignment-polish", "1", "--tsv-polish-thermal", "weighted",
                   "--tsv-polish-heat-weight", str(args.heat_weight), "--tsv-resource-mode", args.tsv_resource_mode]
        command += [value for path in copied["lef"] for value in ("--lef", str(path))]
        command += ["--def", str(copied["def"][0]), "--thermal-json", str(copied["thermal"][0]),
                    "--time-limit", str(args.time_limit), "--output", str(guide), "--stats", str(stats_path), *extra]
        manifest.update(command=command, binary_sha256=manifest["binary"]["frozen_sha256"])
        write_json(manifest_path, manifest)
        execution = run_process(command, output, output / "route.log", args.wall_timeout or args.time_limit + 180)
        manifest.update(execution=execution, seconds=execution["seconds"], returncode=execution["returncode"])
        if execution.get("timeout"):
            manifest["status"] = "TIMEOUT"
        elif execution.get("interrupted"):
            manifest["status"] = "INTERRUPTED"
        elif execution["returncode"] != 0:
            manifest["status"] = "FAIL"
        else:
            if not guide.is_file() or guide.stat().st_size == 0:
                raise ValueError("successful router did not produce a nonempty Guide")
            manifest["guide_sha256"] = sha256(guide)
            manifest["stats_sha256"] = sha256(stats_path)
            manifest["stats_summary"] = stats_summary(stats_path)
            if baseline:
                manifest["comparison"] = comparison(baseline, manifest["stats_summary"])
            manifest["status"] = "PASS"
            if args.validator:
                manifest["validator"] = freeze_binary(args.validator.resolve(), output / "eda_validate_guide")
                validation = [manifest["validator"]["frozen"], "--model", "official", "--tsv-resources", args.tsv_resource_mode]
                validation += [value for path in copied["lef"] for value in ("--lef", str(path))]
                validation += ["--def", str(copied["def"][0]), "--thermal", str(copied["thermal"][0]),
                               "--guide", str(guide), "--report", str(output / "validation.json")]
                manifest["validation_execution"] = run_process(validation, output, output / "validation.log", args.validator_timeout)
                report = read_json(output / "validation.json") if (output / "validation.json").is_file() else {}
                manifest["validation_report"] = report
                if manifest["validation_execution"]["returncode"] != 0 or not isinstance(report, dict) or report.get("status") != "PASS":
                    manifest["status"] = "VALIDATION_FAIL"
    except Exception as error:
        # Preserve an ERROR artifact even for unexpected malformed report
        # shapes; a diagnostic failure must never leave a RUNNING manifest.
        manifest.update(status="ERROR", error=str(error), error_type=type(error).__name__)
    finally:
        manifest["artifact_hashes_after"] = {}
        manifest["changed_artifacts"] = []
        for name, key in (("result.guide", "guide_sha256"), ("stats.json", "stats_sha256")):
            artifact = output / name
            try:
                digest = sha256(artifact) if artifact.is_file() else None
            except OSError as error:
                digest = None
                manifest.setdefault("artifact_integrity_errors", {})[name] = str(error)
            manifest["artifact_hashes_after"][name] = digest
            if key in manifest:
                if digest != manifest[key]:
                    manifest["changed_artifacts"].append(name)
            elif digest is not None:
                # Failed routes may still produce useful diagnostic files.
                manifest[key] = digest
        originals_after = file_hashes([path for _, path in inputs])
        runtime_after = directory_hashes(runtime)
        manifest.update(source_inputs_after=originals_after, runtime_after=runtime_after,
                        changed_source_inputs=changed(before, originals_after),
                        changed_runtime_files=changed(runtime_before, runtime_after),
                        source_files_after=source_hashes())
        manifest["changed_source_files"] = changed(sources_before, manifest["source_files_after"])
        manifest["source_inputs_unchanged"] = not manifest["changed_source_inputs"]
        manifest["runtime_unchanged"] = not manifest["changed_runtime_files"]
        copies_after = file_hashes([Path(record["copy"]) for record in manifest["input_copies"]])
        manifest["input_copies_after"] = copies_after
        manifest["changed_input_copies"] = [record["copy"] for record in manifest["input_copies"]
                                             if copies_after[record["copy"]] != record["sha256"]]
        for key in ("binary", "validator"):
            if key in manifest:
                frozen = Path(manifest[key]["frozen"])
                manifest[key]["sha256_after"] = sha256(frozen) if frozen.is_file() else None
        altered_program = any(manifest[key]["sha256_after"] != manifest[key]["frozen_sha256"]
                              for key in ("binary", "validator") if key in manifest)
        if (manifest["changed_source_inputs"] or manifest["changed_runtime_files"] or
                manifest["changed_input_copies"] or altered_program or manifest["changed_artifacts"] or
                manifest.get("artifact_integrity_errors")):
            manifest["status"] = "INTEGRITY_FAIL"
        manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
        write_json(manifest_path, manifest)
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--case-dir", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--official-runtime", "--runtime", type=Path, required=True)
    parser.add_argument("--heat-weight", type=finite_number, required=True)
    parser.add_argument("--time-limit", type=positive_number, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--wall-timeout", type=positive_number, help="process deadline; default time-limit + 180 seconds")
    parser.add_argument("--baseline-stats", type=Path)
    parser.add_argument("--validator", type=Path)
    parser.add_argument("--validator-timeout", type=positive_number, default=180)
    parser.add_argument("--tsv-resource-mode", choices=("auto", "paired", "projected", "global-grid"), default="auto")
    parser.add_argument("router_args", nargs=argparse.REMAINDER, help="additional router arguments after --")
    args = parser.parse_args(argv)
    try:
        manifest = benchmark(args)
    except (OSError, ValueError, TypeError) as error:
        print(f"benchmark_official: {error}", file=sys.stderr)
        return 1
    print(json.dumps({"status": manifest["status"], "run": str(args.output_dir.resolve() / "run.json"),
                      "seconds": manifest.get("seconds"), "official_evaluator_run": False}, ensure_ascii=False))
    return 0 if manifest["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
