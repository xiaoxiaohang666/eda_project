#include "router/tsv_policy.hpp"
#include "objectives/guide_metrics.hpp"
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <numeric>
#include <sstream>
#include <set>
#include <tuple>
#include <array>
#include <deque>
#include <memory>
#include <chrono>

namespace eda {
namespace {
constexpr double eps = 1e-6;
constexpr size_t global_grid_cell_limit = 4000000;
// Guide coordinates are integral DBU. Clip inward so neighboring abstract
// landings have disjoint interiors and never expand beyond a cell or DIEAREA.
bool global_cell_region(const GCellGrid& grid,const Rect& die,int col,int row,Rect& region) {
    const auto box=grid.get_cell_rect(col,row);
    const double x1=std::ceil(std::max(box.x1,die.x1)),y1=std::ceil(std::max(box.y1,die.y1));
    const double x2=std::floor(std::min(box.x2,die.x2)),y2=std::floor(std::min(box.y2,die.y2));
    if (x1>=x2 || y1>=y2) return false;
    region={x1,y1,x2,y2};return true;
}
size_t global_grid_capacity(const DefParser& def) {
    const auto& grid=def.gcell_grid();const auto& die=def.die_area();
    if (!grid.valid() || !std::isfinite(die.x1) || !std::isfinite(die.y1) ||
        !std::isfinite(die.x2) || !std::isfinite(die.y2) || die.width()<=0 || die.height()<=0)
        throw std::runtime_error("GLOBAL_GRID requires a valid GCell grid and DIEAREA");
    const size_t cols=static_cast<size_t>(grid.cols),rows=static_cast<size_t>(grid.rows);
    if (cols>global_grid_cell_limit/rows)
        throw std::runtime_error("TSV_GLOBAL_GRID_BUDGET: grid exceeds 4000000 cells");
    // Legal clipping is separable: the preflight uses O(cols+rows) work and
    // no graph/candidate allocation, even for a large public input.
    size_t legal_cols=0,legal_rows=0;
    for (int c=0;c<grid.cols;++c)
        legal_cols+=std::ceil(std::max(grid.x_lines[c],die.x1))<
                    std::floor(std::min(grid.x_lines[c+1],die.x2));
    for (int r=0;r<grid.rows;++r)
        legal_rows+=std::ceil(std::max(grid.y_lines[r],die.y1))<
                    std::floor(std::min(grid.y_lines[r+1],die.y2));
    return legal_cols*legal_rows;
}
bool same(Point a, Point b) { return a.distance_to(b) <= eps; }
bool contains(const Rect& outer, const Rect& inner) {
    return inner.x1 >= outer.x1 - eps && inner.y1 >= outer.y1 - eps &&
           inner.x2 <= outer.x2 + eps && inner.y2 <= outer.y2 + eps;
}
Rect combine(Rect a, const Rect& b) {
    return {std::min(a.x1,b.x1),std::min(a.y1,b.y1),std::max(a.x2,b.x2),std::max(a.y2,b.y2)};
}
Rect translate(const Rect& r, Point p, double dbu) {
    return {p.x + r.x1 * dbu, p.y + r.y1 * dbu, p.x + r.x2 * dbu, p.y + r.y2 * dbu};
}
struct PortInstance { std::string name; Point point; Rect region; int layer; };
// Compatible edges only. Fixed interface nets usually have a single candidate,
// so no cross-net x all-slot matrix or absent residual edges are allocated.
using CandidateCosts = std::vector<std::vector<std::pair<int,long long>>>;
struct FlowEdge { int to, reverse, capacity; long long cost; };
using PointBucket = std::pair<long long,long long>;
PointBucket point_bucket(Point p) {
    // Unit-DBU buckets avoid amplification of floating rounding by 1/eps.
    // Neighbor buckets are filtered by the original Manhattan epsilon test.
    const double x = std::floor(p.x), y = std::floor(p.y);
    constexpr double bound = static_cast<double>(std::numeric_limits<long long>::max() / 2);
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > bound || std::abs(y) > bound)
        throw std::runtime_error("TSV coordinate exceeds spatial-index range");
    return {static_cast<long long>(x),static_cast<long long>(y)};
}
template<class F>
void near_point(const std::map<PointBucket,std::vector<int>>& buckets,Point p,F consume) {
    const auto [x,y] = point_bucket(p);
    for (long long dx=-1; dx<=1; ++dx) for (long long dy=-1; dy<=1; ++dy) {
        const auto found=buckets.find({x+dx,y+dy});
        if (found!=buckets.end()) for (int index:found->second) consume(index);
    }
}
std::vector<Point> fixed_crossings(const Net& net) {
    std::vector<Point> forced;
    for (const auto& lower:net.pins) if (lower.layer_idx==10)
        for (const auto& upper:net.pins)
            if (upper.layer_idx==11 && same(lower.coord,upper.coord)) forced.push_back(lower.coord);
    return forced;
}
struct Matching { bool feasible{false}; long long cost{0}; std::vector<int> slots; };
Matching minimum_cost_matching(const CandidateCosts& costs,
                               const std::vector<bool>& forbidden) {
    const int n = static_cast<int>(costs.size()), m = static_cast<int>(forbidden.size());
    const int source = n + m, sink = source + 1, nodes = sink + 1;
    std::vector<std::vector<FlowEdge>> edges(nodes);
    auto add = [&](int u, int v, long long cost) {
        const int ru = static_cast<int>(edges[v].size()), rv = static_cast<int>(edges[u].size());
        edges[u].push_back({v, ru, 1, cost}); edges[v].push_back({u, rv, 0, -cost});
    };
    for (int i = 0; i < n; ++i) {
        add(source,i,0);
        for (const auto& [j,cost]:costs[i]) if (!forbidden[j]) add(i,n+j,cost);
    }
    for (int j = 0; j < m; ++j) if (!forbidden[j]) add(n+j,sink,0);
    constexpr long long inf = std::numeric_limits<long long>::max()/8;
    std::vector<long long> potential(nodes,0);
    Matching result;
    for (int flow = 0; flow < n; ++flow) {
        std::vector<long long> dist(nodes, inf);
        std::vector<int> parent(nodes,-1), parent_edge(nodes,-1);
        using Item = std::pair<long long,int>;
        std::priority_queue<Item,std::vector<Item>,std::greater<Item>> queue;
        dist[source]=0; queue.emplace(0,source);
        while (!queue.empty()) {
            const auto [d,u]=queue.top(); queue.pop(); if (d!=dist[u]) continue;
            for (int k=0;k<static_cast<int>(edges[u].size());++k) {
                const auto& e=edges[u][k]; if (!e.capacity) continue;
                const long long candidate=d+e.cost+potential[u]-potential[e.to];
                if (candidate<dist[e.to]) {
                    dist[e.to]=candidate;parent[e.to]=u;parent_edge[e.to]=k;queue.emplace(candidate,e.to);
                }
            }
        }
        if (parent[sink]<0) return result;
        for (int v=0;v<nodes;++v) if (dist[v]<inf) potential[v]+=dist[v];
        for (int v=sink;v!=source;v=parent[v]) {
            auto& e=edges[parent[v]][parent_edge[v]];
            result.cost+=e.cost; --e.capacity; ++edges[v][e.reverse].capacity;
        }
    }
    result.feasible=true;result.slots.assign(n,-1);
    for (int i=0;i<n;++i) for (const auto& e:edges[i])
        if (e.to>=n && e.to<n+m && e.capacity==0) result.slots[i]=e.to-n;
    return result;
}
double terminal_mst(const Net& net, const SlotCandidate& slot, bool top, double via_penalty) {
    struct Terminal { Point point; int layer; };
    std::vector<Terminal> pins{{slot.point,top?11:10}};
    for (const auto& pin:net.pins) if ((pin.layer_idx>=11)==top) pins.push_back({pin.coord,pin.layer_idx});
    std::vector<double> dist(pins.size(),std::numeric_limits<double>::infinity());
    std::vector<bool> used(pins.size(),false); dist[0]=0; double total=0;
    for (size_t k=0;k<pins.size();++k) {
        int u=-1;
        for (int j=0;j<static_cast<int>(pins.size());++j)
            if (!used[j] && (u<0 || dist[j]<dist[u])) u=j;
        used[u]=true;total+=dist[u];
        for (size_t v=0;v<pins.size();++v) if (!used[v]) {
            const double d=pins[u].point.distance_to(pins[v].point)+via_penalty*std::abs(pins[u].layer-pins[v].layer);
            dist[v]=std::min(dist[v],d);
        }
    }
    return total;
}

// Balanced spatial index. Query results are exact Manhattan nearest resources;
// the limited pool and subsequent preference matching are engineering heuristics.
class SlotPointIndex {
    struct Node { int slot{-1},left{-1},right{-1};Rect bounds;double maximum_heat{0}; };
    const std::vector<SlotCandidate>& slots_;
    const std::vector<double>* normalized_heat_;
    std::vector<int> ids_;
    std::vector<Node> nodes_;
    std::vector<int> parents_,active_counts_,slot_nodes_;
    std::vector<bool> active_;
    int root_{-1};
    int build(size_t first,size_t last,int depth) {
        if (first==last) return -1;
        const size_t mid=first+(last-first)/2;const int axis=depth%2;
        std::nth_element(ids_.begin()+static_cast<std::ptrdiff_t>(first),
                         ids_.begin()+static_cast<std::ptrdiff_t>(mid),
                         ids_.begin()+static_cast<std::ptrdiff_t>(last),[&](int a,int b) {
            const auto pa=slots_[a].point,pb=slots_[b].point;
            return axis==0?std::tie(pa.x,pa.y,a)<std::tie(pb.x,pb.y,b):
                           std::tie(pa.y,pa.x,a)<std::tie(pb.y,pb.x,b);
        });
        const int id=static_cast<int>(nodes_.size()),slot=ids_[mid];
        const auto point=slots_[slot].point;nodes_.push_back({slot,-1,-1,{point.x,point.y,point.x,point.y}});
        if (normalized_heat_) nodes_[id].maximum_heat=(*normalized_heat_)[slot];
        parents_.push_back(-1);active_counts_.push_back(1);slot_nodes_[slot]=id;
        const int left=build(first,mid,depth+1),right=build(mid+1,last,depth+1);
        nodes_[id].left=left;nodes_[id].right=right;
        if (left>=0) {
            nodes_[id].bounds=combine(nodes_[id].bounds,nodes_[left].bounds);
            nodes_[id].maximum_heat=std::max(nodes_[id].maximum_heat,nodes_[left].maximum_heat);
            parents_[left]=id;active_counts_[id]+=active_counts_[left];
        }
        if (right>=0) {
            nodes_[id].bounds=combine(nodes_[id].bounds,nodes_[right].bounds);
            nodes_[id].maximum_heat=std::max(nodes_[id].maximum_heat,nodes_[right].maximum_heat);
            parents_[right]=id;active_counts_[id]+=active_counts_[right];
        }
        return id;
    }
    static double lower_bound(Point p,const Rect& box) {
        return std::max({0.0,box.x1-p.x,p.x-box.x2})+std::max({0.0,box.y1-p.y,p.y-box.y2});
    }
public:
    SlotPointIndex(const std::vector<SlotCandidate>& slots,const std::vector<bool>& enabled,
                   const std::vector<double>* normalized_heat=nullptr)
        :slots_(slots),normalized_heat_(normalized_heat),slot_nodes_(slots.size(),-1),active_(enabled) {
        if (normalized_heat_ && (normalized_heat_->size()!=slots.size() ||
            std::any_of(normalized_heat_->begin(),normalized_heat_->end(),[](double h) {
                return !std::isfinite(h) || h<0 || h>1;
            }))) throw std::logic_error("invalid normalized TSV index heat");
        for (int i=0;i<static_cast<int>(slots.size());++i) if (enabled[i]) ids_.push_back(i);
        nodes_.reserve(ids_.size());parents_.reserve(ids_.size());active_counts_.reserve(ids_.size());
        root_=build(0,ids_.size(),0);
    }
    void remove(int slot) {
        if (!active_[slot]) return;
        active_[slot]=false;
        for (int id=slot_nodes_[slot];id>=0;id=parents_[id]) --active_counts_[id];
    }
    void restore(int slot) {
        if (active_[slot]) return;
        if (slot_nodes_[slot]<0) throw std::logic_error("cannot activate disabled TSV resource");
        active_[slot]=true;
        for (int id=slot_nodes_[slot];id>=0;id=parents_[id]) ++active_counts_[id];
    }
    std::vector<int> nearest(Point point,size_t count) const {
        using Item=std::pair<double,int>;
        std::priority_queue<Item> best;
        auto visit=[&](auto&& self,int id)->void {
            if (id<0 || !active_counts_[id]) return;
            const auto& node=nodes_[id];
            if (best.size()==count && lower_bound(point,node.bounds)>best.top().first) return;
            const Item current{point.distance_to(slots_[node.slot].point),node.slot};
            if (active_[node.slot]) {
                if (best.size()<count) best.push(current);
                else if (current<best.top()) {best.pop();best.push(current);}
            }
            int first=node.left,second=node.right;
            if (first<0 || (second>=0 && lower_bound(point,nodes_[second].bounds)<lower_bound(point,nodes_[first].bounds)))
                std::swap(first,second);
            self(self,first);self(self,second);
        };
        if (count) visit(visit,root_);
        std::vector<Item> ordered;ordered.reserve(best.size());
        while (!best.empty()) {ordered.push_back(best.top());best.pop();}
        std::sort(ordered.begin(),ordered.end());
        std::vector<int> result;result.reserve(ordered.size());
        for (const auto& [distance,id]:ordered) {(void)distance;result.push_back(id);}
        return result;
    }
    // Exact ranking for this proposal surrogate only. The terminal MST/risk
    // proxy still decides acceptance. Static subtree maxima include inactive
    // points: deletion can loosen the lower bound but never invalidate it.
    std::vector<int> nearest_thermal(Point point,size_t count,double heat_weight_dbu) const {
        if (!normalized_heat_ || !std::isfinite(heat_weight_dbu) || heat_weight_dbu<0)
            throw std::logic_error("invalid thermal TSV index query");
        using Item=std::pair<double,int>;
        std::priority_queue<Item> best;
        auto bound=[&](int id) {
            return lower_bound(point,nodes_[id].bounds)+heat_weight_dbu*(1-nodes_[id].maximum_heat);
        };
        auto visit=[&](auto&& self,int id)->void {
            if (id<0 || !active_counts_[id]) return;
            if (best.size()==count && bound(id)>best.top().first) return;
            const auto& node=nodes_[id];
            if (active_[node.slot]) {
                const Item current{point.distance_to(slots_[node.slot].point)+
                    heat_weight_dbu*(1-(*normalized_heat_)[node.slot]),node.slot};
                if (best.size()<count) best.push(current);
                else if (current<best.top()) {best.pop();best.push(current);}
            }
            int first=node.left,second=node.right;
            if (first<0 || (second>=0 && bound(second)<bound(first))) std::swap(first,second);
            self(self,first);self(self,second);
        };
        if (count) visit(visit,root_);
        std::vector<Item> ordered;ordered.reserve(best.size());
        while (!best.empty()) {ordered.push_back(best.top());best.pop();}
        std::sort(ordered.begin(),ordered.end());
        std::vector<int> result;result.reserve(ordered.size());
        for (const auto& [cost,id]:ordered) {(void)cost;result.push_back(id);}
        return result;
    }
};
Point pin_median_anchor(const Net& net) {
    std::vector<double> x,y;x.reserve(net.pins.size());y.reserve(net.pins.size());
    for (const auto& pin:net.pins) {x.push_back(pin.coord.x);y.push_back(pin.coord.y);}
    if (x.empty()) throw std::runtime_error("cross-die net has no terminals: "+net.name);
    std::sort(x.begin(),x.end());std::sort(y.begin(),y.end());
    const auto middle=[&](const std::vector<double>& values) {
        return (values[(values.size()-1)/2]+values[values.size()/2])*0.5;
    };
    return {middle(x),middle(y)};
}
// Hopcroft-Karp cardinality completion on the sparse admissible pool. DFS is
// iterative to avoid a recursion depth proportional to a large netlist.
// Candidate order reflects wire/thermal/capacity preference; it is not global
// minimum-cost matching and does not claim a contest-defined candidate rule.
size_t sparse_cardinality_matching(const CandidateCosts& candidates,size_t slot_count,
                                  std::vector<int>& chosen) {
    const int count=static_cast<int>(candidates.size());
    std::vector<int> owner(slot_count,-1);
    if (chosen.size()!=candidates.size()) chosen.assign(candidates.size(),-1);
    size_t matched=0;
    for (int n=0;n<count;++n) if (chosen[n]>=0) {owner[chosen[n]]=n;++matched;}
    for (int n=0;n<count;++n) if (chosen[n]<0)
        for (const auto& [slot,cost]:candidates[n]) {
            (void)cost;if (owner[slot]>=0) continue;
            chosen[n]=slot;owner[slot]=n;++matched;break;
        }
    std::vector<int> level(count),cursor(count);
    while (matched<candidates.size()) {
        std::fill(level.begin(),level.end(),-1);std::fill(cursor.begin(),cursor.end(),0);
        std::deque<int> queue;
        for (int n=0;n<count;++n) if (chosen[n]<0) {level[n]=0;queue.push_back(n);}
        int free_level=count;
        while (!queue.empty()) {
            const int n=queue.front();queue.pop_front();
            if (level[n]>=free_level) continue;
            for (const auto& [slot,cost]:candidates[n]) {
                (void)cost;const int other=owner[slot];
                if (other<0) free_level=level[n];
                else if (level[other]<0) {level[other]=level[n]+1;queue.push_back(other);}
            }
        }
        if (free_level==count) break;
        size_t added=0;
        for (int root=0;root<count;++root) if (chosen[root]<0) {
            std::vector<int> path{root},edges;
            bool complete=false;
            while (!path.empty() && !complete) {
                const int n=path.back();bool advanced=false;
                while (cursor[n]<static_cast<int>(candidates[n].size())) {
                    const int slot=candidates[n][cursor[n]++].first,other=owner[slot];
                    if (other<0) {
                        int landing=slot;
                        for (size_t i=path.size();i>0;--i) {
                            chosen[path[i-1]]=landing;owner[landing]=path[i-1];
                            if (i>1) landing=edges[i-2];
                        }
                        complete=true;++added;break;
                    }
                    if (level[other]==level[n]+1) {
                        edges.push_back(slot);path.push_back(other);advanced=true;break;
                    }
                }
                if (!complete && !advanced) {
                    level[n]=-1;path.pop_back();if (!edges.empty()) edges.pop_back();
                }
            }
        }
        if (!added) break;
        matched+=added;
    }
    return matched;
}
}

