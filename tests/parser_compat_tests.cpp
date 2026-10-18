#include "parser/lef_parser.hpp"
#include "parser/thermal_parser.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace eda;
namespace {
fs::path work;
size_t checks = 0;
void require(bool value, const std::string& reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
std::string file(const std::string& name, const std::string& content) {
    const auto path = work / name;
    std::ofstream out(path); out << content; out.close();
    if (!out) throw std::runtime_error("cannot write parser fixture");
    return path.string();
}
void test_lef_compatibility() {
    const std::string lef_text =
        "VIA stack DEFAULT TOPOFSTACKONLY "
        "LAYER metal1 ; RECT 2 3 -2 -3 ; "
        "LAYER cut1 ; RECT 0.5 0.5 -0.5 -0.5 ; "
        "LAYER metal2 ; RECT 2 3 -2 -3 ; END stack "
        "MACRO cell SIZE 10 BY 10 ; "
        "PIN A PORT LAYER metal1 ; RECT 2.59 0.40 3.67 0.36 ; END END A "
        "OBS LAYER metal1 ; RECT 4 6 2 3 ; END END cell";
    LefParser lef;
    require(lef.parse(file("reverse.lef", lef_text)), "nonzero reversed LEF corners accepted");
    const auto* via = lef.get_via("stack");
    require(via && via->is_default && via->top_of_stack_only, "via qualifier is retained");
    require(via->shapes.front().rect.x1 == -2 && via->shapes.front().rect.y1 == -3,
            "via corners normalized");
    const auto* macro = lef.get_macro("cell");
    const auto& pin = macro->pins.at("A").bbox;
    require(pin.y1 == 0.36 && pin.y2 == 0.40 && pin.width() > 0, "pin corners normalized");
    require(macro->obstructions.front().rect.x1 == 2 && macro->obstructions.front().rect.y1 == 3,
            "OBS corners normalized");
    for (const auto& geometry : {std::string("0 0 0 1"), std::string("0 0 1 0")}) {
        LefParser bad;
        require(!bad.parse(file("zero.lef", "VIA zero LAYER metal1 ; RECT " + geometry + " ; END zero")),
                "zero area via is rejected");
        require(!bad.parse(file("zero-pin.lef", "MACRO c SIZE 1 BY 1 ; PIN A PORT LAYER metal1 ; RECT " +
                geometry + " ; END END A END c")), "zero area pin is rejected");
    }
    require(!lef.parse(file("duplicate-top.lef", "VIA duplicate TOPOFSTACKONLY TOPOFSTACKONLY "
                "LAYER metal1 ; RECT 0 0 1 1 ; END duplicate")), "duplicate qualifier rejected");
    require(lef.get_via("stack") && !lef.get_via("duplicate"), "LEF merge remains atomic");
}

void test_sparse_thermal() {
    const GCellGrid grid{{100, 110, 120, 130, 140}, {200, 210, 220, 230, 240}, 4, 4};
    const Rect die{100, 200, 140, 240};
    // T(x,y)=10+2x+3y+xy in microns; Z means at the four corners are
    // 10,18,22,46. Bilinear interpolation reproduces this field exactly.
    const std::string json = R"({"meta":{"ambientTemp":25},"nodes":[
        [0,0,0,4],[0,0,1,16],[4,0,0,12],[4,0,1,24],
        [0,4,0,16],[0,4,1,28],[4,4,0,40],[4,4,1,52]]})";
    ThermalParser thermal;
    require(thermal.parse_json(file("sparse.json", json), grid, 10, die), "regular sparse thermal mesh");
    require(thermal.interpolated_cells() == 12, "exact missing-cell count");
    require(std::string(thermal.mapping_mode()) == "direct-cell-average+bilinear-missing", "sparse mode recorded");
    require(thermal.get_temperature(0, 0) == 10 && thermal.get_temperature(3, 3) == 46,
            "sampled-cell means are preserved instead of interpolating them");
    require(std::abs(thermal.get_temperature(1, 2) - 24.25) < 1e-12,
            "bilinear values with DBU and nonzero origin");
    require(std::abs(thermal.get_temperature(2, 1) - 23.25) < 1e-12,
            "bilinear asymmetric interior value");
    require(thermal.get_temperature(0, 3) == 22 && thermal.get_temperature(3, 0) == 18,
            "exact mesh upper endpoints assigned to final GCells");

    // Centers on the mesh endpoints exercise weights 0 and 1, including the
    // upper-bound bracket where upper_bound returns axis.end().
    const GCellGrid edge_grid{{-1, 1, 3, 5}, {-1, 1, 3, 5}, 3, 3};
    ThermalParser edge;
    require(edge.parse_json(file("edges.json", json), edge_grid, 1, {0, 0, 4, 4}),
            "mesh endpoint interpolation succeeds");
    require(edge.get_temperature(0, 1) == 16 && edge.get_temperature(2, 1) == 32 &&
                edge.get_temperature(1, 0) == 14 && edge.get_temperature(1, 2) == 34,
            "all four edge interpolation values are exact");

    for (const auto& bad : {
        std::string(R"({"meta":{"ambientTemp":25},"nodes":[[0,0,0,10],[4,0,0,18],[0,4,0,22]]})"),
        std::string(R"({"meta":{"ambientTemp":25},"nodes":[[0,0,0,10],[1,0,0,12],[4,0,0,18],[0,4,0,22],[1,4,0,28],[4,4,0,46]]})"),
        std::string(R"({"meta":{"ambientTemp":25},"nodes":[[0,0,0,10],[0,4,0,22]]})"),
        std::string(R"({"meta":{"ambientTemp":25},"nodes":[[0,0,0,10],[2,0,0,14],[0,2,0,16],[2,2,0,24]]})"),
        std::string(R"({"meta":{"ambientTemp":25},"nodes":[]})")}) {
        require(!thermal.parse_json(file("invalid-sparse.json", bad), grid, 10, die),
                "incomplete, nonuniform, degenerate, uncovered, or empty mesh fails");
        require(thermal.hotspots().empty() && thermal.interpolated_cells() == 0 &&
                    thermal.get_temperature(0, 0) == 25,
                "failed thermal parse exposes no partial results");
    }
}

