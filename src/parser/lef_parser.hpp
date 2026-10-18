#pragma once

#include "common/types.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace eda {

// All LEF geometry is in microns. Convert only at the DEF placement boundary.
struct LayerRect {
    std::string layer;
    Rect rect;
};

struct LayerInfo {
    Direction direction{Direction::UNKNOWN};
    double width{0.0};
    std::string type;
};

struct ViaInfo {
    std::string name;
    std::vector<LayerRect> shapes;
    bool is_default{false};
    // Restricted to the top of a stacked via. Generic routing must exclude it
    // until a stack-aware legality model can enforce this qualifier.
    bool top_of_stack_only{false};
};

struct MacroPinInfo {
    std::string name;
    std::string layer;
    Rect bbox;
    bool has_rect{false};
    // Keep disconnected and multilayer PORT rectangles individually; bbox is
    // still the first rectangle for compatibility with existing checker code.
    std::vector<LayerRect> port_rects;
};

struct MacroInfo {
    std::string name;
    double width{0.0};
    double height{0.0};
    Point origin;
    std::unordered_map<std::string, MacroPinInfo> pins;
    std::vector<LayerRect> obstructions;
    std::string macro_class; // LEF main CLASS, e.g. CORE or BLOCK; empty if absent.
    std::string macro_subclass;
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
        return Direction::UNKNOWN;
    }

    const MacroInfo* get_macro(const std::string& macro_name) const {
        auto it = macros_.find(macro_name);
        if (it != macros_.end()) return &it->second;
        return nullptr;
    }

    const LayerInfo* get_layer(const std::string& name) const {
        const auto found = layers_.find(name);
        return found == layers_.end() ? nullptr : &found->second;
    }
    const ViaInfo* get_via(const std::string& name) const {
        const auto found = vias_.find(name);
        return found == vias_.end() ? nullptr : &found->second;
    }
    const std::unordered_map<std::string, ViaInfo>& vias() const { return vias_; }
    const std::unordered_map<std::string, MacroInfo>& macros() const { return macros_; }

private:
    std::unordered_map<std::string, Direction> layer_directions_;
    std::unordered_map<std::string, MacroInfo> macros_;
    std::unordered_map<std::string, LayerInfo> layers_;
    std::unordered_map<std::string, ViaInfo> vias_;
};

} // namespace eda
