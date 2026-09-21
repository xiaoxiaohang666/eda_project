#include "parser/lef_parser.hpp"
#include "parser/text_tokens.hpp"
#include <iostream>

namespace eda {
namespace {
void parse_port(TextTokens& t, MacroPinInfo& pin) {
    std::string layer;
    while (t.peek() != "END") {
        const auto key = t.take();
        if (key == "LAYER") {
            layer = t.take(); t.expect(";");
        } else if (key == "RECT") {
            const double x1 = t.number(), y1 = t.number(), x2 = t.number(), y2 = t.number();
            t.expect(";");
            if (layer.empty() || x2 <= x1 || y2 <= y1)
                throw std::runtime_error("invalid RECT in pin " + pin.name);
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
            while (t.peek() != "END") t.skip_statement();
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
        bool saw_content = false;
        while (!t.done()) {
            const auto key = t.take();
            if (key == "LAYER") {
                const auto name = t.take();
                std::string type;
                Direction direction = Direction::UNKNOWN;
                while (t.peek() != "END") {
                    const auto field = t.take();
                    if (field == "TYPE") { type = t.take(); t.expect(";"); }
                    else if (field == "DIRECTION") {
                        const auto value = t.take();
                        if (value == "HORIZONTAL") direction = Direction::HORIZONTAL;
                        else if (value == "VERTICAL") direction = Direction::VERTICAL;
                        else throw std::runtime_error("invalid layer DIRECTION: " + value);
                        t.expect(";");
                    } else t.skip_statement();
                }
                t.expect("END"); t.expect(name);
                if (type == "ROUTING") {
                    if (!get_layer_index(name) || direction == Direction::UNKNOWN)
                        throw std::runtime_error("expected metal1..metal20 and DIRECTION: " + name);
                    if (!directions.emplace(name, direction).second)
                        throw std::runtime_error("duplicate routing layer: " + name);
                }
                saw_content = true;
            } else if (key == "MACRO") {
                auto macro = parse_macro(t);
                if (!macros.emplace(macro.name, macro).second)
                    throw std::runtime_error("duplicate MACRO: " + macro.name);
                saw_content = true;
            } else if (key == "VIA" || key == "VIARULE" || key == "SITE") {
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
        std::cout << "[INFO] LEF parsed: " << macros_.size() << " macros\n";
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << lef_file_path << ": " << error.what() << "\n";
        return false;
    }
}
} // namespace eda