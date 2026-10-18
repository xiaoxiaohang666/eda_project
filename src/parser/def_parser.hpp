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

struct TrackInfo {
    char axis{'X'};
    double start{0.0};
    int count{0};
    double step{0.0};
    std::vector<std::string> layers;
};

// Local LEF microns -> oriented, placed global DEF DBU.
Point transform_macro_point(Point point, const MacroInfo& macro,
                            const Component& component, double dbu_per_micron);
Rect transform_macro_rect(const Rect& rect, const MacroInfo& macro,
                          const Component& component, double dbu_per_micron);
// The oriented SIZE box is distinct from geometry shifted by LEF ORIGIN.
Rect component_bbox(const MacroInfo& macro, const Component& component, double dbu_per_micron);

enum class DefGridMapping { CLIPPED_PHYSICAL, OFFICIAL_FULL };

class DefParser {
public:
    DefParser() = default;

    bool parse(const std::string& def_file_path, const LefParser& lef_parser, DefGridMapping mapping=DefGridMapping::CLIPPED_PHYSICAL);

    const Rect& die_area() const { return die_area_; }
    double dbu_per_micron() const { return dbu_per_micron_; }
    const GCellGrid& gcell_grid() const { return grid_; }
    double gcell_pitch_x() const { return pitch_x_; }
    double gcell_pitch_y() const { return pitch_y_; }
    const std::vector<Net>& nets() const { return nets_; }
    const std::unordered_map<std::string, Pin>& io_pins() const { return io_pins_; }
    const std::unordered_map<std::string, Component>& components() const { return components_; }
    const std::vector<TrackInfo>& tracks() const { return tracks_; }

private:
    Rect die_area_;
    double dbu_per_micron_{0.0};
    GCellGrid grid_;
    double pitch_x_{0}, pitch_y_{0};
    std::vector<Net> nets_;
    std::unordered_map<std::string, Pin> io_pins_;
    std::unordered_map<std::string, Component> components_;
    std::vector<TrackInfo> tracks_;

};

} // namespace eda
