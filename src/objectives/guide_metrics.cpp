#include "objectives/guide_metrics.hpp"
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <tuple>

namespace eda {
namespace {
using RectKey = std::tuple<double, double, double, double, std::string>;
std::vector<GuideRect> unique_rectangles(const std::vector<GuideRect>& raw) {
    std::set<RectKey> seen;
    std::vector<GuideRect> result;
    for (const auto& r : raw) {
        if (!std::isfinite(r.x1) || !std::isfinite(r.y1) || !std::isfinite(r.x2) ||
            !std::isfinite(r.y2) || r.x1 > r.x2 || r.y1 > r.y2 || !get_layer_index(r.layer))
            throw std::runtime_error("invalid rectangle in guide metrics");
        if (seen.emplace(r.x1, r.y1, r.x2, r.y2, r.layer).second) result.push_back(r);
    }
    return result;
}
bool connects(const GuideRect& a, const GuideRect& b) {
    return std::abs(get_layer_index(a.layer) - get_layer_index(b.layer)) <= 1 &&
           a.x1 <= b.x2 + 1e-6 && b.x1 <= a.x2 + 1e-6 &&
           a.y1 <= b.y2 + 1e-6 && b.y1 <= a.y2 + 1e-6;
}
std::pair<int, int> intervals(double lower, double upper, const std::vector<double>& lines) {
    // Python math.isclose default relative tolerance, zero absolute tolerance.
    const bool point = std::abs(upper - lower) <= 1e-9 * std::max(std::abs(lower), std::abs(upper));
    if (point) {
        if (lower < lines.front() || lower > lines.back()) return {0, 0};
        int cell = static_cast<int>(std::upper_bound(lines.begin(), lines.end(), lower) - lines.begin()) - 1;
        cell = std::min(cell, static_cast<int>(lines.size()) - 2);
        return {cell, cell + 1};
    }
    const int begin = std::max(0, static_cast<int>(std::upper_bound(lines.begin(), lines.end(), lower) - lines.begin()) - 1);
    const int end = std::min(static_cast<int>(lines.size()) - 1,
                             static_cast<int>(std::lower_bound(lines.begin(), lines.end(), upper) - lines.begin()));
    return {begin, end};
}
}

NetGuideMetrics measure_net_guides(const std::vector<GuideRect>& raw,
                                  const RoutingGraph& graph, const LefParser& lef,
                                  double tsv_cost, double via_cost) {
    const auto guides = unique_rectangles(raw);
    if (guides.empty()) throw std::runtime_error("cannot measure an empty guide net");
    NetGuideMetrics result;
    const auto& grid = graph.grid();
    std::map<int, int> demand;
    double xmin = std::numeric_limits<double>::infinity(), ymin = xmin;
    double xmax = -xmin, ymax = -xmin;
    bool bottom = false, top = false;
    for (const auto& r : guides) {
        const int layer = get_layer_index(r.layer);
        if (lef.get_layer_direction(r.layer) == Direction::UNKNOWN)
            throw std::runtime_error("guide layer has no routing direction: " + r.layer);
        const auto center = r.center();
        xmin = std::min(xmin, center.x); xmax = std::max(xmax, center.x);
        ymin = std::min(ymin, center.y); ymax = std::max(ymax, center.y);
        bottom = bottom || layer <= 10; top = top || layer >= 11;
        const double x1 = std::max(r.x1, grid.x_lines.front()), x2 = std::min(r.x2, grid.x_lines.back());
        const double y1 = std::max(r.y1, grid.y_lines.front()), y2 = std::min(r.y2, grid.y_lines.back());
        if (x2 < x1 || y2 < y1) continue;
        const auto [cb, ce] = intervals(x1, x2, grid.x_lines);
        const auto [rb, re] = intervals(y1, y2, grid.y_lines);
        for (int row = rb; row < re; ++row)
            for (int col = cb; col < ce; ++col) ++demand[graph.node(col, row, layer)];
    }
    result.demand.assign(demand.begin(), demand.end());
    result.reference = xmax - xmin + ymax - ymin + (bottom && top ? tsv_cost : 0.0);
    // The checker uses (weight, vertex, parent), not arbitrary equal-cost MSTs:
    // its scoring pass assigns direction-dependent lengths to the selected edges.
    using HeapItem = std::tuple<double, int, int>;
    std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>> queue;
    queue.emplace(0.0, 0, -1);
    std::vector<bool> visited(guides.size(), false);
    int reached = 0;
    while (!queue.empty()) {
        const auto [weight, u, parent] = queue.top(); queue.pop();
        if (visited[u]) continue;
        visited[u] = true; ++reached;
        if (parent >= 0) {
            const auto& a = guides[parent]; const auto& b = guides[u];
            if (a.layer != b.layer) result.wirelength += weight;
            else if (lef.get_layer_direction(a.layer) == Direction::HORIZONTAL)
                result.wirelength += std::max(a.x2, b.x2) - std::min(a.x1, b.x1);
            else result.wirelength += std::max(a.y2, b.y2) - std::min(a.y1, b.y1);
        }
        for (int v = 0; v < static_cast<int>(guides.size()); ++v) {
            if (visited[v] || !connects(guides[u], guides[v])) continue;
            const int lu = get_layer_index(guides[u].layer), lv = get_layer_index(guides[v].layer);
            double cost = guides[u].center().distance_to(guides[v].center());
            if (lu != lv) cost += ((lu <= 10) != (lv <= 10)) ? tsv_cost : via_cost * std::abs(lu - lv);
            queue.emplace(cost, v, u);
        }
    }
    if (reached != static_cast<int>(guides.size())) throw std::runtime_error("disconnected guide net in exact metrics");
    for (const auto& a : guides) {
        if (get_layer_index(a.layer) != 10) continue;
        for (const auto& b : guides) {
            if (get_layer_index(b.layer) != 11) continue;
            const double x1 = std::max(a.x1, b.x1), x2 = std::min(a.x2, b.x2);
            const double y1 = std::max(a.y1, b.y1), y2 = std::min(a.y2, b.y2);
            if (x1 >= x2 || y1 >= y2) continue;
            Point p{(x1 + x2) / 2.0, (y1 + y2) / 2.0};
            const int c = grid.get_col(p.x), r = grid.get_row(p.y);
            if (c >= 0 && r >= 0) {
                result.tsv_cells.push_back(r * grid.cols + c);
                result.tsv_points.push_back(p);
            }
        }
    }
    return result;
}

PearsonState::PearsonState(std::vector<double> heat, std::vector<int> counts)
    : heat_(std::move(heat)), counts_(std::move(counts)) {
    if (heat_.empty() || heat_.size() != counts_.size()) throw std::runtime_error("invalid Pearson vector dimensions");
    for (size_t i = 0; i < heat_.size(); ++i) {
        if (!std::isfinite(heat_[i]) || heat_[i] < 0 || counts_[i] < 0)
            throw std::runtime_error("invalid Pearson input");
        const long double h = heat_[i], t = counts_[i];
        heat_sum_ += h; heat_square_ += h * h;
        count_sum_ += t; count_square_ += t * t; product_ += h * t;
    }
}
double PearsonState::correlation_with(long double count_square, long double product) const {
    if (heat_sum_ <= 0 || count_sum_ <= 0) return 0.0;
    const long double n = static_cast<long double>(heat_.size());
    const long double vh = std::max(0.0L, heat_square_ - heat_sum_ * heat_sum_ / n);
    const long double vt = std::max(0.0L, count_square - count_sum_ * count_sum_ / n);
    const long double denominator = std::sqrt(vh) * std::sqrt(vt);
    // score.py checks the denominator AFTER normalizing each vector to unit sum.
    if (denominator <= 1e-15L * heat_sum_ * count_sum_) return 0.0;
    const long double numerator = product - heat_sum_ * count_sum_ / n;
    return std::clamp(static_cast<double>(numerator / denominator), -1.0, 1.0);
}
double PearsonState::correlation() const {
    return correlation_with(count_square_, product_);
}
double PearsonState::correlation_after_move(size_t from, size_t to) const {
    if (from >= counts_.size() || to >= counts_.size() || counts_[from] <= 0)
        throw std::runtime_error("invalid incremental TSV move");
    if (from == to) return correlation();
    const long double square = count_square_ + (2.0L * (counts_[to] - counts_[from]) + 2.0L);
    const long double product = product_ + (static_cast<long double>(heat_[to]) - heat_[from]);
    return correlation_with(square, product);
}
void PearsonState::move(int from, int to) {
    if (from < 0 || to < 0 || from >= static_cast<int>(counts_.size()) || to >= static_cast<int>(counts_.size()) || counts_[from] <= 0)
        throw std::runtime_error("invalid incremental TSV move");
    if (from == to) return;
    count_square_ += 2.0L * (counts_[to] - counts_[from]) + 2.0L;
    product_ += static_cast<long double>(heat_[to]) - heat_[from];
    --counts_[from]; ++counts_[to];
}

GuideMetrics evaluate_guides(const GuideMap& guides, const RoutingGraph& graph,
                            const LefParser& lef, const ThermalParser& thermal,
                            double tsv_cost, double via_cost) {
    GuideMetrics result;
    result.demand.assign(graph.size(), 0);
    const auto& grid = graph.grid();
    std::vector<double> heat(grid.cols * grid.rows);
    std::vector<int> counts(heat.size(), 0);
    for (int r = 0; r < grid.rows; ++r)
        for (int c = 0; c < grid.cols; ++c)
            heat[r * grid.cols + c] = std::max(0.0, thermal.get_temperature(c, r) - thermal.ambient_temperature());
    for (const auto& [name, rects] : guides) {
        const auto one = measure_net_guides(rects, graph, lef, tsv_cost, via_cost);
        result.wirelength += one.wirelength; result.reference += one.reference;
        for (const auto& [node, demand] : one.demand) result.demand.at(node) += demand;
        for (const int cell : one.tsv_cells) { ++counts[cell]; ++result.tsv_count; }
        // score.py also forbids prefixed intra-die nets that cross the interface.
        for (const auto& rect : rects) {
            const int l = get_layer_index(rect.layer);
            if ((name.rfind("BOTTOM__", 0) == 0 && l > 10) || (name.rfind("TOP__", 0) == 0 && l < 11))
                throw std::runtime_error("intra-die guide uses another die: " + name);
        }
    }
    for (int node = 0; node < static_cast<int>(result.demand.size()); ++node) {
        if (result.demand[node] == 0) continue;
        ++result.used_cells;
        const int excess = std::max(0, result.demand[node] - int(graph.guide_capacity(node)));
        result.overflow += excess; result.max_overflow = std::max(result.max_overflow, excess);
    }
    result.normalized = result.reference > 0 ? result.wirelength / result.reference : 100.0;
    if (result.tsv_count == 0) throw std::runtime_error("no TSVs available for full evaluator metrics");
    if (std::none_of(heat.begin(),heat.end(),[](double h){return h>0;}))
        throw std::runtime_error("thermal map contains no temperature rise above ambient");
    result.correlation = PearsonState(std::move(heat), std::move(counts)).correlation();
    result.quality = 0.45 / std::max(1.0, result.normalized) +
                     0.30 * std::max(0.0, 1.0 - result.overflow / 20.0) +
                     0.25 * (result.correlation + 1.0) / 2.0;
    return result;
}

GuideMetrics aggregate_cached_metrics(const std::vector<const NetGuideMetrics*>& nets,
                                     const std::vector<int>& aggregate_demand,
                                     const RoutingGraph& graph, const ThermalParser& thermal,
                                     bool capture_demand) {
    if (aggregate_demand.size() != graph.size())
        throw std::runtime_error("cached guide demand dimensions do not match routing graph");
    GuideMetrics result;
    if (capture_demand) result.demand = aggregate_demand;
    const auto& grid = graph.grid();
    std::vector<double> heat(static_cast<size_t>(grid.cols) * grid.rows);
    std::vector<int> counts(heat.size(), 0);
    for (int row = 0; row < grid.rows; ++row)
        for (int col = 0; col < grid.cols; ++col)
            heat[static_cast<size_t>(row) * grid.cols + col] =
                std::max(0.0, thermal.get_temperature(col, row) - thermal.ambient_temperature());
    for (const auto* net : nets) {
        if (!net || !net->connected || !std::isfinite(net->wirelength) || net->wirelength < 0 ||
            !std::isfinite(net->reference) || net->reference < 0)
            throw std::runtime_error("invalid cached net guide metrics");
        result.wirelength += net->wirelength;
        result.reference += net->reference;
        for (const int cell : net->tsv_cells) {
            if (cell < 0 || static_cast<size_t>(cell) >= counts.size())
                throw std::runtime_error("cached TSV cell is outside thermal grid");
            if (counts[cell] == std::numeric_limits<int>::max() ||
                result.tsv_count == std::numeric_limits<int>::max())
                throw std::runtime_error("cached TSV count overflows");
            ++counts[cell]; ++result.tsv_count;
        }
    }
    if (!std::isfinite(result.wirelength) || !std::isfinite(result.reference))
        throw std::runtime_error("cached guide length sum overflows");
    for (size_t node = 0; node < aggregate_demand.size(); ++node) {
        const int demand = aggregate_demand[node];
        if (demand < 0) throw std::runtime_error("cached guide demand is negative");
        if (!demand) continue;
        ++result.used_cells;
        const int excess = std::max(0, demand - int(graph.guide_capacity(static_cast<int>(node))));
        if (result.overflow > std::numeric_limits<int>::max() - excess)
            throw std::runtime_error("cached guide overflow count overflows");
        result.overflow += excess;
        result.max_overflow = std::max(result.max_overflow, excess);
    }
    result.normalized = result.reference > 0 ? result.wirelength / result.reference : 100.0;
    if (!result.tsv_count) throw std::runtime_error("no TSVs available for full evaluator metrics");
    if (std::none_of(heat.begin(), heat.end(), [](double h) { return h > 0; }))
        throw std::runtime_error("thermal map contains no temperature rise above ambient");
    result.correlation = PearsonState(std::move(heat), std::move(counts)).correlation();
    result.quality = 0.45 / std::max(1.0, result.normalized) +
                     0.30 * std::max(0.0, 1.0 - result.overflow / 20.0) +
                     0.25 * (result.correlation + 1.0) / 2.0;
    return result;
}
} // namespace eda
