#include "parser/thermal_parser.hpp"
#include "parser/json_reader.hpp"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <tuple>
#include <vector>

namespace eda {
namespace {

int locate_cell(const std::vector<double>& lines, double coordinate) {
    if (!std::isfinite(coordinate) || coordinate < lines.front() || coordinate > lines.back()) {
        return -1;
    }
    const auto next = std::upper_bound(lines.begin(), lines.end(), coordinate);
    const int index = static_cast<int>(next - lines.begin()) - 1;
    return std::min(index, static_cast<int>(lines.size()) - 2);
}

// The official helper clamps out-of-grid thermal positions onto edge cells.
int locate_official_cell(const std::vector<double>& lines, double coordinate) {
    if (!std::isfinite(coordinate))
        throw std::runtime_error("thermal DBU coordinate overflows");
    if (coordinate < lines.front()) return 0;
    if (coordinate > lines.back()) return static_cast<int>(lines.size()) - 2;
    const auto next = std::upper_bound(lines.begin(), lines.end(), coordinate);
    return std::clamp(static_cast<int>(next - lines.begin()) - 1,
                      0, static_cast<int>(lines.size()) - 2);
}

// Python round(x, 9) rounds the binary input to a decimal with nine places.
// Fixed decimal conversion preserves that rounding, including decimal ties,
// without the extra binary multiplication used by the historical profile.
double official_round_xy(double value) {
    char text[512];
    const auto result = std::to_chars(text, text + sizeof(text), value,
                                      std::chars_format::fixed, 9);
    if (result.ec != std::errc{})
        throw std::runtime_error("thermal coordinate rounding overflows");
    double rounded = 0;
    const auto parsed = std::from_chars(text, result.ptr, rounded);
    if (parsed.ec != std::errc{} || parsed.ptr != result.ptr || !std::isfinite(rounded))
        throw std::runtime_error("thermal coordinate rounding fails");
    return rounded;
}

using ProjectedSamples = std::map<std::pair<double, double>, std::pair<double, size_t>>;

// Sparse public inputs sample a Cartesian, uniform mesh including die edges.
// This field is used only for empty GCells; directly sampled GCell averages keep
// the historical evaluator-compatible aggregation exactly.
struct RegularField {
    std::vector<double> x, y;
    std::vector<std::vector<double>> values;

    static void check_axis(const std::vector<double>& axis) {
        if (axis.size() < 2)
            throw std::runtime_error("thermal interpolation needs at least two samples per axis");
        const double step = (axis.back() - axis.front()) / static_cast<double>(axis.size() - 1);
        // Public coordinates are rounded to six decimal places in microns.
        const double tolerance = 1e-6 + 1e-9 * std::max(std::abs(axis.front()), std::abs(axis.back()));
        for (size_t i = 0; i < axis.size(); ++i) {
            if (std::abs(axis[i] - (axis.front() + static_cast<double>(i) * step)) > tolerance)
                throw std::runtime_error("sparse thermal samples are not a uniform Cartesian mesh");
        }
    }

    explicit RegularField(const ProjectedSamples& samples) {
        for (const auto& [xy, group] : samples) {
            (void)group;
            x.push_back(xy.first); y.push_back(xy.second);
        }
        auto unique_axis = [](std::vector<double>& axis) {
            std::sort(axis.begin(), axis.end());
            axis.erase(std::unique(axis.begin(), axis.end()), axis.end());
        };
        unique_axis(x); unique_axis(y);
        check_axis(x); check_axis(y);
        if (x.size() > samples.size() / y.size() || x.size() * y.size() != samples.size())
            throw std::runtime_error("sparse thermal samples do not form a complete Cartesian mesh");
        values.assign(y.size(), std::vector<double>(x.size()));
        for (size_t row = 0; row < y.size(); ++row) {
            for (size_t col = 0; col < x.size(); ++col) {
                const auto found = samples.find({x[col], y[row]});
                if (found == samples.end())
                    throw std::runtime_error("sparse thermal mesh has a missing sample");
                values[row][col] = found->second.first / static_cast<double>(found->second.second);
            }
        }
    }

    static std::pair<size_t, double> bracket(const std::vector<double>& axis, double value) {
        // No extrapolation, boundary clamping, or ambient-temperature filling.
        if (!std::isfinite(value) || value < axis.front() || value > axis.back())
            throw std::runtime_error("empty GCell center lies outside the thermal sample mesh");
        const auto next = std::upper_bound(axis.begin(), axis.end(), value);
        const auto i = std::min(static_cast<size_t>(next - axis.begin() - 1), axis.size() - 2);
        return {i, (value - axis[i]) / (axis[i + 1] - axis[i])};
    }

