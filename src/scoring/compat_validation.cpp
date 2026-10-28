#include "scoring/compat_validation.hpp"

#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace eda::compat {
namespace {

std::string canonical_layer(std::string name) {
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const int layer = get_layer_index(name);
    return layer ? get_layer_name(layer) : name;
}

void fail(ValidationResult& result, const std::string& message) {
    ++result.error_count;
    if (result.errors.size() < 50) result.errors.push_back(message);
}

bool positive_overlap(const GuideRect& a, const Rect& b) {
    return std::min(a.x2, b.x2) > std::max(a.x1, b.x1) &&
           std::min(a.y2, b.y2) > std::max(a.y1, b.y1);
}

struct Dsu {
    std::vector<std::size_t> parent, rank;
    explicit Dsu(std::size_t size) : parent(size), rank(size) {
        std::iota(parent.begin(), parent.end(), 0);
    }
    std::size_t find(std::size_t index) {
        while (index != parent[index]) {
            parent[index] = parent[parent[index]];
            index = parent[index];
        }
        return index;
    }
    void join(std::size_t a, std::size_t b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (rank[a] < rank[b]) std::swap(a, b);
        parent[b] = a;
        if (rank[a] == rank[b]) ++rank[a];
    }
};

// Sweep by X within one layer or a pair of adjacent layers. Rectangles whose
// X intervals no longer overlap are discarded before pair comparisons.
template <typename Join>
void visit_overlapping_x(const std::vector<GuideRect>& rectangles,
                         const std::vector<std::size_t>& first,
                         const std::vector<std::size_t>* second, Join join) {
    if (first.empty() || (second && second->empty()) || (!second && first.size() < 2)) return;
    struct Event { std::size_t index; int side; };
    std::vector<Event> events;
    events.reserve(first.size() + (second ? second->size() : 0));
    for (auto index : first) events.push_back({index, 0});
    if (second) for (auto index : *second) events.push_back({index, 1});
    std::sort(events.begin(), events.end(), [&](const Event& a, const Event& b) {
        return std::tie(rectangles[a.index].x1, a.index) <
               std::tie(rectangles[b.index].x1, b.index);
    });
    std::array<std::multimap<double, std::size_t>, 2> active;
    for (const auto& event : events) {
        const auto& rectangle = rectangles[event.index];
        for (auto& set : active)
            while (!set.empty() && set.begin()->first < rectangle.x1) set.erase(set.begin());
        const auto& candidates = active[second ? 1 - event.side : 0];
        for (const auto& [end_x, index] : candidates) {
            (void)end_x;
            join(index, event.index);
        }
        active[event.side].emplace(rectangle.x2, event.index);
    }
}

double coordinate(const std::string& token, const std::string& name) {
    try {
        std::size_t consumed = 0;
        const double value = std::stod(token, &consumed);
        if (consumed == token.size()) return value;
    } catch (const std::exception&) {
        // Include the offending token and net without leaking stdlib-specific
        // exception messages into the external report.
    }
    throw std::runtime_error(name + ": malformed coordinate '" + token + "'");
}

} // namespace

GuideMap read_guides(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open guide: " + path);
    GuideMap result;
    std::string name, token;
    while (input >> name) {
        if (name == "(" || name == ")") throw std::runtime_error("missing guide net name");
        if (result.contains(name)) throw std::runtime_error("duplicate guide net: " + name);
        if (!(input >> token) || token != "(")
            throw std::runtime_error(name + ": missing '('");
        auto& rectangles = result[name];
        bool ended = false;
        while (input >> token) {
            if (token == ")") { ended = true; break; }
            std::array<std::string, 4> values;
            values[0] = token;
            if (!(input >> values[1] >> values[2] >> values[3] >> token))
                throw std::runtime_error(name + ": incomplete rectangle");
            GuideRect rectangle;
            rectangle.x1 = coordinate(values[0], name);
            rectangle.y1 = coordinate(values[1], name);
            rectangle.x2 = coordinate(values[2], name);
            rectangle.y2 = coordinate(values[3], name);
            if (token == "(" || token == ")")
                throw std::runtime_error(name + ": missing rectangle layer");
            rectangle.layer = canonical_layer(token);
            rectangles.push_back(std::move(rectangle));
        }
        if (!ended) throw std::runtime_error(name + ": unterminated guide net");
        if (rectangles.empty()) throw std::runtime_error(name + ": empty guide net");
    }
    if (!input.eof()) throw std::runtime_error("guide read error: " + path);
    if (result.empty()) throw std::runtime_error("empty guide file: " + path);
    return result;
}

