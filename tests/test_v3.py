"""v3 end-to-end regressions for fixed-TSV terminal routing.

Run with: python3 tests/test_v3.py /path/to/eda_router
The independent guide oracle does not require evaluator shared libraries.
"""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

BINARY = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[1]
CASE = ROOT / "benchmarks/sample001"
with patch.object(sys, "argv", [sys.argv[0], str(BINARY)]):
    import test_algorithm as oracle

INPUTS = ["--def", str(CASE / "sample001.input.def"),
          "--lef", str(CASE / "sample001.input.lef"),
          "--thermal-report", str(CASE / "sample001.thermal.input.json")]


def real_pin_checkpoints():
    """Derive actual fixture terminals from LEF geometry and DEF orientation.

    This deliberately does not import the C++ parser or use the package
    checker's untransformed macro-pin convention. Coordinates are in DEF DBU.
    """
    text = (CASE / "sample001.input.def").read_text()
    lef = (CASE / "sample001.input.lef").read_text()
    number = r"([-+\d.]+)"
    dbu = float(re.search(r"UNITS DISTANCE MICRONS\s+" + number, text)[1])

    def orient(x, y, direction):
        return {"N": (x, y), "S": (-x, -y), "W": (-y, x), "E": (y, -x),
                "FN": (-x, y), "FS": (x, -y), "FW": (y, x), "FE": (-y, -x)}[direction]

    macros = {}
    for name, body in re.findall(r"\bMACRO\s+(\w+)\b(.*?)END\s+\1\b", lef, re.S):
        size = tuple(map(float, re.search(r"SIZE\s+" + number + r"\s+BY\s+" + number, body).groups()))
        origin_match = re.search(r"ORIGIN\s+" + number + r"\s+" + number, body)
        origin = tuple(map(float, origin_match.groups())) if origin_match else (0.0, 0.0)
        pins = {}
        for pname, pbody in re.findall(r"\bPIN\s+(\w+)\b(.*?)END\s+\1\b", body, re.S):
            layer = re.search(r"LAYER\s+(metal\d+)\s*;", pbody)[1]
            rectangles = [tuple(map(float, rect)) for rect in re.findall(
                r"RECT\s+" + r"\s+".join([number] * 4), pbody)]
            if not rectangles:
                raise AssertionError(f"missing LEF pin rectangle: {name}/{pname}")
            bbox = (min(r[0] for r in rectangles), min(r[1] for r in rectangles),
                    max(r[2] for r in rectangles), max(r[3] for r in rectangles))
            pins[pname] = ((bbox[0] + bbox[2]) / 2, (bbox[1] + bbox[3]) / 2, layer)
        macros[name] = (size, origin, pins)
    components_text = text.split("COMPONENTS ", 1)[1].split("END COMPONENTS", 1)[0]
    components = {name: (macro, float(x), float(y), direction)
                  for name, macro, x, y, direction in re.findall(
                      r"-\s+(\S+)\s+(\S+)\s+\+\s+(?:PLACED|FIXED)\s*\(\s*" + number +
                      r"\s+" + number + r"\s*\)\s+(\w+)", components_text)}
    io = {}
    pins_text = text.split("PINS ", 1)[1].split("END PINS", 1)[0]
    for name, body in re.findall(r"-\s+(\S+)\s+(.*?);", pins_text, re.S):
        layer, x1, y1, x2, y2 = re.search(
            r"LAYER\s+(metal\d+)\s*\(\s*" + number + r"\s+" + number +
            r"\s*\)\s*\(\s*" + number + r"\s+" + number + r"\s*\)", body).groups()
        x, y, direction = re.search(
            r"(?:PLACED|FIXED)\s*\(\s*" + number + r"\s+" + number + r"\s*\)\s+(\w+)", body).groups()
        dx, dy = orient((float(x1) + float(x2)) / 2, (float(y1) + float(y2)) / 2, direction)
        io[name] = (float(x) + dx, float(y) + dy, layer)
    checkpoints = []
    net_text = text.split("NETS ", 1)[1].split("END NETS", 1)[0]
    for name, body in re.findall(r"-\s+(\S+)\s+(.*?);", net_text, re.S):
        for instance, pname in re.findall(r"\(\s*(\S+)\s+(\S+)\s*\)", body):
            if instance == "PIN":
                checkpoints.append((name, pname, *io[pname], "IO"))
                continue
            macro, x, y, direction = components[instance]
            (width, height), (ox, oy), pins = macros[macro]
            px, py, layer = pins[pname]
            dx, dy = orient(px + ox, py + oy, direction)
            # DEF placement anchors the lower left of the oriented SIZE box.
            corners = [orient(cx, cy, direction) for cx, cy in ((0, 0), (width, 0),
                                                               (0, height), (width, height))]
            dx -= min(point[0] for point in corners)
            dy -= min(point[1] for point in corners)
            checkpoints.append((name, instance + "." + pname, x + dx * dbu, y + dy * dbu,
                                layer, direction))
    return checkpoints