    double interpolate(double px, double py) const {
        const auto [col, tx] = bracket(x, px);
        const auto [row, ty] = bracket(y, py);
        return std::lerp(std::lerp(values[row][col], values[row][col + 1], tx),
                         std::lerp(values[row + 1][col], values[row + 1][col + 1], tx), ty);
    }
};

} // namespace

bool ThermalParser::parse_json(const std::string& json_file_path, const GCellGrid& grid,
                               double dbu_per_micron, const Rect& die_area,
                               ThermalMapping mapping, bool capture_node_counts) {
    ambient_ = 25.0;
    heat_matrix_.clear();
    node_counts_.clear();
    hotspots_.clear();
    interpolated_cells_ = 0;
    filled_cells_ = 0;
    mapped_nodes_ = 0;
    projected_nodes_ = 0;
    mapping_ = mapping;

    try {
        if (mapping != ThermalMapping::LEGACY_BILINEAR &&
            mapping != ThermalMapping::OFFICIAL_PREVIOUS)
            throw std::runtime_error("unknown thermal mapping strategy");
        if (!grid.valid() || !std::isfinite(dbu_per_micron) || dbu_per_micron <= 0.0) {
            throw std::runtime_error("invalid GCell grid or DEF units");
        }

        std::ifstream input(json_file_path);
        if (!input) {
            throw std::runtime_error("cannot open thermal JSON file");
        }
        const std::string source(std::istreambuf_iterator<char>{input},
                                 std::istreambuf_iterator<char>{});
        if (input.bad()) {
            throw std::runtime_error("cannot read thermal JSON file");
        }

        const auto payload = JsonReader(source).read();
        double ambient = 25.0;
        if (mapping == ThermalMapping::LEGACY_BILINEAR) {
            ambient = payload.at("meta").at("ambientTemp").as_number();
        } else if (payload.kind == JsonValue::Kind::OBJECT && payload.object.contains("meta")) {
            const auto& meta = payload.at("meta");
            if (meta.kind != JsonValue::Kind::OBJECT)
                throw std::runtime_error("meta must be a JSON object");
            if (meta.object.contains("ambientTemp")) ambient = meta.at("ambientTemp").as_number();
        }
        const auto& nodes = payload.at("nodes");
        if (nodes.kind != JsonValue::Kind::ARRAY)
            throw std::runtime_error("nodes must be a JSON array");
        // Match the evaluator: average z at each rounded (x,y), then average
        // projected positions in each GCell (not all raw nodes together).
        ProjectedSamples projected;
        std::vector<std::pair<double, double>> projection_order;
        size_t node_count = 0;
        for (const auto& node : nodes.array) {
            if (node.kind != JsonValue::Kind::ARRAY || node.array.size() < 4)
                throw std::runtime_error("thermal node must contain x, y, z, temperature");
            const double x = node.array[0].as_number(), y = node.array[1].as_number();
            node.array[2].as_number();
            const double temperature = node.array[3].as_number();
            const auto xy = mapping == ThermalMapping::OFFICIAL_PREVIOUS
                ? std::make_pair(official_round_xy(x), official_round_xy(y))
                : std::make_pair(std::round(x * 1e9) / 1e9, std::round(y * 1e9) / 1e9);
            if (!std::isfinite(xy.first) || !std::isfinite(xy.second))
                throw std::runtime_error("thermal coordinates overflow");
            auto& group = projected[xy];
            if (mapping == ThermalMapping::OFFICIAL_PREVIOUS && !group.second)
                projection_order.push_back(xy);
            group.first += temperature;
            if (!std::isfinite(group.first)) throw std::runtime_error("thermal temperature sum overflow");
            ++group.second;
            ++node_count;
        }
        if (node_count == 0) {
            throw std::runtime_error("thermal JSON contains no nodes");
        }

        std::vector<std::vector<double>> sums(grid.rows, std::vector<double>(grid.cols, 0.0));
        std::vector<std::vector<size_t>> counts(grid.rows, std::vector<size_t>(grid.cols, 0));
        size_t mapped = 0;
        auto map_sample = [&](const auto& xy, const auto& group) {
            const auto locate = mapping == ThermalMapping::OFFICIAL_PREVIOUS
                ? locate_official_cell : locate_cell;
            const int col = locate(grid.x_lines, die_area.x1 + xy.first * dbu_per_micron);
            const int row = locate(grid.y_lines, die_area.y1 + xy.second * dbu_per_micron);
            if (col >= 0 && row >= 0) {
                sums[row][col] += group.first / static_cast<double>(group.second);
                if (!std::isfinite(sums[row][col])) throw std::runtime_error("GCell temperature sum overflow");
                ++counts[row][col];
                ++mapped;
            }
        };
        if (mapping == ThermalMapping::OFFICIAL_PREVIOUS) {
            // Python dictionaries retain first-occurrence order. Preserve the
            // same summation order for exact dense and sparse numeric parity.
            for (const auto& xy : projection_order) map_sample(xy, projected.at(xy));
        } else {
            for (const auto& [xy, group] : projected) map_sample(xy, group);
        }

        size_t missing_cells = 0;
        for (const auto& row : counts)
            missing_cells += static_cast<size_t>(std::count(row.begin(), row.end(), 0));
        std::vector<std::vector<double>> heat(grid.rows, std::vector<double>(grid.cols, 0.0));
        for (int row = 0; row < grid.rows; ++row)
            for (int col = 0; col < grid.cols; ++col)
                if (counts[row][col]) heat[row][col] = sums[row][col] / static_cast<double>(counts[row][col]);

        if (mapping == ThermalMapping::OFFICIAL_PREVIOUS) {
            std::vector<int> previous_row(grid.rows, 0), previous_col(grid.cols, 0);
            std::vector<bool> sampled_row(grid.rows, false), sampled_col(grid.cols, false);
            for (int row = 0; row < grid.rows; ++row)
                for (int col = 0; col < grid.cols; ++col)
                    if (counts[row][col]) {
                        sampled_row[row] = true;
                        sampled_col[col] = true;
                    }
            int last = 0;
            for (int row = 0; row < grid.rows; ++row) {
                if (sampled_row[row]) last = row;
                previous_row[row] = last;
            }
            last = 0;
            for (int col = 0; col < grid.cols; ++col) {
                if (sampled_col[col]) last = col;
                previous_col[col] = last;
            }
            size_t unresolved = 0;
            int first_row = -1, first_col = -1;
            // Match the official row-major fill, including its fallback index 0
            // and the >0 condition. A missing source is not ambient-filled.
            for (int row = 0; row < grid.rows; ++row) {
                for (int col = 0; col < grid.cols; ++col) {
                    if (counts[row][col]) continue;
                    const double temperature = heat[previous_row[row]][previous_col[col]];
                    if (temperature > 0.0) {
                        heat[row][col] = temperature;
                        counts[row][col] = 1;
                        ++mapped;
                    } else {
                        if (!unresolved) { first_row = row; first_col = col; }
                        ++unresolved;
                    }
                }
            }
            if (unresolved)
                throw std::runtime_error("official thermal coverage missing: " +
                    std::to_string(unresolved) + " GCells; first row=" +
                    std::to_string(first_row) + " col=" + std::to_string(first_col));
        } else if (missing_cells) {
            // Do not impose mesh requirements on already covered dense data.
            const RegularField field(projected);
            for (int row = 0; row < grid.rows; ++row)
                for (int col = 0; col < grid.cols; ++col)
                    if (!counts[row][col])
                        heat[row][col] = field.interpolate(
                            (std::midpoint(grid.x_lines[col], grid.x_lines[col + 1]) - die_area.x1) / dbu_per_micron,
                            (std::midpoint(grid.y_lines[row], grid.y_lines[row + 1]) - die_area.y1) / dbu_per_micron);
        }
        std::vector<HotspotCell> hotspots;
        for (int row = 0; row < grid.rows; ++row) {
            for (int col = 0; col < grid.cols; ++col) {
                const double temperature = heat[row][col];
                if (!std::isfinite(temperature))
                    throw std::runtime_error("thermal mapping overflows");
                if (temperature > ambient)
                    hotspots.push_back({col, row, temperature, temperature - ambient});
            }
        }
        std::sort(hotspots.begin(), hotspots.end(), [](const HotspotCell& a, const HotspotCell& b) {
            if (a.temperature != b.temperature) return a.temperature > b.temperature;
            return std::tie(a.row, a.col) < std::tie(b.row, b.col);
        });
        ambient_ = ambient;
        heat_matrix_ = std::move(heat);
        if (capture_node_counts) node_counts_ = std::move(counts);
        hotspots_ = std::move(hotspots);
        filled_cells_ = missing_cells;
        interpolated_cells_ = mapping == ThermalMapping::LEGACY_BILINEAR ? missing_cells : 0;
        projected_nodes_ = projected.size();
        mapped_nodes_ = mapped;
        std::cout << "[INFO] 热场解析完成: 节点 " << node_count << " 个, GCell "
                  << grid.cols * grid.rows << " 个, 热点 " << hotspots_.size()
                  << " 个, 空 GCell "
                  << (mapping == ThermalMapping::OFFICIAL_PREVIOUS ? "官方前行前列填充 " : "双线性插值 ")
                  << filled_cells_ << " 个\n";
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << json_file_path << ": " << error.what() << "\n";
        return false;
    }
}

} // namespace eda