namespace {
void validate_net_into(const Net& net, const Rect& die, const LefParser& lef,
                       const std::vector<GuideRect>& rectangles,
                       double gcell_pitch_x, double gcell_pitch_y,
                       bool legacy_direction, ValidationResult& result) {
    if (rectangles.empty()) {
        fail(result, net.name + ": missing or empty guide");
        return;
    }
    std::array<std::vector<std::size_t>, 21> by_layer;
    std::array<Direction, 21> directions;
    directions.fill(Direction::UNKNOWN);
    bool geometry_valid = true;
    const bool bottom_only = net.has_bottom_pin() && !net.has_top_pin();
    const bool top_only = net.has_top_pin() && !net.has_bottom_pin();
    for (std::size_t index = 0; index < rectangles.size(); ++index) {
        const auto& rectangle = rectangles[index];
        const auto prefix = [&] { return net.name + ": rectangle " + std::to_string(index); };
        const auto name = canonical_layer(rectangle.layer);
        const int layer = get_layer_index(name);
        const auto* info = lef.get_layer(name);
        if (!layer || !info || info->type != "ROUTING" ||
            info->direction == Direction::UNKNOWN) {
            fail(result, prefix() + " uses unknown/non-routing layer " + rectangle.layer);
            geometry_valid = false;
            continue;
        }
        bool finite = true;
        for (double value : {rectangle.x1, rectangle.y1, rectangle.x2, rectangle.y2})
            finite = finite && std::isfinite(value);
        if (!finite || rectangle.x1 >= rectangle.x2 || rectangle.y1 >= rectangle.y2) {
            fail(result, prefix() + " has non-finite coordinates or non-positive area");
            geometry_valid = false;
            continue;
        }
        // The described check accepts a partly out-of-DIE envelope.
        if (!positive_overlap(rectangle, die)) {
            fail(result, prefix() + " has no positive-area overlap with DIEAREA");
            geometry_valid = false;
            continue;
        }
        if ((bottom_only && layer > 10) || (top_only && layer <= 10))
            fail(result, prefix() + " enters the other die for a single-die net");
        if (!legacy_direction) {
            // First GCELLGRID STEP is the documented compatibility
            // assumption for clipped boundary/nonuniform GCells.
            const bool too_thick = info->direction == Direction::HORIZONTAL
                ? rectangle.y2 - rectangle.y1 > gcell_pitch_y
                : rectangle.x2 - rectangle.x1 > gcell_pitch_x;
            if (too_thick)
                fail(result, prefix() + " exceeds one GCell perpendicular to " +
                     direction_to_string(info->direction));
        }
        by_layer[layer].push_back(index);
        directions[layer] = info->direction;
    }
    for (const auto& pin : net.pins) {
        bool covered = false;
        for (const auto& shape : pin.port_rects) {
            if (shape.layer_idx < 1 || shape.layer_idx > 20) continue;
            for (auto index : by_layer[shape.layer_idx])
                if (positive_overlap(rectangles[index], shape.rect)) {
                    covered = true;
                    break;
                }
            if (covered) break;
        }
        if (!covered)
            fail(result, net.name + ": pin " + pin.name +
                 " has no positive-area same-layer PORT overlap");
    }
    // Invalid rectangles cannot be inserted into geometric connectivity.
    // Their recorded errors already make the entire submission fail.
    if (!geometry_valid) return;
    Dsu connected(rectangles.size());
    bool cross_connection = false;
    for (int layer = 1; layer <= 20; ++layer) {
        visit_overlapping_x(rectangles, by_layer[layer], nullptr,
            [&](std::size_t a, std::size_t b) {
                const auto& left = rectangles[a];
                const auto& right = rectangles[b];
                const double dx = std::min(left.x2, right.x2) - std::max(left.x1, right.x1);
                const double dy = std::min(left.y2, right.y2) - std::max(left.y1, right.y1);
                const bool contact = legacy_direction
                    ? dx >= 0 && dy >= 0 && (dx > 0 || dy > 0)
                    : directions[layer] == Direction::HORIZONTAL
                        ? dx >= 0 && dy > 0 : dy >= 0 && dx > 0;
                if (contact) connected.join(a, b);
            });
        if (layer == 20) continue;
        visit_overlapping_x(rectangles, by_layer[layer], &by_layer[layer + 1],
            [&](std::size_t a, std::size_t b) {
                const auto& left = rectangles[a];
                const auto& right = rectangles[b];
                if (std::min(left.x2, right.x2) <= std::max(left.x1, right.x1) ||
                    std::min(left.y2, right.y2) <= std::max(left.y1, right.y1)) return;
                connected.join(a, b);
                if (layer == 10) cross_connection = true;
            });
    }
    for (std::size_t index = 1; index < rectangles.size(); ++index) {
        if (connected.find(index) != connected.find(0)) {
            fail(result, net.name + ": guide rectangles are disconnected");
            break;
        }
    }
    if (net.has_bottom_pin() && net.has_top_pin() && !cross_connection)
        fail(result, net.name + ": no positive-area metal10/metal11 connection");
}
} // namespace

