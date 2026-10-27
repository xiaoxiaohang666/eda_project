#!/usr/bin/env python3
"""Run the local compatible scorer and record SHA256 provenance.

All evaluator arguments are forwarded as separate argv values. This wrapper
never changes inputs or the official executable and never overwrites a report.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "output/score-compat-20261020/build/bin/eda_score_compat"
DEFAULT_RUNTIME = ROOT / "output/runtime/official-flute"
ASSETS = ("libflute.so", "tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so",
          "POWV9.dat", "POST9.dat", "asset-manifest.json")


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    wrapper, forwarded = parser.parse_known_args(argv)
    binary = wrapper.binary.resolve()
    if not binary.is_file():
        print(f"Missing scorer: {binary}\nBuild with:\n"
              "cmake -S . -B output/score-compat-20261020/build -DCMAKE_BUILD_TYPE=Release\n"
              "cmake --build output/score-compat-20261020/build --target eda_score_compat -j 2",
              file=sys.stderr)
        return 2
    if "--help" in forwarded:
        print("Wrapper option: --binary PATH; remaining options are forwarded to the scorer.",
              flush=True)
        return subprocess.run([str(binary), *forwarded], check=False).returncode
    # Parse only provenance fields; the C++ entry point validates the full CLI.
    provenance = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    provenance.add_argument("guide", type=Path)
    provenance.add_argument("--lefs", action="append", type=Path, required=True)
    provenance.add_argument("--input-def", type=Path, required=True)
    provenance.add_argument("--thermal-json", type=Path, required=True)
    provenance.add_argument("--output-dir", type=Path, required=True)
    provenance.add_argument("--official-runtime", type=Path, default=DEFAULT_RUNTIME)
    # Consume all value options even when they precede GUIDE; otherwise
    # parse_known_args can mistake an unknown option's value for the filename.
    for option in ("--profile", "--zero-capacity-penalty", "--pin-layer-policy", "--resource-profile",
                   "--deduplicate-tracks", "--thermal-gain", "--tsv-cost-ratio",
                   "--via-cost-ratio", "--tsv-congestion-ratio",
                   "--router-runtime-seconds", "--router-peak-memory-bytes"):
        provenance.add_argument(option)
    provenance.add_argument("--no-csv", action="store_true")
    args, _ = provenance.parse_known_args(forwarded)
    if "--official-runtime" not in forwarded and not any(
            a.startswith("--official-runtime=") for a in forwarded):
        forwarded += ["--official-runtime", str(args.official_runtime)]
    output = args.output_dir.resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        print("output-dir must be new or empty", file=sys.stderr)
        return 2
    inputs = [args.guide.resolve(), *[p.resolve() for p in args.lefs],
              args.input_def.resolve(), args.thermal_json.resolve()]
    assets = [args.official_runtime.resolve() / name for name in ASSETS
              if (args.official_runtime / name).is_file()]
    monitored = [*inputs, *assets, binary, Path(__file__).resolve()]
    before = {str(p): digest(p) for p in monitored}
    command = [str(binary), *forwarded]
    start = time.monotonic()
    started = datetime.now(timezone.utc).isoformat()
    execution = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, errors="replace", check=False)
    seconds = time.monotonic() - start
    print(execution.stdout, end="", flush=True)
    after = {str(p): digest(p) if p.is_file() else None for p in monitored}
    changed = [name for name in before if before[name] != after[name]]
    if changed:
        print("INTEGRITY_FAIL: monitored input/runtime/program files changed: "
              + ", ".join(changed), file=sys.stderr)
    # An invalid CLI can fail before creating any artifacts. Do not fabricate a
    # run directory or attach this invocation to an unrelated preexisting run.
    report = output / "evaluation_report.json"
    if not report.is_file():
        return 2
    with (output / "score.log").open("x", encoding="utf-8") as log:
        log.write(execution.stdout)
    payload = json.loads(report.read_text(encoding="utf-8"))
    reported_status = payload.get("status") if isinstance(payload, dict) else None
    expected_exit = {"PASS": 0, "FAIL": 1, "ERROR": 2}.get(reported_status)
    consistent = expected_exit is not None and execution.returncode == expected_exit
    status = "INTEGRITY_FAIL" if changed else reported_status if consistent else "ERROR"
    if not consistent:
        print(f"ERROR: scorer report status {reported_status!r} disagrees with "
              f"process exit code {execution.returncode}", file=sys.stderr)
    manifest = {
        "scope": "local-compatible-evaluator-not-official",
        "started_utc": started,
        "command": command,
        "status": status,
        "reported_status": reported_status,
        "report_exit_consistent": consistent,
        "exit_code": execution.returncode,
        "scorer_wall_seconds": seconds,
        "router_resources_measured_by_this_wrapper": False,
        "sha256_before": before,
        "sha256_after": after,
        "changed_files": changed,
        "report_sha256": digest(report),
        "artifacts": {p.name: digest(p) for p in sorted(output.iterdir()) if p.is_file()},
        "official_equivalence": False,
    }
    with (output / "run_manifest.json").open("x", encoding="utf-8") as target:
        json.dump(manifest, target, ensure_ascii=False, indent=2, allow_nan=False)
        target.write("\n")
    return 2 if changed or not consistent else execution.returncode


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, TypeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(2)
