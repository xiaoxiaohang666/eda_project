#pragma once
#include "router/routing_graph.hpp"
#include "router/tsv_policy.hpp"
#include "objectives/guide_metrics.hpp"
#include "objectives/official_metrics.hpp"
#include <chrono>
#include <limits>
#include <set>
#include <unordered_map>
#include <functional>
#include <optional>
#include <array>

namespace eda {
struct RouterOptions {
    int rrr_rounds{30}, tsv_rounds{10}, wirelength_rounds{3};
    size_t search_limit{250000};
    double time_limit{120};
    bool dijkstra{false}, local_repair{true}, pin_local{true}, tsv_local{true};
    bool cell_pin_access{true}, fast_initial{true};
    bool fast_quality{true};
    bool fast_layer_pruning{true};
    // Compare one pin-rooted, TSV-last tree during large official initial routing.
    // Keep the established TSV-rooted tree as the incumbent.
    bool v5_pin_rooted{false};
    size_t v5_topology_max_pins{32};
    bool rrr_score_aware{false};
    size_t rrr_net_limit{128};
    bool official_score_pricing{false};
    bool large_recovery{false};
    size_t large_recovery_nets{1024};
    int large_recovery_rounds{1}, large_recovery_branches{8};
    double large_recovery_seconds{300};
    TsvPolicyOptions tsv_policy;
    bool official_model{false};
    RoutingGraphOptions graph_options;
    OfficialMetricOptions official_metrics;
    std::string official_runtime;
};
struct RouterCounters {
    size_t expansions{}, search_peak_states{}, rrr_attempted{}, rrr_accepted{}, rollbacks{}, tsv_trials{},
           tsv_accepted{}, policy_rejections{}, local_repairs{}, templates_used{},
           pin_local_candidates{}, wirelength_trials{}, wirelength_accepted{},
           tsv_local_candidates{}, tsv_local_trials{}, tsv_local_accepted{},
           abstract_terminal_accesses{}, port_terminal_accesses{}, omitted_checker_apertures{}, fast_initial_paths{},
           fast_tree_shortcuts{}, fast_truncated_paths{}, fast_z_candidates{}, fast_z_paths{}, fast_layer_prunes{},
           v5_topology_attempted{}, v5_topology_candidates{}, v5_topology_accepted{},
           v5_topology_failures{}, v5_topology_thermal_rejections{},
           v5_topology_budget_skips{}, v5_topology_interruptions{};
    double v5_topology_seconds{};
};
// One undirected resource key; a Net consumes each resource only once.
using EdgeKey = uint64_t;
inline EdgeKey route_edge(int a, int b) {
    if (a > b) std::swap(a,b);
    return (static_cast<uint64_t>(a) << 32) | static_cast<uint32_t>(b);
}
inline std::pair<int,int> edge_nodes(EdgeKey e) {
    return {static_cast<int>(e >> 32), static_cast<int>(e & 0xffffffffU)};
}
struct PinAccess {
    int node{-1};
    std::vector<GuideRect> guides;
    double length{};
    int vias{};
};
struct NetRoute {
    // Stable index into the router's sparse own-pin exemption cache.
    size_t net_index{std::numeric_limits<size_t>::max()};
    // Explicit terminal-to-terminal geometry still reserves the crossed graph
    // resources, but must not emit a second path through their GCell centers.
    bool explicit_geometry{false};
    std::set<int> nodes;
    std::set<EdgeKey> edges;
    std::vector<PinAccess> accesses;
    std::vector<GuideRect> guides;
    NetGuideMetrics measured;
    int slot{-1}, vias{};
    double length{};
};
struct CompleteMetrics {
    bool official_model{false};
    bool score_20261020{false};
    OfficialGuideMetrics official;
    GuideMetrics guide;
    long long physical_overflow{};
    double physical_length{};
    size_t vias{}, physical_tsvs{};
};
struct PathResult {
    enum class Status { FOUND, UNREACHABLE, SEARCH_LIMIT } status{Status::UNREACHABLE};
    std::vector<int> nodes;
    size_t source{};
    double cost{};
};
struct BudgetedRecoveryStats {
    size_t selected{}, zero_reference{}, attempted{}, candidates{}, shared_candidates{}, local_candidates{},
           accepted{}, rejected{}, checkpoints{}, checkpoint_rollbacks{}, candidate_failures{};
    bool timed_out{false};
    double seconds{};
    CompleteMetrics before, after;
};
class NegotiatedRouter {
public:
    NegotiatedRouter(const DefParser&, const LefParser&, const ThermalParser&, RouterOptions = {});
    bool route_all();
    bool export_guide(const std::string&) const;
    bool export_stats(const std::string&) const;
    const GuideMap& guides() const { return guides_; }
    const RouterCounters& counters() const { return counters_; }
    const CompleteMetrics& metrics() const { return best_metrics_; }
    // The same search/cost implementation is exposed for A*/Dijkstra regression checks.
    PathResult search(const std::vector<PinAccess>& sources, const std::set<int>& targets,
                      const NetRoute& own, int low, int high, bool dijkstra);
private:
    friend struct PinLocalTestAccess;
    const DefParser& def_;
    const LefParser& lef_;
    const ThermalParser& thermal_;
    RouterOptions options_;
    RoutingGraph graph_;
    TsvPolicy policy_;
    std::unique_ptr<NativeFluteBaseline> flute_;
    std::vector<double> reference_lengths_;
    std::vector<std::vector<int>> own_pin_nodes_;
    long double full_reference_{0};
    std::vector<NetRoute> routes_;
    GuideMap guides_;
    std::vector<int> demand_;
    std::unordered_map<EdgeKey,int> usage_;
    // Reverse ownership exists only for occupied cells, not every graph state.
    // Most resources have one/few owners. Sorted unique vectors avoid a red-
    // black-tree node allocation for every owner while preserving set order.
    std::unordered_map<int,std::vector<int>> cell_nets_;
    std::unordered_map<EdgeKey,std::vector<int>> edge_nets_;
    std::vector<double> guide_history_;
    std::unordered_map<EdgeKey,double> edge_history_;
    std::map<std::tuple<double,double,int,int,int,bool>,std::vector<PinAccess>> access_cache_;
    CompleteMetrics best_metrics_, initial_metrics_, pre_recovery_metrics_, pre_tsv_local_metrics_;
    RouterCounters counters_;
    std::map<std::string,size_t> rejection_reasons_;
    std::chrono::steady_clock::time_point started_, deadline_;
    double present_{1};
    double history_scale_{1};
    double external_wirelength_{0}, external_reference_{0};
    // Official candidate comparisons read this transactional other-net ledger.
    // Fixed OBS/PIN demand is included once at construction; account() changes
    // only the cells and cached net totals touched by an add/ripup operation.
    long double official_wirelength_{0}, official_reference_{0}, official_squared_overflow_{0};
    uint64_t official_overflow_{0};
    size_t official_zero_capacity_cells_{0};
    // Production route changes are paired with account(). Only a successful
    // full internal validation caches a generation; serialized readback stays independent.
    uint64_t state_generation_{0};
    mutable uint64_t pricing_generation_{std::numeric_limits<uint64_t>::max()};
    mutable double congestion_price_{1}, congestion_price_min_{10000}, congestion_price_max_{0};
    mutable size_t pricing_updates_{0};
    mutable uint64_t validated_generation_{0};
    mutable bool complete_validation_cached_{false};
    mutable size_t complete_validations_{0}, complete_validation_cache_hits_{0};
    bool complete_{false}, initial_build_{true};
    bool v5_topology_trial_{false};
    bool recovery_cell_cost_{false};
    BudgetedRecoveryStats recovery_stats_;
    bool large_design() const { return def_.nets().size()>1000; }
    bool timed_out() const;
    double v5_topology_seconds_limit() const { return std::min(120.0,0.05*options_.time_limit); }
    void check_v5_topology_budget() const;
    bool v5_topology_better(const NetRoute&,const NetRoute&,const CompleteMetrics&) const;
    double official_congestion_price() const;
    const std::vector<int>& cell_owners(int) const;
    double edge_cost(int,int,const NetRoute&) const;
    double access_cost(const PinAccess&,const NetRoute* own=nullptr) const;
    bool pin_exempt(size_t net,int node) const;
    bool validate_route(size_t,const std::vector<GuideRect>&,std::string&) const;
    std::vector<PinAccess> accesses(Point,int,int,int,bool terminal_escape=true);
    std::vector<PinAccess> accesses(const Pin&,int,int);
    NetRoute build_candidate(size_t,int,int);
    NetRoute build_tree(size_t,int,int,const std::array<int,2>& seeds={{-1,-1}},int only_side=-1,bool tsv_last=false);
    NetRoute build_route(size_t,int);
    std::vector<NetRoute> pin_local_candidates(size_t);
    std::vector<NetRoute> terminal_pair_candidates(const Pin&,const Pin&,int,int);
    void encode(size_t,NetRoute&);
    void account(size_t,const NetRoute&,int);
    CompleteMetrics measure() const;
    CompleteMetrics official_candidate_base() const;
    double official_reroute_priority(int node,int owned_demand) const;
    std::vector<std::pair<int,int>> official_protected_demand(size_t,const NetRoute&) const;
    bool official_zero_capacity_blocked(int) const;
    double official_movable_priority(size_t) const;
    void complete_zero_capacity_scope(std::vector<size_t>& scope) const;
    std::tuple<size_t,double,long long,double,int> official_candidate_key(const NetRoute&,const CompleteMetrics&) const;
    static bool better(const CompleteMetrics&,const CompleteMetrics&);
    bool rrr_round(const std::vector<size_t>&);
    void run_rrr(int);
    void optimize_tsv();
    void recover_wirelength();
    void recover_budgeted_wirelength();
    bool recovery_transaction(size_t,const std::function<std::optional<NetRoute>(const NetRoute&,const CompleteMetrics&)>&);
    bool recovery_checkpoint(std::vector<std::pair<size_t,NetRoute>>&,CompleteMetrics&);
    CompleteMetrics recovery_metrics(const CompleteMetrics&) const;
    std::vector<size_t> recovery_selection() const;
    std::optional<NetRoute> recovery_candidates(size_t,const NetRoute&,const CompleteMetrics&);
    std::vector<std::pair<int,int>> recovery_seeds(size_t,const NetRoute&) const;
    std::vector<NetRoute> recovery_local_candidates(size_t,const NetRoute&);
    void recover_tsv_local();
    bool repair_forest(size_t,int,NetRoute&);
    void validate_complete() const;
};
}
