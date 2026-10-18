#include "parser/lef_parser.hpp"
#include "parser/text_tokens.hpp"
#include <iostream>

namespace eda {
namespace {
LayerRect read_rect(TextTokens& t, const std::string& layer) {
    const double x1 = t.number(), y1 = t.number(), x2 = t.number(), y2 = t.number();
    t.expect(";");
    if (layer.empty() || x2 == x1 || y2 == y1)
        throw std::runtime_error("invalid LEF RECT on layer " + layer);
    return {layer, Rect(x1, y1, x2, y2)};
}
std::vector<LayerRect> parse_shapes(TextTokens& t) {
    std::vector<LayerRect> shapes;
    std::string layer;
    while (t.peek() != "END") {
        const auto key = t.take();
        if (key == "LAYER") { layer = t.take(); t.expect(";"); }
        else if (key == "RECT") shapes.push_back(read_rect(t, layer));
        else throw std::runtime_error("unsupported LEF obstruction/via geometry: " + key);
    }
    return shapes;
}
ViaInfo parse_via(TextTokens& t) {
    ViaInfo via; via.name = t.take();
    if (t.peek() == "DEFAULT") { t.take(); via.is_default = true; }
    std::string layer;
    while (t.peek() != "END") {
        const auto key = t.take();
        if (key == "TOPOFSTACKONLY") {
            if (via.top_of_stack_only)
                throw std::runtime_error("duplicate TOPOFSTACKONLY on VIA " + via.name);
            via.top_of_stack_only = true;
        }
        else if (key == "LAYER") { layer = t.take(); t.expect(";"); }
        else if (key == "RECT") via.shapes.push_back(read_rect(t, layer));
        else if (key == "RESISTANCE" || key == "PROPERTY" || key == "FOREIGN") t.skip_statement();
        else throw std::runtime_error("unsupported VIA definition " + via.name + ": " + key);
    }
    t.expect("END"); t.expect(via.name);
    if (via.shapes.empty()) throw std::runtime_error("VIA has no rectangles: " + via.name);
    return via;
}
void parse_port(TextTokens& t, MacroPinInfo& pin) {
    std::string layer;
    while (t.peek() != "END") {
        const auto key = t.take();
        if (key == "LAYER") {
            layer = t.take(); t.expect(";");
        } else if (key == "RECT") {
            const double x1 = t.number(), y1 = t.number(), x2 = t.number(), y2 = t.number();
            t.expect(";");
            if (layer.empty() || x2 == x1 || y2 == y1)
                throw std::runtime_error("invalid RECT in pin " + pin.name);
            pin.port_rects.push_back({layer, Rect(x1, y1, x2, y2)});
            // Select a real rectangle, never a bounding box across disconnected shapes.
            // The package checker also uses the first rectangle.
            if (!pin.has_rect) {
                pin.layer = layer; pin.bbox = Rect(x1, y1, x2, y2); pin.has_rect = true;
            }
        } else if (key == "CLASS") t.skip_statement();
        else throw std::runtime_error("unsupported pin PORT geometry: " + key);
    }
    t.expect("END");
}
MacroInfo parse_macro(TextTokens& t) {
    MacroInfo macro;
    macro.name = t.take();
    while (t.peek() != "END") {
        const auto key = t.take();
        if (key == "SIZE") {
            macro.width = t.number(); t.expect("BY"); macro.height = t.number(); t.expect(";");
        } else if (key == "CLASS") {
            if (!macro.macro_class.empty()) throw std::runtime_error("duplicate CLASS in macro " + macro.name);
            macro.macro_class = t.take();
            while (t.peek() != ";") {
                if (!macro.macro_subclass.empty()) macro.macro_subclass += " ";
                macro.macro_subclass += t.take();
            }
            t.expect(";");
        } else if (key == "ORIGIN") {
            macro.origin.x = t.number(); macro.origin.y = t.number(); t.expect(";");
        } else if (key == "PIN") {
            MacroPinInfo pin; pin.name = t.take();
            while (t.peek() != "END") {
                if (t.take() == "PORT") parse_port(t, pin);
                else t.skip_statement();
            }
            t.expect("END"); t.expect(pin.name);
            if (!pin.has_rect || !macro.pins.emplace(pin.name, pin).second)
                throw std::runtime_error("missing geometry or duplicate pin: " + pin.name);
        } else if (key == "OBS") {
            auto shapes = parse_shapes(t);
            macro.obstructions.insert(macro.obstructions.end(), shapes.begin(), shapes.end());
            t.expect("END");
        } else t.skip_statement();
    }
    t.expect("END"); t.expect(macro.name);
    if (macro.width <= 0 || macro.height <= 0)
        throw std::runtime_error("missing/invalid SIZE for macro " + macro.name);
    return macro;
}
} // namespace
bool LefParser::parse(const std::string& lef_file_path) {
    try {
        TextTokens t(lef_file_path);
        // A failed additional --lefs file must not corrupt previously loaded data.
        auto directions = layer_directions_;
        auto macros = macros_;
        auto layers = layers_;
        auto vias = vias_;
        bool saw_content = false;
        while (!t.done()) {
            const auto key = t.take();
            if (key == "LAYER") {
                const auto name = t.take();
                std::string type;
                Direction direction = Direction::UNKNOWN;
                double width = 0.0;
                while (t.peek() != "END") {
                    const auto field = t.take();
                    if (field == "TYPE") { type = t.take(); t.expect(";"); }
                    else if (field == "DIRECTION") {
                        const auto value = t.take();
                        if (value == "HORIZONTAL") direction = Direction::HORIZONTAL;
                        else if (value == "VERTICAL") direction = Direction::VERTICAL;
                        else throw std::runtime_error("invalid layer DIRECTION: " + value);
                        t.expect(";");
                    } else if (field == "WIDTH") {
                        width = t.number(); t.expect(";");
                        if (width <= 0) throw std::runtime_error("invalid layer WIDTH: " + name);
                    } else t.skip_statement();
                }
                t.expect("END"); t.expect(name);
                if (type == "ROUTING") {
                    if (!get_layer_index(name) || direction == Direction::UNKNOWN)
                        throw std::runtime_error("expected metal1..metal20 and DIRECTION: " + name);
                    if (!directions.emplace(name, direction).second)
                        throw std::runtime_error("duplicate routing layer: " + name);
                }
                if (!layers.emplace(name, LayerInfo{direction, width, type}).second)
                    throw std::runtime_error("duplicate layer: " + name);
                saw_content = true;
            } else if (key == "MACRO") {
                auto macro = parse_macro(t);
                if (!macros.emplace(macro.name, macro).second)
                    throw std::runtime_error("duplicate MACRO: " + macro.name);
                saw_content = true;
            } else if (key == "VIA") {
                auto via = parse_via(t);
                if (!vias.emplace(via.name, via).second)
                    throw std::runtime_error("duplicate VIA: " + via.name);
                saw_content = true;
            } else if (key == "VIARULE" || key == "SITE") {
                t.skip_block(t.take()); saw_content = true;
            } else if (key == "UNITS" || key == "PROPERTYDEFINITIONS" || key == "SPACING") {
                t.skip_block(key);
            } else if (key == "END") {
                t.expect("LIBRARY");
                if (!t.done()) throw std::runtime_error("content after END LIBRARY");
            } else t.skip_statement();
        }
        if (!saw_content) throw std::runtime_error("no layer or macro definitions");
        layer_directions_ = std::move(directions);
        macros_ = std::move(macros);
        layers_ = std::move(layers);
        vias_ = std::move(vias);
        std::cout << "[INFO] LEF parsed: " << macros_.size() << " macros\n";
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << lef_file_path << ": " << error.what() << "\n";
        return false;
    }
}
} // namespace eda
