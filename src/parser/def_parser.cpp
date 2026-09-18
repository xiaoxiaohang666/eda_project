#include "parser/def_parser.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <set>
#include <algorithm>

namespace eda {

bool DefParser::parse(const std::string& def_file_path, const LefParser& lef_parser) {
    std::ifstream fin(def_file_path);
    if (!fin.is_open()) {
        std::cerr << "[ERROR] 无法打开 DEF 文件: " << def_file_path << "\n";
        return false;
    }

    std::string line;
    std::vector<std::tuple<double, int, double>> gcell_x_stmts;
    std::vector<std::tuple<double, int, double>> gcell_y_stmts;

    enum class Section {
        NONE,
        COMPONENTS,
        PINS,
        NETS
    };

    Section current_section = Section::NONE;
    bool parsing_net = false;
    Net current_net;

    while (std::getline(fin, line)) {
        if (line.empty()) continue;
        size_t comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }
        std::istringstream iss(line);
        std::string token;
        if (!(iss >> token)) continue;

        if (token == "UNITS") {
            std::string dist, mic;
            double val;
            if (iss >> dist >> mic >> val) {
                dbu_per_micron_ = val;
            }
        } else if (token == "DIEAREA") {
            char p1, p2, p3, p4;
            double x1, y1, x2, y2;
            if (iss >> p1 >> x1 >> y1 >> p2 >> p3 >> x2 >> y2 >> p4) {
                die_area_ = Rect(x1, y1, x2, y2);
            }
        } else if (token == "GCELLGRID") {
            std::string axis, do_kw, step_kw;
            double start, step;
            int count;
            if (iss >> axis >> start >> do_kw >> count >> step_kw >> step) {
                if (axis == "X") {
                    gcell_x_stmts.emplace_back(start, count, step);
                } else if (axis == "Y") {
                    gcell_y_stmts.emplace_back(start, count, step);
                }
            }
        } else if (token == "COMPONENTS") {
            current_section = Section::COMPONENTS;
        } else if (token == "END" && current_section == Section::COMPONENTS) {
            std::string end_kw;
            iss >> end_kw;
            if (end_kw == "COMPONENTS") current_section = Section::NONE;
        } else if (token == "PINS") {
            current_section = Section::PINS;
        } else if (token == "END" && current_section == Section::PINS) {
            std::string end_kw;
            iss >> end_kw;
            if (end_kw == "PINS") current_section = Section::NONE;
        } else if (token == "NETS") {
            current_section = Section::NETS;
        } else if (token == "END" && current_section == Section::NETS) {
            std::string end_kw;
            iss >> end_kw;
            if (end_kw == "NETS") {
                if (parsing_net) {
                    nets_.push_back(current_net);
                    parsing_net = false;
                }
                current_section = Section::NONE;
            }
        } else if (current_section == Section::COMPONENTS) {
            static Component comp;
            if (token == "-") {
                iss >> comp.name >> comp.macro_type;
            }
            if (line.find("PLACED") != std::string::npos || line.find("FIXED") != std::string::npos) {
                size_t p_pos = line.find('(');
                if (p_pos != std::string::npos) {
                    std::string sub = line.substr(p_pos + 1);
                    std::istringstream ssub(sub);
                    double cx, cy;
                    char p_end;
                    std::string orient;
                    if (ssub >> cx >> cy >> p_end >> orient) {
                        comp.origin = Point(cx, cy);
                        comp.orient = orient;
                        components_[comp.name] = comp;
                    }
                }
            }
        } else if (current_section == Section::PINS) {
            static Pin pin;
            if (token == "-") {
                iss >> pin.name;
            }
            if (line.find("LAYER") != std::string::npos) {
                size_t l_pos = line.find("LAYER");
                std::istringstream liss(line.substr(l_pos + 5));
                liss >> pin.layer;
                pin.layer_idx = get_layer_index(pin.layer);
            }
            if (line.find("PLACED") != std::string::npos || line.find("FIXED") != std::string::npos) {
                size_t p_pos = line.find('(');
                if (p_pos != std::string::npos) {
                    std::string sub = line.substr(p_pos + 1);
                    std::istringstream ssub(sub);
                    double px, py;
                    char p_end;
                    std::string orient;
                    if (ssub >> px >> py >> p_end >> orient) {
                        pin.coord = Point(px, py);
                        io_pins_[pin.name] = pin;
                    }
                }
            }
        } else if (current_section == Section::NETS) {
            if (token == "-") {
                if (parsing_net) {
                    nets_.push_back(current_net);
                }
                current_net = Net();
                iss >> current_net.name;
                parsing_net = true;
            }

            if (parsing_net) {
                // 逐个提取 ( inst pin )
                size_t pos = 0;
                while ((pos = line.find('(', pos)) != std::string::npos) {
                    size_t end_pos = line.find(')', pos);
                    if (end_pos == std::string::npos) break;
                    std::string pin_str = line.substr(pos + 1, end_pos - pos - 1);
                    std::istringstream piss(pin_str);
                    std::string p1, p2;
                    if (piss >> p1 >> p2) {
                        Pin pin;
                        if (p1 == "PIN") {
                            auto it = io_pins_.find(p2);
                            if (it != io_pins_.end()) {
                                pin = it->second;
                            } else {
                                pin.name = p2;
                            }
                        } else {
                            pin.name = p1 + "." + p2;
                            auto cit = components_.find(p1);
                            if (cit != components_.end()) {
                                const Component& c = cit->second;
                                pin.coord = c.origin;
                                const MacroInfo* macro = lef_parser.get_macro(c.macro_type);
                                if (macro) {
                                    auto pit = macro->pins.find(p2);
                                    if (pit != macro->pins.end()) {
                                        pin.layer = pit->second.layer;
                                        pin.layer_idx = get_layer_index(pin.layer);
                                        Point offset = pit->second.bbox.center();
                                        pin.coord.x += offset.x * dbu_per_micron_;
                                        pin.coord.y += offset.y * dbu_per_micron_;
                                    }
                                }
                            }
                            if (pin.layer.empty()) {
                                if (p1.find("top") != std::string::npos) {
                                    pin.layer = "metal19";
                                    pin.layer_idx = 19;
                                } else {
                                    pin.layer = "metal1";
                                    pin.layer_idx = 1;
                                }
                            }
                        }
                        current_net.pins.push_back(pin);
                    }
                    pos = end_pos + 1;
                }

                if (line.find(';') != std::string::npos) {
                    nets_.push_back(current_net);
                    parsing_net = false;
                }
            }
        }
    }

