#pragma once

#include "common/types.hpp"
#include "parser/def_parser.hpp"
#include "parser/lef_parser.hpp"
#include "parser/thermal_parser.hpp"
#include "validation/guide_validator.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace eda {

class BaselineRouter {
public:
    BaselineRouter(const DefParser& def_parser,

        
                 const LefParser& lef_parser,
                 const ThermalParser& thermal_parser);

    bool route_all();

    bool export_guide(const std::string& output_guide_path) const;

private:
    const DefParser& def_;
    const LefParser& lef_;
    const ThermalParser& thermal_;

    // 每个 Net 对应的 Guide 矩形集合
    GuideMap routed_guides_;

    void route_net(const Net& net);
    void route_intra_die_net(const Net& net, bool is_top_die);
    void route_cross_die_net(const Net& net);

    // 辅助：生成一个 GCell 的完整矩形
    GuideRect make_cell_rect(int col, int row, int layer_idx) const;

    // 辅助：在两个 GCell 之间走 L 形或直行路径
    void add_path_between_cells(std::vector<GuideRect>& guides,
                                int c1, int r1, int layer1,
                                int c2, int r2, int layer2) const;

    // 当前只去除完全重复矩形；真正的区间合并待实现。
    static std::vector<GuideRect> deduplicate_guides(const std::vector<GuideRect>& raw_guides);
};

} // namespace eda
