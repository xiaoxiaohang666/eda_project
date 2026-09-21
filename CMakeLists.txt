cmake_minimum_required(VERSION 3.22)
project(eda_3d_router
    VERSION 0.1.0
    DESCRIPTION "3D IC global-routing framework with a runnable baseline"
    LANGUAGES CXX
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
    add_compile_options(-Wall -Wextra -pedantic)
    # CMake owns optimization flags for Release/RelWithDebInfo/MinSizeRel.
    add_compile_options("$<$<CONFIG:Debug>:-fsanitize=address,undefined>")
    add_link_options("$<$<CONFIG:Debug>:-fsanitize=address,undefined>")
endif()

# 包含目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# 收集源码文件
# 新增算法源文件时自动触发 CMake 重新配置。
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

# 构建主可执行文件
list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
add_library(eda_core STATIC ${SOURCES})
add_executable(eda_router src/main.cpp)
target_link_libraries(eda_router PRIVATE eda_core)

# 安装/输出目标
set_target_properties(eda_router PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(framework_tests tests/framework_tests.cpp)
    target_link_libraries(framework_tests PRIVATE eda_core)
    add_test(NAME framework COMMAND framework_tests
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/test-artifacts")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py" "$<TARGET_FILE:eda_router>")
    add_test(NAME evaluation_wrapper COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_evaluate.py")
endif()
