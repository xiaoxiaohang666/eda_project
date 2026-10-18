#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <map>
#include <cstdint>
#include <charconv>
#include <stdexcept>

namespace eda {

enum class Direction {
    HORIZONTAL,
    VERTICAL,
    UNKNOWN
};

inline std::string direction_to_string(Direction dir) {
    if (dir == Direction::HORIZONTAL) return "HORIZONTAL";
    if (dir == Direction::VERTICAL) return "VERTICAL";
    return "UNKNOWN";
}

struct Point {
    double x{0.0};
    double y{0.0};

    Point() = default;
    Point(double px, double py) : x(px), y(py) {}

    double distance_to(const Point& other) const {
        return std::abs(x - other.x) + std::abs(y - other.y);
    }
};

struct Rect {
    double x1{0.0};
    double y1{0.0};
    double x2{0.0};
    double y2{0.0};

    Rect() = default;
    Rect(double _x1, double _y1, double _x2, double _y2)
        : x1(std::min(_x1, _x2)), y1(std::min(_y1, _y2)),
          x2(std::max(_x1, _x2)), y2(std::max(_y1, _y2)) {}

    double width() const { return x2 - x1; }
    double height() const { return y2 - y1; }
    Point center() const { return Point((x1 + x2) * 0.5, (y1 + y2) * 0.5); }

    bool contains(const Point& p) const {
        return p.x >= x1 && p.x <= x2 && p.y >= y1 && p.y <= y2;
    }

    bool intersects(const Rect& other) const {
        return !(x2 < other.x1 || x1 > other.x2 || y2 < other.y1 || y1 > other.y2);
    }
};

struct GuideRect {
    double x1{0.0};
    double y1{0.0};
    double x2{0.0};
    double y2{0.0};
    std::string layer;

    GuideRect() = default;
    GuideRect(double _x1, double _y1, double _x2, double _y2, std::string _layer)
        : x1(std::min(_x1, _x2)), y1(std::min(_y1, _y2)),
          x2(std::max(_x1, _x2)), y2(std::max(_y1, _y2)),
          layer(std::move(_layer)) {}

    Point center() const { return Point((x1 + x2) * 0.5, (y1 + y2) * 0.5); }
};

enum class PinOwnerType {
    UNKNOWN,
    TOP_LEVEL,
    CORE_CELL,
    BLOCK_MACRO,
    OTHER_MACRO
};

// Existing terminal metal, transformed into global DEF DBU. This is not a
// new routed wire and is not enlarged to the technology routing WIDTH.
struct PinShape {
    int layer_idx{0};
    Rect rect;
};

struct Pin {
    std::string name;
    std::string layer;
    Point coord;
    int layer_idx{0}; // 1..20
    int gcell_col{-1};
    int gcell_row{-1};
    Point evaluator_coord; // Fixed checker ignores instance orientation/LEF ORIGIN.
    bool has_evaluator_coord{false};
    std::string instance_name; // Empty for a top-level DEF pin.
    PinOwnerType owner_type{PinOwnerType::UNKNOWN};
    Rect owner_bbox; // Oriented LEF SIZE boundary in global DEF DBU.
    bool has_owner_bbox{false};
    std::vector<PinShape> port_rects;
};

struct Net {
    std::string name;
    std::vector<Pin> pins;
    bool is_cross_die{false};

    bool has_bottom_pin() const {
        for (const auto& pin : pins) {
            if (pin.layer_idx >= 1 && pin.layer_idx <= 10) return true;
        }
        return false;
    }

    bool has_top_pin() const {
        for (const auto& pin : pins) {
            if (pin.layer_idx >= 11 && pin.layer_idx <= 20) return true;
        }
        return false;
    }
};

struct GCellGrid {
    std::vector<double> x_lines;
    std::vector<double> y_lines;
    int cols{0};
    int rows{0};

    bool valid() const {
        auto ordered = [](const std::vector<double>& lines) {
            for (size_t i = 0; i < lines.size(); ++i) {
                if (!std::isfinite(lines[i]) || (i && lines[i] <= lines[i - 1])) return false;
            }
            return true;
        };
        return cols > 0 && rows > 0 && x_lines.size() == static_cast<size_t>(cols) + 1 &&
               y_lines.size() == static_cast<size_t>(rows) + 1 && ordered(x_lines) && ordered(y_lines);
    }

    int get_col(double x) const {
        if (cols <= 0 || x_lines.size() != static_cast<size_t>(cols) + 1 || !std::isfinite(x) ||
            x < x_lines.front() || x > x_lines.back()) return -1;
        if (x <= x_lines.front()) return 0;
        if (x >= x_lines.back()) return static_cast<int>(x_lines.size()) - 2;
        auto it = std::upper_bound(x_lines.begin(), x_lines.end(), x);
        int idx = static_cast<int>(it - x_lines.begin()) - 1;
        return std::clamp(idx, 0, cols - 1);
    }

    int get_row(double y) const {
        if (rows <= 0 || y_lines.size() != static_cast<size_t>(rows) + 1 || !std::isfinite(y) ||
            y < y_lines.front() || y > y_lines.back()) return -1;
        if (y <= y_lines.front()) return 0;
        if (y >= y_lines.back()) return static_cast<int>(y_lines.size()) - 2;
        auto it = std::upper_bound(y_lines.begin(), y_lines.end(), y);
        int idx = static_cast<int>(it - y_lines.begin()) - 1;
        return std::clamp(idx, 0, rows - 1);
    }

    Rect get_cell_rect(int col, int row) const {
        if (col < 0 || col >= cols || row < 0 || row >= rows ||
            x_lines.size() != static_cast<size_t>(cols) + 1 ||
            y_lines.size() != static_cast<size_t>(rows) + 1)
            throw std::out_of_range("invalid GCell index");
        double x1 = x_lines[col];
        double x2 = x_lines[col + 1];
        double y1 = y_lines[row];
        double y2 = y_lines[row + 1];
        return Rect(x1, y1, x2, y2);
    }
};

inline int get_layer_index(const std::string& layer_name) {
    if (layer_name.rfind("metal", 0) == 0) {
        int index = 0;
        const char* begin = layer_name.data() + 5;
        const char* end = layer_name.data() + layer_name.size();
        const auto result = std::from_chars(begin, end, index);
        if (result.ec == std::errc{} && result.ptr == end && index >= 1 && index <= 20 &&
            layer_name == "metal" + std::to_string(index)) return index;
    }
    return 0;
}

inline std::string get_layer_name(int idx) {
    return "metal" + std::to_string(idx);
}

} // namespace eda
