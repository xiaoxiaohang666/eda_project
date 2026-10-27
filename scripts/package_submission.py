#!/usr/bin/env python3
"""Create an official-format, relocatable submission in a fresh output directory.

Uses the current working-tree sources, including uncommitted files. Does not
include benchmarks, reference Guides, the official scorer, or Git metadata.
"""
from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ASSETS = (
    "libflute.so", "tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so",
    "POWV9.dat", "POST9.dat",
)
LIBRARIES = (
    "libpython3.10.so.1.0", "libstdc++.so.6", "libgcc_s.so.1",
    "libexpat.so.1", "libz.so.1",
)
COPYRIGHTS = {
    "libpython3.10.so.1.0": "libpython3.10",
    "libstdc++.so.6": "libstdc++6",
    "libgcc_s.so.1": "libgcc-s1",
    "libexpat.so.1": "libexpat1",
    "libz.so.1": "zlib1g",
}
DOCS = ("编译与运行说明.md", "设计架构.md", "算法原理.md", "第三方库说明.md", "优化结果对比.md")


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def command(*args: str) -> str:
    return subprocess.check_output(args, cwd=ROOT, text=True).strip()


def system_libraries() -> dict[str, Path]:
    libraries = {}
    cache = command("ldconfig", "-p")
    for name in LIBRARIES:
        matches = re.findall(r"^\s*" + re.escape(name) + r"\s+\([^\n]*x86-64[^\n]*\)\s+=>\s+(\S+)", cache, re.M)
        if not matches:
            raise ValueError(f"missing Linux x86-64 runtime library: {name}")
        libraries[name] = Path(matches[0]).resolve(strict=True)
    return libraries


def create_package(binary: Path, runtime: Path, destination: Path) -> dict:
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        raise ValueError("package on the Linux x86-64 main worktree")
    binary = binary.resolve(strict=True)
    runtime = runtime.resolve(strict=True)
    if binary.read_bytes()[:4] != b"\x7fELF":
        raise ValueError("router must be a compiled ELF executable")
    dependencies = subprocess.check_output(["ldd", str(binary)], text=True)
    if "libasan" in dependencies or "libubsan" in dependencies or "not found" in dependencies:
        raise ValueError("use a Release binary with all dependencies resolved")
    for path in (ROOT / "submission" / "docs" / name for name in DOCS):
        if not path.is_file():
            raise ValueError(f"missing official-format document: {path}")
    provenance = json.loads((runtime / "asset-manifest.json").read_text())
    expected = {item["name"]: item["sha256"] for item in provenance["assets"]}
    sources = {f"runtime/official-flute/{name}": runtime / name for name in ASSETS}
    for name in ASSETS:
        if digest(runtime / name) != expected[name]:
            raise ValueError(f"official runtime asset hash mismatch: {name}")
    sources["runtime/official-flute/asset-manifest.json"] = runtime / "asset-manifest.json"
    sources["router"] = binary
    for path in sorted((ROOT / "src").rglob("*")):
        if path.is_file() and path.suffix in {".cpp", ".hpp"}:
            sources[path.relative_to(ROOT).as_posix()] = path
    for path in sorted((ROOT / "submission").rglob("*")):
        if path.is_file():
            sources[path.relative_to(ROOT / "submission").as_posix()] = path
    for name, path in system_libraries().items():
        sources[f"lib/{name}"] = path
        copyright_file = Path("/usr/share/doc") / COPYRIGHTS[name] / "copyright"
        sources[f"licenses/{COPYRIGHTS[name]}-copyright.txt"] = copyright_file.resolve(strict=True)
    for name in ("GPL-3", "LGPL-2.1"):
        sources[f"licenses/{name}.txt"] = Path("/usr/share/common-licenses") / name
    before = {name: digest(path) for name, path in sources.items()}
    # Never replace an earlier candidate or its evidence.
    destination.mkdir(parents=True, exist_ok=False)
    package = destination / "contents"
    package.mkdir()
    for name, source in sources.items():
        target = package / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        target.chmod(0o755 if name in {"router", "run.sh", "build.sh"} or name.startswith("lib/") or name.endswith(".so") else 0o644)
        if digest(target) != before[name] or digest(source) != before[name]:
            raise ValueError(f"source changed while copying: {source}")
    manifest = {
        "format_version": 1,
        "created_at": datetime.now().astimezone().isoformat(),
        "worktree": str(ROOT),
        "git_head": command("git", "rev-parse", "HEAD"),
        "git_branch": command("git", "branch", "--show-current"),
        "includes_uncommitted_worktree_sources": True,
        "entrypoint": "run.sh",
        "target": "Linux x86-64, Ubuntu 22.04 / glibc >= 2.35",
        "profile": "v6 compatible-20261020/global-grid, preferred-direction guides, own-pin demand exemption, plateau W; heat=2, polish=32/8/bidirectional, score pricing, RRR=512; block/topology/recovery disabled",
        "files": [{"path": name, "size": (package / name).stat().st_size, "sha256": before[name]}
                  for name in sorted(sources)],
    }
    (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    archive = destination / "submission.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in sorted(package.rglob("*")):
            if path.is_file():
                zf.write(path, path.relative_to(package).as_posix())
    summary = {"archive": str(archive), "sha256": digest(archive), "size": archive.stat().st_size,
               "contents": str(package), "manifest_sha256": digest(package / "manifest.json")}
    (destination / "package-summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/bin/eda_router")
    parser.add_argument("--official-runtime", type=Path, default=ROOT / "output/runtime/official-flute")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(create_package(args.binary, args.official_runtime, args.output_dir.resolve()), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
