#include "scoring/compat_validation.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

using namespace eda;
namespace fs = std::filesystem;
namespace {
std::size_t checks{};
fs::path work;

void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

std::string write(const std::string& name, const std::string& content) {
    const auto path = work / name;
    std::ofstream output(path);
    output << content;
    output.close();
    if (!output) throw std::runtime_error("cannot write fixture " + path.string());
    return path.string();
}

std::string technology() {
    std::string text;
    for (int layer = 1; layer <= 20; ++layer) {
        const auto name = get_layer_name(layer);
        text += "LAYER " + name + " TYPE ROUTING ; DIRECTION " +
                (layer % 2 ? "HORIZONTAL" : "VERTICAL") +
                " ; WIDTH 0.2 ; END " + name + "\n";
    }
    return text + "LAYER cut1 TYPE CUT ; END cut1\n";
}

DefParser design(const LefParser& lef, const std::vector<std::string>& pins,
                 const std::string& grid = "GCELLGRID X 0 DO 11 STEP 10 ; "
                                          "GCELLGRID Y 0 DO 11 STEP 10 ; ") {
    std::string content = "UNITS DISTANCE MICRONS 1 ; DIEAREA ( 0 0 ) ( 100 100 ) ; " +
        grid + "COMPONENTS 0 ; END COMPONENTS PINS " + std::to_string(pins.size()) + " ; ";
    std::string connections;
    for (std::size_t index = 0; index < pins.size(); ++index) {
        const auto name = "p" + std::to_string(index);
        content += "- " + name + " " + pins[index] + " ; ";
        connections += "( PIN " + name + " ) ";
    }
    content += "END PINS NETS 1 ; - n " + connections + "; END NETS END DESIGN";
    DefParser result;
    require(result.parse(write("fixture.def", content), lef, DefGridMapping::OFFICIAL_FULL),
            "parse fixture DEF");
    return result;
}

std::string pin(int layer, int x, int y) {
    return "+ LAYER " + get_layer_name(layer) + " ( -1 -1 ) ( 1 1 ) + FIXED ( " +
           std::to_string(x) + " " + std::to_string(y) + " ) N";
}

bool has_error(const compat::ValidationResult& result, const std::string& text) {
    for (const auto& error : result.errors) if (error.find(text) != std::string::npos) return true;
    return false;
}

compat::ValidationResult check(const DefParser& def, const LefParser& lef,
                              const std::vector<GuideRect>& rectangles, bool legacy = false) {
    return compat::validate(def, lef, {{"n", rectangles}}, legacy);
}

void direction_tests(const LefParser& lef) {
    const auto horizontal = design(lef, {pin(1, 5, 5)});
    const std::vector<GuideRect> horizontal_edges{{0, 0, 10, 10, "metal1"},
                                                 {10, 0, 20, 10, "metal1"}};
    const auto valid = check(horizontal, lef, horizontal_edges);
    require(valid.error_count == 0 && valid.checked_nets == 1 &&
            valid.checked_pins == 1 && valid.checked_rectangles == 2,
            "horizontal X-edge connection at exact GCell thickness");
    const std::vector<GuideRect> horizontal_wrong{{0, 0, 10, 10, "metal1"},
                                                 {0, 10, 10, 20, "metal1"}};
    require(has_error(check(horizontal, lef, horizontal_wrong), "disconnected"),
            "horizontal Y-edge contact does not connect");
    require(check(horizontal, lef, horizontal_wrong, true).error_count == 0,
            "legacy horizontal Y-edge connection is retained");
    require(has_error(check(horizontal, lef, {{0, 0, 10, 10, "metal1"},
                                              {10, 10, 20, 20, "metal1"}}), "disconnected"),
            "same-layer corner contact does not connect");
    require(check(horizontal, lef, {{0, 0, 10, 10, "metal1"},
                                    {2, 2, 8, 8, "metal1"}}).error_count == 0,
            "contained guide rectangles connect");
    require(has_error(check(horizontal, lef, {{0, 0, 10, 10.01, "metal1"}}), "exceeds one GCell"),
            "horizontal transverse thickness beyond pitch rejected");
    require(check(horizontal, lef, {{0, 0, 10, 20, "metal1"}}, true).error_count == 0,
            "legacy accepts thick horizontal rectangle");
    require(check(horizontal, lef, {{0.5, 0.5, 9.5, 9.5, "metal1"}}).error_count == 0,
            "finite decimal guide coordinates have no invented integer restriction");

    const auto vertical = design(lef, {pin(2, 5, 5)});
    require(check(vertical, lef, {{0, 0, 10, 10, "metal2"},
                                  {0, 10, 10, 20, "metal2"}}).error_count == 0,
            "vertical Y-edge connection");
    require(has_error(check(vertical, lef, {{0, 0, 10, 10, "metal2"},
                                            {10, 0, 20, 10, "metal2"}}), "disconnected"),
            "vertical X-edge contact does not connect");
    require(has_error(check(vertical, lef, {{0, 0, 10.01, 10, "metal2"}}), "exceeds one GCell"),
            "vertical transverse thickness beyond pitch rejected");
    require(check(horizontal, lef, {{0, 0, 10, 10, "metal1"},
                                    {5, 5, 15, 15, "metal2"}}).error_count == 0,
            "adjacent layers with positive area connect");
    require(has_error(check(horizontal, lef, {{0, 0, 10, 10, "metal1"},
                                              {10, 0, 20, 10, "metal2"}}), "disconnected"),
            "adjacent layers with only edge contact do not connect");
    require(has_error(check(horizontal, lef, {{0, 0, 10, 10, "metal1"},
                                              {0, 0, 10, 10, "metal3"}}), "disconnected"),
            "nonadjacent layers do not connect through a missing via layer");
}

void coverage_tests(const LefParser& lef) {
    const auto def = design(lef, {"+ LAYER metal1 ( 4 4 ) ( 8 8 ) "
                                  "+ LAYER metal3 ( 20 20 ) ( 24 24 ) + FIXED ( 0 0 ) N"});
    require(check(def, lef, {{0, 0, 5, 5, "metal1"}}).error_count == 0,
            "PORT positive overlap suffices without covering pin center");
    require(check(def, lef, {{20, 20, 25, 25, "metal3"}}).error_count == 0,
            "any same-layer PORT can connect a multilayer pin");
    require(has_error(check(def, lef, {{0, 0, 4, 4, "metal1"}}), "same-layer PORT"),
            "corner-touching PORT is uncovered");
    require(has_error(check(def, lef, {{0, 4, 4, 8, "metal1"}}), "same-layer PORT"),
            "edge-touching PORT is uncovered");
    require(has_error(check(def, lef, {{4, 4, 8, 8, "metal2"}}), "same-layer PORT"),
            "positive overlap on another layer does not cover the pin");
    const auto wide_pin = design(lef, {"+ LAYER metal1 ( 0 0 ) ( 30 5 ) + FIXED ( 0 0 ) N"});
    require(has_error(check(wide_pin, lef, {{0, 0, 5, 5, "metal1"},
                                           {20, 0, 25, 5, "metal1"}}), "disconnected"),
            "existing PORT metal does not bridge disconnected Guide rectangles");
}

void geometry_tests(const LefParser& lef) {
    const auto def = design(lef, {pin(1, 5, 5)});
    require(check(def, lef, {{-10, 0, 10, 10, "metal1"}}).error_count == 0,
            "partly out-of-DIE rectangle accepted when positive area remains");
    require(has_error(check(def, lef, {{-10, 0, 0, 10, "metal1"}}), "DIEAREA"),
            "DIE edge contact alone is outside");
    require(has_error(check(def, lef, {{0, 0, 10, 10, "cut1"}}), "non-routing"),
            "declared CUT is not a valid routing layer");
    require(has_error(check(def, lef, {{0, 0, 10, 10, "metal21"}}), "non-routing"),
            "layer outside 1..20 rejected");
    require(check(def, lef, {{0, 0, 10, 10, "Metal1"}}).error_count == 0,
            "Guide layer case aliases normalize to declared routing layer");
    require(has_error(check(def, lef, {{0, 0, 0, 10, "metal1"}}), "non-positive"),
            "zero-area rectangle rejected");
    auto reversed = GuideRect{0, 0, 10, 10, "metal1"};
    reversed.x1 = 20;
    require(has_error(check(def, lef, {reversed}), "non-positive"),
            "reversed raw coordinates are rejected");
    for (const auto value : {std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
        auto bad = GuideRect{0, 0, 10, 10, "metal1"};
        bad.x2 = value;
        require(has_error(check(def, lef, {bad}), "non-finite"), "nonfinite coordinate rejected");
    }
    require(has_error(check(def, lef, {}), "missing or empty"), "empty expected net rejected");
    require(has_error(compat::validate(def, lef, {}), "missing or empty"), "missing expected net rejected");
    GuideMap extra{{"n", {{0, 0, 10, 10, "metal1"}}}};
    for (int index = 0; index < 60; ++index) extra["unknown" + std::to_string(index)] = {};
    const auto errors = compat::validate(def, lef, extra);
    require(errors.error_count == 60 && errors.errors.size() == 50,
            "full error count with bounded diagnostic storage");
}

void die_tests(const LefParser& lef) {
    const auto bottom = design(lef, {pin(10, 5, 5)});
    require(has_error(check(bottom, lef, {{0, 0, 10, 10, "metal10"},
                                          {0, 0, 10, 10, "metal11"}}), "other die"),
            "bottom-only net cannot route through top die");
    const auto top = design(lef, {pin(11, 5, 5)});
    require(has_error(check(top, lef, {{0, 0, 10, 10, "metal10"},
                                       {0, 0, 10, 10, "metal11"}}), "other die"),
            "top-only net cannot route through bottom die");
    const auto cross = design(lef, {pin(10, 5, 5), pin(11, 5, 5)});
    require(check(cross, lef, {{0, 0, 10, 10, "metal10"},
                               {0, 0, 10, 10, "metal11"}}).error_count == 0,
            "cross-die net with positive metal10/metal11 overlap");
    const auto shifted = design(lef, {pin(10, 5, 5), pin(11, 15, 5)});
    require(has_error(check(shifted, lef, {{0, 0, 10, 10, "metal10"},
                                          {10, 0, 20, 10, "metal11"}}), "metal10/metal11"),
            "cross-die edge contact is not a TSV");
}

void reader_tests() {
    const auto valid = compat::read_guides(write("valid.guide", "n\n(\n0 1.5 10 11 Metal1\n)\n"));
    require(valid.at("n").front().layer == "metal1" && valid.at("n").front().y1 == 1.5,
            "strict reader accepts decimal tokens and normalizes layer case");
    const auto reversed = compat::read_guides(write("reversed.guide", "n ( 10 0 0 10 metal1 )"));
    require(reversed.at("n").front().x1 == 10 && reversed.at("n").front().x2 == 0,
            "reader preserves reversed coordinates for legality diagnosis");
    std::vector<std::string> malformed{
        "", "n", "n 0 0 10 10 metal1", "n ( )", "n ( 0 0 10 10 metal1",
        "n ( 0 0 10 10 metal1 ) n ( 0 0 10 10 metal1 )",
        "n ( 0 0 10 10 metal1 ) trailing",
        "n ( 0 0 10 10 )", "n ( 0 0 10 10 metal1 extra )",
        "n ( 0 0 10 10 metal1 ) )", "n ( 0 0 10 1e9999 metal1 )"};
    for (int coordinate = 0; coordinate < 4; ++coordinate) {
        std::vector<std::string> tokens{"0", "0", "10", "10"};
        tokens[coordinate] += "suffix";
        malformed.push_back("n ( " + tokens[0] + " " + tokens[1] + " " +
                            tokens[2] + " " + tokens[3] + " metal1 )");
    }
    for (const auto& content : malformed) {
        bool rejected = false;
        try { (void)compat::read_guides(write("bad.guide", content)); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "malformed Guide must be rejected: " + content);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2 && argc != 3)
            throw std::runtime_error("usage: compat_validation_tests [source_root] output_directory");
        work = argv[argc - 1];
        fs::create_directories(work);
        LefParser lef;
        require(lef.parse(write("fixture.lef", technology())), "parse fixture LEF");
        direction_tests(lef);
        coverage_tests(lef);
        geometry_tests(lef);
        die_tests(lef);
        reader_tests();
        std::cout << "Scorer compatibility validation tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Scorer compatibility validation test failed: " << error.what() << '\n';
        return 1;
    }
}
