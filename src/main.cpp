#include "common/types.hpp"
#include "parser/lef_parser.hpp"
#include "parser/def_parser.hpp"
#include "parser/thermal_parser.hpp"
#include "router/baseline_router.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <chrono>

void print_banner() {
    std::cout << "===============================================================\n";
    std::cout << "                 3D IC Global Routing Baseline                 \n";
    std::cout << "  2026 EDA Elite Challenge - Zhuhai Silicon Core Technology    \n";
    std::cout << "===============================================================\n";
}

void print_usage(const char* prog) {
    std::cout << "用法说明 (Usage):\n";
    std::cout << "  " << prog << " --input-def <path> --lefs <path> --thermal-json <path> -o <result.guide>\n\n";
    std::cout << "选项参数:\n";
    std::cout << "  --input-def <file>     : 3D 布局 DEF 输入文件路径\n";
    std::cout << "  --lefs <file>          : 金属层及器件工艺 LEF 文件路径 (可多次指定)\n";
    std::cout << "  --thermal-json <file>  : 稳态温度场分布 JSON 文件路径\n";
    std::cout << "  -o, --output <file>    : 导出的 ISPD 18/19 标准 Route Guide 路径\n";
    std::cout << "===============================================================\n";
}

int main(int argc, char* argv[]) {
    print_banner();

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string def_file;
    std::vector<std::string> lef_files;
    std::string thermal_file;
    std::string output_file = "output/result.guide";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg != "--input-def" && arg != "--lefs" &&
            arg != "--thermal-json" && arg != "-o" && arg != "--output") {
            std::cerr << "[ERROR] 未知参数: " << arg << "\n";
            return 1;
        }
        if (i + 1 >= argc) {
            std::cerr << "[ERROR] 参数缺少文件路径: " << arg << "\n";
            return 1;
        }
        const std::string value = argv[++i];
        if (arg == "--input-def") def_file = value;
        else if (arg == "--lefs") lef_files.push_back(value);
        else if (arg == "--thermal-json") thermal_file = value;
        else output_file = value;
    }

    if (def_file.empty() || lef_files.empty() || thermal_file.empty()) {
        std::cerr << "[ERROR] 缺少必要输入 (--input-def、--lefs、--thermal-json)\n";
        print_usage(argv[0]);
        return 1;
    }

    auto start_time = std::chrono::steady_clock::now();

    // 1. 解析 LEF 工艺库
    std::cout << "[PHASE 1] 正在解析工艺库 (LEF) ...\n";
    eda::LefParser lef_parser;
    for (const auto& lef_path : lef_files) {
        std::cout << "  - 读取: " << lef_path << "\n";
        if (!lef_parser.parse(lef_path)) {
            std::cerr << "[ERROR] 解析 LEF 文件失败: " << lef_path << "\n";
            return 1;
        }
    }

    // 2. 解析 DEF 布局与网络
    std::cout << "[PHASE 2] 正在解析布局与网络 (DEF): " << def_file << " ...\n";
    eda::DefParser def_parser;
    if (!def_parser.parse(def_file, lef_parser)) {
        std::cerr << "[ERROR] 解析 DEF 文件失败: " << def_file << "\n";
        return 1;
    }

    // 3. 解析芯片稳态热力场
    std::cout << "[PHASE 3] 正在加载稳态热力场 (Thermal) ...\n";
    eda::ThermalParser thermal_parser;
    if (!thermal_file.empty()) {
        std::cout << "  - 读取: " << thermal_file << "\n";
        if (!thermal_parser.parse_json(thermal_file, def_parser.gcell_grid(),
                                       def_parser.dbu_per_micron(), def_parser.die_area())) {
            std::cerr << "[ERROR] 热场 JSON 解析失败: " << thermal_file << "\n";
            return 1;
        }
    }

    // 4. 执行当前基线布线；容量感知寻路与重布仍待实现。
    std::cout << "[PHASE 4] 正在执行 3D IC 基线布线 ...\n";
    eda::BaselineRouter router(def_parser, lef_parser, thermal_parser);
    if (!router.route_all()) {
        std::cerr << "[ERROR] 全局布线算法执行异常!\n";
        return 1;
    }

    // 5. 导出合规的 Route Guide 结果
    std::cout << "[PHASE 5] 正在导出 ISPD Route Guide: " << output_file << " ...\n";
    if (!router.export_guide(output_file)) {
        std::cerr << "[ERROR] 导出 Route Guide 失败!\n";
        return 1;
    }

    auto end_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::cout << "===============================================================\n";
    std::cout << "[SUCCESS] 基线 Guide 已导出! 总耗时: " << elapsed << " ms\n";
    std::cout << "===============================================================\n";

    return 0;
}
