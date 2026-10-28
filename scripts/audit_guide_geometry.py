#!/usr/bin/env python3
"""Audit serialized Guide geometry against the observed 2026-10-24 width error.

This is a small independent engineering audit, not the unpublished platform
scorer. A clean report establishes only the listed checks; it does not certify
connectivity, PIN access, TSV legality, detailed DRC, or any official score.
Boundary trimming and nonuniform/multiple GCELLGRID declarations are unresolved.
Exit codes: 0 = listed checks clear, 1 = errors, 2 = unresolved checks only.
"""
from __future__ import annotations

import argparse
from collections import Counter
from decimal import Decimal, InvalidOperation
import hashlib
import json
from pathlib import Path
import re
import sys

PROVENANCE = "platform_log_20261024"
LIMIT = 20
NUMBER = r"[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?"


def without_comments(text: str) -> str:
    return re.sub(r"#[^\n]*", "", text)


def integer(value: str) -> int:
    try:
        number = Decimal(value)
    except InvalidOperation as exc:
        raise ValueError(f"invalid coordinate {value!r}") from exc
    if not number.is_finite() or number != number.to_integral_value():
        raise ValueError(f"non-finite or non-integer coordinate {value!r}")
    return int(number)


def read_definition(path: Path) -> tuple[tuple[int, ...], dict[str, list[dict]]]:
    text = without_comments(path.read_text(encoding="utf-8"))
    die = re.search(r"\bDIEAREA\s+(.+?);", text, re.I | re.S)
    if not die:
        raise ValueError("DEF has no DIEAREA")
    points = re.findall(r"\(\s*(" + NUMBER + r")\s+(" + NUMBER + r")\s*\)", die[1])
    if len(points) != 2:
        raise ValueError("audit supports a rectangular two-point DIEAREA only")
    bounds = tuple(integer(v) for point in points for v in point)
    if bounds[0] >= bounds[2] or bounds[1] >= bounds[3]:
        raise ValueError("DEF DIEAREA has non-positive area")
    grids = {"X": [], "Y": []}
    for match in re.finditer(r"\bGCELLGRID\s+([XY])\s+(" + NUMBER + r")\s+DO\s+(\d+)\s+STEP\s+(" + NUMBER + r")\s*;", text, re.I):
        axis, start, count, step = match.groups()
        item = {"start": integer(start), "count": int(count), "step": integer(step)}
        if item["count"] < 2 or item["step"] <= 0:
            raise ValueError("GCELLGRID count/step must define positive cells")
        grids[axis.upper()].append(item)
    for axis in grids:
        if not grids[axis]:
            raise ValueError(f"DEF has no supported GCELLGRID {axis} declaration")
    return bounds, grids


def read_layers(paths: list[Path]) -> dict[str, str]:
    layers = {}
    for path in paths:
        text = without_comments(path.read_text(encoding="utf-8"))
        # Technology LAYER blocks have TYPE immediately following their name;
        # geometry-use LAYER statements end with a semicolon and cannot match.
        pattern = r"\bLAYER\s+([^\s;]+)\s+TYPE\s+ROUTING\s*;(.*?)\bEND\s+\1(?=\s|$)"
        for match in re.finditer(pattern, text, re.I | re.S):
            name, body = match.groups()
            direction = re.search(r"\bDIRECTION\s+(HORIZONTAL|VERTICAL)\s*;", body, re.I)
            if not direction:
                raise ValueError(f"routing layer {name} has no supported DIRECTION")
            value = direction[1].upper()
            if name in layers and layers[name] != value:
                raise ValueError(f"conflicting DIRECTION for routing layer {name}")
            layers[name] = value
    if not layers:
        raise ValueError("LEF has no supported routing layer definitions")
    return layers


