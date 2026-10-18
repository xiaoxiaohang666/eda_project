#include "parser/def_parser.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace eda;
namespace {
fs::path work;
size_t checks = 0;
void require(bool value, const std::string& message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
std::string file(const std::string& name, const std::string& text) {
    const auto path = work / name;
    std::ofstream out(path); out << text; out.close();
    if (!out) throw std::runtime_error("cannot write terminal parser fixture");
    return path.string();
}
bool equals(const Rect& r, double x1, double y1, double x2, double y2) {
    return r.x1 == x1 && r.y1 == y1 && r.x2 == x2 && r.y2 == y2;
}
void test_terminals() {
    const std::string lef_text =
        "LAYER metal1 TYPE ROUTING ; DIRECTION HORIZONTAL ; END metal1 "
        "LAYER metal2 TYPE ROUTING ; DIRECTION VERTICAL ; END metal2 "
        "MACRO core CLASS CORE FEEDTHRU ; ORIGIN -1 -2 ; SIZE 10 BY 20 ; "
        "PIN A PORT LAYER metal1 ; RECT 1 2 3 4 ; RECT 8 10 9 11 ; "
        "LAYER metal2 ; RECT 4 6 5 7 ; END END A END core "
        "MACRO block CLASS BLOCK SOFT ; SIZE 10 BY 20 ; "
        "PIN B PORT LAYER metal1 ; RECT 1 2 3 4 ; END END B END block "
        "MACRO unknown SIZE 2 BY 2 ; "
        "PIN C PORT LAYER metal1 ; RECT 0.1 0.1 0.2 0.2 ; END END C END unknown";
    const std::string def_text =
        "UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 1000 1000 ) ; "
        "GCELLGRID X 0 DO 11 STEP 100 ; GCELLGRID Y 0 DO 11 STEP 100 ; "
        "COMPONENTS 3 ; - u core + PLACED ( 100 200 ) W ; "
        "- m block + PLACED ( 600 200 ) FS ; "
        "- unknown_i unknown + PLACED ( 500 500 ) N ; END COMPONENTS "
        "PINS 1 ; - io + PORT + LAYER metal1 ( -1 -2 ) ( 1 2 ) "
        "+ LAYER metal2 ( 2 4 ) ( 4 6 ) + FIXED ( 700 600 ) FS ; END PINS "
        "NETS 1 ; - n ( u A ) ( m B ) ( unknown_i C ) ( PIN io ) ; END NETS END DESIGN";
    LefParser lef;
    DefParser def;
    require(lef.parse(file("terminals.lef", lef_text)), "parse class and multilayer ports");
    const auto* core = lef.get_macro("core");
    require(core->macro_class == "CORE" && core->macro_subclass == "FEEDTHRU", "CORE class retained");
    require(lef.get_macro("block")->macro_class == "BLOCK" &&
            lef.get_macro("block")->macro_subclass == "SOFT", "BLOCK class retained");
    require(lef.get_macro("unknown")->macro_class.empty(), "missing class remains unknown");
    const auto& port = core->pins.at("A");
    require(port.port_rects.size() == 3 && port.port_rects[2].layer == "metal2", "all real shapes retained");
    require(equals(port.bbox, 1, 2, 3, 4), "compatibility coordinate uses first real rectangle");
    require(def.parse(file("terminals.def", def_text), lef), "parse placed terminals");
    const auto& pins = def.nets().front().pins;
    const auto& pin = pins[0];
    require(pin.instance_name == "u" && pin.owner_type == PinOwnerType::CORE_CELL && pin.has_owner_bbox,
            "CORE owner identified without size guesses");
    require(equals(pin.owner_bbox, 100, 200, 300, 300), "W owner SIZE box independent of LEF ORIGIN");
    require(pin.port_rects.size() == 3 && equals(pin.port_rects[0].rect, 280, 200, 300, 220),
            "W terminal geometry includes ORIGIN and DBU");
    require(equals(pin.port_rects[1].rect, 210, 270, 220, 280), "disconnected shape remains separate");
    require(pin.port_rects[2].layer_idx == 2 && equals(pin.port_rects[2].rect, 250, 230, 260, 240),
            "second-layer actual shape retained");
    require(pin.coord.x == 290 && pin.coord.y == 210 && pin.evaluator_coord.x == 120 &&
            pin.evaluator_coord.y == 230, "actual and fixed-checker coordinates remain distinct");
    require(pins[1].owner_type == PinOwnerType::BLOCK_MACRO && pins[1].instance_name == "m" &&
            equals(pins[1].owner_bbox, 600, 200, 700, 400), "BLOCK remains explicitly strict");
    require(equals(pins[1].port_rects[0].rect, 610, 360, 630, 380), "FS macro port mirrored correctly");
    require(pins[2].owner_type == PinOwnerType::OTHER_MACRO && pins[2].has_owner_bbox,
            "classless macro cannot gain CORE terminal privilege");
    const auto& io = pins[3];
    require(io.owner_type == PinOwnerType::TOP_LEVEL && !io.has_owner_bbox && io.instance_name.empty(),
            "top-level terminal has no cell owner");
    require(io.port_rects.size() == 2 && equals(io.port_rects[0].rect, 699, 598, 701, 602) &&
            io.port_rects[1].layer_idx == 2 && equals(io.port_rects[1].rect, 702, 594, 704, 596),
            "all IO rectangles use actual orientation and placement");
    require(io.coord.x == 700 && io.coord.y == 600 && io.evaluator_coord.x == 700 && io.evaluator_coord.y == 600,
            "IO compatibility point unchanged");
    require(!lef.parse(file("duplicate-class.lef", "MACRO duplicate CLASS CORE ; CLASS BLOCK ; SIZE 1 BY 1 ; END duplicate")),
            "duplicate macro CLASS rejected");
    require(lef.get_macro("core") && !lef.get_macro("duplicate"), "class parse failure is atomic");
}
void test_official_net_name_does_not_determine_die() {
    LefParser lef;
    require(lef.parse(file("net-names.lef",
        "LAYER metal1 TYPE ROUTING ; DIRECTION HORIZONTAL ; END metal1 "
        "LAYER metal11 TYPE ROUTING ; DIRECTION HORIZONTAL ; END metal11")),
        "parse layers for net name regression");
    const std::string header =
        "UNITS DISTANCE MICRONS 1 ; DIEAREA ( 0 0 ) ( 100 100 ) ; "
        "GCELLGRID X 0 DO 3 STEP 50 ; GCELLGRID Y 0 DO 3 STEP 50 ; "
        "PINS 2 ; - lower + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 25 25 ) N ; "
        "- upper + LAYER metal11 ( -1 -1 ) ( 1 1 ) + FIXED ( 75 75 ) N ; END PINS ";
    for (const auto& name : {std::string("BOTTOM__cross"), std::string("TOP__cross")}) {
        const auto path = file(name + ".def", header + "NETS 1 ; - " + name +
            " ( PIN lower ) ( PIN upper ) ; END NETS END DESIGN");
        DefParser official;
        require(official.parse(path, lef, DefGridMapping::OFFICIAL_FULL),
            "official mode accepts a cross-die net with a die-like name");
        require(official.nets().front().name == name && official.nets().front().is_cross_die,
            "actual terminal layers determine the cross-die flag and names are preserved");
        DefParser legacy;
        require(!legacy.parse(path, lef), "legacy checker prefix contract remains unchanged");
    }
    const auto path = file("TOP__bottom.def", header +
        "NETS 1 ; - TOP__bottom ( PIN lower ) ; END NETS END DESIGN");
    DefParser official;
    require(official.parse(path, lef, DefGridMapping::OFFICIAL_FULL) &&
            !official.nets().front().is_cross_die && official.nets().front().has_bottom_pin(),
        "official mode infers an intra-die net from its terminals despite its name");
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: terminal_parser_tests source_root output_directory");
        work = argv[2]; fs::create_directories(work);
        test_terminals(); test_official_net_name_does_not_determine_die();
        std::cout << "Terminal parser tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Terminal parser tests failed: " << e.what() << "\n";
        return 1;
    }
}
