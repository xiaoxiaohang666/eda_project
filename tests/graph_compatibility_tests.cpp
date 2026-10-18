#include "router/routing_graph.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

using namespace eda;
namespace fs = std::filesystem;
namespace {
int checks = 0;
void require(bool condition, const std::string& reason) {
    ++checks;
    if (!condition) throw std::runtime_error(reason);
}
std::string write(const fs::path& path, const std::string& text) {
    std::ofstream out(path); out << text; out.close();
    if (!out) throw std::runtime_error("fixture write failed");
    return path.string();
}
std::string technology(const std::string& extra = {}) {
    std::string text;
    for (int l = 1; l <= 20; ++l) {
        auto name = get_layer_name(l);
        text += "LAYER " + name + " TYPE ROUTING ; DIRECTION " +
            (l % 2 ? "HORIZONTAL" : "VERTICAL") + " ; WIDTH 0.2 ; END " + name + "\n";
    }
    return text + "LAYER cut1 TYPE CUT ; END cut1\n" + extra;
}
std::string via(const std::string& name, bool restricted) {
    return "VIA " + name + " DEFAULT " + (restricted ? "TOPOFSTACKONLY " : "") +
        "LAYER metal1 ; RECT -0.1 -0.1 0.1 0.1 ; LAYER cut1 ; RECT -0.05 -0.05 0.05 0.05 ; "
        "LAYER metal2 ; RECT -0.1 -0.1 0.1 0.1 ; END " + name + "\n";
}
// Mode 0: ordinary preferred-axis-only DEF. Mode 1: both axes (legacy
// scoring convention), mode 2: only the wrong axis (must still be rejected).
std::string design(int mode, bool obstacle = false, bool duplicate = false) {
    std::string text = "UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 300 300 ) ; "
        "GCELLGRID X 0 DO 4 STEP 100 ; GCELLGRID Y 0 DO 4 STEP 100 ; ";
    for (int l = 1; l <= 20; ++l) {
        const auto name = get_layer_name(l);
        const bool horizontal = l % 2;
        const auto x = "TRACKS X 10 DO 15 STEP 20 LAYER " + name + " ; ";
        const auto y = "TRACKS Y 5 DO 30 STEP 10 LAYER " + name + " ; ";
        const auto physical = horizontal ? y : x;
        const auto other = horizontal ? x : y;
        text += mode == 2 ? other : physical;
        if (mode == 1) text += other;
        if (duplicate && mode != 2) text += physical;
    }
    text += obstacle ? "COMPONENTS 1 ; - block block + PLACED ( 0 0 ) N ; END COMPONENTS "
                     : "COMPONENTS 0 ; END COMPONENTS ";
    text += "PINS 2 ; - a + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; "
        "- b + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 250 250 ) N ; END PINS "
        "NETS 1 ; - n ( PIN a ) ( PIN b ) ; END NETS END DESIGN";
    return text;
}
void test_track_modes(const fs::path& folder) {
    LefParser lef;
    require(lef.parse(write(folder / "tracks.lef", technology())), "technology parsed");
    for (int mode = 0; mode <= 2; ++mode) {
        DefParser def;
        require(def.parse(write(folder / "tracks.def", design(mode, false, true)), lef), "DEF parsed");
        if (mode == 2) {
            bool rejected = false;
            try { RoutingGraph graph(def, lef); } catch (const std::runtime_error& e) {
                rejected = std::string(e.what()).find("physical TRACKS") != std::string::npos;
            }
            require(rejected, "an absent physical axis is not fabricated");
            continue;
        }
        RoutingGraph graph(def, lef);
        for (int l = 1; l <= 20; ++l) {
            require(graph.guide_uses_physical_tracks(l) == (mode == 0), "guide capacity mode recorded per layer");
            const int a = graph.node(0, 0, l);
            const bool horizontal = l % 2;
            const int b = horizontal ? graph.node(1, 0, l) : graph.node(0, 1, l);
            require(graph.edge_capacity(a, b) == (horizontal ? 10 : 5), "physical tracks stay deduplicated");
            require(graph.guide_capacity(a) == (mode == 0 ? (horizontal ? 10 : 5) : (horizontal ? 5 : 10)),
                    "fallback counts the actual preferred-axis tracks and legacy capacities remain exact");
        }
    }
}
void test_restricted_via(const fs::path& folder) {
    for (bool ordinary : {false, true}) {
        LefParser lef; DefParser def;
        require(lef.parse(write(folder / "via.lef", technology(via("restricted", true) +
            (ordinary ? via("ordinary", false) : "")))), "restricted via syntax parsed");
        require(lef.get_via("restricted")->top_of_stack_only, "via restriction retained");
        require(def.parse(write(folder / "via.def", design(0)), lef), "via DEF parsed");
        RoutingGraph graph(def, lef);
        require(graph.legal_via({50, 50}, 1) == ordinary, "restricted via cannot provide an ordinary graph edge");
        require(graph.via_rects({50, 50}, 1).empty() != ordinary, "restricted via footprint not exported");
    }
}
void test_obstacle_index(const fs::path& folder) {
    const std::string macro = "MACRO block SIZE 30 BY 30 ; OBS LAYER metal1 ; "
        "RECT 8 0 12 30 ; RECT 15 8 23 12 ; RECT 0 22 5 30 ; "
        "LAYER metal20 ; RECT 8 0 12 30 ; END END block";
    LefParser lef; DefParser def;
    require(lef.parse(write(folder / "obs.lef", technology(macro))), "OBS LEF parsed");
    require(def.parse(write(folder / "obs.def", design(0, true)), lef), "OBS DEF parsed");
    RoutingGraph graph(def, lef);
    // Independent complete-geometry scan checks the compressed bucket lookup,
    // including shapes spanning cell boundaries and queries touching an OBS.
    std::mt19937 random(17);
    std::uniform_int_distribution<int> coordinate(-5, 305), extent(1, 80);
    for (int l : {1, 2, 20}) for (int i = 0; i < 1500; ++i) {
        const double x = coordinate(random), y = coordinate(random);
        const Rect rect{x, y, x + extent(random), y + extent(random)};
        bool legal = rect.x1 >= 0 && rect.y1 >= 0 && rect.x2 <= 300 && rect.y2 <= 300;
        for (const auto& shape : lef.get_macro("block")->obstructions) {
            if (get_layer_index(shape.layer) != l) continue;
            const auto obs = transform_macro_rect(shape.rect, *lef.get_macro("block"), def.components().at("block"), 10);
            if (rect.x1 < obs.x2 && rect.x2 > obs.x1 && rect.y1 < obs.y2 && rect.y2 > obs.y1) legal = false;
        }
        require(graph.legal_rect(rect, l) == legal, "compressed OBS lookup agrees with a complete geometric scan");
    }
    require(graph.legal_rect({120, 40, 125, 45}, 1), "touching OBS boundary has no positive-area overlap");
    require(!graph.legal_rect({119, 40, 125, 45}, 1), "one DBU overlap is blocked across a cell boundary");
    for (int l : {1, 2, 20}) for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
        const bool horizontal = l % 2;
        const int a = graph.node(c, r, l), b = horizontal ? graph.node(c + 1, r, l) : graph.node(c, r + 1, l);
        int capacity = 0;
        if (graph.legal_node(a) && graph.legal_node(b)) {
            const auto cell = graph.cell(a);
            const auto start = graph.point(a), finish = graph.point(b);
            for (int track = horizontal ? 5 : 10; track < 300; track += horizontal ? 10 : 20) {
                if (track < (horizontal ? cell.y1 : cell.x1) || track >= (horizontal ? cell.y2 : cell.x2)) continue;
                const Point p = horizontal ? Point{start.x, double(track)} : Point{double(track), start.y};
                const Point q = horizontal ? Point{finish.x, double(track)} : Point{double(track), finish.y};
                capacity += graph.legal_segment(p, q, l);
            }
        }
        require(graph.edge_capacity(a, b) == capacity, "batched capacity equals independent per-track geometry checks");
    }
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: graph_compatibility_tests fixture_directory");
        fs::path folder = argv[1]; fs::create_directories(folder);
        test_track_modes(folder); test_restricted_via(folder); test_obstacle_index(folder);
        std::cout << "Graph compatibility tests passed: " << checks << " checks\n";
    } catch (const std::exception& error) {
        std::cerr << "Graph compatibility test failed: " << error.what() << '\n'; return 1;
    }
}
