#pragma once

#include "validation/guide_validator.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace eda::compat {

struct ValidationResult {
    std::size_t checked_nets{};
    std::size_t checked_pins{};
    std::size_t checked_rectangles{};
    std::size_t error_count{};
    // Keep diagnostics bounded even for a large, invalid submission.
    std::vector<std::string> errors;
};

// Standalone S1 checks from the 2026-10-20 scoring description. This does not
// impose the router's physical OBS, TSV slot or exclusive-GCell restrictions.
// The selected TSV's candidate-count/ambiguity rules are additionally checked
// by measure_official_net_guides() in the scoring executable.
//
// legacy_direction restores the old same-layer edge-contact rule and removes
// the new transverse-size limit; the other checks remain identical.
ValidationResult validate(const DefParser& def, const LefParser& lef,
                          const GuideMap& guides, bool legacy_direction = false);

// The same per-net checks without constructing a GuideMap or scanning other
// nets. Pass the original DEF GCELLGRID STEP values, not clipped boundary-cell
// widths. The scoring executable additionally checks TSV candidate uniqueness
// through measure_official_net_guides(), as for the full-design entry point.
ValidationResult validate_net(const Net& net, const Rect& die, const LefParser& lef,
                              const std::vector<GuideRect>& guides,
                              double gcell_pitch_x, double gcell_pitch_y,
                              bool legacy_direction = false);

// Read the serialized submission without normalizing reversed coordinates.
// Numeric tokens must be consumed completely. Malformed structure, duplicate
// nets, missing rectangles and I/O failures throw std::runtime_error.
// Guide layer spellings such as Metal1 are normalized to metal1.
GuideMap read_guides(const std::string& path);

} // namespace eda::compat
