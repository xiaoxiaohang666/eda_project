#pragma once

#include "common/types.hpp"
#include <string>
#include <vector>

namespace eda {

struct HotspotCell {
    int col{0};
    int row{0};
    double temperature{0.0};
    double delta_t{0.0};
};

class ThermalParser {
public:
    ThermalParser() = default;

    // Supports the provided contest schema: meta.ambientTemp and nodes arrays
    // containing [x_um, y_um, z_um, temperature].
    bool parse_json(const std::string& json_file_path, const GCellGrid& grid,
                    double dbu_per_micron, const Rect& die_area);

    double ambient_temperature() const { return ambient_; }
    double get_temperature(int col, int row) const {
        if (row >= 0 && row < static_cast<int>(heat_matrix_.size()) &&
            col >= 0 && col < static_cast<int>(heat_matrix_[row].size())) {
            return heat_matrix_[row][col];
        }
        return ambient_;
    }

    const std::vector<HotspotCell>& hotspots() const { return hotspots_; }

private:
    double ambient_{25.0};
    std::vector<std::vector<double>> heat_matrix_;
    std::vector<HotspotCell> hotspots_;
};

} // namespace eda
