#pragma once

#include "parser/def_parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace eda::compat {

// These two details were not specified by the October 20 update. Defaults
// preserve the old scorer's first-layer pin and repeated-track conventions.
struct ResourceOptions {
    bool pin_first_layer{true};
    bool deduplicate_tracks{false};
    // Explicit observed 0.4.0 platform resource policy. This changes resources
    // only, not physical PIN geometry or October 20 legality/length rules.
    bool platform_20261024{false};
};

// Resource model reconstructed from the published October 20 scoring notes.
// This is independent of the router's historical official-resource model.
// The DEF parser and its grid must outlive this object.
class Resources {
public:
    Resources(const DefParser& def, const LefParser& lef, ResourceOptions options = {});

    size_t size() const { return fixed_demand_.size(); }
    int node(int col, int row, int layer) const;
    int col(int id) const;
    int row(int id) const;
    int layer(int id) const;
    uint32_t capacity(int id) const;
    uint32_t fixed_demand(int id) const;
    const std::vector<uint32_t>& fixed_demands() const { return fixed_demand_; }

    // Sorted, unique cell-layer IDs covered by this net's actual pin shapes.
    // Historical profiles use the fixed-demand pin-layer policy. Platform
    // macro-PIN credit retains only original shapes on the first geometry
    // layer; IO credit retains every original layer. No whole-grid net masks.
    std::vector<int> pin_nodes(const Net& net) const;

    const GCellGrid& grid() const { return grid_; }
    const ResourceOptions& options() const { return options_; }

private:
    const GCellGrid& grid_;
    ResourceOptions options_;
    size_t plane_{0};
    std::array<Direction, 21> directions_{};
    std::array<std::vector<uint32_t>, 21> capacities_;
    std::vector<uint32_t> fixed_demand_;

    void check_id(int id) const;
};

} // namespace eda::compat
