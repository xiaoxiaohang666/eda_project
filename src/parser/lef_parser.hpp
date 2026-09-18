#pragma once

#include "common/types.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace eda {

struct MacroPinInfo {
    std::string name;
    std::string layer;
    Rect bbox;
};

struct MacroInfo {
    std::string name;
    double width{0.0};
    double height{0.0};
    std::unordered_map<std::string, MacroPinInfo> pins;
};

class LefParser {
public:
    LefParser() = default;

    bool parse(const std::string& lef_file_path);

    Direction get_layer_direction(const std::string& layer_name) const {
        auto it = layer_directions_.find(layer_name);
        if (it != layer_directions_.end()) {
            return it->second;
        }
        int idx = get_layer_index(layer_name);
        return (idx % 2 == 1) ? Direction::HORIZONTAL : Direction::VERTICAL;
    }

    const MacroInfo* get_macro(const std::string& macro_name) const {
        auto it = macros_.find(macro_name);
        if (it != macros_.end()) return &it->second;
        return nullptr;
    }

private:
    std::unordered_map<std::string, Direction> layer_directions_;
    std::unordered_map<std::string, MacroInfo> macros_;
};

} // namespace eda
