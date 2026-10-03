#pragma once
#include "router/routing_graph.hpp"
#include "router/tsv_policy.hpp"
#include "objectives/guide_metrics.hpp"
#include <chrono>
#include <set>
#include <unordered_map>

namespace eda {
struct RouterOptions {
    int rrr_rounds{30}, tsv_rounds{10}, wirelength_rounds{3};
    size_t search_limit{250000};
    double time_limit{120};
    bool dijkstra{false}, local_repair{true}, pin_local{true};
};
struct RouterCounters {
    size_t expansions{}, rrr_attempted{}, rrr_accepted{}, rollbacks{}, tsv_trials{},
           tsv_accepted{}, policy_rejections{}, local_repairs{}, templates_used{},
           pin_local_candidates{}, wirelength_trials{}, wirelength_accepted{};
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
    std::vector<NetRoute> routes_;
    GuideMap guides_;
    std::vector<int> demand_;
    std::unordered_map<EdgeKey,int> usage_;
    std::vector<std::set<int>> cell_nets_;
    std::unordered_map<EdgeKey,std::set<int>> edge_nets_;
    std::vector<double> guide_history_;
    std::unordered_map<EdgeKey,double> edge_history_;
    std::map<std::tuple<double,double,int,int,int>,std::vector<PinAccess>> access_cache_;
    CompleteMetrics best_metrics_, initial_metrics_, pre_recovery_metrics_;
    RouterCounters counters_;
    std::map<std::string,size_t> rejection_reasons_;
    std::chrono::steady_clock::time_point started_, deadline_;
    double present_{1};
    double history_scale_{1};
    double external_wirelength_{0}, external_reference_{0};
    bool complete_{false};
    bool timed_out() const;
    double edge_cost(int,int,const NetRoute&) const;
    double access_cost(const PinAccess&) const;
    std::vector<PinAccess> accesses(Point,int,int,int);
    NetRoute build_candidate(size_t,int,int);
    NetRoute build_route(size_t,int);
    std::vector<NetRoute> pin_local_candidates(size_t);
    void encode(size_t,NetRoute&);
    void account(size_t,const NetRoute&,int);
    CompleteMetrics measure() const;
    static bool better(const CompleteMetrics&,const CompleteMetrics&);
    bool rrr_round(const std::vector<size_t>&);
    void run_rrr(int);
    void optimize_tsv();
    void recover_wirelength();
    bool repair_forest(size_t,int,NetRoute&);
    void validate_complete() const;
};
}
