#include "validation/gcell_encoding.hpp"

#include <iomanip>
#include <numeric>
#include <sstream>

namespace eda {

bool validate_gcell_encoding(const Net& net, const Rect& die, const GCellGrid& grid,
                             const LefParser& lef, const std::vector<GuideRect>& guides,
                             std::string& error) {
    error.clear();
    const auto fail = [&](const std::string& detail) {
        error = net.name + ": GCELL_ENCODING: " + detail;
        return false;
    };
    if (!grid.valid()) return fail("invalid GCell grid");
    if (!std::isfinite(die.x1) || !std::isfinite(die.y1) ||
        !std::isfinite(die.x2) || !std::isfinite(die.y2) ||
        die.x1 >= die.x2 || die.y1 >= die.y2)
        return fail("invalid DIEAREA");
    if (guides.empty()) return fail("missing guide rectangles");

    for (std::size_t index = 0; index < guides.size(); ++index) {
        const auto& r = guides[index];
        const auto prefix = "rectangle " + std::to_string(index) + " (" + r.layer + ") ";
        const auto* layer = lef.get_layer(r.layer);
        if (!get_layer_index(r.layer) || !layer || layer->type != "ROUTING" ||
            layer->direction == Direction::UNKNOWN)
            return fail(prefix + "uses an unknown/non-routing layer");
        if (!std::isfinite(r.x1) || !std::isfinite(r.y1) ||
            !std::isfinite(r.x2) || !std::isfinite(r.y2) ||
            r.x1 >= r.x2 || r.y1 >= r.y2)
            return fail(prefix + "has non-finite coordinates or non-positive area");
        if (r.x1 < die.x1 || r.y1 < die.y1 || r.x2 > die.x2 || r.y2 > die.y2)
            return fail(prefix + "extends outside DIEAREA");

        const bool horizontal = layer->direction == Direction::HORIZONTAL;
        const double low = horizontal ? r.y1 : r.x1;
        const double high = horizontal ? r.y2 : r.x2;
        const double middle = std::midpoint(low, high);
        const int cell = horizontal ? grid.get_row(middle) : grid.get_col(middle);
        if (cell < 0) return fail(prefix + "has no transverse GCell");
        const auto& lines = horizontal ? grid.y_lines : grid.x_lines;
        const double expected_low = std::max(lines[cell], horizontal ? die.y1 : die.x1);
        const double expected_high = std::min(lines[cell + 1], horizontal ? die.y2 : die.x2);
        // These coordinates come from the same grid/DIE values as the encoder.
        // Exact endpoints also reject a one-pitch corridor shifted across cells.
        if (expected_low >= expected_high || low != expected_low || high != expected_high) {
            std::ostringstream detail;
            detail << std::setprecision(17) << prefix
                   << "transverse interval [" << low << ',' << high << "] must equal "
                   << "one DIE-clipped GCell [" << expected_low << ',' << expected_high
                   << "] for " << direction_to_string(layer->direction);
            return fail(detail.str());
        }
    }
    return true;
}

} // namespace eda
