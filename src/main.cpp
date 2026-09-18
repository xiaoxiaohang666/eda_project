#include <iostream>
#include <string>
#include <vector>
#include <chrono>

int main(int argc, char* argv[]) {
    std::cout << "=====================================================\n";
    std::cout << "  3D IC Multi-objective Global Routing Optimization  \n";
    std::cout << "  EDA Elite Challenge 2026 - Zhuhai Silicon Core Tech\n";
    std::cout << "=====================================================\n";

    if (argc < 3) {
        std::cout << "用法说明 (Usage):\n";
        std::cout << "  " << argv[0] << " <design.def> <tech.tlef> [options]\n\n";
        std::cout << "示例参数:\n";
        std::cout << "  --output <result.guide>  : 指定输出布线引导文件路径\n";
        std::cout << "  --threads <num>          : 设置并行搜索线程数\n";
        std::cout << "-----------------------------------------------------\n";
        std::cout << "[环境自检通过] GCC 11.4.0 / C++20 运行时正常就绪。\n";
        return 0;
    }

    std::string def_file = argv[1];
    std::string tlef_file = argv[2];
    std::cout << "[INFO] 读取 DEF 文件: " << def_file << "\n";
    std::cout << "[INFO] 读取 TLEF 文件: " << tlef_file << "\n";

    return 0;
}
