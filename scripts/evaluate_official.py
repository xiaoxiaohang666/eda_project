#!/usr/bin/env python3
"""Run the unmodified official executable on byte-identical input copies.

The official scorer writes net_to_die.json beside its DEF input, so every input
is copied into a fresh output workspace. Official source files are never passed
as writable working inputs. A PASS is the official program's report status,
not a certification of detailed-route DRC.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import time


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def package_hashes(directory):
    return {str(path.relative_to(directory)): sha256(path)
            for path in sorted(directory.rglob("*")) if path.is_file()}


def write_json(path, record):
    path.write_text(json.dumps(record, ensure_ascii=False, indent=2,
                               allow_nan=False) + "\n", encoding="utf-8")


def finite_nonnegative(value):
    number = float(value)
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError("must be finite and nonnegative")
    return number


def positive_timeout(value):
    number = finite_nonnegative(value)
    if number == 0:
        raise argparse.ArgumentTypeError("timeout must be positive")
    return number


def stop_process(process):
    if process.poll() is not None:
        return
    if os.name == "posix":
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    else:
        process.kill()
    process.wait()


def run_process(command, cwd, log, timeout):
    started = time.monotonic()
    process = None
    result = {"command": command, "timeout_seconds": timeout}
    try:
        with log.open("w", encoding="utf-8") as stream:
            process = subprocess.Popen(command, cwd=cwd, stdout=stream,
                                       stderr=subprocess.STDOUT,
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
        result["seconds"] = round(time.monotonic() - started, 3)
        result["log"] = str(log)
    return result


def parse_report(path):
    """Summarize only actual JSON fields, with derived totals explicitly labeled."""
    report = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(report, dict) or report.get("status") not in ("PASS", "FAIL"):
        raise ValueError("official JSON report has no recognized PASS/FAIL status")
    summary = {"official_status": report["status"]}
    for name in ("category", "rule", "error", "hint", "fidelity", "wirelength",
                 "congestion", "thermal_tsv", "warnings", "efficiency_score"):
        if name in report:
            summary[name] = report[name]
    score = report.get("score")
    if isinstance(score, (int, float)) and not isinstance(score, bool):
        if not math.isfinite(score):
            raise ValueError("official JSON score is not finite")
        summary["official_score"] = score
    elif isinstance(score, dict):
        summary["official_score_components"] = score
        weights = score.get("weights", {})
        terms = (("wirelength_score", "wirelength"),
                 ("congestion_score", "congestion"),
                 ("thermal_score", "thermal_tsv"))
        values = [score.get(field) for field, _ in terms]
        factors = [weights.get(key) for _, key in terms]
        if all(isinstance(value, (int, float)) and not isinstance(value, bool)
               and math.isfinite(value) for value in values + factors):
            summary["weighted_score_derived_from_official_components"] = sum(
                value * factor for value, factor in zip(values, factors))
    return summary


def evaluate(args):
    score_dir = args.score_dir.resolve()
    executable = score_dir / "score"
    output = args.output_dir.resolve()
    inputs = [("guide", args.guide.resolve())]
    inputs += [("lef", path.resolve()) for path in args.lefs]
    inputs += [("def", args.input_def.resolve()), ("thermal", args.thermal_json.resolve())]
    if not score_dir.is_dir() or not executable.is_file():
        raise ValueError("score-dir must contain the original official score executable")
    if not os.access(executable, os.X_OK):
        raise ValueError("official score is not executable; use its supported Linux environment")
    if output.is_relative_to(score_dir):
        raise ValueError("output-dir must be outside the protected official package")
    for _, path in inputs:
        if not path.is_file():
            raise ValueError(f"missing input: {path}")
        if output.is_relative_to(path.parent):
            raise ValueError(f"output-dir must be outside the source input directory: {path.parent}")
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError("output-dir must be new or empty; prior reports are never overwritten")
    required_resources = [score_dir / name for name in ("POST9.dat", "POWV9.dat")]
    if any(not path.is_file() for path in required_resources):
        raise ValueError("official score-dir must contain POST9.dat and POWV9.dat")
    before = package_hashes(score_dir)
    originals_before = {str(path): sha256(path) for _, path in inputs}
    output.mkdir(parents=True, exist_ok=True)
    manifest = {"scope": "unchanged-official-executable-on-byte-identical-input-copies",
                "started_utc": datetime.now(timezone.utc).isoformat(),
                "status": "RUNNING", "official_package": str(score_dir),
                "official_executable_sha256": before["score"],
                "official_package_before": before, "source_inputs_before": originals_before,
                "python": platform.python_version(), "platform": platform.platform(),
                "wrapper_sha256": sha256(Path(__file__).resolve()),
                "input_copies": [], "resource_copies": [], "full_drc_certified": False}
    manifest_path = output / "run_manifest.json"
    write_json(manifest_path, manifest)
    exit_code = 1
    try:
        copied = {}
        for index, (kind, source) in enumerate(inputs):
            destination = output / "inputs" / f"{index:03d}-{kind}" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            digest = sha256(destination)
            if digest != originals_before[str(source)]:
                raise ValueError(f"input copy hash mismatch: {source}")
            copied.setdefault(kind, []).append(destination)
            manifest["input_copies"].append({"kind": kind, "source": str(source),
                                            "copy": str(destination), "sha256": digest})
        resources = required_resources + [score_dir / "thermal_gain.json"]
        for source in resources:
            if not source.is_file():
                continue
            destination = output / source.name
            shutil.copyfile(source, destination)
            digest = sha256(destination)
            if digest != before[str(source.relative_to(score_dir))]:
                raise ValueError(f"resource copy hash mismatch: {source}")
            manifest["resource_copies"].append({"source": str(source), "copy": str(destination),
                                               "sha256": digest})
        report_dir = output / "official_report"
        command = [str(executable), str(copied["guide"][0])]
        for lef in copied["lef"]:
            command += ["--lefs", str(lef)]
        command += ["--input-def", str(copied["def"][0]), "--thermal-json",
                    str(copied["thermal"][0]), "--output-dir", str(report_dir)]
        for attribute, option in (("tsv_cost_ratio", "--tsv-cost-ratio"),
                                  ("via_cost_ratio", "--via-cost-ratio"),
                                  ("tsv_congestion_ratio", "--tsv-congestion-ratio")):
            value = getattr(args, attribute)
            if value is not None:
                command += [option, str(value)]
        manifest["command"] = command
        write_json(manifest_path, manifest)
        execution = run_process(command, output, output / "score.log", args.timeout)
        manifest["execution"] = execution
        report_path = report_dir / "evaluation_report.json"
        if execution.get("timeout"):
            manifest["status"] = "TIMEOUT"
        elif execution.get("interrupted"):
            manifest["status"] = "INTERRUPTED"
        elif report_path.is_file():
            manifest["report"] = str(report_path)
            manifest["report_sha256"] = sha256(report_path)
            manifest["summary"] = parse_report(report_path)
            manifest["status"] = ("PASS" if execution["returncode"] == 0 and
                                  manifest["summary"]["official_status"] == "PASS" else "FAIL")
            exit_code = 0 if manifest["status"] == "PASS" else 1
        else:
            manifest["status"] = "ERROR"
            manifest["error"] = execution.get("error", "official scorer produced no JSON report")
    except KeyboardInterrupt:
        manifest["status"] = "INTERRUPTED"
    except (OSError, ValueError, TypeError) as error:
        manifest.update(status="ERROR", error=str(error))
    finally:
        manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
        try:
            after = package_hashes(score_dir)
            originals_after = {str(path): sha256(path) if path.is_file() else None
                               for _, path in inputs}
            changed = sorted(name for name in before.keys() | after.keys()
                             if before.get(name) != after.get(name))
            changed_inputs = sorted(name for name in originals_before
                                    if originals_before[name] != originals_after.get(name))
            copies_after = {record["copy"]: sha256(Path(record["copy"]))
                            if Path(record["copy"]).is_file() else None
                            for record in manifest["input_copies"] + manifest["resource_copies"]}
            changed_copies = sorted(record["copy"]
                                    for record in manifest["input_copies"] + manifest["resource_copies"]
                                    if copies_after[record["copy"]] != record["sha256"])
            manifest.update(input_and_resource_copies_after=copies_after,
                            changed_input_or_resource_copies=changed_copies,
                            official_package_after=after, source_inputs_after=originals_after,
                            changed_official_files=changed, changed_source_inputs=changed_inputs,
                            official_package_unchanged=not changed,
                            source_inputs_unchanged=not changed_inputs)
            if changed or changed_inputs or changed_copies:
                manifest["status"] = "INTEGRITY_FAIL"
                exit_code = 1
        except OSError as error:
            manifest.update(status="INTEGRITY_FAIL", integrity_error=str(error))
            exit_code = 1
        write_json(manifest_path, manifest)
    print(f"Official evaluation: {manifest['status']}")
    if "summary" in manifest:
        print(json.dumps(manifest["summary"], ensure_ascii=False, indent=2, allow_nan=False))
    print(f"Manifest: {manifest_path}")
    return exit_code


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("guide", type=Path)
    parser.add_argument("--score-dir", type=Path, required=True)
    parser.add_argument("--lefs", type=Path, action="append", required=True)
    parser.add_argument("--input-def", type=Path, required=True)
    parser.add_argument("--thermal-json", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--timeout", type=positive_timeout, default=900)
    parser.add_argument("--tsv-cost-ratio", type=finite_nonnegative)
    parser.add_argument("--via-cost-ratio", type=finite_nonnegative)
    parser.add_argument("--tsv-congestion-ratio", type=finite_nonnegative)
    args = parser.parse_args(argv)
    try:
        return evaluate(args)
    except (OSError, ValueError) as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
