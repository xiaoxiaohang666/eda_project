#include "router/negotiated_router.hpp"
#include "scoring/compat_validation.hpp"
#include "validation/gcell_encoding.hpp"
#include <queue>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <numeric>
#include <limits>
#include <sstream>
#include <optional>
#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace eda {
namespace {
constexpr double inf = std::numeric_limits<double>::infinity();
// Only the optional topology trial uses this temporary deadline. Every exit,
// including a failed search or encode, restores the initial-routing deadline
// and charges the actual extra work against the shared trial budget.
class ScopedTopologyBudget {
    using Clock=std::chrono::steady_clock;
    Clock::time_point& deadline_;
    bool& active_;
    double& seconds_;
    Clock::time_point saved_,started_;
    bool saved_active_;
public:
    ScopedTopologyBudget(Clock::time_point& deadline,bool& active,double& seconds,double remaining)
        :deadline_(deadline),active_(active),seconds_(seconds),saved_(deadline),started_(Clock::now()),saved_active_(active) {
        deadline_=std::min(deadline_,started_+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(remaining)));
        active_=true;
    }
    ~ScopedTopologyBudget() {
        seconds_+=std::chrono::duration<double>(Clock::now()-started_).count();
        deadline_=saved_;active_=saved_active_;
    }
    ScopedTopologyBudget(const ScopedTopologyBudget&)=delete;
    ScopedTopologyBudget& operator=(const ScopedTopologyBudget&)=delete;
};
struct OfficialCellContribution {
    uint64_t overflow{};
    long double squared{};
    bool zero_capacity{};
};
OfficialCellContribution official_cell_contribution(const RoutingGraph& graph,int node,int guide_demand) {
    const long long total=static_cast<long long>(graph.official_obstacle_demand(node))+guide_demand;
    if(total<0 || total>std::numeric_limits<int>::max())
        throw std::logic_error("official aggregate demand overflow");
    if(!total) return {};
    const double capacity=graph.official_capacity(node);
    if(!std::isfinite(capacity) || capacity<0 || capacity>std::numeric_limits<int>::max())
        throw std::logic_error("invalid official raw capacity");
    const int excess=std::max(0,int(total)-int(capacity));
    if(capacity<=0) return {uint64_t(excess),10000.0L,true};
    const long double ratio=0.1L*excess/capacity;
    return {uint64_t(excess),ratio*ratio,false};
}
double marginal(int u, double c) {
    const double before=std::max(0.0,u-c), after=std::max(0.0,u+1-c);
    return after*after-before*before+std::pow((u+1)/std::max(1.0,c),4);
}
bool overlaps(const GuideRect& a, const Rect& b) {
    return std::min(a.x2,b.x2)>std::max(a.x1,b.x1) &&
           std::min(a.y2,b.y2)>std::max(a.y1,b.y1);
}
GuideRect guide(Rect r,int layer,const Rect& die) {
    return {std::max(die.x1,std::floor(r.x1)),std::max(die.y1,std::floor(r.y1)),
            std::min(die.x2,std::ceil(r.x2)),std::min(die.y2,std::ceil(r.y2)),get_layer_name(layer)};
}
std::string json_string(const std::string& s) {
    std::string r="\"";
    for (unsigned char c:s) {
        if(c=='"'||c=='\\') {r+='\\';r+=c;}
        else if(c<32) {std::ostringstream x;x<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<int(c);r+=x.str();}
        else r+=c;
    }
    return r+'"';
}
}
NegotiatedRouter::NegotiatedRouter(const DefParser& d,const LefParser& l,const ThermalParser& t,RouterOptions o)
    :def_(d),lef_(l),thermal_(t),options_(o),graph_(d,l,o.graph_options),policy_(d,l,graph_,o.tsv_policy),
     demand_(graph_.size()),guide_history_(graph_.size()) {
    if(options_.v7_tsv && (!options_.official_model || !options_.official_metrics.score_20261020 ||
       options_.tsv_policy.resource_mode!=TsvResourceMode::GLOBAL_GRID))
        throw std::invalid_argument("V7 TSV refinement requires official 20261020 global-grid routing");
    if(options_.official_model) {
        for(size_t n=0;n<demand_.size();++n) {
            const auto cell=official_cell_contribution(graph_,int(n),0);
            official_overflow_+=cell.overflow;
            official_squared_overflow_+=cell.squared;
            official_zero_capacity_cells_+=cell.zero_capacity;
        }
        flute_=std::make_unique<NativeFluteBaseline>(options_.official_runtime);
        const double step=std::max(options_.official_metrics.gcell_width_dbu,options_.official_metrics.gcell_height_dbu);
        for(const auto& net:def_.nets()) {
            reference_lengths_.push_back(flute_->measure_net(net,step,options_.official_metrics));
            full_reference_+=reference_lengths_.back();
            if(options_.official_metrics.score_20261020) own_pin_nodes_.push_back(graph_.own_pin_nodes(net));
        }
        if(options_.official_metrics.score_20261020 && full_reference_<=0)
            throw std::runtime_error("20261020 score requires a positive total FLUTE reference");
    }
    started_=std::chrono::steady_clock::now();
    // Keep part of even a short explicit budget available for final encoding/export.
    const double reserve=std::min(60.0,std::max(0.01,options_.time_limit*0.1));
    deadline_=started_+std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(std::max(0.0,options_.time_limit-reserve)));
}
const std::vector<int>& NegotiatedRouter::cell_owners(int node) const {
    static const std::vector<int> empty;
    const auto found=cell_nets_.find(node);
    return found==cell_nets_.end()?empty:found->second;
}
bool NegotiatedRouter::timed_out() const { return std::chrono::steady_clock::now()>=deadline_; }
bool NegotiatedRouter::v7_search_budget_available() const {
    return v7_refinement_active_ && !timed_out() &&
           counters_.expansions-v7_refinement_expansion_start_<options_.v7_rrr_astar_expansions &&
           std::chrono::steady_clock::now()<v7_refinement_deadline_;
}
void NegotiatedRouter::verify_official_ledger(const CompleteMetrics& exact) {
    if(!options_.official_model) return;
    ++counters_.v7_ledger_checks;
    auto near=[](long double a,long double b) {
        return std::isfinite(a) && std::isfinite(b) &&
               std::abs(a-b)<=1e-10L*std::max({1.0L,std::abs(a),std::abs(b)});
    };
    if(!near(official_wirelength_,exact.guide.wirelength) ||
       !near(official_reference_,exact.guide.reference) ||
       !near(official_squared_overflow_,exact.official.squared_normalized_overflow) ||
       official_overflow_!=uint64_t(exact.guide.overflow) ||
       official_zero_capacity_cells_!=exact.official.zero_capacity_cells)
        throw std::logic_error("V7 incremental score ledger differs from complete measurement");
}
void NegotiatedRouter::check_v5_topology_budget() const {
    if(v5_topology_trial_ && timed_out()) throw std::runtime_error("SEARCH_LIMIT: V5 topology work budget exhausted");
}
double NegotiatedRouter::official_congestion_price() const {
    if(!options_.official_model || !options_.official_score_pricing) return 1.0;
    if(pricing_generation_==state_generation_) return congestion_price_;
    // Estimate unfinished nets at their exact reference cost. For L >= R the
    // ratio of the W/C score derivatives calibrates congestion against one
    // geometric ell0 step. Clamping L to R keeps all search costs nonnegative.
    long double scale=1;
    if(full_reference_>0) {
        const long double length=std::max(full_reference_,official_wirelength_+full_reference_-official_reference_);
        const long double squared=std::max(0.0L,official_squared_overflow_);
        scale=length*length/(300.0L*full_reference_*graph_.ell0()*std::pow(1.0L+squared,1.5L));
    }
    congestion_price_=double(std::clamp(scale,0.01L,10000.0L));
    congestion_price_min_=std::min(congestion_price_min_,congestion_price_);
    congestion_price_max_=std::max(congestion_price_max_,congestion_price_);
    pricing_generation_=state_generation_;++pricing_updates_;
    return congestion_price_;
}
bool NegotiatedRouter::pin_exempt(size_t net,int node) const {
    return net<own_pin_nodes_.size() && std::binary_search(own_pin_nodes_[net].begin(),own_pin_nodes_[net].end(),node);
}
bool NegotiatedRouter::validate_route(size_t index,const std::vector<GuideRect>& guides,std::string& error) const {
    if(options_.official_model && options_.tsv_policy.resource_mode==TsvResourceMode::GLOBAL_GRID &&
       !validate_gcell_encoding(def_.nets().at(index),def_.die_area(),graph_.grid(),lef_,guides,error)) return false;
    if(options_.official_metrics.score_20261020)
        return validate_net_guides(def_.nets().at(index),def_.die_area(),lef_,guides,error,def_.gcell_pitch_x(),def_.gcell_pitch_y());
    return validate_net_guides(def_.nets().at(index),def_.die_area(),lef_,guides,error);
}
double NegotiatedRouter::edge_cost(int a,int b,const NetRoute& own) const {
    const auto key=route_edge(a,b);
    if(own.edges.count(key)) return 0;
    double result=graph_.edge_length(a,b)/graph_.ell0();
    if(graph_.layer(a)!=graph_.layer(b)) result+=options_.official_model?options_.official_metrics.via_cost_ratio:0.2;
    if(recovery_cell_cost_ || (options_.v7_cell_cost && options_.official_model)) {
        const auto& m=options_.official_metrics;
        result=own.nodes.count(b)?0:(lef_.get_layer_direction(get_layer_name(graph_.layer(b)))==Direction::HORIZONTAL?
            m.gcell_width_dbu:m.gcell_height_dbu)/graph_.ell0();
        if(graph_.layer(a)!=graph_.layer(b))
            result+=m.via_cost_ratio*std::max(m.gcell_width_dbu,m.gcell_height_dbu)/graph_.ell0();
    }
    const double cap=graph_.edge_capacity(a,b);
    auto it=usage_.find(key);
    const int u=it==usage_.end()?0:it->second;
    if(cap>=0 && !options_.official_model) {
        result+=present_*marginal(u,cap);
        auto h=edge_history_.find(key); if(h!=edge_history_.end()) result+=history_scale_*h->second;
    }
    if(!own.nodes.count(b) && !pin_exempt(own.net_index,b)) {
        if(options_.official_model) {
            const double raw=graph_.official_capacity(b),u=graph_.official_obstacle_demand(b)+demand_[b];
            if(raw<=0) result+=1e6;
            else {double before=std::max(0.0,u-raw)/raw,after=std::max(0.0,u+1-raw)/raw;
                result+=100*official_congestion_price()*present_*0.01*(after*after-before*before)+history_scale_*guide_history_[b];}
        } else result+=present_*marginal(demand_[b],graph_.guide_capacity(b))+history_scale_*guide_history_[b];
    }
    return result;
}
double NegotiatedRouter::access_cost(const PinAccess& a,const NetRoute* own) const {
    double result=a.length/graph_.ell0()+(options_.official_model?options_.official_metrics.via_cost_ratio:0.2)*a.vias;
    const bool cell_cost=recovery_cell_cost_ || (options_.v7_cell_cost && options_.official_model);
    if(cell_cost) result=options_.official_metrics.via_cost_ratio*a.vias*
        std::max(options_.official_metrics.gcell_width_dbu,options_.official_metrics.gcell_height_dbu)/graph_.ell0();
    std::set<int> touched;
    const auto& grid=graph_.grid();
    for(const auto& g:a.guides) {
        int c0=grid.get_col(g.x1), c1=grid.get_col(std::nextafter(g.x2,-inf));
        int r0=grid.get_row(g.y1), r1=grid.get_row(std::nextafter(g.y2,-inf));
        for(int r=r0;r<=r1;++r) for(int c=c0;c<=c1;++c) {
            int n=graph_.node(c,r,get_layer_index(g.layer));
            if(n>=0 && touched.insert(n).second) {
                if(cell_cost && !(options_.v7_cell_cost && own && own->nodes.count(n))) result+=(lef_.get_layer_direction(g.layer)==Direction::HORIZONTAL?
                    options_.official_metrics.gcell_width_dbu:options_.official_metrics.gcell_height_dbu)/graph_.ell0();
                if(options_.official_metrics.score_20261020 && own && (own->nodes.count(n)||pin_exempt(own->net_index,n))) continue;
                if(options_.official_model) {
                    const double raw=graph_.official_capacity(n),u=graph_.official_obstacle_demand(n)+demand_[n];
                    if(raw<=0) result+=1e6;
                    else {double before=std::max(0.0,u-raw)/raw,after=std::max(0.0,u+1-raw)/raw;
                        result+=100*official_congestion_price()*present_*0.01*(after*after-before*before)+history_scale_*guide_history_[n];}
                } else result+=present_*marginal(demand_[n],graph_.guide_capacity(n))+history_scale_*guide_history_[n];
            }
        }
    }
    return result;
}
std::vector<PinAccess> NegotiatedRouter::accesses(Point p,int original,int low,int high,bool terminal_escape) {
    const auto key=std::make_tuple(p.x,p.y,original,low,high,terminal_escape);
    if(auto found=access_cache_.find(key);found!=access_cache_.end()) return found->second;
    std::vector<PinAccess> candidates;
    const auto& grid=graph_.grid();
    if(options_.official_model) {
        const int c=grid.get_col(p.x),r=grid.get_row(p.y),layer=std::clamp(original,low,high);
        const int node=graph_.node(c,r,layer);
        if(!graph_.legal_node(node)) throw std::runtime_error("official Pin outside routable DIE grid");
        PinAccess access;access.node=node;
        for(int l=std::min(original,layer);l<std::max(original,layer);++l) {
            if(l==10) throw std::runtime_error("ordinary access cannot cross DIE");
            if(options_.tsv_policy.resource_mode==TsvResourceMode::GLOBAL_GRID) {
                // The M10 landing reaches the M9 search layer through occupied
                // GCells too; physical via caps would reintroduce thin Guides.
                const auto cell=grid.get_cell_rect(c,r);
                for(int metal:{l,l+1}) access.guides.push_back(guide(cell,metal,def_.die_area()));
            } else {
                for(const auto& shape:graph_.via_rects(p,l))
                    access.guides.push_back(guide(shape.rect,get_layer_index(shape.layer),def_.die_area()));
            }
            ++access.vias;
        }
        return {std::move(access)};
    }
    const int pc=grid.get_col(p.x),pr=grid.get_row(p.y);
    auto add_segment=[&](PinAccess& a,Point x,Point y,int layer)->bool {
        if(!graph_.legal_segment(x,y,layer)) return false;
        a.guides.push_back(guide(graph_.segment_rect(x,y,layer),layer,def_.die_area()));
        a.length+=x.distance_to(y); return true;
    };
    auto add_stack=[&](PinAccess& a,Point x,int first,int last)->bool {
        for(int l=std::min(first,last);l<std::max(first,last);++l) {
            if(l==10 || !graph_.legal_via(x,l)) return false;
            for(const auto& shape:graph_.via_rects(x,l)) {
                const int metal=get_layer_index(shape.layer);
                if(metal) a.guides.push_back(guide(shape.rect,metal,def_.die_area()));
            }
            ++a.vias;
        }
        return true;
    };
    // Real pin access is separate from the GCell center. A legal local via stack
    // and a preferred-direction L connect it to one of several nearby graph nodes.
    std::vector<int> access_layers;
    for(int first=low;first<=high;++first) access_layers.push_back(first);
    const bool fast_access=initial_build_ && large_design() && options_.fast_initial;
    if(fast_access) std::stable_sort(access_layers.begin(),access_layers.end(),[&](int a,int b) {
        return std::tuple(std::abs(a-original),a)<std::tuple(std::abs(b-original),b);
    });
    for(int radius:{1,2,4,8}) {
        for(int first:access_layers) {
            PinAccess prefix;
            if(!add_stack(prefix,p,original,first)) continue;
            const auto dir=lef_.get_layer_direction(get_layer_name(first));
            for(int r=std::max(0,pr-radius);r<=std::min(grid.rows-1,pr+radius);++r)
                for(int c=std::max(0,pc-radius);c<=std::min(grid.cols-1,pc+radius);++c) {
                    const auto q=grid.get_cell_rect(c,r).center();
                    const auto bend=dir==Direction::HORIZONTAL?Point(q.x,p.y):Point(p.x,q.y);
                    PinAccess a=prefix;
                    if(!add_segment(a,p,bend,first)) continue;
                    if(bend.distance_to(q)<1e-8) {
                        int n=graph_.node(c,r,first);
                        if(graph_.legal_node(n) && (!large_design() || graph_.has_neighbor(n,low,high))) {a.node=n;candidates.push_back(a);}
                    }
                    for(int last:{first-1,first+1}) {
                        if(last<low||last>high||lef_.get_layer_direction(get_layer_name(last))==dir) continue;
                        PinAccess b=a;
                        if(!add_stack(b,bend,first,last)||!add_segment(b,bend,q,last)) continue;
                        const int n=graph_.node(c,r,last);
                        if(!graph_.legal_node(n) || (large_design() && !graph_.has_neighbor(n,low,high))) continue;
                        b.node=n;candidates.push_back(std::move(b));
                    }
                }
            if(fast_access && candidates.size()>=8) break;
        }
        if(!candidates.empty()) break;
    }
    // A square cap extends half a wire width behind a terminal. That can
    // intersect a nearby cell OBS even when a full-width outward escape is
    // legal. Try a flat terminal cap only after the original model fails.
    // The corridor retains the LEF width, overlaps the terminal by one DBU,
    // checks all obstacles, and ends where ordinary square-cap access is legal.
    // No OBS exemption or recursive escape chain is permitted.
    if(candidates.empty() && terminal_escape) {
        const double width=graph_.routing_width(original),half=width*0.5;
        const bool horizontal=lef_.get_layer_direction(get_layer_name(original))==Direction::HORIZONTAL;
        for(double factor:{1.0,2.0,4.0}) for(int sign:{-1,1}) {
            const Point q=horizontal?Point(p.x+sign*factor*width,p.y):Point(p.x,p.y+sign*factor*width);
            if(!graph_.legal_segment(q,q,original)) continue;
            const Rect corridor=horizontal?
                Rect(std::min(p.x,q.x)-0.5,p.y-half,std::max(p.x,q.x)+0.5,p.y+half):
                Rect(p.x-half,std::min(p.y,q.y)-0.5,p.x+half,std::max(p.y,q.y)+0.5);
            if(!graph_.legal_rect(corridor,original)) continue;
            const auto aperture=guide(corridor,original,def_.die_area());
            if(!graph_.legal_rect(Rect(aperture.x1,aperture.y1,aperture.x2,aperture.y2),original)) continue;
            try {
                for(auto a:accesses(q,original,low,high,false)) {
                    a.guides.insert(a.guides.begin(),aperture);
                    a.length+=p.distance_to(q);candidates.push_back(std::move(a));
                }
            } catch(const std::runtime_error&) { /* Keep other legal escape directions available. */ }
        }
    }
    std::stable_sort(candidates.begin(),candidates.end(),[&](const auto& a,const auto& b){
        return std::tuple(a.length/graph_.ell0()+0.2*a.vias,a.node,a.vias)<
               std::tuple(b.length/graph_.ell0()+0.2*b.vias,b.node,b.vias);
    });
    std::set<int> nodes;
    std::vector<PinAccess> result;
    for(auto& a:candidates) if(nodes.insert(a.node).second) {
        result.push_back(std::move(a)); if(result.size()==8) break;
    }
    if(result.empty()) throw std::runtime_error("PIN_ACCESS_UNRESOLVED: conservative local access model at "+
        std::to_string(p.x)+","+std::to_string(p.y)+" layer "+std::to_string(original));
    if(large_design() && access_cache_.size()>=4096) access_cache_.clear();
    access_cache_[key]=result;return result;
}
std::vector<PinAccess> NegotiatedRouter::accesses(const Pin& pin,int low,int high) {
    std::string original_failure;
    try { return accesses(pin.coord,pin.layer_idx,low,high); }
    catch(const std::runtime_error& e) { original_failure=e.what(); }
    std::vector<PinAccess> result;
    // LEF PORT metal overrides only its own instance's OBS. A via touching
    // that OBS must have its entire landing inside actual PORT metal. The
    // graph API retains all foreign OBS and checks every other via layer.
    // Reach a normal, unobstructed routing layer before using Point access.
    std::vector<int> parent(pin.port_rects.size(),-1);
    std::vector<size_t> reached;
    for(size_t i=0;i<pin.port_rects.size();++i) {
        const auto& shape=pin.port_rects[i];
        if(shape.layer_idx==pin.layer_idx && shape.rect.contains(pin.coord) &&
           graph_.legal_pin_rect(shape.rect,pin.layer_idx,pin)) {
            parent[i]=int(i);reached.push_back(i);
        }
    }
    for(size_t at=0;at<reached.size();++at) {
        const size_t i=reached[at];
        for(size_t j=0;j<pin.port_rects.size();++j) {
            const auto& shape=pin.port_rects[j];
            if(parent[j]<0 && shape.layer_idx==pin.layer_idx &&
               shape.rect.intersects(pin.port_rects[i].rect) &&
               graph_.legal_pin_rect(shape.rect,pin.layer_idx,pin)) {
                parent[j]=int(i);reached.push_back(j);
            }
        }
    }
    struct TerminalPoint { Point point; int shape; };
    std::vector<TerminalPoint> points;
    if(!reached.empty()) points.push_back({pin.coord,-1});
    for(size_t i:reached) {
        const auto& box=pin.port_rects[i].rect;
        const double inset=graph_.routing_width(pin.layer_idx)*0.5;
        const double x1=std::min(box.x2,std::max(box.x1,box.x1+inset));
        const double x2=std::max(box.x1,std::min(box.x2,box.x2-inset));
        const double y1=std::min(box.y2,std::max(box.y1,box.y1+inset));
        const double y2=std::max(box.y1,std::min(box.y2,box.y2-inset));
        for(Point point:{box.center(),Point(x1,box.center().y),Point(x2,box.center().y),
                         Point(box.center().x,y1),Point(box.center().x,y2)}) {
            bool duplicate=false;for(const auto& old:points)
                if(old.point.distance_to(point)<1e-8) { duplicate=true;break; }
            if(!duplicate) points.push_back({point,int(i)});
        }
    }
    std::vector<int> layers;
    for(int layer=low;layer<=high;++layer) layers.push_back(layer);
    std::stable_sort(layers.begin(),layers.end(),[&](int a,int b) {
        return std::tuple(std::abs(a-pin.layer_idx),a)<std::tuple(std::abs(b-pin.layer_idx),b);
    });
    for(const auto& terminal:points) {
        PinAccess metal;
        bool connected=true;
        if(terminal.shape>=0) {
            int shape=terminal.shape;
            for(;;) {
                const auto& r=pin.port_rects[size_t(shape)].rect;
                // Existing PORT metal is not enlarged to routing WIDTH. An
                // inward integer representation cannot extend beyond the pin.
                const double x1=std::ceil(r.x1),y1=std::ceil(r.y1),x2=std::floor(r.x2),y2=std::floor(r.y2);
                if(x1>=x2 || y1>=y2) { connected=false;break; }
                GuideRect g(x1,y1,x2,y2,pin.layer);
                if(
                   !graph_.legal_pin_rect({g.x1,g.y1,g.x2,g.y2},pin.layer_idx,pin)) {
                    connected=false;break;
                }
                metal.guides.push_back(std::move(g));
                if(parent[size_t(shape)]==shape) break;
                shape=parent[size_t(shape)];
            }
            metal.length=pin.coord.distance_to(terminal.point);
        }
        if(!connected) continue;
        for(int first:layers) {
            if(!graph_.legal_segment(terminal.point,terminal.point,first)) continue;
            PinAccess prefix=metal;
            bool legal=true;
            for(int layer=std::min(pin.layer_idx,first);layer<std::max(pin.layer_idx,first);++layer) {
                if(layer==10 || !graph_.legal_pin_via(terminal.point,layer,pin)) { legal=false;break; }
                for(const auto& shape:graph_.pin_via_rects(terminal.point,layer,pin)) {
                    const int routing=get_layer_index(shape.layer);if(!routing) continue;
                    auto g=guide(shape.rect,routing,def_.die_area());
                    if(!graph_.legal_pin_rect({g.x1,g.y1,g.x2,g.y2},routing,pin)) { legal=false;break; }
                    prefix.guides.push_back(std::move(g));
                }
                if(!legal) break;
                ++prefix.vias;
            }
            if(!legal) continue;
            try {
                for(auto access:accesses(terminal.point,first,low,high,false)) {
                    access.guides.insert(access.guides.begin(),prefix.guides.begin(),prefix.guides.end());
                    access.length+=prefix.length;access.vias+=prefix.vias;
                    result.push_back(std::move(access));
                }
            } catch(const std::runtime_error&) {}
            if(result.size()>=8) break;
        }
        if(result.size()>=8) break;
    }
    if(!result.empty()) {
        std::stable_sort(result.begin(),result.end(),[&](const auto& a,const auto& b) {
            return std::tuple(access_cost(a),a.node)<std::tuple(access_cost(b),b.node);
        });
        std::set<int> nodes;std::vector<PinAccess> unique;
        for(auto& access:result) if(nodes.insert(access.node).second) {
            unique.push_back(std::move(access));if(unique.size()==8) break;
        }
        ++counters_.port_terminal_accesses;return unique;
    }
    if(!options_.cell_pin_access || pin.owner_type!=PinOwnerType::CORE_CELL || !pin.has_owner_bbox)
        throw std::runtime_error(original_failure+"; terminal "+pin.name+
            " has no legal PORT landing or ordinary-via escape");
    // A standard-cell terminal may be narrower than the top-level routing
    // WIDTH. Its local access is an abstract cell-internal connection; outside
    // the owner it retains full WIDTH and every OBS/die check. This does not
    // certify a detailed pin-access DRC solution.
    const double half=graph_.routing_width(pin.layer_idx)*0.5;
    for(const auto& shape:pin.port_rects) {
        if(shape.layer_idx!=pin.layer_idx || !shape.rect.contains(pin.coord)) continue;
        if(!graph_.legal_pin_rect(shape.rect,pin.layer_idx,pin)) continue;
        const auto& owner=pin.owner_bbox;const auto p=pin.coord;
        for(bool horizontal:{true,false}) for(int sign:{-1,1}) {
            Point boundary=horizontal?Point(sign<0?owner.x1:owner.x2,p.y):Point(p.x,sign<0?owner.y1:owner.y2);
            Point q=horizontal?Point(boundary.x+sign*(half+1),p.y):Point(p.x,boundary.y+sign*(half+1));
            Rect inside=horizontal?Rect(std::min(shape.rect.x1,boundary.x),shape.rect.y1,
                                        std::max(shape.rect.x2,boundary.x),shape.rect.y2):
                                   Rect(shape.rect.x1,std::min(shape.rect.y1,boundary.y),
                                        shape.rect.x2,std::max(shape.rect.y2,boundary.y));
            Rect outside=horizontal?Rect(std::min(boundary.x,q.x-half),p.y-half,
                                         std::max(boundary.x,q.x+half),p.y+half):
                                    Rect(p.x-half,std::min(boundary.y,q.y-half),
                                         p.x+half,std::max(boundary.y,q.y+half));
            if(inside.x1<owner.x1 || inside.y1<owner.y1 || inside.x2>owner.x2 || inside.y2>owner.y2) continue;
            if(!graph_.legal_pin_rect(inside,pin.layer_idx,pin) || !graph_.legal_rect(outside,pin.layer_idx) ||
               !graph_.legal_segment(q,q,pin.layer_idx)) continue;
            try {
                for(auto access:accesses(q,pin.layer_idx,low,high,false)) {
                    auto a=guide(inside,pin.layer_idx,def_.die_area()),b=guide(outside,pin.layer_idx,def_.die_area());
                    if(!graph_.legal_pin_rect({a.x1,a.y1,a.x2,a.y2},pin.layer_idx,pin) ||
                       !graph_.legal_rect({b.x1,b.y1,b.x2,b.y2},pin.layer_idx)) continue;
                    access.guides.insert(access.guides.begin(),{a,b});
                    access.length+=p.distance_to(q);result.push_back(std::move(access));
                }
            } catch(const std::runtime_error&) {}
        }
    }
    if(result.empty()) throw std::runtime_error("PIN_ACCESS_UNRESOLVED: no OBS-free cell terminal escape for "+pin.name);
    std::stable_sort(result.begin(),result.end(),[&](const auto& a,const auto& b) {
        return std::tuple(access_cost(a),a.node)<std::tuple(access_cost(b),b.node);
    });
    std::set<int> nodes;std::vector<PinAccess> unique;
    for(auto& access:result) if(nodes.insert(access.node).second) {
        unique.push_back(std::move(access));if(unique.size()==8) break;
    }
    ++counters_.abstract_terminal_accesses;return unique;
}
PathResult NegotiatedRouter::search(const std::vector<PinAccess>& sources,const std::set<int>& targets,
                                   const NetRoute& own,int low,int high,bool dijkstra) {
    check_v5_topology_budget();
    if(sources.empty()||targets.empty()) return {};
    const auto& grid=graph_.grid();
    std::vector<double> source_costs;for(const auto& source:sources) source_costs.push_back(access_cost(source,&own));
    int minc=grid.cols,maxc=0,minr=grid.rows,maxr=0,minl=21,maxl=0;
    for(int n:targets) {minc=std::min(minc,graph_.col(n));maxc=std::max(maxc,graph_.col(n));
        minr=std::min(minr,graph_.row(n));maxr=std::max(maxr,graph_.row(n));
        minl=std::min(minl,graph_.layer(n));maxl=std::max(maxl,graph_.layer(n));}
    const Point boxlo=graph_.point(graph_.node(minc,minr,minl)),boxhi=graph_.point(graph_.node(maxc,maxr,minl));
    // Official large-design refinements use validated template incumbents too.
    // Exact A*/Dijkstra remains available with --fast-initial 0 / --search dijkstra.
    const bool fast_search=(initial_build_ || options_.official_model) && large_design() && options_.fast_initial && !dijkstra;
    const bool improve_fast=fast_search && options_.official_model && options_.fast_quality;
    if(improve_fast) {
        const double cheapest=*std::min_element(source_costs.begin(),source_costs.end());
        for(size_t si=0;si<sources.size();++si)
            if(targets.count(sources[si].node) && source_costs[si]==cheapest) {
                ++counters_.fast_initial_paths;++counters_.fast_tree_shortcuts;
                return {PathResult::Status::FOUND,{sources[si].node},si,source_costs[si]};
            }
    }
    // A geometrically legal GCell can still be isolated by OBS on its edges
    // and via pads. Reject an isolated terminal tree before exploring the die;
    // the caller must choose another real terminal access, not a larger budget.
    if(fast_search && std::all_of(targets.begin(),targets.end(),[&](int n) {
        return graph_.neighbors(n).empty();
    })) {
        PathResult direct;direct.cost=inf;
        for(size_t i=0;i<sources.size();++i) if(targets.count(sources[i].node)) {
            const double cost=source_costs[i];
            if(cost<direct.cost) direct={PathResult::Status::FOUND,{sources[i].node},i,cost};
        }
        return direct;
    }
    auto heuristic=[&](int n) {
        // Free existing-tree edges/cells invalidate a geometric lower bound.
        // The bounded V7 refinement uses h=0; templates remain its incumbent.
        if(dijkstra || v7_refinement_active_ || (options_.v7_cell_cost && options_.official_model)) return 0.0;
        if(recovery_cell_cost_) {
            const int steps=std::max({0,minc-graph_.col(n),graph_.col(n)-maxc})+
                std::max({0,minr-graph_.row(n),graph_.row(n)-maxr});
            const auto& m=options_.official_metrics;
            return (std::max(0,steps-1)*std::min(m.gcell_width_dbu,m.gcell_height_dbu)+
                m.via_cost_ratio*std::max(m.gcell_width_dbu,m.gcell_height_dbu)*
                std::max({0,minl-graph_.layer(n),graph_.layer(n)-maxl}))/graph_.ell0();
        }
        auto p=graph_.point(n);
        return (fast_search?8.0:1.0)*(std::max({0.0,boxlo.x-p.x,p.x-boxhi.x})+std::max({0.0,boxlo.y-p.y,p.y-boxhi.y}))/graph_.ell0()+
            (options_.official_model?options_.official_metrics.via_cost_ratio:0.2)*std::max({0,minl-graph_.layer(n),graph_.layer(n)-maxl});
    };
    for(const auto& a:sources) {minc=std::min(minc,graph_.col(a.node));maxc=std::max(maxc,graph_.col(a.node));
        minr=std::min(minr,graph_.row(a.node));maxr=std::max(maxr,graph_.row(a.node));}
    // Layer-aware L/Z templates provide a legal incumbent. A* still proves the
    // best cost under the same snapshot, so Dijkstra remains an exact oracle.
    PathResult incumbent; incumbent.cost=inf;
    bool incumbent_from_z=false;
    // A connection ends at its first tree node. Keeping a later tail can add
    // cycles, cells and vias even though the net is already connected.
    auto consider=[&](std::vector<int> path,size_t si,bool z=false,bool rank_layer=false,double rank_bound=inf) {
        check_v5_topology_budget();
        if(improve_fast) {
            const auto hit=std::find_if(path.begin(),path.end(),[&](int n){return targets.count(n);});
            if(hit!=path.end() && hit+1!=path.end()) {
                path.erase(hit+1,path.end());++counters_.fast_truncated_paths;
            }
        }
        double cost=source_costs[si],base=cost;
        for(size_t k=1;k<path.size();++k) {
            if((k&255U)==0) check_v5_topology_budget();
            cost+=edge_cost(path[k-1],path[k],own);
            if(improve_fast && !own.edges.count(route_edge(path[k-1],path[k]))) {
                if(recovery_cell_cost_ || (options_.v7_cell_cost && options_.official_model)) {
                    const auto& m=options_.official_metrics;
                    base+=(own.nodes.count(path[k])?0:(lef_.get_layer_direction(get_layer_name(graph_.layer(path[k])))==Direction::HORIZONTAL?
                        m.gcell_width_dbu:m.gcell_height_dbu)/graph_.ell0())+
                        (graph_.layer(path[k-1])!=graph_.layer(path[k])?
                        m.via_cost_ratio*std::max(m.gcell_width_dbu,m.gcell_height_dbu)/graph_.ell0():0);
                } else base+=graph_.edge_length(path[k-1],path[k])/graph_.ell0()+
                    (graph_.layer(path[k-1])!=graph_.layer(path[k])?options_.official_metrics.via_cost_ratio:0.0);
            }
            if(fast_search && !rank_layer && cost>=incumbent.cost) break;
            // Only two layer minima can receive Z alternatives. A partial L
            // already worse than both their completed minima (or its own
            // layer minimum) cannot win either comparison. Strict > keeps ties.
            if(rank_layer && cost>rank_bound) {
                ++counters_.fast_layer_prunes;return std::pair(inf,false);
            }
        }
        if(cost<incumbent.cost) {
            incumbent={PathResult::Status::FOUND,std::move(path),si,cost};incumbent_from_z=z;
        }
        return std::pair(cost,cost>base+1e-9);
    };
    if(!dijkstra) {
        std::vector<std::pair<double,int>> ranked;ranked.reserve(targets.size());
        for(int n:targets) {
            double d=inf;const auto p=graph_.point(n);
            // Fast refinement tries only the first two source accesses below;
            // avoid the unused all-source/all-target matrix for repair forests.
            const size_t count=options_.official_model&&fast_search?std::min<size_t>(2,sources.size()):sources.size();
            for(size_t j=0;j<count;++j) {
                const int source=sources[j].node;
                double distance=graph_.point(source).distance_to(p);
                if(options_.official_model&&fast_search)
                    distance=distance/graph_.ell0()+options_.official_metrics.via_cost_ratio*std::abs(graph_.layer(source)-graph_.layer(n));
                d=std::min(d,distance);
            }
            ranked.emplace_back(d,n);
        }
        const size_t nearest=std::min<size_t>(fast_search?2:4,ranked.size());
        std::partial_sort(ranked.begin(),ranked.begin()+nearest,ranked.end());
        std::vector<int> near;for(size_t i=0;i<nearest;++i) near.push_back(ranked[i].second);
        for(size_t si=0;si<std::min<size_t>(sources.size(),fast_search?2:4);++si) for(int target:near) {
            check_v5_topology_budget();
            const int source=sources[si].node;
            std::vector<std::tuple<double,int,bool>> single_layers;
            std::vector<double> layer_minima(size_t(high-low+1),inf);
            if(graph_.allows_nonpreferred()) for(int l=low;l<=high;++l) for(bool x_first:{true,false}) {
                check_v5_topology_budget();
                std::vector<int> path{source};bool legal=true;
                auto append=[&](int n) {if(n<0 || !graph_.legal_edge(path.back(),n)) {legal=false;return;}path.push_back(n);};
                auto stack=[&](int layer) {while(legal && graph_.layer(path.back())!=layer) {
                    int n=path.back(),next=graph_.layer(n)+(layer>graph_.layer(n)?1:-1);append(graph_.node(graph_.col(n),graph_.row(n),next));}};
                stack(l);
                for(bool x_axis:{x_first,!x_first}) while(legal) {
                    int n=path.back(),c=graph_.col(n),r=graph_.row(n);
                    if(x_axis) {if(c==graph_.col(target)) break;c+=graph_.col(target)>c?1:-1;}
                    else {if(r==graph_.row(target)) break;r+=graph_.row(target)>r?1:-1;}
                    append(graph_.node(c,r,l));
                }
                stack(graph_.layer(target));if(!legal || path.back()!=target) continue;
                double bound=inf;
                if(improve_fast && options_.fast_layer_pruning) {
                    double first=inf,second=inf;
                    for(double minimum:layer_minima)
                        if(minimum<first) {second=first;first=minimum;}
                        else if(minimum<second) second=minimum;
                    bound=std::max(incumbent.cost,std::min(second,layer_minima[size_t(l-low)]));
                }
                const auto [cost,congested]=consider(std::move(path),si,false,improve_fast,bound);
                if(improve_fast && std::isfinite(cost)) {
                    single_layers.emplace_back(cost,l,congested);
                    layer_minima[size_t(l-low)]=std::min(layer_minima[size_t(l-low)],cost);
                }
            }
            if(improve_fast) {
                // Congested corridors among the two cheapest single-layer L
                // layers receive midpoint Z alternatives. This keeps the extra work
                // bounded independently of die area and preserves the L incumbent.
                std::sort(single_layers.begin(),single_layers.end());
                std::set<int> sampled;
                for(auto [cost,l,congested]:single_layers) {
                    (void)cost;
                    if(!sampled.insert(l).second) continue;
                    if(sampled.size()>2) break;
                    if(!congested) continue;
                    const int sc=graph_.col(source),sr=graph_.row(source),tc=graph_.col(target),tr=graph_.row(target);
                    for(bool x_mid:{true,false}) {
                        const int mid=x_mid?(sc+tc)/2:(sr+tr)/2;
                        if(x_mid?(mid==sc || mid==tc):(mid==sr || mid==tr)) continue;
                        ++counters_.fast_z_candidates;
                        std::vector<int> path{source};bool legal=true;
                        auto append=[&](int n) {
                            if(!legal) return;
                            if(n<0 || !graph_.legal_edge(path.back(),n)) {legal=false;return;}
                            path.push_back(n);
                        };
                        auto stack=[&](int layer) {
                            while(legal && graph_.layer(path.back())!=layer) {
                                const int n=path.back();
                                append(graph_.node(graph_.col(n),graph_.row(n),graph_.layer(n)+(layer>graph_.layer(n)?1:-1)));
                            }
                        };
                        auto leg=[&](int c,int r) {
                            while(legal && (graph_.col(path.back())!=c || graph_.row(path.back())!=r)) {
                                const int n=path.back();int nc=graph_.col(n),nr=graph_.row(n);
                                if(nc!=c) nc+=c>nc?1:-1;else nr+=r>nr?1:-1;
                                append(graph_.node(nc,nr,l));
                            }
                        };
                        stack(l);
                        if(x_mid) {leg(mid,sr);leg(mid,tr);leg(tc,tr);}
                        else {leg(sc,mid);leg(tc,mid);leg(tc,tr);}
                        stack(graph_.layer(target));
                        if(legal && path.back()==target) consider(std::move(path),si,true);
                    }
                }
            }
            std::vector<std::tuple<int,int,int>> pairs;
            for(int h=low;h<=high;++h) if(lef_.get_layer_direction(get_layer_name(h))==Direction::HORIZONTAL)
                for(int v=low;v<=high;++v) if(lef_.get_layer_direction(get_layer_name(v))==Direction::VERTICAL)
                    pairs.emplace_back(std::abs(graph_.layer(source)-h)+std::abs(h-v)+std::abs(v-graph_.layer(target)),h,v);
            std::sort(pairs.begin(),pairs.end());
            if(fast_search) {
                const auto all=pairs;
                if(pairs.size()>2) pairs.resize(2);
                auto include=[&](const auto& pair) {
                    if(std::find(pairs.begin(),pairs.end(),pair)==pairs.end()) pairs.push_back(pair);
                };
                // The budget covers routing layers, rather than four shapes
                // on the same low layer pair. Every horizontal layer gets its
                // cheapest-via vertical partner; the top fallback pairs also
                // compete using the same current congestion cost.
                std::set<int> sampled_horizontal;
                for(const auto& pair:all) {
                    const auto [rank,h,v]=pair;(void)rank;(void)v;
                    if(sampled_horizontal.insert(h).second) include(pair);
                }
                for(const auto& pair:all) {
                    const auto [rank,h,v]=pair;(void)rank;
                    if(h>=high-2 && v>=high-2) include(pair);
                }
            } else if(pairs.size()>4) pairs.resize(4);
            for(auto [rank,h,v]:pairs) {
              check_v5_topology_budget();
              bool pair_has_legal_l=false;
              for(int kind=0;kind<4;++kind) {
                // Two legal L shapes are enough to compare this layer pair.
                // Retain Z fallbacks when both L corridors are obstructed.
                if(fast_search && kind>=2 && pair_has_legal_l) break;
                (void)rank;std::vector<int> path{source};bool legal=true;
                auto append=[&](int n) {if(!legal)return;if(n<0||!graph_.legal_edge(path.back(),n)){legal=false;return;}path.push_back(n);};
                auto stack=[&](int l) {while(legal&&graph_.layer(path.back())!=l) {
                    int n=path.back(),next=graph_.layer(n)+(l>graph_.layer(n)?1:-1);
                    append(graph_.node(graph_.col(n),graph_.row(n),next));}};
                auto leg=[&](int c,int r,int l) {
                    if(improve_fast && graph_.col(path.back())==c && graph_.row(path.back())==r) return;
                    stack(l);while(legal&&(graph_.col(path.back())!=c||graph_.row(path.back())!=r)) {
                        int n=path.back(),nc=graph_.col(n),nr=graph_.row(n);
                        if(nc!=c) nc+=c>nc?1:-1;else nr+=r>nr?1:-1;
                        append(graph_.node(nc,nr,l));
                    }
                };
                const int sc=graph_.col(source),sr=graph_.row(source),tc=graph_.col(target),tr=graph_.row(target);
                if(kind==0) {leg(tc,sr,h);leg(tc,tr,v);}
                if(kind==1) {leg(sc,tr,v);leg(tc,tr,h);}
                if(kind==2) {int mid=(sc+tc)/2;leg(mid,sr,h);leg(mid,tr,v);leg(tc,tr,h);}
                if(kind==3) {int mid=(sr+tr)/2;leg(sc,mid,v);leg(tc,mid,h);leg(tc,tr,v);}
                stack(graph_.layer(target));if(!legal||path.back()!=target) continue;
                consider(std::move(path),si);
                if(kind<2) pair_has_legal_l=true;
              }
            }
        }
    }
    if(fast_search && incumbent.status==PathResult::Status::FOUND && !v7_search_budget_available()) {
        if(incumbent_from_z) ++counters_.fast_z_paths;
        if(v7_refinement_active_) ++counters_.v7_astar_fallbacks;
        ++counters_.fast_initial_paths;return incumbent;
    }
    if(v7_refinement_active_) ++counters_.v7_astar_searches;
    size_t expanded=0;
    for(int margin=4;;margin*=2) {
        const int lc=std::max(0,minc-margin),hc=std::min(grid.cols-1,maxc+margin),
                  lr=std::max(0,minr-margin),hr=std::min(grid.rows-1,maxr+margin);
        // Large designs allocate search state only for reached nodes. Even a
        // local window must not clear three arrays over the whole 3D grid.
        // Keep the small-design path unchanged for deterministic regressions.
        struct SearchRecord { double distance{inf}; int parent{-1}, origin{-1}; };
        const bool sparse=graph_.size()>262144;
        std::vector<double> dist(sparse?0:graph_.size(),inf);
        std::vector<int> parent(sparse?0:graph_.size(),-1), origin(sparse?0:graph_.size(),-1);
        std::unordered_map<int,SearchRecord> reached;
        if(sparse) reached.reserve(std::min<size_t>(options_.search_limit,4096));
        auto distance=[&](int n) {
            if(!sparse) return dist[n];
            auto it=reached.find(n);return it==reached.end()?inf:it->second.distance;
        };
        auto source_of=[&](int n) { return sparse?reached.at(n).origin:origin[n]; };
        auto parent_of=[&](int n) { return sparse?reached.at(n).parent:parent[n]; };
        auto update=[&](int n,double d,int p,int source) {
            if(sparse) {
                reached[n]={d,p,source};
                counters_.search_peak_states=std::max(counters_.search_peak_states,reached.size());
            } else {
                dist[n]=d;parent[n]=p;origin[n]=source;
                counters_.search_peak_states=std::max(counters_.search_peak_states,graph_.size());
            }
        };
        using Q=std::tuple<double,double,int>;
        std::priority_queue<Q,std::vector<Q>,std::greater<Q>> todo;
        for(size_t i=0;i<sources.size();++i) {
            const int n=sources[i].node; const double cost=source_costs[i];
            if(cost<distance(n)) {update(n,cost,-1,int(i));todo.emplace(cost+heuristic(n),cost,n);}
        }
        while(!todo.empty()) {
            auto [f,cost,n]=todo.top();todo.pop(); (void)f;
            if(cost!=distance(n)) continue;
            if(f>incumbent.cost+1e-10) continue;
            if(++expanded>options_.search_limit || ((expanded&255U)==0 && timed_out()) ||
               (v7_refinement_active_ && !v7_search_budget_available())) {
                // Optional maze search never discards its valid template when
                // its work budget expires. The batch still decides exact gain.
                if(v7_refinement_active_ && incumbent.status==PathResult::Status::FOUND) {
                    ++counters_.v7_astar_fallbacks;return incumbent;
                }
                return {PathResult::Status::SEARCH_LIMIT,{},0,0};
            }
            ++counters_.expansions;
            if(targets.count(n)) {
                PathResult result{PathResult::Status::FOUND,{},static_cast<size_t>(source_of(n)),cost};
                for(int p=n;p!=-1;p=parent_of(p)) result.nodes.push_back(p);
                std::reverse(result.nodes.begin(),result.nodes.end());return result;
            }
            for(int next:graph_.neighbors(n)) {
                if(graph_.layer(next)<low||graph_.layer(next)>high||graph_.col(next)<lc||graph_.col(next)>hc||
                   graph_.row(next)<lr||graph_.row(next)>hr) continue;
                const double d=cost+edge_cost(n,next,own);
                if(d+1e-10<distance(next)) {
                    update(next,d,n,source_of(n));todo.emplace(d+heuristic(next),d,next);
                }
            }
        }
        if(incumbent.status==PathResult::Status::FOUND) {++counters_.templates_used;return incumbent;}
        if(lc==0&&hc==grid.cols-1&&lr==0&&hr==grid.rows-1) return {};
    }
}
void NegotiatedRouter::encode(size_t index,NetRoute& route) {
    check_v5_topology_budget();
    route.net_index=index;
    std::vector<GuideRect> untrimmed_upper;
    route.guides.clear();route.length=0;route.vias=0;
    const auto& grid=graph_.grid();
    // Maximal preferred-direction runs partition the set of occupied GCells.
    std::set<int> pending=route.explicit_geometry?std::set<int>{}:route.nodes;
    while(!pending.empty()) {
        check_v5_topology_budget();
        const int n=*pending.begin();pending.erase(pending.begin());
        const int l=graph_.layer(n),c=graph_.col(n),r=graph_.row(n);
        const bool horizontal=lef_.get_layer_direction(get_layer_name(l))==Direction::HORIZONTAL;
        int end=horizontal?c:r;
        while(true) {
            int next=horizontal?graph_.node(end+1,r,l):graph_.node(c,end+1,l);
            const int previous=horizontal?graph_.node(end,r,l):graph_.node(c,end,l);
            if(next<0 || (large_design() && !route.edges.count(route_edge(previous,next))) || !pending.erase(next)) break;
            ++end;
        }
        Rect a=grid.get_cell_rect(c,r),b=grid.get_cell_rect(horizontal?end:c,horizontal?r:end);
        if(large_design() && !options_.official_model) {
            const auto corridor=graph_.segment_rect(a.center(),b.center(),l);
            route.guides.push_back(guide(corridor,l,def_.die_area()));
        } else if(options_.official_model) route.guides.push_back(guide(Rect(a.x1,a.y1,b.x2,b.y2),l,def_.die_area()));
        else route.guides.emplace_back(a.x1,a.y1,b.x2,b.y2,get_layer_name(l));
    }
    if(!route.explicit_geometry) for(auto key:route.edges) {auto [a,b]=edge_nodes(key);route.length+=graph_.edge_length(a,b);
        if(graph_.layer(a)!=graph_.layer(b)) {
            ++route.vias;
            if(large_design() && !options_.official_model) for(const auto& shape:graph_.via_rects(graph_.point(a),std::min(graph_.layer(a),graph_.layer(b)))) {
                int metal=get_layer_index(shape.layer);
                if(metal) route.guides.push_back(guide(shape.rect,metal,def_.die_area()));
            }
        }
    }
    for(const auto& a:route.accesses) {
        route.guides.insert(route.guides.end(),a.guides.begin(),a.guides.end());
        route.length+=a.length;route.vias+=a.vias;
    }
    // The fixed checker uses untransformed instance-pin coordinates. Preserve the
    // real access; only widen its small guide aperture when the complete aperture
    // is geometrically free. Never move a terminal or claim a wire through OBS.
    for(const auto& p:def_.nets()[index].pins) if(!options_.official_model && p.has_evaluator_coord) {
        bool covered=false;
        for(const auto& g:route.guides) if(g.layer==p.layer&&Rect(g.x1,g.y1,g.x2,g.y2).contains(p.evaluator_coord)) covered=true;
        if(!covered) {
            double half=graph_.routing_width(p.layer_idx)*0.5;
            Rect aperture(std::min(p.coord.x,p.evaluator_coord.x)-half,std::min(p.coord.y,p.evaluator_coord.y)-half,
                          std::max(p.coord.x,p.evaluator_coord.x)+half,std::max(p.coord.y,p.evaluator_coord.y)+half);
            auto g=guide(aperture,p.layer_idx,def_.die_area());
            if(!graph_.legal_rect(Rect(g.x1,g.y1,g.x2,g.y2),p.layer_idx)) {
                // The fixed checker ignores instance orientation. Correct DEF
                // terminal geometry takes precedence over an illegal aperture.
                ++counters_.omitted_checker_apertures;continue;
            }
            route.guides.push_back(g);
        }
    }
    if(route.slot>=0) {
        const auto& slot=policy_.slots().at(route.slot);
        route.guides.push_back(guide(slot.lower_pad,10,def_.die_area()));
        route.guides.push_back(guide(slot.upper_pad,11,def_.die_area()));
        if(options_.official_model && !large_design()) for(const auto& g:route.guides)
            if(get_layer_index(g.layer)==11) untrimmed_upper.push_back(g);
        // Preserve one explicit interface witness without growing a bounding
        // box across empty space or OBS. The remaining guides are the exact
        // rectangle difference outside the landing and still touch its edge.
        for(int l:{10,11}) {
            const auto pad=l==10?slot.lower_pad:slot.upper_pad;
            if(large_design() || options_.official_model) {
                std::vector<GuideRect> rest;
                for(const auto& g:route.guides) {
                    if(get_layer_index(g.layer)!=l || !overlaps(g,pad)) {rest.push_back(g);continue;}
                    const double x1=std::max(g.x1,pad.x1),x2=std::min(g.x2,pad.x2),
                                 y1=std::max(g.y1,pad.y1),y2=std::min(g.y2,pad.y2);
                    if(g.x1<x1) rest.emplace_back(g.x1,g.y1,x1,g.y2,g.layer);
                    if(x2<g.x2) rest.emplace_back(x2,g.y1,g.x2,g.y2,g.layer);
                    if(g.y1<y1) rest.emplace_back(x1,g.y1,x2,y1,g.layer);
                    if(y2<g.y2) rest.emplace_back(x1,y2,x2,g.y2,g.layer);
                }
                rest.push_back(guide(pad,l,def_.die_area()));route.guides=std::move(rest);
            } else {
                Rect box=pad;std::vector<GuideRect> rest;
                for(const auto& g:route.guides) {
                    if(get_layer_index(g.layer)==l && overlaps(g,pad))
                        box=Rect(std::min(box.x1,g.x1),std::min(box.y1,g.y1),std::max(box.x2,g.x2),std::max(box.y2,g.y2));
                    else rest.push_back(g);
                }
                rest.push_back(guide(box,l,def_.die_area()));route.guides=std::move(rest);
            }
        }
    }
    auto key=[](const GuideRect& r){return std::tuple(get_layer_index(r.layer),r.x1,r.y1,r.x2,r.y2);};
    std::sort(route.guides.begin(),route.guides.end(),[&](auto& a,auto& b){return key(a)<key(b);});
    route.guides.erase(std::unique(route.guides.begin(),route.guides.end(),[&](auto& a,auto& b){return key(a)==key(b);}),route.guides.end());
    // Large designs merge collinear strips in O(R log R); small designs retain
    // the legacy exact-rectangle merge order for deterministic evaluator control.
    if(large_design() || options_.official_metrics.score_20261020) for(bool horizontal:{true,false}) {
        auto strip_key=[&](const GuideRect& r) {
            return horizontal?std::tuple(r.layer,r.y1,r.y2,r.x1,r.x2):std::tuple(r.layer,r.x1,r.x2,r.y1,r.y2);
        };
        std::sort(route.guides.begin(),route.guides.end(),[&](const auto& a,const auto& b){return strip_key(a)<strip_key(b);});
        std::vector<GuideRect> merged;
        for(const auto& r:route.guides) {
            bool combine=false;
            const bool preferred=horizontal==(lef_.get_layer_direction(r.layer)==Direction::HORIZONTAL);
            if(!merged.empty() && (!options_.official_metrics.score_20261020 || preferred)) {
                auto& a=merged.back();
                if(horizontal && a.layer==r.layer && a.y1==r.y1 && a.y2==r.y2 && r.x1<=a.x2) {
                    a.x2=std::max(a.x2,r.x2);combine=true;
                }
                if(!horizontal && a.layer==r.layer && a.x1==r.x1 && a.x2==r.x2 && r.y1<=a.y2) {
                    a.y2=std::max(a.y2,r.y2);combine=true;
                }
            }
            if(!combine) merged.push_back(r);
        }
        route.guides=std::move(merged);
    }
    if(large_design()) {
        // Remove duplicate internal apertures covered by a larger exact guide.
        // This preserves the geometric union and avoids counting redundant
        // rectangles as separate demand/MST vertices.
        std::vector<bool> contained(route.guides.size());
        for(size_t i=0;i<route.guides.size();++i) for(size_t j=0;j<route.guides.size();++j) {
            if((j&255U)==0) check_v5_topology_budget();
            if(i==j || contained[j]) continue;
            const auto& a=route.guides[i];const auto& b=route.guides[j];
            if(a.layer!=b.layer || a.x1<b.x1 || a.y1<b.y1 || a.x2>b.x2 || a.y2>b.y2) continue;
            if(key(a)!=key(b) || j<i) {contained[i]=true;break;}
        }
        std::vector<GuideRect> reduced;
        for(size_t i=0;i<route.guides.size();++i) if(!contained[i]) reduced.push_back(std::move(route.guides[i]));
        route.guides=std::move(reduced);
    }
    bool changed=!large_design() && !options_.official_metrics.score_20261020;
    while(changed) {
        changed=false;
        for(size_t i=0;i<route.guides.size()&&!changed;++i) for(size_t j=i+1;j<route.guides.size();++j) {
            auto& a=route.guides[i];const auto& b=route.guides[j];if(a.layer!=b.layer) continue;
            const double dx=std::max(0.0,std::min(a.x2,b.x2)-std::max(a.x1,b.x1));
            const double dy=std::max(0.0,std::min(a.y2,b.y2)-std::max(a.y1,b.y1));
            Rect box(std::min(a.x1,b.x1),std::min(a.y1,b.y1),std::max(a.x2,b.x2),std::max(a.y2,b.y2));
            const double area=(a.x2-a.x1)*(a.y2-a.y1)+(b.x2-b.x1)*(b.y2-b.y1)-dx*dy;
            if(std::abs(box.width()*box.height()-area)<0.5) {
                a=GuideRect(box.x1,box.y1,box.x2,box.y2,a.layer);
                route.guides.erase(route.guides.begin()+static_cast<std::ptrdiff_t>(j));changed=true;break;
            }
        }
    }
    std::sort(route.guides.begin(),route.guides.end(),[&](auto& a,auto& b){return key(a)<key(b);});
    if(large_design()) for(const auto& g:route.guides) {
        const Rect rect(g.x1,g.y1,g.x2,g.y2);const int l=get_layer_index(g.layer);
        if(graph_.legal_rect(rect,l)) continue;
        bool terminal=false;
        for(const auto& pin:def_.nets()[index].pins) if(graph_.legal_pin_rect(rect,l,pin)) {terminal=true;break;}
        if(!terminal) throw std::runtime_error("GUIDE_OBS_VIOLATION: "+g.layer+" "+std::to_string(g.x1)+","+std::to_string(g.y1)+","+std::to_string(g.x2)+","+std::to_string(g.y2)+" for "+def_.nets()[index].name);
    }
    std::string error;
    check_v5_topology_budget();
    if(!policy_.validate_mapping(def_.nets()[index],route.guides,route.slot,error)) {
        bool repaired=false;
        if(options_.official_model && !large_design() && route.slot>=0) {
            const auto& pad=policy_.slots().at(route.slot).lower_pad;
            // Whole-GCell terminal apertures can overlap the upper landing
            // outside a narrow TSV pad. They are redundant only when every
            // real M10 pin is inside the landing and no retained graph branch
            // uses M10. Preserve all already-valid encodings unchanged.
            const bool confined=std::all_of(def_.nets()[index].pins.begin(),def_.nets()[index].pins.end(),
                [&](const Pin& pin){return pin.layer_idx!=10 || pad.contains(pin.coord);});
            const bool aperture_only=std::none_of(route.nodes.begin(),route.nodes.end(),
                [&](int node){return graph_.layer(node)==10;});
            if(confined && aperture_only) {
                auto retry=route.guides;
                retry.erase(std::remove_if(retry.begin(),retry.end(),[](const GuideRect& g){const int l=get_layer_index(g.layer);return l==10 || l==11;}),retry.end());
                retry.push_back(guide(pad,10,def_.die_area()));
                // Restore the original integer upper union; rounded narrow
                // pads must not leave half-DBU strips or spurious crossings.
                std::vector<bool> contained(untrimmed_upper.size());
                for(size_t i=0;i<untrimmed_upper.size();++i) for(size_t j=0;j<untrimmed_upper.size();++j) {
                    if(i==j) continue;
                    const auto& x=untrimmed_upper[i];const auto& y=untrimmed_upper[j];
                    if(x.x1>=y.x1 && x.y1>=y.y1 && x.x2<=y.x2 && x.y2<=y.y2 &&
                        (key(x)!=key(y) || j<i)) {contained[i]=true;break;}
                }
                for(size_t i=0;i<untrimmed_upper.size();++i) if(!contained[i]) retry.push_back(untrimmed_upper[i]);
                std::string retry_error;
                if(policy_.validate_mapping(def_.nets()[index],retry,route.slot,retry_error)) {
                    route.guides=std::move(retry);repaired=true;
                }
            }
        }
        if(!repaired) throw std::runtime_error("TSV_MAPPING_UNRESOLVED: "+error);
    }
    // Full connectivity and pin checks for just this net, without fabricating other guides.
    for(const auto& p:def_.nets()[index].pins) {
        bool found=false;
        for(const auto& g:route.guides) if(g.layer==p.layer && Rect(g.x1,g.y1,g.x2,g.y2).contains(p.coord)) found=true;
        if(!found) throw std::runtime_error("HARD_RULE_VIOLATION: pin lost: "+p.name);
    }
    if((options_.official_metrics.score_20261020 ||
        (options_.official_model && options_.tsv_policy.resource_mode==TsvResourceMode::GLOBAL_GRID)) &&
       !validate_route(index,route.guides,error))
        throw std::runtime_error("HARD_RULE_VIOLATION: "+error);
    route.measured=options_.official_model?measure_official_net_guides(def_.nets()[index],route.guides,graph_,lef_,options_.official_metrics,reference_lengths_.at(index)):
        measure_net_guides(route.guides,graph_,lef_);
    check_v5_topology_budget();
}
NetRoute NegotiatedRouter::build_tree(size_t index,int slot,int variant,const std::array<int,2>& seeds,int only_side,bool tsv_last) {
    const auto& net=def_.nets()[index];NetRoute result;result.slot=slot;result.net_index=index;
    for(int side=0;side<2;++side) {
        if(only_side>=0 && side!=only_side) continue;
        std::vector<Pin> terminals;
        for(const auto& p:net.pins) if((p.layer_idx>10)==bool(side)) terminals.push_back(p);
        if(terminals.empty()) continue;
        int low=side?11:1,high=side?20:10;
        if(slot>=0&&!side) high=9; // M10 landing remains a single explicit TSV witness.
        if(slot>=0) {
            const auto& s=policy_.slots()[slot];
            Pin p;p.name="planned_tsv";p.coord=s.point;p.layer_idx=side?11:10;p.layer=get_layer_name(p.layer_idx);
            terminals.insert(terminals.begin(),p);
        }
        std::vector<Pin> unique;
        for(const auto& p:terminals) {
            bool seen=false;for(const auto& q:unique) if(p.layer_idx==q.layer_idx&&p.coord.distance_to(q.coord)<1e-8) seen=true;
            if(!seen) unique.push_back(p);
        }
        terminals=std::move(unique);
        size_t root=0;
        if(slot<0&&variant) {
            for(size_t i=1;i<terminals.size();++i)
                if(terminals[i].coord.distance_to(terminals[0].coord)>terminals[root].coord.distance_to(terminals[0].coord)) root=i;
        }
        const bool pin_rooted=tsv_last && slot>=0 && terminals.size()>1;
        if(pin_rooted) {
            // Grow the local real-pin tree before joining its fixed landing.
            // A medoid is an actual terminal, so no artificial Steiner leaf or
            // additional terminal aperture has to be emitted. With the caller's
            // fanout bound this O(P^2) choice stays independent of die area.
            root=1;double best=inf;
            for(size_t i=1;i<terminals.size();++i) {
                double cost=0;
                for(size_t j=1;j<terminals.size();++j)
                    cost+=terminals[i].coord.distance_to(terminals[j].coord)/graph_.ell0()+
                        0.2*std::abs(terminals[i].layer_idx-terminals[j].layer_idx);
                if(cost<best) {best=cost;root=i;}
            }
        }
        std::vector<bool> added(terminals.size());std::vector<double> nearest_terminal(terminals.size(),inf);std::set<int> tree;
        if(seeds[side]>=0) {tree.insert(seeds[side]);result.nodes.insert(seeds[side]);}
        for(size_t count=0;count<terminals.size();++count) {
            if(timed_out()) throw std::runtime_error("SEARCH_LIMIT: routing time budget exhausted");
            size_t selected=root;
            if(count) {
                double best=variant?-1:inf;
                for(size_t i=0;i<terminals.size();++i) if(!added[i]) {
                    if(pin_rooted && i==0 && count+1<terminals.size()) continue;
                    const double distance=nearest_terminal[i];
                    if((!variant&&distance<best)||(variant&&distance>best)) {best=distance;selected=i;}
                }
            }
            const auto& pin=terminals[selected];auto choices=accesses(pin,low,high);
            if(tree.empty()) {
                size_t chosen=0;for(size_t j=1;j<choices.size();++j)
                    if(access_cost(choices[j],&result)<access_cost(choices[chosen],&result)) chosen=j;
                result.accesses.push_back(choices[chosen]);tree.insert(choices[chosen].node);result.nodes.insert(choices[chosen].node);
            } else {
                const auto path=search(choices,tree,result,low,high,options_.dijkstra);
                if(path.status!=PathResult::Status::FOUND)
                    throw std::runtime_error(path.status==PathResult::Status::SEARCH_LIMIT?
                        "SEARCH_LIMIT: A* expansion/time budget":"UNREACHABLE: full die search exhausted");
                result.accesses.push_back(choices[path.source]);
                for(size_t i=0;i<path.nodes.size();++i) {
                    tree.insert(path.nodes[i]);result.nodes.insert(path.nodes[i]);
                    if(i) result.edges.insert(route_edge(path.nodes[i-1],path.nodes[i]));
                }
            }
            added[selected]=true;
            for(size_t i=0;i<terminals.size();++i) if(!added[i])
                nearest_terminal[i]=std::min(nearest_terminal[i],terminals[i].coord.distance_to(terminals[selected].coord)/graph_.ell0()+
                    0.2*std::abs(terminals[i].layer_idx-terminals[selected].layer_idx));
        }
    }
    return result;
}
NetRoute NegotiatedRouter::build_candidate(size_t index,int slot,int variant) {
    auto result=build_tree(index,slot,variant);encode(index,result);return result;
}
// Partial initial routes have no TSV yet. Candidate W/C ranking does not
// require thermal scoring, which is undefined before the first crossing net.
CompleteMetrics NegotiatedRouter::official_candidate_base() const {
    CompleteMetrics base;base.official_model=true;base.score_20261020=options_.official_metrics.score_20261020;
    if(official_overflow_>uint64_t(std::numeric_limits<int>::max()))
        throw std::logic_error("official aggregate overflow exceeds metric range");
    base.guide.wirelength=double(official_wirelength_);
    base.guide.reference=double(official_reference_);
    base.guide.overflow=int(official_overflow_);
    base.official.squared_normalized_overflow=double(official_squared_overflow_);
    base.official.zero_capacity_cells=official_zero_capacity_cells_;
    return base;
}
double NegotiatedRouter::official_reroute_priority(int node,int owned_demand) const {
    if(owned_demand<=0 || owned_demand>demand_.at(size_t(node)))
        throw std::logic_error("invalid official reroute owner demand");
    if(graph_.official_capacity(node)<=0) {
        // A shared zero-capacity cell has a flat occupied-cell penalty: removing
        // one owner alone gives no marginal relief. Give every movable owner a
        // share of that penalty so a joint transaction can clear the cell.
        // Fixed OBS/PIN occupancy makes such a cell impossible to clear.
        if(graph_.official_obstacle_demand(node)>0) return 0;
        return 10000.0*owned_demand/demand_[node];
    }
    const auto before=official_cell_contribution(graph_,node,demand_[node]);
    const auto after=official_cell_contribution(graph_,node,demand_[node]-owned_demand);
    return double(before.squared-after.squared);
}
std::vector<std::pair<int,int>> NegotiatedRouter::official_protected_demand(size_t index,const NetRoute& route) const {
    // This is a ranking filter, never a subtraction from the real score ledger.
    // Retain terminal/interconnect resources that a fixed-slot reroute cannot
    // usefully clear. Explicit PinAccess geometry can contain a whole route.
    std::map<int,int> protected_cells;
    const auto& grid=graph_.grid();
    auto cell=[&](int node) {if(node>=0) protected_cells[node]=1;};
    auto region=[&](const GuideRect& g) {
        if(g.x1>=g.x2 || g.y1>=g.y2) return;
        const int c0=grid.get_col(g.x1),c1=grid.get_col(std::nextafter(g.x2,-inf)),
                  r0=grid.get_row(g.y1),r1=grid.get_row(std::nextafter(g.y2,-inf));
        if(c0<0 || c1<0 || r0<0 || r1<0) return;
        for(int r=r0;r<=r1;++r) for(int c=c0;c<=c1;++c) cell(graph_.node(c,r,get_layer_index(g.layer)));
    };
    const auto& net=def_.nets().at(index);
    for(const auto& pin:net.pins) {
        const int c=grid.get_col(pin.coord.x),r=grid.get_row(pin.coord.y);
        if(c<0 || r<0) continue;
        const auto box=grid.get_cell_rect(c,r);
        // A Pin exactly on a grid line may be covered from either side.
        if(pin.coord.x>box.x1 && pin.coord.x<box.x2 && pin.coord.y>box.y1 && pin.coord.y<box.y2)
            cell(graph_.node(c,r,pin.layer_idx));
    }
    if(!route.explicit_geometry && (route.slot>=0 || net.pins.size()!=2 || !options_.pin_local))
        for(const auto& access:route.accesses) {
            cell(access.node);
            for(const auto& g:access.guides) region(g);
        }
    if(route.slot>=0) {
        const auto& slot=policy_.slots().at(size_t(route.slot));
        region(guide(slot.lower_pad,10,def_.die_area()));
        region(guide(slot.upper_pad,11,def_.die_area()));
        const int c=grid.get_col(slot.point.x),r=grid.get_row(slot.point.y);
        for(int layer:{10,11}) protected_cells[graph_.node(c,r,layer)]+=options_.official_metrics.tsv_congestion_ratio;
    }
    std::vector<std::pair<int,int>> result;
    for(auto [node,amount]:protected_cells) {
        const auto found=std::lower_bound(route.measured.demand.begin(),route.measured.demand.end(),node,
            [](const auto& entry,int n){return entry.first<n;});
        if(found!=route.measured.demand.end() && found->first==node)
            result.emplace_back(node,std::min(amount,found->second));
    }
    return result;
}
bool NegotiatedRouter::official_zero_capacity_blocked(int node) const {
    if(graph_.official_obstacle_demand(node)>0) return true;
    for(int owner:cell_owners(node)) {
        const auto required=official_protected_demand(size_t(owner),routes_[owner]);
        const auto entry=std::lower_bound(required.begin(),required.end(),node,
            [](const auto& p,int n){return p.first<n;});
        if(entry!=required.end() && entry->first==node && entry->second>0) return true;
    }
    return false;
}
double NegotiatedRouter::official_movable_priority(size_t index) const {
    const auto& route=routes_.at(index);
    const auto protected_cells=official_protected_demand(index,route);
    double result=0;
    for(auto [node,amount]:route.measured.demand) {
        const auto fixed=std::lower_bound(protected_cells.begin(),protected_cells.end(),node,
            [](const auto& entry,int n){return entry.first<n;});
        const int movable=amount-(fixed!=protected_cells.end() && fixed->first==node?fixed->second:0);
        if(movable<=0) continue;
        if(graph_.official_capacity(node)<=0) {
            // A zero-capacity occupied-cell penalty can only disappear when
            // every owner is movable. A protected owner makes it unavoidable.
            if(official_zero_capacity_blocked(node)) continue;
        }
        result+=official_reroute_priority(node,movable);
    }
    return result;
}
void NegotiatedRouter::complete_zero_capacity_scope(std::vector<size_t>& scope) const {
    const size_t limit=v7_scope_limit_?v7_scope_limit_:(options_.rrr_score_aware && large_design()?options_.rrr_net_limit:128);
    if(!options_.official_model || scope.size()>=limit) return;
    const auto seeds=scope;
    std::set<size_t> included(scope.begin(),scope.end());
    for(size_t seed:seeds) for(auto [node,amount]:routes_[seed].measured.demand) {
        (void)amount;
        if(graph_.official_capacity(node)>0 || graph_.official_obstacle_demand(node)>0) continue;
        if(options_.rrr_score_aware && official_zero_capacity_blocked(node)) continue;
        for(int owner:cell_owners(node)) if(included.insert(size_t(owner)).second) {
            scope.push_back(size_t(owner));
            if(scope.size()>=limit) return;
        }
    }
}
// Rank alternatives against the current other-net ledger. Fixed-TSV
// alternatives have identical thermal contribution, so only W/C can differ.
std::tuple<size_t,double,long long,double,int> NegotiatedRouter::official_candidate_key(
        const NetRoute& r,const CompleteMetrics& base) const {
    double squared=base.official.squared_normalized_overflow;
    size_t zeros=base.official.zero_capacity_cells;
    long long overflow=base.guide.overflow;
    for(auto [n,m]:r.measured.demand) {
        const double cap=graph_.official_capacity(n),u=graph_.official_obstacle_demand(n)+demand_[n];
        const double old_over=std::max(0.0,u-cap),new_over=std::max(0.0,u+m-cap);
        overflow+=static_cast<long long>(new_over-old_over);
        if(cap>0) squared+=0.01*(new_over*new_over-old_over*old_over)/(cap*cap);
        else if(u<=0 && u+m>0) {++zeros;squared+=10000.0;}
    }
    double ref=base.guide.reference+r.measured.reference;
    double length=base.guide.wirelength+r.measured.wirelength;
    // Reserve the reference cost of all unaccounted nets, including a second
    // temporarily removed net in a TSV swap, until the transaction is complete.
    if(options_.official_metrics.score_20261020) {
        length+=std::max(0.0,double(full_reference_)-ref);ref=double(full_reference_);
    }
    const double w=official_wirelength_score(length,ref,options_.official_metrics)/100;
    const double quality=0.45*w+0.30/std::sqrt(1.0+std::max(0.0,squared));
    return {options_.official_metrics.score_20261020?0:zeros,-quality,overflow,r.length,r.vias};
}
bool NegotiatedRouter::v5_topology_better(const NetRoute& trial,const NetRoute& baseline,const CompleteMetrics& base) const {
    const auto trial_key=official_candidate_key(trial,base),baseline_key=official_candidate_key(baseline,base);
    // The legacy key puts zero-capacity cells before quality. Preserve that
    // safeguard, but do not buy fewer bad cells with a lower real W/C score.
    return trial_key<baseline_key && std::get<1>(trial_key)<=std::get<1>(baseline_key);
}
NetRoute NegotiatedRouter::build_route(size_t index,int slot) {
    std::optional<NetRoute> best;std::string failure;
    std::optional<CompleteMetrics> official_base;
    auto candidate_better=[&](const NetRoute& a,const NetRoute& b) {
        if(!official_base) official_base=official_candidate_base();
        return official_candidate_key(a,*official_base)<official_candidate_key(b,*official_base);
    };
    auto local_key=[&](const NetRoute& r) {
        long long overflow=0,physical=0;
        for(auto [n,m]:r.measured.demand)
            overflow+=std::max(0,demand_[n]+m-int(graph_.guide_capacity(n)))-std::max(0,demand_[n]-int(graph_.guide_capacity(n)));
        for(auto e:r.edges) {auto [a,b]=edge_nodes(e);double cap=graph_.edge_capacity(a,b);if(cap<0) continue;
            auto it=usage_.find(e);int u=it==usage_.end()?0:it->second;
            physical+=std::max(0,u+1-int(cap))-std::max(0,u-int(cap));}
        // During a replacement pass rank against the unchanged other nets:
        // sum(W)/sum(R) is not the same objective as this net's W/R.
        const double reference=external_reference_+r.measured.reference;
        const double norm=reference>0?(external_wirelength_+r.measured.wirelength)/reference:100;
        return std::tuple(overflow,physical,norm,r.length,r.vias);
    };
    for(int variant=0;variant<(initial_build_&&large_design()&&options_.fast_initial?1:2);++variant) try {
        auto trial=build_candidate(index,slot,variant);
        if(!best || (options_.official_model?candidate_better(trial,*best):local_key(trial)<local_key(*best))) best=std::move(trial);
    } catch(const std::exception& e) {failure=e.what();}
    const auto& net=def_.nets()[index];
    const bool has_shared_side=net.pins.size()>2;
    if(best && options_.v5_pin_rooted && options_.official_model && initial_build_ && large_design() &&
       slot>=0 && net.is_cross_die && has_shared_side && net.pins.size()<=options_.v5_topology_max_pins && !timed_out()) {
      const double remaining=v5_topology_seconds_limit()-counters_.v5_topology_seconds;
      if(remaining<=0) ++counters_.v5_topology_budget_skips;
      else {
        ScopedTopologyBudget budget(deadline_,v5_topology_trial_,counters_.v5_topology_seconds,remaining);
        ++counters_.v5_topology_attempted;
        try {
            // Construction and comparison are read-only with respect to all
            // ownership and aggregate-score ledgers. Exceptions retain best.
            auto trial=build_tree(index,slot,0,{{-1,-1}},-1,true);encode(index,trial);
            ++counters_.v5_topology_candidates;
            bool same_tsv=trial.measured.tsv_cells==best->measured.tsv_cells &&
                trial.measured.tsv_points.size()==best->measured.tsv_points.size();
            if(same_tsv) for(size_t k=0;k<trial.measured.tsv_points.size();++k)
                if(trial.measured.tsv_points[k].distance_to(best->measured.tsv_points[k])!=0) {same_tsv=false;break;}
            if(!same_tsv) ++counters_.v5_topology_thermal_rejections;
            else {
              if(!official_base) official_base=official_candidate_base();
              if(v5_topology_better(trial,*best,*official_base)) {
                // encode() verifies mapping, pin coverage and obstruction;
                // check full connectivity before admitting the alternative.
                std::string error;
                if(!validate_route(index,trial.guides,error))
                    throw std::runtime_error("v5 pin-rooted invalid Guide: "+error);
                check_v5_topology_budget();
                best=std::move(trial);++counters_.v5_topology_accepted;
              }
            }
        } catch(const std::exception&) {
            if(timed_out()) ++counters_.v5_topology_interruptions;
            else ++counters_.v5_topology_failures;
        }
      }
    }
    // Compare a shared pin-rooted tree with the TSV-rooted incumbent in both
    // initial routing and later replacement passes. Final encoded W/C, with
    // the same TSV occupancy, decides acceptance; the proxy only builds it.
    if(best && options_.v7_topology && options_.official_model && slot>=0 &&
       net.is_cross_die && has_shared_side && net.pins.size()<=32 && !timed_out()) {
        const double remaining=std::min(120.0,0.05*options_.time_limit)-counters_.v7_topology_seconds;
        if(remaining>0) {
            ScopedTopologyBudget budget(deadline_,v5_topology_trial_,counters_.v7_topology_seconds,remaining);
            ++counters_.v7_topology_attempted;
            try {
                auto trial=build_tree(index,slot,0,{{-1,-1}},-1,true);encode(index,trial);
                ++counters_.v7_topology_candidates;
                if(trial.measured.tsv_cells!=best->measured.tsv_cells)
                    throw std::logic_error("V7 topology changed fixed TSV occupancy");
                if(candidate_better(trial,*best)) {
                    std::string error;
                    if(!validate_route(index,trial.guides,error))
                        throw std::runtime_error("V7 shared tree invalid Guide: "+error);
                    check_v5_topology_budget();best=std::move(trial);++counters_.v7_topology_accepted;
                }
            } catch(const std::exception&) {++counters_.v7_topology_failures;}
        }
    }
    if(best && options_.pin_local && slot<0 && !timed_out() && !(initial_build_&&large_design())) {
        for(auto& trial:pin_local_candidates(index)) {
            // A useful direct access must reduce real routing work as well as
            // the Guide proxy; never win solely by growing its HPWL denominator.
            if(!best || (options_.official_model?candidate_better(trial,*best):(trial.length<=best->length+1e-6 &&
                         trial.measured.wirelength<=best->measured.wirelength+1e-6 &&
                         (trial.length<best->length-1e-6 ||
                          trial.measured.wirelength<best->measured.wirelength-1e-6 || trial.vias<best->vias) &&
                         local_key(trial)<local_key(*best)))) best=std::move(trial);
        }
    }
    if(!best) throw std::runtime_error(def_.nets()[index].name+": "+failure);
    return std::move(*best);
}
void NegotiatedRouter::account(size_t index,const NetRoute& r,int sign) {
    if((sign!=1&&sign!=-1)||index>size_t(std::numeric_limits<int>::max()))
        throw std::logic_error("invalid resource ownership transaction");
    const int owner_id=int(index);
    // Validate ownership before changing either ledger. Duplicate account or
    // missing ripup cannot leave a partially updated demand state.
    for(auto e:r.edges) {
        const auto found=edge_nets_.find(e);
        const bool owns=found!=edge_nets_.end()&&std::binary_search(found->second.begin(),found->second.end(),owner_id);
        if(owns!=(sign<0)) throw std::logic_error(sign>0?"duplicate physical owner during account":"missing physical owner during ripup");
        const auto usage=usage_.find(e);
        if(sign<0&&(usage==usage_.end()||usage->second<=0)) throw std::logic_error("negative physical demand");
        if(sign>0&&usage!=usage_.end()&&usage->second==std::numeric_limits<int>::max())
            throw std::logic_error("physical demand overflow");
    }
    for(auto [n,m]:r.measured.demand) {
        if(n<0||size_t(n)>=demand_.size()||m<=0) throw std::logic_error("invalid Guide resource demand");
        const auto found=cell_nets_.find(n);
        const bool owns=found!=cell_nets_.end()&&std::binary_search(found->second.begin(),found->second.end(),owner_id);
        if(owns!=(sign<0)) throw std::logic_error(sign>0?"duplicate Guide owner during account":"missing Guide owner during ripup");
        if(sign<0&&demand_[n]<m) throw std::logic_error("negative guide demand");
        if(sign>0&&demand_[n]>std::numeric_limits<int>::max()-m) throw std::logic_error("guide demand overflow");
    }
    // Compute every official delta after ownership validation and before either
    // resource ledger changes. A rejected account leaves the score cache intact.
    long double next_wirelength=official_wirelength_,next_reference=official_reference_,next_squared=official_squared_overflow_;
    uint64_t next_overflow=official_overflow_;
    size_t next_zeros=official_zero_capacity_cells_;
    if(options_.official_model) {
        if(!std::isfinite(r.measured.wirelength)||r.measured.wirelength<0 ||
           !std::isfinite(r.measured.reference)||r.measured.reference<0)
            throw std::logic_error("invalid cached official net totals");
        next_wirelength+=sign*static_cast<long double>(r.measured.wirelength);
        next_reference+=sign*static_cast<long double>(r.measured.reference);
        for(auto [n,m]:r.measured.demand) {
            const auto old=official_cell_contribution(graph_,n,demand_[n]);
            const auto updated=official_cell_contribution(graph_,n,demand_[n]+sign*m);
            if(next_overflow<old.overflow || (old.zero_capacity && !next_zeros))
                throw std::logic_error("official score ledger mismatch");
            next_overflow=next_overflow-old.overflow+updated.overflow;
            next_squared+=updated.squared-old.squared;
            next_zeros=next_zeros-size_t(old.zero_capacity)+size_t(updated.zero_capacity);
        }
    }
    auto insert_owner=[&](auto& owners) {
        owners.insert(std::lower_bound(owners.begin(),owners.end(),owner_id),owner_id);
    };
    auto erase_owner=[&](auto& map,auto resource) {
        auto found=map.find(resource);
        auto& owners=found->second;
        owners.erase(std::lower_bound(owners.begin(),owners.end(),owner_id));
        if(owners.empty()) map.erase(found);
    };
    for(auto e:r.edges) {
        usage_[e]+=sign;
        if(sign>0) insert_owner(edge_nets_[e]);else erase_owner(edge_nets_,e);
    }
    for(auto [n,m]:r.measured.demand) {
        demand_[n]+=sign*m;
        if(sign>0) insert_owner(cell_nets_[n]);else erase_owner(cell_nets_,n);
    }
    if(options_.official_model) {
        official_wirelength_=next_wirelength;official_reference_=next_reference;
        official_squared_overflow_=next_squared;official_overflow_=next_overflow;
        official_zero_capacity_cells_=next_zeros;
    }
    ++state_generation_;
    complete_validation_cached_=false;
}
CompleteMetrics NegotiatedRouter::measure() const {
    CompleteMetrics result;GuideMap guides;std::vector<const NetGuideMetrics*> cached;
    for(size_t i=0;i<routes_.size();++i) {
        if(large_design() || options_.official_model) cached.push_back(&routes_[i].measured);
        else guides[def_.nets()[i].name]=routes_[i].guides;
        result.physical_length+=routes_[i].length;
        result.vias+=routes_[i].vias;result.physical_tsvs+=routes_[i].slot>=0;
    }
    result.official_model=options_.official_model;
    result.score_20261020=options_.official_metrics.score_20261020;
    if(options_.official_model) {
        result.official=aggregate_official_metrics(cached,demand_,graph_,thermal_,options_.official_metrics,false);
        result.guide=result.official.guide;
    } else {
        result.guide=large_design()?aggregate_cached_metrics(cached,demand_,graph_,thermal_,false):evaluate_guides(guides,graph_,lef_,thermal_);
        if(!large_design() && result.guide.demand!=demand_) throw std::logic_error("guide ledger disagrees with exact recomputation");
    }
    for(auto [e,u]:usage_) {auto [a,b]=edge_nodes(e);const double cap=graph_.edge_capacity(a,b);
        if(cap>=0) result.physical_overflow+=std::max(0,u-int(cap));}
    return result;
}
bool NegotiatedRouter::better(const CompleteMetrics& a,const CompleteMetrics& b) {
    if(a.official_model) {
        if(!a.score_20261020 && a.official.zero_capacity_cells!=b.official.zero_capacity_cells) return a.official.zero_capacity_cells<b.official.zero_capacity_cells;
        if(std::abs(a.guide.quality-b.guide.quality)>1e-10) return a.guide.quality>b.guide.quality;
        if(a.guide.overflow!=b.guide.overflow) return a.guide.overflow<b.guide.overflow;
        return a.physical_length<b.physical_length;
    }
    if(a.guide.overflow!=b.guide.overflow) return a.guide.overflow<b.guide.overflow;
    if(a.physical_overflow!=b.physical_overflow) return a.physical_overflow<b.physical_overflow;
    if(std::abs(a.guide.quality-b.guide.quality)>1e-6) return a.guide.quality>b.guide.quality;
    if(std::abs(a.physical_length-b.physical_length)>1e-6) return a.physical_length<b.physical_length;
    return a.vias<b.vias;
}
bool NegotiatedRouter::repair_forest(size_t index,int slot,NetRoute& trial) {
    // Keep pin access geometry and unaffected graph branches. A TSV relocation
    // with different terminal accesses uses the full-net fallback below.
    if(slot!=routes_[index].slot || routes_[index].explicit_geometry) return false;
    trial=routes_[index];
    std::set<int> terminals;
    for(const auto& a:trial.accesses) terminals.insert(a.node);
    std::vector<EdgeKey> cuts;
    for(auto e:trial.edges) {
        auto [a,b]=edge_nodes(e);
        if(demand_[a]>graph_.guide_capacity(a)||demand_[b]>graph_.guide_capacity(b)||
           guide_history_[a]>0||guide_history_[b]>0) cuts.push_back(e);
    }
    if(cuts.empty()) return false;
    for(auto e:cuts) trial.edges.erase(e);
    // Delete only dangling branches without a required access terminal.
    bool pruned=true;
    while(pruned) {
        pruned=false;std::map<int,std::vector<EdgeKey>> adjacency;
        for(auto e:trial.edges) {auto [a,b]=edge_nodes(e);adjacency[a].push_back(e);adjacency[b].push_back(e);}
        for(const auto& [n,es]:adjacency) if(es.size()==1&&!terminals.count(n)) {trial.edges.erase(es[0]);pruned=true;}
    }
    trial.nodes=terminals;
    for(auto e:trial.edges) {auto [a,b]=edge_nodes(e);trial.nodes.insert(a);trial.nodes.insert(b);}
    for(int side=0;side<2;++side) {
        std::set<int> remaining;for(int n:trial.nodes) if((graph_.layer(n)>10)==bool(side)) remaining.insert(n);
        if(remaining.empty()) continue;
        auto component=[&](int start) {
            std::map<int,std::vector<int>> adjacency;
            for(auto e:trial.edges) {auto [a,b]=edge_nodes(e);adjacency[a].push_back(b);adjacency[b].push_back(a);}
            std::set<int> reached{start};std::vector<int> todo{start};
            while(!todo.empty()) {int n=todo.back();todo.pop_back();for(int next:adjacency[n])
                if(reached.insert(next).second) todo.push_back(next);}
            return reached;
        };
        std::set<int> connected=component(*remaining.begin());
        for(int n:connected) remaining.erase(n);
        while(!remaining.empty()) {
            std::vector<PinAccess> sources;for(int n:connected) {PinAccess a;a.node=n;sources.push_back(a);}
            auto path=search(sources,remaining,trial,side?11:1,side?20:(slot>=0?9:10),options_.dijkstra);
            if(path.status!=PathResult::Status::FOUND) return false;
            for(size_t k=0;k<path.nodes.size();++k) {
                trial.nodes.insert(path.nodes[k]);if(k) trial.edges.insert(route_edge(path.nodes[k-1],path.nodes[k]));
            }
            connected=component(*connected.begin());for(int n:connected) remaining.erase(n);
        }
    }
    encode(index,trial);++counters_.local_repairs;return true;
}
bool NegotiatedRouter::rrr_round(const std::vector<size_t>& scope) {
    ++counters_.rrr_attempted;
    const bool incremental=options_.v7_rrr && options_.official_model;
    const auto before=incremental?recovery_metrics(best_metrics_):measure();
    std::vector<std::pair<size_t,NetRoute>> saved;
    // Allocate and copy the rollback journal before touching a net's ledgers.
    // A journal allocation failure after account(+1) must not double-account
    // the old route or lose the only complete checkpoint.
    saved.reserve(scope.size());
    size_t maze_nets=0;
    auto rollback=[&] {
        v7_refinement_active_=false;
        for(auto& [i,old]:saved) {account(i,routes_[i],-1);routes_[i]=std::move(old);account(i,routes_[i],1);}
        saved.clear();best_metrics_=before;
        ++counters_.rollbacks;
        if(incremental) ++counters_.v7_checkpoint_restores;
    };
    try {
        for(size_t i:scope) {
            if(timed_out()) {rollback();return false;}
            const bool maze=incremental && maze_nets<options_.v7_rrr_astar_net_limit &&
                options_.v7_rrr_astar_expansions>0 && options_.v7_rrr_astar_seconds>0 && official_movable_priority(i)>0;
            v7_refinement_active_=maze;
            if(maze) {
                ++maze_nets;++counters_.v7_astar_nets;
                v7_refinement_expansion_start_=counters_.expansions;
                v7_refinement_deadline_=std::min(deadline_,std::chrono::steady_clock::now()+
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(options_.v7_rrr_astar_seconds)));
            }
            if(incremental) ++counters_.v7_rrr_nets;
            saved.emplace_back(i,routes_.at(i));
            const NetRoute& old=saved.back().second;
            bool removed=false,inserted=false;
            try {
                account(i,old,-1);removed=true;
                NetRoute trial;
                routes_[i]=NetRoute{};
                bool repaired=false;
                if(options_.local_repair) {
                    routes_[i]=old;
                    try {repaired=repair_forest(i,old.slot,trial);} catch(const std::exception&) {}
                    routes_[i]=NetRoute{};
                }
                auto full=build_route(i,old.slot);
                auto candidate_key=[&](const NetRoute& r) {
                    long long overflow=0;
                    for(auto [n,m]:r.measured.demand) overflow+=std::max(0,demand_[n]+m-int(graph_.guide_capacity(n)))-std::max(0,demand_[n]-int(graph_.guide_capacity(n)));
                    return std::tuple(overflow,r.measured.wirelength/std::max(1.0,r.measured.reference),r.length,r.vias);
                };
                if(!repaired) trial=std::move(full);
                else if(options_.official_model) {
                    const auto base=official_candidate_base();
                    if(official_candidate_key(full,base)<official_candidate_key(trial,base)) trial=std::move(full);
                } else if(candidate_key(full)<candidate_key(trial)) trial=std::move(full);
                // Keep group negotiation: an individual move may lose score.
                // Fixed-slot RRR must preserve T before using the W/C ledger.
                if(incremental && trial.measured.tsv_cells!=old.measured.tsv_cells)
                    throw std::logic_error("V7 fixed-slot RRR changed thermal TSV cells");
                routes_[i]=std::move(trial);account(i,routes_[i],1);inserted=true;
            } catch(const std::exception& e) {
                if(inserted) account(i,routes_[i],-1);
                if(removed) {routes_[i]=std::move(saved.back().second);account(i,routes_[i],1);}
                saved.pop_back();
                ++rejection_reasons_[e.what()];
            }
            v7_refinement_active_=false;
        }
        auto after=incremental?recovery_metrics(before):measure();
        if(incremental) for(const auto& [i,old]:saved) {
            after.physical_length+=routes_[i].length-old.length;
            after.vias=after.vias-old.vias+routes_[i].vias;
        }
        if(!incremental || better(after,before)) {
            const auto started=std::chrono::steady_clock::now();
            validate_complete();after=measure();
            if(incremental) {
                verify_official_ledger(after);
                counters_.v7_validation_seconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
            }
            if(better(after,before)) {best_metrics_=after;++counters_.rrr_accepted;return true;}
        }
        ++rejection_reasons_["RRR round did not improve the complete comparison key"];
        rollback();return false;
    } catch(...) {rollback();throw;}
}
void NegotiatedRouter::run_rrr(int rounds) {
    const auto started=std::chrono::steady_clock::now();
    int stale=0,polish=0;
    const bool v7=options_.v7_rrr && options_.official_model;
    const bool score_aware=options_.official_model && options_.rrr_score_aware;
    for(int round=0;round<rounds&&!timed_out();++round) {
        std::vector<double> priority(routes_.size());bool overflow=false;
        for(size_t n=0;n<demand_.size();++n) {
            const double cap=graph_.guide_capacity(int(n)),o=std::max(0.0,demand_[n]-cap);
            const double normalization=options_.official_model&&(!large_design() || score_aware)?graph_.official_capacity(int(n)):cap;
            guide_history_[n]+=o/std::max(1.0,normalization);
            if(o<=0) continue;
            if(score_aware) continue;
            const auto& owners=cell_owners(int(n));
            if(!options_.official_model) overflow=true;
            for(int i:owners) {
                const auto& cells=routes_[i].measured.demand;
                const auto owned=std::lower_bound(cells.begin(),cells.end(),int(n),
                    [](const auto& entry,int node){return entry.first<node;});
                if(owned==cells.end()||owned->first!=int(n)) continue;
                if(options_.official_model) {
                    // Retain the large-design congestion schedule validated by full official runs.
                    // Small designs use exact marginal scores; zero-capacity groups always need joint priority.
                    const double relief=large_design()&&graph_.official_capacity(int(n))>0
                        ?o*owned->second:official_reroute_priority(int(n),owned->second);
                    if(relief>0) {priority[i]+=relief;overflow=true;}
                } else priority[i]+=o*owned->second;
            }
        }
        if(score_aware) for(size_t i=0;i<routes_.size();++i) {
            priority[i]=official_movable_priority(i);
            if(priority[i]>0) overflow=true;
        }
        if(!options_.official_model||(!score_aware && large_design())) for(auto [e,u]:usage_) {
            auto [a,b]=edge_nodes(e);double cap=graph_.edge_capacity(a,b);if(cap<0) continue;
            const double o=std::max(0.0,u-cap);edge_history_[e]+=o/std::max(1.0,cap);
            if(o>0) {overflow=true;for(int i:edge_nets_.at(e)) priority[i]+=o;}
        }
        if(!overflow&&polish++>=2) break;
        std::vector<size_t> scope;
        if(overflow) {
            for(size_t i=0;i<routes_.size();++i) if(priority[i]>0) scope.push_back(i);
        } else {
            for(size_t i=0;i<routes_.size();++i) {
                const auto& pins=def_.nets()[i].pins;
                // Prim terminal MST is a ranking estimate, not a Steiner lower bound.
                std::vector<double> d(pins.size(),inf);std::vector<bool> used(pins.size());double mst=0;
                if(!d.empty()) d[0]=0;
                for(size_t k=0;k<pins.size();++k) {
                    size_t n=pins.size();for(size_t j=0;j<pins.size();++j) if(!used[j]&&(n==pins.size()||d[j]<d[n])) n=j;
                    used[n]=true;mst+=d[n];for(size_t j=0;j<pins.size();++j) if(!used[j])
                        d[j]=std::min(d[j],pins[n].coord.distance_to(pins[j].coord));
                }
                priority[i]=routes_[i].length/std::max(mst,graph_.ell0());scope.push_back(i);
            }
        }
        std::stable_sort(scope.begin(),scope.end(),[&](size_t a,size_t b){
            if(priority[a]!=priority[b]) return priority[a]>priority[b];
            if(def_.nets()[a].pins.size()!=def_.nets()[b].pins.size()) return def_.nets()[a].pins.size()<def_.nets()[b].pins.size();
            return def_.nets()[a].name<def_.nets()[b].name;
        });
        scope.resize(std::min(scope.size(),std::max<size_t>(1,size_t(std::ceil(routes_.size()*(overflow?0.2:0.05))))));
        size_t limit=score_aware?options_.rrr_net_limit:128;
        if(v7) {
            const size_t ceiling=std::max<size_t>(1,options_.v7_rrr_net_limit);
            const size_t growth=size_t(std::min(round+1,8));
            limit=std::min(ceiling,std::max<size_t>(1,limit)*growth);
        }
        if((large_design() || v7) && scope.size()>limit) scope.resize(limit);
        v7_scope_limit_=v7?limit:0;
        complete_zero_capacity_scope(scope);
        if(v7) counters_.v7_rrr_peak_scope=std::max(counters_.v7_rrr_peak_scope,scope.size());
        if(scope.empty()) break;
        bool accepted=rrr_round(scope);
        std::cout<<"[RRR] round="<<round+1<<" accepted="<<accepted<<" guide_overflow="<<best_metrics_.guide.overflow
                 <<" physical_overflow="<<best_metrics_.physical_overflow<<"\n";
        stale=accepted?0:stale+1;if(stale>=3) break;
        if(overflow) present_=std::min(64.0,present_*1.5);
    }
    v7_scope_limit_=0;v7_refinement_active_=false;
    if(v7) {
        const auto validation_started=std::chrono::steady_clock::now();
        validate_complete();best_metrics_=measure();verify_official_ledger(best_metrics_);
        counters_.v7_validation_seconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-validation_started).count();
        counters_.v7_rrr_seconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    }
}
void NegotiatedRouter::optimize_tsv() {
    struct Move {size_t net;int slot;int other;double estimate;};
    int stale=0;
    for(int round=0;round<options_.tsv_rounds&&!timed_out();++round) {
        std::vector<Move> moves;
        std::vector<int> occupied(policy_.slots().size(),-1);
        for(size_t n=0;n<routes_.size();++n) if(routes_[n].slot>=0) occupied[routes_[n].slot]=int(n);
        std::vector<double> heat;std::vector<int> counts(graph_.grid().cols*graph_.grid().rows);
        for(int r=0;r<graph_.grid().rows;++r) for(int c=0;c<graph_.grid().cols;++c)
            heat.push_back(std::max(0.0,thermal_.get_temperature(c,r)-thermal_.ambient_temperature()));
        for(const auto& route:routes_) if(route.slot>=0) {
            const auto& s=policy_.slots()[route.slot];++counts[graph_.grid().get_row(s.point.y)*graph_.grid().cols+graph_.grid().get_col(s.point.x)];
        }
        PearsonState pearson(heat,counts);
        for(size_t n=0;n<routes_.size();++n) if(routes_[n].slot>=0) {
            const int old=routes_[n].slot;const auto& a=policy_.slots()[old];
            for(const auto& b:policy_.slots()) if(b.id!=old) {
                if(!policy_.compatible(def_.nets()[n],b.id)) {++counters_.policy_rejections;++rejection_reasons_["TSV fixed-pin mapping policy"];continue;}
                const int other=occupied[b.id];
                if(other>=0&&!policy_.compatible(def_.nets()[other],old)) {++counters_.policy_rejections;++rejection_reasons_["TSV fixed-pin mapping policy"];continue;}
                auto trial=pearson;
                if(other<0) trial.move(graph_.grid().get_row(a.point.y)*graph_.grid().cols+graph_.grid().get_col(a.point.x),
                                      graph_.grid().get_row(b.point.y)*graph_.grid().cols+graph_.grid().get_col(b.point.x));
                double delta=0;for(const auto& p:def_.nets()[n].pins) delta+=p.coord.distance_to(b.point)-p.coord.distance_to(a.point);
                moves.push_back({n,b.id,other,delta/graph_.ell0()-2*(trial.correlation()-pearson.correlation())});
            }
        }
        std::stable_sort(moves.begin(),moves.end(),[](auto& a,auto& b){return std::tie(a.estimate,a.net,a.slot)<std::tie(b.estimate,b.net,b.slot);});
        if(moves.size()>8) moves.resize(8);
        bool accepted=false;
        for(const auto& move:moves) {
            if(timed_out()) break;
            ++counters_.tsv_trials;
            std::vector<std::pair<size_t,NetRoute>> saved{{move.net,routes_[move.net]}};
            if(move.other>=0) saved.emplace_back(size_t(move.other),routes_[move.other]);
            const auto before=measure();const auto previous_demand=demand_;
            auto rollback=[&] {
                for(auto& [n,old]:saved) {account(n,routes_[n],-1);routes_[n]=std::move(old);account(n,routes_[n],1);}
                ++counters_.rollbacks;
            };
            try {
                // Remove all involved nets before the first replacement, preserving the same external ledger.
                for(const auto& [n,old]:saved) {account(n,old,-1);routes_[n]=NetRoute{};}
                routes_[move.net]=build_route(move.net,move.slot);account(move.net,routes_[move.net],1);
                if(move.other>=0) {routes_[move.other]=build_route(move.other,saved[0].second.slot);account(move.other,routes_[move.other],1);}
                if(options_.local_repair) {
                    std::map<int,int> neighbors;
                    for(size_t c=0;c<demand_.size();++c) {
                        double cap=graph_.guide_capacity(int(c));
                        if(std::max(0.0,demand_[c]-cap)>std::max(0.0,previous_demand[c]-cap))
                            for(int n:cell_owners(int(c))) if(n!=int(move.net)&&n!=move.other) ++neighbors[n];
                    }
                    std::vector<std::pair<int,int>> ranked;for(auto [n,p]:neighbors) ranked.emplace_back(-p,n);std::sort(ranked.begin(),ranked.end());
                    if(ranked.size()>8) ranked.resize(8);
                    for(auto [priority,n]:ranked) {
                        (void)priority;saved.emplace_back(size_t(n),routes_[n]);account(n,routes_[n],-1);
                        try {NetRoute candidate;if(!repair_forest(n,routes_[n].slot,candidate)) candidate=build_route(n,routes_[n].slot);
                            routes_[n]=std::move(candidate);} catch(const std::exception&) {}
                        account(n,routes_[n],1);
                    }
                }
                validate_complete();const auto after=measure();
                if(better(after,before)) {best_metrics_=after;++counters_.tsv_accepted;accepted=true;break;}
                ++rejection_reasons_["TSV trial did not improve the complete comparison key"];
                rollback();
            } catch(const std::exception& e) {++rejection_reasons_[e.what()];rollback();}
        }
        stale=accepted?0:stale+1;if(stale>=2) break;
    }
}
void NegotiatedRouter::optimize_v7_tsv() {
    if(!options_.v7_tsv || options_.tsv_rounds<=0 || timed_out()) return;
    const auto baseline=measure();
    std::map<size_t,NetRoute> journal;
    try {
        {
            ScopedTopologyBudget budget(deadline_,v5_topology_trial_,counters_.v7_tsv_seconds,options_.v7_tsv_seconds);
            std::vector<int> assignment;assignment.reserve(routes_.size());
            for(const auto& route:routes_) assignment.push_back(route.slot);
            TsvMoveSearch proposals(def_,graph_,policy_,thermal_,assignment);
            if(std::abs(proposals.current_correlation()-baseline.guide.correlation)>1e-9)
                throw std::logic_error("V7 TSV proposal occupancy differs from encoded Guide");
            std::vector<size_t> scope;
            for(size_t i=0;i<routes_.size();++i)
                if(routes_[i].slot>=0 && def_.nets()[i].pins.size()<=128) scope.push_back(i);
            std::stable_sort(scope.begin(),scope.end(),[&](size_t a,size_t b) {
                const double x=routes_[a].measured.wirelength-routes_[a].measured.reference;
                const double y=routes_[b].measured.wirelength-routes_[b].measured.reference;
                return x!=y?x>y:def_.nets()[a].name<def_.nets()[b].name;
            });
            if(scope.size()>options_.v7_tsv_nets) scope.resize(options_.v7_tsv_nets);
            for(int pass=0;pass<options_.tsv_rounds && !timed_out();++pass) {
                size_t accepted=0;
                for(size_t i:scope) {
                    if(timed_out()) break;
                    const auto before=recovery_metrics(best_metrics_);
                    TsvMoveSearchOptions settings;
                    settings.max_candidates=options_.v7_tsv_candidates;
                    settings.thermal_gain=options_.official_metrics.thermal_gain;
                    settings.length_price=before.guide.wirelength>before.guide.reference?
                        45.0*before.guide.reference/(before.guide.wirelength*before.guide.wirelength):0.0;
                    const auto candidates=proposals.candidates(i,settings);
                    counters_.v7_tsv_candidates+=candidates.size();
                    for(const auto& candidate:candidates) {
                        if(timed_out()) break;
                        ++counters_.v7_tsv_attempted;
                        NetRoute old=routes_[i];
                        bool removed=false,inserted=false;
                        auto restore=[&] {
                            if(inserted) account(i,routes_[i],-1);
                            if(removed) {routes_[i]=std::move(old);account(i,routes_[i],1);}
                            removed=inserted=false;
                            ++counters_.v7_tsv_rollbacks;
                        };
                        try {
                            account(i,old,-1);removed=true;routes_[i]=NetRoute{};
                            auto trial=build_route(i,candidate.slot);
                            if(trial.measured.tsv_cells.size()!=1)
                                throw std::logic_error("V7 TSV trial must have one encoded landing");
                            routes_[i]=std::move(trial);account(i,routes_[i],1);inserted=true;
                            auto after=recovery_metrics(before);
                            after.physical_length+=routes_[i].length-old.length;
                            after.vias=after.vias-old.vias+routes_[i].vias;
                            after.guide.correlation=candidate.correlation_after;
                            after.official.thermal_score=50.0*(1+std::tanh(options_.official_metrics.thermal_gain*after.guide.correlation));
                            after.guide.quality=(.45*after.official.wirelength_score+.30*after.official.congestion_score+.25*after.official.thermal_score)/100.0;
                            after.official.guide=after.guide;
                            if(!timed_out() && better(after,before)) {
                                // Save the stage's original only once, before its first accepted move.
                                journal.try_emplace(i,old);
                                if(!proposals.commit_move(i,candidate.slot))
                                    throw std::logic_error("V7 TSV occupancy commit rejected");
                                removed=inserted=false;best_metrics_=after;
                                ++accepted;++counters_.v7_tsv_accepted;break;
                            }
                            restore();
                        } catch(const std::exception& e) {
                            restore();++counters_.v7_tsv_failures;
                            ++rejection_reasons_[std::string("V7 TSV trial: ")+e.what()];
                        }
                    }
                }
                std::cout<<"[V7 TSV] pass="<<pass+1<<" accepted="<<accepted
                         <<" quality="<<best_metrics_.guide.quality*100<<"\n";
                if(!accepted) break;
            }
            if(timed_out()) ++counters_.v7_tsv_budget_stops;
        }
        validate_complete();const auto exact=measure();verify_official_ledger(exact);
        if(std::abs(exact.guide.correlation-best_metrics_.guide.correlation)>1e-9 ||
           std::abs(exact.guide.quality-best_metrics_.guide.quality)>1e-9 ||
           exact.guide.quality+1e-10<baseline.guide.quality)
            throw std::logic_error("V7 TSV incremental thermal/quality checkpoint mismatch");
        best_metrics_=exact;
    } catch(const std::exception& e) {
        // A proposal/context/verification failure restores the whole phase.
        for(auto& [i,old]:journal) {account(i,routes_[i],-1);routes_[i]=std::move(old);account(i,routes_[i],1);}
        ++counters_.v7_checkpoint_restores;
        ++rejection_reasons_[std::string("V7 TSV phase restored: ")+e.what()];
        validate_complete();best_metrics_=measure();verify_official_ledger(best_metrics_);
        if(std::abs(best_metrics_.guide.quality-baseline.guide.quality)>1e-9)
            throw std::logic_error("V7 TSV rollback did not restore phase baseline");
    }
}