void TsvPolicy::preflight(const DefParser& def,const LefParser& lef,TsvPolicyOptions options) {
    if (options.polish_thermal_pareto && !options.assignment_polish)
        throw std::runtime_error("Pearson Pareto thermal rule requires assignment polish");
    if (options.polish_thermal_candidates && (!options.assignment_polish || options.polish_thermal_pareto))
        throw std::runtime_error("thermal TSV candidates require weighted assignment polish");
    if (!std::isfinite(options.polish_heat_weight) || options.polish_heat_weight<0 || options.polish_heat_weight>1000000)
        throw std::runtime_error("invalid TSV polish heat weight: require finite 0..1000000");
    if (options.polish_candidates>65536 || !options.polish_passes || options.polish_passes>64)
        throw std::runtime_error("invalid TSV polish bounds: require 0 <= candidates <= 65536 and 1 <= passes <= 64");
    if (options.block_size<3 || options.block_size>64 || !options.block_passes || options.block_passes>16 ||
        !options.block_max_groups || options.block_max_groups>65536 ||
        !options.block_max_cost_evaluations || options.block_max_cost_evaluations>100000000 ||
        !std::isfinite(options.block_max_seconds) || options.block_max_seconds<=0 || options.block_max_seconds>600)
        throw std::runtime_error("invalid TSV block bounds: size 3..64, passes 1..16, groups 1..65536, evaluations 1..100000000, seconds (0,600]");
    if (!options.nearest_candidates || options.max_candidates<options.nearest_candidates ||
        options.max_candidates>65536)
        throw std::runtime_error("invalid TSV candidate bounds: require 1 <= initial <= maximum <= 65536");
    size_t cross=0,bottom=0,top=0;
    for (const auto& net:def.nets()) cross+=net.is_cross_die;
    if (options.resource_mode==TsvResourceMode::GLOBAL_GRID) {
        const size_t available=global_grid_capacity(def);
        if (cross>available)
            throw std::runtime_error("TSV_GLOBAL_GRID_SUPPLY: cross_die_nets="+std::to_string(cross)+
                ", positive-area integer GCells="+std::to_string(available)+
                "; one crossing per cell is a conservative implementation policy");
        return;
    }
    if (!cross) return;
    for (const auto& [name,component]:def.components()) {
        if (component.macro_type.find("RESOURCE_SLOT")==std::string::npos) continue;
        if (!lef.get_macro(component.macro_type))
            throw std::runtime_error("missing resource-slot macro: "+component.macro_type);
        if (component.macro_type.find("bottom")!=std::string::npos) ++bottom;
        else if (component.macro_type.find("top")!=std::string::npos) ++top;
        else throw std::runtime_error("resource-slot die cannot be identified: "+name);
    }
    const bool paired=options.resource_mode==TsvResourceMode::PAIRED;
    const size_t upper_bound=paired?std::min(bottom,top):bottom+top;
    if (upper_bound<cross) {
        std::ostringstream reason;
        reason<<"TSV_POLICY_UNRESOLVED: "<<(paired?"paired-reservation":"reservation-projection")
              <<" engineering capacity bound; cross_die_nets="<<cross
              <<", bottom_slots="<<bottom<<", top_slots="<<top
              <<", maximum_candidates="<<upper_bound
              <<". This is an implementation policy, not a contest legality rule. "
              <<"No input instance or terminal will be invented to satisfy this bound.";
        throw std::runtime_error(reason.str());
    }
}

