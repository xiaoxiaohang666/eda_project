#include "scoring/compat_resources.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace eda::compat {
namespace {

// Positive-area shapes use the cells whose interiors they intersect. A point
// or line uses the containing half-open cell, with the last cell closed.
std::pair<int, int> covered_cells(double low, double high, const std::vector<double>& lines) {
    if (!std::isfinite(low) || !std::isfinite(high))
        throw std::runtime_error("non-finite geometry in compatibility resources");
    if (high < low) std::swap(low, high);
    if (high < lines.front() || low > lines.back()) return {0, 0};
    const int count = static_cast<int>(lines.size()) - 1;
    if (low == high) {
        const int cell = std::clamp(
            static_cast<int>(std::upper_bound(lines.begin(), lines.end(), low) - lines.begin()) - 1,
            0, count - 1);
        return {cell, cell + 1};
    }
    if (high <= lines.front() || low >= lines.back()) return {0, 0};
    const int begin = std::clamp(
        static_cast<int>(std::upper_bound(lines.begin(), lines.end(), low) - lines.begin()) - 1,
        0, count);
    const int end = std::clamp(
        static_cast<int>(std::lower_bound(lines.begin(), lines.end(), high) - lines.begin()),
        0, count);
    return {begin, end};
}

template<class Visitor>
void visit_cells(const Rect& rect, const GCellGrid& grid, Visitor&& visitor) {
    const auto [cb, ce] = covered_cells(rect.x1, rect.x2, grid.x_lines);
    const auto [rb, re] = covered_cells(rect.y1, rect.y2, grid.y_lines);
    for (int r = rb; r < re; ++r)
        for (int c = cb; c < ce; ++c) visitor(c, r);
}

uint32_t checked_count(size_t count) {
    if (count > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("compatibility track count exceeds uint32 range");
    return static_cast<uint32_t>(count);
}

} // namespace

Resources::Resources(const DefParser& def, const LefParser& lef, ResourceOptions options)
    : grid_(def.gcell_grid()), options_(options) {
    if (options_.platform_20261024 && (!options_.pin_first_layer || options_.deduplicate_tracks))
        throw std::runtime_error("platform resources require first-layer macro PIN projection and repeated tracks");
    if (!grid_.valid() || !std::isfinite(def.dbu_per_micron()) || def.dbu_per_micron() <= 0)
        throw std::runtime_error("compatibility resources require a valid DEF grid and units");
    plane_ = static_cast<size_t>(grid_.cols) * static_cast<size_t>(grid_.rows);
    if (plane_ > static_cast<size_t>(std::numeric_limits<int>::max()) / 20)
        throw std::runtime_error("compatibility grid exceeds integer node limit");
    fixed_demand_.assign(20 * plane_, 0);
    std::array<std::vector<double>, 21> tracks;
    for (int l = 1; l <= 20; ++l)
        directions_[l] = lef.get_layer_direction(get_layer_name(l));

    for (const auto& pattern : def.tracks()) {
        if (pattern.count > 10000000)
            throw std::runtime_error("compatibility TRACKS expansion exceeds supported size");
        for (const auto& name : pattern.layers) {
            const int l = get_layer_index(name);
            if (!l || directions_[l] == Direction::UNKNOWN) continue;
            const bool horizontal = directions_[l] == Direction::HORIZONTAL;
            if (pattern.axis != (horizontal ? 'Y' : 'X')) continue;
            const auto& lines = horizontal ? grid_.y_lines : grid_.x_lines;
            auto& values = tracks[l];
            for (int i = 0; i < pattern.count; ++i) {
                const double coordinate = pattern.start + i * pattern.step;
                // A TRACKS declaration may extend beyond GCELLGRID. It must
                // not be clamped into either edge cell.
                if (coordinate >= lines.front() && coordinate <= lines.back())
                    values.push_back(coordinate);
            }
        }
    }
    for (int l = 1; l <= 20; ++l) {
        auto& values = tracks[l];
        std::sort(values.begin(), values.end());
        if (options_.deduplicate_tracks)
            values.erase(std::unique(values.begin(), values.end()), values.end());
        checked_count(values.size());
        const auto& lines = directions_[l] == Direction::HORIZONTAL ? grid_.y_lines : grid_.x_lines;
        auto& caps = capacities_[l];
        caps.reserve(lines.size() - 1);
        for (size_t i = 0; i + 1 < lines.size(); ++i) {
            const auto begin = std::lower_bound(values.begin(), values.end(), lines[i]);
            const auto end = i + 2 == lines.size()
                ? std::upper_bound(values.begin(), values.end(), lines[i + 1])
                : std::lower_bound(values.begin(), values.end(), lines[i + 1]);
            caps.push_back(checked_count(static_cast<size_t>(end - begin)));
        }
    }

    std::array<std::vector<Rect>, 21> occupied;
    for (const auto& [name, component] : def.components()) {
        (void)name;
        const auto* macro = lef.get_macro(component.macro_type);
        if (!macro) throw std::runtime_error("missing macro in compatibility fixed demand");
        for (const auto& shape : macro->obstructions) {
            const int l = get_layer_index(shape.layer);
            if (l) occupied[l].push_back(
                transform_macro_rect(shape.rect, *macro, component, def.dbu_per_micron()));
        }
        // Include all master pins, including pins absent from DEF NETS.
        for (const auto& [pin_name, pin] : macro->pins) {
            (void)pin_name;
            for (const auto& shape : pin.port_rects) {
                const auto& first_layer = options_.platform_20261024 ? pin.port_rects.front().layer : pin.layer;
                const int l = get_layer_index(options_.pin_first_layer ? first_layer : shape.layer);
                if (l) occupied[l].push_back(
                    transform_macro_rect(shape.rect, *macro, component, def.dbu_per_micron()));
            }
        }
    }
    // DEF parser has already applied IO placement and orientation in DBU.
    for (const auto& [name, pin] : def.io_pins()) {
        (void)name;
        for (const auto& shape : pin.port_rects) {
            const int l = options_.pin_first_layer && !options_.platform_20261024
                ? pin.layer_idx : shape.layer_idx;
            if (l >= 1 && l <= 20) occupied[l].push_back(shape.rect);
        }
    }

    // All fixed shapes are unioned by track index per cell-layer, independent
    // of whether they came from OBS, macro PIN or IO PIN. Intervals avoid a
    // separate allocation for every track in every occupied cell.
    using Interval = std::pair<uint32_t, uint32_t>;
    for (int l = 1; l <= 20; ++l) {
        const auto& values = tracks[l];
        if (values.empty()) {
            std::vector<Rect>().swap(occupied[l]);
            continue;
        }
        const bool horizontal = directions_[l] == Direction::HORIZONTAL;
        const auto& lines = horizontal ? grid_.y_lines : grid_.x_lines;
        std::unordered_map<size_t, std::vector<Interval>> ranges;
        for (const auto& rect : occupied[l]) {
            visit_cells(rect, grid_, [&](int c, int r) {
                const size_t band = static_cast<size_t>(horizontal ? r : c);
                const double low = std::max(horizontal ? rect.y1 : rect.x1, lines[band]);
                const double high = std::min(horizontal ? rect.y2 : rect.x2, lines[band + 1]);
                if (high < low) return;
                const auto begin = std::lower_bound(values.begin(), values.end(), low);
                // Historical shapes include their upper-edge tracks except
                // at internal cell boundaries. Platform shapes are half-open
                // even at the shape's final exterior boundary. If a shape
                // extends beyond the grid, the last grid track is interior to
                // the shape and is retained by the closed final capacity band.
                const auto shape_end = std::lower_bound(values.begin(), values.end(),
                    horizontal ? rect.y2 : rect.x2);
                const auto band_end = band + 2 == lines.size()
                    ? std::upper_bound(values.begin(), values.end(), lines[band + 1])
                    : std::lower_bound(values.begin(), values.end(), lines[band + 1]);
                const auto end = options_.platform_20261024
                    ? std::min(shape_end, band_end)
                    : ((high == lines[band + 1] && band + 2 < lines.size())
                        ? std::lower_bound(values.begin(), values.end(), high)
                        : std::upper_bound(values.begin(), values.end(), high));
                if (begin == end) return;
                ranges[static_cast<size_t>(r) * grid_.cols + c].emplace_back(
                    checked_count(static_cast<size_t>(begin - values.begin())),
                    checked_count(static_cast<size_t>(end - values.begin())));
            });
        }
        for (auto& [cell, intervals] : ranges) {
            std::sort(intervals.begin(), intervals.end());
            auto [begin, end] = intervals.front();
            size_t count = 0;
            for (size_t i = 1; i < intervals.size(); ++i) {
                const auto [next_begin, next_end] = intervals[i];
                if (next_begin <= end) end = std::max(end, next_end);
                else {
                    count += end - begin;
                    begin = next_begin;
                    end = next_end;
                }
            }
            count += end - begin;
            fixed_demand_[static_cast<size_t>(l - 1) * plane_ + cell] = checked_count(count);
        }
        std::vector<Rect>().swap(occupied[l]);
        std::vector<double>().swap(tracks[l]);
    }
}

void Resources::check_id(int id) const {
    if (id < 0 || static_cast<size_t>(id) >= size())
        throw std::out_of_range("invalid compatibility resource node");
}
int Resources::node(int c, int r, int l) const {
    if (c < 0 || c >= grid_.cols || r < 0 || r >= grid_.rows || l < 1 || l > 20) return -1;
    return static_cast<int>(static_cast<size_t>(l - 1) * plane_ +
                            static_cast<size_t>(r) * grid_.cols + c);
}
int Resources::col(int id) const {
    check_id(id);
    return static_cast<int>(static_cast<size_t>(id) % plane_ % grid_.cols);
}
int Resources::row(int id) const {
    check_id(id);
    return static_cast<int>(static_cast<size_t>(id) % plane_ / grid_.cols);
}
int Resources::layer(int id) const {
    check_id(id);
    return static_cast<int>(static_cast<size_t>(id) / plane_) + 1;
}
uint32_t Resources::capacity(int id) const {
    const int l = layer(id);
    return capacities_[l][directions_[l] == Direction::HORIZONTAL ? row(id) : col(id)];
}
uint32_t Resources::fixed_demand(int id) const {
    check_id(id);
    return fixed_demand_[static_cast<size_t>(id)];
}
std::vector<int> Resources::pin_nodes(const Net& net) const {
    std::vector<int> result;
    for (const auto& pin : net.pins) {
        for (const auto& shape : pin.port_rects) {
            const bool macro_pin = !pin.instance_name.empty();
            if (options_.platform_20261024 && macro_pin &&
                shape.layer_idx != pin.port_rects.front().layer_idx) continue;
            const int l = options_.platform_20261024 ? shape.layer_idx
                : (options_.pin_first_layer ? pin.layer_idx : shape.layer_idx);
            if (l < 1 || l > 20) continue;
            visit_cells(shape.rect, grid_, [&](int c, int r) { result.push_back(node(c, r, l)); });
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace eda::compat
