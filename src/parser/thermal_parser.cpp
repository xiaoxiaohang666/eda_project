#include "parser/thermal_parser.hpp"

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
#include <vector>

namespace eda {
namespace {

void skip_space(const std::string& source, size_t& pos) {
    while (pos < source.size() &&
           (source[pos] == ' ' || source[pos] == '\n' ||
            source[pos] == '\r' || source[pos] == '\t')) {
        ++pos;
    }
}

void expect(const std::string& source, size_t& pos, char required) {
    skip_space(source, pos);
    if (pos >= source.size() || source[pos] != required) {
        throw std::runtime_error(std::string("expected '") + required + "' in thermal JSON");
    }
    ++pos;
}

double read_number(const std::string& source, size_t& pos) {
    skip_space(source, pos);
    if (pos >= source.size()) {
        throw std::runtime_error("unexpected end of thermal JSON number");
    }
    const char* start = source.c_str() + pos;
    char* end = nullptr;
    const double value = std::strtod(start, &end);
    if (end == start || !std::isfinite(value)) {
        throw std::runtime_error("invalid thermal JSON number");
    }
    pos += static_cast<size_t>(end - start);
    return value;
}

size_t value_after_key(const std::string& source, const std::string& key) {
    const size_t key_pos = source.find('"' + key + '"');
    if (key_pos == std::string::npos) {
        throw std::runtime_error("missing '" + key + "' in thermal JSON");
    }
    size_t pos = key_pos + key.size() + 2;
    expect(source, pos, ':');
    return pos;
}

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
    heat_matrix_.clear();
    hotspots_.clear();

    try {
        if (grid.cols <= 0 || grid.rows <= 0 ||
            grid.x_lines.size() != static_cast<size_t>(grid.cols + 1) ||
            grid.y_lines.size() != static_cast<size_t>(grid.rows + 1) ||
            !std::isfinite(dbu_per_micron) || dbu_per_micron <= 0.0) {
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

        size_t pos = value_after_key(source, "ambientTemp");
        const double ambient = read_number(source, pos);
        pos = value_after_key(source, "nodes");
        expect(source, pos, '[');

        // The evaluator first averages nodes with the same (x, y) across z,
        // then averages those projected positions within each GCell.
        std::map<std::pair<double, double>, std::pair<double, size_t>> projected;
        size_t node_count = 0;
        while (true) {
            skip_space(source, pos);
            if (pos < source.size() && source[pos] == ']') {
                ++pos;
                break;
            }
            if (node_count > 0) {
                expect(source, pos, ',');
            }
            expect(source, pos, '[');
            std::vector<double> values;
            while (true) {
                values.push_back(read_number(source, pos));
                skip_space(source, pos);
                if (pos < source.size() && source[pos] == ']') {
                    ++pos;
                    break;
                }
                expect(source, pos, ',');
            }
            if (values.size() < 4) {
                throw std::runtime_error("thermal node must contain x, y, z, temperature");
            }
            const auto xy = std::make_pair(std::round(values[0] * 1e9) / 1e9,
                                           std::round(values[1] * 1e9) / 1e9);
            auto& group = projected[xy];
            group.first += values[3];
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
            return a.temperature > b.temperature;
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
