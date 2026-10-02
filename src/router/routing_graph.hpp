#pragma once

#include "parser/def_parser.hpp"
#include <array>
#include <cstddef>

namespace eda {

// GCell-center resource graph. Geometry is global DEF DBU throughout this API.
// Preferred-direction planar edges and explicit LEF vias within each die only.
// Capacity and corridor feasibility are separate: a partially obstructed cell is
// retained when its local corridor survives. This is a global-routing model,
// not a spacing/DRC checker or a detailed track-assignment engine.
class RoutingGraph {
public:
    RoutingGraph(const DefParser& def, const LefParser& lef);

    int node(int col, int row, int layer) const;
    int col(int id) const;
    int row(int id) const;
    int layer(int id) const;
    size_t size() const { return legal_.size(); }
    Point point(int id) const;
    Rect cell(int id) const;
    std::vector<int> neighbors(int id) const;
    bool legal_node(int id) const;
    bool legal_edge(int a, int b) const;
    // 0 means illegal/no tracks; -1 is a legal ordinary via (no track capacity).
    double edge_capacity(int a, int b) const;
    double edge_length(int a, int b) const;
    // Scorer compatibility: H uses X TRACKS, V uses Y TRACKS. Physical planar
    // capacity instead uses the orthogonal axis (H -> Y, V -> X).
    double guide_capacity(int id) const;
    double ell0() const { return ell0_; }
    const GCellGrid& grid() const { return grid_; }
    double routing_width(int layer) const;
    bool legal_rect(const Rect& rect, int layer) const;
    Rect segment_rect(Point a, Point b, int layer) const;
    bool legal_segment(Point a, Point b, int layer) const;
    bool legal_via(Point point, int lower_layer) const;
    std::vector<LayerRect> via_rects(Point point, int lower_layer) const;

private:
    const GCellGrid& grid_;
    Rect die_;
    double dbu_{1.0};
    double ell0_{1.0};
    size_t plane_{0};
    std::array<Direction, 21> directions_{};
    std::array<double, 21> widths_{};
    std::array<std::vector<Rect>, 21> obstacles_;
    std::array<std::vector<std::vector<size_t>>, 21> obstacle_cells_;
    std::array<std::vector<const ViaInfo*>, 21> vias_;
    std::array<std::array<std::vector<double>, 2>, 21> physical_tracks_;
    std::array<std::array<std::vector<double>, 2>, 21> scorer_tracks_;
    std::vector<unsigned char> legal_;
    std::vector<unsigned char> via_up_;
    std::vector<double> planar_forward_;
    std::vector<double> guide_capacity_;

    bool valid_id(int id) const;
    double physical_capacity(int a, int b) const;
    bool via_shapes(Point point, const ViaInfo& via, std::vector<LayerRect>* result) const;
};

} // namespace eda
