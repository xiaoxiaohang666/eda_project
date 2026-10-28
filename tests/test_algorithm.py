"""End-to-end algorithm regressions; independent Python guide accounting, no .so."""
import bisect
import collections
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[1]
CASE = ROOT / "benchmarks/sample001"
INPUTS = ["--def", str(CASE / "sample001.input.def"),
          "--lef", str(CASE / "sample001.input.lef"),
          "--thermal-report", str(CASE / "sample001.thermal.input.json")]


def parse_guide(path):
    nets = {}
    name = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        fields = raw.split()
        if not fields or fields == ["("]:
            continue
        if fields == [")"]:
            name = None
        elif name is None:
            if fields[0] in nets:
                raise AssertionError("duplicate net")
            name = fields[0]
            nets[name] = []
        else:
            nets[name].append((*map(int, fields[:4]), fields[4]))
    return nets


def guide_accounting(guide):
    """Independent rectangle-demand and positive-area TSV reference model."""
    text = (CASE / "sample001.input.def").read_text()
    die_match = re.search(r"DIEAREA\s*\(\s*(\d+)\s+(\d+)\s*\)\s*\(\s*(\d+)\s+(\d+)\s*\)", text)
    die = list(map(int, die_match.groups()))
    axes = {}
    for axis, lo, hi in [("X", die[0], die[2]), ("Y", die[1], die[3])]:
        lines = set()
        for start, count, step in re.findall(r"GCELLGRID\s+" + axis + r"\s+(-?\d+)\s+DO\s+(\d+)\s+STEP\s+(\d+)", text):
            lines.update(int(start) + index * int(step) for index in range(int(count)))
        axes[axis] = sorted(lines | {lo, hi})
    lef = (CASE / "sample001.input.lef").read_text()
    directions = dict(re.findall(r"LAYER\s+(metal\d+)\s+TYPE\s+ROUTING\s*;[\s\S]*?DIRECTION\s+(HORIZONTAL|VERTICAL)", lef))
    capacity = collections.Counter()
    for axis, start, count, step, layer in re.findall(r"TRACKS\s+([XY])\s+(-?\d+)\s+DO\s+(\d+)\s+STEP\s+(\d+)\s+LAYER\s+(metal\d+)", text):
        lines = axes[axis]
        for index in range(int(count)):
            value = int(start) + index * int(step)
            cell = min(bisect.bisect_right(lines, value) - 1, len(lines) - 2)
            if lines[0] <= value <= lines[-1]:
                capacity[(axis, layer, cell)] += 1
    demand = collections.Counter()
    overlaps = []
    nets = parse_guide(guide)
    for name, rectangles in nets.items():
        if len(rectangles) != len(set(rectangles)):
            raise AssertionError("duplicate rectangles")
        for x1, y1, x2, y2, layer in rectangles:
            for col in range(bisect.bisect_right(axes["X"], x1) - 1,
                             bisect.bisect_left(axes["X"], x2)):
                for row in range(bisect.bisect_right(axes["Y"], y1) - 1,
                                 bisect.bisect_left(axes["Y"], y2)):
                    demand[(col, row, layer)] += 1
        lower = [r for r in rectangles if r[4] == "metal10"]
        upper = [r for r in rectangles if r[4] == "metal11"]
        for a in lower:
            for b in upper:
                x1, y1 = max(a[0], b[0]), max(a[1], b[1])
                x2, y2 = min(a[2], b[2]), min(a[3], b[3])
                if x1 < x2 and y1 < y2:
                    overlaps.append((name, x1, y1, x2, y2))
    overflow = 0
    for (col, row, layer), used in demand.items():
        axis, cell = ("X", col) if directions[layer] == "HORIZONTAL" else ("Y", row)
        overflow += max(0, used - capacity[(axis, layer, cell)])
    return nets, overflow, overlaps


class AlgorithmTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory(prefix="eda-algorithm-", dir=BINARY.parent.parent)
        cls.directory = Path(cls.folder.name)
        # Initial metrics precede every optional optimization stage. Frozen
        # earlier binaries do not advertise the v3 switch.
        help_result = subprocess.run([str(BINARY), "--help"], capture_output=True,
                                     text=True, timeout=30)
        tsv_disabled = ["--tsv-local", "0"] if "--tsv-local" in help_result.stdout else []
        cls.initial = cls.route("initial", ["--rrr-rounds", "0", "--tsv-rounds", "0",
                                            "--wirelength-rounds", "0", *tsv_disabled])
        cls.full = cls.route("full", [])
        cls.repeat = cls.route("repeat", [])

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    @classmethod
    def route(cls, name, options):
        guide, stats = cls.directory / (name + ".guide"), cls.directory / (name + ".json")
        command = [str(BINARY), *INPUTS, "--router", "negotiated", "--time-limit", "600",
                   "--output", str(guide), "--stats", str(stats), *options]
        result = subprocess.run(command, capture_output=True, text=True, timeout=650)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return guide, json.loads(stats.read_text(encoding="utf-8"))

    def test_deterministic_complete_result(self):
        self.assertEqual(self.full[0].read_bytes(), self.repeat[0].read_bytes())
        self.assertEqual(self.full[1]["metrics"], self.repeat[1]["metrics"])
        self.assertEqual(self.full[1]["assignment"], self.repeat[1]["assignment"])
        nets, overflow, points = guide_accounting(self.full[0])
        self.assertEqual(len(nets), 203)
        self.assertEqual(overflow, self.full[1]["metrics"]["guide_overflow"])
        self.assertEqual(len(points), self.full[1]["metrics"]["evaluator_tsv_count"])
        self.assertEqual(len(points), self.full[1]["metrics"]["physical_tsv_count"])
        self.assertEqual(len(points), 3)
        self.assertEqual({p[0] for p in points}, {"vlink0", "vlink1", "vlink2"})
        for name, rects in nets.items():
            layers = [int(rect[4][5:]) for rect in rects]
            if name.startswith("BOTTOM__"):
                self.assertLessEqual(max(layers), 10)
            if name.startswith("TOP__"):
                self.assertGreaterEqual(min(layers), 11)

    def test_optimization_preserves_best_complete_solution(self):
        before, after = self.initial[1]["metrics"], self.full[1]["metrics"]
        self.assertLessEqual(after["guide_overflow"], before["guide_overflow"])
        if after["guide_overflow"] == before["guide_overflow"]:
            self.assertLessEqual(after["physical_overflow"], before["physical_overflow"])
            if after["physical_overflow"] == before["physical_overflow"]:
                self.assertGreaterEqual(after["quality"] + 1e-6, before["quality"])
        self.assertEqual(self.full[1]["initial_metrics"], self.initial[1]["metrics"])
        self.assertGreater(self.full[1]["counters"]["policy_rejections"], 0)

    def test_fixed_pins_and_planned_slots_match_every_inferred_tsv(self):
        nets, _, points = guide_accounting(self.full[0])
        assignments = {item["net"]: item for item in self.full[1]["assignment"]}
        self.assertEqual(len(assignments), 3)
        self.assertEqual(len({item["slot"] for item in assignments.values()}), 3)
        fixed = {"vlink0": (51390, 67340), "vlink1": (188990, 67340),
                 "vlink2": (118590, 156940)}
        for name, x1, y1, x2, y2 in points:
            x, y = fixed[name]
            self.assertLessEqual(x1, x)
            self.assertLessEqual(y1, y)
            self.assertGreaterEqual(x2, x)
            self.assertGreaterEqual(y2, y)
            self.assertLessEqual(x1, assignments[name]["x"])
            self.assertLessEqual(y1, assignments[name]["y"])
            self.assertGreaterEqual(x2, assignments[name]["x"])
            self.assertGreaterEqual(y2, assignments[name]["y"])

    def test_actual_oriented_pin_centers_are_covered(self):
        # Coverage follows DEF orientation and LEF geometry. The historical
        # evaluator's untransformed checkpoints are not routing terminals.
        text = (CASE / "sample001.input.def").read_text()
        lef = (CASE / "sample001.input.lef").read_text()
        dbu = float(re.search(r"UNITS DISTANCE MICRONS (\d+)", text).group(1))
        macros = {}
        for name, body in re.findall(r"\bMACRO\s+(\w+)\b(.*?)END\s+\1\b", lef, re.S):
            w, h = map(float, re.search(r"SIZE\s+([\d.]+)\s+BY\s+([\d.]+)", body).groups())
            origin = re.search(r"ORIGIN\s+([-\d.]+)\s+([-\d.]+)", body)
            ox, oy = map(float, origin.groups()) if origin else (0., 0.)
            pins = {}
            for pname, pbody in re.findall(r"\bPIN\s+(\w+)\b(.*?)END\s+\1\b", body, re.S):
                match = re.search(r"LAYER\s+(metal\d+)\s*;\s*RECT\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)", pbody)
                if match:
                    layer, x1, y1, x2, y2 = match.groups()
                    pins[pname] = ((float(x1)+float(x2))/2+ox,(float(y1)+float(y2))/2+oy,layer)
            macros[name] = (pins,w,h)
        components_text = text.split("COMPONENTS ",1)[1].split("END COMPONENTS",1)[0]
        components = {name:(macro,int(x),int(y),orient) for name,macro,x,y,orient in re.findall(
            r"-\s+(\S+)\s+(\S+)\s+\+\s+(?:PLACED|FIXED)\s*\(\s*(-?\d+)\s+(-?\d+)\s*\)\s+(N|S|E|W|FN|FS|FE|FW)\b",components_text)}
        guide = parse_guide(self.full[0]);count=0
        net_text=text.split("NETS ",1)[1].split("END NETS",1)[0]
        for name,body in re.findall(r"-\s+(\S+)\s+(.*?);",net_text,re.S):
            for instance,pname in re.findall(r"\(\s*(\S+)\s+(\S+)\s*\)",body):
                if instance=="PIN":continue
                macro,x,y,orient=components[instance];pins,w,h=macros[macro]
                px,py,layer=pins[pname]
                px,py={'N':(px,py),'S':(w-px,h-py),'E':(py,w-px),'W':(h-py,px),
                       'FN':(w-px,py),'FS':(px,h-py),'FW':(py,px),'FE':(h-py,w-px)}[orient]
                px,py=x+px*dbu,y+py*dbu
                self.assertTrue(any(r[4]==layer and r[0]<=px<=r[2] and r[1]<=py<=r[3]
                                    for r in guide[name]),f"missing real PIN {name}/{instance}.{pname}")
                count+=1
        self.assertGreater(count,400)

    def test_movable_fixture_accepts_candidates_and_rolls_back_later_trials(self):
        # A synthetic derivative enables movement by removing only the six
        # paired fixed interface IO endpoints. Original benchmark stays intact.
        original = (CASE / "sample001.input.def").read_text()
        fixture, count = re.subn(r"\s*-\s+vlink\d+_(?:bottom|top)\b[^;]*;", "", original)
        self.assertEqual(count, 6)
        fixture, count = re.subn(r"(\bPINS\s+)12(\s*;)", r"\g<1>6\2", fixture)
        self.assertEqual(count, 1)
        fixture, count = re.subn(r"\(\s*PIN\s+vlink\d+_(?:bottom|top)\s*\)", "", fixture)
        self.assertEqual(count, 6)
        path = self.directory / "movable.def"
        path.write_text(fixture)
        common = ["--def", str(path), "--rrr-rounds", "0", "--wirelength-rounds", "0", "--pin-local", "0"]
        prefix = self.route("movable-prefix", [*common, "--tsv-rounds", "2"])
        full = self.route("movable-full", [*common, "--tsv-rounds", "10"])
        repeated = self.route("movable-repeat", [*common, "--tsv-rounds", "10"])
        self.assertGreater(full[1]["counters"]["tsv_accepted"], 0)
        self.assertGreater(full[1]["counters"]["rollbacks"], 0)
        self.assertGreater(full[1]["counters"]["tsv_trials"], prefix[1]["counters"]["tsv_trials"])
        self.assertEqual(prefix[0].read_bytes(), full[0].read_bytes(),
                         "later unsuccessful transactions must restore the prior complete guide")
        self.assertEqual(prefix[1]["metrics"], full[1]["metrics"])
        self.assertEqual(prefix[1]["assignment"], full[1]["assignment"])
        self.assertEqual(full[0].read_bytes(), repeated[0].read_bytes())
        self.assertEqual(full[1]["metrics"], repeated[1]["metrics"])
        self.assertEqual(full[1]["assignment"], repeated[1]["assignment"])
        _, overflow, points = guide_accounting(full[0])
        self.assertEqual(overflow, full[1]["metrics"]["guide_overflow"])
        self.assertEqual(len(points), 3)
        self.assertEqual(full[1]["metrics"]["physical_tsv_count"], 3)
        self.assertEqual((CASE / "sample001.input.def").read_text(), original)

    def test_search_limit_does_not_emit_partial_solution(self):
        output = self.directory / "limited.guide"
        stats = self.directory / "limited.json"
        result = subprocess.run([str(BINARY), *INPUTS, "--search-limit", "1",
                                 "--output", str(output), "--stats", str(stats)],
                                capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(output.exists())
        self.assertFalse(stats.exists())
        self.assertIn("SEARCH_LIMIT", result.stdout + result.stderr)
        output.write_bytes(b"previous verified guide\n")
        stats.write_bytes(b"previous verified stats\n")
        repeated = subprocess.run([str(BINARY), *INPUTS, "--search-limit", "1",
                                   "--output", str(output), "--stats", str(stats)],
                                  capture_output=True, text=True, timeout=30)
        self.assertNotEqual(repeated.returncode, 0)
        self.assertEqual(output.read_bytes(), b"previous verified guide\n")
        self.assertEqual(stats.read_bytes(), b"previous verified stats\n")

    def test_invalid_configuration_does_not_touch_outputs(self):
        for index, options in enumerate([
            ["--rrr-rounds", "-1"], ["--tsv-rounds", "abc"], ["--search-limit", "0"],
            ["--time-limit", "nan"], ["--search", "greedy"], ["--router", "unknown"],
            ["--local-repair", "2"],
        ]):
            output = self.directory / f"bad-{index}.guide"
            result = subprocess.run([str(BINARY), *INPUTS, "--output", str(output), *options],
                                    capture_output=True, text=True, timeout=30)
            with self.subTest(options=options):
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(output.exists())

    def test_stats_cannot_overwrite_input_or_guide(self):
        input_path = Path(INPUTS[1])
        before = input_path.read_bytes()
        output = self.directory / "stats-alias.guide"
        for stats in [input_path, output]:
            result = subprocess.run([str(BINARY), *INPUTS, "--output", str(output),
                                     "--stats", str(stats)], capture_output=True, text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(input_path.read_bytes(), before)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
