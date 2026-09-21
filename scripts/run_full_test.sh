#!/usr/bin/env bash
# 一键运行：构建 -> 框架回归 -> 生成 Route Guide/独立检查 -> 固定版本评分器
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${PROJECT_DIR}"

BUILD_TYPE="${1:-Release}"
BENCHMARK_DIR="${PROJECT_DIR}/benchmarks/sample001"
if [[ "$BUILD_TYPE" == Release ]]; then
    DEFAULT_BUILD_DIR="$PROJECT_DIR/build"
else
    DEFAULT_BUILD_DIR="$PROJECT_DIR/build-$BUILD_TYPE"
fi
BUILD_DIR="${EDA_BUILD_DIR:-$DEFAULT_BUILD_DIR}"
mkdir -p "$PROJECT_DIR/output/runs"
OUTPUT_DIR="${2:-$(mktemp -d "$PROJECT_DIR/output/runs/sample001-XXXXXXXX")}"
if [[ -e "$OUTPUT_DIR/router.guide" || -e "$OUTPUT_DIR/evaluation_report.json" ]]; then
    echo "[ERROR] Output already contains a run; choose a new directory: $OUTPUT_DIR" >&2
    exit 1
fi

echo "================================================================"
echo "步骤 1/3: 构建 C++ 布线器 (${BUILD_TYPE})"
echo "================================================================"
bash "${PROJECT_DIR}/scripts/build.sh" "${BUILD_TYPE}"
ctest --test-dir "$BUILD_DIR" --output-on-failure --no-tests=error

echo ""
echo "================================================================"
echo "步骤 2/3: 运行布线器生成 Guide"
echo "================================================================"
mkdir -p "${OUTPUT_DIR}"
"$BUILD_DIR/bin/eda_router" \
    --input-def "${BENCHMARK_DIR}/sample001.input.def" \
    --lefs "${BENCHMARK_DIR}/sample001.input.lef" \
    --thermal-json "${BENCHMARK_DIR}/sample001.thermal.input.json" \
    -o "${OUTPUT_DIR}/router.guide"

echo ""
echo "================================================================"
echo "步骤 3/3: 运行仓库固定版本评估器 (score.py)"
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
