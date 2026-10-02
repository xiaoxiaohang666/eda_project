#include "router/tsv_policy.hpp"
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <tuple>

namespace eda {
namespace {
constexpr double eps = 1e-6;
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
struct FlowEdge { int to, reverse, capacity; long long cost; };
struct Matching { bool feasible{false}; long long cost{0}; std::vector<int> slots; };
Matching minimum_cost_matching(const std::vector<std::vector<long long>>& costs,
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
        for (int j = 0; j < m; ++j) if (!forbidden[j] && costs[i][j] >= 0) add(i,n+j,costs[i][j]);
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
}

TsvPolicy::TsvPolicy(const DefParser& def, const LefParser& lef, const RoutingGraph& graph)
    : def_(def),lef_(lef),graph_(graph) {
    std::vector<const ViaInfo*> vias;
    for (const auto& [name,via]:lef.vias()) {
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
    std::set<std::string> paired_top;
    for (const auto& b:bottom) {
        std::vector<const PortInstance*> matches;
        for (const auto& t:top) if (same(b.point,t.point)) matches.push_back(&t);
        if (matches.size()!=1) throw std::runtime_error("ambiguous or missing geometric top slot for "+b.name);
        const auto& t=*matches.front();
        if (!paired_top.insert(t.name).second) throw std::runtime_error("top resource slot paired twice: "+t.name);
        SlotCandidate slot;slot.bottom_name=b.name;slot.top_name=t.name;slot.point=b.point;
        const double x1=std::max(b.region.x1,t.region.x1),y1=std::max(b.region.y1,t.region.y1);
        const double x2=std::min(b.region.x2,t.region.x2),y2=std::min(b.region.y2,t.region.y2);
        if (x2<=x1 || y2<=y1) continue;
        slot.region={x1,y1,x2,y2};
        bool found=false;
        for (const auto* via:vias) {
            bool first=true,lower_first=true,upper_first=true;
            for (const auto& shape:via->shapes) {
                const Rect r=translate(shape.rect,slot.point,def.dbu_per_micron());
                slot.envelope=first?r:combine(slot.envelope,r);first=false;
                if (get_layer_index(shape.layer)==10) {slot.lower_pad=lower_first?r:combine(slot.lower_pad,r);lower_first=false;}
                if (get_layer_index(shape.layer)==11) {slot.upper_pad=upper_first?r:combine(slot.upper_pad,r);upper_first=false;}
            }
            if (!contains(slot.region,slot.envelope) || !contains(def.die_area(),slot.envelope)) continue;
            bool blocked=false;
            for (const auto& [component_name,component]:def.components()) {
                const auto* macro=lef.get_macro(component.macro_type);if (!macro) continue;
                for (const auto& obs:macro->obstructions) {
                    const int l=get_layer_index(obs.layer);if (l!=10&&l!=11) continue;
                    const auto obstacle=transform_macro_rect(obs.rect,*macro,component,def.dbu_per_micron());
                    if (obstacle.intersects(l==10?slot.lower_pad:slot.upper_pad)) blocked=true;
                }
            }
            if (blocked) continue;
            found=true;break;
        }
        if (!found) continue;
        const int c=graph.grid().get_col(slot.point.x),r=graph.grid().get_row(slot.point.y);
        if (c<0||r<0) continue;
        slot.lower_node=graph.node(c,r,10);slot.upper_node=graph.node(c,r,11);
        slots_.push_back(std::move(slot));
    }
    if (paired_top.size()!=top.size()) throw std::runtime_error("unpaired top RESOURCE_SLOT region");
    std::sort(slots_.begin(),slots_.end(),[](const auto& a,const auto& b){return std::tie(a.point.x,a.point.y,a.bottom_name,a.top_name)<std::tie(b.point.x,b.point.y,b.bottom_name,b.top_name);});
    for (int i=0;i<static_cast<int>(slots_.size());++i) slots_[i].id=i;
    // The reservation outlines include the supplied replacement macro's keepout.
    for (int i=0;i<static_cast<int>(slots_.size());++i) for (int j=i+1;j<static_cast<int>(slots_.size());++j)
        if (slots_[i].region.intersects(slots_[j].region)) {slots_[i].conflicts.push_back(j);slots_[j].conflicts.push_back(i);}
}

bool TsvPolicy::compatible(const Net& net,int id) const {
    if (!net.is_cross_die || id<0 || id>=static_cast<int>(slots_.size())) return false;
    // Both fixed interface pins must remain connected. An aligned M10/M11 pair
    // forces a crossing at its original position; moving to another slot would
    // create an unexplained second electrical connection even with edge-touch.
    for (const auto& lower:net.pins) if (lower.layer_idx==10)
        for (const auto& upper:net.pins) if (upper.layer_idx==11 && same(lower.coord,upper.coord))
            if (!same(lower.coord,slots_[id].point)) return false;
    return true;
}

std::vector<int> TsvPolicy::assign(const std::vector<Net>& nets,const ThermalParser& thermal) const {
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
    std::vector<std::vector<long long>> costs(cross.size(),std::vector<long long>(slots_.size(),-1));
    const double scale=1000000.0,ell0=std::max(1.0,graph_.ell0());
    for (size_t n=0;n<cross.size();++n) {
        bool available=false;
        for (int s=0;s<static_cast<int>(slots_.size());++s) if (compatible(nets[cross[n]],s)) {
            const auto& slot=slots_[s];
            const double mst=terminal_mst(nets[cross[n]],slot,false,0.2*ell0)+terminal_mst(nets[cross[n]],slot,true,0.2*ell0)+2000.0;
            const double h=maxheat>minheat?(heat[s]-minheat)/(maxheat-minheat):0.0;
            const double risk=0.5/std::max(1.0,graph_.guide_capacity(slot.lower_node))+0.5/std::max(1.0,graph_.guide_capacity(slot.upper_node));
            costs[n][s]=std::llround(scale*(mst/ell0+2*risk+2*(1-h)));available=true;
        }
        if (!available) throw std::runtime_error("TSV expression/policy unresolved: no compatible slot for "+nets[cross[n]].name+" while retaining original pins");
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
    if (!contains(slot.region,inferred)||!contains(inferred,slot.envelope))
        return fail("inferred TSV overlap must contain the actual VIA envelope inside the paired slot");
    for (const auto& crossing:crosses) if (!contains(inferred,crossing))
        return fail("extra M10/M11 electrical connection outside the planned VIA, including edge-touch");
    const auto p=inferred.center();const auto& grid=graph_.grid();
    if (grid.get_col(p.x)!=grid.get_col(slot.point.x)||grid.get_row(p.y)!=grid.get_row(slot.point.y))
        return fail("inferred thermal TSV cell differs from the planned physical TSV cell");
    return true;
}
} // namespace eda