TsvPolicy::TsvPolicy(const DefParser& def, const LefParser& lef, const RoutingGraph& graph,
                     TsvPolicyOptions options)
    : def_(def),lef_(lef),graph_(graph),options_(options) {
    preflight(def,lef,options);
    if (options_.resource_mode==TsvResourceMode::GLOBAL_GRID) {
        if (!graph_.global_guide_mode())
            throw std::runtime_error("GLOBAL_GRID TSV resources require GLOBAL_GUIDE routing geometry");
        projected_=true;
        const auto& grid=graph_.grid();
        slots_.reserve(global_grid_capacity(def));
        global_cell_slots_.assign(static_cast<size_t>(grid.cols)*grid.rows,-1);
        for (int row=0;row<grid.rows;++row) for (int col=0;col<grid.cols;++col) {
            const int lower=graph_.node(col,row,10),upper=graph_.node(col,row,11);
            Rect region;
            if (!graph_.legal_node(lower) || !graph_.legal_node(upper) ||
                !global_cell_region(grid,def_.die_area(),col,row,region)) continue;
            SlotCandidate slot;slot.id=static_cast<int>(slots_.size());slot.projected=true;
            slot.region=slot.lower_pad=slot.upper_pad=slot.envelope=region;
            slot.point=region.center();slot.lower_node=lower;slot.upper_node=upper;
            global_cell_slots_[static_cast<size_t>(row)*grid.cols+col]=slot.id;
            slots_.push_back(std::move(slot));
        }
        size_t cross=0;for (const auto& net:def_.nets()) cross+=net.is_cross_die;
        if (cross>slots_.size())
            throw std::runtime_error("TSV_GLOBAL_GRID_SUPPLY: legal routing cells cannot accommodate all cross-die nets");
        // No fabricated instance names or physical VIA witness. Every candidate
        // is an independent abstract GCell landing with unit occupancy.
        enabled_.assign(slots_.size(),true);
        return;
    }
    std::vector<const ViaInfo*> vias;
    for (const auto& [name,via]:lef.vias()) {
        if (via.top_of_stack_only) continue; // No stack-top context model: never ignore the restriction.
        bool lower=false,upper=false;
        for (const auto& shape:via.shapes) { lower=lower||get_layer_index(shape.layer)==10; upper=upper||get_layer_index(shape.layer)==11; }
        if (lower&&upper) vias.push_back(&via);
    }
    std::sort(vias.begin(),vias.end(),[](const auto* a,const auto* b){return std::make_pair(!a->is_default,a->name)<std::make_pair(!b->is_default,b->name);});
    std::vector<PortInstance> bottom,top;
    std::vector<std::string> names;
    for (const auto& [name,component]:def.components()) if (component.macro_type.find("RESOURCE_SLOT")!=std::string::npos) names.push_back(name);
    std::sort(names.begin(),names.end());
    for (const auto& name:names) {
        const auto& component=def.components().at(name);
        const auto* reserve=lef.get_macro(component.macro_type);
        if (!reserve) throw std::runtime_error("missing resource-slot macro: "+component.macro_type);
        int layer=0;
        if (component.macro_type.find("bottom")!=std::string::npos) layer=10;
        if (component.macro_type.find("top")!=std::string::npos) layer=11;
        if (!layer) throw std::runtime_error("resource-slot die cannot be identified: "+name);
        // RESOURCE_SLOT is a placement reservation, often with no PIN geometry.
        // Discover a dimension-compatible TSV replacement macro and transform its
        // real port. Never substitute instance origin or GCell center for the port.
        std::vector<std::pair<std::string,std::pair<Point,Rect>>> ports;
        auto consider=[&](const MacroInfo& macro) {
            if (std::abs(macro.width-reserve->width)>eps || std::abs(macro.height-reserve->height)>eps) return;
            for (const auto& [pin_name,pin]:macro.pins) if (pin.has_rect && get_layer_index(pin.layer)==layer && pin_name=="TSV_PORT") {
                ports.push_back({macro.name,{transform_macro_point(pin.bbox.center(),macro,component,def.dbu_per_micron()),
                    transform_macro_rect(pin.bbox,macro,component,def.dbu_per_micron())}});
            }
        };
        if (!reserve->pins.empty()) consider(*reserve);
        if (ports.empty()) for (const auto& [macro_name,macro]:lef.macros()) consider(macro);
        std::sort(ports.begin(),ports.end(),[](const auto& a,const auto& b){return a.first<b.first;});
        if (ports.empty()) throw std::runtime_error("no dimension-compatible TSV_PORT macro for "+name);
        for (const auto& port:ports) if (!same(port.second.first,ports.front().second.first) ||
            !contains(port.second.second,ports.front().second.second) || !contains(ports.front().second.second,port.second.second))
            throw std::runtime_error("ambiguous TSV replacement port geometry for "+name);
        const Rect region=transform_macro_rect({0,0,reserve->width,reserve->height},*reserve,component,def.dbu_per_micron());
        const PortInstance p{name,ports.front().second.first,region,layer};
        (layer==10?bottom:top).push_back(p);
    }
    if (bottom.empty() && top.empty()) {
        for (const auto& net:def.nets()) if (net.is_cross_die)
            throw std::runtime_error("TSV policy unresolved: cross-die nets but no RESOURCE_SLOT regions");
        return;
    }
    if (vias.empty()) throw std::runtime_error("no LEF VIA connects metal10 to metal11");
    std::map<PointBucket,std::vector<int>> top_index;
    for (int i=0;i<static_cast<int>(top.size());++i) top_index[point_bucket(top[i].point)].push_back(i);
    std::set<std::string> paired_top;
    std::vector<std::pair<const PortInstance*,const PortInstance*>> pairs;
    bool fully_paired=bottom.size()==top.size();
    for (const auto& b:bottom) {
        std::vector<const PortInstance*> matches;
        near_point(top_index,b.point,[&](int i) { if (same(b.point,top[i].point)) matches.push_back(&top[i]); });
        if (matches.size()!=1 || !paired_top.insert(matches.front()->name).second) {
            fully_paired=false;
            if (options_.resource_mode==TsvResourceMode::PAIRED)
                throw std::runtime_error("ambiguous or missing geometric top slot for "+b.name);
            continue;
        }
        pairs.emplace_back(&b,matches.front());
    }
    fully_paired=fully_paired && paired_top.size()==top.size();
    projected_=options_.resource_mode==TsvResourceMode::PROJECTED ||
        (options_.resource_mode==TsvResourceMode::AUTO && !fully_paired);
    auto populate_via=[&](SlotCandidate& slot,const Rect* lower_port,const Rect* upper_port) {
        for (const auto* via:vias) {
            bool first=true,lower_first=true,upper_first=true;
            for (const auto& shape:via->shapes) {
                const Rect r=translate(shape.rect,slot.point,def.dbu_per_micron());
                slot.envelope=first?r:combine(slot.envelope,r);first=false;
                if (get_layer_index(shape.layer)==10) {slot.lower_pad=lower_first?r:combine(slot.lower_pad,r);lower_first=false;}
                if (get_layer_index(shape.layer)==11) {slot.upper_pad=upper_first?r:combine(slot.upper_pad,r);upper_first=false;}
            }
            if (first || lower_first || upper_first || !contains(slot.region,slot.envelope) ||
                !contains(def.die_area(),slot.envelope)) continue;
            if (lower_port && (!contains(*lower_port,slot.lower_pad)||!contains(*upper_port,slot.upper_pad))) continue;
            if (projected_) {
                // Existing instance OBS on both dies apply to the true landing.
                // This is a projected resource/VIA witness, not inserted cells.
                if (!graph_.legal_rect(slot.lower_pad,10)||!graph_.legal_rect(slot.upper_pad,11)) continue;
            } else {
                bool blocked=false;
                for (const auto& [component_name,component]:def.components()) {
                    (void)component_name;
                    const auto* macro=lef.get_macro(component.macro_type);if (!macro) continue;
                    for (const auto& obs:macro->obstructions) {
                        const int l=get_layer_index(obs.layer);if (l!=10&&l!=11) continue;
                        const auto obstacle=transform_macro_rect(obs.rect,*macro,component,def.dbu_per_micron());
                        if (obstacle.intersects(l==10?slot.lower_pad:slot.upper_pad)) blocked=true;
                    }
                }
                if (blocked) continue;
            }
            slot.via_name=via->name; return true;
        }
        return false;
    };
    auto add_slot=[&](SlotCandidate slot,const Rect* lower_port=nullptr,const Rect* upper_port=nullptr) {
        if (!populate_via(slot,lower_port,upper_port)) return;
        const int c=graph.grid().get_col(slot.point.x),r=graph.grid().get_row(slot.point.y);
        if (c<0||r<0) return;
        slot.lower_node=graph.node(c,r,10);slot.upper_node=graph.node(c,r,11);
        slots_.push_back(std::move(slot));
    };
    if (!projected_) {
        for (const auto& [b,t]:pairs) {
            SlotCandidate slot;slot.bottom_name=b->name;slot.top_name=t->name;slot.point=b->point;
            const double x1=std::max(b->region.x1,t->region.x1),y1=std::max(b->region.y1,t->region.y1);
            const double x2=std::min(b->region.x2,t->region.x2),y2=std::min(b->region.y2,t->region.y2);
            if (x2<=x1||y2<=y1) continue;
            slot.region={x1,y1,x2,y2};add_slot(std::move(slot));
        }
        if (paired_top.size()!=top.size()) throw std::runtime_error("unpaired top RESOURCE_SLOT region");
    } else {
        std::map<PointBucket,std::vector<int>> added;
        auto project=[&](const PortInstance& source) {
            const auto& component=def.components().at(source.name);
            const auto* reserve=lef.get_macro(component.macro_type);
            std::array<Rect,2> ports;std::array<Point,2> points;
            for (int side=0;side<2;++side) {
                std::vector<std::pair<std::string,Rect>> replacements;
                for (const auto& [name,macro]:lef.macros()) {
                    if (std::abs(macro.width-reserve->width)>eps || std::abs(macro.height-reserve->height)>eps) continue;
                    for (const auto& [pin_name,pin]:macro.pins)
                        if (pin_name=="TSV_PORT" && pin.has_rect && get_layer_index(pin.layer)==10+side)
                            replacements.emplace_back(name,transform_macro_rect(pin.bbox,macro,component,def.dbu_per_micron()));
                }
                std::sort(replacements.begin(),replacements.end(),[](const auto& a,const auto& b){return a.first<b.first;});
                if (replacements.empty())
                    throw std::runtime_error("no dimension-compatible projected TSV_PORT for "+source.name+" on "+get_layer_name(10+side));
                for (const auto& replacement:replacements)
                    if (!contains(replacement.second,replacements.front().second) ||
                        !contains(replacements.front().second,replacement.second))
                        throw std::runtime_error("ambiguous projected TSV replacement geometry for "+source.name);
                ports[side]=replacements.front().second;points[side]=ports[side].center();
            }
            if (!same(points[0],points[1]) || !same(source.point,points[0]))
                throw std::runtime_error("projected M10/M11 TSV_PORT centers are not aligned for "+source.name);
            int duplicate=-1;
            near_point(added,source.point,[&](int id){ if (same(slots_[id].point,source.point)) duplicate=id; });
            if (duplicate>=0) {
                auto& slot=slots_[duplicate];
                if (source.layer==10) slot.bottom_name=source.name;else slot.top_name=source.name;
                return;
            }
            SlotCandidate slot;slot.projected=true;slot.point=source.point;slot.region=source.region;
            if (source.layer==10) slot.bottom_name=source.name;else slot.top_name=source.name;
            const size_t before=slots_.size();add_slot(std::move(slot),&ports[0],&ports[1]);
            if (slots_.size()>before) added[point_bucket(source.point)].push_back(static_cast<int>(before));
        };
        // Prefer actual top reservations when projections overlap. All source
        // names remain actual inputs; absent opposite-side names remain empty.
        for (const auto& t:top) project(t);
        for (const auto& b:bottom) project(b);
    }
    std::sort(slots_.begin(),slots_.end(),[](const auto& a,const auto& b){return std::tie(a.point.x,a.point.y,a.bottom_name,a.top_name)<std::tie(b.point.x,b.point.y,b.bottom_name,b.top_name);});
    for (int i=0;i<static_cast<int>(slots_.size());++i) slots_[i].id=i;
    // The reservation outlines include the supplied replacement macro's keepout.
    std::vector<int> ordered(slots_.size());
    std::iota(ordered.begin(),ordered.end(),0);
    std::sort(ordered.begin(),ordered.end(),[&](int a,int b) {
        return std::tie(slots_[a].region.x1,a)<std::tie(slots_[b].region.x1,b);
    });
    std::multimap<double,int> active;
    for (int id:ordered) {
        const auto& current=slots_[id].region;
        active.erase(active.begin(),active.lower_bound(current.x1)); // Edge-touch is still a conflict.
        for (const auto& [right,other]:active) {
            (void)right;
            const auto& other_region=slots_[other].region;
            const bool conflict=projected_?
                std::min(current.x2,other_region.x2)>std::max(current.x1,other_region.x1)+eps &&
                std::min(current.y2,other_region.y2)>std::max(current.y1,other_region.y1)+eps:
                current.intersects(other_region);
            if (conflict) {
                slots_[id].conflicts.push_back(other); slots_[other].conflicts.push_back(id);
            }
        }
        active.emplace(current.x2,id);
    }
    for (auto& slot:slots_) std::sort(slot.conflicts.begin(),slot.conflicts.end());
    enabled_.assign(slots_.size(),true);
    if (projected_) {
        std::vector<int> preferred(slots_.size());std::iota(preferred.begin(),preferred.end(),0);
        std::stable_sort(preferred.begin(),preferred.end(),[&](int a,int b) {
            return std::make_pair(slots_[a].top_name.empty(),a)<std::make_pair(slots_[b].top_name.empty(),b);
        });
        // Deterministic independent supply, preserving real 7um reservation
        // interiors. This is a heuristic, not maximum independent set.
        for (int id:preferred) if (enabled_[id])
            for (int other:slots_[id].conflicts) enabled_[other]=false;
    }
}