    // 构建 GCellGrid
    std::set<double> x_set;
    for (const auto& stmt : gcell_x_stmts) {
        double start = std::get<0>(stmt);
        int count = std::get<1>(stmt);
        double step = std::get<2>(stmt);
        for (int i = 0; i < count; ++i) {
            x_set.insert(start + i * step);
        }
    }
    std::set<double> y_set;
    for (const auto& stmt : gcell_y_stmts) {
        double start = std::get<0>(stmt);
        int count = std::get<1>(stmt);
        double step = std::get<2>(stmt);
        for (int i = 0; i < count; ++i) {
            y_set.insert(start + i * step);
        }
    }

    grid_.x_lines.assign(x_set.begin(), x_set.end());
    grid_.y_lines.assign(y_set.begin(), y_set.end());

    if (!grid_.x_lines.empty() && grid_.x_lines.back() < die_area_.x2) {
        grid_.x_lines.push_back(die_area_.x2);
    }
    if (!grid_.y_lines.empty() && grid_.y_lines.back() < die_area_.y2) {
        grid_.y_lines.push_back(die_area_.y2);
    }

    grid_.cols = static_cast<int>(grid_.x_lines.size()) - 1;
    grid_.rows = static_cast<int>(grid_.y_lines.size()) - 1;

    for (auto& net : nets_) {
        bool has_bottom = false;
        bool has_top = false;
        for (auto& pin : net.pins) {
            pin.gcell_col = grid_.get_col(pin.coord.x);
            pin.gcell_row = grid_.get_row(pin.coord.y);
            if (pin.layer_idx <= 10) has_bottom = true;
            if (pin.layer_idx >= 11) has_top = true;
        }
        net.is_cross_die = (has_bottom && has_top);
    }

    std::cout << "[INFO] DEF 解析完毕: 实例 " << components_.size() 
              << " 个, IO引脚 " << io_pins_.size() 
              << " 个, 网络 " << nets_.size() 
              << " 个, GCell 网格 " << grid_.cols << "x" << grid_.rows << "\n";

    return true;
}

} // namespace eda
