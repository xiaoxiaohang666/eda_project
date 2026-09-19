#!/usr/bin/env bash
# 检查本地构建与赛题评估器所需的工具和运行库。
set -u

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail_count=0

check() {
    local description="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        printf '[PASS] %s\n' "$description"
    else
        printf '[FAIL] %s\n' "$description"
        fail_count=$((fail_count + 1))
    fi
}

check_file() {
    test -f "$PROJECT_DIR/$1"
}

check_cmake_version() {
    local version major minor
    command -v cmake >/dev/null 2>&1 || return 1
    version="$(cmake --version | head -n 1)"
    [[ "$version" =~ ([0-9]+)\.([0-9]+) ]] || return 1
    major="${BASH_REMATCH[1]}"
    minor="${BASH_REMATCH[2]}"
    (( major > 3 || (major == 3 && minor >= 22) ))
}

check_cxx20() {
    command -v c++ >/dev/null 2>&1 || return 1
    printf '#include <concepts>\nstatic_assert(std::same_as<int, int>);\n' |
        c++ -std=c++20 -x c++ -fsyntax-only -
}

check_build_tool() {
    command -v make >/dev/null 2>&1 || command -v ninja >/dev/null 2>&1
}

check_evaluator_imports() {
    command -v python3 >/dev/null 2>&1 || return 1
    LD_LIBRARY_PATH="$PROJECT_DIR/evaluator${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    PYTHONPATH="$PROJECT_DIR/evaluator${PYTHONPATH:+:${PYTHONPATH}}" \
        python3 -c 'import tse_framework_pybind_def, tse_framework_pybind_lef'
}

printf '本地构建与评估环境检查: %s\n' "$PROJECT_DIR"
check 'CMake >= 3.22' check_cmake_version
check 'C++20 编译器' check_cxx20
check 'Make 或 Ninja' check_build_tool
check 'nproc（构建脚本需要）' command -v nproc
check 'Python 及评估器二进制模块可导入（需兼容 Python 3.10）' check_evaluator_imports
check '赛题评估器脚本' check_file evaluator/score.py
check '示例 DEF' check_file benchmarks/sample001/sample001.input.def
check '示例 LEF' check_file benchmarks/sample001/sample001.input.lef
check '示例热场' check_file benchmarks/sample001/sample001.thermal.input.json
check '示例 Guide' check_file benchmarks/sample001/sample001.output.guide

printf '检查完成：%d 项失败。\n' "$fail_count"
if (( fail_count > 0 )); then
    exit 1
fi