bool TsvPolicy::compatible(const Net& net,int id) const {
    if (!net.is_cross_die || id<0 || id>=static_cast<int>(slots_.size()) || !enabled_[id]) return false;
    // Both fixed interface pins must remain connected. An aligned M10/M11 pair
    // forces a crossing at its original position; moving to another slot would
    // create an unexplained second electrical connection even with edge-touch.
    for (const auto& lower:net.pins) if (lower.layer_idx==10) {
        for (const auto& upper:net.pins) if (upper.layer_idx==11 && same(lower.coord,upper.coord)) {
            if (options_.resource_mode==TsvResourceMode::GLOBAL_GRID) {
                const auto& slot=slots_[id];const auto& grid=graph_.grid();
                if (grid.get_col(lower.coord.x)!=graph_.col(slot.lower_node) ||
                    grid.get_row(lower.coord.y)!=graph_.row(slot.lower_node) ||
                    !slot.region.contains(lower.coord) || !slot.region.contains(upper.coord)) return false;
            } else if (!same(lower.coord,slots_[id].point)) return false;
        }
    }
    return true;
}

size_t TsvPolicy::enabled_slots() const {
    return static_cast<size_t>(std::count(enabled_.begin(),enabled_.end(),true));
}

std::vector<int> TsvPolicy::assign(const std::vector<Net>& nets,const ThermalParser& thermal) const {
    assignment_block_groups_=assignment_block_improvements_=assignment_block_moved_nets_=0;
    assignment_block_cost_evaluations_=assignment_block_mst_work_=assignment_block_skipped_high_fanout_=0;
    assignment_block_cost_reduction_=0;assignment_block_budget_exhausted_=false;assignment_block_seconds_=0;
    if (projected_) return assign_projected(nets,thermal);
    std::vector<int> cross;
    for (int i=0;i<static_cast<int>(nets.size());++i) if (nets[i].is_cross_die) cross.push_back(i);
    std::sort(cross.begin(),cross.end(),[&](int a,int b){return nets[a].name<nets[b].name;});
    std::vector<int> result(nets.size(),-1);if (cross.empty()) return result;
    if (slots_.empty()) throw std::runtime_error("TSV policy unresolved: no legal paired candidate slots");
    std::vector<double> heat(slots_.size());double maxheat=0,minheat=std::numeric_limits<double>::infinity();
    for (size_t s=0;s<slots_.size();++s) {
        heat[s]=std::max(0.0,thermal.get_temperature(graph_.grid().get_col(slots_[s].point.x),graph_.grid().get_row(slots_[s].point.y))-thermal.ambient_temperature());
        maxheat=std::max(maxheat,heat[s]);minheat=std::min(minheat,heat[s]);
    }
    if (slots_.size()<cross.size())
        throw std::runtime_error("TSV assignment infeasible: fewer legal paired slots than cross-die nets");
    std::map<PointBucket,std::vector<int>> slot_index;
    for (int id=0;id<static_cast<int>(slots_.size());++id) slot_index[point_bucket(slots_[id].point)].push_back(id);
    std::vector<std::vector<int>> candidates(cross.size());
    size_t edge_count=0;
    constexpr size_t candidate_edge_limit=2000000;
    for (size_t n=0;n<cross.size();++n) {
        const auto forced=fixed_crossings(nets[cross[n]]);
        auto& ids=candidates[n];
        if (forced.empty()) {
            // Every slot is admissible under the old flexible-net experiment.
            // A large public case needs explicit legal regions, not unproven k-nearest pruning.
            if (slots_.size()>candidate_edge_limit-edge_count)
                throw std::runtime_error("TSV_CANDIDATE_BUDGET: flexible-net paired policy exceeds 2000000 compatible edges; explicit per-net legal TSV regions are needed");
            ids.resize(slots_.size()); std::iota(ids.begin(),ids.end(),0);
        } else {
            near_point(slot_index,forced.front(),[&](int id) {
                if (compatible(nets[cross[n]],id)) ids.push_back(id);
            });
            std::sort(ids.begin(),ids.end());
            ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
            if (ids.size()>candidate_edge_limit-edge_count)
                throw std::runtime_error("TSV_CANDIDATE_BUDGET: paired policy exceeds 2000000 compatible edges");
        }
        edge_count+=ids.size();
        if (ids.empty()) throw std::runtime_error("TSV expression/policy unresolved: no compatible slot for "+nets[cross[n]].name+" while retaining original pins");
    }
    CandidateCosts costs(cross.size());
    const double scale=1000000.0,ell0=std::max(1.0,graph_.ell0());
    for (size_t n=0;n<cross.size();++n) {
        costs[n].reserve(candidates[n].size());
        for (int s:candidates[n]) {
            const auto& slot=slots_[s];
            const double mst=terminal_mst(nets[cross[n]],slot,false,0.2*ell0)+terminal_mst(nets[cross[n]],slot,true,0.2*ell0)+2000.0;
            const double h=maxheat>minheat?(heat[s]-minheat)/(maxheat-minheat):0.0;
            const double risk=0.5/std::max(1.0,graph_.guide_capacity(slot.lower_node))+0.5/std::max(1.0,graph_.guide_capacity(slot.upper_node));
            costs[n].emplace_back(s,std::llround(scale*(mst/ell0+2*risk+2*(1-h))));
        }
    }
    struct State { Matching matching;std::vector<bool> banned; };
    auto worse=[](const State& a,const State& b){return std::tie(a.matching.cost,a.banned)>std::tie(b.matching.cost,b.banned);};
    std::priority_queue<State,std::vector<State>,decltype(worse)> pending(worse);
    std::vector<bool> initial(slots_.size(),false);
    auto first=minimum_cost_matching(costs,initial);
    if (!first.feasible) throw std::runtime_error("TSV assignment infeasible under one-net-per-slot capacity and fixed-pin policy");
    pending.push({std::move(first),initial});std::set<std::vector<bool>> seen{initial};
    constexpr int conflict_branch_limit=128;
    for (int iteration=0;iteration<conflict_branch_limit&&!pending.empty();++iteration) {
        auto state=pending.top();pending.pop();std::set<int> used(state.matching.slots.begin(),state.matching.slots.end());
        int left=-1,right=-1;
        for (int s:used) {for (int c:slots_[s].conflicts) if (used.count(c)) {left=s;right=c;break;} if (left>=0) break;}
        if (left<0) {for (size_t n=0;n<cross.size();++n) result[cross[n]]=state.matching.slots[n];return result;}
        for (int ban:{left,right}) {
            auto next=state.banned;next[ban]=true;if (!seen.insert(next).second) continue;
            auto matching=minimum_cost_matching(costs,next);if (matching.feasible) pending.push({std::move(matching),std::move(next)});
        }
    }
    throw std::runtime_error(pending.empty()?"TSV assignment has no conflict-free capacity matching":"TSV conflict search reached its 128-state bound without a legal assignment");
}

