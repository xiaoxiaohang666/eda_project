#include "router/routing_graph.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

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
std::string technology(bool widths = true, bool vias = true) {
    std::string text;
    for (int l = 1; l <= 20; ++l) {
        const auto name = get_layer_name(l);
        text += "LAYER " + name + " TYPE ROUTING ; DIRECTION " +
            (l % 2 ? "HORIZONTAL" : "VERTICAL") + " ; " +
            (widths ? "WIDTH 0.2 ; " : "") + "END " + name + "\n";
    }
    if (vias) for (int l = 1; l < 20; ++l) {
        const auto cut = "cut" + std::to_string(l), via = "v" + std::to_string(l);
        text += "LAYER " + cut + " TYPE CUT ; END " + cut + "\nVIA " + via + " DEFAULT " +
            "LAYER " + get_layer_name(l) + " ; RECT -0.2 -0.2 0.2 0.2 ; LAYER " +
            cut + " ; RECT -0.1 -0.1 0.1 0.1 ; LAYER " + get_layer_name(l + 1) +
            " ; RECT -0.2 -0.2 0.2 0.2 ; END " + via + "\n";
    }
    return text;
}
std::string design(bool tracks = true, bool obstacle = false) {
    std::string text = "UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 300 300 ) ; "
        "GCELLGRID X 0 DO 4 STEP 100 ; GCELLGRID Y 0 DO 4 STEP 100 ; ";
    if (tracks) for (int l = 1; l <= 20; ++l) {
        const auto name = get_layer_name(l);
        text += "TRACKS X 10 DO 15 STEP 20 LAYER " + name + " ; "
                "TRACKS Y 5 DO 30 STEP 10 LAYER " + name + " ; ";
    }
    text += obstacle ? "COMPONENTS 1 ; - block obstacle + PLACED ( 0 0 ) N ; END COMPONENTS "
                     : "COMPONENTS 0 ; END COMPONENTS ";
    text += "PINS 2 ; - a + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; "
            "- b + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 250 250 ) N ; END PINS "
            "NETS 1 ; - n ( PIN a ) ( PIN b ) ; END NETS END DESIGN";
    return text;
}
void test_capacity_and_geometry() {
    LefParser lef; DefParser def;
    require(lef.parse(file("resource.lef", technology())), "parse resource LEF");
    require(lef.get_layer("metal1")->width == 0.2, "LEF width retained in microns");
    require(lef.get_via("v1") && lef.get_via("v1")->shapes.size() == 3, "complete via geometry");
    require(def.parse(file("resource.def", design()), lef), "parse resource DEF");
    require(def.tracks().size() == 40, "all TRACKS retained");
    RoutingGraph graph(def, lef);
    const int a = graph.node(0, 0, 1), b = graph.node(1, 0, 1);
    require(graph.size() == 180 && graph.node(3, 0, 1) == -1, "graph bounds");
    require(graph.col(b) == 1 && graph.row(b) == 0 && graph.layer(b) == 1, "node round trip");
    require(graph.edge_capacity(a, b) == 10, "horizontal physical capacity uses Y tracks");
    require(graph.guide_capacity(a) == 5, "horizontal guide proxy uses scorer X convention");
    require(graph.edge_capacity(graph.node(0, 0, 2), graph.node(0, 1, 2)) == 5,
            "vertical physical capacity uses X tracks");
    require(graph.guide_capacity(graph.node(0, 0, 2)) == 10, "vertical guide capacity uses Y tracks");
    require(graph.edge_capacity(a, graph.node(0, 1, 1)) == 0, "preferred direction only");
    require(graph.edge_capacity(a, graph.node(0, 0, 2)) == -1, "legal via has no invented track capacity");
    require(!graph.legal_edge(graph.node(0, 0, 10), graph.node(0, 0, 11)), "no implicit TSV edge");
    require(!graph.legal_edge(a, graph.node(0, 0, 3)), "no skipped layers");
    require(graph.edge_length(a, b) == 100 && graph.ell0() == 100, "physical DBU lengths");
    require(!graph.legal_segment({50, 50}, {100, 100}, 1), "no diagonal connector");
    require(!graph.legal_segment({0, 50}, {50, 50}, 1), "wire width must fit inside die");
    require(graph.via_rects({50, 50}, 1).size() == 2, "via export includes routing footprints");
    require(!graph.legal_via({1, 50}, 1), "via enclosure must fit inside die");
}
void test_obstruction_and_transform() {
    LefParser lef; DefParser def;
    const std::string macro = "MACRO obstacle SIZE 30 BY 30 ; OBS LAYER metal1 ; "
        "RECT 8 0 12 3 ; END END obstacle";
    require(lef.parse(file("obs.lef", technology() + macro)), "parse macro OBS");
    require(def.parse(file("obs.def", design(true, true)), lef), "parse OBS placement");
    require(lef.get_macro("obstacle")->obstructions.size() == 1, "OBS shapes retained");
    RoutingGraph graph(def, lef);
    const int a = graph.node(0, 0, 1), b = graph.node(1, 0, 1);
    require(graph.legal_node(a) && graph.legal_node(b), "partial OBS does not erase whole GCell");
    require(graph.edge_capacity(a, b) == 7, "OBS subtracts blocked orthogonal tracks");
    require(!graph.legal_segment({50, 20}, {150, 20}, 1), "real corridor obstruction blocked");
    require(graph.legal_segment({50, 50}, {150, 50}, 1), "clear corridor survives nearby OBS");
    require(!graph.legal_via({100, 20}, 1), "via footprint obeys OBS");
    MacroInfo m; m.width = 10; m.height = 20; m.origin = {-1, -2};
    const Component c{"u", "m", {100, 200}, "W"};
    const auto rect = transform_macro_rect({1, 2, 3, 4}, m, c, 10);
    require(rect.x1 == 280 && rect.x2 == 300 && rect.y1 == 200 && rect.y2 == 220,
            "rectangle orientation plus origin plus DBU matches pin contract");
}
void test_missing_resources() {
    for (int kind = 0; kind < 2; ++kind) {
        LefParser lef; DefParser def;
        require(lef.parse(file("missing.lef", technology(kind != 0))), "legacy LEF allowed");
        require(def.parse(file("missing.def", design(kind != 1)), lef), "legacy DEF allowed");
        bool rejected = false;
        try { RoutingGraph graph(def, lef); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "routing graph diagnoses missing physical resources");
    }
    LefParser lef; DefParser def;
    require(lef.parse(file("no-via.lef", technology(true, false))), "no-via LEF");
    require(def.parse(file("no-via.def", design()), lef), "no-via DEF");
    RoutingGraph graph(def, lef);
    require(!graph.legal_via({50, 50}, 1), "missing via definitions never fabricate connectivity");
    const auto before = lef.vias().size();
    require(!lef.parse(file("bad-via.lef", "VIA malformed LAYER metal1 ; RECT 0 0 1 1 ;")),
            "truncated VIA rejected");
    require(lef.vias().size() == before && lef.get_layer("metal1"), "failed LEF merge stays atomic");
}
void test_sample(const fs::path& source) {
    const auto dir = source / "benchmarks/sample001";
    LefParser lef; DefParser def;
    require(lef.parse((dir / "sample001.input.lef").string()), "sample LEF with vias and OBS");
    require(def.parse((dir / "sample001.input.def").string(), lef), "sample TRACKS");
    RoutingGraph graph(def, lef);
    size_t usable = 0;
    for (int id = 0; id < static_cast<int>(graph.size()); ++id) {
        if (graph.legal_node(id)) ++usable;
        for (int other : graph.neighbors(id)) {
            require(graph.legal_edge(other, id), "sample edge symmetry");
            require((graph.layer(id) <= 10) == (graph.layer(other) <= 10), "sample no TSV leak");
        }
    }
    require(usable > graph.size() * 9 / 10, "sample partial OBS retains useful graph");
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: resource_tests source_root output_directory");
        work = argv[2]; fs::create_directories(work);
        test_capacity_and_geometry(); test_obstruction_and_transform(); test_missing_resources();
        test_sample(argv[1]);
        std::cout << "Resource tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Resource test failed: " << error.what() << "\n";
        return 1;
    }
}
