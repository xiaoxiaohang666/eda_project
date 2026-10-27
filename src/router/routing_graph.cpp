#include "router/routing_graph.hpp"
#include "scoring/compat_resources.hpp"

#include <limits>
#include <set>
#include <iostream>
#include <unordered_map>

namespace eda {
namespace {
bool overlaps(const Rect& a, const Rect& b) {
    return a.x1 < b.x2 && a.x2 > b.x1 && a.y1 < b.y2 && a.y2 > b.y1;
}
size_t count_interval(const std::vector<double>& values, double lo, double hi, bool last) {
    const auto first = std::lower_bound(values.begin(), values.end(), lo);
    const auto end = last ? std::upper_bound(values.begin(), values.end(), hi)
                          : std::lower_bound(values.begin(), values.end(), hi);
    return static_cast<size_t>(end - first);
}
bool covered_by_port(const Rect& rect, int layer, const Pin& pin) {
    std::vector<Rect> remaining{rect};
    for(const auto& shape:pin.port_rects) {
        if(shape.layer_idx!=layer) continue;
        std::vector<Rect> next;
        for(const auto& piece:remaining) {
            if(!overlaps(piece,shape.rect)) {next.push_back(piece);continue;}
            const double x1=std::max(piece.x1,shape.rect.x1),x2=std::min(piece.x2,shape.rect.x2),
                         y1=std::max(piece.y1,shape.rect.y1),y2=std::min(piece.y2,shape.rect.y2);
            if(piece.x1<x1) next.emplace_back(piece.x1,piece.y1,x1,piece.y2);
            if(x2<piece.x2) next.emplace_back(x2,piece.y1,piece.x2,piece.y2);
            if(piece.y1<y1) next.emplace_back(x1,piece.y1,x2,y1);
            if(y2<piece.y2) next.emplace_back(x1,y2,x2,piece.y2);
        }
        remaining=std::move(next);if(remaining.empty()) return true;
    }
    return remaining.empty();
}
// These reproduce the new official Python bisect and interval rules, even
// for the scorer's unsorted concatenation of multiple TRACKS patterns. Sorting
// or deduplicating that list here would silently change its OBS accounting.
int official_bisect(const std::vector<double>& values,double value,bool right) {
    size_t lo=0,hi=values.size();
    while(lo<hi) {
        const size_t mid=lo+(hi-lo)/2;
        if(right ? value<values[mid] : !(values[mid]<value)) hi=mid;
        else lo=mid+1;
    }
    return static_cast<int>(lo);
}
int official_locate(const std::vector<double>& lines,double coordinate) {
    if(lines.empty()) return -1;
    if(coordinate<lines.front()) return 0;
    if(coordinate>lines.back()) return static_cast<int>(lines.size())-2;
    const int index=official_bisect(lines,coordinate,true)-1;
    if(index<0) return 0;
    return std::min(index,static_cast<int>(lines.size())-2);
}
std::pair<int,int> official_intervals(double lo,double hi,const std::vector<double>& lines) {
    if(lines.empty()) return {0,0};
    if(hi<lo) std::swap(lo,hi);
    if(std::abs(hi-lo)<=1e-9*std::max(std::abs(lo),std::abs(hi))) {
        const int index=official_locate(lines,lo);return {index,index+1};
    }
    return {std::max(0,official_bisect(lines,lo,true)-1),
            std::min(static_cast<int>(lines.size())-1,official_bisect(lines,hi,false))};
}
bool same_die(int a, int b) { return (a <= 10) == (b <= 10); }
} // namespace

RoutingGraph::RoutingGraph(const DefParser& def, const LefParser& lef, RoutingGraphOptions options)
    : options_(options), grid_(def.gcell_grid()), die_(def.die_area()), dbu_(def.dbu_per_micron()) {
    if (options_.platform_20261024 && !options_.score_20261020)
        throw std::runtime_error("platform resources require the October 20 scoring profile");
    if (options_.score_20261020) {
        if (!global_guide_mode())
            throw std::runtime_error("October 20 scoring profile requires GLOBAL_GUIDE geometry mode");
        options_.allow_nonpreferred = false;
    }
    if (!grid_.valid() || dbu_ <= 0) throw std::runtime_error("routing graph requires a valid DEF grid");
    plane_ = static_cast<size_t>(grid_.cols) * static_cast<size_t>(grid_.rows);
    if (plane_ > static_cast<size_t>(std::numeric_limits<int>::max()) / 20)
        throw std::runtime_error("routing graph exceeds integer node limit");
    std::vector<double> steps;
    for (const auto* lines : {&grid_.x_lines, &grid_.y_lines})
        for (size_t i = 1; i < lines->size(); ++i) steps.push_back((*lines)[i] - (*lines)[i - 1]);
    std::sort(steps.begin(), steps.end()); ell0_ = steps[steps.size() / 2];
    for (int l = 1; l <= 20; ++l) {
        const auto* info = lef.get_layer(get_layer_name(l));
        if (!info || info->direction == Direction::UNKNOWN || info->width <= 0)
            throw std::runtime_error("routing graph needs DIRECTION and WIDTH for " + get_layer_name(l));
        directions_[l] = info->direction; widths_[l] = info->width * dbu_;
    }
    for (const auto& pattern : def.tracks()) {
        const int axis = pattern.axis == 'X' ? 0 : 1;
        const double lo = axis == 0 ? die_.x1 : die_.y1;
        const double hi = axis == 0 ? die_.x2 : die_.y2;
        const double first = std::max(0.0, std::ceil((lo - pattern.start) / pattern.step));
        const double last = std::min(static_cast<double>(pattern.count - 1),
                                     std::floor((hi - pattern.start) / pattern.step));
        if (last - first > 10000000) throw std::runtime_error("TRACKS expansion exceeds supported size");
        for (const auto& name : pattern.layers) {
            const int l = get_layer_index(name);
            for (double i = first; i <= last; ++i)
                scorer_tracks_[l][axis].push_back(pattern.start + i * pattern.step);
        }
    }
    for (int l = 1; l <= 20; ++l) {
        for (int axis = 0; axis < 2; ++axis) {
            auto& tracks = scorer_tracks_[l][axis];
            std::sort(tracks.begin(), tracks.end());
            physical_tracks_[l][axis] = tracks;
            auto& unique = physical_tracks_[l][axis];
            unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        }
        const int physical_axis = directions_[l] == Direction::HORIZONTAL ? 1 : 0;
        const int scorer_axis = 1 - physical_axis;
        if (physical_tracks_[l][physical_axis].empty() && !score_20261020())
            throw std::runtime_error("routing graph needs preferred-direction physical TRACKS for " + get_layer_name(l));
        physical_guide_mode_[l] = global_guide_mode() || scorer_tracks_[l][scorer_axis].empty();
        guide_axes_[l] = physical_guide_mode_[l] ? physical_axis : scorer_axis;
        if (physical_guide_mode_[l])
            std::cout << "[RESOURCE] " << get_layer_name(l)
                      << " guide_capacity=physical_tracks axis=" << (physical_axis == 0 ? 'X' : 'Y')
                      << (global_guide_mode()?" (official global-Guide mode)\n":" (legacy scorer axis absent)\n");
        const int axis = guide_axes_[l];
        const auto& values = physical_guide_mode_[l] ? physical_tracks_[l][axis] : scorer_tracks_[l][axis];
        const auto& lines = axis == 0 ? grid_.x_lines : grid_.y_lines;
        auto& capacities = guide_capacity_[l];
        capacities.resize(lines.size() - 1);
        for (size_t i = 0; i < capacities.size(); ++i)
            capacities[i] = static_cast<double>(count_interval(values, lines[i], lines[i + 1], i + 1 == capacities.size()));
    }
    if(global_guide_mode()) {
        if(score_20261020()) {
            compat::ResourceOptions resources;
            resources.platform_20261024=platform_resources();
            updated_resources_=std::make_shared<compat::Resources>(def,lef,resources);
        }
        else build_official_resources(def,lef);
        legal_.assign(20*plane_,1);via_up_.assign(size(),0);planar_forward_.assign(size(),0);
        for(int id=0;id<static_cast<int>(size());++id) {
            const auto box=cell(id);
            legal_[id]=std::min(box.x2,die_.x2)>std::max(box.x1,die_.x1) &&
                       std::min(box.y2,die_.y2)>std::max(box.y1,die_.y1);
        }
        for(int id=0;id<static_cast<int>(size());++id) {
            if(!legal_node(id)) continue;
            const int l=layer(id),c=col(id),r=row(id);
            const int next=directions_[l]==Direction::HORIZONTAL?node(c+1,r,l):node(c,r+1,l);
            if(legal_node(next)) {
                // A full cell is still routable with overflow. The planar
                // edge is an abstract adjacency; exact congestion is charged
                // against the official per-GCell residual capacity below.
                const double residual=std::min(guide_capacity(id),guide_capacity(next));
                planar_forward_[id]=static_cast<uint32_t>(std::max(1.0,std::min(residual,double(std::numeric_limits<uint32_t>::max()))));
            }
            if(l<20 && same_die(l,l+1) && legal_node(node(c,r,l+1))) via_up_[id]=1;
        }
        return;
    }
    for (const auto& [name, component] : def.components()) {
        (void)name;
        const auto* macro = lef.get_macro(component.macro_type);
        if (!macro) throw std::runtime_error("missing component macro in routing graph");
        for (const auto& shape : macro->obstructions) {
            const int l = get_layer_index(shape.layer);
            if (!l) continue; // CUT/master-slice geometry has no planar graph layer.
            obstacles_[l].push_back(transform_macro_rect(shape.rect, *macro, component, dbu_));
            obstacle_owners_[l].push_back(&name);
        }
    }
    for (int l = 1; l <= 20; ++l) {
        if (obstacles_[l].empty()) continue;
        if (obstacles_[l].size() > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("routing graph OBS index exceeds uint32 limit");
        auto& offsets = obstacle_offsets_[l];
        offsets.resize(plane_ + 1);
        auto visit_cells = [&](const Rect& rect, auto&& visit) {
            if (!overlaps(rect, die_)) return;
            const int c0 = grid_.get_col(std::max(rect.x1, die_.x1));
            const int c1 = grid_.get_col(std::min(rect.x2, die_.x2));
            const int r0 = grid_.get_row(std::max(rect.y1, die_.y1));
            const int r1 = grid_.get_row(std::min(rect.y2, die_.y2));
            for (int r = r0; r <= r1; ++r)
                for (int c = c0; c <= c1; ++c)
                    visit(static_cast<size_t>(r) * grid_.cols + c);
        };
        for (const auto& rect : obstacles_[l]) visit_cells(rect, [&](size_t cell) {
            if (offsets[cell + 1] == std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("routing graph OBS bucket exceeds uint32 limit");
            ++offsets[cell + 1];
        });
        uint64_t total = 0;
        for (size_t cell = 1; cell < offsets.size(); ++cell) {
            total += offsets[cell];
            if (total > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("routing graph OBS references exceed uint32 limit");
            offsets[cell] = static_cast<uint32_t>(total);
        }
        auto& indices = obstacle_indices_[l];
        indices.resize(static_cast<size_t>(total));
        // This cursor is temporary for one layer; no dense vector headers or
        // per-bucket allocation remain after construction.
        std::vector<uint32_t> cursor(offsets.begin(), offsets.end() - 1);
        for (size_t index = 0; index < obstacles_[l].size(); ++index)
            visit_cells(obstacles_[l][index], [&](size_t cell) {
                indices[cursor[cell]++] = static_cast<uint32_t>(index);
            });
    }
    for (const auto& [name, via] : lef.vias()) {
        (void)name;
        // This graph does not enforce the position of a via within a stack.
        // A top-of-stack-only definition cannot be an unrestricted edge.
        if (via.top_of_stack_only) continue;
        std::set<int> metal_layers;
        bool cut = false;
        for (const auto& shape : via.shapes) {
            const int l = get_layer_index(shape.layer);
            if (l) metal_layers.insert(l);
            else if (const auto* info = lef.get_layer(shape.layer); info && info->type == "CUT") cut = true;
        }
        if (metal_layers.size() != 2 || !cut) continue;
        const int low = *metal_layers.begin(), high = *metal_layers.rbegin();
        if (high == low + 1 && same_die(low, high)) vias_[low].push_back(&via);
    }
    for (auto& list : vias_) {
        std::sort(list.begin(), list.end(), [](const ViaInfo* a, const ViaInfo* b) {
            if (a->is_default != b->is_default) return a->is_default > b->is_default;
            return a->name < b->name;
        });
    }
    legal_.resize(20 * plane_); via_up_.resize(size());
    planar_forward_.resize(size());
    for (int id = 0; id < static_cast<int>(size()); ++id) {
        const int l = layer(id); const auto p = point(id);
        legal_[id] = legal_segment(p, p, l);
    }
    for (int id = 0; id < static_cast<int>(size()); ++id) {
        if (!legal_[id]) continue;
        const int l = layer(id), c = col(id), r = row(id);
        const int next = directions_[l] == Direction::HORIZONTAL ? node(c + 1, r, l) : node(c, r + 1, l);
        if (legal_node(next) && legal_segment(point(id), point(next), l))
            planar_forward_[id] = physical_capacity(id, next);
        const int above = node(c, r, l + 1);
        if (legal_node(above) && same_die(l, l + 1)) via_up_[id] = legal_via(point(id), l);
    }
}

void RoutingGraph::build_official_resources(const DefParser& def,const LefParser& lef) {
    std::array<std::array<std::vector<double>,2>,21> raw_tracks;
    for(const auto& pattern:def.tracks()) {
        if(pattern.count>10000000) throw std::runtime_error("official TRACKS expansion exceeds supported size");
        const int axis=pattern.axis=='X'?0:1;
        for(const auto& name:pattern.layers) {
            const int l=get_layer_index(name);if(!l) continue;
            auto& values=raw_tracks[l][axis];
            for(int i=0;i<pattern.count;++i) values.push_back(pattern.start+i*pattern.step);
        }
    }
    for(int l=1;l<=20;++l) {
        const int axis=directions_[l]==Direction::HORIZONTAL?1:0;
        const auto& lines=axis?grid_.y_lines:grid_.x_lines;
        auto& capacity=official_capacity_[l];capacity.assign(lines.size()-1,0);
        for(double track:raw_tracks[l][axis]) {
            const int index=official_locate(lines,track);
            if(index>=0 && size_t(index)<capacity.size()) ++capacity[index];
        }
    }
    std::array<std::vector<Rect>,21> occupied_geometry;
    for(const auto& [name,component]:def.components()) {
        (void)name;const auto* macro=lef.get_macro(component.macro_type);
        if(!macro) throw std::runtime_error("missing macro in official OBS/PIN capacity model");
        for(const auto& shape:macro->obstructions) {
            const int l=get_layer_index(shape.layer);if(l)
                occupied_geometry[l].push_back(transform_macro_rect(shape.rect,*macro,component,dbu_));
        }
        for(const auto& [pin_name,pin]:macro->pins) {
            (void)pin_name;
            // The distributed scorer assigns every PORT geometry of a Pin to
            // the layer of its first geometry. Preserve that scoring contract;
            // this is separate from the actual multilayer terminal geometry.
            const int l=get_layer_index(pin.layer);if(!l) continue;
            for(const auto& shape:pin.port_rects)
                occupied_geometry[l].push_back(transform_macro_rect(shape.rect,*macro,component,dbu_));
        }
    }
    official_obstacle_demand_.assign(20*plane_,0);
    for(int l=1;l<=20;++l) {
        const int axis=directions_[l]==Direction::HORIZONTAL?1:0;
        const auto& tracks=raw_tracks[l][axis];
        // Interval union is equivalent to the scorer's per-cell set of track
        // indices, without allocating a tree/set node for every occupied track.
        std::unordered_map<size_t,std::vector<std::pair<int,int>>> ranges;
        for(const auto& rect:occupied_geometry[l]) {
            const auto [cb,ce]=official_intervals(rect.x1,rect.x2,grid_.x_lines);
            const auto [rb,re]=official_intervals(rect.y1,rect.y2,grid_.y_lines);
            for(int r=rb;r<re;++r) for(int c=cb;c<ce;++c) {
                const auto box=grid_.get_cell_rect(c,r);
                const double lo=axis?std::max(rect.y1,box.y1):std::max(rect.x1,box.x1);
                const double hi=axis?std::min(rect.y2,box.y2):std::min(rect.x2,box.x2);
                const auto interval=official_intervals(lo,hi,tracks);
                if(interval.first<interval.second)
                    ranges[static_cast<size_t>(r)*grid_.cols+c].push_back(interval);
            }
        }
        for(auto& [cell,intervals]:ranges) {
            std::sort(intervals.begin(),intervals.end());
            uint64_t count=0;int begin=intervals.front().first,end=intervals.front().second;
            for(size_t i=1;i<intervals.size();++i) {
                const auto [a,b]=intervals[i];
                if(a<=end) end=std::max(end,b);
                else {count+=static_cast<uint64_t>(end-begin);begin=a;end=b;}
            }
            count+=static_cast<uint64_t>(end-begin);
            if(count>std::numeric_limits<uint32_t>::max()) throw std::runtime_error("official OBS track demand overflow");
            official_obstacle_demand_[static_cast<size_t>(l-1)*plane_+cell]=static_cast<uint32_t>(count);
        }
        std::vector<Rect>().swap(occupied_geometry[l]);
    }
}

double RoutingGraph::official_capacity(int id) const {
    if(!global_guide_mode()) throw std::logic_error("official capacity requires GLOBAL_GUIDE geometry mode");
    if(!valid_id(id)) throw std::out_of_range("invalid official capacity node");
    if(updated_resources_) return updated_resources_->capacity(id);
    const int l=layer(id);return official_capacity_[l][directions_[l]==Direction::HORIZONTAL?row(id):col(id)];
}
uint32_t RoutingGraph::official_obstacle_demand(int id) const {
    if(!global_guide_mode()) throw std::logic_error("official obstacle demand requires GLOBAL_GUIDE geometry mode");
    if(!valid_id(id)) throw std::out_of_range("invalid official obstacle node");
    if(updated_resources_) return updated_resources_->fixed_demand(id);
    return official_obstacle_demand_[id];
}
const std::vector<uint32_t>& RoutingGraph::official_obstacle_demands() const {
    if(!global_guide_mode()) throw std::logic_error("official obstacle demand requires GLOBAL_GUIDE geometry mode");
    if(updated_resources_) return updated_resources_->fixed_demands();
    return official_obstacle_demand_;
}
std::vector<int> RoutingGraph::own_pin_nodes(const Net& net) const {
    return updated_resources_ ? updated_resources_->pin_nodes(net) : std::vector<int>{};
}

bool RoutingGraph::valid_id(int id) const { return id >= 0 && static_cast<size_t>(id) < size(); }
int RoutingGraph::node(int c, int r, int l) const {
    if (c < 0 || c >= grid_.cols || r < 0 || r >= grid_.rows || l < 1 || l > 20) return -1;
    return static_cast<int>(static_cast<size_t>(l - 1) * plane_ + static_cast<size_t>(r) * grid_.cols + c);
}
int RoutingGraph::col(int id) const {
    if (!valid_id(id)) throw std::out_of_range("invalid routing node");
    return id % grid_.cols;
}
int RoutingGraph::row(int id) const {
    if (!valid_id(id)) throw std::out_of_range("invalid routing node");
    return static_cast<int>((static_cast<size_t>(id) % plane_) / grid_.cols);
}
int RoutingGraph::layer(int id) const {
    if (!valid_id(id)) throw std::out_of_range("invalid routing node");
    return static_cast<int>(static_cast<size_t>(id) / plane_) + 1;
}
Point RoutingGraph::point(int id) const {
    const auto box=cell(id);
    if(global_guide_mode() && std::min(box.x2,die_.x2)>std::max(box.x1,die_.x1) &&
       std::min(box.y2,die_.y2)>std::max(box.y1,die_.y1))
        return Rect{std::max(box.x1,die_.x1),std::max(box.y1,die_.y1),
                    std::min(box.x2,die_.x2),std::min(box.y2,die_.y2)}.center();
    return box.center();
}
Rect RoutingGraph::cell(int id) const { return grid_.get_cell_rect(col(id), row(id)); }
bool RoutingGraph::legal_node(int id) const { return valid_id(id) && legal_[id] != 0; }
double RoutingGraph::routing_width(int l) const {
    if (l < 1 || l > 20) throw std::out_of_range("invalid routing layer");
    return widths_[l];
}
bool RoutingGraph::legal_rect(const Rect& rect, int l) const {
    if (l < 1 || l > 20 || !std::isfinite(rect.x1) || !std::isfinite(rect.y1) ||
        !std::isfinite(rect.x2) || !std::isfinite(rect.y2) ||
        rect.width() <= 0 || rect.height() <= 0 ||
        rect.x1 < die_.x1 || rect.y1 < die_.y1 || rect.x2 > die_.x2 || rect.y2 > die_.y2) return false;
    if (global_guide_mode() || obstacle_offsets_[l].empty()) return true;
    const int c0 = grid_.get_col(rect.x1), c1 = grid_.get_col(rect.x2);
    const int r0 = grid_.get_row(rect.y1), r1 = grid_.get_row(rect.y2);
    for (int r = r0; r <= r1; ++r)
        for (int c = c0; c <= c1; ++c) {
            const size_t bucket = static_cast<size_t>(r) * grid_.cols + c;
            for (uint32_t entry = obstacle_offsets_[l][bucket]; entry < obstacle_offsets_[l][bucket + 1]; ++entry)
                if (overlaps(rect, obstacles_[l][obstacle_indices_[l][entry]])) return false;
        }
    return true;
}
bool RoutingGraph::legal_pin_rect(const Rect& rect,int l,const Pin& pin) const {
    if(legal_rect(rect,l)) return true;
    if(l<1 || l>20 || pin.instance_name.empty() || !std::isfinite(rect.x1) || !std::isfinite(rect.y1) ||
       !std::isfinite(rect.x2) || !std::isfinite(rect.y2) || rect.width()<=0 || rect.height()<=0 ||
       rect.x1<die_.x1 || rect.y1<die_.y1 || rect.x2>die_.x2 || rect.y2>die_.y2) return false;
    if(obstacle_offsets_[l].empty()) return true;
    const int c0=grid_.get_col(rect.x1),c1=grid_.get_col(rect.x2),r0=grid_.get_row(rect.y1),r1=grid_.get_row(rect.y2);
    for(int r=r0;r<=r1;++r) for(int c=c0;c<=c1;++c) {
        size_t bucket=static_cast<size_t>(r)*grid_.cols+c;
        for(uint32_t entry=obstacle_offsets_[l][bucket];entry<obstacle_offsets_[l][bucket+1];++entry) {
            const auto index=obstacle_indices_[l][entry];const auto& obs=obstacles_[l][index];
            if(!overlaps(rect,obs)) continue;
            if(*obstacle_owners_[l][index]!=pin.instance_name) return false;
            Rect intersection(std::max(rect.x1,obs.x1),std::max(rect.y1,obs.y1),std::min(rect.x2,obs.x2),std::min(rect.y2,obs.y2));
            if(!covered_by_port(intersection,l,pin)) return false;
        }
    }
    return true;
}
Rect RoutingGraph::segment_rect(Point a, Point b, int l) const {
    const double half = routing_width(l) * 0.5;
    Rect result{std::floor(std::min(a.x, b.x) - half), std::floor(std::min(a.y, b.y) - half),
                std::ceil(std::max(a.x, b.x) + half), std::ceil(std::max(a.y, b.y) + half)};
    if(global_guide_mode()) result=Rect{std::max(die_.x1,result.x1),std::max(die_.y1,result.y1),
                                      std::min(die_.x2,result.x2),std::min(die_.y2,result.y2)};
    return result;
}
bool RoutingGraph::legal_segment(Point a, Point b, int l) const {
    if (l < 1 || l > 20 || !std::isfinite(a.x) || !std::isfinite(a.y) ||
        !std::isfinite(b.x) || !std::isfinite(b.y) || (a.x != b.x && a.y != b.y)) return false;
    if(global_guide_mode() && (!die_.contains(a)||!die_.contains(b))) return false;
    return legal_rect(segment_rect(a, b, l), l);
}
bool RoutingGraph::via_shapes(Point p, const ViaInfo& via, std::vector<LayerRect>* result, const Pin* pin) const {
    std::vector<LayerRect> rectangles;
    bool has_routing_shape = false;
    for (const auto& shape : via.shapes) {
        const int l = get_layer_index(shape.layer);
        Rect rect{std::floor(p.x + shape.rect.x1 * dbu_), std::floor(p.y + shape.rect.y1 * dbu_),
                  std::ceil(p.x + shape.rect.x2 * dbu_), std::ceil(p.y + shape.rect.y2 * dbu_)};
        if (rect.x1 < die_.x1 || rect.y1 < die_.y1 || rect.x2 > die_.x2 || rect.y2 > die_.y2) return false;
        if (!l) continue; // CUT shapes must also fit inside DIEAREA.
        if (!legal_rect(rect, l) && (!pin || !covered_by_port(rect,l,*pin) || !legal_pin_rect(rect,l,*pin))) return false;
        has_routing_shape = true;
        // Graph construction asks only about existence. Avoid allocating a
        // vector and copying layer names at every GCell for that query.
        if (result) rectangles.push_back({shape.layer, rect});
    }
    if (!has_routing_shape) return false;
    if (result) *result = std::move(rectangles);
    return true;
}
bool RoutingGraph::legal_via(Point p, int lower) const {
    if (lower < 1 || lower >= 20 || lower == 10 || !std::isfinite(p.x) || !std::isfinite(p.y)) return false;
    if(global_guide_mode()) return die_.contains(p);
    for (const auto* via : vias_[lower]) if (via_shapes(p, *via, nullptr)) return true;
    return false;
}
std::vector<LayerRect> RoutingGraph::via_rects(Point p, int lower) const {
    std::vector<LayerRect> result;
    if (lower < 1 || lower >= 20 || lower == 10 || !std::isfinite(p.x) || !std::isfinite(p.y)) return result;
    if(global_guide_mode()) {
        if(!die_.contains(p)) return result;
        result.push_back({get_layer_name(lower),segment_rect(p,p,lower)});
        result.push_back({get_layer_name(lower+1),segment_rect(p,p,lower+1)});
        return result;
    }
    for (const auto* via : vias_[lower]) if (via_shapes(p, *via, &result)) return result;
    return result;
}
bool RoutingGraph::legal_pin_via(Point p,int lower,const Pin& pin) const {
    if(lower<1 || lower>=20 || lower==10 || !std::isfinite(p.x) || !std::isfinite(p.y)) return false;
    if(global_guide_mode()) return legal_via(p,lower);
    for(const auto* via:vias_[lower]) if(via_shapes(p,*via,nullptr,&pin)) return true;
    return false;
}
std::vector<LayerRect> RoutingGraph::pin_via_rects(Point p,int lower,const Pin& pin) const {
    std::vector<LayerRect> result;
    if(lower<1 || lower>=20 || lower==10 || !std::isfinite(p.x) || !std::isfinite(p.y)) return result;
    if(global_guide_mode()) return via_rects(p,lower);
    for(const auto* via:vias_[lower]) if(via_shapes(p,*via,&result,&pin)) return result;
    return result;
}
uint32_t RoutingGraph::physical_capacity(int a, int b) const {
    const int l = layer(a); const bool horizontal = directions_[l] == Direction::HORIZONTAL;
    const auto box = cell(a); const auto pa = point(a), pb = point(b);
    const auto& tracks = physical_tracks_[l][horizontal ? 1 : 0];
    const double lo = horizontal ? box.y1 : box.x1, hi = horizontal ? box.y2 : box.x2;
    const bool last = horizontal ? row(a) == grid_.rows - 1 : col(a) == grid_.cols - 1;
    auto it = std::lower_bound(tracks.begin(), tracks.end(), lo);
    const auto end = last ? std::upper_bound(tracks.begin(), tracks.end(), hi)
                          : std::lower_bound(tracks.begin(), tracks.end(), hi);
    if (it == end) return 0;
    // Most high-layer corridors contain no OBS. A clear envelope proves all
    // contained physical tracks legal in one geometry query; obstructed or
    // die-edge envelopes retain the exact per-track check below.
    const double half = routing_width(l) * 0.5;
    const Rect envelope = horizontal
        ? Rect{std::floor(std::min(pa.x, pb.x) - half), std::floor(*it - half),
               std::ceil(std::max(pa.x, pb.x) + half), std::ceil(*(end - 1) + half)}
        : Rect{std::floor(*it - half), std::floor(std::min(pa.y, pb.y) - half),
               std::ceil(*(end - 1) + half), std::ceil(std::max(pa.y, pb.y) + half)};
    if (legal_rect(envelope, l)) {
        const auto count = static_cast<size_t>(end - it);
        if (count > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("routing graph track capacity exceeds uint32 limit");
        return static_cast<uint32_t>(count);
    }
    uint32_t capacity = 0;
    for (; it != end; ++it) {
        const Point start = horizontal ? Point{pa.x, *it} : Point{*it, pa.y};
        const Point end = horizontal ? Point{pb.x, *it} : Point{*it, pb.y};
        if (legal_segment(start, end, l)) {
            if (capacity == std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("routing graph track capacity exceeds uint32 limit");
            ++capacity;
        }
    }
    return capacity;
}
double RoutingGraph::edge_capacity(int a, int b) const {
    if (!legal_node(a) || !legal_node(b)) return 0;
    const int al = layer(a), bl = layer(b);
    if (al == bl) {
        const bool horizontal = directions_[al] == Direction::HORIZONTAL;
        if(global_guide_mode()) {
            const bool along_x=row(a)==row(b) && std::abs(col(a)-col(b))==1;
            const bool along_y=col(a)==col(b) && std::abs(row(a)-row(b))==1;
            if((along_x||along_y) && (options_.allow_nonpreferred||(horizontal?along_x:along_y)))
                return std::max(1.0,std::min(guide_capacity(a),guide_capacity(b)));
            return 0;
        }
        if ((horizontal && row(a) == row(b) && std::abs(col(a) - col(b)) == 1) ||
            (!horizontal && col(a) == col(b) && std::abs(row(a) - row(b)) == 1))
            return planar_forward_[std::min(a, b)];
    } else if (col(a) == col(b) && row(a) == row(b) && std::abs(al - bl) == 1 && same_die(al, bl)) {
        return via_up_[std::min(a, b)] ? -1 : 0;
    }
    return 0;
}
bool RoutingGraph::legal_edge(int a, int b) const { return edge_capacity(a, b) != 0; }
double RoutingGraph::edge_length(int a, int b) const {
    if (!legal_edge(a, b)) return std::numeric_limits<double>::infinity();
    return point(a).distance_to(point(b));
}
double RoutingGraph::guide_capacity(int id) const {
    if (!valid_id(id)) throw std::out_of_range("invalid routing node");
    const int l = layer(id);
    if(global_guide_mode()) return official_capacity(id)-official_obstacle_demand(id);
    return guide_capacity_[l][guide_axes_[l] == 0 ? col(id) : row(id)];
}
bool RoutingGraph::guide_uses_physical_tracks(int l) const {
    if (l < 1 || l > 20) throw std::out_of_range("invalid routing layer");
    return physical_guide_mode_[l];
}
bool RoutingGraph::has_neighbor(int id,int low,int high) const {
    if(!legal_node(id)) return false;
    const int c=col(id),r=row(id),l=layer(id);const bool horizontal=directions_[l]==Direction::HORIZONTAL;
    const bool nonpreferred=allows_nonpreferred();
    for(int next:{node(c-(horizontal||nonpreferred?1:0),r-(!horizontal&&!nonpreferred?1:0),l),
                  node(c+(horizontal||nonpreferred?1:0),r+(!horizontal&&!nonpreferred?1:0),l),
                  nonpreferred?node(c,r-1,l):-1,nonpreferred?node(c,r+1,l):-1,
                  node(c,r,l-1),node(c,r,l+1)}) if(next>=0 && layer(next)>=low && layer(next)<=high && legal_edge(id,next)) return true;
    return false;
}
std::vector<int> RoutingGraph::neighbors(int id) const {
    std::vector<int> result;
    if (!legal_node(id)) return result;
    const int c = col(id), r = row(id), l = layer(id);
    const bool horizontal = directions_[l] == Direction::HORIZONTAL;
    const bool nonpreferred=allows_nonpreferred();
    for (int next : {node(c - (horizontal||nonpreferred ? 1 : 0), r - (!horizontal&&!nonpreferred ? 1 : 0), l),
                     node(c + (horizontal||nonpreferred ? 1 : 0), r + (!horizontal&&!nonpreferred ? 1 : 0), l),
                     nonpreferred?node(c,r-1,l):-1,nonpreferred?node(c,r+1,l):-1,
                     node(c, r, l - 1), node(c, r, l + 1)})
        if (legal_edge(id, next)) result.push_back(next);
    return result;
}
} // namespace eda
