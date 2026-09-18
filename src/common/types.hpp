#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <map>
#include <cstdint>

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

struct Pin {
    std::string name;
    std::string layer;
    Point coord;
    int layer_idx{0}; // 1..20
    int gcell_col{-1};
    int gcell_row{-1};
};

struct Net {
    std::string name;
    std::vector<Pin> pins;
    bool is_cross_die{false};

    bool has_bottom_pin() const {
        for (const auto& pin : pins) {
            if (pin.layer_idx <= 10) return true;
        }
        return false;
    }

    bool has_top_pin() const {
        for (const auto& pin : pins) {
            if (pin.layer_idx >= 11) return true;
        }
        return false;
    }
};

struct GCellGrid {
    std::vector<double> x_lines;
    std::vector<double> y_lines;
    int cols{0};
    int rows{0};

    int get_col(double x) const {
        if (x_lines.empty()) return -1;
        if (x <= x_lines.front()) return 0;
        if (x >= x_lines.back()) return static_cast<int>(x_lines.size()) - 2;
        auto it = std::upper_bound(x_lines.begin(), x_lines.end(), x);
        int idx = static_cast<int>(it - x_lines.begin()) - 1;
        return std::clamp(idx, 0, cols - 1);
    }

    int get_row(double y) const {
        if (y_lines.empty()) return -1;
        if (y <= y_lines.front()) return 0;
        if (y >= y_lines.back()) return static_cast<int>(y_lines.size()) - 2;
        auto it = std::upper_bound(y_lines.begin(), y_lines.end(), y);
        int idx = static_cast<int>(it - y_lines.begin()) - 1;
        return std::clamp(idx, 0, rows - 1);
    }

    Rect get_cell_rect(int col, int row) const {
        double x1 = x_lines[col];
        double x2 = x_lines[col + 1];
        double y1 = y_lines[row];
        double y2 = y_lines[row + 1];
        return Rect(x1, y1, x2, y2);
    }
};

inline int get_layer_index(const std::string& layer_name) {
    if (layer_name.rfind("metal", 0) == 0) {
        try {
            return std::stoi(layer_name.substr(5));
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

inline std::string get_layer_name(int idx) {
    return "metal" + std::to_string(idx);
}

} // namespace eda
