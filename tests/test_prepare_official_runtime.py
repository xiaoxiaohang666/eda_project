#!/usr/bin/env python3
"""Safety and byte-preservation checks for the standalone runtime preparer."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("prepare_runtime", ROOT / "scripts/prepare_official_runtime.py")
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


def archive(entries=None):
    entries = entries or [
        (runtime.BINARY_ASSETS[0], b"original native flute\0\xff", 1),
        (runtime.BINARY_ASSETS[1], b"original native extension\0\xfe", 0),
    ]
    payload = bytearray()
    toc = bytearray()
    for name, raw, compression in entries:
        stored = zlib.compress(raw) if compression else raw
        encoded_name = name.encode() + b"\0"
        toc.extend(runtime.TOC_ENTRY.pack(runtime.TOC_ENTRY.size + len(encoded_name),
                                         len(payload), len(stored), len(raw),
                                         compression, b"b") + encoded_name)
        payload.extend(stored)
    cookie = runtime.COOKIE.pack(runtime.MAGIC, len(payload) + len(toc) + runtime.COOKIE.size,
                                 len(payload), len(toc), 310, b"libpython3.10.so")
    # The real executable also has ELF data after its cookie.
    return b"ELF-prefix" + payload + toc + cookie + b"ELF-tail"


class PrepareRuntimeTests(unittest.TestCase):
    def setUp(self):
        test_parent = ROOT / "output/test-prepare-official-runtime"
        test_parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=test_parent)
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name)
        self.official = self.folder / "official"
        self.official.mkdir()
        self.score = archive()
        (self.official / "score").write_bytes(self.score)
        (self.official / "POWV9.dat").write_bytes(b"original POWV table\n")
        (self.official / "POST9.dat").write_bytes(b"original POST table\n")
        self.output = self.folder / "runtime"
        self.digest = runtime.sha256_bytes(self.score)

    def prepare(self):
        return runtime.prepare_runtime(self.official, self.output, self.digest)

    def test_exact_assets_source_hashes_manifest_and_idempotence(self):
        before = {p.name: p.read_bytes() for p in self.official.iterdir()}
        manifest = self.prepare()
        binaries, _ = runtime.extract_binary_assets(self.score)
        for name in runtime.BINARY_ASSETS:
            self.assertEqual((self.output / name).read_bytes(), binaries[name])
        for name in runtime.TABLE_ASSETS:
            self.assertEqual((self.output / name).read_bytes(), before[name])
        for item in manifest["sources"]:
            self.assertEqual(item["sha256_before"], item["sha256_after"])
        for item in manifest["assets"]:
            self.assertEqual(item["sha256"], runtime.hash_file(self.output / item["name"]))
        manifest_bytes = (self.output / runtime.MANIFEST_NAME).read_bytes()
        self.assertEqual(json.loads(manifest_bytes), manifest)
        self.assertEqual(self.prepare(), manifest)
        self.assertEqual((self.output / runtime.MANIFEST_NAME).read_bytes(), manifest_bytes)
        self.assertEqual({p.name: p.read_bytes() for p in self.official.iterdir()}, before)

    def test_different_asset_blocks_all_writes(self):
        self.output.mkdir()
        conflicting = self.output / runtime.BINARY_ASSETS[0]
        conflicting.write_bytes(b"keep this existing file")
        with self.assertRaisesRegex(ValueError, "existing output differs"):
            self.prepare()
        self.assertEqual(conflicting.read_bytes(), b"keep this existing file")
        self.assertEqual(list(self.output.iterdir()), [conflicting])

    def test_different_manifest_blocks_all_writes(self):
        self.output.mkdir()
        conflicting = self.output / runtime.MANIFEST_NAME
        conflicting.write_bytes(b"existing manifest")
        with self.assertRaisesRegex(ValueError, "existing output differs"):
            self.prepare()
        self.assertEqual(list(self.output.iterdir()), [conflicting])

    def test_input_digest_mismatch_does_not_create_output(self):
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            runtime.prepare_runtime(self.official, self.output, "0" * 64)
        self.assertFalse(self.output.exists())

    def test_output_cannot_enter_official_directory_even_through_symlink(self):
        for output in (self.official, self.official / "extracted"):
            with self.assertRaisesRegex(ValueError, "outside the official"):
                runtime.prepare_runtime(self.official, output, self.digest)
        linked = self.folder / "linked"
        linked.symlink_to(self.official, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "outside the official"):
            runtime.prepare_runtime(self.official, linked / "extracted", self.digest)
        self.assertEqual(len(list(self.official.iterdir())), 3)

    def test_output_must_stay_in_project(self):
        outside = ROOT.parent / "must-not-create-official-runtime"
        with self.assertRaisesRegex(ValueError, "inside this project"):
            runtime.prepare_runtime(self.official, outside, self.digest)

    def test_existing_output_symlink_is_refused(self):
        self.output.mkdir()
        target = self.output / runtime.TABLE_ASSETS[0]
        target.symlink_to(self.official / runtime.TABLE_ASSETS[0])
        with self.assertRaisesRegex(ValueError, "output symlink"):
            self.prepare()
        self.assertEqual(list(self.output.iterdir()), [target])

    def test_malformed_archive_is_rejected(self):
        duplicate = archive([
            (runtime.BINARY_ASSETS[0], b"a", 1),
            (runtime.BINARY_ASSETS[0], b"b", 0),
            (runtime.BINARY_ASSETS[1], b"c", 1),
        ])
        missing = archive([(runtime.BINARY_ASSETS[0], b"a", 1)])
        bad_length = bytearray(self.score)
        cookie_at = bad_length.rfind(runtime.MAGIC)
        start = cookie_at + runtime.COOKIE.size - runtime.COOKIE.unpack_from(bad_length, cookie_at)[1]
        toc_at = start + runtime.COOKIE.unpack_from(bad_length, cookie_at)[2]
        struct.pack_into("!i", bad_length, toc_at, 1)
        bad_size = bytearray(self.score)
        struct.pack_into("!I", bad_size, toc_at + 12, 999)
        for data in (b"not an archive", duplicate, missing, bytes(bad_length), bytes(bad_size)):
            with self.subTest(length=len(data)):
                with self.assertRaises(ValueError):
                    runtime.extract_binary_assets(data)


if __name__ == "__main__":
    unittest.main()