void test_dense_thermal_unchanged() {
    const GCellGrid grid{{-10, 0, 10}, {20, 30}, 2, 1};
    const Rect die{-10, 20, 10, 30};
    ThermalParser thermal;
    require(thermal.parse_json(file("dense.json", R"({"meta":{"ambientTemp":25},"nodes":[
        [0.5,0.5,0,40],[0.5,0.5,1,80],[0.6,0.5,0,30],[1.5,0.5,0,55]]})"), grid, 10, die),
        "covered dense data does not require a regular mesh");
    require(thermal.get_temperature(0, 0) == 45 && thermal.get_temperature(1, 0) == 55,
            "Z-first and then XY mean preserved exactly");
    require(thermal.interpolated_cells() == 0 && std::string(thermal.mapping_mode()) == "direct-cell-average",
            "dense data retains original mode");
}

void test_rounded_uniform_mesh() {
    const GCellGrid grid{{0, 1, 2, 3, 4, 5, 6}, {0, 1, 2}, 6, 2};
    ThermalParser thermal;
    require(thermal.parse_json(file("rounded.json", R"({"meta":{"ambientTemp":25},"nodes":[
        [0,0,0,10],[2.000000333,0,0,12],[3.999999667,0,0,14],[6,0,0,16],
        [0,2,0,20],[2.000000333,2,0,22],[3.999999667,2,0,24],[6,2,0,26]]})"),
        grid, 1, {0, 0, 6, 2}), "uniform mesh tolerates sub-micro-unit coordinate rounding");
    require(thermal.interpolated_cells() == 4, "rounded mesh missing count");
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: parser_compat_tests source_root output_directory");
        work = argv[2]; fs::create_directories(work);
        test_lef_compatibility(); test_sparse_thermal(); test_dense_thermal_unchanged(); test_rounded_uniform_mesh();
        std::cout << "Parser compatibility tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Parser compatibility test failed: " << error.what() << "\n";
        return 1;
    }
}
