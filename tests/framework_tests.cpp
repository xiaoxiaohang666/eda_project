#include "parser/def_parser.hpp"
#include "parser/thermal_parser.hpp"
#include "router/baseline_router.hpp"
#include "validation/guide_validator.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace eda;
namespace {
fs::path work;
int checks = 0;
void require(bool value, const std::string& reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
std::string file(const std::string& name, const std::string& content) {
    const auto path = work / name;
    std::ofstream out(path); out << content; out.close();
    if (!out) throw std::runtime_error("cannot write test fixture");
    return path.string();
}
std::string technology() {
    std::string text;
    for (int l = 1; l <= 20; ++l) {
        const auto layer = get_layer_name(l);
        text += "LAYER " + layer + " TYPE ROUTING ; DIRECTION " +
                (l % 2 ? "HORIZONTAL" : "VERTICAL") + " ; END " + layer + "\n";
    }
    return text;
}
std::string macro() {
    return "MACRO cell ORIGIN 0 0 ; SIZE 10 BY 20 ; "
           "PIN A PORT LAYER metal1 ; RECT 1 2 3 4 ; RECT 8 10 9 11 ; END END A END cell\n";
}
std::string design(const std::string& orient = "N", const std::string& net = "n") {
    return "VERSION 5.8 ; UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 1000 1000 ) ; "
           "GCELLGRID X 0 DO 11 STEP 100 ; GCELLGRID Y 0 DO 11 STEP 100 ; "
           "COMPONENTS 1 ; - u cell + PLACED ( 100 200 ) " + orient + " ; END COMPONENTS "
           "PINS 1 ; - io + NET " + net + " + LAYER metal1 ( 10 20 ) ( 30 40 ) "
           "+ FIXED ( 700 600 ) FS ; END PINS "
           "NETS 1 ; - " + net + " ( u A ) ( PIN io ) ; END NETS END DESIGN";
}
std::string replace(std::string s, const std::string& from, const std::string& to) {
    const auto p = s.find(from);
    if (p == std::string::npos) throw std::runtime_error("bad test replacement");
    s.replace(p, from.size(), to); return s;
}
void test_grid() {
    GCellGrid g;
    require(!g.valid() && g.get_col(0) == -1 && g.get_row(0) == -1, "empty grid");
    g = {{0, 10, 20}, {0, 10}, 2, 1};
    require(g.valid() && g.get_col(10) == 1 && g.get_col(20) == 1, "grid edge mapping");
    require(g.get_col(-1) == -1 && g.get_col(21) == -1, "out-of-grid must not clamp");
    g.x_lines = {0, 10, 10}; require(!g.valid(), "duplicate grid line");
    for (const auto& name : {"metal0", "metal21", "metal1junk", "metal01", "metal", "M1"})
        require(get_layer_index(name) == 0, "strict layer name");
}
void test_parsers() {
    LefParser lef;
    require(lef.parse(file("tech.lef", technology())), "technology-only LEF");
    require(lef.parse(file("cell.lef", macro())), "incremental macro LEF");
    require(lef.get_macro("cell")->pins.at("A").bbox.center().x == 2, "first real pin rectangle");
    require(!lef.parse(file("broken.lef", "MACRO broken SIZE 1 BY 1 ;")), "truncated LEF");
    require(lef.get_macro("broken") == nullptr && lef.get_macro("cell"), "atomic LEF parse");
    require(!lef.parse(file("duplicate.lef", macro())), "duplicate macro rejected");
    require(lef.get_layer_direction("metal99") == Direction::UNKNOWN, "no invented direction");
    DefParser def;
    const std::vector<std::pair<std::string, Point>> cases = {
        {"N", {120, 230}}, {"S", {180, 370}}, {"W", {270, 220}}, {"E", {130, 280}},
        {"FN", {180, 230}}, {"FS", {120, 370}}, {"FW", {130, 220}}, {"FE", {270, 280}}};
    for (const auto& [orient, expected] : cases) {
        require(def.parse(file("orient.def", design(orient)), lef), "orientation parse " + orient);
        const auto p = def.nets()[0].pins[0].coord;
        require(p.x == expected.x && p.y == expected.y, "orientation coordinates " + orient);
        const auto io = def.nets()[0].pins[1].coord;
        require(io.x == 720 && io.y == 570, "IO placement plus oriented local rectangle");
        require(def.nets().size() == 1 && def.components().size() == 1, "parse reuse resets state");
    }
    LefParser shifted;
    require(shifted.parse(file("shift.lef", technology() + replace(macro(), "ORIGIN 0 0", "ORIGIN -1 -2"))), "LEF origin");
    require(def.parse(file("origin.def", design()), shifted), "nonzero origin DEF");
    require(def.nets()[0].pins[0].coord.x == 110 && def.nets()[0].pins[0].coord.y == 210, "origin shift sign");
    for (const auto& bad : {
            std::string{}, replace(design(), "END DESIGN", ""),
            replace(design(), "( u A )", "( absent A )"),
            replace(design(), "( u A )", "( u missing )"),
            replace(design(), "( PIN io )", "( PIN missing )"),
            replace(design(), "NETS 1 ;", "NETS 2 ;"),
            replace(design(), "STEP 100", "STEP 0"),
            replace(design(), "GCELLGRID X 0 DO 11 STEP 100 ;", ""),
            replace(design(), "( 100 200 )", "( 2000 2000 )"),
            replace(design(), "u cell", "u unknown"),
            replace(design(), "( u A ) ( PIN io )", ""),
            design("INVALID"), design("N", "TOP__wrong")}) {
        require(!def.parse(file("bad.def", bad), lef), "invalid DEF must fail");
        require(def.nets().empty() && !def.gcell_grid().valid(), "no stale partial DEF");
    }
    auto clipped = replace(design(), "X 0 DO 11 STEP 100", "X -100 DO 14 STEP 100");
    clipped = replace(clipped, "Y 0 DO 11 STEP 100", "Y 100 DO 9 STEP 100");
    require(def.parse(file("clipped.def", clipped), lef), "grid clipping/extension");
    require(def.gcell_grid().x_lines.front() == 0 && def.gcell_grid().x_lines.back() == 1000 &&
            def.gcell_grid().y_lines.front() == 0 && def.gcell_grid().y_lines.back() == 1000, "die boundaries");
}
void test_thermal() {
    const GCellGrid grid{{-10, 0, 10}, {20, 30}, 2, 1};
    const Rect die{-10, 20, 10, 30};
    const std::string json = R"({"meta":{"ambientTemp":25},"nodes":[[0.5,0.5,0,40],[0.5,0.5,1,80],[0.6,0.5,0,30],[1.5,0.5,0,55]]})";
    ThermalParser thermal;
    require(thermal.parse_json(file("heat.json", json), grid, 10, die), "thermal parse");
    require(thermal.get_temperature(0, 0) == 45 && thermal.get_temperature(1, 0) == 55,
            "average z first, then xy; nonzero die origin");
    for (const auto& bad : {
            json.substr(0, json.size() - 1), json + "garbage",
            replace(json, "25", "025"), replace(json, "25", "NaN"),
            replace(json, "\"ambientTemp\":25", "\"other\":25"),
            replace(json, "\"ambientTemp\":25", "\"ambientTemp\":25,\"ambientTemp\":30"),
            replace(json, "[1.5,0.5,0,55]", "[9.5,0.5,0,55]"),
            replace(json, "[0.5,0.5,0,40]", "[0.5,0.5]")}) {
        require(!thermal.parse_json(file("bad.json", bad), grid, 10, die), "invalid thermal JSON must fail");
        require(thermal.hotspots().empty() && thermal.ambient_temperature() == 25, "thermal resets on failure");
    }
    require(!thermal.parse_json(file("heat.json", json), {{0, 0, 1}, {0, 1}, 2, 1}, 10, die),
            "invalid thermal grid");
}
void test_validation() {
    LefParser lef;
    require(lef.parse(file("valid.lef", technology() + macro())), "validation LEF");
    DefParser def;
    require(def.parse(file("valid.def", design()), lef), "validation DEF");
    GuideMap guides{{"n", {{100, 200, 800, 600, "metal1"}}}};
    std::string error;
    require(validate_guides(def, lef, guides, error), "valid guide");
    for (const auto& bad : {
            GuideMap{}, GuideMap{{"extra", guides.at("n")}},
            GuideMap{{"n", {{0, 0, 100, 100, "metal1"}}}},
            GuideMap{{"n", {{100, 200, 800, 600, "metal1"}, {900, 900, 1000, 1000, "metal1"}}}},
            GuideMap{{"n", {{100, 200, 800, 600, "metal1"}, {100, 200, 800, 600, "metal11"}}}},
            GuideMap{{"n", {{100, 200, 1800, 600, "metal1"}}}},
            GuideMap{{"n", {{100, 200, 800, 600, "metal21"}}}}}) {
        require(!validate_guides(def, lef, bad, error) && !error.empty(), "reject invalid guide");
    }
    ThermalParser thermal;
    BaselineRouter router(def, lef, thermal);
    require(!router.export_guide((work / "premature.guide").string()), "export before routing");
    require(router.route_all() && router.export_guide((work / "valid.guide").string()), "baseline routing");
    require(!router.export_guide("/dev/full"), "write/flush failure must propagate");
    // Cross-die nets require a positive-area overlap and no layer skipping.
    const auto top_macro = replace(replace(macro(), "MACRO cell", "MACRO topcell"),
                                   "END cell", "END topcell");
    require(lef.parse(file("top.lef", replace(top_macro, "LAYER metal1", "LAYER metal11"))), "top macro");
    auto cross_def = replace(design(), "COMPONENTS 1", "COMPONENTS 2");
    cross_def = replace(cross_def, "END COMPONENTS",
                        "- v topcell + PLACED ( 100 200 ) N ; END COMPONENTS");
    cross_def = replace(cross_def, "( PIN io )", "( v A )");
    require(def.parse(file("cross.def", cross_def), lef), "cross die input");
    GuideMap cross{{"n", {}}};
    for (int l = 1; l <= 11; ++l) cross["n"].emplace_back(100, 200, 200, 300, get_layer_name(l));
    require(validate_guides(def, lef, cross, error), "valid layer stack");
    auto skipped = cross; skipped["n"].erase(skipped["n"].begin() + 5);
    require(!validate_guides(def, lef, skipped, error), "reject skipped layer");
    auto touching = cross;
    touching["n"][9] = {200, 200, 300, 300, "metal10"};
    require(!validate_guides(def, lef, touching, error), "reject boundary-only TSV contact");
}
void test_sample(const fs::path& source) {
    const auto base = source / "benchmarks/sample001";
    LefParser lef;
    require(lef.parse((base / "sample001.input.lef").string()), "sample LEF");
    DefParser def;
    require(def.parse((base / "sample001.input.def").string(), lef), "sample DEF");
    require(def.nets().size() == 203 && def.components().size() == 218 &&
            def.io_pins().size() == 12 && def.gcell_grid().cols == 42, "sample counts");
    int cross = 0; for (const auto& net : def.nets()) cross += net.is_cross_die;
    require(cross == 3, "sample cross-die nets");
    ThermalParser thermal;
    require(thermal.parse_json((base / "sample001.thermal.input.json").string(),
            def.gcell_grid(), def.dbu_per_micron(), def.die_area()), "sample thermal");
    std::ofstream heat(work / "sample_heat.csv"); heat.precision(17);
    for (int r = 0; r < def.gcell_grid().rows; ++r) {
        for (int c = 0; c < def.gcell_grid().cols; ++c) {
            if (c) heat << ',';
            heat << thermal.get_temperature(c, r);
        }
        heat << '\n';
    }
    BaselineRouter router(def, lef, thermal);
    require(router.route_all(), "sample guide validation");
    require(router.export_guide((work / "sample.guide").string()), "sample guide export");
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("expected source and artifact directories");
        work = argv[2]; fs::create_directories(work);
        test_grid(); test_parsers(); test_thermal(); test_validation(); test_sample(argv[1]);
        std::cout << "PASS: " << checks << " framework checks\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << "\n"; return 1;
    }
}
