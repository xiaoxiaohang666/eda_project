#include "validation/gcell_encoding.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

using namespace eda;
namespace fs = std::filesystem;
namespace {
std::size_t checks{};

void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

GCellGrid regular(int cols, int rows, double pitch) {
    GCellGrid grid;
    grid.cols = cols;
    grid.rows = rows;
    for (int c = 0; c <= cols; ++c) grid.x_lines.push_back(c * pitch);
    for (int r = 0; r <= rows; ++r) grid.y_lines.push_back(r * pitch);
    return grid;
}

void check(const LefParser& lef, const GCellGrid& grid, const Rect& die,
           const std::vector<GuideRect>& guides, bool expected, const std::string& message) {
    Net net;
    net.name = "net1364";
    std::string error = "stale diagnostic";
    const bool result = validate_gcell_encoding(net, die, grid, lef, guides, error);
    require(result == expected, message + ": " + error);
    require(expected ? error.empty() : error.starts_with("net1364: GCELL_ENCODING:"),
            message + ": diagnostic is cleared or identifies the net and invariant");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("gcell_encoding_tests OUTPUT_DIRECTORY");
        const fs::path directory = argv[1];
        fs::create_directories(directory);
        const auto path = directory / "fixture.lef";
        std::ofstream out(path);
        out << "LAYER metal11 TYPE ROUTING ; DIRECTION VERTICAL ; WIDTH 0.05 ; END metal11\n"
               "LAYER metal12 TYPE ROUTING ; DIRECTION HORIZONTAL ; WIDTH 0.05 ; END metal12\n"
               "LAYER cut1 TYPE CUT ; END cut1\n";
        out.close();
        require(bool(out), "write LEF fixture");
        LefParser lef;
        require(lef.parse(path.string()), "parse LEF fixture");

        const auto grid = regular(10, 10, 10);
        const Rect die{0, 0, 100, 100};
        check(lef, grid, die, {{10, 15, 20, 85, "metal11"}}, true, "vertical exact cell width");
        check(lef, grid, die, {{15, 10, 85, 20, "metal12"}}, true, "horizontal exact cell height");
        check(lef, grid, die, {{12, 15, 18, 85, "metal11"}}, false, "vertical narrow strip");
        check(lef, grid, die, {{15, 12, 85, 18, "metal12"}}, false, "horizontal narrow strip");
        check(lef, grid, die, {{10, 15, 30, 85, "metal11"}}, false, "vertical wide strip");
        check(lef, grid, die, {{15, 10, 85, 30, "metal12"}}, false, "horizontal wide strip");
        check(lef, grid, die, {{11, 15, 21, 85, "metal11"}}, false, "vertical pitch-width shifted strip");
        check(lef, grid, die, {{15, 11, 85, 21, "metal12"}}, false, "horizontal pitch-height shifted strip");
        check(lef, grid, die, {{10, 10, 20, 20, "metal11"}}, true, "full-cell terminal cap");
        check(lef, grid, die, {{14, 14, 16, 16, "metal11"}}, false, "physical terminal cap");

        // Boundary clipping is an engineering convention. These tests do NOT
        // assert how the unpublished platform scorer treats boundary cells.
        const Rect clipped{3, 4, 97, 96};
        check(lef, grid, clipped, {{3, 15, 10, 85, "metal11"}}, true, "low-X clipped cell");
        check(lef, grid, clipped, {{90, 15, 97, 85, "metal11"}}, true, "high-X clipped cell");
        check(lef, grid, clipped, {{15, 4, 85, 10, "metal12"}}, true, "low-Y clipped cell");
        check(lef, grid, clipped, {{15, 90, 85, 96, "metal12"}}, true, "high-Y clipped cell");
        check(lef, grid, clipped, {{4, 15, 10, 85, "metal11"}}, false, "boundary proximity does not excuse a thin strip");
        check(lef, grid, clipped, {{0, 15, 10, 85, "metal11"}}, false, "unclipped outside-DIE cell");

        GCellGrid nonuniform;
        nonuniform.x_lines = {0, 10, 25, 40};
        nonuniform.y_lines = {0, 10, 30, 40};
        nonuniform.cols = nonuniform.rows = 3;
        const Rect small_die{0, 0, 40, 40};
        check(lef, nonuniform, small_die, {{10, 5, 25, 35, "metal11"}}, true, "actual nonuniform X width");
        check(lef, nonuniform, small_die, {{5, 10, 35, 30, "metal12"}}, true, "actual nonuniform Y height");
        check(lef, nonuniform, small_die, {{10, 5, 20, 35, "metal11"}}, false, "first pitch cannot replace local X width");
        check(lef, nonuniform, small_die, {{5, 10, 35, 20, "metal12"}}, false, "first pitch cannot replace local Y height");

        const auto actual = regular(255, 251, 3000);
        const Rect actual_die{0, 0, 764200, 751900};
        check(lef, actual, actual_die, {{151190, 432901, 151290, 433001, "metal11"}}, false,
              "platform case001 net1364 rejected rectangle");
        check(lef, actual, actual_die, {{150000, 432000, 153000, 435000, "metal11"}}, true,
              "case001 grid-native terminal rectangle");
        check(lef, actual, actual_die, {{151190, 432901, 235243, 433001, "metal12"}}, false,
              "case001 companion horizontal thin rectangle");

        check(lef, grid, die, {}, false, "empty net output");
        check(lef, grid, die, {{10, 10, 20, 20, "metal3"}}, false, "unknown routing layer");
        check(lef, grid, die, {{10, 10, 20, 20, "cut1"}}, false, "non-routing layer");
        check(lef, grid, die, {{10, 10, 10, 20, "metal11"}}, false, "zero area");
        GuideRect invalid{10, 10, 20, 20, "metal11"};
        invalid.x1 = std::numeric_limits<double>::quiet_NaN();
        check(lef, grid, die, {invalid}, false, "NaN coordinate");
        invalid = {10, 10, 20, 20, "metal11"};
        invalid.y2 = std::numeric_limits<double>::infinity();
        check(lef, grid, die, {invalid}, false, "infinite coordinate");
        auto broken = grid;
        broken.x_lines[2] = broken.x_lines[1];
        check(lef, broken, die, {{10, 10, 20, 20, "metal11"}}, false, "invalid grid");
        std::cout << "gcell_encoding_tests: " << checks << " engineering-invariant checks PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
