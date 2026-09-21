#!/usr/bin/env bash
# 调用仓库内的赛题评估器副本；结果仅反映该副本的检查和代理指标。
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${PROJECT_DIR}"

export LD_LIBRARY_PATH="${PROJECT_DIR}/evaluator${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export PYTHONPATH="${PROJECT_DIR}/evaluator${PYTHONPATH:+:${PYTHONPATH}}"

GUIDE_FILE="${1:-benchmarks/sample001/sample001.output.guide}"
LEF_FILE="${2:-benchmarks/sample001/sample001.input.lef}"
DEF_FILE="${3:-benchmarks/sample001/sample001.input.def}"
THERMAL_FILE="${4:-benchmarks/sample001/sample001.thermal.input.json}"
if (( $# > 5 )); then
    echo "[ERROR] Usage: run_eval.sh [guide [lef [def [thermal [output_dir]]]]]" >&2
    exit 1
fi
if [[ -n "${5:-}" ]]; then
    OUT_DIR="$5"
else
    mkdir -p output/evaluations
    OUT_DIR="$(mktemp -d output/evaluations/run-XXXXXXXX)"
fi

mkdir -p "${OUT_DIR}"

echo "================================================================"
echo "          启动仓库内赛题评估器副本 (score.py)                    "
echo "================================================================"
echo "Guide:   ${GUIDE_FILE}"
echo "LEF:     ${LEF_FILE}"
echo "DEF:     ${DEF_FILE}"
echo "Thermal: ${THERMAL_FILE}"
echo "OutDir:  ${OUT_DIR}"
echo "----------------------------------------------------------------"

python3 scripts/evaluate.py "${GUIDE_FILE}" \
    --lefs "${LEF_FILE}" \
    --input-def "${DEF_FILE}" \
    --thermal-json "${THERMAL_FILE}" \
    --output-dir "${OUT_DIR}"

echo "================================================================"
echo "评估完成。详细报告: ${OUT_DIR}/evaluation_report.json"
echo "================================================================"
