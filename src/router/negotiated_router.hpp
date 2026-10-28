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
    // V7 refinements are opt-in. Bound expensive maze work per rerouted net,
    // independently of the inexpensive template/negotiation budget.
    bool v7_rrr{false}, v7_cell_cost{false}, v7_tsv{false}, v7_topology{false};
    size_t v7_tsv_nets{256}, v7_tsv_candidates{4};
    double v7_tsv_seconds{30.0};
    size_t v7_rrr_net_limit{4096}, v7_rrr_astar_net_limit{64};
    size_t v7_rrr_astar_expansions{20000};
    double v7_rrr_astar_seconds{0.10};
    // V8 stages remain opt-in so A0 uses the identical V7 algorithm.
    bool v8_rrr{false}, v8_tsv_dynamic{false}, v8_tsv_rematch{false}, v8_recovery{false};
    bool unified_budget{false}, score_unit_cost{false};
    double congestion_price_cap{10000};
    size_t v8_rrr_group_size{48}, v8_tsv_probe_candidates{2};
    size_t v8_tsv_rematch_group_size{6}, v8_tsv_rematch_nets{256};
    double v8_tsv_rematch_seconds{30};
    // V9 mechanisms are independent for same-binary documented-rule ablations.
    bool v9_incremental_rrr{false}, v9_subtree_recovery{false};
    bool v9_access_rematch{false}, v9_tsv_coverage{false}, v9_thermal_regions{false};
    bool v9_adaptive_budget{false};
    // Independent experiments after the October 27 TSV occupancy update.
    bool tsv_landing_pricing{false}, tsv_landing_rrr{false};
    size_t v9_checkpoint_groups{32};
    double v9_checkpoint_seconds{5.0};
    double v9_rematch_access_seconds{0.004};
    size_t v9_subtree_branches{4}, v9_subtree_boundary{16}, v9_rematch_edges{12};
    size_t v9_tsv_stale_sweeps{1};
    // Supplied by CLI before parsing. Default keeps library callers usable.
    std::chrono::steady_clock::time_point process_started{};
    bool large_recovery{false};
    size_t large_recovery_nets{1024};
    int large_recovery_rounds{1}, large_recovery_branches{8};
    double large_recovery_seconds{300};
    TsvPolicyOptions tsv_policy;
    bool official_model{false};
    RoutingGraphOptions graph_options;
    OfficialMetricOptions official_metrics;
    std::string official_runtime;
    bool explicit_thermal_gain{false};
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
    size_t v7_topology_attempted{}, v7_topology_candidates{}, v7_topology_accepted{}, v7_topology_failures{};
    double v7_topology_seconds{};
    size_t v7_rrr_nets{}, v7_astar_nets{}, v7_astar_searches{}, v7_astar_fallbacks{},
           v7_ledger_checks{}, v7_checkpoint_restores{}, v7_rrr_peak_scope{},
           v7_tsv_candidates{}, v7_tsv_attempted{}, v7_tsv_accepted{}, v7_tsv_rollbacks{},
           v7_tsv_failures{}, v7_tsv_budget_stops{};
    size_t v8_rrr_groups{}, v8_rrr_groups_accepted{}, v8_rrr_groups_rejected{}, v8_rrr_group_nets{};
    size_t v8_rematch_groups{}, v8_rematch_candidates{}, v8_rematch_accepted{}, v8_rematch_rollbacks{}, v8_rematch_failures{};
    size_t v8_tsv_probed_nets{}, v8_tsv_unique_nets{}, v8_tsv_repeat_visits{}, v8_tsv_probe_queries{}, v8_tsv_refreshes{};
    std::array<size_t,3> v8_tsv_queue_visits{};
    std::array<double,3> v8_tsv_queue_gain{}, v8_tsv_queue_seconds{};
    double v8_rrr_seconds{}, v8_rematch_seconds{}, v8_tsv_probe_seconds{};
    double v5_topology_seconds{}, v7_rrr_seconds{}, v7_validation_seconds{}, v7_tsv_seconds{};
    size_t v9_local_validations{}, v9_full_checkpoints{}, v9_checkpoint_restores{};
    double v9_local_validation_seconds{}, v9_full_validation_seconds{};
    size_t v9_subtree_candidates{}, v9_subtree_accepted{}, v9_subtree_searches{};
    size_t v9_rematch_access_queries{}, v9_rematch_cross_window_groups{};
    size_t v9_tsv_queried_unique{}, v9_tsv_sweeps{}, v9_tsv_rerouted_unique{}, v9_tsv_moved_unique{}, v9_tsv_explore_visits{};
    size_t v9_region_candidates{}, v9_region_accepted{};
    double v9_tsv_explore_gain{}, v9_tsv_explore_seconds{}, v9_tsv_refresh_seconds{};
    double v9_rrr_budget{}, v9_recovery_budget{}, v9_rematch_budget{}, v9_tsv_budget{};

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
    bool recovery_subtree{false};
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
    friend struct TsvLandingRrrTestAccess;
    friend struct PinLocalTestAccess;
    friend struct V7RouterTestAccess;
    friend struct HotspotRrrTestAccess;
    friend struct DynamicTsvTestAccess;
    friend struct TsvRematchTestAccess;
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
    std::vector<double> pre_tsv_lengths_;
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
    mutable double congestion_price_{1}, congestion_price_min_{std::numeric_limits<double>::infinity()}, congestion_price_max_{0};
    mutable size_t pricing_updates_{0};
    mutable uint64_t validated_generation_{0};
    mutable bool complete_validation_cached_{false};
    mutable size_t complete_validations_{0}, complete_validation_cache_hits_{0};
    bool complete_{false}, initial_build_{true};
    bool v5_topology_trial_{false};
    bool recovery_cell_cost_{false};
    bool v7_refinement_active_{false};
    size_t v7_refinement_expansion_start_{0};
    std::chrono::steady_clock::time_point v7_refinement_deadline_;
    size_t v7_scope_limit_{0};
    // The journal spans local RRR transactions until the next full proof.
    std::set<size_t> v9_tsv_rerouted_seen_, v9_tsv_moved_seen_, v9_tsv_queried_seen_;
    // Adaptive time slices resume the same TSV coverage epoch. Estimates are
    // shortlist proxies only; selected nets are always queried against live state.
    struct DynamicTsvState {
        struct Estimate {std::array<double,3> value{};double work{1};};
        std::vector<Estimate> estimates;
        std::vector<double> measured_work;
        std::vector<unsigned char> visited,ever_visited;
        std::array<double,3> recent_gain{},recent_seconds{};
        std::array<size_t,3> local_visits{};
        size_t visited_count{},explore_cursor{},refresh_cursor{},probe_cursor{},picks{};
        size_t sweep_accepted{},stale_sweeps{};
    } v9_tsv_state_;
    bool v9_rrr_checkpoint_active_{false};
    std::map<size_t,NetRoute> v9_rrr_journal_;
    CompleteMetrics v9_rrr_checkpoint_metrics_;
    std::chrono::steady_clock::time_point v9_rrr_checkpoint_time_;
    size_t v9_rrr_pending_groups_{0}, v9_rrr_accepted_checkpoint_{0}, v9_hotspot_accepted_checkpoint_{0};
    void begin_rrr_checkpoint();
    void journal_rrr_scope(const std::vector<size_t>&);
    void validate_rrr_group(const std::vector<std::pair<size_t,NetRoute>>&);
    bool finish_rrr_checkpoint(bool force=false);
    void restore_rrr_checkpoint(const std::string&);

    bool v7_search_budget_available() const;
    void verify_official_ledger(const CompleteMetrics&);
    BudgetedRecoveryStats recovery_stats_;
    bool large_design() const { return def_.nets().size()>1000; }
    bool timed_out() const;
    double v5_topology_seconds_limit() const { return std::min(120.0,0.05*options_.time_limit); }
    void check_v5_topology_budget() const;
    bool v5_topology_better(const NetRoute&,const NetRoute&,const CompleteMetrics&) const;
    double official_congestion_price() const;
    double official_search_scale() const;
    double official_cell_search_cost(int) const;
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
    void optimize_v7_tsv();
    void run_hotspot_rrr(int);
    void optimize_dynamic_tsv();
    void rematch_tsv_ownership();
    void recover_wirelength();
    void recover_budgeted_wirelength();
    bool recovery_transaction(size_t,const std::function<std::optional<NetRoute>(const NetRoute&,const CompleteMetrics&)>&);
    bool recovery_checkpoint(std::vector<std::pair<size_t,NetRoute>>&,CompleteMetrics&);
    CompleteMetrics recovery_metrics(const CompleteMetrics&) const;
    std::vector<size_t> recovery_selection() const;
    std::optional<NetRoute> recovery_candidates(size_t,const NetRoute&,const CompleteMetrics&);
    std::vector<std::pair<int,int>> recovery_seeds(size_t,const NetRoute&) const;
    std::vector<NetRoute> recovery_local_candidates(size_t,const NetRoute&);
    std::vector<NetRoute> recovery_subtree_candidates(size_t,const NetRoute&);
    void run_v9_schedule();
    void recover_tsv_local();
    bool repair_forest(size_t,int,NetRoute&);
    void validate_complete() const;
};
}
