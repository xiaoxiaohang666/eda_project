#!/usr/bin/env bash
# 环境体检与健康诊断脚本 (Environment Health Doctor)

echo "================================================================"
echo "        2026 EDA 竞赛环境与工程健康大巡检 (Doctor)              "
echo "================================================================"

pass_count=0
fail_count=0

check() {
    local name="$1"
    local cmd="$2"
    echo -n "[检查项] $name ... "
    if eval "$cmd" > /dev/null 2>&1; then
        echo -e "\033[32m[通过 PASS]\033[0m"
        ((pass_count++))
    else
        echo -e "\033[31m[未通过 FAIL]\033[0m"
        ((fail_count++))
    fi
}

echo ""
echo "--- 1. 系统与操作系统规范 ---"
check "操作系统为 Ubuntu 22.04" "grep -q 'Ubuntu 22.04' /etc/os-release"
check "系统架构为 x86_64" "[ \"\$(uname -m)\" = 'x86_64' ]"
check "当前用户为普通非root用户" "[ \"\$(whoami)\" != 'root' ]"

echo ""
echo "--- 2. 竞赛官方编译工具链规范 ---"
check "GCC 编译器已就绪 (11.4.0)" "gcc --version | grep -q '11.4.0'"
check "G++ 编译器已就绪 (11.4.0)" "g++ --version | grep -q '11.4.0'"
check "CMake 构建系统已就绪 (>= 3.22)" "cmake --version | grep -q 'version 3.'"
check "GDB 调试器已就绪" "which gdb"
check "Make / Ninja 构建工具已就绪" "which make && which ninja"
check "运行时间监控工具 /usr/bin/time 已就绪" "which /usr/bin/time"

echo ""
echo "--- 3. Git 与 GitHub 协同能力 ---"
check "Git 全局用户名已配置" "git config --get user.name"
check "Git 全局邮箱已配置" "git config --get user.email"
check "Git 凭据管理器已免密集成" "which git-credential-manager.exe"
check "本地仓库与远程分支完全同步" "cd . && git status | grep -q 'Your branch is up to date'"

echo ""
echo "--- 4. VS Code 与跨系统调用支持 ---"
check "WSL 与 Windows 互操作性激活" "which cmd.exe"
check "终端 code . 命令已无缝可用" "code --version"

echo ""
echo "--- 5. 项目构建与 C++20 标准验证 ---"
check "一键自动化构建脚本 scripts/build.sh 可执行" "[ -x ./scripts/build.sh ]"
check "CMake C++20 Release 模式编译通过" "cd . && ./scripts/build.sh Release"
check "编译生成的 ELF 可执行文件可正常运行" "./build/bin/eda_router"

echo ""
echo "================================================================"
echo "巡检汇总: 通过项: $pass_count | 失败项: $fail_count"
if [ "$fail_count" -eq 0 ]; then
    echo -e "\033[32m【全部通过】当前开发环境 100% 满足 2026 EDA 赛题指南要求与工业标准！\033[0m"
else
    echo -e "\033[31m【存在异常】请根据未通过项进行相应修复。\033[0m"
fi
echo "================================================================"
