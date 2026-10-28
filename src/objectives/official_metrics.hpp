#pragma once

#include "objectives/guide_metrics.hpp"
#include <memory>
#include <string>
#include <vector>

namespace eda {
// Explicit new-official profile; legacy guide_metrics remains unchanged.
struct OfficialMetricOptions {
    double tsv_cost_ratio{6.0};
    double via_cost_ratio{3.0};
    int tsv_congestion_ratio{1};
    double thermal_gain{20.0};
    // Exact first GCELLGRID statement STEP, including clipped boundary cells.
    double gcell_width_dbu{0.0};
    double gcell_height_dbu{0.0};
    bool score_20261020{false}; // Platform wirelength score and own-pin exemption.
    // Opt-in documented landing-cell resource estimate. The graph overload
    // merges this net's Guide with the physical TSV track reservation.
    bool tsv_track_demand{false};
};
struct NativeFluteResult {
    double wirelength_dbu{0.0};
    int via_count{0};
    bool skipped_degree_limit{false};
};
// Uses the unmodified official Linux CPython 3.10 native extension and its
// libflute through their exported native methods. The original FLUTE tree
// is released by owned C++ code after matching the official length/via rules.
// A single pin safely returns zero; the official wrapper crashes for degree 1.
// Coordinate Manhattan span must fit signed native int arithmetic.
// No Python code or DEF/LEF input is executed. Assets must reside in an owned
// scratch directory:
// libflute.so, tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so,
// POWV9.dat, POST9.dat. The system libpython3.10.so.1.0 supplies module symbols.
// Same-directory instances share one process-wide native initialization;
// mixing different canonical asset directories in one process is rejected.
// This runtime dependency is opt-in and is never needed for legacy routing.
class NativeFluteBaseline {
public:
    explicit NativeFluteBaseline(const std::string& asset_directory);
    ~NativeFluteBaseline();
    NativeFluteBaseline(NativeFluteBaseline&&) noexcept;
    NativeFluteBaseline& operator=(NativeFluteBaseline&&) noexcept;
    NativeFluteBaseline(const NativeFluteBaseline&) = delete;
    NativeFluteBaseline& operator=(const NativeFluteBaseline&) = delete;
    NativeFluteResult evaluate_points(const std::vector<Point>& points) const;
    double measure_net(const Net& net, double gcell_step_dbu,
                       const OfficialMetricOptions& options = {}) const;
    const std::string& asset_directory() const;
    int max_degree() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// Historical diagnostic helper only; production routing never calls it.
// The published text does not provide this table or filename selection rule.
double official_thermal_gain_for_def_file(const std::string& def_path);

// Shared score in [0,100] for final measurement and candidate comparisons.
// The October 20 profile requires a positive total reference and rewards
// every nonnegative length <= reference with 100. Historical tent stays default.
double official_wirelength_score(double length_dbu, double reference_dbu,
                                 const OfficialMetricOptions& options);

// Submitted-guide metric: reject malformed/out-of-DIE geometry, deduplicate only
// planar GCells, charge each adjacent-layer rectangle, and select M10/M11 TSV. reference_dbu
// is the caller's cached exact FLUTE cost; it must not be a guide bbox proxy.
NetGuideMetrics measure_official_net_guides(const Net& net,
                                          const std::vector<GuideRect>& guides,
                                          const RoutingGraph& graph,
                                          const LefParser& lef,
                                          const OfficialMetricOptions& options,
                                          double reference_dbu);
// Geometry-only overload for independent scorers. Requires an already
// validated GCell grid, but no routing graph or positive track capacities.
// Node IDs retain the RoutingGraph layer/row/column layout.
// This overload leaves demand unmasked; independent scorers apply their own
// resource pin policy. The graph overload applies the selected profile's mask.
NetGuideMetrics measure_official_net_guides(const Net& net,
                                          const std::vector<GuideRect>& guides,
                                          const GCellGrid& grid,
                                          const Rect& die,
                                          const LefParser& lef,
                                          const OfficialMetricOptions& options,
                                          double reference_dbu);
struct OfficialGuideMetrics {
    GuideMetrics guide; // quality in [0,1], consistent with router comparisons
    double wirelength_score{0.0};
    double congestion_score{0.0};
    double thermal_score{0.0};
    double squared_normalized_overflow{0.0};
    double average_overflow{0.0};
    size_t overflow_entries{0};
    size_t zero_capacity_cells{0};
};
// guide_demand is an aggregate of per-net demand, INCLUDING TSV top/bottom.
// Add graph.official_obstacle_demand exactly once here. The graph must be
// constructed with GLOBAL_GUIDE to provide the official raw capacity/base data.
// ThermalParser must likewise use the explicit official previous-row/col mode.
OfficialGuideMetrics aggregate_official_metrics(
    const std::vector<const NetGuideMetrics*>& nets,
    const std::vector<int>& guide_demand,
    const RoutingGraph& graph,
    const ThermalParser& thermal,
    const OfficialMetricOptions& options,
    bool capture_demand = true);
} // namespace eda
