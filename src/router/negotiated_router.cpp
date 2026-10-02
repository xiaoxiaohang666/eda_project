#include "router/negotiated_router.hpp"
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
    :def_(d),lef_(l),thermal_(t),options_(o),graph_(d,l),policy_(d,l,graph_),
     demand_(graph_.size()),cell_nets_(graph_.size()),guide_history_(graph_.size()) {
    started_=std::chrono::steady_clock::now();
    // Keep part of even a short explicit budget available for final encoding/export.
    const double reserve=std::min(60.0,std::max(0.01,options_.time_limit*0.1));
    deadline_=started_+std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(std::max(0.0,options_.time_limit-reserve)));
}
bool NegotiatedRouter::timed_out() const { return std::chrono::steady_clock::now()>=deadline_; }
double NegotiatedRouter::edge_cost(int a,int b,const NetRoute& own) const {
    const auto key=route_edge(a,b);
    if(own.edges.count(key)) return 0;
    double result=graph_.edge_length(a,b)/graph_.ell0();
    if(graph_.layer(a)!=graph_.layer(b)) result+=0.2;
    const double cap=graph_.edge_capacity(a,b);
    auto it=usage_.find(key);
    const int u=it==usage_.end()?0:it->second;
    if(cap>=0) {
        result+=present_*marginal(u,cap);
        auto h=edge_history_.find(key); if(h!=edge_history_.end()) result+=h->second;
    }
    if(!own.nodes.count(b))
        result+=present_*marginal(demand_[b],graph_.guide_capacity(b))+guide_history_[b];
    return result;
}
double NegotiatedRouter::access_cost(const PinAccess& a) const {
    double result=a.length/graph_.ell0()+0.2*a.vias;
    std::set<int> touched;
    const auto& grid=graph_.grid();
    for(const auto& g:a.guides) {
        int c0=grid.get_col(g.x1), c1=grid.get_col(std::nextafter(g.x2,-inf));
        int r0=grid.get_row(g.y1), r1=grid.get_row(std::nextafter(g.y2,-inf));
        for(int r=r0;r<=r1;++r) for(int c=c0;c<=c1;++c) {
            int n=graph_.node(c,r,get_layer_index(g.layer));
            if(n>=0 && touched.insert(n).second)
                result+=present_*marginal(demand_[n],graph_.guide_capacity(n))+guide_history_[n];
        }
    }
    return result;
}
std::vector<PinAccess> NegotiatedRouter::accesses(Point p,int original,int low,int high) {
    const auto key=std::make_tuple(p.x,p.y,original,low,high);
    if(auto found=access_cache_.find(key);found!=access_cache_.end()) return found->second;
    std::vector<PinAccess> candidates;
    const auto& grid=graph_.grid();
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
    for(int radius:{1,2,4,8}) {
        for(int first=low;first<=high;++first) {
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
                        if(graph_.legal_node(n)) {a.node=n;candidates.push_back(a);}
                    }
                    for(int last:{first-1,first+1}) {
                        if(last<low||last>high||lef_.get_layer_direction(get_layer_name(last))==dir) continue;
                        PinAccess b=a;
                        if(!add_stack(b,bend,first,last)||!add_segment(b,bend,q,last)) continue;
                        const int n=graph_.node(c,r,last);
                        if(!graph_.legal_node(n)) continue;
                        b.node=n;candidates.push_back(std::move(b));
                    }
                }
        }
        if(!candidates.empty()) break;
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
    access_cache_[key]=result;return result;
}
PathResult NegotiatedRouter::search(const std::vector<PinAccess>& sources,const std::set<int>& targets,
                                   const NetRoute& own,int low,int high,bool dijkstra) {
    if(sources.empty()||targets.empty()) return {};
    const auto& grid=graph_.grid();
    int minc=grid.cols,maxc=0,minr=grid.rows,maxr=0,minl=21,maxl=0;
    for(int n:targets) {minc=std::min(minc,graph_.col(n));maxc=std::max(maxc,graph_.col(n));
        minr=std::min(minr,graph_.row(n));maxr=std::max(maxr,graph_.row(n));
        minl=std::min(minl,graph_.layer(n));maxl=std::max(maxl,graph_.layer(n));}
    const Point boxlo=grid.get_cell_rect(minc,minr).center(),boxhi=grid.get_cell_rect(maxc,maxr).center();
    auto heuristic=[&](int n) {
        if(dijkstra) return 0.0;
        auto p=graph_.point(n);
        return (std::max({0.0,boxlo.x-p.x,p.x-boxhi.x})+std::max({0.0,boxlo.y-p.y,p.y-boxhi.y}))/graph_.ell0()+
            0.2*std::max({0,minl-graph_.layer(n),graph_.layer(n)-maxl});
    };
    for(const auto& a:sources) {minc=std::min(minc,graph_.col(a.node));maxc=std::max(maxc,graph_.col(a.node));
        minr=std::min(minr,graph_.row(a.node));maxr=std::max(maxr,graph_.row(a.node));}
    // Layer-aware L/Z templates provide a legal incumbent. A* still proves the
    // best cost under the same snapshot, so Dijkstra remains an exact oracle.
    PathResult incumbent; incumbent.cost=inf;
    if(!dijkstra) {
        std::vector<int> near(targets.begin(),targets.end());
        auto target_distance=[&](int n) {double d=inf;for(const auto& a:sources) d=std::min(d,graph_.point(a.node).distance_to(graph_.point(n)));return d;};
        std::stable_sort(near.begin(),near.end(),[&](int a,int b){return std::tuple(target_distance(a),a)<std::tuple(target_distance(b),b);});
        if(near.size()>4) near.resize(4);
        for(size_t si=0;si<std::min<size_t>(sources.size(),4);++si) for(int target:near) {
            const int source=sources[si].node;
            std::vector<std::tuple<int,int,int>> pairs;
            for(int h=low;h<=high;++h) if(lef_.get_layer_direction(get_layer_name(h))==Direction::HORIZONTAL)
                for(int v=low;v<=high;++v) if(lef_.get_layer_direction(get_layer_name(v))==Direction::VERTICAL)
                    pairs.emplace_back(std::abs(graph_.layer(source)-h)+std::abs(h-v)+std::abs(v-graph_.layer(target)),h,v);
            std::sort(pairs.begin(),pairs.end());if(pairs.size()>4) pairs.resize(4);
            for(auto [rank,h,v]:pairs) for(int kind=0;kind<4;++kind) {
                (void)rank;std::vector<int> path{source};bool legal=true;
                auto append=[&](int n) {if(!legal)return;if(n<0||!graph_.legal_edge(path.back(),n)){legal=false;return;}path.push_back(n);};
                auto stack=[&](int l) {while(legal&&graph_.layer(path.back())!=l) {
                    int n=path.back(),next=graph_.layer(n)+(l>graph_.layer(n)?1:-1);
                    append(graph_.node(graph_.col(n),graph_.row(n),next));}};
                auto leg=[&](int c,int r,int l) {
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
                double cost=access_cost(sources[si]);for(size_t k=1;k<path.size();++k) cost+=edge_cost(path[k-1],path[k],own);
                if(cost<incumbent.cost) incumbent={PathResult::Status::FOUND,std::move(path),si,cost};
            }
        }
    }
    size_t expanded=0;
    for(int margin=4;;margin*=2) {
        const int lc=std::max(0,minc-margin),hc=std::min(grid.cols-1,maxc+margin),
                  lr=std::max(0,minr-margin),hr=std::min(grid.rows-1,maxr+margin);
        std::vector<double> dist(graph_.size(),inf);
        std::vector<int> parent(graph_.size(),-1), origin(graph_.size(),-1);
        using Q=std::tuple<double,double,int>;
        std::priority_queue<Q,std::vector<Q>,std::greater<Q>> todo;
        for(size_t i=0;i<sources.size();++i) {
            const int n=sources[i].node; const double cost=access_cost(sources[i]);
            if(cost<dist[n]) {dist[n]=cost;origin[n]=int(i);todo.emplace(cost+heuristic(n),cost,n);}
        }
        while(!todo.empty()) {
            auto [f,cost,n]=todo.top();todo.pop(); (void)f;
            if(cost!=dist[n]) continue;
            if(f>incumbent.cost+1e-10) continue;
            if(++expanded>options_.search_limit || ((expanded&255U)==0 && timed_out()))
                return {PathResult::Status::SEARCH_LIMIT,{},0,0};
            ++counters_.expansions;
            if(targets.count(n)) {
                PathResult result{PathResult::Status::FOUND,{},static_cast<size_t>(origin[n]),cost};
                for(int p=n;p!=-1;p=parent[p]) result.nodes.push_back(p);
                std::reverse(result.nodes.begin(),result.nodes.end());return result;
            }
            for(int next:graph_.neighbors(n)) {
                if(graph_.layer(next)<low||graph_.layer(next)>high||graph_.col(next)<lc||graph_.col(next)>hc||
                   graph_.row(next)<lr||graph_.row(next)>hr) continue;
                const double d=cost+edge_cost(n,next,own);
                if(d+1e-10<dist[next]) {
                    dist[next]=d;parent[next]=n;origin[next]=origin[n];todo.emplace(d+heuristic(next),d,next);
                }
            }
        }
        if(incumbent.status==PathResult::Status::FOUND) {++counters_.templates_used;return incumbent;}
        if(lc==0&&hc==grid.cols-1&&lr==0&&hr==grid.rows-1) return {};
    }
}
void NegotiatedRouter::encode(size_t index,NetRoute& route) {
    route.guides.clear();route.length=0;route.vias=0;
    const auto& grid=graph_.grid();
    // Maximal preferred-direction runs partition the set of occupied GCells.
    std::set<int> pending=route.nodes;
    while(!pending.empty()) {
        const int n=*pending.begin();pending.erase(pending.begin());
        const int l=graph_.layer(n),c=graph_.col(n),r=graph_.row(n);
        const bool horizontal=lef_.get_layer_direction(get_layer_name(l))==Direction::HORIZONTAL;
        int end=horizontal?c:r;
        while(true) {
            int next=horizontal?graph_.node(end+1,r,l):graph_.node(c,end+1,l);
            if(next<0||!pending.erase(next)) break;
            ++end;
        }
        Rect a=grid.get_cell_rect(c,r),b=grid.get_cell_rect(horizontal?end:c,horizontal?r:end);
        route.guides.emplace_back(a.x1,a.y1,b.x2,b.y2,get_layer_name(l));
    }
    for(auto key:route.edges) {auto [a,b]=edge_nodes(key);route.length+=graph_.edge_length(a,b);
        if(graph_.layer(a)!=graph_.layer(b)) ++route.vias;}
    for(const auto& a:route.accesses) {
        route.guides.insert(route.guides.end(),a.guides.begin(),a.guides.end());
        route.length+=a.length;route.vias+=a.vias;
    }
    // The fixed checker uses untransformed instance-pin coordinates. Preserve the
    // real access; only widen its small guide aperture when the complete aperture
    // is geometrically free. Never move a terminal or claim a wire through OBS.
    for(const auto& p:def_.nets()[index].pins) if(p.has_evaluator_coord) {
        bool covered=false;
        for(const auto& g:route.guides) if(g.layer==p.layer&&Rect(g.x1,g.y1,g.x2,g.y2).contains(p.evaluator_coord)) covered=true;
        if(!covered) {
            double half=graph_.routing_width(p.layer_idx)*0.5;
            Rect aperture(std::min(p.coord.x,p.evaluator_coord.x)-half,std::min(p.coord.y,p.evaluator_coord.y)-half,
                          std::max(p.coord.x,p.evaluator_coord.x)+half,std::max(p.coord.y,p.evaluator_coord.y)+half);
            auto g=guide(aperture,p.layer_idx,def_.die_area());
            if(!graph_.legal_rect(Rect(g.x1,g.y1,g.x2,g.y2),p.layer_idx))
                throw std::runtime_error("CHECKER_PIN_LOCATION_MISMATCH: "+p.name);
            route.guides.push_back(g);
        }
    }
    if(route.slot>=0) {
        const auto& slot=policy_.slots().at(route.slot);
        route.guides.push_back(guide(slot.lower_pad,10,def_.die_area()));
        route.guides.push_back(guide(slot.upper_pad,11,def_.die_area()));
        // The landing and its escape are one guide witness per interface layer.
        // Widening this small local access corridor is accounted in exact demand.
        for(int l:{10,11}) {
            const auto pad=l==10?slot.lower_pad:slot.upper_pad;
            Rect box=pad;std::vector<GuideRect> rest;
            for(const auto& g:route.guides) {
                if(get_layer_index(g.layer)==l && overlaps(g,pad))
                    box=Rect(std::min(box.x1,g.x1),std::min(box.y1,g.y1),std::max(box.x2,g.x2),std::max(box.y2,g.y2));
                else rest.push_back(g);
            }
            rest.push_back(guide(box,l,def_.die_area()));route.guides=std::move(rest);
        }
    }
    auto key=[](const GuideRect& r){return std::tuple(get_layer_index(r.layer),r.x1,r.y1,r.x2,r.y2);};
    std::sort(route.guides.begin(),route.guides.end(),[&](auto& a,auto& b){return key(a)<key(b);});
    route.guides.erase(std::unique(route.guides.begin(),route.guides.end(),[&](auto& a,auto& b){return key(a)==key(b);}),route.guides.end());
    // Exact rectangular unions only. Re-scan deterministically after each merge.
    bool changed=true;
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
    std::string error;
    if(!policy_.validate_mapping(def_.nets()[index],route.guides,route.slot,error))
        throw std::runtime_error("TSV_MAPPING_UNRESOLVED: "+error);
    // Full connectivity and pin checks for just this net, without fabricating other guides.
    for(const auto& p:def_.nets()[index].pins) {
        bool found=false;
        for(const auto& g:route.guides) if(g.layer==p.layer && Rect(g.x1,g.y1,g.x2,g.y2).contains(p.coord)) found=true;
        if(!found) throw std::runtime_error("HARD_RULE_VIOLATION: pin lost: "+p.name);
    }
    route.measured=measure_net_guides(route.guides,graph_,lef_);
}
NetRoute NegotiatedRouter::build_candidate(size_t index,int slot,int variant) {
    const auto& net=def_.nets()[index];NetRoute result;result.slot=slot;
    for(int side=0;side<2;++side) {
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
        std::vector<bool> added(terminals.size());std::set<int> tree;
        for(size_t count=0;count<terminals.size();++count) {
            if(timed_out()) throw std::runtime_error("SEARCH_LIMIT: routing time budget exhausted");
            size_t selected=root;
            if(count) {
                double best=variant?-1:inf;
                for(size_t i=0;i<terminals.size();++i) if(!added[i]) {
                    double distance=inf;
                    for(size_t j=0;j<terminals.size();++j) if(added[j])
                        distance=std::min(distance,terminals[i].coord.distance_to(terminals[j].coord)/graph_.ell0()+
                            0.2*std::abs(terminals[i].layer_idx-terminals[j].layer_idx));
                    if((!variant&&distance<best)||(variant&&distance>best)) {best=distance;selected=i;}
                }
            }
            const auto& pin=terminals[selected];auto choices=accesses(pin.coord,pin.layer_idx,low,high);
            if(tree.empty()) {
                size_t chosen=0;for(size_t j=1;j<choices.size();++j)
                    if(access_cost(choices[j])<access_cost(choices[chosen])) chosen=j;
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
        }
    }
    encode(index,result);return result;
}
NetRoute NegotiatedRouter::build_route(size_t index,int slot) {
    std::optional<NetRoute> best;std::string failure;
    auto local_key=[&](const NetRoute& r) {
        long long overflow=0,physical=0;
        for(auto [n,m]:r.measured.demand)
            overflow+=std::max(0,demand_[n]+m-int(graph_.guide_capacity(n)))-std::max(0,demand_[n]-int(graph_.guide_capacity(n)));
        for(auto e:r.edges) {auto [a,b]=edge_nodes(e);double cap=graph_.edge_capacity(a,b);if(cap<0) continue;
            auto it=usage_.find(e);int u=it==usage_.end()?0:it->second;
            physical+=std::max(0,u+1-int(cap))-std::max(0,u-int(cap));}
        const double norm=r.measured.reference>0?r.measured.wirelength/r.measured.reference:100;
        return std::tuple(overflow,physical,norm,r.length,r.vias);
    };
    for(int variant=0;variant<2;++variant) try {
        auto trial=build_candidate(index,slot,variant);
        if(!best || local_key(trial)<local_key(*best)) best=std::move(trial);
    } catch(const std::exception& e) {failure=e.what();}
    if(!best) throw std::runtime_error(def_.nets()[index].name+": "+failure);
    return std::move(*best);
}
void NegotiatedRouter::account(size_t index,const NetRoute& r,int sign) {
    for(auto e:r.edges) {
        usage_[e]+=sign;
        if(usage_[e]<0) throw std::logic_error("negative physical demand");
        if(sign>0) edge_nets_[e].insert(int(index));else edge_nets_[e].erase(int(index));
    }
    for(auto [n,m]:r.measured.demand) {
        demand_[n]+=sign*m;
        if(demand_[n]<0) throw std::logic_error("negative guide demand");
        if(sign>0) cell_nets_[n].insert(int(index));else cell_nets_[n].erase(int(index));
    }
}
CompleteMetrics NegotiatedRouter::measure() const {
    CompleteMetrics result;GuideMap guides;
    for(size_t i=0;i<routes_.size();++i) {
        guides[def_.nets()[i].name]=routes_[i].guides;result.physical_length+=routes_[i].length;
        result.vias+=routes_[i].vias;result.physical_tsvs+=routes_[i].slot>=0;
    }
    result.guide=evaluate_guides(guides,graph_,lef_,thermal_);
    if(result.guide.demand!=demand_) throw std::logic_error("guide ledger disagrees with exact recomputation");
    for(auto [e,u]:usage_) {auto [a,b]=edge_nodes(e);const double cap=graph_.edge_capacity(a,b);
        if(cap>=0) result.physical_overflow+=std::max(0,u-int(cap));}
    return result;
}
bool NegotiatedRouter::better(const CompleteMetrics& a,const CompleteMetrics& b) {
    if(a.guide.overflow!=b.guide.overflow) return a.guide.overflow<b.guide.overflow;
    if(a.physical_overflow!=b.physical_overflow) return a.physical_overflow<b.physical_overflow;
    if(std::abs(a.guide.quality-b.guide.quality)>1e-6) return a.guide.quality>b.guide.quality;
    if(std::abs(a.physical_length-b.physical_length)>1e-6) return a.physical_length<b.physical_length;
    return a.vias<b.vias;
}
bool NegotiatedRouter::repair_forest(size_t index,int slot,NetRoute& trial) {
    // Keep pin access geometry and unaffected graph branches. A TSV relocation
    // with different terminal accesses uses the full-net fallback below.
    if(slot!=routes_[index].slot) return false;
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
    const auto before=measure();std::vector<std::pair<size_t,NetRoute>> saved;
    auto rollback=[&] {
        for(auto& [i,old]:saved) {account(i,routes_[i],-1);routes_[i]=std::move(old);account(i,routes_[i],1);}
        ++counters_.rollbacks;
    };
    try {
        for(size_t i:scope) {
            if(timed_out()) {rollback();return false;}
            const NetRoute old=routes_[i];account(i,old,-1);
            try {
                NetRoute trial;
                bool repaired=false;
                if(options_.local_repair) try {repaired=repair_forest(i,old.slot,trial);} catch(const std::exception&) {}
                auto full=build_route(i,old.slot);
                auto candidate_key=[&](const NetRoute& r) {
                    long long overflow=0;
                    for(auto [n,m]:r.measured.demand) overflow+=std::max(0,demand_[n]+m-int(graph_.guide_capacity(n)))-std::max(0,demand_[n]-int(graph_.guide_capacity(n)));
                    return std::tuple(overflow,r.measured.wirelength/std::max(1.0,r.measured.reference),r.length,r.vias);
                };
                if(!repaired||candidate_key(full)<candidate_key(trial)) trial=std::move(full);
                // Transaction permits one net to get worse so later nets can benefit.
                routes_[i]=std::move(trial);account(i,routes_[i],1);saved.emplace_back(i,old);
            } catch(const std::exception& e) {++rejection_reasons_[e.what()];routes_[i]=old;account(i,old,1);}
        }
        validate_complete();auto after=measure();
        if(better(after,before)) {best_metrics_=after;++counters_.rrr_accepted;return true;}
        ++rejection_reasons_["RRR round did not improve the complete comparison key"];
        rollback();return false;
    } catch(...) {rollback();throw;}
}
void NegotiatedRouter::run_rrr(int rounds) {
    int stale=0,polish=0;
    for(int round=0;round<rounds&&!timed_out();++round) {
        std::vector<double> priority(routes_.size());bool overflow=false;
        for(size_t n=0;n<demand_.size();++n) {
            const double cap=graph_.guide_capacity(int(n)),o=std::max(0.0,demand_[n]-cap);
            guide_history_[n]+=o/std::max(1.0,cap);
            if(o>0) {overflow=true;for(int i:cell_nets_[n]) {
                for(auto [cell,count]:routes_[i].measured.demand) if(cell==int(n)) priority[i]+=o*count;
            }}
        }
        for(auto [e,u]:usage_) {
            auto [a,b]=edge_nodes(e);double cap=graph_.edge_capacity(a,b);if(cap<0) continue;
            const double o=std::max(0.0,u-cap);edge_history_[e]+=o/std::max(1.0,cap);
            if(o>0) {overflow=true;for(int i:edge_nets_[e]) priority[i]+=o;}
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
        if(scope.empty()) break;
        bool accepted=rrr_round(scope);
        std::cout<<"[RRR] round="<<round+1<<" accepted="<<accepted<<" guide_overflow="<<best_metrics_.guide.overflow
                 <<" physical_overflow="<<best_metrics_.physical_overflow<<"\n";
        stale=accepted?0:stale+1;if(stale>=3) break;
        if(overflow) present_=std::min(64.0,present_*1.5);
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
                            for(int n:cell_nets_[c]) if(n!=int(move.net)&&n!=move.other) ++neighbors[n];
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
void NegotiatedRouter::validate_complete() const {
    GuideMap all;std::set<int> occupied;
    std::unordered_map<EdgeKey,int> expected_usage;
    for(size_t i=0;i<routes_.size();++i) {
        const auto& r=routes_[i];std::string error;
        for(auto edge:r.edges) {
            auto [a,b]=edge_nodes(edge);
            if(!graph_.legal_edge(a,b)) throw std::logic_error("illegal retained resource edge");
            ++expected_usage[edge];
            auto found=edge_nets_.find(edge);
            if(found==edge_nets_.end()||!found->second.count(int(i))) throw std::logic_error("missing reverse edge owner");
        }
        for(auto [node,count]:r.measured.demand) {
            (void)count;
            if(!cell_nets_[node].count(int(i))) throw std::logic_error("missing reverse Guide owner");
        }
        if(!policy_.validate_mapping(def_.nets()[i],r.guides,r.slot,error)) throw std::runtime_error("TSV_MAPPING_UNRESOLVED: "+error);
        if(r.slot>=0) {
            if(!occupied.insert(r.slot).second) throw std::runtime_error("SLOT_CAPACITY: duplicate assignment");
            for(int conflict:policy_.slots()[r.slot].conflicts) if(occupied.count(conflict))
                throw std::runtime_error("SLOT_CAPACITY: geometric conflict");
        }
        all[def_.nets()[i].name]=r.guides;
    }
    for(auto [edge,count]:usage_) if(expected_usage[edge]!=count) throw std::logic_error("physical ledger rollback mismatch");
    for(auto [edge,count]:expected_usage) {
        auto found=usage_.find(edge);
        if(found==usage_.end()||found->second!=count||edge_nets_.at(edge).size()!=size_t(count))
            throw std::logic_error("physical ledger ownership mismatch");
    }
    std::string error;if(!validate_guides(def_,lef_,all,error)) throw std::runtime_error("HARD_RULE_VIOLATION: "+error);
}
bool NegotiatedRouter::route_all() {
    try {
        const auto assignment=policy_.assign(def_.nets(),thermal_);routes_.resize(def_.nets().size());
        std::vector<size_t> order(routes_.size());std::iota(order.begin(),order.end(),0);
        auto hpwl=[](const Net& n) {Rect b(n.pins[0].coord.x,n.pins[0].coord.y,n.pins[0].coord.x,n.pins[0].coord.y);
            for(const auto& p:n.pins) b=Rect(std::min(b.x1,p.coord.x),std::min(b.y1,p.coord.y),std::max(b.x2,p.coord.x),std::max(b.y2,p.coord.y));
            return b.width()+b.height();};
        std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b) {
            const auto& x=def_.nets()[a];const auto& y=def_.nets()[b];
            if(x.is_cross_die!=y.is_cross_die) return x.is_cross_die>y.is_cross_die;
            if(x.pins.size()!=y.pins.size()) return x.pins.size()>y.pins.size();
            if(hpwl(x)!=hpwl(y)) return hpwl(x)>hpwl(y);
            return x.name<y.name;
        });
        for(size_t i:order) {routes_[i]=build_route(i,assignment[i]);account(i,routes_[i],1);}
        validate_complete();initial_metrics_=best_metrics_=measure();complete_=true;
        std::cout<<"[INITIAL] nets="<<routes_.size()<<" slots="<<policy_.slots().size()<<" guide_overflow="<<best_metrics_.guide.overflow
                 <<" physical_overflow="<<best_metrics_.physical_overflow<<"\n";
        run_rrr(options_.rrr_rounds);optimize_tsv();validate_complete();best_metrics_=measure();
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
               <<",\"ordinary_vias\":"<<m.vias<<"}";
        };
        out<<"{\n\"policy\":\"one aligned TSV per cross net; slot capacity 1; fixed-pin compatible Guide mapping\",\n\"metrics\":";
        metrics(best_metrics_);out<<",\n\"initial_metrics\":";metrics(initial_metrics_);
        out<<",\n\"counters\":{\"expansions\":"<<counters_.expansions<<",\"rrr_attempted\":"<<counters_.rrr_attempted
           <<",\"rrr_accepted\":"<<counters_.rrr_accepted<<",\"rollbacks\":"<<counters_.rollbacks
           <<",\"tsv_trials\":"<<counters_.tsv_trials<<",\"tsv_accepted\":"<<counters_.tsv_accepted
           <<",\"policy_rejections\":"<<counters_.policy_rejections<<",\"local_repairs\":"<<counters_.local_repairs
           <<",\"templates_used\":"<<counters_.templates_used<<"},\n\"assignment\":[";
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
           <<",\"local_repair\":"<<(options_.local_repair?"true":"false")<<",\"search\":"<<json_string(options_.dijkstra?"dijkstra":"astar")
           <<",\"search_limit\":"<<options_.search_limit<<",\"time_limit_seconds\":"<<options_.time_limit<<"},\n";
#if defined(__linux__)
        struct rusage resource_usage{};
        if(getrusage(RUSAGE_SELF,&resource_usage)==0) out<<"\"peak_rss_kb\":"<<resource_usage.ru_maxrss<<",\n";
#endif
        out<<"\"elapsed_seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-started_).count()<<"\n}\n";
        out.close();return bool(out);
    } catch(const std::exception& e) {std::cerr<<"[ERROR] stats: "<<e.what()<<"\n";return false;}
}
}
