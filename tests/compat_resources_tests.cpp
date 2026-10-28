#include "scoring/compat_resources.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace eda;
using namespace eda::compat;
namespace {
fs::path work;
int checks = 0;
void require(bool value, const std::string& reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
std::string file(const std::string& name, const std::string& content) {
    const auto path = work / name;
    std::ofstream out(path);
    out << content;
    out.close();
    if (!out) throw std::runtime_error("cannot write compatibility resource fixture");
    return path.string();
}
std::string technology(const std::string& macros = "") {
    std::string text;
    for (int l = 1; l <= 20; ++l) {
        const auto name = get_layer_name(l);
        text += "LAYER " + name + " TYPE ROUTING ; DIRECTION " +
            (l % 2 ? "HORIZONTAL" : "VERTICAL") + " ; WIDTH 0.2 ; END " + name + "\n";
    }
    return text + macros;
}
std::string design(const std::string& tracks, const std::string& components,
                   const std::string& pins, const std::string& nets) {
    return "UNITS DISTANCE MICRONS 1 ; DIEAREA ( 0 0 ) ( 300 300 ) ; "
        "GCELLGRID X 0 DO 4 STEP 100 ; GCELLGRID Y 0 DO 4 STEP 100 ; " +
        tracks + components + pins + nets + " END DESIGN";
}
void parse(LefParser& lef, DefParser& def, const std::string& macros,
           const std::string& tracks, const std::string& components,
           const std::string& pins, const std::string& nets) {
    require(lef.parse(file("resource.lef", technology(macros))), "parse LEF resource fixture");
    require(def.parse(file("resource.def", design(tracks, components, pins, nets)), lef,
                      DefGridMapping::OFFICIAL_FULL), "parse DEF resource fixture");
}
void test_union_and_unconnected_pins() {
    LefParser lef;
    DefParser def;
    parse(lef, def,
        "MACRO block SIZE 300 BY 300 ; "
        "OBS LAYER metal1 ; RECT 0 0 100 60 ; END "
        "PIN a PORT LAYER metal1 ; RECT 0 40 100 80 ; END END a "
        "PIN unconnected PORT LAYER metal1 ; RECT 100 10 200 30 ; END END unconnected "
        "END block",
        "TRACKS Y 10 DO 15 STEP 20 LAYER metal1 ; ",
        "COMPONENTS 1 ; - u block + PLACED ( 0 0 ) N ; END COMPONENTS ",
        "PINS 1 ; - io + LAYER metal1 ( 0 0 ) ( 100 20 ) "
        "+ FIXED ( 0 70 ) N ; END PINS ",
        "NETS 1 ; - n ( u a ) ( PIN io ) ; END NETS");
    const Resources resources(def, lef);
    require(resources.capacity(resources.node(0, 0, 1)) == 5, "five physical Y tracks in H cell");
    require(resources.fixed_demand(resources.node(0, 0, 1)) == 5,
            "OBS, macro pin and IO tracks use union, including shape high boundary");
    require(resources.fixed_demand(resources.node(1, 0, 1)) == 2,
            "unconnected macro pins remain fixed demand");
    require(resources.fixed_demand(resources.node(2, 0, 1)) == 0,
            "a shape ending on a grid boundary does not occupy the next cell");
    require(resources.pin_nodes(def.nets().front()) == std::vector<int>{resources.node(0, 0, 1)},
            "only this net's pins contribute to its sorted unique mask");
}
void test_tracks_clipped_to_cells() {
    LefParser lef;
    DefParser def;
    parse(lef, def,
        "MACRO block SIZE 300 BY 300 ; OBS LAYER metal1 ; "
        "RECT 0 101 100 109 ; RECT 100 90 200 110 ; "
        "RECT 200 300 300 310 ; END END block",
        "TRACKS Y -50 DO 9 STEP 50 LAYER metal1 ; "
        "TRACKS X -50 DO 9 STEP 50 LAYER metal2 ; ",
        "COMPONENTS 1 ; - u block + PLACED ( 0 0 ) N ; END COMPONENTS ",
        "PINS 1 ; - io + LAYER metal2 ( -1 -1 ) ( 1 1 ) + FIXED ( 20 20 ) N ; END PINS ",
        "NETS 1 ; - n ( PIN io ) ; END NETS");
    const Resources resources(def, lef);
    require(resources.capacity(resources.node(0, 0, 1)) == 2 &&
            resources.capacity(resources.node(0, 1, 1)) == 2 &&
            resources.capacity(resources.node(0, 2, 1)) == 3,
            "track capacity uses half-open bands, a closed final band, and no outside tracks");
    require(resources.capacity(resources.node(0, 0, 2)) == 2 &&
            resources.capacity(resources.node(2, 0, 2)) == 3,
            "vertical layers use physical X tracks and closed last column");
    require(resources.fixed_demand(resources.node(0, 1, 1)) == 0,
            "preceding track 100 is outside shape 101..109 and must not be charged");
    require(resources.fixed_demand(resources.node(1, 0, 1)) == 0 &&
            resources.fixed_demand(resources.node(1, 1, 1)) == 1,
            "track at 100 belongs only to row 1 although shape spans two rows");
    require(resources.fixed_demand(resources.node(2, 2, 1)) == 0,
            "an outside obstacle touching the upper grid boundary has no positive-area cell");
}
void test_duplicate_and_unsorted_patterns() {
    LefParser lef;
    DefParser def;
    parse(lef, def,
        "MACRO block SIZE 100 BY 100 ; OBS LAYER metal1 ; "
        "RECT 0 10 100 10.1 ; END END block",
        "TRACKS Y 110 DO 1 STEP 20 LAYER metal1 ; "
        "TRACKS Y 10 DO 2 STEP 20 LAYER metal1 ; "
        "TRACKS Y 10 DO 1 STEP 20 LAYER metal1 ; ",
        "COMPONENTS 1 ; - u block + PLACED ( 0 0 ) N ; END COMPONENTS ",
        "PINS 1 ; - io + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; END PINS ",
        "NETS 1 ; - n ( PIN io ) ; END NETS");
    const Resources repeated(def, lef);
    const Resources unique(def, lef, {true, true});
    const int cell = repeated.node(0, 0, 1);
    require(repeated.capacity(cell) == 3 && repeated.fixed_demand(cell) == 2,
            "default repeated declarations retain distinct track identities after sorting");
    require(unique.capacity(cell) == 2 && unique.fixed_demand(cell) == 1,
            "deduplicate option applies consistently to capacity and occupied tracks");
    require(repeated.capacity(repeated.node(0, 1, 1)) == 1,
            "multiple unsorted declarations use actual coordinates");
}
void test_io_orientation_and_masks() {
    LefParser lef;
    DefParser def;
    parse(lef, def, "",
        "TRACKS Y 5 DO 30 STEP 10 LAYER metal1 ; ",
        "COMPONENTS 0 ; END COMPONENTS ",
        "PINS 2 ; - rotated + LAYER metal1 ( 0 0 ) ( 120 20 ) + FIXED ( 150 50 ) W ; "
        "- unconnected + LAYER metal1 ( -5 -5 ) ( 5 5 ) + FIXED ( 250 255 ) N ; END PINS ",
        "NETS 1 ; - n ( PIN rotated ) ; END NETS");
    const Resources resources(def, lef);
    require(resources.fixed_demand(resources.node(1, 0, 1)) == 5 &&
            resources.fixed_demand(resources.node(1, 1, 1)) == 7,
            "rotated IO geometry charges tracks in all covered cells");
    require(resources.fixed_demand(resources.node(2, 2, 1)) == 1,
            "unconnected DEF IO contributes fixed demand");
    require(resources.pin_nodes(def.nets().front()) ==
            std::vector<int>{resources.node(1, 0, 1), resources.node(1, 1, 1)},
            "pin mask covers the whole rotated pin and excludes other IO pins");
}
void test_multilayer_pin_policy_and_node_encoding() {
    LefParser lef;
    DefParser def;
    parse(lef, def,
        "MACRO block SIZE 300 BY 300 ; PIN a PORT "
        "LAYER metal1 ; RECT 10 10 20 20 ; RECT 90 30 110 40 ; "
        "LAYER metal2 ; RECT 210 210 220 220 ; END END a END block",
        "TRACKS Y 15 DO 3 STEP 100 LAYER metal1 ; "
        "TRACKS X 15 DO 3 STEP 100 LAYER metal2 ; ",
        "COMPONENTS 1 ; - u block + PLACED ( 0 0 ) N ; END COMPONENTS ",
        "PINS 0 ; END PINS ",
        "NETS 1 ; - n ( u a ) ; END NETS");
    const Resources first(def, lef);
    const Resources actual(def, lef, {false, false});
    require(first.size() == 180 && first.node(2, 2, 20) == 179 &&
            first.node(3, 0, 1) == -1 && first.node(0, 0, 21) == -1,
            "node indexing and bounds match RoutingGraph");
    const int id = first.node(2, 1, 12);
    require(first.col(id) == 2 && first.row(id) == 1 && first.layer(id) == 12,
            "resource node decodes cell and layer");
    require(first.pin_nodes(def.nets().front()) == std::vector<int>{
                first.node(0, 0, 1), first.node(1, 0, 1), first.node(2, 2, 1)},
            "default first-layer policy applies to every PORT rectangle");
    require(actual.pin_nodes(def.nets().front()) == std::vector<int>{
                actual.node(0, 0, 1), actual.node(1, 0, 1), actual.node(2, 2, 2)},
            "actual-layer policy preserves multilayer masks and geometry spanning cells");
    require(first.fixed_demand(first.node(2, 2, 1)) == 1 &&
            first.fixed_demand(first.node(2, 2, 2)) == 0 &&
            actual.fixed_demand(actual.node(2, 2, 1)) == 0 &&
            actual.fixed_demand(actual.node(2, 2, 2)) == 1,
            "pin fixed demand and pin mask always use the same layer policy");
    bool rejected = false;
    try { (void)first.capacity(-1); }
    catch (const std::out_of_range&) { rejected = true; }
    require(rejected, "invalid node queries fail explicitly");
}
void test_platform_pin_views_and_original_io_layers() {
    LefParser lef;
    DefParser def;
    parse(lef, def,
        "MACRO block SIZE 300 BY 300 ; PIN a PORT "
        "LAYER metal1 ; RECT 10 10 20 20 ; RECT 90 30 110 40 ; "
        "LAYER metal2 ; RECT 210 210 220 220 ; END END a END block",
        "TRACKS Y 15 DO 3 STEP 100 LAYER metal1 ; "
        "TRACKS X 15 DO 3 STEP 100 LAYER metal2 ; ",
        "COMPONENTS 1 ; - u block + PLACED ( 0 0 ) N ; END COMPONENTS ",
        "PINS 1 ; - io + LAYER metal1 ( 10 110 ) ( 20 120 ) "
        "+ LAYER metal2 ( 110 10 ) ( 120 20 ) + FIXED ( 0 0 ) N ; END PINS ",
        "NETS 2 ; - n ( u a ) ; - io_net ( PIN io ) ; END NETS");
    const Resources platform(def, lef, {true, false, true});
    require(platform.fixed_demand(platform.node(2, 2, 1)) == 1 &&
            platform.fixed_demand(platform.node(2, 2, 2)) == 0,
            "platform projects every macro PIN shape to its first geometry layer");
    require(platform.pin_nodes(def.nets()[0]) == std::vector<int>{
                platform.node(0, 0, 1), platform.node(1, 0, 1)},
            "projected macro shapes never become additional own-net credit");
    require(platform.fixed_demand(platform.node(1, 0, 2)) == 1 &&
            platform.fixed_demand(platform.node(1, 0, 1)) == 0,
            "platform fixed IO shapes retain their original layers");
    require(platform.pin_nodes(def.nets()[1]) == std::vector<int>{
                platform.node(0, 1, 1), platform.node(1, 0, 2)},
            "platform IO exemption retains every original PORT layer");
    bool rejected = false;
    try { Resources inconsistent(def, lef, {false, false, true}); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "platform profile rejects contradictory shape projection override");
}
void test_platform_half_open_shape_tracks() {
    LefParser lef;
    DefParser def;
    parse(lef, def,
        "MACRO block SIZE 300 BY 300 ; OBS "
        "LAYER metal1 ; RECT 0 10 100 30 ; RECT 200 280 300 300 ; RECT 100 290 200 310 ; "
        "LAYER metal2 ; RECT 10 100 30 200 ; RECT 280 200 300 300 ; RECT 290 100 310 200 ; END "
        "PIN a PORT LAYER metal3 ; RECT 150 150 160 160 ; END END a END block",
        "TRACKS Y 0 DO 31 STEP 10 LAYER metal1 ; "
        "TRACKS X 0 DO 31 STEP 10 LAYER metal2 ; ",
        "COMPONENTS 1 ; - u block + PLACED ( 0 0 ) N ; END COMPONENTS ",
        "PINS 0 ; END PINS ", "NETS 1 ; - n ( u a ) ; END NETS");
    const Resources historical(def, lef), platform(def, lef, {true, false, true});
    for (const int node : {platform.node(0, 0, 1), platform.node(2, 2, 1),
                          platform.node(0, 1, 2), platform.node(2, 2, 2)}) {
        require(historical.fixed_demand(node) == 3 && platform.fixed_demand(node) == 2,
                "platform excludes upper-edge track in H/V and final exterior cells");
        require(historical.capacity(node) == platform.capacity(node),
                "half-open fixed shapes do not change cell capacity");
    }
    require(platform.fixed_demand(platform.node(1, 2, 1)) == 2 &&
            platform.fixed_demand(platform.node(2, 1, 2)) == 2,
            "track at final grid boundary stays occupied when interior to a shape extending beyond grid");
    require(platform.capacity(platform.node(2, 2, 1)) == 11,
            "platform capacity still includes the final exterior grid track");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: compat_resources_tests source_root output_directory");
        work = argv[2];
        fs::create_directories(work);
        test_union_and_unconnected_pins();
        test_tracks_clipped_to_cells();
        test_duplicate_and_unsorted_patterns();
        test_io_orientation_and_masks();
        test_multilayer_pin_policy_and_node_encoding();
        test_platform_pin_views_and_original_io_layers();
        test_platform_half_open_shape_tracks();
        std::cout << "Compatibility resource tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Compatibility resource tests failed: " << error.what() << "\n";
        return 1;
    }
}