std::vector<int> TsvPolicy::assign_projected(const std::vector<Net>& nets,const ThermalParser& thermal) const {
    std::vector<int> cross;
    for (int i=0;i<static_cast<int>(nets.size());++i) if (nets[i].is_cross_die) cross.push_back(i);
    std::sort(cross.begin(),cross.end(),[&](int a,int b) {
        return std::make_pair(fixed_crossings(nets[a]).empty(),nets[a].name)<
               std::make_pair(fixed_crossings(nets[b]).empty(),nets[b].name);
    });
    std::vector<int> result(nets.size(),-1);if (cross.empty()) return result;
    const size_t available=enabled_slots();
    if (available<cross.size())
        throw std::runtime_error(options_.resource_mode==TsvResourceMode::GLOBAL_GRID ?
            "TSV_GLOBAL_GRID_SUPPLY: "+std::to_string(available)+" global GCell landings for "+
                std::to_string(cross.size())+" cross-die nets" :
            "TSV projected geometry supply insufficient: "+std::to_string(available)+
            " nonoverlapping VIA/OBS-checked reservations for "+std::to_string(cross.size())+
            " cross-die nets; this is a conservative engineering model");
    SlotPointIndex index(slots_,enabled_);
    std::map<PointBucket,std::vector<int>> exact;
    if (options_.resource_mode!=TsvResourceMode::GLOBAL_GRID)
        for (int id=0;id<static_cast<int>(slots_.size());++id) if (enabled_[id])
            exact[point_bucket(slots_[id].point)].push_back(id);
    std::vector<Point> anchors;std::vector<std::vector<Point>> forced;
    anchors.reserve(cross.size());forced.reserve(cross.size());
    for (int n:cross) {anchors.push_back(pin_median_anchor(nets[n]));forced.push_back(fixed_crossings(nets[n]));}
    std::vector<double> heat(slots_.size());
    double maxheat=0,minheat=std::numeric_limits<double>::infinity();
    for (int id=0;id<static_cast<int>(slots_.size());++id) if (enabled_[id]) {
        const auto p=slots_[id].point;
        heat[id]=std::max(0.0,thermal.get_temperature(graph_.grid().get_col(p.x),graph_.grid().get_row(p.y))-thermal.ambient_temperature());
        maxheat=std::max(maxheat,heat[id]);minheat=std::min(minheat,heat[id]);
    }
    const double ell0=std::max(1.0,graph_.ell0()),scale=1000000.0;
    const size_t maximum=std::min(options_.max_candidates,available);
    size_t limit=std::min(options_.nearest_candidates,maximum);
    std::vector<int> chosen(cross.size(),-1);
    assignment_expansions_=0;assignment_global_completions_=0;assignment_max_displacement_dbu_=0;
    for (;;) {
        ++assignment_expansions_;
        CandidateCosts candidates(cross.size());size_t edge_count=0;
        constexpr size_t edge_budget=8000000;
        for (size_t n=0;n<cross.size();++n) {
            std::vector<int> pool;
            if (forced[n].empty()) pool=index.nearest(anchors[n],limit);
            else if (options_.resource_mode==TsvResourceMode::GLOBAL_GRID) {
                const auto point=forced[n].front();const auto& grid=graph_.grid();
                const int col=grid.get_col(point.x),row=grid.get_row(point.y);
                if (col>=0 && row>=0) {
                    const int id=global_cell_slots_[static_cast<size_t>(row)*grid.cols+col];
                    if (compatible(nets[cross[n]],id)) pool.push_back(id);
                }
            }
            else near_point(exact,forced[n].front(),[&](int id){if (compatible(nets[cross[n]],id)) pool.push_back(id);});
            if (pool.size()>edge_budget-edge_count)
                throw std::runtime_error("TSV_CANDIDATE_BUDGET: projected sparse candidate graph exceeds 8000000 edges");
            edge_count+=pool.size();candidates[n].reserve(pool.size());
            for (int id:pool) {
                const auto& slot=slots_[id];
                const double mst=terminal_mst(nets[cross[n]],slot,false,0.2*ell0)+
                    terminal_mst(nets[cross[n]],slot,true,0.2*ell0)+2000.0;
                const double h=maxheat>minheat?(heat[id]-minheat)/(maxheat-minheat):0.0;
                const double risk=0.5/std::max(1.0,graph_.guide_capacity(slot.lower_node))+
                    0.5/std::max(1.0,graph_.guide_capacity(slot.upper_node));
                candidates[n].emplace_back(id,std::llround(scale*(mst/ell0+2*risk+2*(1-h))));
            }
            std::sort(candidates[n].begin(),candidates[n].end(),[](const auto& a,const auto& b) {
                return std::tie(a.second,a.first)<std::tie(b.second,b.first);
            });
            if (candidates[n].empty())
                throw std::runtime_error("TSV projected mapping has no compatible resource for fixed interface net "+nets[cross[n]].name);
        }
        assignment_candidate_edges_=edge_count;
        size_t matched=sparse_cardinality_matching(candidates,slots_.size(),chosen);
        if (matched<cross.size() && limit==maximum && options_.global_completion) {
            // Complete only flexible nets against still-free actual resources.
            // Dynamic KD deletion avoids a dense all-net x all-slot graph.
            // No new region or instance is generated, and fixed pins never move.
            std::vector<bool> free=enabled_;
            for (int id:chosen) if (id>=0) free[id]=false;
            SlotPointIndex remaining(slots_,free);
            for (size_t n=0;n<cross.size();++n) if (chosen[n]<0) {
                if (!forced[n].empty())
                    throw std::runtime_error("TSV fixed-interface capacity conflict: "+nets[cross[n]].name);
                auto pool=remaining.nearest(anchors[n],options_.nearest_candidates);
                if (pool.empty()) throw std::logic_error("projected supply count disagrees with free-resource index");
                int best=-1;long long best_cost=std::numeric_limits<long long>::max();
                for (int id:pool) {
                    const auto& slot=slots_[id];
                    const double mst=terminal_mst(nets[cross[n]],slot,false,0.2*ell0)+
                        terminal_mst(nets[cross[n]],slot,true,0.2*ell0)+2000.0;
                    const double h=maxheat>minheat?(heat[id]-minheat)/(maxheat-minheat):0.0;
                    const double risk=0.5/std::max(1.0,graph_.guide_capacity(slot.lower_node))+
                        0.5/std::max(1.0,graph_.guide_capacity(slot.upper_node));
                    const auto cost=std::llround(scale*(mst/ell0+2*risk+2*(1-h)));
                    if (std::tie(cost,id)<std::tie(best_cost,best)) {best_cost=cost;best=id;}
                }
                if (pool.size()>edge_budget-assignment_candidate_edges_)
                    throw std::runtime_error("TSV_CANDIDATE_BUDGET: free-resource completion exceeds 8000000 evaluated edges");
                assignment_candidate_edges_+=pool.size();
                chosen[n]=best;remaining.remove(best);++matched;++assignment_global_completions_;
            }
        }
        if (matched==cross.size()) {
            assignment_polish_moves_=assignment_polish_swaps_=0;
            assignment_polish_passes_=0;
            assignment_polish_thermal_queries_=assignment_polish_thermal_candidates_=0;
            assignment_polish_cost_reduction_=0;
            assignment_polish_correlation_before_=assignment_polish_correlation_after_=0;
            if (options_.assignment_polish) {
                // Feasibility comes first. Preserve actual reservations and
                // fixed pins. Weighted mode retains the legacy proxy; Pareto
                // mode also requires exact full-grid Pearson not to decrease.
                const auto& grid=graph_.grid();
                const size_t cell_count=static_cast<size_t>(grid.cols)*grid.rows;
                std::vector<double> full_heat(cell_count);
                std::vector<int> counts(cell_count,0),slot_cells(slots_.size(),-1);
                for (int row=0;row<grid.rows;++row) for (int col=0;col<grid.cols;++col)
                    full_heat[static_cast<size_t>(row)*grid.cols+col]=
                        std::max(0.0,thermal.get_temperature(col,row)-thermal.ambient_temperature());
                for (int id=0;id<static_cast<int>(slots_.size());++id) if (enabled_[id]) {
                    const auto point=slots_[id].point;
                    const int col=grid.get_col(point.x),row=grid.get_row(point.y);
                    if (col<0 || row<0 || col>=grid.cols || row>=grid.rows)
                        throw std::logic_error("enabled TSV slot outside thermal grid");
                    slot_cells[id]=row*grid.cols+col;
                }
                for (int id:chosen) ++counts[slot_cells[id]];
                PearsonState pearson(std::move(full_heat),std::move(counts));
                assignment_polish_correlation_before_=pearson.correlation();
                std::vector<double> normalized_heat;
                if (options_.polish_thermal_candidates) {
                    normalized_heat.resize(slots_.size());
                    if (maxheat>minheat) for (size_t id=0;id<slots_.size();++id) if (enabled_[id])
                        normalized_heat[id]=(heat[id]-minheat)/(maxheat-minheat);
                }
                SlotPointIndex free_index(slots_,enabled_,options_.polish_thermal_candidates?&normalized_heat:nullptr);
                std::vector<int> owners(slots_.size(),-1);
                for (size_t n=0;n<chosen.size();++n) {
                    if (owners[chosen[n]]>=0) throw std::logic_error("duplicate assigned TSV resource");
                    owners[chosen[n]]=int(n);free_index.remove(chosen[n]);
                }
                auto cost=[&](size_t n,int id) {
                    const auto& slot=slots_[id];
                    const double mst=terminal_mst(nets[cross[n]],slot,false,0.2*ell0)+
                        terminal_mst(nets[cross[n]],slot,true,0.2*ell0)+2000.0;
                    const double h=maxheat>minheat?(heat[id]-minheat)/(maxheat-minheat):0.0;
                    const double risk=0.5/std::max(1.0,graph_.guide_capacity(slot.lower_node))+
                        0.5/std::max(1.0,graph_.guide_capacity(slot.upper_node));
                    return std::llround(scale*(mst/ell0+2*risk+(options_.polish_thermal_pareto?0.0:options_.polish_heat_weight*(1-h))));
                };
                std::vector<long long> costs(chosen.size());
                for (size_t n=0;n<chosen.size();++n) costs[n]=cost(n,chosen[n]);
                const size_t polish_budget=options_.polish_candidates?options_.polish_candidates:options_.nearest_candidates;
                // Forward proposals find slots near this net's terminals.
                // Reverse proposals find nets whose terminals are near this
                // net's current slot, including owners of distant slots left
                // by global completion. Only flexible nets enter this index.
                // Its points stay fixed while chosen[] tracks moved owners.
                std::vector<SlotCandidate> anchor_slots;
                std::unique_ptr<SlotPointIndex> anchor_index;
                if (options_.polish_bidirectional) {
                    anchor_slots.resize(anchors.size());
                    std::vector<bool> flexible(anchors.size());
                    for (size_t n=0;n<anchors.size();++n) {
                        anchor_slots[n].point=anchors[n];flexible[n]=forced[n].empty();
                    }
                    anchor_index=std::make_unique<SlotPointIndex>(anchor_slots,flexible);
                }
                std::vector<size_t> swap_seen(anchor_index?slots_.size():0,0);
                size_t swap_generation=0;
                for (size_t pass=0;pass<options_.polish_passes;++pass) {
                    ++assignment_polish_passes_;
                    bool improved=false;
                    for (size_t n=0;n<chosen.size();++n) {
                        const int old=chosen[n];int best=-1,other=-1;long long reduction=0,new_cost=costs[n],other_cost=0;
                        const double current_correlation=pearson.correlation();
                        double best_correlation=current_correlation;
                        auto free_slots=free_index.nearest(anchors[n],polish_budget);
                        if (options_.polish_thermal_candidates) {
                            ++assignment_polish_thermal_queries_;
                            auto spatial_slots=free_slots;std::sort(spatial_slots.begin(),spatial_slots.end());
                            for (int id:free_index.nearest_thermal(anchors[n],polish_budget,ell0*options_.polish_heat_weight))
                                if (!std::binary_search(spatial_slots.begin(),spatial_slots.end(),id)) {
                                    free_slots.push_back(id);++assignment_polish_thermal_candidates_;
                                }
                        }
                        for (int id:free_slots) {
                            if (!compatible(nets[cross[n]],id)) continue;
                            bool conflict=false;
                            for (int c:slots_[id].conflicts) if (owners[c]>=0 && owners[c]!=int(n)) {conflict=true;break;}
                            if (conflict) continue;
                            const auto trial=cost(n,id),gain=costs[n]-trial;
                            if (options_.polish_thermal_pareto) {
                                const double after=pearson.correlation_after_move(slot_cells[old],slot_cells[id]);
                                if (gain<0 || after<current_correlation || (gain==0 && after<=current_correlation)) continue;
                                if (gain>reduction || (gain==reduction && after>best_correlation)) {
                                    best=id;other=-1;reduction=gain;new_cost=trial;best_correlation=after;
                                }
                            } else if (gain>reduction) {best=id;other=-1;reduction=gain;new_cost=trial;}
                        }
                        auto swap_slots=index.nearest(anchors[n],polish_budget);
                        if (anchor_index) {
                            ++swap_generation;
                            for (int id:swap_slots) swap_seen[id]=swap_generation;
                            for (int partner:anchor_index->nearest(slots_[old].point,polish_budget)) {
                                const int id=chosen[partner];
                                if (swap_seen[id]!=swap_generation) {
                                    swap_seen[id]=swap_generation;swap_slots.push_back(id);
                                }
                            }
                        }
                        for (int id:swap_slots) {
                            const int partner=owners[id];
                            if (partner<0 || partner==int(n) || !compatible(nets[cross[n]],id) ||
                                !compatible(nets[cross[partner]],old)) continue;
                            const auto trial=cost(n,id),paired=cost(size_t(partner),old);
                            const auto gain=costs[n]+costs[partner]-trial-paired;
                            if (gain>reduction) {best=id;other=partner;reduction=gain;new_cost=trial;other_cost=paired;}
                        }
                        if (best<0) continue;
                        chosen[n]=best;owners[best]=int(n);costs[n]=new_cost;
                        if (other<0) {
                            pearson.move(slot_cells[old],slot_cells[best]);
                            owners[old]=-1;free_index.remove(best);free_index.restore(old);++assignment_polish_moves_;
                        } else {
                            chosen[other]=old;owners[old]=other;costs[other]=other_cost;++assignment_polish_swaps_;
                        }
                        assignment_polish_cost_reduction_+=reduction;improved=true;
                    }
                    if (!improved) break;
                }
                assignment_polish_correlation_after_=pearson.correlation();
                if (options_.polish_thermal_pareto &&
                    assignment_polish_correlation_after_<assignment_polish_correlation_before_)
                    throw std::logic_error("Pareto TSV polish decreased exact Pearson correlation");
            }
            if (options_.assignment_block_refine) refine_occupied_blocks(nets,cross,anchors,chosen);
            for (size_t n=0;n<cross.size();++n) {
                if (!compatible(nets[cross[n]],chosen[n]))
                    throw std::logic_error("sparse projected assignment violates compatible resource policy");
                result[cross[n]]=chosen[n];
                assignment_max_displacement_dbu_=std::max(assignment_max_displacement_dbu_,
                    anchors[n].distance_to(slots_[chosen[n]].point));
            }
            return result;
        }
        if (limit==maximum)
            throw std::runtime_error("TSV projected nearest-candidate heuristic exhausted: matched "+
                std::to_string(matched)+"/"+std::to_string(cross.size())+", candidates_per_net="+
                std::to_string(limit)+"; increase --tsv-max-candidates or revise the engineering candidate rule");
        limit=std::min(maximum,std::max(limit+1,limit*2));
    }
}

