#!/usr/bin/env bash
set -euo pipefail

# 3D IC 全局布线框架与基线构建脚本
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TYPE="${1:-Release}"
case "$BUILD_TYPE" in
    Release) DEFAULT_BUILD_DIR="$PROJECT_DIR/build" ;;
    Debug|RelWithDebInfo|MinSizeRel) DEFAULT_BUILD_DIR="$PROJECT_DIR/build-$BUILD_TYPE" ;;
    *) echo "[ERROR] Unknown build type: $BUILD_TYPE" >&2; exit 1 ;;
esac
BUILD_DIR="${EDA_BUILD_DIR:-$DEFAULT_BUILD_DIR}"
if [[ "$BUILD_DIR" != /* ]]; then
    BUILD_DIR="$PROJECT_DIR/$BUILD_DIR"
fi

echo "====================================================="
echo "  正在构建 3D IC 全局布线基线 (构建模式: ${BUILD_TYPE})"
echo "====================================================="

mkdir -p "${BUILD_DIR}"
cd "${PROJECT_DIR}"

cmake -S "$PROJECT_DIR" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build "${BUILD_DIR}" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"

echo "-----------------------------------------------------"
echo "  构建成功！可执行文件路径: ${BUILD_DIR}/bin/eda_router"
echo "====================================================="
