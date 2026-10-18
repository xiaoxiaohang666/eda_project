#!/usr/bin/env python3
"""Prepare untouched FLUTE assets from the official CPython 3.10 score package.

Uses only the Python standard library. The official score is never executed.
Output is confined to this project and must be outside the official directory.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SCORE_SHA256 = "4104d0b2869ef7376d716ce9c33b56668e659575ab84615f5c0a5f282e108a8a"
BINARY_ASSETS = (
    "libflute.so",
    "tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so",
)
TABLE_ASSETS = ("POWV9.dat", "POST9.dat")
MANIFEST_NAME = "asset-manifest.json"
COOKIE = struct.Struct("!8sIIII64s")
TOC_ENTRY = struct.Struct("!iIIIBc")
MAGIC = bytes.fromhex("4d45490c0b0a0b0e")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def extract_binary_assets(data: bytes) -> tuple[dict[str, bytes], dict]:
    """Read only the two exact archive members; reject malformed boundaries."""
    cookie_at = data.rfind(MAGIC)
    if cookie_at < 0 or cookie_at + COOKIE.size > len(data):
        raise ValueError("score has no complete PyInstaller CArchive cookie")
    _, package_size, toc_offset, toc_size, python_version, python_lib = COOKIE.unpack_from(data, cookie_at)
    archive_start = cookie_at + COOKIE.size - package_size
    payload_size = package_size - COOKIE.size
    if (archive_start < 0 or payload_size < 0 or toc_size == 0
            or toc_offset + toc_size > payload_size):
        raise ValueError("invalid CArchive package/TOC boundaries")
    if python_version != 310:
        raise ValueError(f"official FLUTE requires CPython 3.10 assets; archive has {python_version}")
    toc_at = archive_start + toc_offset
    toc_end = toc_at + toc_size
    assets: dict[str, bytes] = {}
    entries = {}
    cursor = toc_at
    while cursor < toc_end:
        if cursor + TOC_ENTRY.size > toc_end:
            raise ValueError("truncated CArchive TOC header")
        entry_size, offset, stored_size, original_size, compression, kind = TOC_ENTRY.unpack_from(data, cursor)
        if entry_size <= TOC_ENTRY.size or cursor + entry_size > toc_end:
            raise ValueError("invalid CArchive TOC entry length")
        raw_name = data[cursor + TOC_ENTRY.size:cursor + entry_size]
        if b"\0" not in raw_name:
            raise ValueError("CArchive member name is not terminated")
        name = raw_name.split(b"\0", 1)[0].decode("utf-8")
        if offset + stored_size > toc_offset:
            raise ValueError(f"CArchive member escapes data area: {name}")
        if name in BINARY_ASSETS:
            if name in assets:
                raise ValueError(f"duplicate required CArchive member: {name}")
            if kind != b"b" or compression not in (0, 1):
                raise ValueError(f"unsupported binary CArchive member: {name}")
            member = data[archive_start + offset:archive_start + offset + stored_size]
            if compression:
                decoder = zlib.decompressobj()
                member = decoder.decompress(member) + decoder.flush()
                if not decoder.eof or decoder.unused_data:
                    raise ValueError(f"invalid compressed CArchive member: {name}")
            if len(member) != original_size:
                raise ValueError(f"CArchive uncompressed size mismatch: {name}")
            assets[name] = member
            entries[name] = {
                "archive_member": name,
                "offset": offset,
                "stored_size": stored_size,
                "compression": compression,
            }
        cursor += entry_size
    missing = set(BINARY_ASSETS) - assets.keys()
    if missing:
        raise ValueError("score is missing original FLUTE assets: " + ", ".join(sorted(missing)))
    metadata = {
        "archive_start": archive_start,
        "cookie_offset": cookie_at,
        "python_version": python_version,
        "python_library": python_lib.split(b"\0", 1)[0].decode("utf-8"),
        "entries": entries,
    }
    return assets, metadata


def _is_within(path: Path, parent: Path) -> bool:
    return path == parent or parent in path.parents


def _check_sources(paths: tuple[Path, ...], before: dict[str, str]) -> dict[str, str]:
    after = {str(path): hash_file(path) for path in paths}
    if after != before:
        raise ValueError("official source changed during preparation; no stable runtime can be claimed")
    return after


def _check_destination(path: Path, data: bytes) -> None:
    if path.is_symlink():
        raise ValueError(f"refusing output symlink: {path}")
    if path.exists() and (not path.is_file() or path.read_bytes() != data):
        raise ValueError(f"existing output differs; choose another directory: {path}")


def _write_new_or_identical(path: Path, data: bytes) -> None:
    _check_destination(path, data)
    if not path.exists():
        # Exclusive creation also prevents overwriting a file created after preflight.
        with path.open("xb") as output:
            output.write(data)


def prepare_runtime(official_dir: Path, output_dir: Path,
                    expected_score_sha256: str = DEFAULT_SCORE_SHA256) -> dict:
    official_dir = official_dir.resolve(strict=True)
    output_dir = output_dir.resolve()
    if not official_dir.is_dir():
        raise ValueError("official directory must be an existing directory")
    if not _is_within(output_dir, PROJECT_ROOT):
        raise ValueError(f"runtime output must be inside this project: {PROJECT_ROOT}")
    if output_dir == PROJECT_ROOT or _is_within(output_dir, official_dir):
        raise ValueError("runtime output must be a project subdirectory outside the official directory")
    if output_dir.exists() and not output_dir.is_dir():
        raise ValueError("runtime output must be a directory")
    expected_score_sha256 = expected_score_sha256.lower()
    if len(expected_score_sha256) != 64 or any(c not in "0123456789abcdef" for c in expected_score_sha256):
        raise ValueError("expected score SHA-256 must contain 64 hexadecimal characters")
    score = official_dir / "score"
    sources = (score,) + tuple(official_dir / name for name in TABLE_ASSETS)
    for source in sources:
        if not source.is_file():
            raise ValueError(f"missing official source file: {source}")
        if _is_within(source.resolve(), output_dir):
            raise ValueError(f"official source overlaps runtime output: {source}")
    before = {str(source): hash_file(source) for source in sources}
    if before[str(score)] != expected_score_sha256:
        raise ValueError(f"official score SHA-256 mismatch: expected {expected_score_sha256}, got {before[str(score)]}")
    score_bytes = score.read_bytes()
    if sha256_bytes(score_bytes) != before[str(score)]:
        raise ValueError("official score changed while reading")
    assets, archive = extract_binary_assets(score_bytes)
    for name in TABLE_ASSETS:
        source = official_dir / name
        assets[name] = source.read_bytes()
        if sha256_bytes(assets[name]) != before[str(source)]:
            raise ValueError(f"official table changed while reading: {source}")
    after = _check_sources(sources, before)
    manifest = {
        "format_version": 1,
        "preparer": "scripts/prepare_official_runtime.py",
        "expected_score_sha256": expected_score_sha256,
        "sources": [
            {"path": str(source), "sha256_before": before[str(source)],
             "sha256_after": after[str(source)]}
            for source in sources
        ],
        "archive": archive,
        "assets": [
            {"name": name, "size": len(assets[name]),
             "sha256": sha256_bytes(assets[name]),
             "origin": archive["entries"][name] if name in BINARY_ASSETS
             else {"source_file": str(official_dir / name)}}
            for name in (*BINARY_ASSETS, *TABLE_ASSETS)
        ],
    }
    manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    # Check every destination before creating the directory or writing any asset.
    for name, contents in (*assets.items(), (MANIFEST_NAME, manifest_bytes)):
        _check_destination(output_dir / name, contents)
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, contents in assets.items():
        _write_new_or_identical(output_dir / name, contents)
    _check_sources(sources, before)
    for name, contents in assets.items():
        if hash_file(output_dir / name) != sha256_bytes(contents):
            raise ValueError(f"runtime output changed while writing: {output_dir / name}")
    _write_new_or_identical(output_dir / MANIFEST_NAME, manifest_bytes)
    _check_sources(sources, before)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--official-dir", required=True, type=Path,
                        help="unchanged official directory containing score, POWV9.dat and POST9.dat")
    parser.add_argument("--output-dir", type=Path,
                        default=PROJECT_ROOT / "output/runtime/official-flute",
                        help="project output directory (default: output/runtime/official-flute)")
    parser.add_argument("--expected-score-sha256", default=DEFAULT_SCORE_SHA256,
                        help="expected original score digest; default pins the 20261012 official package")
    args = parser.parse_args()
    try:
        manifest = prepare_runtime(args.official_dir, args.output_dir, args.expected_score_sha256)
    except (OSError, ValueError, struct.error, UnicodeError, zlib.error) as error:
        print(f"prepare_official_runtime: {error}", file=sys.stderr)
        return 1
    print(f"official FLUTE runtime: {args.output_dir.resolve()}")
    print(f"source score SHA-256 unchanged: {manifest['sources'][0]['sha256_after']}")
    print(f"4 original assets verified; manifest: {args.output_dir.resolve() / MANIFEST_NAME}")
    print("Runtime dependency: Linux libpython3.10.so.1.0 (not copied or installed).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
