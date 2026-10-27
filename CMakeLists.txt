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
target_link_libraries(eda_core PRIVATE ${CMAKE_DL_LIBS})
add_executable(eda_router src/main.cpp)
target_link_libraries(eda_router PRIVATE eda_core)

# 安装/输出目标
set_target_properties(eda_router PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(eda_validate_guide scripts/validate_exported_guide.cpp)
target_link_libraries(eda_validate_guide PRIVATE eda_core)
set_target_properties(eda_validate_guide PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_executable(eda_score_compat scripts/score_compat.cpp)
target_link_libraries(eda_score_compat PRIVATE eda_core)
set_target_properties(eda_score_compat PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(CTest)
if(BUILD_TESTING)
    add_executable(grid_local_tests tests/grid_local_tests.cpp)
    target_link_libraries(grid_local_tests PRIVATE eda_core)
    add_test(NAME grid_local COMMAND grid_local_tests "${CMAKE_CURRENT_BINARY_DIR}/grid-local-artifacts"
        "${CMAKE_CURRENT_SOURCE_DIR}/output/runtime/official-flute")
    add_executable(gcell_encoding_tests tests/gcell_encoding_tests.cpp)
    target_link_libraries(gcell_encoding_tests PRIVATE eda_core)
    add_test(NAME gcell_encoding COMMAND gcell_encoding_tests "${CMAKE_CURRENT_BINARY_DIR}/gcell-encoding-artifacts")
    add_executable(v6_scoring_tests tests/v6_scoring_tests.cpp)
    target_link_libraries(v6_scoring_tests PRIVATE eda_core)
    add_test(NAME v6_scoring COMMAND v6_scoring_tests "${CMAKE_CURRENT_BINARY_DIR}/v6-scoring-artifacts")
    add_executable(compat_net_validation_tests tests/compat_net_validation_tests.cpp)
    target_link_libraries(compat_net_validation_tests PRIVATE eda_core)
    add_test(NAME compat_net_validation COMMAND compat_net_validation_tests "${CMAKE_CURRENT_BINARY_DIR}/compat-net-validation-artifacts")
    add_executable(v6_router_tests tests/v6_router_tests.cpp)
    target_link_libraries(v6_router_tests PRIVATE eda_core)
    add_test(NAME v6_router COMMAND v6_router_tests "${CMAKE_CURRENT_BINARY_DIR}/v6-router-artifacts"
        "${CMAKE_CURRENT_SOURCE_DIR}/output/runtime/official-flute")
endif()
if(BUILD_TESTING)
    add_executable(compat_resources_tests tests/compat_resources_tests.cpp)
    target_link_libraries(compat_resources_tests PRIVATE eda_core)
    add_test(NAME compat_resources COMMAND compat_resources_tests
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/compat-resource-artifacts")
    add_executable(compat_validation_tests tests/compat_validation_tests.cpp)
    target_link_libraries(compat_validation_tests PRIVATE eda_core)
    add_test(NAME compat_validation COMMAND compat_validation_tests
        "${CMAKE_CURRENT_BINARY_DIR}/compat-validation-artifacts")
    add_executable(framework_tests tests/framework_tests.cpp)
    target_link_libraries(framework_tests PRIVATE eda_core)
    add_test(NAME framework COMMAND framework_tests
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/test-artifacts")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME observed_guide_geometry COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audit_guide_geometry.py")
    add_test(NAME score_compat_wrapper COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_score_compat_wrapper.py")
    add_test(NAME score_compat_cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_score_compat.py" "$<TARGET_FILE:eda_score_compat>"
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/score-compat-artifacts"
        "${CMAKE_CURRENT_SOURCE_DIR}/output/runtime/official-flute")
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

    add_test(NAME algorithm_v3 COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_v3.py" "$<TARGET_FILE:eda_router>")

    add_executable(pin_local_tests tests/pin_local_tests.cpp)
    target_link_libraries(pin_local_tests PRIVATE eda_core)
    add_test(NAME pin_local COMMAND pin_local_tests "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/pin-local-artifacts")
    add_executable(parser_compat_tests tests/parser_compat_tests.cpp)
    target_link_libraries(parser_compat_tests PRIVATE eda_core)
    add_test(NAME parser_compat COMMAND parser_compat_tests
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/parser-compat-artifacts")

    add_executable(tsv_preflight_tests tests/tsv_preflight_tests.cpp)
    target_link_libraries(tsv_preflight_tests PRIVATE eda_core)
    add_test(NAME tsv_preflight COMMAND tsv_preflight_tests
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/tsv-preflight-artifacts")

    add_executable(graph_compatibility_tests tests/graph_compatibility_tests.cpp)
    target_link_libraries(graph_compatibility_tests PRIVATE eda_core)
    add_test(NAME graph_compatibility COMMAND graph_compatibility_tests
        "${CMAKE_CURRENT_BINARY_DIR}/graph-compatibility-artifacts")
    add_executable(large_search_tests tests/large_search_tests.cpp)
    target_link_libraries(large_search_tests PRIVATE eda_core)
    add_test(NAME large_search COMMAND large_search_tests
        "${CMAKE_CURRENT_BINARY_DIR}/large-search-artifacts")
    add_executable(terminal_parser_tests tests/terminal_parser_tests.cpp)
    target_link_libraries(terminal_parser_tests PRIVATE eda_core)
    add_test(NAME terminal_parser COMMAND terminal_parser_tests
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/terminal-parser-artifacts")
    add_executable(cached_metrics_tests tests/cached_metrics_tests.cpp)
    target_link_libraries(cached_metrics_tests PRIVATE eda_core)
    add_test(NAME cached_metrics COMMAND cached_metrics_tests
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/cached-metrics-artifacts")
    add_executable(pin_obstruction_tests tests/pin_obstruction_tests.cpp)
    target_link_libraries(pin_obstruction_tests PRIVATE eda_core)
    add_test(NAME pin_obstruction COMMAND pin_obstruction_tests "${CMAKE_CURRENT_BINARY_DIR}/pin-obstruction-artifacts")
    add_executable(fast_search_tests tests/fast_search_tests.cpp)
    target_link_libraries(fast_search_tests PRIVATE eda_core)
    add_test(NAME fast_search COMMAND fast_search_tests "${CMAKE_CURRENT_BINARY_DIR}/fast-search-artifacts")
    add_test(NAME export_validator COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_export_validator.py" "$<TARGET_FILE:eda_validate_guide>"
        "${CMAKE_CURRENT_BINARY_DIR}/export-validator-artifacts")
    add_executable(terminal_access_tests tests/terminal_access_tests.cpp)
    target_link_libraries(terminal_access_tests PRIVATE eda_core)
    add_test(NAME terminal_access COMMAND terminal_access_tests "${CMAKE_CURRENT_BINARY_DIR}/terminal-access-artifacts")
    add_test(NAME official_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_official_cli.py"
        "$<TARGET_FILE:eda_router>" "${CMAKE_CURRENT_SOURCE_DIR}/output/runtime/official-flute")
    add_test(NAME official_runtime_packaging COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prepare_official_runtime.py")
    add_test(NAME official_wrapper COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_official_wrapper.py")
    add_test(NAME submission_entrypoint COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_submission.py")
    add_executable(global_graph_tests tests/global_graph_tests.cpp)
    target_link_libraries(global_graph_tests PRIVATE eda_core)
    add_test(NAME global_graph COMMAND global_graph_tests "${CMAKE_CURRENT_BINARY_DIR}/global-graph-artifacts")
    add_executable(official_thermal_tests tests/official_thermal_tests.cpp)
    target_link_libraries(official_thermal_tests PRIVATE eda_core)
    add_test(NAME official_thermal COMMAND official_thermal_tests "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/official-thermal-artifacts")
    add_executable(official_ledger_tests tests/official_ledger_tests.cpp)
    target_link_libraries(official_ledger_tests PRIVATE eda_core)
    add_test(NAME official_ledger COMMAND official_ledger_tests
        "${CMAKE_CURRENT_BINARY_DIR}/official-ledger-artifacts" "${CMAKE_CURRENT_SOURCE_DIR}/output/runtime/official-flute")
    add_executable(official_metrics_tests tests/official_metrics_tests.cpp)
    target_link_libraries(official_metrics_tests PRIVATE eda_core)
    add_test(NAME official_metrics COMMAND official_metrics_tests "${CMAKE_CURRENT_BINARY_DIR}/official-metrics-artifacts")

    add_executable(fast_quality_tests tests/fast_quality_tests.cpp)
    target_link_libraries(fast_quality_tests PRIVATE eda_core)
    add_test(NAME fast_quality COMMAND fast_quality_tests
        "${CMAKE_CURRENT_BINARY_DIR}/fast-quality-artifacts"
        "${CMAKE_CURRENT_SOURCE_DIR}/output/runtime/official-flute")

    add_executable(tsv_polish_tests tests/tsv_polish_tests.cpp)
    target_link_libraries(tsv_polish_tests PRIVATE eda_core)
    add_test(NAME tsv_polish COMMAND tsv_polish_tests "${CMAKE_CURRENT_BINARY_DIR}/tsv-polish-artifacts")

    add_test(NAME official_benchmark COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_official.py")

    add_executable(eda_analyze_congestion scripts/analyze_official_congestion.cpp)
    target_link_libraries(eda_analyze_congestion PRIVATE eda_core)
    set_target_properties(eda_analyze_congestion PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    add_executable(rrr_score_tests tests/rrr_score_tests.cpp)
    target_link_libraries(rrr_score_tests PRIVATE eda_core)
    add_test(NAME rrr_score COMMAND rrr_score_tests
        "${CMAKE_CURRENT_BINARY_DIR}/rrr-score-artifacts"
        "${CMAKE_CURRENT_SOURCE_DIR}/output/runtime/official-flute")

    add_executable(tsv_thermal_candidate_tests tests/tsv_thermal_candidate_tests.cpp)
    target_link_libraries(tsv_thermal_candidate_tests PRIVATE eda_core)
    add_test(NAME tsv_thermal_candidates COMMAND tsv_thermal_candidate_tests
        "${CMAKE_CURRENT_BINARY_DIR}/tsv-thermal-candidate-artifacts")

    add_executable(score_pricing_tests tests/score_pricing_tests.cpp)
    target_link_libraries(score_pricing_tests PRIVATE eda_core)
    add_test(NAME score_pricing COMMAND score_pricing_tests
        "${CMAKE_CURRENT_BINARY_DIR}/score-pricing-artifacts"
        "${CMAKE_CURRENT_SOURCE_DIR}/output/runtime/official-flute")

    add_executable(net_validator_tests tests/net_validator_tests.cpp)
    target_link_libraries(net_validator_tests PRIVATE eda_core)
    add_test(NAME net_validator COMMAND net_validator_tests "${CMAKE_CURRENT_BINARY_DIR}/net-validator-artifacts")

    add_executable(budgeted_recovery_tests tests/budgeted_recovery_tests.cpp)
    target_link_libraries(budgeted_recovery_tests PRIVATE eda_core)
    add_test(NAME budgeted_recovery COMMAND budgeted_recovery_tests
        "${CMAKE_CURRENT_BINARY_DIR}/budgeted-recovery-artifacts"
        "${CMAKE_CURRENT_SOURCE_DIR}/output/runtime/official-flute")

    add_executable(fast_layer_pruning_tests tests/fast_layer_pruning_tests.cpp)
    target_link_libraries(fast_layer_pruning_tests PRIVATE eda_core)
    add_test(NAME fast_layer_pruning COMMAND fast_layer_pruning_tests
        "${CMAKE_CURRENT_BINARY_DIR}/fast-layer-pruning-artifacts"
        "${CMAKE_CURRENT_SOURCE_DIR}/output/runtime/official-flute")

    add_test(NAME v5_cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_v5_cli.py" "$<TARGET_FILE:eda_router>")
    add_test(NAME v6_cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_v6_cli.py" "$<TARGET_FILE:eda_router>"
        "$<TARGET_FILE:eda_score_compat>" "$<TARGET_FILE:eda_validate_guide>"
        "${CMAKE_CURRENT_SOURCE_DIR}/output/runtime/official-flute")
    add_executable(tsv_assignment_v5_tests tests/tsv_assignment_v5_tests.cpp)
    target_link_libraries(tsv_assignment_v5_tests PRIVATE eda_core)
    add_test(NAME tsv_assignment_v5 COMMAND tsv_assignment_v5_tests
        "${CMAKE_CURRENT_BINARY_DIR}/tsv-assignment-v5-artifacts")
    add_executable(v5_topology_tests tests/v5_topology_tests.cpp)
    target_link_libraries(v5_topology_tests PRIVATE eda_core)
    add_test(NAME v5_topology COMMAND v5_topology_tests
        "${CMAKE_CURRENT_BINARY_DIR}/v5-topology-artifacts"
        "${CMAKE_CURRENT_SOURCE_DIR}/output/runtime/official-flute")
    add_executable(global_tsv_tests tests/global_tsv_tests.cpp)
    target_link_libraries(global_tsv_tests PRIVATE eda_core)
    add_test(NAME global_tsv COMMAND global_tsv_tests
        "${CMAKE_CURRENT_BINARY_DIR}/global-tsv-artifacts")
endif()

# V7 refinement regressions use the same immutable local FLUTE runtime.
if(BUILD_TESTING)
    add_test(NAME v7_cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_v7_cli.py" "$<TARGET_FILE:eda_router>"
        "$<TARGET_FILE:eda_score_compat>" "$<TARGET_FILE:eda_validate_guide>"
        "${CMAKE_CURRENT_SOURCE_DIR}/output/runtime/official-flute")
endif()

if(BUILD_TESTING)
    add_executable(v7_tsv_tests tests/v7_tsv_tests.cpp)
    target_link_libraries(v7_tsv_tests PRIVATE eda_core)
    add_test(NAME v7_tsv COMMAND v7_tsv_tests "${CMAKE_CURRENT_BINARY_DIR}/v7-tsv-artifacts")
endif()

if(BUILD_TESTING)
    add_executable(v7_router_tests tests/v7_router_tests.cpp)
    target_link_libraries(v7_router_tests PRIVATE eda_core)
    add_test(NAME v7_router COMMAND v7_router_tests
        "${CMAKE_CURRENT_BINARY_DIR}/v7-router-artifacts"
        "${CMAKE_CURRENT_SOURCE_DIR}/output/runtime/official-flute")
endif()
