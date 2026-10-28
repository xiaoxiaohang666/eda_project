#pragma once

#include "parser/lef_parser.hpp"

namespace eda {

// Engineering invariant for this router's grid-native Guide representation:
// the transverse interval is exactly one actual GCell, clipped to DIEAREA.
// This is deliberately separate from historical compatible-scoring rules.
// A successful check is NOT certification by the unpublished platform scorer;
// in particular, its treatment of clipped boundary cells remains unconfirmed.
// Along-wire alignment, connectivity, pin coverage, TSV mapping and scores are
// outside this check and must be validated separately.
bool validate_gcell_encoding(const Net& net, const Rect& die, const GCellGrid& grid,
                             const LefParser& lef, const std::vector<GuideRect>& guides,
                             std::string& error);

} // namespace eda
