#!/usr/bin/env bash
# 官方评测运行脚本：支持在 WSL2/Linux 环境下直接调用官方 score.py 评测布线结果

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${PROJECT_DIR}"

export LD_LIBRARY_PATH="${PROJECT_DIR}/evaluator:${LD_LIBRARY_PATH}"
export PYTHONPATH="${PROJECT_DIR}/evaluator:${PYTHONPATH}"

GUIDE_FILE="${1:-benchmarks/sample001/sample001.output.guide}"
LEF_FILE="${2:-benchmarks/sample001/sample001.input.lef}"
DEF_FILE="${3:-benchmarks/sample001/sample001.input.def}"
THERMAL_FILE="${4:-benchmarks/sample001/sample001.thermal.input.json}"
OUT_DIR="${5:-output/sample001}"

mkdir -p "${OUT_DIR}"

echo "================================================================"
echo "          启动 2026 EDA 官方评测程序 (score.py)                 "
echo "================================================================"
echo "Guide:   ${GUIDE_FILE}"
echo "LEF:     ${LEF_FILE}"
echo "DEF:     ${DEF_FILE}"
echo "Thermal: ${THERMAL_FILE}"
echo "OutDir:  ${OUT_DIR}"
echo "----------------------------------------------------------------"

python3 evaluator/score.py "${GUIDE_FILE}" \
    --lefs "${LEF_FILE}" \
    --input-def "${DEF_FILE}" \
    --thermal-json "${THERMAL_FILE}" \
    --output-dir "${OUT_DIR}"

echo "================================================================"
echo "评测完成！详细报告已输出至: ${OUT_DIR}/evaluation_report.json"
echo "================================================================"