ValidationResult validate_net(const Net& net, const Rect& die, const LefParser& lef,
                              const std::vector<GuideRect>& guides,
                              double gcell_pitch_x, double gcell_pitch_y,
                              bool legacy_direction) {
    ValidationResult result;
    result.checked_nets = 1;
    result.checked_pins = net.pins.size();
    result.checked_rectangles = guides.size();
    if (!legacy_direction &&
        (!std::isfinite(gcell_pitch_x) || gcell_pitch_x <= 0 ||
         !std::isfinite(gcell_pitch_y) || gcell_pitch_y <= 0)) {
        fail(result, "invalid DEF GCell pitch for preferred-direction check");
        return result;
    }
    validate_net_into(net, die, lef, guides, gcell_pitch_x, gcell_pitch_y,
                      legacy_direction, result);
    return result;
}

ValidationResult validate(const DefParser& def, const LefParser& lef,
                          const GuideMap& guides, bool legacy_direction) {
    ValidationResult result;
    result.checked_nets = def.nets().size();
    for (const auto& net : def.nets()) result.checked_pins += net.pins.size();
    for (const auto& [name, rectangles] : guides) {
        (void)name;
        result.checked_rectangles += rectangles.size();
    }
    if (!def.gcell_grid().valid() || def.nets().empty()) {
        fail(result, "invalid or empty DEF design");
        return result;
    }
    if (!legacy_direction &&
        (!std::isfinite(def.gcell_pitch_x()) || def.gcell_pitch_x() <= 0 ||
         !std::isfinite(def.gcell_pitch_y()) || def.gcell_pitch_y() <= 0)) {
        fail(result, "invalid DEF GCell pitch for preferred-direction check");
        return result;
    }
    std::set<std::string> expected;
    for (const auto& net : def.nets()) expected.insert(net.name);
    for (const auto& [name, rectangles] : guides) {
        (void)rectangles;
        if (!expected.contains(name)) fail(result, name + ": net is not defined in DEF");
    }
    for (const auto& net : def.nets()) {
        const auto found = guides.find(net.name);
        if (found == guides.end()) {
            fail(result, net.name + ": missing or empty guide");
            continue;
        }
        validate_net_into(net, def.die_area(), lef, found->second,
                          def.gcell_pitch_x(), def.gcell_pitch_y(), legacy_direction, result);
    }
    return result;
}

} // namespace eda::compat
