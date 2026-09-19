#!/usr/bin/env bash
# 一键运行完整测试流程：构建 -> 生成 Route Guide -> 评分器验证
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${PROJECT_DIR}"

BUILD_TYPE="${1:-Release}"
BENCHMARK_DIR="${PROJECT_DIR}/benchmarks/sample001"
OUTPUT_DIR="${PROJECT_DIR}/output/sample001"

echo "================================================================"
echo "步骤 1/3: 构建 C++ 布线器 (${BUILD_TYPE})"
echo "================================================================"
bash "${PROJECT_DIR}/scripts/build.sh" "${BUILD_TYPE}"

echo ""
echo "================================================================"
echo "步骤 2/3: 运行布线器生成 Guide"
echo "================================================================"
mkdir -p "${OUTPUT_DIR}"
"${PROJECT_DIR}/build/bin/eda_router" \
    --input-def "${BENCHMARK_DIR}/sample001.input.def" \
    --lefs "${BENCHMARK_DIR}/sample001.input.lef" \
    --thermal-json "${BENCHMARK_DIR}/sample001.thermal.input.json" \
    -o "${OUTPUT_DIR}/router.guide"

echo ""
echo "================================================================"
echo "步骤 3/3: 运行赛题官方评估器 (score.py)"
echo "================================================================"
bash "${PROJECT_DIR}/scripts/run_eval.sh" \
    "${OUTPUT_DIR}/router.guide" \
    "${BENCHMARK_DIR}/sample001.input.lef" \
    "${BENCHMARK_DIR}/sample001.input.def" \
    "${BENCHMARK_DIR}/sample001.thermal.input.json" \
    "${OUTPUT_DIR}"

echo ""
echo "================================================================"
echo "全部流程执行完成！结果保存在: ${OUTPUT_DIR}/evaluation_report.json"
echo "================================================================"
