#pragma once

#include "parser/def_parser.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace eda {
namespace compat { class Resources; }

enum class RoutingGeometryMode { PHYSICAL_STRICT, GLOBAL_GUIDE };
struct RoutingGraphOptions {
    RoutingGeometryMode geometry{RoutingGeometryMode::PHYSICAL_STRICT};
    bool allow_nonpreferred{true}; // Applies only to abstract GLOBAL_GUIDE adjacency.
    bool score_20261020{false}; // Opt-in documented October 20 resource/direction rules.
    bool platform_20261024{false}; // Historical scorer diagnostics only; not production optimization.
    bool documented_rules{false}; // Actual PIN layers and unique physical track coordinates.
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
    bool score_20261020() const { return options_.score_20261020; }
    bool platform_resources() const { return options_.platform_20261024; }
    bool documented_rules() const { return options_.documented_rules; }
    // New official scorer capacity and fixed OBS/all-master-PIN demand. These
    // are built only in GLOBAL_GUIDE mode, independently of legacy proxies.
    double official_capacity(int id) const;
    // Physical TSV track reservation in its selected GCell only. Unique DEF
    // track coordinates follow the layer's preferred direction. Capacity bands
    // are half-open (last band closed); TSV metal includes its boundary tracks.
    // This is an explicit documented-model estimate, not a platform boundary
    // calibration or detailed physical footprint check outside the landing cell.
    int tsv_landing_tracks(Point point, int layer, double width_um = 5.0) const;
    uint32_t official_obstacle_demand(int id) const;
    const std::vector<uint32_t>& official_obstacle_demands() const;
    // Sparse, sorted cell-layer IDs exempt from this net's GUIDE/TSV demand.
    // The historical profile returns an empty mask, retaining its old demand.
    std::vector<int> own_pin_nodes(const Net& net) const;
    const char* capacity_model() const { return global_guide_mode()
        ? (documented_rules() ? "20261020-documented-actual-pin-layers-unique-tracks"
            : (platform_resources() ? "20261024-platform-first-macro-pin-half-open-fixed-tracks"
                : (score_20261020() ? "20261020-compat-clipped-tracks-and-obs-pin-io-union"
                                    : "official-pattern-track-capacity-and-obs-pin-index-union")))
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
    std::shared_ptr<const compat::Resources> updated_resources_;

    void build_official_resources(const DefParser&, const LefParser&);

    bool valid_id(int id) const;
    uint32_t physical_capacity(int a, int b) const;
    bool via_shapes(Point point, const ViaInfo& via, std::vector<LayerRect>* result, const Pin* pin=nullptr) const;
};

} // namespace eda
