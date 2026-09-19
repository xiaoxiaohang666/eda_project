#!/usr/bin/env bash
set -e

# 3D IC 全局布线框架与基线构建脚本
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
BUILD_TYPE="${1:-Release}"

echo "====================================================="
echo "  正在构建 3D IC 全局布线基线 (构建模式: ${BUILD_TYPE})"
echo "====================================================="

mkdir -p "${BUILD_DIR}"
cd "${PROJECT_DIR}"

cmake -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "-----------------------------------------------------"
echo "  构建成功！可执行文件路径: ${BUILD_DIR}/bin/eda_router"
echo "====================================================="
