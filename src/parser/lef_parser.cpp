#include "parser/lef_parser.hpp"
#include <fstream>
#include <sstream>
#include <iostream>

namespace eda {

bool LefParser::parse(const std::string& lef_file_path) {
    std::ifstream fin(lef_file_path);
    if (!fin.is_open()) {
        std::cerr << "[ERROR] 无法打开 LEF 文件: " << lef_file_path << "\n";
        return false;
    }

    std::string line;
    std::string current_layer;
    std::string current_macro;
    std::string current_pin;
    std::string current_pin_layer;

    while (std::getline(fin, line)) {
        std::istringstream iss(line);
        std::string token;
        if (!(iss >> token)) continue;
        if (token.rfind('#', 0) == 0) continue;

        if (token == "LAYER") {
            iss >> current_layer;
            if (!current_macro.empty() && !current_pin.empty()) {
                current_pin_layer = current_layer;
            }
        } else if (token == "DIRECTION") {
            std::string dir_str;
            iss >> dir_str;
            if (dir_str.find("HORIZONTAL") != std::string::npos) {
                layer_directions_[current_layer] = Direction::HORIZONTAL;
            } else if (dir_str.find("VERTICAL") != std::string::npos) {
                layer_directions_[current_layer] = Direction::VERTICAL;
            }
        } else if (token == "MACRO") {
            iss >> current_macro;
            macros_[current_macro].name = current_macro;
        } else if (token == "PIN" && !current_macro.empty()) {
            iss >> current_pin;
            macros_[current_macro].pins[current_pin].name = current_pin;
            current_pin_layer.clear();
        } else if (token == "RECT" && !current_macro.empty() && !current_pin.empty()) {
            double x1, y1, x2, y2;
            if (iss >> x1 >> y1 >> x2 >> y2) {
                macros_[current_macro].pins[current_pin].layer = current_pin_layer;
                macros_[current_macro].pins[current_pin].bbox = Rect(x1, y1, x2, y2);
            }
        } else if (token == "END") {
            std::string end_name;
            iss >> end_name;
            if (end_name == current_pin) {
                current_pin.clear();
                current_pin_layer.clear();
            } else if (end_name == current_macro) {
                current_macro.clear();
            } else if (end_name == current_layer) {
                current_layer.clear();
            }
        }
    }

    std::cout << "[INFO] LEF 解析完成: 记录宏单元 " << macros_.size() << " 个\n";
    return true;
}

} // namespace eda
