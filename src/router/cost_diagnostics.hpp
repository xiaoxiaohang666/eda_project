#pragma once
#include "router/negotiated_router.hpp"
#include <ostream>
namespace eda {
void write_cost_diagnostics(std::ostream&,const std::vector<NetRoute>&,const std::vector<Net>&,const std::vector<double>&);
}