void TsvPolicy::refine_occupied_blocks(const std::vector<Net>& nets,const std::vector<int>& cross,
                                      const std::vector<Point>& anchors,std::vector<int>& chosen) const {
    using Clock=std::chrono::steady_clock;
    const auto started=Clock::now();
    auto finish=[&] {assignment_block_seconds_=std::chrono::duration<double>(Clock::now()-started).count();};
    auto out_of_time=[&] {
        return std::chrono::duration<double>(Clock::now()-started).count()>=options_.block_max_seconds;
    };
    // Each proxy evaluation performs quadratic Prim work on both die sides.
    // A separate work bound and a fanout cap prevent a few pathological nets
    // from making an otherwise small candidate-edge budget unbounded in time.
    constexpr size_t max_pins=64,mst_work_limit=200000000;
    std::vector<int> owners(slots_.size(),-1),seeds;
    std::vector<bool> movable(chosen.size(),false),occupied_movable(slots_.size(),false);
    std::vector<SlotCandidate> anchor_slots(chosen.size());
    std::vector<size_t> mst_work(chosen.size(),0);
    for (size_t n=0;n<chosen.size();++n) {
        const int id=chosen[n];
        if (id<0 || static_cast<size_t>(id)>=slots_.size() || !enabled_[id] || owners[id]>=0 ||
            !compatible(nets[cross[n]],id)) throw std::logic_error("invalid initial block TSV assignment");
        owners[id]=int(n);anchor_slots[n].point=anchors[n];
        const auto& net=nets[cross[n]];
        if (net.pins.size()>max_pins) {++assignment_block_skipped_high_fanout_;continue;}
        if (!fixed_crossings(net).empty()) continue;
        movable[n]=occupied_movable[id]=true;seeds.push_back(int(n));
        size_t lower=1,upper=1;
        for (const auto& pin:net.pins) {if (pin.layer_idx>=11) ++upper;else ++lower;}
        mst_work[n]=lower*lower+upper*upper;
    }
    if (seeds.size()<2) {finish();return;}
    if (out_of_time()) {assignment_block_budget_exhausted_=true;finish();return;}
    // Both indexes remain valid after a block permutation: anchor locations
    // and the set of occupied movable reservations are exactly invariant.
    SlotPointIndex slot_index(slots_,occupied_movable),anchor_index(anchor_slots,movable);
    std::vector<size_t> seen(chosen.size(),0);size_t generation=0;
    const double ell0=std::max(1.0,graph_.ell0()),scale=1000000.0;
    for (size_t pass=0;pass<options_.block_passes;++pass) {
        std::sort(seeds.begin(),seeds.end(),[&](int a,int b) {
            const double da=anchors[a].distance_to(slots_[chosen[a]].point);
            const double db=anchors[b].distance_to(slots_[chosen[b]].point);
            return da!=db?da>db:a<b;
        });
        std::vector<bool> covered(chosen.size(),false);bool improved=false;
        for (int seed:seeds) {
            if (covered[seed]) continue;
            if (assignment_block_groups_>=options_.block_max_groups ||
                assignment_block_cost_evaluations_>=options_.block_max_cost_evaluations || out_of_time()) {
                assignment_block_budget_exhausted_=true;finish();return;
            }
            std::vector<int> block;block.reserve(options_.block_size);++generation;
            auto append=[&](int n) {
                if (n>=0 && movable[n] && seen[n]!=generation && block.size()<options_.block_size) {
                    seen[n]=generation;block.push_back(n);
                }
            };
            append(seed);
            // Forward owners expose resources this net wants. Reverse anchor
            // neighbors expose nets that want its currently occupied resource.
            // Following both directions admits cycles that pair swaps miss.
            for (size_t cursor=0;cursor<block.size() && block.size()<options_.block_size;++cursor) {
                const int n=block[cursor];
                const auto forward=slot_index.nearest(anchors[n],options_.block_size);
                const auto reverse=anchor_index.nearest(slots_[chosen[n]].point,options_.block_size);
                for (size_t k=0;k<std::max(forward.size(),reverse.size()) && block.size()<options_.block_size;++k) {
                    if ((pass%2)==0) {
                        if (k<forward.size()) append(owners[forward[k]]);
                        if (k<reverse.size()) append(reverse[k]);
                    } else {
                        if (k<reverse.size()) append(reverse[k]);
                        if (k<forward.size()) append(owners[forward[k]]);
                    }
                }
            }
            for (int n:block) covered[n]=true;
            if (block.size()<2) continue;
            ++assignment_block_groups_;
            std::vector<int> old;old.reserve(block.size());
            for (int n:block) old.push_back(chosen[n]);
            CandidateCosts costs(block.size());long long before=0;
            // Only this B x B graph is materialized, never the global net x
            // resource graph. The diagonal is the existing feasible mapping.
            for (size_t i=0;i<block.size();++i) for (size_t j=0;j<block.size();++j) {
                const int n=block[i],id=old[j];
                if (!compatible(nets[cross[n]],id)) continue;
                if (assignment_block_cost_evaluations_>=options_.block_max_cost_evaluations ||
                    mst_work[n]>mst_work_limit-assignment_block_mst_work_ || out_of_time()) {
                    assignment_block_budget_exhausted_=true;finish();return;
                }
                ++assignment_block_cost_evaluations_;assignment_block_mst_work_+=mst_work[n];
                const double mst=terminal_mst(nets[cross[n]],slots_[id],false,0.2*ell0)+
                    terminal_mst(nets[cross[n]],slots_[id],true,0.2*ell0);
                const double scaled=scale*mst/ell0;
                // Leave the known feasible mapping untouched on numeric
                // outliers; bounded costs keep residual potentials safe.
                if (!std::isfinite(scaled) || scaled<0 ||
                    scaled>double(std::numeric_limits<long long>::max()/4096)) {finish();return;}
                const auto cost=std::llround(scaled);costs[i].emplace_back(int(j),cost);
                if (i==j) before+=cost;
            }
            const auto matching=minimum_cost_matching(costs,std::vector<bool>(block.size(),false));
            if (!matching.feasible) throw std::logic_error("occupied TSV block lost its feasible diagonal");
            if (out_of_time()) {assignment_block_budget_exhausted_=true;finish();return;}
            if (matching.cost>=before) continue;
            std::vector<bool> used(block.size(),false);size_t moved=0;
            for (size_t i=0;i<block.size();++i) {
                const int j=matching.slots[i];
                if (j<0 || static_cast<size_t>(j)>=block.size() || used[j] ||
                    !compatible(nets[cross[block[i]]],old[j]))
                    throw std::logic_error("invalid occupied TSV block permutation");
                used[j]=true;moved+=static_cast<size_t>(j)!=i;
            }
            // Commit a complete verified permutation. No partial matching is
            // ever visible, including on work-budget or wall-clock expiry.
            for (size_t i=0;i<block.size();++i) {
                const int n=block[i],id=old[matching.slots[i]];chosen[n]=id;owners[id]=n;
            }
            ++assignment_block_improvements_;assignment_block_moved_nets_+=moved;
            assignment_block_cost_reduction_+=before-matching.cost;improved=true;
        }
        if (!improved) break;
    }
    finish();
}

