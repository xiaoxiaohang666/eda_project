#pragma once

#include "common/types.hpp"
#include "parser/lef_parser.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace eda {

struct Component {
    std::string name;
    std::string macro_type;
    Point origin;
    std::string orient;
};

class DefParser {
public:
    DefParser() = default;

    bool parse(const std::string& def_file_path, const LefParser& lef_parser);

    const Rect& die_area() const { return die_area_; }
    double dbu_per_micron() const { return dbu_per_micron_; }
    const GCellGrid& gcell_grid() const { return grid_; }
    const std::vector<Net>& nets() const { return nets_; }
    const std::unordered_map<std::string, Pin>& io_pins() const { return io_pins_; }
    const std::unordered_map<std::string, Component>& components() const { return components_; }

private:
    Rect die_area_;
    double dbu_per_micron_{2000.0};
    GCellGrid grid_;
    std::vector<Net> nets_;
    std::unordered_map<std::string, Pin> io_pins_;
    std::unordered_map<std::string, Component> components_;

    void build_grid(const std::vector<double>& x_steps, const std::vector<double>& y_steps);
};

} // namespace eda
