#include "scoring/compat_validation.hpp"
#include "validation/guide_validator.hpp"

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
std::string write(const fs::path& directory, const std::string& name, const std::string& content) {
    const auto path = directory / name;
    std::ofstream out(path);
    out << content;
    out.close();
    require(bool(out), "write validator fixture");
    return path.string();
}
std::string technology() {
    std::string result;
    for (int layer = 1; layer <= 20; ++layer) {
        const auto name = get_layer_name(layer);
        result += "LAYER " + name + " TYPE ROUTING ; DIRECTION " +
                  (layer % 2 ? "HORIZONTAL" : "VERTICAL") +
                  " ; WIDTH 0.2 ; END " + name + "\n";
    }
    return result;
}
std::string design() {
    return "UNITS DISTANCE MICRONS 1 ; DIEAREA ( 0 0 ) ( 100 100 ) ; "
           "GCELLGRID X 0 DO 11 STEP 10 ; GCELLGRID X 5 DO 2 STEP 10 ; "
           "GCELLGRID Y 0 DO 11 STEP 10 ; COMPONENTS 0 ; END COMPONENTS "
           "PINS 4 ; - a + LAYER metal1 ( -2 -2 ) ( 2 2 ) + FIXED ( 5 5 ) N ; "
           "- b + LAYER metal1 ( -2 -2 ) ( 2 2 ) + FIXED ( 25 5 ) N ; "
           "- lower + LAYER metal10 ( -2 -2 ) ( 2 2 ) + FIXED ( 45 5 ) N ; "
           "- upper + LAYER metal11 ( -2 -2 ) ( 2 2 ) + FIXED ( 45 5 ) N ; "
           "END PINS NETS 2 ; - local ( PIN a ) ( PIN b ) ; "
           "- cross ( PIN lower ) ( PIN upper ) ; END NETS END DESIGN";
}
GuideMap valid_guides() {
    return {{"local", {{0, 0, 30, 10, "metal1"}}},
            {"cross", {{40, 0, 50, 10, "metal10"}, {40, 0, 50, 10, "metal11"}}}};
}
const Net& named(const DefParser& def, const std::string& name) {
    for (const auto& net : def.nets()) if (net.name == name) return net;
    throw std::runtime_error("fixture net missing");
}
void equivalent(const DefParser& def, const LefParser& lef, const std::string& name,
                const std::vector<GuideRect>& guides, bool expected, const std::string& reason) {
    const auto& net = named(def, name);
    const auto single = compat::validate_net(net, def.die_area(), lef, guides,
                                             def.gcell_pitch_x(), def.gcell_pitch_y());
    auto all = valid_guides();
    all.at(name) = guides;
    const auto whole = compat::validate(def, lef, all);
    std::string error = "stale diagnostic";
    const bool wrapper = validate_net_guides(net, def.die_area(), lef, guides, error,
                                             def.gcell_pitch_x(), def.gcell_pitch_y());
    require((single.error_count == 0) == expected && wrapper == expected, reason);
    require(single.error_count == whole.error_count && single.errors == whole.errors,
            "single and full evaluator diagnostics agree: " + reason);
    require(single.checked_nets == 1 && single.checked_pins == net.pins.size() &&
            single.checked_rectangles == guides.size(), "single-net accounting: " + reason);
    require(error == (single.errors.empty() ? std::string{} : single.errors.front()),
            "boolean wrapper returns first error or clears stale error: " + reason);
}
void tests(const DefParser& def, const LefParser& lef) {
    const auto all = valid_guides();
    require(compat::validate(def, lef, all).error_count == 0, "valid full-design fixture");
    require(def.gcell_grid().x_lines[1] - def.gcell_grid().x_lines[0] == 5 &&
            def.gcell_pitch_x() == 10, "nonuniform grid preserves original first STEP");
    equivalent(def, lef, "local", all.at("local"), true, "valid single-net route");
    equivalent(def, lef, "cross", all.at("cross"), true,
               "TSV width uses original STEP, not first nonuniform cell width");
    equivalent(def, lef, "local", {{0, 0, 10, 10, "metal1"}, {10, 0, 30, 10, "metal1"}},
               true, "horizontal X-edge contact");
    equivalent(def, lef, "local", {{0, 0, 30, 10, "metal1"}, {0, 10, 30, 20, "metal1"}},
               false, "horizontal Y-edge contact must fail");
    equivalent(def, lef, "local", {{0, 0, 30, 20, "metal1"}}, false,
               "horizontal cross section beyond one GCell");
    equivalent(def, lef, "local", {{0, 6, 30, 7, "metal1"}}, true,
               "positive PORT overlap without covering its center");
    equivalent(def, lef, "local", {{0, 7, 30, 8, "metal1"}}, false,
               "PORT edge-only overlap must fail");
    equivalent(def, lef, "local", {{-5, 0, 30, 10, "metal1"}}, true,
               "partly outside DIE remains permitted");
    equivalent(def, lef, "local", {{0, 0, 30, 10, "metal1"}, {0, 0, 30, 10, "metal3"}},
               false, "missing intermediate layer");
    equivalent(def, lef, "cross", {{40, 0, 50, 10, "metal10"}, {40, 10, 50, 20, "metal11"}},
               false, "cross-layer edge does not supply positive-area TSV");
    equivalent(def, lef, "local", {}, false, "empty candidate");
    equivalent(def, lef, "local", {{0, 0, 30, 10, "metal99"}}, false, "unknown layer");
    auto nonfinite = all.at("local");
    nonfinite.front().x2 = std::numeric_limits<double>::infinity();
    equivalent(def, lef, "local", nonfinite, false, "nonfinite raw geometry");

    const auto& net = named(def, "local");
    std::string error;
    require(validate_net_guides(net, def.die_area(), lef, {{0, 0, 30, 20, "metal1"}}, error),
            "old five-argument API retains old thick-rectangle behavior");
    require(!validate_net_guides(net, def.die_area(), lef, {{0, 6, 30, 7, "metal1"}}, error),
            "old five-argument API retains old center-coverage behavior");
    require(!validate_net_guides(net, def.die_area(), lef, {{-5, 0, 30, 10, "metal1"}}, error),
            "old five-argument API retains full-DIE containment behavior");
    require(compat::validate_net(net, def.die_area(), lef, {{0, 0, 30, 20, "metal1"}},
                                 10, 10, true).error_count == 0,
            "single-net scorer legacy-direction mode remains selectable");
    require(!validate_net_guides(net, def.die_area(), lef, all.at("local"), error, 0, 10) &&
            error.find("pitch") != std::string::npos, "invalid explicit pitch fails");
    const auto incomplete = compat::validate(def, lef, {{"local", all.at("local")}});
    require(incomplete.error_count != 0 &&
            validate_net_guides(net, def.die_area(), lef, all.at("local"), error, 10, 10),
            "candidate check is independent of unrelated missing nets");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: compat_net_validation_tests output_directory");
        const fs::path work = argv[1];
        fs::create_directories(work);
        LefParser lef;
        require(lef.parse(write(work, "fixture.lef", technology())), "parse LEF fixture");
        DefParser def;
        require(def.parse(write(work, "fixture.def", design()), lef, DefGridMapping::OFFICIAL_FULL),
                "parse DEF fixture");
        tests(def, lef);
        std::cout << "Compatible single-net validation passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Compatible single-net validation failed: " << error.what() << '\n';
        return 1;
    }
}
