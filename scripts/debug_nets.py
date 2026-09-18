import sys
from pathlib import Path

# 添加评测器路径
eval_dir = Path(__file__).parent.parent / "evaluator"
sys.path.append(str(eval_dir))

import tse_framework_pybind_def as def_pybind
import tse_framework_pybind_lef as lef_pybind
from checker import get_position

def main():
    def_path = "benchmarks/sample001/sample001.input.def"
    lef_path = "benchmarks/sample001/sample001.input.lef"

    p_def = def_pybind.DefParser()
    d_def = p_def.Parse(def_path)
    p_lef = lef_pybind.LefParser()
    d_lef = p_lef.Parse([lef_path])

    dbu = d_def.GetDatabaseUnits()

    # 打印前 5 个网络的引脚真实坐标与所在层
    for net in d_def.getNets()[:10]:
        print(f"Net: {net.name}")
        for conn in net.connections:
            pos = get_position(conn, d_lef, d_def, dbu)
            print(f"  Conn: inst={conn.instance}, pin={conn.pin} -> X={pos[0]}, Y={pos[1]}, Layer={pos[2]}")

if __name__ == "__main__":
    main()
