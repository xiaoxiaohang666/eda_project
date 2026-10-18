#include "objectives/guide_metrics.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

namespace fs = std::filesystem;
using namespace eda;
namespace {
fs::path work;
size_t checks = 0;
void require(bool value, const std::string& message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F operation, const std::string& message) {
    bool failed = false;
    try { operation(); } catch (const std::exception&) { failed = true; }
    require(failed, message);
}
std::string file(const std::string& name, const std::string& text) {
    const auto path = work / name;
    std::ofstream out(path); out << text; out.close();
    if (!out) throw std::runtime_error("cannot write cached metrics fixture");
    return path.string();
}
void equal_scalars(const GuideMetrics& a, const GuideMetrics& b) {
    require(a.overflow == b.overflow && a.max_overflow == b.max_overflow &&
            a.tsv_count == b.tsv_count && a.used_cells == b.used_cells, "integer metric parity");
    require(a.wirelength == b.wirelength && a.reference == b.reference &&
            a.normalized == b.normalized && a.correlation == b.correlation && a.quality == b.quality,
            "objective scalar parity");
}
void test_pearson_move_prediction() {
    const std::vector<double> concentration_heat{75,74,0,0};
    const PearsonState concentration(concentration_heat, {1,1,0,0});
    const auto original_counts = concentration.counts();
    const double original = concentration.correlation();
    const double concentrated = concentration.correlation_after_move(1,0);
    require(std::abs(original - 0.9999549600270011) < 1e-12 &&
            std::abs(concentrated - 0.5850735843470649) < 1e-12,
            "warmer same-cell concentration can lower the exact Pearson objective");
    require(concentrated < original &&
            concentration.counts() == original_counts && concentration.correlation() == original,
            "candidate prediction rejects concentration incentive without mutating state");
    require(concentration.correlation_after_move(0,0) == original,
            "same-cell move preserves correlation exactly");
    require(std::abs(concentration.correlation_after_move(0,3) -
            PearsonState(concentration_heat, {0,1,0,1}).correlation()) < 1e-12,
            "first-to-last grid cell prediction matches independent recomputation");
    rejects([&] { concentration.correlation_after_move(4,0); }, "source index past grid rejected");
    rejects([&] { concentration.correlation_after_move(0,4); }, "target index past grid rejected");
    rejects([&] { concentration.correlation_after_move(std::numeric_limits<size_t>::max(),0); },
            "wrapped negative source index rejected");
    rejects([&] { concentration.correlation_after_move(0,std::numeric_limits<size_t>::max()); },
            "wrapped negative target index rejected");
    rejects([&] { concentration.correlation_after_move(2,0); }, "empty source cell rejected");
    rejects([&] { concentration.correlation_after_move(2,2); }, "empty same-cell source rejected like move");
    require(concentration.counts() == original_counts && concentration.correlation() == original,
            "rejected prediction preserves all observable state");

    for (const std::vector<double>& heat : {std::vector<double>{0,0,0,0},
                                           std::vector<double>{1,1,1,1},
                                           std::vector<double>{4,2,1,0},
                                           std::vector<double>{1e-100,2e-100,0,4e-100}}) {
        const PearsonState state(heat, {1,1,1,1});
        require(state.correlation() == 0, "uniform counts have zero initial Pearson variance");
        require(std::abs(state.correlation_after_move(0,3) -
                PearsonState(heat, {0,1,1,2}).correlation()) < 1e-12,
                "zero variance and tiny positive heat follow the same evaluator threshold");
    }
    const PearsonState no_tsv({4,2,1,0}, {0,0,0,0});
    require(no_tsv.correlation() == 0, "empty TSV population has zero correlation");
    rejects([&] { no_tsv.correlation_after_move(0,1); }, "zero population cannot move a TSV");

    const std::vector<double> heat{0.001,100.125,3.25,0,11.75,5.125,98.5};
    PearsonState state(heat, {3,0,2,1,0,4,0});
    for (size_t step=0; step<64; ++step) {
        size_t from=step%heat.size(), to=(3*step+2)%heat.size();
        while (state.counts()[from] == 0) from=(from+1)%heat.size();
        const auto saved_counts=state.counts();
        const double saved_correlation=state.correlation();
        auto expected_counts=saved_counts; --expected_counts[from]; ++expected_counts[to];
        const double predicted=state.correlation_after_move(from,to);
        require(state.counts() == saved_counts && state.correlation() == saved_correlation,
                "repeated candidate predictions do not mutate incremental state");
        require(std::abs(predicted - PearsonState(heat,expected_counts).correlation()) < 1e-12,
                "prediction agrees with rebuilding all Pearson statistics");
        state.move(static_cast<int>(from),static_cast<int>(to));
        require(predicted == state.correlation() && state.counts() == expected_counts,
                "prediction matches committed incremental move exactly");
    }
}
void test_cache_aggregation() {
    std::string technology, tracks;
    for (int layer = 1; layer <= 20; ++layer) {
        const auto name = get_layer_name(layer);
        technology += "LAYER " + name + " TYPE ROUTING ; DIRECTION " +
            (layer % 2 ? "HORIZONTAL" : "VERTICAL") + " ; WIDTH 0.2 ; END " + name + "\n";
        tracks += "TRACKS X 10 DO 15 STEP 20 LAYER " + name + " ; "
                  "TRACKS Y 5 DO 30 STEP 10 LAYER " + name + " ; ";
    }
    const std::string design =
        "UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 300 300 ) ; "
        "GCELLGRID X 0 DO 4 STEP 100 ; GCELLGRID Y 0 DO 4 STEP 100 ; " + tracks +
        "COMPONENTS 0 ; END COMPONENTS PINS 2 ; "
        "- a + LAYER metal10 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; "
        "- b + LAYER metal11 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; END PINS "
        "NETS 1 ; - n ( PIN a ) ( PIN b ) ; END NETS END DESIGN";
    LefParser lef; DefParser def; ThermalParser thermal;
    require(lef.parse(file("metrics.lef", technology)), "fixture LEF");
    require(def.parse(file("metrics.def", design), lef), "fixture DEF");
    require(thermal.parse_json(file("metrics.json", R"({"meta":{"ambientTemp":25},"nodes":[
        [5,5,0,35],[15,5,0,40],[25,5,0,45],[5,15,0,30],[15,15,0,55],
        [25,15,0,40],[5,25,0,40],[15,25,0,35],[25,25,0,30]]})"),
        def.gcell_grid(), def.dbu_per_micron(), def.die_area()), "fixture heat");
    RoutingGraph graph(def, lef);
    const GuideMap guides{
        {"cross_a", {{45,45,55,55,"metal10"}, {45,45,55,55,"metal11"}}},
        {"cross_b", {{145,145,155,155,"metal10"}, {145,145,155,155,"metal11"}}},
        {"local", {{45,45,155,55,"metal1"}, {145,45,255,55,"metal1"}}}};
    std::vector<NetGuideMetrics> storage;
    for (const auto& [name, rects] : guides) {
        (void)name;
        storage.push_back(measure_net_guides(rects, graph, lef));
    }
    std::vector<const NetGuideMetrics*> cache;
    std::vector<int> demand(graph.size(), 0);
    for (const auto& one : storage) {
        cache.push_back(&one);
        for (const auto& [node, value] : one.demand) demand[node] += value;
    }
    const auto original = evaluate_guides(guides, graph, lef, thermal);
    const auto captured = aggregate_cached_metrics(cache, demand, graph, thermal);
    equal_scalars(original, captured);
    require(original.demand == captured.demand && captured.demand == demand, "captured ledger parity");
    const auto compact = aggregate_cached_metrics(cache, demand, graph, thermal, false);
    equal_scalars(original, compact);
    require(compact.demand.empty(), "large-run scalar metrics omit dense demand copy");

    auto perturbed = demand;
    const int changed_node = graph.node(2, 2, 2);
    require(perturbed[changed_node] == 0, "perturbation starts with an unused cell");
    perturbed[changed_node] = static_cast<int>(graph.guide_capacity(changed_node)) + 7;
    const auto changed = aggregate_cached_metrics(cache, perturbed, graph, thermal, false);
    require(changed.overflow == original.overflow + 7 && changed.max_overflow == 7 &&
            changed.used_cells == original.used_cells + 1, "aggregation reads caller ledger, not net demand again");
    require(changed.wirelength == original.wirelength && changed.reference == original.reference &&
            changed.correlation == original.correlation && changed.tsv_count == original.tsv_count,
            "ledger perturbation preserves unrelated objectives");
    require(std::abs(changed.quality - (original.quality - 0.30 * 7 / 20.0)) < 1e-12,
            "congestion quality changes by the exact expected amount");

    // A cache update for one changed net must match a complete remeasurement.
    auto modified_guides = guides;
    modified_guides["cross_b"] = {{245,245,255,255,"metal10"}, {245,245,255,255,"metal11"}};
    for (const auto& [node, value] : storage[1].demand) demand[node] -= value;
    storage[1] = measure_net_guides(modified_guides.at("cross_b"), graph, lef);
    for (const auto& [node, value] : storage[1].demand) demand[node] += value;
    const auto updated = aggregate_cached_metrics(cache, demand, graph, thermal, false);
    equal_scalars(updated, evaluate_guides(modified_guides, graph, lef, thermal));
    require(updated.correlation != original.correlation, "TSV relocation changes thermal correlation");

    rejects([&] { aggregate_cached_metrics(cache, {}, graph, thermal); }, "dimension mismatch rejected");
    auto negative = demand; negative[0] = -1;
    rejects([&] { aggregate_cached_metrics(cache, negative, graph, thermal); }, "negative ledger rejected");
    auto null_cache = cache; null_cache[0] = nullptr;
    rejects([&] { aggregate_cached_metrics(null_cache, demand, graph, thermal); }, "null cache rejected");
    storage[0].tsv_cells.push_back(def.gcell_grid().cols * def.gcell_grid().rows);
    rejects([&] { aggregate_cached_metrics(cache, demand, graph, thermal); }, "out-of-range cached TSV rejected");
    storage[0].tsv_cells.pop_back();
    storage[0].connected = false;
    rejects([&] { aggregate_cached_metrics(cache, demand, graph, thermal); }, "disconnected cache rejected");
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: cached_metrics_tests source_root output_directory");
        work = argv[2]; fs::create_directories(work);
        test_pearson_move_prediction();
        test_cache_aggregation();
        std::cout << "Cached metrics tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Cached metrics tests failed: " << e.what() << "\n";
        return 1;
    }
}