bool TsvPolicy::validate_mapping(const Net& net,const std::vector<GuideRect>& guides,int id,std::string& error) const {
    error.clear();
    auto fail=[&](const std::string& why){error=net.name+": "+why;return false;};
    if (net.is_cross_die && !compatible(net,id)) return fail("incompatible planned TSV slot");
    if (!net.is_cross_die && id!=-1) return fail("intra-die net has a planned TSV");
    std::set<std::tuple<double,double,double,double,std::string>> seen;
    std::vector<GuideRect> unique;
    for (const auto& r:guides) if (seen.emplace(r.x1,r.y1,r.x2,r.y2,r.layer).second) unique.push_back(r);
    std::vector<Rect> crosses;int positive=0;Rect inferred;
    for (const auto& a:unique) if (get_layer_index(a.layer)==10)
        for (const auto& b:unique) if (get_layer_index(b.layer)==11) {
            const double x1=std::max(a.x1,b.x1),y1=std::max(a.y1,b.y1),x2=std::min(a.x2,b.x2),y2=std::min(a.y2,b.y2);
            if (x1>x2+eps||y1>y2+eps) continue;
            if (!net.is_cross_die) return fail("unplanned electrical M10/M11 crossing");
            crosses.emplace_back(x1,y1,x2,y2);
            if (x2>x1&&y2>y1) {++positive;inferred={x1,y1,x2,y2};}
        }
    if (!net.is_cross_die) return true;
    if (positive!=1) return fail("guide infers "+std::to_string(positive)+" positive-area TSV pairs; policy requires exactly one");
    const auto& slot=slots_[id];
    const bool global=options_.resource_mode==TsvResourceMode::GLOBAL_GRID;
    if (!contains(slot.region,inferred)||!contains(inferred,slot.envelope))
        return fail(global ? "inferred TSV overlap must equal the selected global GCell landing" :
            "inferred TSV overlap must contain the actual VIA envelope inside the selected resource projection");
    for (const auto& crossing:crosses) if (!contains(inferred,crossing))
        return fail(global ? "extra M10/M11 electrical connection outside the global GCell landing, including edge-touch" :
            "extra M10/M11 electrical connection outside the planned VIA, including edge-touch");
    const auto p=inferred.center();const auto& grid=graph_.grid();
    if (grid.get_col(p.x)!=grid.get_col(slot.point.x)||grid.get_row(p.y)!=grid.get_row(slot.point.y))
        return fail(global ? "inferred thermal TSV cell differs from the selected global GCell landing" :
            "inferred thermal TSV cell differs from the planned physical TSV cell");
    return true;
}

struct TsvMoveSearch::Impl {
    const DefParser& def;
    const RoutingGraph& graph;
    const TsvPolicy& policy;
    std::vector<int> assignment, owner, slot_cells;
    std::vector<double> normalized_heat;
    std::unique_ptr<PearsonState> pearson;
    std::unique_ptr<SlotPointIndex> free_index;
    double correlation_per_normalized_heat{0.0};

