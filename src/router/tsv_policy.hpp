#pragma once
#include "router/routing_graph.hpp"
#include "parser/thermal_parser.hpp"
#include <memory>
#include <chrono>

namespace eda {
enum class TsvResourceMode { AUTO, PAIRED, PROJECTED, GLOBAL_GRID };
struct TsvPolicyOptions {
    TsvResourceMode resource_mode{TsvResourceMode::AUTO};
    size_t nearest_candidates{8};
    size_t max_candidates{128};
    bool global_completion{true};
    bool assignment_polish{false};
    double polish_heat_weight{2.0};
    bool polish_thermal_pareto{false};
    // Zero preserves the initial candidate budget; polish never changes the
    // cardinality matching candidate graph. Legacy defaults are two passes.
    size_t polish_candidates{0};
    size_t polish_passes{2};
    bool polish_bidirectional{false};
    bool polish_thermal_candidates{false};
    // V5: bounded exact rematching of small groups of occupied reservations.
    // This never changes the occupied-slot set or its thermal distribution.
    bool assignment_block_refine{false};
    size_t block_size{16};
    size_t block_passes{2};
    size_t block_max_groups{8192};
    size_t block_max_cost_evaluations{2000000};
    double block_max_seconds{60.0};
};
struct SlotCandidate {
    int id{-1};
    std::string bottom_name;
    std::string top_name;
    Point point;
    Rect region;
    Rect lower_pad;
    Rect upper_pad;
    Rect envelope;
    int lower_node{-1};
    int upper_node{-1};
    std::vector<int> conflicts;
    bool projected{false};
    std::string via_name;
};

// Paired legacy mode preserves the old sample. Projected engineering mode uses
// the global projection of real reservations and explicit VIA/OBS geometry.
// One TSV/net and one net/reservation remain conservative engineering policies;
// neither this projection nor the nearest-candidate heuristic is official DRC.
// Explicit GLOBAL_GRID uses one abstract M10/M11 Guide landing per GCell,
// without interpreting placement reservations as global-routing restrictions.
class TsvPolicy {
public:
    // Geometry-free policy bound. Call before graph allocation on large inputs.
    static void preflight(const DefParser& def, const LefParser& lef, TsvPolicyOptions options = {});
    TsvPolicy(const DefParser& def, const LefParser& lef, const RoutingGraph& graph,
              TsvPolicyOptions options = {});
    const std::vector<SlotCandidate>& slots() const { return slots_; }
    int slot_capacity(int) const { return 1; }
    int net_tsv_count(const Net& net) const { return net.is_cross_die ? 1 : 0; }
    const char* pairing_mode() const {
        if (options_.resource_mode==TsvResourceMode::GLOBAL_GRID) return "global-grid";
        return projected_ ? "resource-projection-via-geometry" : "transformed-port-center";
    }
    const char* assignment_mode() const {
        return projected_ ? (assignment_global_completions_?
            "sparse-local-augmenting-free-reservation-completion-heuristic":
            "sparse-nearest-augmenting-heuristic") : "exact-compatible-min-cost-flow";
    }
    size_t enabled_slots() const;
    const char* assignment_polish_thermal_mode() const {
        if (!projected_ || !options_.assignment_polish) return "not-applied";
        return options_.polish_thermal_pareto ? "pearson-pareto" : "weighted-proxy";
    }
    double assignment_polish_correlation_before() const { return assignment_polish_correlation_before_; }
    double assignment_polish_correlation_after() const { return assignment_polish_correlation_after_; }
    size_t assignment_polish_moves() const { return assignment_polish_moves_; }
    size_t assignment_polish_swaps() const { return assignment_polish_swaps_; }
    size_t assignment_polish_passes() const { return assignment_polish_passes_; }
    size_t assignment_polish_thermal_queries() const { return assignment_polish_thermal_queries_; }
    size_t assignment_polish_thermal_candidates() const { return assignment_polish_thermal_candidates_; }
    long long assignment_polish_cost_reduction() const { return assignment_polish_cost_reduction_; }
    size_t assignment_block_groups() const { return assignment_block_groups_; }
    size_t assignment_block_improvements() const { return assignment_block_improvements_; }
    size_t assignment_block_moved_nets() const { return assignment_block_moved_nets_; }
    size_t assignment_block_cost_evaluations() const { return assignment_block_cost_evaluations_; }
    size_t assignment_block_mst_work() const { return assignment_block_mst_work_; }
    size_t assignment_block_skipped_high_fanout() const { return assignment_block_skipped_high_fanout_; }
    long long assignment_block_cost_reduction() const { return assignment_block_cost_reduction_; }
    bool assignment_block_budget_exhausted() const { return assignment_block_budget_exhausted_; }
    double assignment_block_seconds() const { return assignment_block_seconds_; }
    size_t assignment_candidate_edges() const { return assignment_candidate_edges_; }
    size_t assignment_expansions() const { return assignment_expansions_; }
    size_t assignment_global_completions() const { return assignment_global_completions_; }
    double assignment_max_displacement_dbu() const { return assignment_max_displacement_dbu_; }
    bool compatible(const Net& net, int slot) const;
    std::vector<int> assign(const std::vector<Net>& nets, const ThermalParser& thermal,
        std::chrono::steady_clock::time_point deadline=std::chrono::steady_clock::time_point::max()) const;
    bool validate_mapping(const Net& net, const std::vector<GuideRect>& guides,
                          int slot, std::string& error) const;
private:
    const DefParser& def_;
    const LefParser& lef_;
    const RoutingGraph& graph_;
    std::vector<SlotCandidate> slots_;
    TsvPolicyOptions options_;
    bool projected_{false};
    std::vector<bool> enabled_;
    // Only GLOBAL_GRID populates this compact cell -> candidate lookup. Fixed
    // interface pins need their containing cell, not an exact center match.
    std::vector<int> global_cell_slots_;
    mutable size_t assignment_candidate_edges_{0}, assignment_expansions_{0};
    mutable size_t assignment_global_completions_{0};
    mutable double assignment_max_displacement_dbu_{0};
    mutable size_t assignment_polish_moves_{0}, assignment_polish_swaps_{0};
    mutable size_t assignment_polish_passes_{0};
    mutable size_t assignment_polish_thermal_queries_{0}, assignment_polish_thermal_candidates_{0};
    mutable long long assignment_polish_cost_reduction_{0};
    mutable double assignment_polish_correlation_before_{0}, assignment_polish_correlation_after_{0};
    mutable size_t assignment_block_groups_{0}, assignment_block_improvements_{0};
    mutable size_t assignment_block_moved_nets_{0}, assignment_block_cost_evaluations_{0};
    mutable size_t assignment_block_mst_work_{0}, assignment_block_skipped_high_fanout_{0};
    mutable long long assignment_block_cost_reduction_{0};
    mutable bool assignment_block_budget_exhausted_{false};
    mutable double assignment_block_seconds_{0};
    mutable std::chrono::steady_clock::time_point assignment_deadline_{std::chrono::steady_clock::time_point::max()};
    void refine_occupied_blocks(const std::vector<Net>& nets, const std::vector<int>& cross,
                                const std::vector<Point>& anchors, std::vector<int>& chosen) const;
    std::vector<int> assign_projected(const std::vector<Net>& nets,
                                     const ThermalParser& thermal) const;
};

// Bounded, deterministic proposals for post-route joint TSV/routing trials.
// Scores use 0..100 weighted quality units; this is only a shortlist proxy.
struct TsvMoveSearchOptions {
    // Upper bounds; an internal quadratic-work cap can shorten/skip queries.
    size_t max_candidates{16}, max_pins{128};
    double length_price{0.0}; // Positive weighted-quality cost per DBU.
    double thermal_gain{20.0}, risk_price{0.0};
};
struct TsvMoveCandidate {
    int slot{-1};
    double correlation_after{};
    double proxy_length_delta_dbu{};
    double thermal_score_delta{}; // Exact 0.25 * delta(T), in quality points.
    double estimated_score_delta{};
};
class TsvMoveSearch {
public:
    // All referenced design objects must outlive this context. The assignment
    // is copied, includes all net indices, and must contain one unique slot per
    // cross-die net and -1 for each same-die net. GLOBAL_GRID only.
    TsvMoveSearch(const DefParser&, const RoutingGraph&, const TsvPolicy&,
                  const ThermalParser&, const std::vector<int>& assignment);
    ~TsvMoveSearch();
    TsvMoveSearch(const TsvMoveSearch&) = delete;
    TsvMoveSearch& operator=(const TsvMoveSearch&) = delete;
    std::vector<TsvMoveCandidate> candidates(size_t net_index,
                                            const TsvMoveSearchOptions& options = {}) const;
    double current_correlation() const;
    int current_slot(size_t net_index) const;
    // Call only after a fully routed, encoded, validated move is accepted.
    // An invalid/occupied destination leaves every context field unchanged.
    bool commit_move(size_t net_index, int new_slot);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace eda