def audit(definition: Path, lefs: list[Path], guide: Path) -> dict:
    report = {
        "schema_version": 1,
        "rule_provenance": PROVENANCE,
        "official_scorer_run": False,
        "scope": "observed interior transverse-width rule and engineering syntax/geometry checks only",
        "observed_platform_checks": ["interior transverse extent equals one GCell"],
        "engineering_checks": ["Guide block syntax", "finite integer coordinates", "positive rectangle area", "known routing layer", "rectangle contained in DIEAREA"],
        "unresolved_rules": ["DIE boundary trimming", "nonuniform or multiple GCELLGRID declarations"],
        "not_checked": ["full official legality", "PIN coverage", "connectivity", "TSV mapping", "detailed DRC", "official score"],
        "inputs": {"def": str(definition), "lef": [str(p) for p in lefs], "guide": str(guide)},
        "guide_sha256": None,
        "errors": [], "unresolved": [],
    }
    counts = Counter()
    counts.update({"nets": 0, "rectangles": 0, "width_checked": 0, "errors": 0, "unresolved": 0})
    codes = Counter()

    def issue(category: str, code: str, message: str, **where) -> None:
        counts[category] += 1
        codes[code] += 1
        if len(report[category]) < LIMIT:
            report[category].append({"code": code, "message": message, **where})

    try:
        contents = guide.read_bytes()
        report["guide_sha256"] = hashlib.sha256(contents).hexdigest()
        die, grids = read_definition(definition)
        layers = read_layers(lefs)
        report["diearea"] = list(die)
        report["gcellgrid"] = grids
        report["routing_layers"] = layers
        text = contents.decode("utf-8")
    except (OSError, UnicodeError, ValueError) as exc:
        issue("errors", "input_error", str(exc))
        report.update(status="ERROR", summary=dict(counts), issue_counts=dict(codes), exit_code=1)
        return report

    net = None
    state = "net"
    seen_nets = set()
    net_rectangles = 0
    for line_number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        where = {"net": net, "line": line_number}
        if state == "net":
            if line in {"(", ")"} or len(line.split()) != 1:
                issue("errors", "guide_syntax", "expected a single net name", **where)
                continue
            net = line
            if net in seen_nets:
                issue("errors", "duplicate_net", "duplicate Guide net block", net=net, line=line_number)
            seen_nets.add(net)
            counts["nets"] += 1
            net_rectangles = 0
            state = "open"
            continue
        if state == "open":
            if line != "(":
                issue("errors", "guide_syntax", "expected opening parenthesis", **where)
            else:
                state = "rectangles"
            continue
        if line == ")":
            if not net_rectangles:
                issue("errors", "empty_net", "Guide net block has no rectangles", **where)
            state = "net"
            net = None
            continue
        fields = line.split()
        if len(fields) != 5:
            issue("errors", "guide_syntax", "expected xl yl xh yh layer", **where)
            continue
        counts["rectangles"] += 1
        net_rectangles += 1
        try:
            x1, y1, x2, y2 = (integer(value) for value in fields[:4])
        except ValueError as exc:
            issue("errors", "coordinate_format", str(exc), **where)
            continue
        layer = fields[4]
        where.update(layer=layer, rectangle=[x1, y1, x2, y2])
        if layer not in layers:
            issue("errors", "unknown_layer", "layer is not a parsed routing layer", **where)
            continue
        if x1 >= x2 or y1 >= y2:
            issue("errors", "nonpositive_rectangle", "rectangle must have positive area", **where)
            continue
        if x1 < die[0] or y1 < die[1] or x2 > die[2] or y2 > die[3]:
            # The unpublished scorer trims rectangles. Treat the exporter
            # invariant as an engineering error, without inventing its outcome.
            issue("errors", "engineering_outside_die", "rectangle extends beyond DIEAREA; official trim semantics are unknown", **where)
            continue
        vertical = layers[layer] == "VERTICAL"
        axis = "X" if vertical else "Y"
        low, high = (x1, x2) if vertical else (y1, y2)
        axis_low, axis_high = (die[0], die[2]) if vertical else (die[1], die[3])
        if len(grids[axis]) != 1:
            issue("unresolved", "multiple_grid_declarations", "transverse axis has multiple GCELLGRID declarations; official width selection is unknown", axis=axis, **where)
            continue
        grid = grids[axis][0]
        start, step = grid["start"], grid["step"]
        end = start + step * (grid["count"] - 1)
        width = high - low
        # A narrower cell touching a clipped DIE boundary is not proven invalid
        # by the interior rectangle in the supplied platform log.
        boundary_trim = ((low == axis_low and (axis_low - start) % step != 0)
                         or (high == axis_high and (axis_high - start) % step != 0))
        if boundary_trim and width != step:
            issue("unresolved", "boundary_trim", "transverse width differs at a non-grid-aligned DIE boundary; official trimming rule is unknown", width=width, nominal_pitch=step, **where)
            continue
        if low < start or high > end:
            issue("unresolved", "outside_declared_grid", "rectangle lies outside the declared transverse grid extent", **where)
            continue
        counts["width_checked"] += 1
        if width != step:
            issue("errors", "transverse_width", "interior transverse width must equal one GCell according to the observed platform error", width=width, expected_width=step, direction=layers[layer], **where)
    if state != "net":
        issue("errors", "guide_syntax", "unterminated net block", net=net)
    if counts["rectangles"] == 0:
        issue("errors", "empty_guide", "Guide contains no rectangle records")
    exit_code = 1 if counts["errors"] else 2 if counts["unresolved"] else 0
    report.update(status="ERROR" if exit_code == 1 else "UNRESOLVED" if exit_code == 2 else "KNOWN_CHECKS_CLEAR",
                  summary=dict(counts), issue_counts=dict(codes), exit_code=exit_code)
    return report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--def", required=True, dest="definition", type=Path)
    parser.add_argument("--lef", required=True, action="append", type=Path)
    parser.add_argument("--guide", required=True, type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args(argv)
    report = audit(args.definition, args.lef, args.guide)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("status", "rule_provenance", "official_scorer_run", "guide_sha256", "summary", "issue_counts")}, ensure_ascii=False))
    return report["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
