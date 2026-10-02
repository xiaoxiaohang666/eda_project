#include "router/routing_graph.hpp"

#include <limits>
#include <set>

namespace eda {
namespace {
bool overlaps(const Rect& a, const Rect& b) {
    return a.x1 < b.x2 && a.x2 > b.x1 && a.y1 < b.y2 && a.y2 > b.y1;
}
size_t count_interval(const std::vector<double>& values, double lo, double hi, bool last) {
    const auto first = std::lower_bound(values.begin(), values.end(), lo);
    const auto end = last ? std::upper_bound(values.begin(), values.end(), hi)
                          : std::lower_bound(values.begin(), values.end(), hi);
    return static_cast<size_t>(end - first);
}
bool same_die(int a, int b) { return (a <= 10) == (b <= 10); }
} // namespace

RoutingGraph::RoutingGraph(const DefParser& def, const LefParser& lef)
    : grid_(def.gcell_grid()), die_(def.die_area()), dbu_(def.dbu_per_micron()) {
    if (!grid_.valid() || dbu_ <= 0) throw std::runtime_error("routing graph requires a valid DEF grid");
    plane_ = static_cast<size_t>(grid_.cols) * static_cast<size_t>(grid_.rows);
    if (plane_ > static_cast<size_t>(std::numeric_limits<int>::max()) / 20)
        throw std::runtime_error("routing graph exceeds integer node limit");
    std::vector<double> steps;
    for (const auto* lines : {&grid_.x_lines, &grid_.y_lines})
        for (size_t i = 1; i < lines->size(); ++i) steps.push_back((*lines)[i] - (*lines)[i - 1]);
    std::sort(steps.begin(), steps.end()); ell0_ = steps[steps.size() / 2];
    for (int l = 1; l <= 20; ++l) {
        const auto* info = lef.get_layer(get_layer_name(l));
        if (!info || info->direction == Direction::UNKNOWN || info->width <= 0)
            throw std::runtime_error("routing graph needs DIRECTION and WIDTH for " + get_layer_name(l));
        directions_[l] = info->direction; widths_[l] = info->width * dbu_;
        obstacle_cells_[l].resize(plane_);
    }
    for (const auto& pattern : def.tracks()) {
        const int axis = pattern.axis == 'X' ? 0 : 1;
        const double lo = axis == 0 ? die_.x1 : die_.y1;
        const double hi = axis == 0 ? die_.x2 : die_.y2;
        const double first = std::max(0.0, std::ceil((lo - pattern.start) / pattern.step));
        const double last = std::min(static_cast<double>(pattern.count - 1),
                                     std::floor((hi - pattern.start) / pattern.step));
        if (last - first > 10000000) throw std::runtime_error("TRACKS expansion exceeds supported size");
        for (const auto& name : pattern.layers) {
            const int l = get_layer_index(name);
            for (double i = first; i <= last; ++i)
                scorer_tracks_[l][axis].push_back(pattern.start + i * pattern.step);
        }
    }
    for (int l = 1; l <= 20; ++l) {
        for (int axis = 0; axis < 2; ++axis) {
            auto& tracks = scorer_tracks_[l][axis];
            std::sort(tracks.begin(), tracks.end());
            physical_tracks_[l][axis] = tracks;
            auto& unique = physical_tracks_[l][axis];
            unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        }
        const int physical_axis = directions_[l] == Direction::HORIZONTAL ? 1 : 0;
        const int scorer_axis = 1 - physical_axis;
        if (physical_tracks_[l][physical_axis].empty() || scorer_tracks_[l][scorer_axis].empty())
            throw std::runtime_error("routing graph needs physical and scorer TRACKS for " + get_layer_name(l));
    }
    for (const auto& [name, component] : def.components()) {
        (void)name;
        const auto* macro = lef.get_macro(component.macro_type);
        if (!macro) throw std::runtime_error("missing component macro in routing graph");
        for (const auto& shape : macro->obstructions) {
            const int l = get_layer_index(shape.layer);
            if (!l) continue; // CUT/master-slice geometry has no planar graph layer.
            obstacles_[l].push_back(transform_macro_rect(shape.rect, *macro, component, dbu_));
        }
    }
    for (int l = 1; l <= 20; ++l) {
        for (size_t index = 0; index < obstacles_[l].size(); ++index) {
            const auto& rect = obstacles_[l][index];
            if (!overlaps(rect, die_)) continue;
            const int c0 = grid_.get_col(std::max(rect.x1, die_.x1));
            const int c1 = grid_.get_col(std::min(rect.x2, die_.x2));
            const int r0 = grid_.get_row(std::max(rect.y1, die_.y1));
            const int r1 = grid_.get_row(std::min(rect.y2, die_.y2));
            for (int r = r0; r <= r1; ++r)
                for (int c = c0; c <= c1; ++c)
                    obstacle_cells_[l][static_cast<size_t>(r) * grid_.cols + c].push_back(index);
        }
    }
    for (const auto& [name, via] : lef.vias()) {
        (void)name;
        std::set<int> metal_layers;
        bool cut = false;
        for (const auto& shape : via.shapes) {
            const int l = get_layer_index(shape.layer);
            if (l) metal_layers.insert(l);
            else if (const auto* info = lef.get_layer(shape.layer); info && info->type == "CUT") cut = true;
        }
        if (metal_layers.size() != 2 || !cut) continue;
        const int low = *metal_layers.begin(), high = *metal_layers.rbegin();
        if (high == low + 1 && same_die(low, high)) vias_[low].push_back(&via);
    }
    for (auto& list : vias_) {
        std::sort(list.begin(), list.end(), [](const ViaInfo* a, const ViaInfo* b) {
            if (a->is_default != b->is_default) return a->is_default > b->is_default;
            return a->name < b->name;
        });
    }
    legal_.resize(20 * plane_); via_up_.resize(size());
    planar_forward_.resize(size()); guide_capacity_.resize(size());
    for (int id = 0; id < static_cast<int>(size()); ++id) {
        const int l = layer(id); const auto p = point(id); const auto rect = cell(id);
        legal_[id] = legal_segment(p, p, l);
        const bool horizontal = directions_[l] == Direction::HORIZONTAL;
        guide_capacity_[id] = static_cast<double>(horizontal
            ? count_interval(scorer_tracks_[l][0], rect.x1, rect.x2, col(id) == grid_.cols - 1)
            : count_interval(scorer_tracks_[l][1], rect.y1, rect.y2, row(id) == grid_.rows - 1));
    }
    for (int id = 0; id < static_cast<int>(size()); ++id) {
        if (!legal_[id]) continue;
        const int l = layer(id), c = col(id), r = row(id);
        const int next = directions_[l] == Direction::HORIZONTAL ? node(c + 1, r, l) : node(c, r + 1, l);
        if (legal_node(next) && legal_segment(point(id), point(next), l))
            planar_forward_[id] = physical_capacity(id, next);
        const int above = node(c, r, l + 1);
        if (legal_node(above) && same_die(l, l + 1)) via_up_[id] = legal_via(point(id), l);
    }
}

bool RoutingGraph::valid_id(int id) const { return id >= 0 && static_cast<size_t>(id) < size(); }
int RoutingGraph::node(int c, int r, int l) const {
    if (c < 0 || c >= grid_.cols || r < 0 || r >= grid_.rows || l < 1 || l > 20) return -1;
    return static_cast<int>(static_cast<size_t>(l - 1) * plane_ + static_cast<size_t>(r) * grid_.cols + c);
}
int RoutingGraph::col(int id) const {
    if (!valid_id(id)) throw std::out_of_range("invalid routing node");
    return id % grid_.cols;
}
int RoutingGraph::row(int id) const {
    if (!valid_id(id)) throw std::out_of_range("invalid routing node");
    return static_cast<int>((static_cast<size_t>(id) % plane_) / grid_.cols);
}
int RoutingGraph::layer(int id) const {
    if (!valid_id(id)) throw std::out_of_range("invalid routing node");
    return static_cast<int>(static_cast<size_t>(id) / plane_) + 1;
}
Point RoutingGraph::point(int id) const { return cell(id).center(); }
Rect RoutingGraph::cell(int id) const { return grid_.get_cell_rect(col(id), row(id)); }
bool RoutingGraph::legal_node(int id) const { return valid_id(id) && legal_[id] != 0; }
double RoutingGraph::routing_width(int l) const {
    if (l < 1 || l > 20) throw std::out_of_range("invalid routing layer");
    return widths_[l];
}
bool RoutingGraph::legal_rect(const Rect& rect, int l) const {
    if (l < 1 || l > 20 || !std::isfinite(rect.x1) || !std::isfinite(rect.y1) ||
        !std::isfinite(rect.x2) || !std::isfinite(rect.y2) ||
        rect.width() <= 0 || rect.height() <= 0 ||
        rect.x1 < die_.x1 || rect.y1 < die_.y1 || rect.x2 > die_.x2 || rect.y2 > die_.y2) return false;
    const int c0 = grid_.get_col(rect.x1), c1 = grid_.get_col(rect.x2);
    const int r0 = grid_.get_row(rect.y1), r1 = grid_.get_row(rect.y2);
    for (int r = r0; r <= r1; ++r)
        for (int c = c0; c <= c1; ++c)
            for (size_t index : obstacle_cells_[l][static_cast<size_t>(r) * grid_.cols + c])
                if (overlaps(rect, obstacles_[l][index])) return false;
    return true;
}
Rect RoutingGraph::segment_rect(Point a, Point b, int l) const {
    const double half = routing_width(l) * 0.5;
    return {std::floor(std::min(a.x, b.x) - half), std::floor(std::min(a.y, b.y) - half),
            std::ceil(std::max(a.x, b.x) + half), std::ceil(std::max(a.y, b.y) + half)};
}
bool RoutingGraph::legal_segment(Point a, Point b, int l) const {
    if (l < 1 || l > 20 || !std::isfinite(a.x) || !std::isfinite(a.y) ||
        !std::isfinite(b.x) || !std::isfinite(b.y) || (a.x != b.x && a.y != b.y)) return false;
    return legal_rect(segment_rect(a, b, l), l);
}
bool RoutingGraph::via_shapes(Point p, const ViaInfo& via, std::vector<LayerRect>* result) const {
    std::vector<LayerRect> rectangles;
    for (const auto& shape : via.shapes) {
        const int l = get_layer_index(shape.layer);
        Rect rect{std::floor(p.x + shape.rect.x1 * dbu_), std::floor(p.y + shape.rect.y1 * dbu_),
                  std::ceil(p.x + shape.rect.x2 * dbu_), std::ceil(p.y + shape.rect.y2 * dbu_)};
        if (rect.x1 < die_.x1 || rect.y1 < die_.y1 || rect.x2 > die_.x2 || rect.y2 > die_.y2) return false;
        if (!l) continue; // CUT shapes must also fit inside DIEAREA.
        if (!legal_rect(rect, l)) return false;
        rectangles.push_back({shape.layer, rect});
    }
    if (rectangles.empty()) return false;
    if (result) *result = std::move(rectangles);
    return true;
}
bool RoutingGraph::legal_via(Point p, int lower) const {
    if (lower < 1 || lower >= 20 || lower == 10 || !std::isfinite(p.x) || !std::isfinite(p.y)) return false;
    for (const auto* via : vias_[lower]) if (via_shapes(p, *via, nullptr)) return true;
    return false;
}
std::vector<LayerRect> RoutingGraph::via_rects(Point p, int lower) const {
    std::vector<LayerRect> result;
    if (lower < 1 || lower >= 20 || lower == 10 || !std::isfinite(p.x) || !std::isfinite(p.y)) return result;
    for (const auto* via : vias_[lower]) if (via_shapes(p, *via, &result)) return result;
    return result;
}
double RoutingGraph::physical_capacity(int a, int b) const {
    const int l = layer(a); const bool horizontal = directions_[l] == Direction::HORIZONTAL;
    const auto box = cell(a); const auto pa = point(a), pb = point(b);
    const auto& tracks = physical_tracks_[l][horizontal ? 1 : 0];
    const double lo = horizontal ? box.y1 : box.x1, hi = horizontal ? box.y2 : box.x2;
    const bool last = horizontal ? row(a) == grid_.rows - 1 : col(a) == grid_.cols - 1;
    auto it = std::lower_bound(tracks.begin(), tracks.end(), lo);
    double capacity = 0;
    for (; it != tracks.end() && (*it < hi || (last && *it == hi)); ++it) {
        const Point start = horizontal ? Point{pa.x, *it} : Point{*it, pa.y};
        const Point end = horizontal ? Point{pb.x, *it} : Point{*it, pb.y};
        if (legal_segment(start, end, l)) capacity += 1;
    }
    return capacity;
}
double RoutingGraph::edge_capacity(int a, int b) const {
    if (!legal_node(a) || !legal_node(b)) return 0;
    const int al = layer(a), bl = layer(b);
    if (al == bl) {
        const bool horizontal = directions_[al] == Direction::HORIZONTAL;
        if ((horizontal && row(a) == row(b) && std::abs(col(a) - col(b)) == 1) ||
            (!horizontal && col(a) == col(b) && std::abs(row(a) - row(b)) == 1))
            return planar_forward_[std::min(a, b)];
    } else if (col(a) == col(b) && row(a) == row(b) && std::abs(al - bl) == 1 && same_die(al, bl)) {
        return via_up_[std::min(a, b)] ? -1 : 0;
    }
    return 0;
}
bool RoutingGraph::legal_edge(int a, int b) const { return edge_capacity(a, b) != 0; }
double RoutingGraph::edge_length(int a, int b) const {
    if (!legal_edge(a, b)) return std::numeric_limits<double>::infinity();
    return point(a).distance_to(point(b));
}
double RoutingGraph::guide_capacity(int id) const {
    if (!valid_id(id)) throw std::out_of_range("invalid routing node");
    return guide_capacity_[id];
}
std::vector<int> RoutingGraph::neighbors(int id) const {
    std::vector<int> result;
    if (!legal_node(id)) return result;
    const int c = col(id), r = row(id), l = layer(id);
    const bool horizontal = directions_[l] == Direction::HORIZONTAL;
    for (int next : {node(c - (horizontal ? 1 : 0), r - (horizontal ? 0 : 1), l),
                     node(c + (horizontal ? 1 : 0), r + (horizontal ? 0 : 1), l),
                     node(c, r, l - 1), node(c, r, l + 1)})
        if (legal_edge(id, next)) result.push_back(next);
    return result;
}
} // namespace eda
