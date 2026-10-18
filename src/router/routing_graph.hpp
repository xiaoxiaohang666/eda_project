#pragma once

#include "parser/def_parser.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace eda {

enum class RoutingGeometryMode { PHYSICAL_STRICT, GLOBAL_GUIDE };
struct RoutingGraphOptions {
    RoutingGeometryMode geometry{RoutingGeometryMode::PHYSICAL_STRICT};
    bool allow_nonpreferred{true}; // Applies only to abstract GLOBAL_GUIDE adjacency.
};

// Resource graph in global DEF DBU. PHYSICAL_STRICT retains preferred-direction
// center corridors and explicit LEF vias. GLOBAL_GUIDE represents GCell regions:
// OBS/PIN reduce capacity demand and do not invalidate a Guide envelope; ordinary
// layer overlap and optional nonpreferred adjacency are abstract connectivity.
// Capacity and corridor feasibility are separate: a partially obstructed cell is
// retained when its local corridor survives. This is a global-routing model,
// not a spacing/DRC checker or a detailed track-assignment engine.
class RoutingGraph {
public:
    RoutingGraph(const DefParser& def, const LefParser& lef, RoutingGraphOptions options = {});

    int node(int col, int row, int layer) const;
    int col(int id) const;
    int row(int id) const;
    int layer(int id) const;
    size_t size() const { return legal_.size(); }
    Point point(int id) const;
    Rect cell(int id) const;
    std::vector<int> neighbors(int id) const;
    bool legal_node(int id) const;
    bool has_neighbor(int id,int low=1,int high=20) const;
    bool legal_edge(int a, int b) const;
    // 0 means illegal/no tracks; -1 is a legal ordinary via (no track capacity).
    double edge_capacity(int a, int b) const;
    double edge_length(int a, int b) const;
    // Preserve the legacy scorer axis when supplied (H -> X, V -> Y).
    // A preferred-axis-only DEF uses its actual physical TRACKS for this proxy.
    // GLOBAL_GUIDE instead returns official raw capacity minus fixed OBS/PIN
    // demand; it may be negative and is a routing cost budget, not a DRC result.
    double guide_capacity(int id) const;
    bool guide_uses_physical_tracks(int layer) const;
    RoutingGeometryMode geometry_mode() const { return options_.geometry; }
    bool global_guide_mode() const { return options_.geometry == RoutingGeometryMode::GLOBAL_GUIDE; }
    bool allows_nonpreferred() const { return global_guide_mode() && options_.allow_nonpreferred; }
    // New official scorer capacity and fixed OBS/all-master-PIN demand. These
    // are built only in GLOBAL_GUIDE mode, independently of legacy proxies.
    double official_capacity(int id) const;
    uint32_t official_obstacle_demand(int id) const;
    const std::vector<uint32_t>& official_obstacle_demands() const;
    const char* capacity_model() const { return global_guide_mode()
        ? "official-pattern-track-capacity-and-obs-pin-index-union"
        : "legacy-guide-axis-and-strict-track-corridors"; }
    double ell0() const { return ell0_; }
    const GCellGrid& grid() const { return grid_; }
    const Rect& die_area() const { return die_; }
    double routing_width(int layer) const;
    bool legal_rect(const Rect& rect, int layer) const;
    // LEF PORT metal overrules only its own instance OBS within that PORT.
    // Other owners, DIEAREA and unrestricted routing keep their normal checks.
    bool legal_pin_rect(const Rect& rect, int layer, const Pin& pin) const;
    bool legal_pin_via(Point point, int lower_layer, const Pin& pin) const;
    std::vector<LayerRect> pin_via_rects(Point point, int lower_layer, const Pin& pin) const;
    Rect segment_rect(Point a, Point b, int layer) const;
    bool legal_segment(Point a, Point b, int layer) const;
    bool legal_via(Point point, int lower_layer) const;
    std::vector<LayerRect> via_rects(Point point, int lower_layer) const;

private:
    RoutingGraphOptions options_;
    const GCellGrid& grid_;
    Rect die_;
    double dbu_{1.0};
    double ell0_{1.0};
    size_t plane_{0};
    std::array<Direction, 21> directions_{};
    std::array<double, 21> widths_{};
    std::array<std::vector<Rect>, 21> obstacles_;
    std::array<std::vector<const std::string*>, 21> obstacle_owners_;
    // Compressed row index: empty cells need only one uint32 offset, rather
    // than a vector object. Each bucket references the original OBS geometry.
    std::array<std::vector<uint32_t>, 21> obstacle_offsets_;
    std::array<std::vector<uint32_t>, 21> obstacle_indices_;
    std::array<std::vector<const ViaInfo*>, 21> vias_;
    std::array<std::array<std::vector<double>, 2>, 21> physical_tracks_;
    std::array<std::array<std::vector<double>, 2>, 21> scorer_tracks_;
    std::vector<unsigned char> legal_;
    std::vector<unsigned char> via_up_;
    std::vector<uint32_t> planar_forward_;
    std::array<int, 21> guide_axes_{};
    std::array<bool, 21> physical_guide_mode_{};
    std::array<std::vector<double>, 21> guide_capacity_;
    std::array<std::vector<double>, 21> official_capacity_;
    std::vector<uint32_t> official_obstacle_demand_;

    void build_official_resources(const DefParser&, const LefParser&);

    bool valid_id(int id) const;
    uint32_t physical_capacity(int a, int b) const;
    bool via_shapes(Point point, const ViaInfo& via, std::vector<LayerRect>* result, const Pin* pin=nullptr) const;
};

} // namespace eda
