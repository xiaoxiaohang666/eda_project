#pragma once
#include "parser/def_parser.hpp"
#include <map>

namespace eda {
using GuideMap = std::map<std::string, std::vector<GuideRect>>;
// Structural guide checks only: no detailed-route DRC, capacity or TSV spacing.
bool validate_net_guides(const Net& net, const Rect& die, const LefParser& lef,
                         const std::vector<GuideRect>& guides, std::string& error);
// October 20 profile: reuses the standalone compatible scorer's per-net
// geometry checks. Explicit original DEF pitches select the new rules; the
// original five-argument API above retains its historical behavior.
bool validate_net_guides(const Net& net, const Rect& die, const LefParser& lef,
                         const std::vector<GuideRect>& guides, std::string& error,
                         double gcell_pitch_x, double gcell_pitch_y);
bool validate_guides(const DefParser& def, const LefParser& lef,
                     const GuideMap& guides, std::string& error);
}
