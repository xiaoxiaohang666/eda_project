"""Synthetic physical oracles; run on benchmark-host, never on the local workstation."""
from __future__ import annotations

from copy import deepcopy
import gzip
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace as NS
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
DEPENDENCY = Path(os.environ.get("SCORER_20261027_DEPENDENCY_ROOT",
                                ROOT / "references/community-score-0.4.2"))
sys.path.insert(0, str(DEPENDENCY))
from scorer_20261027.profile import (Options, congestion, covered_cells,
                                     first_layer_credit_source, physical_track_count,
                                     tsv_footprint_demands, interval_track_count,
                                     set_capacity_boundary, landing_tsv_demands)


class Pattern:
    def __init__(self, coords):
        self.coords = coords

    def coordinates(self):
        return self.coords


class GeometryOracles(unittest.TestCase):
    def make_run(self, *, components=(), guides=None, pins=None):
        grid = NS(x_lines=(0, 3, 6), y_lines=(0, 3, 6), rows=2, cols=2,
                  die=NS(x1=0, y1=0, x2=6, y2=6))
        tracks = {"metal10": {"Y": [Pattern(tuple(range(7)))]},
                  "metal11": {"X": [Pattern(tuple(range(7)))]}}
        run = NS(gcell_grid=grid, def_data=NS(tracks=tracks), gcell_width=3, gcell_height=3,
                 layer_directions={"metal10": "HORIZONTAL", "metal11": "VERTICAL"},
                 def_definition=NS(GetDatabaseUnits=lambda: 1, getComponents=lambda: components),
                 net_guides=guides or {}, _repair_pin_cells=pins or {}, _repair_io_positions={},
                 tsv_congestion_ratio=1.)
        common = NS(rectangle_cell_indices=covered_cells)
        return run, common

    def capacity(self, run):
        from evaluation.repaired.core import Capacity
        return Capacity(run.def_data.tracks, run.gcell_grid)

    def point(self, net="n", x=3, y=3):
        return NS(net=net, x_dbu=x, y_dbu=y, bottom_layer="metal10", top_layer="metal11",
                  gcell_col=1, gcell_row=1)

    def test_five_micron_footprint_crosses_cells_and_counts_both_layers(self):
        run, _ = self.make_run()
        got = dict(tsv_footprint_demands(self.point(), run.gcell_grid, self.capacity(run),
                                        run.layer_directions, 1))
        # [0.5,5.5) intersects two rows/cols; H counts 2 then 3, V likewise.
        self.assertEqual(got, {(0, 0, "metal10"): 2, (1, 0, "metal10"): 2,
                               (0, 1, "metal10"): 3, (1, 1, "metal10"): 3,
                               (0, 0, "metal11"): 2, (0, 1, "metal11"): 2,
                               (1, 0, "metal11"): 3, (1, 1, "metal11"): 3})
        self.assertEqual(sum(got.values()), 20)

    def test_physical_upper_edge_excluded_but_interior_final_track_retained(self):
        tracks = tuple(range(7))
        self.assertEqual(physical_track_count(tracks, 0, 6, 3, 6, final=True), 3)
        self.assertEqual(physical_track_count(tracks, -1, 7, 3, 6, final=True), 4)
        self.assertEqual(physical_track_count(tracks, 1, 3, 0, 3), 2)
        self.assertEqual(physical_track_count(tracks, 1, 3, 3, 6, final=True), 0)

    def test_die_clipping_does_not_clamp_outside_tracks_into_edge_cells(self):
        run, _ = self.make_run()
        got = dict(tsv_footprint_demands(self.point(x=1, y=1), run.gcell_grid,
                                        self.capacity(run), run.layer_directions, 1))
        self.assertEqual(sum(v for k, v in got.items() if k[2] == "metal10"), 8)
        self.assertEqual(sum(v for k, v in got.items() if k[2] == "metal11"), 8)
        self.assertEqual(list(covered_cells(-8, -2, run.gcell_grid.x_lines)), [])
        self.assertEqual(list(covered_cells(6, 8, run.gcell_grid.x_lines)), [])

    def test_cell_track_and_ceil_are_explicit_distinct_models(self):
        run, _ = self.make_run()
        cap = self.capacity(run)
        proportional = dict(tsv_footprint_demands(self.point(), run.gcell_grid, cap,
                                                 run.layer_directions, 1, model="cell-track"))
        rounded = dict(tsv_footprint_demands(self.point(), run.gcell_grid, cap,
                                            run.layer_directions, 1, model="ceil"))
        self.assertAlmostEqual(proportional[0, 1, "metal10"], 10 / 3)
        self.assertEqual(rounded[0, 1, "metal10"], 4)
        self.assertAlmostEqual(proportional[1, 0, "metal11"], 10 / 3)

    def test_multilayer_fixed_pin_uses_each_original_layer(self):
        comp = NS(macro_name="M", x=0, y=0, orient=0)
        run, common = self.make_run(components=[comp])
        box = ("0", "0", "2", "2")
        source = {"M": {"size": ("3", "3"),
                        "rectangles": [("metal10", box), ("metal11", box)],
                        "pins": {"P": [("metal10", box), ("metal11", box)]}}}
        original = deepcopy(source)
        audit = {}
        congestion(run, [], common, audit, source, Options())
        self.assertEqual(audit["congestion"]["fixed_track_demand_by_layer"],
                         {"metal10": 2, "metal11": 2})
        self.assertEqual(source, original)

    def test_first_layer_credit_keeps_geometry_without_projecting_other_shapes(self):
        source = {"M": {"size": ("3", "3"), "rectangles": [],
                        "pins": {"P": [("metal11", (0, 0, 1, 1)),
                                       ("metal10", (1, 1, 2, 2)),
                                       ("metal11", (2, 2, 3, 3))]}}}
        original = deepcopy(source)
        credit = first_layer_credit_source(source)
        self.assertEqual(credit["M"]["pins"]["P"],
                         [("metal11", (0, 0, 1, 1)), ("metal11", (2, 2, 3, 3))])
        self.assertEqual(source, original)

    def test_tsv_pin_exemption_is_per_net_cell_and_layer(self):
        run, common = self.make_run(pins={"n": {(0, 0, "metal10")}})
        audit = {}
        congestion(run, [self.point()], common, audit, {}, Options())
        self.assertEqual(audit["congestion"]["tsv_demand"], 18)
        self.assertEqual(audit["congestion"]["own_pin_tsv_exempt_demand"], 2)
        audit = {}
        congestion(run, [self.point(net="other")], common, audit, {}, Options())
        self.assertEqual(audit["congestion"]["tsv_demand"], 20)
        audit = {}
        congestion(run, [self.point()], common, audit, {}, Options(tsv_pin_exemption="none"))
        self.assertEqual(audit["congestion"]["tsv_demand"], 20)

    def test_distinct_tsv_instances_are_additive(self):
        run, common = self.make_run()
        audit = {}
        congestion(run, [self.point("a"), self.point("b")], common, audit, {}, Options())
        self.assertEqual(audit["congestion"]["tsv_demand"], 40)

    def test_route_same_net_cell_dedup_and_pin_exemption(self):
        rect = NS(xl=0, yl=0, xh=3, yh=3, layer="metal10")
        run, common = self.make_run(guides={"n": [rect, rect]})
        audit = {}
        congestion(run, [], common, audit, {}, Options())
        self.assertEqual(audit["congestion"]["guide_net_cell_demand"], 1)
        run._repair_pin_cells = {"n": {(0, 0, "metal10")}}
        congestion(run, [], common, audit, {}, Options())
        self.assertEqual(audit["congestion"]["guide_net_cell_demand"], 0)
        self.assertEqual(audit["congestion"]["own_pin_guide_exempt_demand"], 1)

    def test_capture_preserves_replayable_geometry_and_masks(self):
        run, common = self.make_run()
        run._scorer_20261027_pin_masks = {"all-layers": {"n": {(0, 0, "metal10")}},
                                         "first-layer": {"n": set()}}
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "state.json.gz"
            congestion(run, [self.point()], common, {}, {}, Options(dump_state=target))
            with gzip.open(target, "rt", encoding="utf-8") as stream:
                state = json.load(stream)
            self.assertEqual(state["tsv_points"][0]["x_dbu"], 3)
            self.assertEqual(state["tsv_points"][0]["net"], "n")
            self.assertEqual(state["pin_masks"]["all-layers"]["n"], [[0, 0, "metal10"]])
            self.assertFalse(state["official_equivalence"])
            self.assertEqual(state["tracks"]["metal10"]["Y"], list(range(7)))

    def test_rejects_undefined_model_parameters(self):
        for kwargs in ({"tsv_width_um": 0}, {"tsv_width_um": float("nan")},
                       {"tsv_demand_model": "case-special"}, {"zero_capacity_penalty": 0}):
            with self.assertRaises(ValueError):
                Options(**kwargs)

    def test_boundary_capacity_and_interior_reservation_are_distinct(self):
        run,_ = self.make_run()
        cap = self.capacity(run)
        self.assertEqual(cap[0,0,"metal10","HORIZONTAL"],3)
        set_capacity_boundary(cap,run.gcell_grid,"closed")
        self.assertEqual(cap[0,0,"metal10","HORIZONTAL"],4)
        self.assertEqual(interval_track_count(tuple(range(7)),3,6,"open"),2)
        self.assertEqual(interval_track_count(tuple(range(7)),3,6,"closed"),4)

    def test_landing_tsv_does_not_recharge_own_guide(self):
        guides={"n":[NS(xl=3,yl=3,xh=6,yh=6,layer=layer)
                     for layer in ("metal10","metal11")]}
        run,common=self.make_run(guides=guides)
        options=Options(tsv_demand_model="landing-union",capacity_boundary="closed",
                        fixed_track_boundary="open",landing_track_boundary="open")
        audit={}
        result=congestion(run,[self.point(x=4.5,y=4.5)],common,audit,{},options)
        # Two interior tracks per landing layer; one Guide track was already paid.
        self.assertEqual(audit["congestion"]["tsv_demand"],2)
        self.assertEqual(result[4][1,1,"metal10"],2)
        self.assertEqual(result[4][1,1,"metal11"],2)

    def test_landing_tsv_extends_partial_pin_reservation_without_double_charge(self):
        component=NS(macro_name="M",x=3,y=3,orient=0)
        guides={"n":[NS(xl=3,yl=3,xh=6,yh=6,layer=layer)
                     for layer in ("metal10","metal11")]}
        run,common=self.make_run(components=[component],guides=guides,
                                 pins={"n":{(1,1,"metal10")}})
        box=("0","0","2","2")
        source={"M":{"size":("3","3"),"rectangles":[("metal10",box)],
                     "pins":{"P":[("metal10",box)]}}}
        audit={}
        options=Options(tsv_demand_model="landing-union",capacity_boundary="closed",
                        fixed_track_boundary="open",landing_track_boundary="open")
        result=congestion(run,[self.point(x=4.5,y=4.5)],common,audit,source,options)
        # PIN reserves y=4 only; TSV landing adds y=5. The owned Guide stays exempt.
        self.assertEqual(result[4][1,1,"metal10"],2)
        self.assertEqual(audit["congestion"]["fixed_track_demand"],1)
        self.assertEqual(audit["congestion"]["tsv_demand"],2)

    def test_landing_reservation_clips_to_actual_entity_when_gcell_is_larger(self):
        run,_=self.make_run()
        cap=self.capacity(run)
        point=self.point(x=4.5,y=4.5)
        result=dict(landing_tsv_demands(point,run.gcell_grid,cap,run.layer_directions,
                                        1,.5,"open"))
        self.assertEqual(result,{(1,1,"metal10"):0,(1,1,"metal11"):0})


if __name__ == "__main__":
    unittest.main()
