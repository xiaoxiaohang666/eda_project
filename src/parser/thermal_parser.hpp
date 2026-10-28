#pragma once

#include "common/types.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace eda {

// Keep historical interpolation for existing callers; the new official scorer
// instead extends sparse samples from preceding sampled row/column indices.
enum class ThermalMapping { LEGACY_BILINEAR, OFFICIAL_PREVIOUS };

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
                    double dbu_per_micron, const Rect& die_area,
                    ThermalMapping mapping = ThermalMapping::LEGACY_BILINEAR,
                    bool capture_node_counts = false);

    double ambient_temperature() const { return ambient_; }
    double get_temperature(int col, int row) const {
        if (row >= 0 && row < static_cast<int>(heat_matrix_.size()) &&
            col >= 0 && col < static_cast<int>(heat_matrix_[row].size())) {
            return heat_matrix_[row][col];
        }
        return ambient_;
    }

    const std::vector<HotspotCell>& hotspots() const { return hotspots_; }
    size_t interpolated_cells() const { return interpolated_cells_; }
    size_t filled_cells() const { return filled_cells_; }
    // Official mapped count includes one synthetic node for each successful
    // preceding-cell fill, as does the official report's mapped_thermal_xy_nodes.
    size_t mapped_nodes() const { return mapped_nodes_; }
    size_t projected_nodes() const { return projected_nodes_; }
    // Optional scorer artifact; routing callers do not retain this matrix.
    const std::vector<std::vector<size_t>>& node_counts() const { return node_counts_; }
    ThermalMapping mapping_strategy() const { return mapping_; }
    const char* mapping_mode() const {
        if (mapping_ == ThermalMapping::OFFICIAL_PREVIOUS && filled_cells_)
            return "direct-cell-average+official-previous-missing";
        return interpolated_cells_ ? "direct-cell-average+bilinear-missing" : "direct-cell-average";
    }

private:
    double ambient_{25.0};
    std::vector<std::vector<double>> heat_matrix_;
    std::vector<std::vector<size_t>> node_counts_;
    std::vector<HotspotCell> hotspots_;
    size_t interpolated_cells_{0};
    size_t filled_cells_{0};
    size_t mapped_nodes_{0};
    size_t projected_nodes_{0};
    ThermalMapping mapping_{ThermalMapping::LEGACY_BILINEAR};
};

} // namespace eda
