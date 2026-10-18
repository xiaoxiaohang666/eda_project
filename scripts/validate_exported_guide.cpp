#include "router/routing_graph.hpp"
#include "router/tsv_policy.hpp"
#include "validation/guide_validator.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

// Independent export boundary: reload original inputs and the serialized
// Guide. No router NetRoute, resource ledger, cached validation or score is used.
// Shared parsers, physical OBS checks and TSV policy remain engineering
// contracts; this executable is not an independent contest DRC oracle.
using namespace eda;
namespace fs = std::filesystem;
namespace {
struct Options {
    std::vector<std::string> lefs;
    std::string def, thermal, guide, report;
    bool official_model{false}, strict_obs{true}, strict_obs_explicit{false};
    TsvPolicyOptions tsv;
};
std::string quoted(const std::string& value) {
    std::ostringstream out; out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << c;
    }
    out << '"'; return out.str();
}
Options options(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help") {
            std::cout << "eda_validate_guide --lef FILE (repeatable) --def FILE --thermal FILE --guide FILE\n"
                      << "  [--model physical|official] [--tsv-resources auto|paired|projected]\n"
                      << "  [--strict-obs 0|1] [--report JSON]\n"
                      << "Physical defaults to strict OBS. Official uses full DEF grids and OBS/PIN capacity demand;\n"
                      << "  explicit --strict-obs 1 conflicts with --model official. Pin/TSV checks remain mandatory.\n"
                      << "Checks oriented terminals, Guide connectivity, DIE/OBS and engineering TSV mapping.\n"
                      << "Does not run the official evaluator or certify detailed-route DRC.\n";
            std::exit(0);
        }
        if (i + 1 == argc) throw std::runtime_error("missing value for " + key);
        const std::string value = argv[++i];
        if (key == "--lef") result.lefs.push_back(value);
        else if (key == "--def") result.def = value;
        else if (key == "--thermal") result.thermal = value;
        else if (key == "--guide") result.guide = value;
        else if (key == "--report") result.report = value;
        else if (key == "--model") {
            if (value != "physical" && value != "official") throw std::runtime_error("model must be physical or official");
            result.official_model = value == "official";
        }
        else if (key == "--strict-obs") {
            if (value != "0" && value != "1") throw std::runtime_error("strict-obs must be 0 or 1");
            result.strict_obs = value == "1"; result.strict_obs_explicit = true;
        }
        else if (key == "--tsv-resources") {
            if (value == "auto") result.tsv.resource_mode = TsvResourceMode::AUTO;
            else if (value == "paired") result.tsv.resource_mode = TsvResourceMode::PAIRED;
            else if (value == "projected") result.tsv.resource_mode = TsvResourceMode::PROJECTED;
            else throw std::runtime_error("unknown TSV resource mode: " + value);
        } else throw std::runtime_error("unknown option: " + key);
    }
    if (result.official_model) {
        if (result.strict_obs_explicit && result.strict_obs)
            throw std::runtime_error("--strict-obs 1 conflicts with --model official: OBS/PIN are capacity demand in the global-Guide model");
        result.strict_obs = false;
    }
    if (result.lefs.empty() || result.def.empty() || result.thermal.empty() || result.guide.empty())
        throw std::runtime_error("required: --lef --def --thermal --guide");
    if (!result.report.empty()) {
        std::vector<std::string> inputs = result.lefs;
        inputs.insert(inputs.end(), {result.def, result.thermal, result.guide});
        for (const auto& input : inputs)
            if (fs::weakly_canonical(input) == fs::weakly_canonical(result.report) ||
                (fs::exists(input) && fs::exists(result.report) && fs::equivalent(input, result.report)))
                throw std::runtime_error("report cannot overwrite input: " + input);
    }
    return result;
}
GuideMap read_guide(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open guide: " + path);
    GuideMap result; std::string name, token;
    while (in >> name) {
        if (result.contains(name)) throw std::runtime_error("duplicate guide net: " + name);
        if (!(in >> token) || token != "(") throw std::runtime_error("missing '(' for " + name);
        auto& rectangles = result[name]; bool ended = false;
        while (in >> token) {
            if (token == ")") { ended = true; break; }
            GuideRect rect; size_t consumed = 0;
            rect.x1 = std::stod(token, &consumed);
            if (consumed != token.size() || !(in >> rect.y1 >> rect.x2 >> rect.y2 >> rect.layer))
                throw std::runtime_error("malformed rectangle for " + name);
            // Preserve raw coordinates: a reversed export must not be normalized.
            rectangles.push_back(rect);
        }
        if (!ended) throw std::runtime_error("unterminated guide net: " + name);
        if (rectangles.empty()) throw std::runtime_error("empty guide net: " + name);
    }
    if (!in.eof()) throw std::runtime_error("guide read error: " + path);
    if (result.empty()) throw std::runtime_error("empty guide file");
    return result;
}
bool contains(const GuideRect& rect, Point point) {
    return point.x >= rect.x1 && point.x <= rect.x2 && point.y >= rect.y1 && point.y <= rect.y2;
}
bool positive_overlap(const GuideRect& a, const Rect& b) {
    return std::min(a.x2, b.x2) > std::max(a.x1, b.x1) &&
           std::min(a.y2, b.y2) > std::max(a.y1, b.y1);
}
struct Dsu {
    std::vector<size_t> parent, rank;
    explicit Dsu(size_t n) : parent(n), rank(n) { std::iota(parent.begin(), parent.end(), 0); }
    size_t find(size_t x) { while (x != parent[x]) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; }
    void join(size_t a, size_t b) {
        a = find(a); b = find(b); if (a == b) return;
        if (rank[a] < rank[b]) std::swap(a, b);
        parent[b] = a; if (rank[a] == rank[b]) ++rank[a];
    }
};
struct Results {
    size_t checked_nets{}, checked_pins{}, checked_rectangles{}, checked_cross_nets{}, errors{}, checker_uncovered_pins{};
    size_t pin_errors{}, connectivity_errors{}, geometry_errors{}, mapping_errors{}, fallback_layers{}, interpolated_cells{};
    size_t guide_envelope_obs_intersections{}, pin_port_precedence_rectangles{};
    bool official_model{false}, strict_obs{true};
    size_t grid_rows{}, grid_cols{};
    std::string thermal_mode, tsv_mode, capacity_model;
    std::vector<std::string> examples;
    void fail(const std::string& message, size_t& category) {
        ++errors; ++category; if (examples.size() < 50) examples.push_back(message);
    }
};
std::string report(const Results& result) {
    std::ostringstream out;
    out << "{\n\"status\":" << quoted(result.errors ? "FAIL" : "PASS")
        << ",\n\"validation_scope\":" << quoted(result.official_model
            ? "reloaded-export-oriented-pin-connectivity-die-global-guide-engineering-tsv-policy"
            : "reloaded-export-oriented-pin-connectivity-die-obs-engineering-tsv-policy")
        << ",\n\"official_evaluator_status\":\"not_run\",\n\"full_drc_certified\":false"
        << ",\n\"strict_obs_enforced\":" << (result.strict_obs ? "true" : "false")
        << ",\n\"guide_envelope_obs_intersections\":" << result.guide_envelope_obs_intersections
        << ",\n\"pin_port_precedence_rectangles\":" << result.pin_port_precedence_rectangles
        << ",\n\"existing_pin_port_connectivity_enforced\":" << (result.official_model ? "false" : "true")
        << ",\n\"checked\":{\"nets\":" << result.checked_nets << ",\"pins\":" << result.checked_pins
        << ",\"rectangles\":" << result.checked_rectangles << ",\"cross_die_nets\":" << result.checked_cross_nets << '}'
        << ",\n\"errors\":{\"total\":" << result.errors << ",\"pins\":" << result.pin_errors
        << ",\"connectivity\":" << result.connectivity_errors << ",\"die_or_obs\":" << result.geometry_errors
        << ",\"tsv_mapping\":" << result.mapping_errors << '}'
        << ",\n\"input_models\":{\"model\":" << quoted(result.official_model ? "official" : "physical")
        << ",\"grid\":" << quoted(result.official_model ? "official-full" : "clipped-physical")
        << ",\"grid_rows\":" << result.grid_rows << ",\"grid_cols\":" << result.grid_cols
        << ",\"geometry\":" << quoted(result.official_model ? "global-guide" : "physical-strict")
        << ",\"obs_interpretation\":" << quoted(result.official_model ? "capacity-demand" : "physical-geometry")
        << ",\"capacity_model\":" << quoted(result.capacity_model)
        << ",\"thermal_strategy\":" << quoted(result.official_model ? "official-previous" : "legacy-bilinear")
        << ",\"thermal\":" << quoted(result.thermal_mode)
        << ",\"interpolated_cells\":" << result.interpolated_cells
        << ",\"physical_track_proxy_layers\":" << result.fallback_layers
        << ",\"tsv_policy\":" << quoted(result.tsv_mode) << '}'
        << ",\n\"fixed_checker_coordinate_comparison\":{\"uncovered_checker_points\":" << result.checker_uncovered_pins
        << ",\"affects_actual_geometry_status\":false}"
        << ",\n\"limitations\":[\"Shared input parsers and engineering TSV policy are not an independent contest legality oracle.\","
        << quoted(result.official_model
            ? "Official global Guides may cover OBS; fixed OBS/PIN capacity demand is represented by the shared graph. This tool does not calculate congestion or certify physical wire legality."
            : "Exact own-instance PIN PORT precedence over OBS is checked. CORE-cell narrow terminal guides remain engineering abstractions; minimum-width and pin-access DRC are not certified.") << ","
        << "\"A legacy GCell Guide is a routing search envelope and may contain OBS although its retained center-wire corridor is legal; this tool cannot reconstruct or certify that private corridor ledger.\","
        << "\"No spacing, cut enclosure, detailed track assignment, inter-net short, or official quality score is certified.\"]"
        << ",\n\"error_examples\":[";
    for (size_t i = 0; i < result.examples.size(); ++i) { if (i) out << ','; out << quoted(result.examples[i]); }
    out << "]\n}\n"; return out.str();
}
}
int main(int argc, char** argv) {
    try {
        const auto config = options(argc, argv);
        LefParser lef; for (const auto& path : config.lefs) if (!lef.parse(path)) return 2;
        DefParser def; if (!def.parse(config.def, lef, config.official_model
            ? DefGridMapping::OFFICIAL_FULL : DefGridMapping::CLIPPED_PHYSICAL)) return 2;
        ThermalParser thermal;
        if (!thermal.parse_json(config.thermal, def.gcell_grid(), def.dbu_per_micron(), def.die_area(),
            config.official_model ? ThermalMapping::OFFICIAL_PREVIOUS : ThermalMapping::LEGACY_BILINEAR)) return 2;
        const auto guides = read_guide(config.guide);
        RoutingGraphOptions graph_options;
        if (config.official_model) graph_options.geometry = RoutingGeometryMode::GLOBAL_GUIDE;
        RoutingGraph graph(def, lef, graph_options); TsvPolicy policy(def, lef, graph, config.tsv);
        Results result; result.thermal_mode = thermal.mapping_mode(); result.tsv_mode = policy.pairing_mode();
        result.official_model = config.official_model; result.strict_obs = config.strict_obs;
        result.grid_rows = graph.grid().rows; result.grid_cols = graph.grid().cols;
        result.capacity_model = graph.capacity_model();
        result.interpolated_cells = thermal.interpolated_cells();
        for (int layer = 1; layer <= 20; ++layer) result.fallback_layers += graph.guide_uses_physical_tracks(layer);
        std::unordered_map<int, std::vector<int>> slot_cells;
        for (const auto& slot : policy.slots()) {
            int c = graph.grid().get_col(slot.point.x), r = graph.grid().get_row(slot.point.y);
            if (c >= 0 && r >= 0) slot_cells[r * graph.grid().cols + c].push_back(slot.id);
        }
        std::set<std::string> expected; std::unordered_map<int, std::string> occupied;
        for (const auto& net : def.nets()) {
            expected.insert(net.name); ++result.checked_nets;
            auto found = guides.find(net.name);
            if (found == guides.end()) { result.fail(net.name + ": missing exported net", result.connectivity_errors); continue; }
            const auto& rects = found->second;
            std::array<std::vector<size_t>, 21> by_layer; bool valid = true;
            for (size_t i = 0; i < rects.size(); ++i) {
                const auto& rect = rects[i]; ++result.checked_rectangles; const int layer = get_layer_index(rect.layer);
                bool coordinate = true;
                for (double value : {rect.x1, rect.y1, rect.x2, rect.y2})
                    coordinate = coordinate && std::isfinite(value) && std::floor(value) == value && std::abs(value) <= 9007199254740991.0;
                const auto& die = def.die_area();
                if (!coordinate || !layer || lef.get_layer_direction(rect.layer) == Direction::UNKNOWN ||
                    rect.x1 >= rect.x2 || rect.y1 >= rect.y2 || rect.x1 < die.x1 || rect.y1 < die.y1 ||
                    rect.x2 > die.x2 || rect.y2 > die.y2) {
                    valid = false; result.fail(net.name + ": rectangle " + std::to_string(i) + " violates DIE/export geometry", result.geometry_errors);
                    continue;
                }
                const Rect physical{rect.x1, rect.y1, rect.x2, rect.y2};
                if (!graph.legal_rect(physical, layer)) {
                    ++result.guide_envelope_obs_intersections;
                    // LEF PIN geometry takes precedence over OBS belonging to
                    // its own instance. The graph checks the exact intersection
                    // against the actual oriented PORT union; other-owner OBS
                    // and geometry outside that PORT stay forbidden.
                    bool pin_precedence = false;
                    for (const auto& pin : net.pins)
                        if (graph.legal_pin_rect(physical, layer, pin)) { pin_precedence = true; break; }
                    if (pin_precedence) ++result.pin_port_precedence_rectangles;
                    else if (config.strict_obs) result.fail(net.name + ": Guide envelope rectangle " + std::to_string(i) +
                        " intersects OBS outside its own actual PIN PORT; private route corridors are not inferred by this check", result.geometry_errors);
                }
                if (!net.is_cross_die && ((net.has_bottom_pin() && layer > 10) || (net.has_top_pin() && layer <= 10)))
                    result.fail(net.name + ": intra-die net enters the other die", result.mapping_errors);
                by_layer[layer].push_back(i);
            }
            if (!valid) continue;
            for (const auto& pin : net.pins) {
                ++result.checked_pins; bool center = false, port = false, checker = !pin.has_evaluator_coord;
                for (const auto& rect : rects) {
                    int layer = get_layer_index(rect.layer);
                    if (layer == pin.layer_idx && contains(rect, pin.coord)) center = true;
                    if (layer == pin.layer_idx && pin.has_evaluator_coord && contains(rect, pin.evaluator_coord)) checker = true;
                    for (const auto& shape : pin.port_rects)
                        if (shape.layer_idx == layer && positive_overlap(rect, shape.rect)) port = true;
                }
                if (!center || (!pin.port_rects.empty() && !port))
                    result.fail(net.name + ": actual oriented Pin is uncovered: " + pin.name, result.pin_errors);
                if (!checker) ++result.checker_uncovered_pins;
            }
            Dsu connected(rects.size());
            for (auto& indices : by_layer) std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
                return std::tie(rects[a].x1, a) < std::tie(rects[b].x1, b);
            });
            for (int layer = 1; layer <= 20; ++layer) for (size_t a : by_layer[layer])
                for (int next = layer; next <= std::min(20, layer + 1); ++next)
                    for (size_t b : by_layer[next]) {
                        if (rects[b].x1 > rects[a].x2) break;
                        if (next == layer && b <= a) continue;
                        const double dx = std::min(rects[a].x2, rects[b].x2) - std::max(rects[a].x1, rects[b].x1);
                        const double dy = std::min(rects[a].y2, rects[b].y2) - std::max(rects[a].y1, rects[b].y1);
                        // Integer DBU make the official 1e-6 same-layer
                        // tolerance equivalent to >0; vias need positive area.
                        const bool contact = layer == next
                            ? dx >= 0 && dy >= 0 && (dx > 0 || dy > 0)
                            : dx > 0 && dy > 0;
                        if (contact) connected.join(a, b);
                    }
            // Physical geometry can connect through existing terminal metal.
            // The official checker builds adjacency from Guide rectangles
            // alone and requires every ordinary layer transition explicitly.
            if (!config.official_model) for (const auto& pin : net.pins) {
                size_t first = rects.size();
                for (size_t i = 0; i < rects.size(); ++i) {
                    const int layer = get_layer_index(rects[i].layer);
                    bool contacts_port = false;
                    for (const auto& shape : pin.port_rects)
                        if (shape.layer_idx == layer && positive_overlap(rects[i], shape.rect)) {
                            contacts_port = true; break;
                        }
                    if (!contacts_port) continue;
                    if (first == rects.size()) first = i;
                    else connected.join(first, i);
                }
            }
            bool all_connected = true;
            for (size_t i = 1; i < rects.size(); ++i) all_connected = all_connected && connected.find(i) == connected.find(0);
            if (!all_connected) result.fail(net.name + ": exported rectangles/terminals are disconnected", result.connectivity_errors);
            if (!net.is_cross_die) {
                std::string error;
                if (!policy.validate_mapping(net, rects, -1, error)) result.fail(error, result.mapping_errors);
                continue;
            }
            ++result.checked_cross_nets;
            std::set<std::tuple<double, double, double, double>> positive_crosses;
            for (size_t a : by_layer[10]) for (size_t b : by_layer[11]) {
                double x1 = std::max(rects[a].x1, rects[b].x1), y1 = std::max(rects[a].y1, rects[b].y1);
                double x2 = std::min(rects[a].x2, rects[b].x2), y2 = std::min(rects[a].y2, rects[b].y2);
                if (x1 < x2 && y1 < y2) positive_crosses.emplace(x1, y1, x2, y2);
            }
            if (positive_crosses.size() != 1) {
                result.fail(net.name + ": expected one positive-area TSV overlap, found " + std::to_string(positive_crosses.size()), result.mapping_errors); continue;
            }
            const auto [x1, y1, x2, y2] = *positive_crosses.begin(); const Point p{(x1 + x2) * 0.5, (y1 + y2) * 0.5};
            int cell = graph.grid().get_row(p.y) * graph.grid().cols + graph.grid().get_col(p.x);
            std::vector<int> matches;
            // Outward-rounded integer landing rectangles can shift the inferred
            // center by half a DBU. Match slots in its thermal GCell, then let
            // validate_mapping enforce the actual VIA envelope and reservation;
            // do not require the inferred center to equal the physical slot point.
            if (auto candidates = slot_cells.find(cell); candidates != slot_cells.end())
                for (int slot : candidates->second) { std::string error; if (policy.validate_mapping(net, rects, slot, error)) matches.push_back(slot); }
            if (matches.size() != 1) {
                result.fail(net.name + ": TSV reservation mapping has " + std::to_string(matches.size()) + " candidates; unique mapping cannot be verified", result.mapping_errors); continue;
            }
            const int slot = matches.front();
            if (occupied.contains(slot)) result.fail(net.name + ": TSV reservation reused by " + occupied.at(slot), result.mapping_errors);
            for (int conflict : policy.slots()[slot].conflicts)
                if (occupied.contains(conflict)) result.fail(net.name + ": geometric TSV conflict with " + occupied.at(conflict), result.mapping_errors);
            occupied[slot] = net.name;
        }
        for (const auto& [name, rects] : guides) { (void)rects; if (!expected.contains(name)) result.fail(name + ": unexpected exported net", result.connectivity_errors); }
        const std::string json = report(result);
        if (!config.report.empty()) {
            const auto parent = fs::path(config.report).parent_path(); if (!parent.empty()) fs::create_directories(parent);
            std::ofstream out(config.report); out << json; out.close(); if (!out) throw std::runtime_error("cannot write report");
        }
        std::cout << json; return result.errors ? 1 : 0;
    } catch (const std::exception& error) { std::cerr << "[EXPORT_VALIDATOR_ERROR] " << error.what() << '\n'; return 2; }
}
