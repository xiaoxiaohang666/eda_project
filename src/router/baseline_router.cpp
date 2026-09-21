#include "router/baseline_router.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <iomanip>
#include <filesystem>
#include <tuple>

namespace eda {

BaselineRouter::BaselineRouter(const DefParser& def_parser,
                           const LefParser& lef_parser,
                           const ThermalParser& thermal_parser)
    : def_(def_parser), lef_(lef_parser), thermal_(thermal_parser) {}

GuideRect BaselineRouter::make_cell_rect(int col, int row, int layer_idx) const {
    const auto& grid = def_.gcell_grid();
    const auto cell = grid.get_cell_rect(col, row);
    if (layer_idx < 1 || layer_idx > 20) throw std::runtime_error("invalid guide layer");
    return GuideRect(cell.x1, cell.y1, cell.x2, cell.y2, get_layer_name(layer_idx));
}

void BaselineRouter::add_path_between_cells(std::vector<GuideRect>& guides,
                                         int c1, int r1, int layer1,
                                         int c2, int r2, int layer2) const {
    int base_h_layer = (layer1 <= 10) ? 1 : 11;
    int base_v_layer = (layer1 <= 10) ? 2 : 12;

    // 起点垂直跳层到基础层
    int min_l = std::min(layer1, base_h_layer);
    int max_l = std::max(layer1, base_h_layer);
    for (int l = min_l; l <= max_l; ++l) {
        guides.push_back(make_cell_rect(c1, r1, l));
    }
    guides.push_back(make_cell_rect(c1, r1, base_v_layer));

    // 水平段：在 base_h_layer 上走
    int min_c = std::min(c1, c2);
    int max_c = std::max(c1, c2);
    for (int c = min_c; c <= max_c; ++c) {
        guides.push_back(make_cell_rect(c, r1, base_h_layer));
    }

    // 拐角 (c2, r1) 连接水平与垂直层
    guides.push_back(make_cell_rect(c2, r1, base_h_layer));
    guides.push_back(make_cell_rect(c2, r1, base_v_layer));

    // 垂直段：在 base_v_layer 上走
    int min_r = std::min(r1, r2);
    int max_r = std::max(r1, r2);
    for (int r = min_r; r <= max_r; ++r) {
        guides.push_back(make_cell_rect(c2, r, base_v_layer));
    }

    // 终点垂直跳层到目标引脚层
    int min_l2 = std::min(layer2, base_v_layer);
    int max_l2 = std::max(layer2, base_v_layer);
    for (int l = min_l2; l <= max_l2; ++l) {
        guides.push_back(make_cell_rect(c2, r2, l));
    }
}

void BaselineRouter::route_intra_die_net(const Net& net, bool is_top_die) {
    (void)is_top_die;
    std::vector<GuideRect> guides;
    if (net.pins.empty()) return;

    // 强制覆盖所有引脚所在 GCell 与层
    for (const auto& p : net.pins) {
        guides.push_back(make_cell_rect(p.gcell_col, p.gcell_row, p.layer_idx));
    }

    if (net.pins.size() == 1) {
        routed_guides_[net.name] = deduplicate_guides(guides);
        return;
    }

    const auto& root = net.pins[0];
    for (size_t i = 1; i < net.pins.size(); ++i) {
        const auto& target = net.pins[i];
        add_path_between_cells(guides,
                               root.gcell_col, root.gcell_row, root.layer_idx,
                               target.gcell_col, target.gcell_row, target.layer_idx);
    }

    routed_guides_[net.name] = deduplicate_guides(guides);
}

void BaselineRouter::route_cross_die_net(const Net& net) {
    std::vector<GuideRect> guides;
    std::vector<Pin> bottom_pins;
    std::vector<Pin> top_pins;

    // 强制覆盖所有引脚所在层与格点
    for (const auto& p : net.pins) {
        guides.push_back(make_cell_rect(p.gcell_col, p.gcell_row, p.layer_idx));
        if (p.layer_idx <= 10) bottom_pins.push_back(p);
        else top_pins.push_back(p);
    }

    if (bottom_pins.empty() || top_pins.empty()) {
        route_intra_die_net(net, bottom_pins.empty());
        return;
    }

    // 热感知 TSV 选点：优先选择与引脚较近且处于全芯片高温区域的格点
    double avg_c = 0.0, avg_r = 0.0;
    for (const auto& p : net.pins) {
        avg_c += p.gcell_col;
        avg_r += p.gcell_row;
    }
    avg_c /= net.pins.size();
    avg_r /= net.pins.size();

    int best_c = static_cast<int>(std::round(avg_c));
    int best_r = static_cast<int>(std::round(avg_r));
    double best_score = -1e9;

    const auto& hotspots = thermal_.hotspots();
    if (!hotspots.empty()) {
        for (const auto& hs : hotspots) {
            double dist = std::abs(hs.col - avg_c) + std::abs(hs.row - avg_r);
            double score = hs.delta_t * 10000.0 - dist * 0.15;
            if (score > best_score) {
                best_score = score;
                best_c = hs.col;
                best_r = hs.row;
            }
        }
    }

    // 跨 Die TSV 核心：必须在同一个 GCell 输出 metal10 与 metal11
    guides.push_back(make_cell_rect(best_c, best_r, 10));
    guides.push_back(make_cell_rect(best_c, best_r, 11));

    // Bottom Die 侧连接
    for (const auto& bp : bottom_pins) {
        for (int l = bp.layer_idx; l <= 10; ++l) {
            guides.push_back(make_cell_rect(bp.gcell_col, bp.gcell_row, l));
        }
        int min_c = std::min(bp.gcell_col, best_c);
        int max_c = std::max(bp.gcell_col, best_c);
        for (int c = min_c; c <= max_c; ++c) {
            guides.push_back(make_cell_rect(c, bp.gcell_row, 9));
        }
        guides.push_back(make_cell_rect(best_c, bp.gcell_row, 9));
        guides.push_back(make_cell_rect(best_c, bp.gcell_row, 10));

        int min_r = std::min(bp.gcell_row, best_r);
        int max_r = std::max(bp.gcell_row, best_r);
        for (int r = min_r; r <= max_r; ++r) {
            guides.push_back(make_cell_rect(best_c, r, 10));
        }
    }

    // Top Die 侧连接
    for (const auto& tp : top_pins) {
        int min_c = std::min(best_c, tp.gcell_col);
        int max_c = std::max(best_c, tp.gcell_col);
        for (int c = min_c; c <= max_c; ++c) {
            guides.push_back(make_cell_rect(c, best_r, 11));
        }
        guides.push_back(make_cell_rect(tp.gcell_col, best_r, 11));
        guides.push_back(make_cell_rect(tp.gcell_col, best_r, 12));

        int min_r = std::min(best_r, tp.gcell_row);
        int max_r = std::max(best_r, tp.gcell_row);
        for (int r = min_r; r <= max_r; ++r) {
            guides.push_back(make_cell_rect(tp.gcell_col, r, 12));
        }

        int min_l = std::min(12, tp.layer_idx);
        int max_l = std::max(12, tp.layer_idx);
        for (int l = min_l; l <= max_l; ++l) {
            guides.push_back(make_cell_rect(tp.gcell_col, tp.gcell_row, l));
        }
    }

    routed_guides_[net.name] = deduplicate_guides(guides);
}

void BaselineRouter::route_net(const Net& net) {
    if (net.is_cross_die) {
        route_cross_die_net(net);
    } else {
        bool is_top = net.has_top_pin();
        route_intra_die_net(net, is_top);
    }
}

bool BaselineRouter::route_all() {
    routed_guides_.clear();
    if (!def_.gcell_grid().valid() || def_.nets().empty()) {
        std::cerr << "[ERROR] Cannot route an empty or invalid design\n";
        return false;
    }
    const auto& nets = def_.nets();
    std::cout << "[INFO] 开始执行基线布线, 总网络数: " << nets.size() << " ...\n";

    int cross_count = 0;
    try {
        for (const auto& net : nets) {
            route_net(net);
            if (net.is_cross_die) cross_count++;
        }
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << "\n";
        routed_guides_.clear();
        return false;
    }
    std::string error;
    if (!validate_guides(def_, lef_, routed_guides_, error)) {
        std::cerr << "[ERROR] Guide validation: " << error << "\n";
        routed_guides_.clear();
        return false;
    }

    std::cout << "[INFO] 全局布线完成! 跨 Die 网络: " << cross_count 
              << ", 完成网络: " << routed_guides_.size() << "\n";
    return true;
}

std::vector<GuideRect> BaselineRouter::deduplicate_guides(const std::vector<GuideRect>& raw_guides) {
    // Preserve the baseline's layer/rectangle traversal: the package scorer's
    // MST tie breaking can change its length proxy after a pure reordering.
    std::unordered_map<std::string, std::vector<Rect>> layer_rects;
    for (const auto& g : raw_guides)
        layer_rects[g.layer].emplace_back(g.x1, g.y1, g.x2, g.y2);
    std::vector<GuideRect> unique;
    for (const auto& [layer, rects] : layer_rects) {
        std::set<std::tuple<double, double, double, double>> seen;
        for (const auto& r : rects) {
            if (seen.emplace(r.x1, r.y1, r.x2, r.y2).second)
                unique.emplace_back(r.x1, r.y1, r.x2, r.y2, layer);
        }
    }
    return unique;
}

bool BaselineRouter::export_guide(const std::string& output_guide_path) const {
    std::string validation_error;
    if (!validate_guides(def_, lef_, routed_guides_, validation_error)) {
        std::cerr << "[ERROR] Cannot export: " << validation_error << "\n";
        return false;
    }
    const auto parent = std::filesystem::path(output_guide_path).parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            std::cerr << "[ERROR] 无法创建 Guide 输出目录: " << parent << ": "
                      << error.message() << "\n";
            return false;
        }
    }
    std::ofstream fout(output_guide_path);
    if (!fout.is_open()) {
        std::cerr << "[ERROR] 无法写入 Guide 输出文件: " << output_guide_path << "\n";
        return false;
    }

    for (const auto& [net_name, rects] : routed_guides_) {
        fout << net_name << "\n(\n";
        for (const auto& r : rects) {
            fout << std::fixed << std::setprecision(0)
                 << r.x1 << " " << r.y1 << " " << r.x2 << " " << r.y2 << " " << r.layer << "\n";
        }
        fout << ")\n";
    }

    fout.close();
    if (!fout) {
        std::cerr << "[ERROR] Failed writing/flushing Guide: " << output_guide_path << "\n";
        return false;
    }
    std::cout << "[INFO] Route Guide 导出成功: " << output_guide_path << "\n";
    return true;
}

} // namespace eda
