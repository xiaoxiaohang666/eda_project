#include "parser/thermal_parser.hpp"
#include "parser/json_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
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

} // namespace

bool ThermalParser::parse_json(const std::string& json_file_path, const GCellGrid& grid,
                               double dbu_per_micron, const Rect& die_area) {
    ambient_ = 25.0;
    heat_matrix_.clear();
    hotspots_.clear();

    try {
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
        const double ambient = payload.at("meta").at("ambientTemp").as_number();
        const auto& nodes = payload.at("nodes");
        if (nodes.kind != JsonValue::Kind::ARRAY)
            throw std::runtime_error("nodes must be a JSON array");
        // Match the evaluator: average z at each rounded (x,y), then average
        // projected positions in each GCell (not all raw nodes together).
        std::map<std::pair<double, double>, std::pair<double, size_t>> projected;
        size_t node_count = 0;
        for (const auto& node : nodes.array) {
            if (node.kind != JsonValue::Kind::ARRAY || node.array.size() < 4)
                throw std::runtime_error("thermal node must contain x, y, z, temperature");
            const double x = node.array[0].as_number(), y = node.array[1].as_number();
            node.array[2].as_number();
            const double temperature = node.array[3].as_number();
            const auto xy = std::make_pair(std::round(x * 1e9) / 1e9, std::round(y * 1e9) / 1e9);
            if (!std::isfinite(xy.first) || !std::isfinite(xy.second))
                throw std::runtime_error("thermal coordinates overflow");
            auto& group = projected[xy];
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
        for (const auto& [xy, group] : projected) {
            const int col = locate_cell(grid.x_lines, die_area.x1 + xy.first * dbu_per_micron);
            const int row = locate_cell(grid.y_lines, die_area.y1 + xy.second * dbu_per_micron);
            if (col >= 0 && row >= 0) {
                sums[row][col] += group.first / static_cast<double>(group.second);
                if (!std::isfinite(sums[row][col])) throw std::runtime_error("GCell temperature sum overflow");
                ++counts[row][col];
            }
        }

        std::vector<std::vector<double>> heat(grid.rows, std::vector<double>(grid.cols));
        std::vector<HotspotCell> hotspots;
        for (int row = 0; row < grid.rows; ++row) {
            for (int col = 0; col < grid.cols; ++col) {
                if (counts[row][col] == 0) {
                    throw std::runtime_error("thermal JSON leaves GCell (" +
                                             std::to_string(col) + "," +
                                             std::to_string(row) + ") without nodes");
                }
                const double temperature = sums[row][col] / static_cast<double>(counts[row][col]);
                heat[row][col] = temperature;
                if (temperature > ambient) {
                    hotspots.push_back({col, row, temperature, temperature - ambient});
                }
            }
        }
        std::sort(hotspots.begin(), hotspots.end(), [](const HotspotCell& a, const HotspotCell& b) {
            if (a.temperature != b.temperature) return a.temperature > b.temperature;
            return std::tie(a.row, a.col) < std::tie(b.row, b.col);
        });
        ambient_ = ambient;
        heat_matrix_ = std::move(heat);
        hotspots_ = std::move(hotspots);
        std::cout << "[INFO] 热场解析完成: 节点 " << node_count << " 个, GCell "
                  << grid.cols * grid.rows << " 个, 热点 " << hotspots_.size() << " 个\n";
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << json_file_path << ": " << error.what() << "\n";
        return false;
    }
}

} // namespace eda
