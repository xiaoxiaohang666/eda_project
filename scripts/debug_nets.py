"""Inspect package-checker pin coordinates (its orientation limitations still apply)."""
import argparse
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
EVALUATOR = ROOT / "evaluator"


def main():
    # The ELF loader reads LD_LIBRARY_PATH at process startup.
    if str(EVALUATOR) not in os.environ.get("LD_LIBRARY_PATH", "").split(os.pathsep):
        env = os.environ.copy()
        env["LD_LIBRARY_PATH"] = str(EVALUATOR) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
        os.execve(sys.executable, [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]], env)
    sys.path.insert(0, str(EVALUATOR))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-def", type=Path, default=ROOT / "benchmarks/sample001/sample001.input.def")
    parser.add_argument("--lefs", type=Path, action="append")
    parser.add_argument("--limit", type=int, default=10)
    args = parser.parse_args()
    if args.limit < 1:
        parser.error("--limit must be positive")
    try:
        import tse_framework_pybind_def as def_pybind
        import tse_framework_pybind_lef as lef_pybind
        from checker import get_position
        lefs = args.lefs or [ROOT / "benchmarks/sample001/sample001.input.lef"]
        d_def = def_pybind.DefParser().Parse(str(args.input_def))
        d_lef = lef_pybind.LefParser().Parse([str(path) for path in lefs])
        for net in d_def.getNets()[:args.limit]:
            print(f"Net: {net.name}")
            for conn in net.connections:
                pos = get_position(conn, d_lef, d_def, d_def.GetDatabaseUnits())
                print(f"  {conn.instance}.{conn.pin}: X={pos[0]}, Y={pos[1]}, Layer={pos[2]}")
    except (ImportError, OSError, ValueError, RuntimeError, KeyError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
