#!/usr/bin/env python3
"""Export boundary regressions using synthetic, project-owned input files."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

binary = Path(sys.argv[1]).resolve()
work = Path(sys.argv[2]).resolve()
work.mkdir(parents=True, exist_ok=True)
lef = work / "ports.lef"
definition = work / "ports.def"
thermal = work / "heat.json"
guide = work / "route.guide"
report = work / "report.json"
technology = "".join(
    f"LAYER metal{layer} TYPE ROUTING ; DIRECTION "
    f"{'HORIZONTAL' if layer % 2 else 'VERTICAL'} ; WIDTH 0.2 ; END metal{layer}\n"
    for layer in range(1, 21)
)
technology += """
MACRO own CLASS CORE ; SIZE 2 BY 2 ;
PIN P PORT LAYER metal1 ; RECT 0.4 0.4 0.8 0.8 ;
LAYER metal3 ; RECT 0.4 0.4 0.8 0.8 ; END END P
OBS LAYER metal1 ; RECT 0 0 2 2 ; LAYER metal3 ; RECT 0 0 2 2 ; END END own
MACRO foreign CLASS BLOCK ; SIZE 1 BY 1 ;
OBS LAYER metal1 ; RECT 0 0 1 1 ; END END foreign
"""
lef.write_text(technology)
thermal.write_text(json.dumps({"meta": {"ambientTemp": 25}, "nodes": [
    [5 + 10 * col, 5 + 10 * row, 0, 30]
    for row in range(10) for col in range(10)
]}))

def design(foreign=False):
    tracks = "".join(
        f"TRACKS {'Y' if layer % 2 else 'X'} 5 DO 100 STEP 10 LAYER metal{layer} ; "
        for layer in range(1, 21)
    )
    return (
        "UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 1000 1000 ) ; "
        "GCELLGRID X 0 DO 11 STEP 100 ; GCELLGRID Y 0 DO 11 STEP 100 ; "
        + tracks + f"COMPONENTS {2 if foreign else 1} ; "
        "- c own + PLACED ( 100 100 ) N ; "
        + ("- f foreign + PLACED ( 104 104 ) N ; " if foreign else "")
        + "END COMPONENTS PINS 1 ; "
        "- p + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 106 106 ) N ; "
        "END PINS NETS 1 ; - n0 ( c P ) ( PIN p ) ; END NETS END DESIGN"
    )

valid = "n0\n(\n104 104 108 108 metal1\n104 104 108 108 metal3\n)\n"
official_valid = valid.replace("104 104 108 108 metal3", "104 104 108 108 metal2\n104 104 108 108 metal3")
checks = 0

def run(expected, contents=None, foreign=False, extra=(), definition_text=None, technology_text=None):
    global checks
    if contents is None:
        official = "--model" in extra and extra[extra.index("--model") + 1] == "official"
        contents = official_valid if official else valid
    lef.write_text(technology if technology_text is None else technology_text)
    definition.write_text(design(foreign) if definition_text is None else definition_text)
    guide.write_text(contents)
    report.unlink(missing_ok=True)
    command = [str(binary), "--lef", str(lef), "--def", str(definition),
               "--thermal", str(thermal), "--guide", str(guide),
               "--report", str(report), *extra]
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    assert result.returncode == expected, (command, result.returncode, result.stdout, result.stderr)
    checks += 1
    if expected in (0, 1):
        value = json.loads(report.read_text())
        assert value["status"] == ("PASS" if expected == 0 else "FAIL")
        assert value["official_evaluator_status"] == "not_run"
        assert value["full_drc_certified"] is False
        checks += 3
        return value
    assert not report.exists(), "invalid parameters/input must not create a report"
    checks += 1
    return None

value = run(0)
assert value["strict_obs_enforced"] is True
assert value["pin_port_precedence_rectangles"] == 2
assert value["errors"]["connectivity"] == 0
checks += 3
value = run(1, foreign=True)
assert value["errors"]["die_or_obs"] > 0
checks += 1
value = run(1, contents=valid.replace("104 104 108 108 metal1", "103 103 109 109 metal1"))
assert value["errors"]["die_or_obs"] > 0
checks += 1
value = run(1, contents=valid.replace("104 104 108 108 metal1", "100 100 102 102 metal1"))
assert value["errors"]["pins"] > 0
checks += 1
value = run(1, contents=valid.replace("104 104 108 108 metal3", "300 300 302 302 metal3"))
assert value["errors"]["connectivity"] > 0
checks += 1
value = run(1, contents=valid.replace("104 104 108 108 metal1", "108 104 104 108 metal1"))
assert value["errors"]["die_or_obs"] > 0
checks += 1
run(2, contents=valid + valid)
run(2, extra=("--strict-obs", "maybe"))
# Model semantics are explicit and order-independent. The official model may
# cover another owner's OBS, while real PORT coverage and connectivity remain
# required even though physical Guide envelope OBS rejection is disabled.
value = run(0, extra=("--model", "physical"))
assert value["strict_obs_enforced"] is True
assert value["input_models"]["geometry"] == "physical-strict"
assert value["existing_pin_port_connectivity_enforced"] is True
checks += 3
value = run(0, foreign=True, extra=("--model", "official"))
assert value["strict_obs_enforced"] is False
assert value["input_models"]["model"] == "official"
assert value["input_models"]["geometry"] == "global-guide"
assert value["input_models"]["obs_interpretation"] == "capacity-demand"
assert value["input_models"]["thermal_strategy"] == "official-previous"
assert value["existing_pin_port_connectivity_enforced"] is False
checks += 6
# A multi-layer PORT cannot supply an implicit M1-to-M3 connection in official
# mode. Physical mode's existing-terminal geometry remains covered above.
value = run(1, contents=valid, extra=("--model", "official"))
assert value["errors"]["connectivity"] > 0
assert value["errors"]["pins"] == 0
checks += 2
run(0, foreign=True, extra=("--strict-obs", "0", "--model", "official"))
value = run(1, contents=valid.replace("104 104 108 108 metal1", "100 100 102 102 metal1"),
            extra=("--model", "official"))
assert value["errors"]["pins"] > 0
checks += 1
# Mirroring moves the real component PORT to x=112..116, while the fixed
# checker's untransformed point stays at x=106 inside these original Guides.
# Actual oriented PORT checks must reject it even when checker points are covered.
value = run(1, definition_text=design().replace("c own + PLACED ( 100 100 ) N", "c own + PLACED ( 100 100 ) FN"),
            extra=("--model", "official"))
assert value["errors"]["pins"] > 0
assert value["fixed_checker_coordinate_comparison"]["uncovered_checker_points"] == 0
checks += 2
value = run(1, contents=valid.replace("104 104 108 108 metal3", "300 300 302 302 metal3"),
            extra=("--model", "official"))
assert value["errors"]["connectivity"] > 0
checks += 1
for rectangle in ("103.5 104 108 108 metal1", "108 104 104 108 metal1", "104 104 1001 108 metal1"):
    value = run(1, contents=valid.replace("104 104 108 108 metal1", rectangle),
                extra=("--model", "official"))
    assert value["errors"]["die_or_obs"] > 0
    checks += 1
# Ordinary vias require positive XY area, and same-layer corners do not form
# connections. Use only IO terminals so a shared macro PORT cannot bridge the
# intentionally disconnected Guides. Exercise both geometric model settings.
contact_design = design().replace("PINS 1 ;", "PINS 2 ;").replace(
    "END PINS", "- q + LAYER metal3 ( -1 -1 ) ( 1 1 ) + FIXED ( 306 106 ) N ; END PINS"
).replace("- n0 ( c P ) ( PIN p ) ;", "- n0 ( PIN p ) ( PIN q ) ;")
for extra in (("--model", "official"), ("--strict-obs", "0")):
    for middle in ("150 104 160 108 metal2", "150 108 160 112 metal2"):
        contact = f"n0\n(\n104 104 150 108 metal1\n{middle}\n160 104 308 108 metal3\n)\n"
        value = run(1, contents=contact, definition_text=contact_design, extra=extra)
        assert value["errors"]["connectivity"] > 0
        assert value["errors"]["pins"] == 0
        checks += 2
    same_layer_edge = ("n0\n(\n104 104 150 108 metal1\n150 104 160 108 metal1\n"
                       "155 104 165 112 metal2\n160 104 308 108 metal3\n)\n")
    run(0, contents=same_layer_edge, definition_text=contact_design, extra=extra)
    value = run(1, contents=same_layer_edge.replace("150 104 160 108", "150 108 160 112"),
                definition_text=contact_design, extra=extra)
    assert value["errors"]["connectivity"] > 0
    assert value["errors"]["pins"] == 0
    checks += 2
run(2, extra=("--model", "unknown"))
run(2, extra=("--model", "official", "--strict-obs", "1"))
run(2, extra=("--strict-obs", "1", "--model", "official"))

# Full official GCELLGRID lines extend beyond DIEAREA and must survive reload;
# the default physical model keeps its clipped ten-cell grid unchanged.
boundary = design().replace("DO 11 STEP 100", "DO 13 STEP 100")
value = run(0, definition_text=boundary)
assert value["input_models"]["grid"] == "clipped-physical"
assert value["input_models"]["grid_cols"] == value["input_models"]["grid_rows"] == 10
checks += 2
value = run(0, definition_text=boundary, extra=("--model", "official"))
assert value["input_models"]["grid"] == "official-full"
assert value["input_models"]["grid_cols"] == value["input_models"]["grid_rows"] == 12
assert "official-previous-missing" in value["input_models"]["thermal"]
checks += 3

# The actual TSV/VIA center is 108.5 DBU. Outward-rounded integer Guides below
# infer center 109, so exact center equality would falsely reject a legal
# envelope in the same thermal GCell. These fixtures use project-owned input.
tsv_technology = technology + """
MACRO RESOURCE_SLOT_bottom SIZE 2 BY 2 ; END RESOURCE_SLOT_bottom
MACRO RESOURCE_SLOT_top SIZE 2 BY 2 ; END RESOURCE_SLOT_top
MACRO TSV_BOTTOM SIZE 2 BY 2 ; PIN TSV_PORT PORT LAYER metal10 ;
RECT 0.65 0.65 1.05 1.05 ; END END TSV_PORT END TSV_BOTTOM
MACRO TSV_TOP SIZE 2 BY 2 ; PIN TSV_PORT PORT LAYER metal11 ;
RECT 0.65 0.65 1.05 1.05 ; END END TSV_PORT END TSV_TOP
LAYER tsv TYPE CUT ; END tsv
VIA tsv_v DEFAULT LAYER metal10 ; RECT -0.2 -0.2 0.2 0.2 ;
LAYER tsv ; RECT -0.1 -0.1 0.1 0.1 ;
LAYER metal11 ; RECT -0.2 -0.2 0.2 0.2 ; END tsv_v
"""
tracks = "".join(
    f"TRACKS {'Y' if layer % 2 else 'X'} 5 DO 100 STEP 10 LAYER metal{layer} ; "
    for layer in range(1, 21)
)
def tsv_design(two_nets=False):
    return (
        "UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 1000 1000 ) ; "
        "GCELLGRID X 0 DO 11 STEP 100 ; GCELLGRID Y 0 DO 11 STEP 100 ; "
        + tracks + "COMPONENTS 4 ; "
        "- rb RESOURCE_SLOT_bottom + PLACED ( 100 100 ) N ; "
        "- rt RESOURCE_SLOT_top + PLACED ( 100 100 ) N ; "
        "- b TSV_BOTTOM + PLACED ( 100 100 ) N ; "
        "- t TSV_TOP + PLACED ( 100 100 ) N ; END COMPONENTS "
        f"NETS {2 if two_nets else 1} ; - n0 ( b TSV_PORT ) ( t TSV_PORT ) ; "
        + ("- n1 ( b TSV_PORT ) ( t TSV_PORT ) ; " if two_nets else "")
        + "END NETS END DESIGN"
    )
rounded = "n0\n(\n106 106 112 112 metal10\n106 106 112 112 metal11\n)\n"
for extra in (("--model", "official"), ("--model", "official", "--tsv-resources", "projected"), ()):
    value = run(0, contents=rounded, definition_text=tsv_design(),
                technology_text=tsv_technology, extra=extra)
    assert value["checked"]["cross_die_nets"] == 1
    assert value["errors"]["tsv_mapping"] == 0
    checks += 2
value = run(1, contents=rounded.replace("106 106 112 112", "108 108 109 109"),
            definition_text=tsv_design(), technology_text=tsv_technology, extra=("--model", "official"))
assert value["errors"]["tsv_mapping"] > 0
checks += 1
value = run(1, contents=rounded + rounded.replace("n0", "n1"),
            definition_text=tsv_design(True), technology_text=tsv_technology,
            extra=("--model", "official"))
assert value["errors"]["tsv_mapping"] > 0
assert any("reservation reused" in message for message in value["error_examples"])
checks += 2

# Uniqueness and geometric reservation conflicts remain enforced in the
# official model; allowing OBS envelopes is not a waiver of TSV hard rules.
value = run(1, contents=rounded.replace("112 112 metal11\n", "112 112 metal11\n110 106 115 112 metal11\n"),
            definition_text=tsv_design(), technology_text=tsv_technology, extra=("--model", "official"))
assert value["errors"]["tsv_mapping"] > 0
checks += 1
conflicting = tsv_design().replace("COMPONENTS 4 ;", "COMPONENTS 8 ;").replace(
    "END COMPONENTS", "- rb2 RESOURCE_SLOT_bottom + PLACED ( 110 100 ) N ; "
    "- rt2 RESOURCE_SLOT_top + PLACED ( 110 100 ) N ; "
    "- b2 TSV_BOTTOM + PLACED ( 110 100 ) N ; "
    "- t2 TSV_TOP + PLACED ( 110 100 ) N ; END COMPONENTS"
).replace("NETS 1 ;", "NETS 2 ;").replace(
    "END NETS", "- n1 ( b2 TSV_PORT ) ( t2 TSV_PORT ) ; END NETS"
)
second_landing = "n1\n(\n116 106 122 112 metal10\n116 106 122 112 metal11\n)\n"
value = run(1, contents=rounded + second_landing, definition_text=conflicting,
            technology_text=tsv_technology, extra=("--model", "official"))
assert value["errors"]["tsv_mapping"] > 0
assert any("geometric TSV conflict" in message for message in value["error_examples"])
checks += 2

# Output guards must reject a report directed at any of the explicitly supplied
# inputs before truncating or parsing it. This test never touches official data.
lef.write_text(technology)
definition.write_text(design())
guide.write_text(valid)
before = hashlib.sha256(guide.read_bytes()).digest()
result = subprocess.run([str(binary), "--lef", str(lef), "--def", str(definition),
                         "--thermal", str(thermal), "--guide", str(guide),
                         "--report", str(guide)], capture_output=True, text=True, timeout=30)
assert result.returncode == 2 and "cannot overwrite input" in result.stderr
assert hashlib.sha256(guide.read_bytes()).digest() == before
checks += 2
print(f"PASS: {checks} export validation checks")
