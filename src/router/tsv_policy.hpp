#pragma once
#include "router/routing_graph.hpp"
#include "parser/thermal_parser.hpp"

namespace eda {
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
};

// Explicit first-version experimental policy: geometric pairing, one physical
// TSV per cross-die net, one net per slot. These are not claimed contest rules.
class TsvPolicy {
public:
    TsvPolicy(const DefParser& def, const LefParser& lef, const RoutingGraph& graph);
    const std::vector<SlotCandidate>& slots() const { return slots_; }
    int slot_capacity(int) const { return 1; }
    int net_tsv_count(const Net& net) const { return net.is_cross_die ? 1 : 0; }
    const char* pairing_mode() const { return "transformed-port-center"; }
    bool compatible(const Net& net, int slot) const;
    std::vector<int> assign(const std::vector<Net>& nets, const ThermalParser& thermal) const;
    bool validate_mapping(const Net& net, const std::vector<GuideRect>& guides,
                          int slot, std::string& error) const;
private:
    const DefParser& def_;
    const LefParser& lef_;
    const RoutingGraph& graph_;
    std::vector<SlotCandidate> slots_;
};
} // namespace eda
