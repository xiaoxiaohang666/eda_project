cmake_minimum_required(VERSION 3.22)
project(eda_3d_router
    VERSION 1.0.0
    DESCRIPTION "3D IC capacity-aware global router"
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

    add_executable(resource_tests tests/resource_tests.cpp)
    target_link_libraries(resource_tests PRIVATE eda_core)
    add_test(NAME resources COMMAND resource_tests "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/resource-artifacts")
    add_test(NAME algorithm COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_algorithm.py" "$<TARGET_FILE:eda_router>")

    add_executable(metrics_policy_tests tests/metrics_policy_tests.cpp)
    target_link_libraries(metrics_policy_tests PRIVATE eda_core)
    add_test(NAME metrics_policy COMMAND metrics_policy_tests "${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(search_tests tests/search_tests.cpp)
    target_link_libraries(search_tests PRIVATE eda_core)
    add_test(NAME search COMMAND search_tests "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME algorithm_v2 COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_v2.py" "$<TARGET_FILE:eda_router>")

    add_executable(pin_local_tests tests/pin_local_tests.cpp)
    target_link_libraries(pin_local_tests PRIVATE eda_core)
    add_test(NAME pin_local COMMAND pin_local_tests "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/pin-local-artifacts")
endif()
