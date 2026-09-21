#pragma once
#include "parser/def_parser.hpp"
#include <map>

namespace eda {
using GuideMap = std::map<std::string, std::vector<GuideRect>>;
// Structural guide checks only: no detailed-route DRC, capacity or TSV spacing.
bool validate_guides(const DefParser& def, const LefParser& lef,
                     const GuideMap& guides, std::string& error);
}
