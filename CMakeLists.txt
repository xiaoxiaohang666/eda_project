cmake_minimum_required(VERSION 3.22)
project(eda_3d_router
    VERSION 1.0.0
    DESCRIPTION "Multi-objective Global Routing for 3D IC"
    LANGUAGES C CXX
)

# 强制使用 C++20 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 导出 compile_commands.json 供 clangd / VS Code 智能提示使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 默认采用 Release 模式以获得最高布线性能
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 编译选项与优化标志
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -pedantic
        $<$<CONFIG:Release>:-O3 -march=native -DNDEBUG>
        $<$<CONFIG:Debug>:-O0 -g -fsanitize=address,undefined>
    )
    # 若在 Debug 模式启用 AddressSanitizer，链接时也需加上
    add_link_options(
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    )
endif()

# 查找常用线程库
find_package(Threads REQUIRED)

# 包含目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# 收集源码文件
file(GLOB_RECURSE SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

# 构建主可执行文件
add_executable(eda_router ${SOURCES})
target_link_libraries(eda_router PRIVATE Threads::Threads)

# 安装/输出目标
set_target_properties(eda_router PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