    Impl(const DefParser& d,const RoutingGraph& g,const TsvPolicy& p,
         const ThermalParser& thermal,const std::vector<int>& chosen)
        :def(d),graph(g),policy(p),assignment(chosen),owner(p.slots().size(),-1),
         slot_cells(p.slots().size(),-1),normalized_heat(p.slots().size(),0.0) {
        if (std::string(policy.pairing_mode())!="global-grid")
            throw std::invalid_argument("TSV move search requires global-grid resources");
        if (assignment.size()!=def.nets().size())
            throw std::invalid_argument("TSV move search assignment size mismatch");
        const auto& grid=graph.grid();
        const size_t cells=size_t(grid.cols)*grid.rows;
        std::vector<double> heat(cells);
        std::vector<int> counts(cells,0);
        long double sum=0,square=0;
        double minimum=std::numeric_limits<double>::infinity(),maximum=0;
        for (int row=0;row<grid.rows;++row) for (int col=0;col<grid.cols;++col) {
            const double excess=thermal.get_temperature(col,row)-thermal.ambient_temperature();
            if (!std::isfinite(excess)) throw std::invalid_argument("nonfinite TSV move heat");
            const double h=std::max(0.0,excess);
            heat[size_t(row)*grid.cols+col]=h;sum+=h;square+=static_cast<long double>(h)*h;
            minimum=std::min(minimum,h);maximum=std::max(maximum,h);
        }
        const auto& slots=policy.slots();
        for (size_t id=0;id<slots.size();++id) {
            const int col=grid.get_col(slots[id].point.x),row=grid.get_row(slots[id].point.y);
            if (col<0||row<0||col>=grid.cols||row>=grid.rows)
                throw std::logic_error("TSV move slot outside thermal grid");
            slot_cells[id]=row*grid.cols+col;
            normalized_heat[id]=maximum>minimum?(heat[size_t(slot_cells[id])]-minimum)/(maximum-minimum):0;
        }
        size_t occupied=0;
        for (size_t n=0;n<assignment.size();++n) {
            const int id=assignment[n];const auto& net=def.nets()[n];
            if (!net.is_cross_die) {
                if (id!=-1) throw std::invalid_argument("same-die net owns a TSV move slot");
                continue;
            }
            if (!policy.compatible(net,id)||owner[size_t(id)]!=-1)
                throw std::invalid_argument("invalid or duplicate TSV move assignment");
            const size_t cell=size_t(slot_cells[size_t(id)]);
            if (counts[cell]) throw std::invalid_argument("TSV move requires binary thermal occupancy");
            owner[size_t(id)]=int(n);++counts[cell];++occupied;
        }
        // Pearson's binary-occupancy denominator remains fixed after a move.
        const long double variance=std::max(0.0L,square-sum*sum/static_cast<long double>(cells));
        const long double count_variance=static_cast<long double>(occupied)*
            (1.0L-static_cast<long double>(occupied)/static_cast<long double>(cells));
        const long double denominator=std::sqrt(variance*count_variance);
        if (denominator>0) correlation_per_normalized_heat=double((maximum-minimum)/denominator);
        pearson=std::make_unique<PearsonState>(std::move(heat),std::move(counts));
        // GLOBAL_GRID slots are all enabled; remove occupied points after tree
        // construction so commit_move can restore their nodes without rebuilding.
        free_index=std::make_unique<SlotPointIndex>(slots,std::vector<bool>(slots.size(),true),&normalized_heat);
        for (int id:assignment) if (id>=0) free_index->remove(id);
    }

    double risk(int slot) const {
        const auto& s=policy.slots()[size_t(slot)];
        return 0.5/std::max(1.0,graph.guide_capacity(s.lower_node))+
               0.5/std::max(1.0,graph.guide_capacity(s.upper_node));
    }
};

TsvMoveSearch::TsvMoveSearch(const DefParser& def,const RoutingGraph& graph,const TsvPolicy& policy,
                           const ThermalParser& thermal,const std::vector<int>& assignment)
    :impl_(std::make_unique<Impl>(def,graph,policy,thermal,assignment)) {}
TsvMoveSearch::~TsvMoveSearch()=default;

double TsvMoveSearch::current_correlation() const {return impl_->pearson->correlation();}
int TsvMoveSearch::current_slot(size_t net_index) const {return impl_->assignment.at(net_index);}

std::vector<TsvMoveCandidate> TsvMoveSearch::candidates(size_t net_index,const TsvMoveSearchOptions& o) const {
    if (o.max_candidates>4096 || !std::isfinite(o.length_price)||o.length_price<0 ||
        !std::isfinite(o.thermal_gain)||o.thermal_gain<0 || !std::isfinite(o.risk_price)||o.risk_price<0)
        throw std::invalid_argument("invalid TSV move proposal budget or score price");
    const auto& state=*impl_;const auto& net=state.def.nets().at(net_index);
    const int old=state.assignment.at(net_index);
    if (!o.max_candidates||!net.is_cross_die||old<0||net.pins.size()>o.max_pins) return {};
    // Six spatial/thermal queries may each contribute a full shortlist. Bound
    // quadratic MST work before collecting that union, including the old slot.
    // This also remains bounded if a caller raises max_pins above its default.
    constexpr size_t mst_work_limit=2000000,queries_per_net=6;
    const size_t terminals=net.pins.size()+2;
    if (terminals>mst_work_limit/terminals) return {};
    const size_t evaluations=mst_work_limit/(terminals*terminals);
    const size_t query_count=std::min(o.max_candidates,(evaluations-1)/queries_per_net);
    if (!query_count) return {};
    const auto& slots=state.policy.slots();
    std::vector<Point> anchors{slots[size_t(old)].point};
    const auto add_anchor=[&](Point point) {
        for (const auto existing:anchors) if (existing.distance_to(point)==0) return;
        anchors.push_back(point);
    };
    add_anchor(pin_median_anchor(net));
    // Both side medians permit shorter wiring moves as well as hotter moves.
    for (bool top:{false,true}) {
        std::vector<double> xs,ys;
        for (const auto& pin:net.pins) if ((pin.layer_idx>=11)==top) {xs.push_back(pin.coord.x);ys.push_back(pin.coord.y);}
        if (xs.empty()) continue;
        std::sort(xs.begin(),xs.end());std::sort(ys.begin(),ys.end());
        add_anchor({(xs[(xs.size()-1)/2]+xs[xs.size()/2])*0.5,
                    (ys[(ys.size()-1)/2]+ys[ys.size()/2])*0.5});
    }
    std::set<int> pool;
    for (const auto point:anchors) {
        const auto ids=state.free_index->nearest(point,query_count);
        pool.insert(ids.begin(),ids.end());
    }
    const double before=state.pearson->correlation();
    const double tanh_before=std::tanh(o.thermal_gain*before);
    double heat_price=12.5*o.thermal_gain*(1-tanh_before*tanh_before)*state.correlation_per_normalized_heat;
    // A saturated tanh has a zero floating-point local derivative, although a
    // finite move towards hotter cells can still improve the exact score. Use
    // a secant to the hottest possible target as an additional retrieval price.
    const double heat_step=1-state.normalized_heat[size_t(old)];
    if (heat_step>0) {
        const double hottest_correlation=std::clamp(before+heat_step*state.correlation_per_normalized_heat,-1.0,1.0);
        heat_price=std::max(heat_price,12.5*(std::tanh(o.thermal_gain*hottest_correlation)-tanh_before)/heat_step);
    }
    if (heat_price>0) {
        const auto& die=state.graph.die_area();
        const double max_weight=4*std::max(1.0,die.width()+die.height());
        const double heat_weight=o.length_price>0?std::min(max_weight,heat_price/o.length_price):max_weight;
        for (size_t a=0;a<std::min<size_t>(2,anchors.size());++a) {
            const auto ids=state.free_index->nearest_thermal(anchors[a],query_count,heat_weight);
            pool.insert(ids.begin(),ids.end());
        }
    }
    const double via_penalty=0.2*state.graph.ell0();
    const auto length=[&](int id) {
        return terminal_mst(net,slots[size_t(id)],false,via_penalty)+terminal_mst(net,slots[size_t(id)],true,via_penalty);
    };
    const double old_length=length(old),old_risk=state.risk(old);
    std::vector<TsvMoveCandidate> result;result.reserve(pool.size());
    for (int id:pool) {
        if (id==old||state.owner[size_t(id)]!=-1||!state.policy.compatible(net,id)) continue;
        const double after=state.pearson->correlation_after_move(size_t(state.slot_cells[size_t(old)]),size_t(state.slot_cells[size_t(id)]));
        const double delta_length=length(id)-old_length;
        const double delta_thermal=12.5*(std::tanh(o.thermal_gain*after)-tanh_before);
        result.push_back({id,after,delta_length,delta_thermal,
                          delta_thermal-o.length_price*delta_length-o.risk_price*(state.risk(id)-old_risk)});
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b) {
        if (a.estimated_score_delta!=b.estimated_score_delta) return a.estimated_score_delta>b.estimated_score_delta;
        if (a.proxy_length_delta_dbu!=b.proxy_length_delta_dbu) return a.proxy_length_delta_dbu<b.proxy_length_delta_dbu;
        return a.slot<b.slot;
    });
    if (result.size()>o.max_candidates) result.resize(o.max_candidates);
    return result;
}

bool TsvMoveSearch::commit_move(size_t net_index,int new_slot) {
    auto& state=*impl_;
    if (net_index>=state.assignment.size()||new_slot<0||new_slot>=int(state.owner.size())) return false;
    const int old=state.assignment[net_index];
    if (old<0||old==new_slot||state.owner[size_t(new_slot)]!=-1||
        !state.policy.compatible(state.def.nets()[net_index],new_slot)) return false;
    state.pearson->move(state.slot_cells[size_t(old)],state.slot_cells[size_t(new_slot)]);
    state.free_index->remove(new_slot);state.free_index->restore(old);
    state.owner[size_t(new_slot)]=int(net_index);state.owner[size_t(old)]=-1;
    state.assignment[net_index]=new_slot;return true;
}
} // namespace eda