void NegotiatedRouter::validate_complete() const {
    if(complete_validation_cached_ && validated_generation_==state_generation_) {
        ++complete_validation_cache_hits_;
        return;
    }
    ++complete_validations_;
    GuideMap all;std::set<int> occupied;
    std::vector<int> expected_demand(large_design()?graph_.size():0);
    for(size_t i=0;i<routes_.size();++i) {
        const auto& r=routes_[i];std::string error;
        if(options_.official_model && options_.tsv_policy.resource_mode==TsvResourceMode::GLOBAL_GRID &&
           !validate_gcell_encoding(def_.nets()[i],def_.die_area(),graph_.grid(),lef_,r.guides,error))
            throw std::runtime_error("GCELL_ENCODING_VIOLATION: "+error);
        for(auto edge:r.edges) {
            auto found=edge_nets_.find(edge);
            if(found==edge_nets_.end()||!std::binary_search(found->second.begin(),found->second.end(),int(i))) throw std::logic_error("missing reverse edge owner");
        }
        for(auto [node,count]:r.measured.demand) {
            if(large_design()) expected_demand[node]+=count;
            auto owner=cell_nets_.find(node);
            if(owner==cell_nets_.end() || !std::binary_search(owner->second.begin(),owner->second.end(),int(i)))
                throw std::logic_error("missing reverse Guide owner");
        }
        if(!policy_.validate_mapping(def_.nets()[i],r.guides,r.slot,error)) throw std::runtime_error("TSV_MAPPING_UNRESOLVED: "+error);
        if(r.slot>=0) {
            if(!occupied.insert(r.slot).second) throw std::runtime_error("SLOT_CAPACITY: duplicate assignment");
            for(int conflict:policy_.slots()[r.slot].conflicts) if(occupied.count(conflict))
                throw std::runtime_error("SLOT_CAPACITY: geometric conflict");
        }
        all[def_.nets()[i].name]=r.guides;
    }
    for(const auto& [edge,count]:usage_) {
        const auto owners=edge_nets_.find(edge);
        // Ripup retains zero usage keys after their last reverse owner leaves.
        if(owners==edge_nets_.end()?count!=0:count<0 || size_t(count)!=owners->second.size())
            throw std::logic_error("physical ledger rollback mismatch");
    }
    auto valid_owners=[&](const std::vector<int>& owners) {
        if(owners.empty()||std::adjacent_find(owners.begin(),owners.end(),std::greater_equal<int>())!=owners.end())
            throw std::logic_error("reverse ownership is not sorted and unique");
        for(int owner:owners) if(owner<0||size_t(owner)>=routes_.size())
            throw std::logic_error("reverse resource owner is outside route array");
    };
    for(const auto& [edge,owners]:edge_nets_) {
        valid_owners(owners);
        // Forward membership and this sorted/unique reverse membership prove
        // owners are exactly the routes using this edge. Their cardinality is
        // the independently verified expected usage; no temporary hash is needed.
        auto [a,b]=edge_nodes(edge);
        if(!graph_.legal_edge(a,b)) throw std::logic_error("illegal retained resource edge");
        const auto used=usage_.find(edge);
        if(used==usage_.end() || used->second<0 || size_t(used->second)!=owners.size())
            throw std::logic_error("physical ledger ownership mismatch");
        for(int owner:owners) if(!routes_[owner].edges.count(edge))
            throw std::logic_error("extraneous reverse edge owner");
    }
    for(const auto& [node,owners]:cell_nets_) {
        valid_owners(owners);
        for(int owner:owners) {
            const auto& measured=routes_[owner].measured.demand;
            const auto found=std::lower_bound(measured.begin(),measured.end(),node,
                [](const auto& entry,int resource){return entry.first<resource;});
            if(found==measured.end()||found->first!=node)
                throw std::logic_error("extraneous reverse Guide owner");
        }
    }
    if(large_design() && expected_demand!=demand_) throw std::logic_error("Guide ledger rollback mismatch");
    if(options_.official_metrics.score_20261020) {
        const auto checked=compat::validate(def_,lef_,all);
        if(checked.error_count) throw std::runtime_error("HARD_RULE_VIOLATION: "+checked.errors.front());
    } else {
        std::string error;if(!validate_guides(def_,lef_,all,error)) throw std::runtime_error("HARD_RULE_VIOLATION: "+error);
    }
    validated_generation_=state_generation_;
    complete_validation_cached_=true;
}
bool NegotiatedRouter::route_all() {
    try {
        const auto assignment=policy_.assign(def_.nets(),thermal_);routes_.resize(def_.nets().size());
        std::vector<size_t> order(routes_.size());std::iota(order.begin(),order.end(),0);
        auto hpwl=[](const Net& n) {Rect b(n.pins[0].coord.x,n.pins[0].coord.y,n.pins[0].coord.x,n.pins[0].coord.y);
            for(const auto& p:n.pins) b=Rect(std::min(b.x1,p.coord.x),std::min(b.y1,p.coord.y),std::max(b.x2,p.coord.x),std::max(b.y2,p.coord.y));
            return b.width()+b.height();};
        std::vector<double> spans;for(const auto& n:def_.nets()) spans.push_back(hpwl(n));
        std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b) {
            const auto& x=def_.nets()[a];const auto& y=def_.nets()[b];
            if(x.is_cross_die!=y.is_cross_die) return x.is_cross_die>y.is_cross_die;
            if(x.pins.size()!=y.pins.size()) return x.pins.size()>y.pins.size();
            if(spans[a]!=spans[b]) return spans[a]>spans[b];
            return x.name<y.name;
        });
        std::cout<<"[ASSIGNMENT] mode="<<policy_.pairing_mode()<<" slots="<<policy_.enabled_slots()
                 <<" candidate_edges="<<policy_.assignment_candidate_edges()<<std::endl;
        size_t completed=0;
        for(size_t i:order) {
            routes_[i]=build_route(i,assignment[i]);account(i,routes_[i],1);++completed;
            if(completed%1000==0) std::cout<<"[PROGRESS] nets="<<completed<<"/"<<order.size()
                <<" seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-started_).count()<<std::endl;
        }
        initial_build_=false;
        validate_complete();initial_metrics_=best_metrics_=measure();complete_=true;
        std::cout<<"[INITIAL] nets="<<routes_.size()<<" slots="<<policy_.slots().size()<<" guide_overflow="<<best_metrics_.guide.overflow
                 <<" physical_overflow="<<best_metrics_.physical_overflow<<"\n";
        if(options_.v7_rrr && options_.official_model) {
            try { run_rrr(options_.rrr_rounds); }
            catch(const std::exception& e) {
                // rrr_round rolls its entire journal back before throwing.
                // Export only after the restored checkpoint verifies exactly.
                v7_refinement_active_=false;v7_scope_limit_=0;
                ++rejection_reasons_[std::string("V7 optional RRR stopped: ")+e.what()];
                validate_complete();best_metrics_=measure();verify_official_ledger(best_metrics_);
            }
        } else run_rrr(large_design()?std::min(3,options_.rrr_rounds):options_.rrr_rounds);
        // Global-grid supply can contain millions of cells even with few nets.
        // Its bounded spatial assignment/polish replaces this exhaustive
        // legacy net x reservation proposal enumeration.
        if(!large_design() && options_.tsv_policy.resource_mode!=TsvResourceMode::GLOBAL_GRID) optimize_tsv();
        if(options_.v7_tsv) optimize_v7_tsv();
        pre_recovery_metrics_=measure();
        if(!large_design()) recover_wirelength();
        if(options_.large_recovery) recover_budgeted_wirelength();
        pre_tsv_local_metrics_=best_metrics_=measure();
        if(!large_design()) recover_tsv_local();
        validate_complete();best_metrics_=measure();
        guides_.clear();for(size_t i=0;i<routes_.size();++i) guides_[def_.nets()[i].name]=routes_[i].guides;
        std::cout<<"[FINAL] guide_overflow="<<best_metrics_.guide.overflow<<" normalized="<<best_metrics_.guide.normalized
                 <<" TSV="<<best_metrics_.guide.tsv_count<<" expansions="<<counters_.expansions<<"\n";
        return true;
    } catch(const std::exception& e) {std::cerr<<"[ERROR] "<<e.what()<<"\n";complete_=false;return false;}
}
bool NegotiatedRouter::export_guide(const std::string& path) const {
    if(!complete_||guides_.empty()) return false;
    try {
        validate_complete();const auto parent=std::filesystem::path(path).parent_path();
        if(!parent.empty()) std::filesystem::create_directories(parent);
        std::ofstream out(path);if(!out) return false;
        for(const auto& [name,rects]:guides_) {
            out<<name<<"\n(\n";
            for(const auto& g:rects) out<<std::fixed<<std::setprecision(0)<<g.x1<<' '<<g.y1<<' '<<g.x2<<' '<<g.y2<<' '<<g.layer<<'\n';
            out<<")\n";
        }
        out.close();if(!out) return false;
        // Read back exactly what was serialized before reporting success.
        std::ifstream in(path);GuideMap loaded;std::string name,token;
        while(in>>name) {
            if(!(in>>token)||token!="(") return false;
            while(in>>token&&token!=")") {
                GuideRect g;g.x1=std::stod(token);
                if(!(in>>g.y1>>g.x2>>g.y2>>g.layer)) return false;
                loaded[name].push_back(g);
            }
            if(token!=")") return false;
        }
        if(options_.official_model && options_.tsv_policy.resource_mode==TsvResourceMode::GLOBAL_GRID) {
            for(const auto& net:def_.nets()) {
                const auto found=loaded.find(net.name);std::string error;
                if(found==loaded.end() || !validate_gcell_encoding(net,def_.die_area(),graph_.grid(),lef_,found->second,error))
                    throw std::runtime_error("GCELL_ENCODING_VIOLATION: "+error);
            }
        }
        if(options_.official_metrics.score_20261020) return compat::validate(def_,lef_,loaded).error_count==0;
        std::string error;return validate_guides(def_,lef_,loaded,error);
    } catch(const std::exception& e) {std::cerr<<"[ERROR] export: "<<e.what()<<"\n";return false;}
}
bool NegotiatedRouter::export_stats(const std::string& path) const {
    if(!complete_) return false;
    try {
        auto parent=std::filesystem::path(path).parent_path();if(!parent.empty()) std::filesystem::create_directories(parent);
        std::ofstream out(path);if(!out) return false;out<<std::setprecision(17);
        auto metrics=[&](const CompleteMetrics& m) {
            out<<"{\"guide_overflow\":"<<m.guide.overflow<<",\"physical_overflow\":"<<m.physical_overflow
               <<",\"quality\":"<<m.guide.quality<<",\"guide_mst_dbu\":"<<m.guide.wirelength
               <<",\"reference_dbu\":"<<m.guide.reference<<",\"normalized_wirelength\":"<<m.guide.normalized
               <<",\"correlation\":"<<m.guide.correlation<<",\"evaluator_tsv_count\":"<<m.guide.tsv_count
               <<",\"physical_tsv_count\":"<<m.physical_tsvs<<",\"physical_length_dbu\":"<<m.physical_length
               <<",\"ordinary_vias\":"<<m.vias;
            if(m.official_model) out<<",\"wirelength_score\":"<<m.official.wirelength_score<<",\"congestion_score\":"<<m.official.congestion_score
                <<",\"thermal_score\":"<<m.official.thermal_score<<",\"zero_capacity_cells\":"<<m.official.zero_capacity_cells;
            out<<"}";
        };
        out<<"{\n\"scoring_model\":"<<json_string(options_.official_metrics.score_20261020?"compat-20261020":options_.official_model?"official-2026":"legacy-proxy")<<",\n\"geometry_model\":"<<json_string(graph_.capacity_model())<<",\n\"guide_encoding\":"<<json_string(options_.official_model?"global-gcell-regions":large_design()?"width-corridors-and-via-enclosures":"legacy-gcell-envelopes")
           <<",\n\"guide_encoding_contract\":"<<json_string(options_.official_model && options_.tsv_policy.resource_mode==TsvResourceMode::GLOBAL_GRID?"grid-native-20261024":"historical")
           <<",\n\"resource_profile\":"<<json_string(!graph_.global_guide_mode()?"physical":options_.official_model && options_.official_metrics.score_20261020?
                (graph_.platform_resources()?"platform-20261024":"20261020"):"legacy-20261011")
           <<",\n\"official_scorer_run\":false,\n\"score_evidence\":\"local-model-only\""
           <<",\n\"large_design_schedule\":"<<(large_design()?"true":"false")
           <<",\n\"input_mapping\":{\"thermal\":"<<json_string(thermal_.mapping_mode())
           <<",\"interpolated_cells\":"<<thermal_.interpolated_cells()<<",\"physical_track_capacity_layers\":[";
        bool first_layer=true;
        for(int l=1;l<=20;++l) if(graph_.guide_uses_physical_tracks(l)) {
            if(!first_layer) out<<',';
            first_layer=false;out<<l;
        }
        out<<"]},\n\"policy\":{\"pairing\":"<<json_string(policy_.pairing_mode())
           <<",\"assignment\":"<<json_string(policy_.assignment_mode())<<",\"enabled_slots\":"<<policy_.enabled_slots()
           <<",\"candidate_edges\":"<<policy_.assignment_candidate_edges()<<",\"expansions\":"<<policy_.assignment_expansions()<<",\"global_completions\":"<<policy_.assignment_global_completions()
           <<",\"assignment_polish_thermal_rule\":"<<json_string(policy_.assignment_polish_thermal_mode())
           <<",\"assignment_polish_correlation_before\":"<<policy_.assignment_polish_correlation_before()<<",\"assignment_polish_correlation_after\":"<<policy_.assignment_polish_correlation_after()
           <<",\"assignment_polish_heat_weight\":"<<options_.tsv_policy.polish_heat_weight<<",\"assignment_polish_moves\":"<<policy_.assignment_polish_moves()<<",\"assignment_polish_swaps\":"<<policy_.assignment_polish_swaps()<<",\"assignment_polish_cost_reduction\":"<<policy_.assignment_polish_cost_reduction()
           <<",\"assignment_polish_candidates\":"<<options_.tsv_policy.polish_candidates
           <<",\"assignment_polish_passes_limit\":"<<options_.tsv_policy.polish_passes
           <<",\"assignment_polish_bidirectional\":"<<(options_.tsv_policy.polish_bidirectional?"true":"false")
           <<",\"assignment_polish_passes\":"<<policy_.assignment_polish_passes()
           <<",\"assignment_polish_thermal_candidates_enabled\":"<<(options_.tsv_policy.polish_thermal_candidates?"true":"false")
           <<",\"assignment_polish_thermal_queries\":"<<policy_.assignment_polish_thermal_queries()
           <<",\"assignment_polish_thermal_candidates\":"<<policy_.assignment_polish_thermal_candidates()
           <<",\"assignment_block_refine\":"<<(options_.tsv_policy.assignment_block_refine?"true":"false")
           <<",\"assignment_block_size\":"<<options_.tsv_policy.block_size
           <<",\"assignment_block_passes_limit\":"<<options_.tsv_policy.block_passes
           <<",\"assignment_block_max_groups\":"<<options_.tsv_policy.block_max_groups
           <<",\"assignment_block_max_evaluations\":"<<options_.tsv_policy.block_max_cost_evaluations
           <<",\"assignment_block_seconds_limit\":"<<options_.tsv_policy.block_max_seconds
           <<",\"assignment_block_groups\":"<<policy_.assignment_block_groups()
           <<",\"assignment_block_improvements\":"<<policy_.assignment_block_improvements()
           <<",\"assignment_block_moved_nets\":"<<policy_.assignment_block_moved_nets()
           <<",\"assignment_block_cost_evaluations\":"<<policy_.assignment_block_cost_evaluations()
           <<",\"assignment_block_mst_work\":"<<policy_.assignment_block_mst_work()
           <<",\"assignment_block_skipped_high_fanout\":"<<policy_.assignment_block_skipped_high_fanout()
           <<",\"assignment_block_cost_reduction\":"<<policy_.assignment_block_cost_reduction()
           <<",\"assignment_block_budget_exhausted\":"<<(policy_.assignment_block_budget_exhausted()?"true":"false")
           <<",\"assignment_block_seconds\":"<<policy_.assignment_block_seconds()
                  <<",\"max_anchor_displacement_dbu\":"<<policy_.assignment_max_displacement_dbu()
           <<",\"slot_capacity\":1,\"tsv_per_cross_net\":1,\"status\":\"engineering global-routing model\"},\n\"metrics\":";
        metrics(best_metrics_);out<<",\n\"initial_metrics\":";metrics(initial_metrics_);
        out<<",\n\"pre_recovery_metrics\":";metrics(pre_recovery_metrics_);
        out<<",\n\"pre_tsv_local_metrics\":";metrics(pre_tsv_local_metrics_);
        out<<",\n\"budgeted_wirelength_recovery\":{\"enabled\":"<<(options_.large_recovery?"true":"false")
           <<",\"net_limit\":"<<options_.large_recovery_nets<<",\"rounds\":"<<options_.large_recovery_rounds
           <<",\"branch_limit\":"<<options_.large_recovery_branches<<",\"budget_seconds\":"<<options_.large_recovery_seconds
           <<",\"selected\":"<<recovery_stats_.selected<<",\"zero_reference\":"<<recovery_stats_.zero_reference
           <<",\"attempted\":"<<recovery_stats_.attempted<<",\"candidates\":"<<recovery_stats_.candidates
           <<",\"shared_candidates\":"<<recovery_stats_.shared_candidates<<",\"local_candidates\":"<<recovery_stats_.local_candidates
           <<",\"accepted\":"<<recovery_stats_.accepted<<",\"rejected\":"<<recovery_stats_.rejected
           <<",\"candidate_failures\":"<<recovery_stats_.candidate_failures<<",\"checkpoints\":"<<recovery_stats_.checkpoints
           <<",\"checkpoint_rollbacks\":"<<recovery_stats_.checkpoint_rollbacks<<",\"timed_out\":"<<(recovery_stats_.timed_out?"true":"false")
           <<",\"seconds\":"<<recovery_stats_.seconds<<",\"before\":";metrics(recovery_stats_.before);
        out<<",\"after\":";metrics(recovery_stats_.after);out<<"}";
        out<<",\n\"counters\":{\"expansions\":"<<counters_.expansions<<",\"search_peak_states\":"<<counters_.search_peak_states<<",\"rrr_attempted\":"<<counters_.rrr_attempted
           <<",\"complete_validations\":"<<complete_validations_<<",\"complete_validation_cache_hits\":"<<complete_validation_cache_hits_
           <<",\"rrr_accepted\":"<<counters_.rrr_accepted<<",\"rollbacks\":"<<counters_.rollbacks
           <<",\"tsv_trials\":"<<counters_.tsv_trials<<",\"tsv_accepted\":"<<counters_.tsv_accepted
           <<",\"policy_rejections\":"<<counters_.policy_rejections<<",\"local_repairs\":"<<counters_.local_repairs
           <<",\"templates_used\":"<<counters_.templates_used
           <<",\"pin_local_candidates\":"<<counters_.pin_local_candidates
           <<",\"wirelength_trials\":"<<counters_.wirelength_trials
           <<",\"wirelength_accepted\":"<<counters_.wirelength_accepted
           <<",\"tsv_local_candidates\":"<<counters_.tsv_local_candidates
           <<",\"tsv_local_trials\":"<<counters_.tsv_local_trials
           <<",\"tsv_local_accepted\":"<<counters_.tsv_local_accepted<<",\"abstract_terminal_accesses\":"<<counters_.abstract_terminal_accesses
           <<",\"port_terminal_accesses\":"<<counters_.port_terminal_accesses<<",\"omitted_checker_apertures\":"<<counters_.omitted_checker_apertures
           <<",\"fast_initial_paths\":"<<counters_.fast_initial_paths
           <<",\"fast_tree_shortcuts\":"<<counters_.fast_tree_shortcuts
           <<",\"fast_truncated_paths\":"<<counters_.fast_truncated_paths
           <<",\"fast_z_candidates\":"<<counters_.fast_z_candidates
           <<",\"fast_z_paths\":"<<counters_.fast_z_paths
           <<",\"fast_layer_prunes\":"<<counters_.fast_layer_prunes
           <<",\"v7_topology_attempted\":"<<counters_.v7_topology_attempted
           <<",\"v7_topology_candidates\":"<<counters_.v7_topology_candidates
           <<",\"v7_topology_accepted\":"<<counters_.v7_topology_accepted
           <<",\"v7_topology_failures\":"<<counters_.v7_topology_failures
           <<",\"v7_topology_seconds\":"<<counters_.v7_topology_seconds
           <<",\"v7_rrr_nets\":"<<counters_.v7_rrr_nets
           <<",\"v7_astar_nets\":"<<counters_.v7_astar_nets
           <<",\"v7_astar_searches\":"<<counters_.v7_astar_searches
           <<",\"v7_astar_fallbacks\":"<<counters_.v7_astar_fallbacks
           <<",\"v7_ledger_checks\":"<<counters_.v7_ledger_checks
           <<",\"v7_checkpoint_restores\":"<<counters_.v7_checkpoint_restores
           <<",\"v7_rrr_peak_scope\":"<<counters_.v7_rrr_peak_scope
           <<",\"v7_rrr_seconds\":"<<counters_.v7_rrr_seconds
           <<",\"v7_validation_seconds\":"<<counters_.v7_validation_seconds
           <<",\"v7_tsv_candidates\":"<<counters_.v7_tsv_candidates
           <<",\"v7_tsv_attempted\":"<<counters_.v7_tsv_attempted
           <<",\"v7_tsv_accepted\":"<<counters_.v7_tsv_accepted
           <<",\"v7_tsv_rollbacks\":"<<counters_.v7_tsv_rollbacks
           <<",\"v7_tsv_failures\":"<<counters_.v7_tsv_failures
           <<",\"v7_tsv_budget_stops\":"<<counters_.v7_tsv_budget_stops
           <<",\"v7_tsv_seconds\":"<<counters_.v7_tsv_seconds
           <<",\"v5_topology_attempted\":"<<counters_.v5_topology_attempted
           <<",\"v5_topology_candidates\":"<<counters_.v5_topology_candidates
           <<",\"v5_topology_accepted\":"<<counters_.v5_topology_accepted
           <<",\"v5_topology_failures\":"<<counters_.v5_topology_failures
           <<",\"v5_topology_thermal_rejections\":"<<counters_.v5_topology_thermal_rejections
           <<",\"v5_topology_budget_skips\":"<<counters_.v5_topology_budget_skips
           <<",\"v5_topology_interruptions\":"<<counters_.v5_topology_interruptions
           <<",\"v5_topology_seconds\":"<<counters_.v5_topology_seconds<<"},\n\"assignment\":[";
        bool first=true;for(size_t i=0;i<routes_.size();++i) if(routes_[i].slot>=0) {
            if(!first) out<<',';
            first=false;const auto& s=policy_.slots()[routes_[i].slot];
            out<<"{\"net\":"<<json_string(def_.nets()[i].name)<<",\"slot\":"<<s.id<<",\"x\":"<<s.point.x<<",\"y\":"<<s.point.y<<"}";
        }
        out<<"],\n\"rejection_reasons\":{";
        bool first_reason=true;
        for(const auto& [reason,count]:rejection_reasons_) {
            if(!first_reason) out<<',';
            first_reason=false;out<<json_string(reason)<<':'<<count;
        }
        out<<"},\n\"options\":{\"rrr_rounds\":"<<options_.rrr_rounds<<",\"tsv_rounds\":"<<options_.tsv_rounds
           <<",\"rrr_score_aware\":"<<(options_.rrr_score_aware?"true":"false")
           <<",\"rrr_net_limit\":"<<options_.rrr_net_limit
           <<",\"v7_rrr\":"<<(options_.v7_rrr?"true":"false")
           <<",\"v7_rrr_net_limit\":"<<options_.v7_rrr_net_limit
           <<",\"v7_rrr_astar_net_limit\":"<<options_.v7_rrr_astar_net_limit
           <<",\"v7_rrr_astar_expansions\":"<<options_.v7_rrr_astar_expansions
           <<",\"v7_rrr_astar_seconds\":"<<options_.v7_rrr_astar_seconds
           <<",\"v7_topology\":"<<(options_.v7_topology?"true":"false")
           <<",\"v7_cell_cost\":"<<(options_.v7_cell_cost?"true":"false")
           <<",\"v7_tsv\":"<<(options_.v7_tsv?"true":"false")
           <<",\"v7_tsv_nets\":"<<options_.v7_tsv_nets
           <<",\"v7_tsv_candidates\":"<<options_.v7_tsv_candidates
           <<",\"v7_tsv_seconds\":"<<options_.v7_tsv_seconds
           <<",\"official_score_pricing\":"<<(options_.official_score_pricing?"true":"false")
           <<",\"congestion_price_updates\":"<<pricing_updates_
           <<",\"congestion_price_min\":"<<(pricing_updates_?congestion_price_min_:1)
           <<",\"congestion_price_max\":"<<(pricing_updates_?congestion_price_max_:1)
           <<",\"local_repair\":"<<(options_.local_repair?"true":"false")<<",\"search\":"<<json_string(options_.dijkstra?"dijkstra":"astar")
           <<",\"search_limit\":"<<options_.search_limit<<",\"time_limit_seconds\":"<<options_.time_limit
           <<",\"wirelength_rounds\":"<<options_.wirelength_rounds<<",\"pin_local\":"<<(options_.pin_local?"true":"false")
           <<",\"fast_initial\":"<<(options_.fast_initial?"true":"false")
           <<",\"fast_quality\":"<<(options_.fast_quality?"true":"false")
           <<",\"fast_layer_pruning\":"<<(options_.fast_layer_pruning?"true":"false")
           <<",\"v5_pin_rooted\":"<<(options_.v5_pin_rooted?"true":"false")
           <<",\"v5_topology_max_pins\":"<<options_.v5_topology_max_pins
           <<",\"v5_topology_seconds_limit\":"<<v5_topology_seconds_limit()
           <<",\"tsv_local\":"<<(options_.tsv_local?"true":"false")<<"},\n";
#if defined(__linux__)
        struct rusage resource_usage{};
        if(getrusage(RUSAGE_SELF,&resource_usage)==0) out<<"\"peak_rss_kb\":"<<resource_usage.ru_maxrss<<",\n";
#endif
        out<<"\"elapsed_seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-started_).count()<<"\n}\n";
        out.close();return bool(out);
    } catch(const std::exception& e) {std::cerr<<"[ERROR] stats: "<<e.what()<<"\n";return false;}
}
}
