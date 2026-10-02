#pragma once
#include "router/routing_graph.hpp"
#include "parser/thermal_parser.hpp"
#include "validation/guide_validator.hpp"
#include <utility>

namespace eda {
struct NetGuideMetrics {
    std::vector<std::pair<int, int>> demand;
    std::vector<int> tsv_cells;
    std::vector<Point> tsv_points;
    double wirelength{0.0};
    double reference{0.0};
    bool connected{true};
};
struct GuideMetrics {
    std::vector<int> demand;
    int overflow{0};
    int max_overflow{0};
    int tsv_count{0};
    int used_cells{0};
    double wirelength{0.0};
    double reference{0.0};
    double normalized{100.0};
    double correlation{0.0};
    double quality{0.0};
};
// Match evaluator/common.py deduplication, checker.py heap tie ordering,
// and score.py rectangle demand, MST length, HPWL and positive overlap counting.
NetGuideMetrics measure_net_guides(const std::vector<GuideRect>& guides,
                                  const RoutingGraph& graph, const LefParser& lef,
                                  double tsv_cost = 2000.0, double via_cost = 0.0);
GuideMetrics evaluate_guides(const GuideMap& guides, const RoutingGraph& graph,
                            const LefParser& lef, const ThermalParser& thermal,
                            double tsv_cost = 2000.0, double via_cost = 0.0);

// Sufficient statistics of raw nonnegative heat and integer TSV counts give the
// same Pearson coefficient as the evaluator's separately normalized vectors.
class PearsonState {
public:
    PearsonState(std::vector<double> heat_rise, std::vector<int> tsv_counts);
    double correlation() const;
    void move(int from, int to);
    const std::vector<int>& counts() const { return counts_; }
private:
    std::vector<double> heat_;
    std::vector<int> counts_;
    long double heat_sum_{0}, heat_square_{0}, count_sum_{0}, count_square_{0}, product_{0};
};
} // namespace eda
