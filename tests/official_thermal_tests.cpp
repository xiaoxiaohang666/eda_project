#include "parser/thermal_parser.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace eda;
namespace fs = std::filesystem;
namespace {
fs::path work;
size_t checks = 0;
void require(bool passed, const std::string& reason) {
    ++checks;
    if (!passed) throw std::runtime_error(reason);
}
std::string fixture(const std::string& name, const std::string& text) {
    const auto path = work / name;
    std::ofstream file(path); file << text; file.close();
    if (!file) throw std::runtime_error("cannot save fixture");
    return path.string();
}
void test_preceding_sparse_cells() {
    const GCellGrid grid{{100, 110, 120, 130, 140}, {200, 210, 220, 230, 240}, 4, 4};
    const Rect die{100, 200, 140, 240};
    const auto json = fixture("corners.json", R"({"meta":{"ambientTemp":9},"nodes":[
        [0,0,0,4],[0,0,1,16],[4,0,0,12],[4,0,1,24],
        [0,4,0,16],[0,4,1,28],[4,4,0,40],[4,4,1,52]]})");
    ThermalParser heat;
    require(heat.parse_json(json, grid, 10, die, ThermalMapping::OFFICIAL_PREVIOUS), "official sparse fill succeeds");
    require(heat.filled_cells() == 12 && heat.interpolated_cells() == 0, "previous fills are not bilinear interpolation");
    require(heat.projected_nodes() == 4 && heat.mapped_nodes() == 16, "Z projection and synthetic node counts");
    require(heat.mapping_strategy() == ThermalMapping::OFFICIAL_PREVIOUS, "profile retained");
    require(std::string(heat.mapping_mode()) == "direct-cell-average+official-previous-missing", "profile mapping metadata");
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            require(heat.get_temperature(col,row) == (row == 3 ? (col == 3 ? 46 : 22) : (col == 3 ? 18 : 10)),
                    "previous global sampled row and col are used independently");
    require(heat.hotspots().size() == 16 && heat.hotspots()[0].temperature == 46, "hotspot sorting and ambient temperature");
    ThermalParser legacy;
    require(legacy.parse_json(json, grid, 10, die), "historical default remains enabled");
    require(std::abs(legacy.get_temperature(1,2) - 24.25) < 1e-12, "historical bilinear value remains exact");
    require(legacy.interpolated_cells() == 12 && legacy.filled_cells() == 12, "legacy interpolation metadata preserved");
}
void test_nonuniform_dense_and_sparse() {
    const GCellGrid grid{{0, 1, 2, 3, 4}, {0, 1}, 4, 1};
    ThermalParser heat;
    require(heat.parse_json(fixture("nonuniform.json", R"({"nodes":[
        [0.1,0.5,0,40],[0.1,0.5,1,80],[0.2,0.5,0,30],[2.8,0.5,0,55]]})"),
        grid, 1, {0,0,4,1}, ThermalMapping::OFFICIAL_PREVIOUS), "official does not require regular Cartesian mesh");
    require(heat.ambient_temperature() == 25, "missing meta receives official ambient default");
    require(heat.get_temperature(0,0) == 45 && heat.get_temperature(1,0) == 45 &&
            heat.get_temperature(2,0) == 55 && heat.get_temperature(3,0) == 55,
            "Z-first mean followed by equal-weight XY means and preceding fill");
    require(heat.projected_nodes() == 3 && heat.mapped_nodes() == 5 && heat.filled_cells() == 2,
            "multiple projected nodes in a cell retain their independent counts");
    require(heat.parse_json(fixture("dense-zero.json", R"({"meta":{},"nodes":[
        [0.5,0.5,0,0],[1.5,0.5,0,-2],[2.5,0.5,0,20],[3.5,0.5,0,30]]})"),
        grid, 1, {0,0,4,1}, ThermalMapping::OFFICIAL_PREVIOUS), "dense nonpositive cells do not need fill");
    require(heat.get_temperature(0,0) == 0 && heat.get_temperature(1,0) == -2 && heat.filled_cells() == 0,
            "dense nonpositive measurements preserved");
}
void test_clamped_positions_and_rounding() {
    const GCellGrid grid{{0, 1, 2}, {0, 1}, 2, 1};
    ThermalParser heat;
    require(heat.parse_json(fixture("clamp.json", R"({"nodes":[
        [-100,-100,0,10],[0,0,0,30],[1,0,0,60],[100,100,0,80]]})"),
        grid, 1, {0,0,2,1}, ThermalMapping::OFFICIAL_PREVIOUS), "outside positions map to official edge cells");
    require(heat.get_temperature(0,0) == 20 && heat.get_temperature(1,0) == 70 && heat.mapped_nodes() == 4,
            "internal line belongs to cell on right and exterior points are clamped");
    // Binary doubles on opposite sides of the half-decimal rounding boundary.
    require(heat.parse_json(fixture("rounding.json", R"({"nodes":[
        [0.9999999994,0,0,10],[0.9999999996,0,0,30],[2,0,0,50]]})"),
        grid, 1, {0,0,2,1}, ThermalMapping::OFFICIAL_PREVIOUS), "nine-place decimal coordinates");
    require(heat.get_temperature(0,0) == 10 && heat.get_temperature(1,0) == 40,
            "rounding occurs before GCell projection");
}
void test_unresolved_sources_fail_atomically() {
    const GCellGrid grid{{0,1,2,3}, {0,1,2,3}, 3,3};
    for (const auto& json : {
        std::string(R"({"nodes":[[0,0,0,10],[2,0,0,20],[0,2,0,30]]})"),
        std::string(R"({"nodes":[[2,2,0,40]]})"),
        std::string(R"({"nodes":[[0,0,0,0],[2,0,0,20],[0,2,0,30],[2,2,0,40]]})"),
        std::string(R"({"nodes":[[0,0,0,-1],[2,0,0,20],[0,2,0,30],[2,2,0,40]]})")}) {
        ThermalParser heat;
        require(!heat.parse_json(fixture("unresolved.json", json), grid, 1, {0,0,3,3},
                                 ThermalMapping::OFFICIAL_PREVIOUS),
                "unsampled global row-col intersections or nonpositive fill sources must fail");
        require(heat.filled_cells() == 0 && heat.interpolated_cells() == 0 && heat.mapped_nodes() == 0 &&
                heat.projected_nodes() == 0 && heat.hotspots().empty() && heat.get_temperature(0,0) == 25,
                "failed mapping exposes no partial thermal state");
    }
}
void test_original_bytecode_golden(const fs::path& path) {
    // Optional integration fixture emitted by the unmodified official pure
    // function code objects; no reconstructed Python implementation is used.
    std::ifstream file(path);
    std::string json;
    std::getline(file, json);
    GCellGrid grid;
    Rect die;
    double units;
    size_t mapped;
    file >> units >> die.x1 >> die.y1 >> die.x2 >> die.y2 >> grid.cols >> grid.rows >> mapped;
    grid.x_lines.resize(static_cast<size_t>(grid.cols + 1));
    grid.y_lines.resize(static_cast<size_t>(grid.rows + 1));
    for (auto& x : grid.x_lines) file >> x;
    for (auto& y : grid.y_lines) file >> y;
    ThermalParser heat;
    require(heat.parse_json(json, grid, units, die, ThermalMapping::OFFICIAL_PREVIOUS), "actual thermal JSON parses");
    require(heat.mapped_nodes() == mapped, "official mapped-node count matches");
    double max_difference = 0;
    for (int row = 0; row < grid.rows; ++row) {
        for (int col = 0; col < grid.cols; ++col) {
            double expected = 0;
            file >> expected;
            max_difference = std::max(max_difference, std::abs(expected - heat.get_temperature(col,row)));
        }
    }
    require(static_cast<bool>(file), "read complete original-bytecode golden matrix");
    require(max_difference <= 1e-12, "all actual GCell temperatures match original official bytecode");
    std::cout << "Original-bytecode parity: " << path.filename() << " cells=" << grid.cols * grid.rows
              << " fills=" << heat.filled_cells() << " mapped=" << heat.mapped_nodes()
              << " max_abs_error=" << std::setprecision(17) << max_difference << '\n';
}
}
int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("usage: official_thermal_tests source_root output_directory [official_golden_fixture ...]");
        work = argv[2]; fs::create_directories(work);
        test_preceding_sparse_cells(); test_nonuniform_dense_and_sparse();
        test_clamped_positions_and_rounding(); test_unresolved_sources_fail_atomically();
        for (int i = 3; i < argc; ++i) test_original_bytecode_golden(argv[i]);
        std::cout << "Official thermal tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Official thermal tests failed: " << error.what() << '\n';
        return 1;
    }
}