def rectangles_connected(rectangles):
    """Independent closed-rectangle adjacency on same or neighboring layers."""
    if not rectangles:
        return False
    visited, todo = {0}, [0]
    while todo:
        a = rectangles[todo.pop()]
        for index, b in enumerate(rectangles):
            if index in visited or abs(int(a[4][5:]) - int(b[4][5:])) > 1:
                continue
            if max(a[0], b[0]) <= min(a[2], b[2]) and max(a[1], b[1]) <= min(a[3], b[3]):
                visited.add(index)
                todo.append(index)
    return len(visited) == len(rectangles)


class V3Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory(prefix="eda-v3-", dir=BINARY.parent.parent)
        cls.directory = Path(cls.folder.name)
        cls.before = cls.route("disabled", ["--tsv-local", "0"])
        cls.full = cls.route("full", [])
        cls.repeat = cls.route("repeat", [])

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    @classmethod
    def route(cls, name, options):
        guide, stats = cls.directory / (name + ".guide"), cls.directory / (name + ".json")
        result = subprocess.run([str(BINARY), *INPUTS, "--router", "negotiated",
                                 "--time-limit", "600", "--output", str(guide),
                                 "--stats", str(stats), *options],
                                capture_output=True, text=True, timeout=690)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return guide, json.loads(stats.read_text(encoding="utf-8"))

    def test_all_metrics_nonworsening_and_real_length_improves(self):
        before, after = self.before[1]["metrics"], self.full[1]["metrics"]
        self.assertEqual(before, self.full[1]["pre_tsv_local_metrics"])
        for key in ("guide_overflow", "physical_overflow", "guide_mst_dbu",
                    "physical_length_dbu", "ordinary_vias"):
            self.assertLessEqual(after[key], before[key], key)
        self.assertGreaterEqual(after["quality"] + 1e-12, before["quality"])
        self.assertLess(after["physical_length_dbu"], before["physical_length_dbu"])
        self.assertEqual(after["physical_tsv_count"], before["physical_tsv_count"])
        self.assertEqual(after["evaluator_tsv_count"], before["evaluator_tsv_count"])
        self.assertEqual(self.full[1]["assignment"], self.before[1]["assignment"])
        self.assertEqual(after["correlation"], before["correlation"])

    def test_complete_guide_preserves_fixed_pins_and_tsv_assignments(self):
        checkpoints = real_pin_checkpoints()
        self.assertGreater(len(checkpoints), 400)
        net_text = (CASE / "sample001.input.def").read_text().split("NETS ", 1)[1].split("END NETS", 1)[0]
        used_io_count = len(re.findall(r"\(\s*PIN\s+\S+\s*\)", net_text))
        self.assertGreater(used_io_count, 0)
        self.assertEqual(sum(p[-1] == "IO" for p in checkpoints), used_io_count)
        self.assertGreater(sum(p[-1] == "FS" for p in checkpoints), 0)
        before_points = None
        for result in (self.before, self.full):
            nets, overflow, points = oracle.guide_accounting(result[0])
            self.assertEqual(len(nets), 203)
            self.assertTrue(all(nets.values()))
            self.assertEqual(overflow, result[1]["metrics"]["guide_overflow"])
            self.assertEqual(len(points), 3)
            self.assertEqual(len(points), result[1]["metrics"]["physical_tsv_count"])
            self.assertEqual(len(points), result[1]["metrics"]["evaluator_tsv_count"])
            for name, pin, x, y, layer, _ in checkpoints:
                self.assertTrue(any(r[4] == layer and r[0] <= x <= r[2] and r[1] <= y <= r[3]
                                    for r in nets[name]), f"missing actual pin {name}/{pin}")
            for name in ("vlink0", "vlink1", "vlink2"):
                self.assertTrue(rectangles_connected(nets[name]), f"disconnected guide: {name}")
                self.assertTrue(all(r[0] < r[2] and r[1] < r[3] for r in nets[name]))
            if before_points is None:
                before_points = points
            else:
                self.assertEqual(points, before_points, "fixed interface apertures must stay unchanged")
        oracle.AlgorithmTests.test_actual_oriented_pin_centers_are_covered(self)
        oracle.AlgorithmTests.test_fixed_pins_and_planned_slots_match_every_inferred_tsv(self)

    def test_stage_changes_only_fixed_cross_die_nets(self):
        before, after = oracle.parse_guide(self.before[0]), oracle.parse_guide(self.full[0])
        changed = {name for name in before if before[name] != after[name]}
        self.assertTrue(changed, "default stage must improve real routing on the original fixture")
        self.assertLessEqual(changed, {"vlink0", "vlink1", "vlink2"})
        self.assertEqual(set(before), set(after))

    def test_default_is_deterministic_and_switch_controls_stage(self):
        self.assertEqual(self.full[0].read_bytes(), self.repeat[0].read_bytes())
        for key in ("metrics", "assignment", "counters"):
            self.assertEqual(self.full[1][key], self.repeat[1][key], key)
        self.assertEqual(self.before[1]["options"]["tsv_local"], 0)
        self.assertEqual(self.full[1]["options"]["tsv_local"], 1)
        for key in ("tsv_local_candidates", "tsv_local_trials", "tsv_local_accepted"):
            self.assertEqual(self.before[1]["counters"][key], 0)
            self.assertGreater(self.full[1]["counters"][key], 0)

    def test_invalid_switch_preserves_existing_outputs(self):
        guide, stats = self.directory / "invalid.guide", self.directory / "invalid.json"
        guide.write_bytes(b"previous guide\n")
        stats.write_bytes(b"previous stats\n")
        for value in ("2", "-1", "true", "1.5"):
            with self.subTest(value=value):
                result = subprocess.run([str(BINARY), *INPUTS, "--tsv-local", value,
                                         "--output", str(guide), "--stats", str(stats)],
                                        capture_output=True, text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(guide.read_bytes(), b"previous guide\n")
                self.assertEqual(stats.read_bytes(), b"previous stats\n")

    def test_comparison_refuses_to_reuse_nonempty_output_directory(self):
        out = self.directory / "protected-comparison"
        out.mkdir()
        sentinel = out / "existing-evidence.txt"
        sentinel.write_bytes(b"keep previous evidence\n")
        result = subprocess.run([sys.executable, str(ROOT / "scripts/compare_router_v3.py"),
                                 "--v2-binary", str(BINARY), "--v3-binary", str(BINARY),
                                 "--output-dir", str(out)], capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("output directory is not empty", result.stderr)
        self.assertEqual(sentinel.read_bytes(), b"keep previous evidence\n")
        self.assertEqual(list(out.iterdir()), [sentinel])


if __name__ == "__main__":
    unittest.main()
