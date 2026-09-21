#include "parser/def_parser.hpp"
#include "parser/text_tokens.hpp"
#include <iostream>
#include <set>
#include <tuple>

namespace eda {
namespace {
Point read_point(TextTokens& t) {
    t.expect("(");
    const double x = t.number(), y = t.number();
    t.expect(")"); return {x, y};
}
Point orient_point(Point p, const std::string& o) {
    if (o == "N") return p;
    if (o == "S") return {-p.x, -p.y};
    if (o == "W") return {-p.y, p.x};
    if (o == "E") return {p.y, -p.x};
    if (o == "FN") return {-p.x, p.y};
    if (o == "FS") return {p.x, -p.y};
    if (o == "FW") return {p.y, p.x};
    if (o == "FE") return {-p.y, -p.x};
    throw std::runtime_error("unsupported DEF orientation: " + o);
}
Point macro_point(Point p, const MacroInfo& m, const std::string& o) {
    // LEF ORIGIN shifts geometry; DEF places the oriented SIZE box's lower left.
    p.x += m.origin.x; p.y += m.origin.y;
    p = orient_point(p, o);
    const auto corner = orient_point({m.width, m.height}, o);
    p.x -= std::min(0.0, corner.x); p.y -= std::min(0.0, corner.y);
    return p;
}
void skip_property(TextTokens& t) {
    while (t.peek() != "+" && t.peek() != ";") {
        if (t.peek() == "END" || t.peek() == "-")
            throw std::runtime_error("unterminated DEF record");
        t.take();
    }
}
std::vector<double> grid_lines(const std::vector<std::tuple<double, int, double>>& statements,
                               double lower, double upper) {
    if (statements.empty()) throw std::runtime_error("both GCELLGRID axes are required");
    std::set<double> lines{lower, upper};
    for (const auto& [start, count, step] : statements) {
        if (count < 2 || step <= 0) throw std::runtime_error("invalid GCELLGRID count/step");
        const double first = std::max(0.0, std::ceil((lower - start) / step));
        const double last = std::min(static_cast<double>(count - 1), std::floor((upper - start) / step));
        if (last - first > 1000000) throw std::runtime_error("GCELLGRID axis exceeds supported size");
        for (double i = first; i <= last; ++i) {
            const double value = start + i * step;
            if (value >= lower && value <= upper) lines.insert(value);
        }
    }
    return {lines.begin(), lines.end()};
}
} // namespace

bool DefParser::parse(const std::string& def_file_path, const LefParser& lef_parser) {
    *this = DefParser{};
    try {
        TextTokens t(def_file_path);
        std::vector<std::tuple<double, int, double>> xs, ys;
        bool units = false, die = false, nets_seen = false, design_end = false;
        std::set<std::string> sections, net_names;
        while (!t.done()) {
            const auto key = t.take();
            if (key == "UNITS") {
                if (units) throw std::runtime_error("duplicate UNITS");
                t.expect("DISTANCE"); t.expect("MICRONS");
                dbu_per_micron_ = t.number(); t.expect(";");
                if (dbu_per_micron_ <= 0) throw std::runtime_error("invalid DEF units");
                units = true;
            } else if (key == "DIEAREA") {
                if (die) throw std::runtime_error("duplicate DIEAREA");
                const auto a = read_point(t), b = read_point(t); t.expect(";");
                if (b.x <= a.x || b.y <= a.y) throw std::runtime_error("invalid rectangular DIEAREA");
                die_area_ = Rect(a.x, a.y, b.x, b.y); die = true;
            } else if (key == "GCELLGRID") {
                const auto axis = t.take();
                const double start = t.number(); t.expect("DO");
                const int count = t.integer(); t.expect("STEP");
                const double step = t.number(); t.expect(";");
                if (axis != "X" && axis != "Y") throw std::runtime_error("invalid GCELLGRID axis");
                (axis == "X" ? xs : ys).emplace_back(start, count, step);
            } else if (key == "COMPONENTS" || key == "PINS" || key == "NETS") {
                if (!units || !die) throw std::runtime_error("UNITS/DIEAREA must precede design objects");
                if (!sections.insert(key).second) throw std::runtime_error("duplicate section " + key);
                const int declared = t.integer(); t.expect(";");
                int records = 0;
                while (t.peek() != "END") {
                    t.expect("-");
                    const auto name = t.take();
                    if (key == "COMPONENTS") {
                        Component c{name, t.take(), {}, {}};
                        bool placed = false;
                        while (t.peek() != ";") {
                            t.expect("+");
                            const auto property = t.take();
                            if (property == "PLACED" || property == "FIXED" || property == "COVER") {
                                if (placed) throw std::runtime_error("duplicate component placement");
                                c.origin = read_point(t); c.orient = t.take();
                                orient_point({}, c.orient); placed = true;
                            } else if (property == "UNPLACED")
                                throw std::runtime_error("unplaced component " + name);
                            else skip_property(t);
                        }
                        if (!placed || !lef_parser.get_macro(c.macro_type) || !components_.emplace(name, c).second)
                            throw std::runtime_error("unplaced/duplicate component or missing macro: " + name);
                    } else if (key == "PINS") {
                        Pin pin; pin.name = name;
                        Point placement, offset;
                        std::string orient;
                        bool placed = false, layer_seen = false, port_seen = false;
                        while (t.peek() != ";") {
                            t.expect("+");
                            const auto property = t.take();
                            if (property == "PORT") {
                                if (port_seen) throw std::runtime_error("multiple IO PORTs unsupported: " + name);
                                port_seen = true;
                            } else if (property == "LAYER") {
                                if (layer_seen) throw std::runtime_error("multiple IO shapes unsupported: " + name);
                                pin.layer = t.take();
                                const auto a = read_point(t), b = read_point(t);
                                if (b.x <= a.x || b.y <= a.y) throw std::runtime_error("invalid IO rectangle " + name);
                                offset = Rect(a.x, a.y, b.x, b.y).center(); layer_seen = true;
                            } else if (property == "PLACED" || property == "FIXED" || property == "COVER") {
                                if (placed) throw std::runtime_error("duplicate IO placement " + name);
                                placement = read_point(t); orient = t.take(); placed = true;
                            } else if (property == "POLYGON" || property == "VIA" || property == "UNPLACED") {
                                throw std::runtime_error("unsupported IO geometry/placement: " + name);
                            } else skip_property(t);
                        }
                        if (!placed || !layer_seen) throw std::runtime_error("missing IO placement/layer: " + name);
                        offset = orient_point(offset, orient);
                        pin.coord = {placement.x + offset.x, placement.y + offset.y};
                        pin.layer_idx = get_layer_index(pin.layer);
                        if (!pin.layer_idx || lef_parser.get_layer_direction(pin.layer) == Direction::UNKNOWN ||
                            !io_pins_.emplace(name, pin).second)
                            throw std::runtime_error("duplicate IO pin or unknown routing layer: " + name);
                    } else {
                        Net net; net.name = name;
                        while (t.peek() == "(") {
                            t.expect("(");
                            const auto instance = t.take(), pin_name = t.take(); t.expect(")");
                            Pin pin;
                            if (instance == "PIN") {
                                const auto found = io_pins_.find(pin_name);
                                if (found == io_pins_.end()) throw std::runtime_error("missing IO pin: " + pin_name);
                                pin = found->second;
                            } else {
                                const auto found = components_.find(instance);
                                if (found == components_.end()) throw std::runtime_error("missing component: " + instance);
                                const auto& c = found->second;
                                const auto* macro = lef_parser.get_macro(c.macro_type);
                                const auto mp = macro->pins.find(pin_name);
                                if (mp == macro->pins.end() || !mp->second.has_rect)
                                    throw std::runtime_error("missing macro pin: " + instance + "." + pin_name);
                                pin.name = instance + "." + pin_name;
                                pin.layer = mp->second.layer; pin.layer_idx = get_layer_index(pin.layer);
                                const auto offset = macro_point(mp->second.bbox.center(), *macro, c.orient);
                                pin.coord = {c.origin.x + offset.x * dbu_per_micron_,
                                             c.origin.y + offset.y * dbu_per_micron_};
                            }
                            net.pins.push_back(pin);
                        }
                        while (t.peek() != ";") {
                            t.expect("+");
                            const auto property = t.take();
                            if (property == "ROUTED" || property == "FIXED" || property == "COVER" ||
                                property == "SUBNET" || property == "VPIN")
                                throw std::runtime_error("pre-routed/subnet geometry unsupported: " + name);
                            skip_property(t);
                        }
                        if (net.pins.empty() || !net_names.insert(name).second)
                            throw std::runtime_error("empty/duplicate net: " + name);
                        nets_.push_back(std::move(net));
                    }
                    t.expect(";"); ++records;
                }
                t.expect("END"); t.expect(key);
                if (records != declared) throw std::runtime_error("record count mismatch in " + key);
                if (key == "NETS") nets_seen = true;
            } else if (key == "VIAS" || key == "SPECIALNETS" || key == "REGIONS" || key == "GROUPS") {
                t.skip_block(key);
            } else if (key == "BLOCKAGES") {
                const int count = t.integer(); t.expect(";");
                if (count) throw std::runtime_error("DEF BLOCKAGES are not supported by the baseline");
                t.expect("END"); t.expect(key);
            } else if (key == "PROPERTYDEFINITIONS") t.skip_block(key);
            else if (key == "END") {
                t.expect("DESIGN"); design_end = true;
                if (!t.done()) throw std::runtime_error("content after END DESIGN");
            } else t.skip_statement();
        }
        if (!units || !die || !nets_seen || !design_end || nets_.empty())
            throw std::runtime_error("missing UNITS, DIEAREA, NETS or END DESIGN");
        grid_.x_lines = grid_lines(xs, die_area_.x1, die_area_.x2);
        grid_.y_lines = grid_lines(ys, die_area_.y1, die_area_.y2);
        grid_.cols = static_cast<int>(grid_.x_lines.size()) - 1;
        grid_.rows = static_cast<int>(grid_.y_lines.size()) - 1;
        if (!grid_.valid()) throw std::runtime_error("invalid GCell grid");
        for (const auto* lines : {&grid_.x_lines, &grid_.y_lines}) {
            for (double value : *lines) {
                if (std::floor(value) != value || std::abs(value) > 9007199254740991.0)
                    throw std::runtime_error("guide grid coordinates must be exact integer DBU");
            }
        }
        for (auto& net : nets_) {
            for (auto& pin : net.pins) {
                pin.gcell_col = grid_.get_col(pin.coord.x); pin.gcell_row = grid_.get_row(pin.coord.y);
                if (!pin.layer_idx || lef_parser.get_layer_direction(pin.layer) == Direction::UNKNOWN ||
                    pin.gcell_col < 0 || pin.gcell_row < 0)
                    throw std::runtime_error("invalid pin layer or out-of-die coordinate: " + pin.name);
            }
            net.is_cross_die = net.has_bottom_pin() && net.has_top_pin();
            if ((net.name.starts_with("BOTTOM__") && net.has_top_pin()) ||
                (net.name.starts_with("TOP__") && net.has_bottom_pin()))
                throw std::runtime_error("net prefix conflicts with pin die: " + net.name);
        }
        std::cout << "[INFO] DEF parsed: " << components_.size() << " components, "
                  << io_pins_.size() << " IO pins, " << nets_.size() << " nets, "
                  << grid_.cols << "x" << grid_.rows << " GCells\n";
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << def_file_path << ": " << error.what() << "\n";
        *this = DefParser{};
        return false;
    }
}
} // namespace eda