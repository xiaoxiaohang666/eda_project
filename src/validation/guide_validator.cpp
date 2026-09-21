#include "validation/guide_validator.hpp"
#include <queue>

namespace eda {
bool validate_guides(const DefParser& def, const LefParser& lef,
                     const GuideMap& guides, std::string& error) {
    error.clear();
    auto fail = [&](const std::string& message) { error = message; return false; };
    if (!def.gcell_grid().valid() || def.nets().empty()) return fail("invalid/empty design");
    if (guides.size() != def.nets().size()) return fail("guide/DEF Net set mismatch");
    const auto& die = def.die_area();
    for (const auto& net : def.nets()) {
        const auto found = guides.find(net.name);
        if (found == guides.end() || found->second.empty()) return fail("missing guide for " + net.name);
        const auto& rects = found->second;
        std::vector<int> layers;
        for (const auto& r : rects) {
            const int layer = get_layer_index(r.layer);
            if (!layer || lef.get_layer_direction(r.layer) == Direction::UNKNOWN)
                return fail(net.name + ": unknown routing layer");
            for (double v : {r.x1, r.y1, r.x2, r.y2}) {
                if (!std::isfinite(v) || std::floor(v) != v || std::abs(v) > 9007199254740991.0)
                    return fail(net.name + ": non-integer/non-finite guide coordinate");
            }
            if (r.x1 >= r.x2 || r.y1 >= r.y2 || r.x1 < die.x1 || r.y1 < die.y1 ||
                r.x2 > die.x2 || r.y2 > die.y2) return fail(net.name + ": invalid/out-of-die rectangle");
            if (!net.is_cross_die && ((net.has_bottom_pin() && layer > 10) ||
                                     (net.has_top_pin() && layer <= 10)))
                return fail(net.name + ": intra-die net uses the other die");
            layers.push_back(layer);
        }
        for (const auto& pin : net.pins) {
            bool covered = false;
            for (const auto& r : rects) {
                if (r.layer == pin.layer && pin.coord.x >= r.x1 && pin.coord.x <= r.x2 &&
                    pin.coord.y >= r.y1 && pin.coord.y <= r.y2) { covered = true; break; }
            }
            if (!covered) return fail(net.name + ": uncovered pin " + pin.name);
        }
        std::vector<bool> visited(rects.size(), false);
        std::queue<size_t> todo;
        visited[0] = true; todo.push(0);
        bool tsv = false;
        size_t count = 0;
        while (!todo.empty()) {
            const auto i = todo.front(); todo.pop(); ++count;
            for (size_t j = 0; j < rects.size(); ++j) {
                if (i == j || std::abs(layers[i] - layers[j]) > 1) continue;
                const auto& a = rects[i]; const auto& b = rects[j];
                const double dx = std::min(a.x2, b.x2) - std::max(a.x1, b.x1);
                const double dy = std::min(a.y2, b.y2) - std::max(a.y1, b.y1);
                const bool cross = (layers[i] <= 10) != (layers[j] <= 10);
                if (dx < 0 || dy < 0 || (cross && (dx <= 0 || dy <= 0))) continue;
                if (cross) tsv = true;
                if (!visited[j]) { visited[j] = true; todo.push(j); }
            }
        }
        if (count != rects.size()) return fail(net.name + ": disconnected rectangles");
        if (net.is_cross_die && !tsv) return fail(net.name + ": no positive-area M10/M11 connection");
    }
    return true;
}
} // namespace eda
