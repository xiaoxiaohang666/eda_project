#pragma once
#include "parser/def_parser.hpp"
#include <map>

namespace eda {
using GuideMap = std::map<std::string, std::vector<GuideRect>>;
// Structural guide checks only: no detailed-route DRC, capacity or TSV spacing.
bool validate_net_guides(const Net& net, const Rect& die, const LefParser& lef,
                         const std::vector<GuideRect>& guides, std::string& error);
bool validate_guides(const DefParser& def, const LefParser& lef,
                     const GuideMap& guides, std::string& error);
}
