#include "router/negotiated_router.hpp"
#include <numeric>
#include <iostream>
#include <unordered_map>

namespace eda {
namespace {
using Clock=std::chrono::steady_clock;
// Keep a phase inside the outer optimization deadline, including candidate
// construction. The active flag makes template/tree work check that deadline.
struct RematchBudget {
    Clock::time_point& deadline;
    bool& active;
    double& seconds;
    Clock::time_point saved,started{Clock::now()};
    bool saved_active;
    RematchBudget(Clock::time_point& d,bool& a,double& s,double limit)
        :deadline(d),active(a),seconds(s),saved(d),saved_active(a) {
        deadline=std::min(saved,started+std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(std::max(0.0,limit-s))));
        active=true;
    }
    ~RematchBudget() {
        seconds+=std::chrono::duration<double>(Clock::now()-started).count();
        deadline=saved;active=saved_active;
    }
};
// Small exact assignment over an already occupied group. Unlike a sequence of
// swaps, subset DP can propose a three-net cycle in a single transaction.
std::vector<int> rematch_assignment(const std::vector<std::vector<double>>& cost) {
    const size_t n=cost.size(),states=size_t(1)<<n;
    const double infinity=std::numeric_limits<double>::infinity();
    std::vector<double> value(states,infinity);
    std::vector<int> chosen(states,-1);
    value[0]=0;
    for(size_t mask=0;mask<states;++mask) {
        if(!std::isfinite(value[mask])) continue;
        size_t row=0;for(size_t bits=mask;bits;bits>>=1) row+=bits&1U;
        if(row==n) continue;
        for(size_t slot=0;slot<n;++slot) if(!(mask&(size_t(1)<<slot))) {
            const auto next=mask|(size_t(1)<<slot);
            const double v=value[mask]+cost[row][slot];
            if(v<value[next]) {value[next]=v;chosen[next]=int(slot);}
        }
    }
    if(!std::isfinite(value.back())) return {};
    std::vector<int> result(n);size_t mask=states-1;
    for(size_t row=n;row;--row) {
        const int slot=chosen[mask];if(slot<0) return {};
        result[row-1]=slot;mask^=size_t(1)<<slot;
    }
    return result;
}
}

void NegotiatedRouter::rematch_tsv_ownership() {
    if(!options_.v8_tsv_rematch || !options_.official_model || timed_out() ||
       options_.v8_tsv_rematch_nets<2 || options_.v8_tsv_rematch_seconds<=counters_.v8_rematch_seconds) return;
    const size_t group_limit=std::clamp(options_.v8_tsv_rematch_group_size,size_t(2),size_t(8));
    const auto baseline=measure();
    std::map<size_t,NetRoute> journal;
    size_t phase_accepted=0;
    try {
        {
            RematchBudget budget(deadline_,v5_topology_trial_,counters_.v8_rematch_seconds,
                                 options_.v8_tsv_rematch_seconds);
            std::vector<size_t> eligible;
            for(size_t i=0;i<routes_.size();++i)
                if(routes_[i].slot>=0 && def_.nets()[i].pins.size()<=128) eligible.push_back(i);
            if(eligible.size()<2) return;
            auto slot_point=[&](size_t i) {return policy_.slots()[routes_[i].slot].point;};
            std::vector<size_t> by_x=eligible,by_y=eligible;
            auto xy=[&](size_t a,size_t b) {
                const auto x=slot_point(a),y=slot_point(b);
                return std::tie(x.x,x.y,a)<std::tie(y.x,y.y,b);
            };
            auto yx=[&](size_t a,size_t b) {
                const auto x=slot_point(a),y=slot_point(b);
                return std::tie(x.y,x.x,a)<std::tie(y.y,y.x,b);
            };
            std::sort(by_x.begin(),by_x.end(),xy);std::sort(by_y.begin(),by_y.end(),yx);
            std::stable_sort(eligible.begin(),eligible.end(),[&](size_t a,size_t b) {
                const double x=routes_[a].measured.wirelength-routes_[a].measured.reference;
                const double y=routes_[b].measured.wirelength-routes_[b].measured.reference;
                return x!=y?x>y:a<b;
            });
            std::vector<size_t> xpos(routes_.size()),ypos(routes_.size());
            for(size_t k=0;k<by_x.size();++k) {xpos[by_x[k]]=k;ypos[by_y[k]]=k;}
            auto rebuild_spatial=[&] {
                std::sort(by_x.begin(),by_x.end(),xy);std::sort(by_y.begin(),by_y.end(),yx);
                for(size_t k=0;k<by_x.size();++k) {xpos[by_x[k]]=k;ypos[by_y[k]]=k;}
            };
            // Cache bounded access estimates only until an accepted transaction
            // changes live Q. Trim the old landing's leaf stem so its zero-cost
            // presence cannot make the current assignment win by construction.
            std::map<size_t,NetRoute> skeletons;
            std::map<std::pair<size_t,int>,double> access_cache;
            const auto skeleton=[&](size_t index)->const NetRoute& {
                auto found=skeletons.find(index);if(found!=skeletons.end()) return found->second;
                NetRoute own=routes_[index];std::map<int,std::set<int>> adjacency;
                for(auto edge:own.edges) {auto [a,b]=edge_nodes(edge);adjacency[a].insert(b);adjacency[b].insert(a);}
                std::set<int> terminals;
                for(const auto& pin:def_.nets()[index].pins) {
                    const int layer=std::clamp(pin.layer_idx,pin.layer_idx>10?11:1,pin.layer_idx>10?20:9);
                    terminals.insert(graph_.node(graph_.grid().get_col(pin.coord.x),graph_.grid().get_row(pin.coord.y),layer));
                }
                const auto& old=policy_.slots()[size_t(own.slot)];
                for(int layer:{9,11}) {
                    int node=graph_.node(graph_.col(old.lower_node),graph_.row(old.lower_node),layer);
                    while(!terminals.count(node) && adjacency[node].size()==1) {
                        const int next=*adjacency[node].begin();own.edges.erase(route_edge(node,next));
                        adjacency[next].erase(node);adjacency.erase(node);own.nodes.erase(node);node=next;
                    }
                }
                return skeletons.emplace(index,std::move(own)).first->second;
            };
            const auto access=[&](size_t index,int slot) {
                const auto key=std::make_pair(index,slot);
                if(auto found=access_cache.find(key);found!=access_cache.end()) return found->second;
                ++counters_.v9_rematch_access_queries;
                const double infinity=std::numeric_limits<double>::infinity();
                if(timed_out() || !policy_.compatible(def_.nets()[index],slot)) return infinity;
                const auto& own=skeleton(index);const auto& landing=policy_.slots()[size_t(slot)];
                const auto saved_deadline=deadline_,saved_refinement_deadline=v7_refinement_deadline_;
                const bool saved_active=v7_refinement_active_;const auto saved_start=v7_refinement_expansion_start_;
                const size_t saved_limit=options_.search_limit,saved_expansions=options_.v7_rrr_astar_expansions;
                struct SearchRestore {std::function<void()> action;~SearchRestore(){action();}} restore{[&] {
                    deadline_=saved_deadline;v7_refinement_deadline_=saved_refinement_deadline;
                    v7_refinement_active_=saved_active;v7_refinement_expansion_start_=saved_start;
                    options_.search_limit=saved_limit;options_.v7_rrr_astar_expansions=saved_expansions;
                }};
                deadline_=std::min(deadline_,Clock::now()+std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(options_.v9_rematch_access_seconds)));
                options_.search_limit=std::min(saved_limit,size_t(4096));
                options_.v7_rrr_astar_expansions=std::min(saved_expansions,size_t(4096));
                v7_refinement_active_=true;v7_refinement_expansion_start_=counters_.expansions;
                v7_refinement_deadline_=deadline_;
                double total=0;
                try {
                    for(int side=0;side<2;++side) {
                        const int low=side?11:1,high=side?20:9;
                        std::vector<std::pair<double,int>> ranked;
                        for(int node:own.nodes) if(graph_.layer(node)>=low && graph_.layer(node)<=high)
                            ranked.emplace_back(graph_.point(node).distance_to(landing.point)+
                                graph_.ell0()*options_.official_metrics.via_cost_ratio*std::abs(graph_.layer(node)-(side?11:9)),node);
                        std::sort(ranked.begin(),ranked.end());std::set<int> targets;
                        for(size_t k=0;k<std::min(size_t(16),ranked.size());++k) targets.insert(ranked[k].second);
                        if(targets.empty()) {total=infinity;break;}
                        auto sources=accesses(landing.point,side?11:10,low,high);
                        const auto path=search(sources,targets,own,low,high,options_.dijkstra);
                        if(path.status!=PathResult::Status::FOUND) {total=infinity;break;}
                        total+=path.cost;
                    }
                } catch(const std::exception&) {total=infinity;}
                access_cache.emplace(key,total);return total;
            };
            std::vector<bool> visited(routes_.size());size_t visited_count=0;
            for(size_t seed:eligible) {
                if(timed_out() || visited_count>=options_.v8_tsv_rematch_nets) break;
                if(visited[seed]) continue;
                std::set<size_t> nearby{seed};
                std::map<size_t,double> group_keys;
                const size_t radius=std::max(size_t(32),4*group_limit);
                auto gather=[&](const std::vector<size_t>& order,size_t pos) {
                    const size_t first=pos>radius?pos-radius:0,last=std::min(order.size(),pos+radius+1);
                    for(size_t k=first;k<last;++k) if(!visited[order[k]] || options_.v9_access_rematch) nearby.insert(order[k]);
                };
                gather(by_x,xpos[seed]);gather(by_y,ypos[seed]);
                const auto local_nearby=nearby;
                if(options_.v9_access_rematch) {
                    std::vector<double> xs,ys;
                    for(const auto& pin:def_.nets()[seed].pins) {xs.push_back(pin.coord.x);ys.push_back(pin.coord.y);}
                    std::sort(xs.begin(),xs.end());std::sort(ys.begin(),ys.end());
                    std::vector<Point> anchors{{xs[xs.size()/2],ys[ys.size()/2]}};
                    std::map<int,int> degrees;
                    for(auto edge:routes_[seed].edges) {auto [a,b]=edge_nodes(edge);++degrees[a];++degrees[b];}
                    std::vector<std::pair<double,int>> branches;
                    for(auto [node,degree]:degrees) if(degree>=3)
                        branches.emplace_back(graph_.point(node).distance_to(anchors.front()),node);
                    std::sort(branches.begin(),branches.end());
                    for(size_t k=0;k<std::min(size_t(3),branches.size());++k) anchors.push_back(graph_.point(branches[k].second));
                    const size_t width=std::clamp(options_.v9_rematch_edges,size_t(2),size_t(32));
                    for(const auto& point:anchors) {
                        auto collect=[&](const std::vector<size_t>& order,bool x_axis) {
                            const double coordinate=x_axis?point.x:point.y;
                            const auto at=std::lower_bound(order.begin(),order.end(),coordinate,[&](size_t i,double value) {
                                const auto p=slot_point(i);return (x_axis?p.x:p.y)<value;
                            });
                            const size_t pos=size_t(at-order.begin()),first=pos>width?pos-width:0,last=std::min(order.size(),pos+width);
                            for(size_t k=first;k<last;++k) if(!visited[order[k]] || options_.v9_access_rematch) nearby.insert(order[k]);
                        };
                        collect(by_x,true);collect(by_y,false);
                    }
                    // Cheap geometric pruning precedes bounded path queries;
                    // actual access differences then decide group membership.
                    std::vector<std::pair<double,size_t>> shortlist;
                    for(size_t i:nearby) if(i!=seed) {
                        double distance=slot_point(i).distance_to(slot_point(seed));
                        for(const auto& anchor:anchors) distance=std::min(distance,slot_point(i).distance_to(anchor));
                        shortlist.emplace_back(distance,i);
                    }
                    std::sort(shortlist.begin(),shortlist.end());
                    if(shortlist.size()>2*width) shortlist.resize(2*width);
                    std::vector<std::pair<double,size_t>> priced;
                    const double own=access(seed,routes_[seed].slot);
                    for(const auto& [distance,i]:shortlist) {
                        (void)distance;if(timed_out()) break;
                        const double a=access(seed,routes_[i].slot),b=access(i,routes_[seed].slot),current=access(i,routes_[i].slot);
                        if(std::isfinite(a)&&std::isfinite(b)&&std::isfinite(own)&&std::isfinite(current)) {
                            const double key=a+b-own-current;
                            if(std::isfinite(key)) {priced.emplace_back(key,i);group_keys.emplace(i,key);}
                        }
                    }
                    std::sort(priced.begin(),priced.end());nearby={seed};
                    for(size_t k=0;k<std::min(width,priced.size());++k) nearby.insert(priced[k].second);
                }
                std::vector<size_t> group(nearby.begin(),nearby.end());
                const auto center=slot_point(seed);
                std::stable_sort(group.begin(),group.end(),[&](size_t a,size_t b) {
                    if(a==seed || b==seed) return a==seed && b!=seed;
                    if(options_.v9_access_rematch) {
                        // Keys were materialized before sorting. Comparators
                        // must never search, mutate caches, or observe deadlines.
                        const double da=group_keys.at(a),db=group_keys.at(b);
                        return da!=db?da<db:a<b;
                    }
                    const auto x=slot_point(a),y=slot_point(b);
                    const double da=std::abs(x.x-center.x)+std::abs(x.y-center.y);
                    const double db=std::abs(y.x-center.x)+std::abs(y.y-center.y);
                    return da!=db?da<db:a<b;
                });
                group.resize(std::min({group.size(),group_limit,options_.v8_tsv_rematch_nets-visited_count}));
                // A net already considered as a seed may re-enter a later V9
                // group when another seed exposes a useful exchange relation.
                // Count participation against the bounded phase work budget.
                for(size_t i:group) {visited[i]=true;++visited_count;}
                if(group.size()<2) continue;
                if(options_.v9_access_rematch && std::any_of(group.begin(),group.end(),[&](size_t i){return !local_nearby.count(i);}))
                    ++counters_.v9_rematch_cross_window_groups;
                ++counters_.v8_rematch_groups;
                std::vector<NetRoute> originals;std::vector<int> slots;
                for(size_t i:group) {originals.push_back(routes_[i]);slots.push_back(routes_[i].slot);}
                const size_t n=group.size();
                // A per-die rectilinear MST is only a shortlist estimate. Use
                // actual encoded length as each row's incumbent cost; all final
                // decisions below rebuild both dies and use the exact ledger.
                auto mst=[&](size_t index,int slot) {
                    double total=0;
                    for(int side=0;side<2;++side) {
                        std::vector<Point> points{policy_.slots()[slot].point};
                        for(const auto& pin:def_.nets()[index].pins)
                            if((pin.layer_idx>10)==bool(side)) points.push_back(pin.coord);
                        std::vector<double> dist(points.size(),std::numeric_limits<double>::infinity());
                        std::vector<bool> done(points.size());dist[0]=0;
                        for(size_t step=0;step<points.size();++step) {
                            size_t v=points.size();
                            for(size_t k=0;k<points.size();++k) if(!done[k] && (v==points.size()||dist[k]<dist[v])) v=k;
                            total+=dist[v];done[v]=true;
                            for(size_t k=0;k<points.size();++k) if(!done[k])
                                dist[k]=std::min(dist[k],std::abs(points[k].x-points[v].x)+std::abs(points[k].y-points[v].y));
                        }
                    }
                    return total;
                };
                std::vector<std::vector<double>> cost(n,std::vector<double>(n));
                for(size_t row=0;row<n;++row) {
                    const double incumbent=options_.v9_access_rematch?access(group[row],slots[row]):mst(group[row],slots[row]);
                    for(size_t col=0;col<n;++col) {
                        if(!policy_.compatible(def_.nets()[group[row]],slots[col])) {
                            cost[row][col]=std::numeric_limits<double>::infinity();continue;
                        }
                        if(options_.v9_access_rematch) {
                            const double proposed=access(group[row],slots[col]);
                            cost[row][col]=std::isfinite(incumbent)&&std::isfinite(proposed)?proposed-incumbent:
                                std::numeric_limits<double>::infinity();
                        } else cost[row][col]=originals[row].measured.wirelength+mst(group[row],slots[col])-incumbent;
                    }
                }
                std::vector<int> identity(n);std::iota(identity.begin(),identity.end(),0);
                std::map<std::vector<int>,double> unique;
                auto propose=[&](const std::vector<int>& permutation) {
                    if(permutation.size()!=n || permutation==identity) return;
                    double score=0;for(size_t row=0;row<n;++row) score+=cost[row][permutation[row]];
                    if(std::isfinite(score)) unique.emplace(permutation,score);
                };
                propose(rematch_assignment(cost));
                for(size_t a=0;a<n;++a) for(size_t b=a+1;b<n;++b) {
                    auto p=identity;std::swap(p[a],p[b]);propose(p);
                }
                for(size_t a=0;a<n;++a) for(size_t b=a+1;b<n;++b) for(size_t c=b+1;c<n;++c) {
                    auto p=identity;p[a]=int(b);p[b]=int(c);p[c]=int(a);propose(p);
                    p[a]=int(c);p[b]=int(a);p[c]=int(b);propose(p);
                }
                std::vector<std::pair<double,std::vector<int>>> candidates;
                for(auto& [permutation,value]:unique) candidates.emplace_back(value,permutation);
                std::sort(candidates.begin(),candidates.end());
                if(candidates.size()>16) candidates.resize(16);
                for(const auto& [proxy,permutation]:candidates) {
                    (void)proxy;
                    if(timed_out()) break;
                    ++counters_.v8_rematch_candidates;
                    const auto before=recovery_metrics(best_metrics_);
                    std::vector<bool> removed(n),inserted(n);
                    auto rollback=[&] {
                        for(size_t row=0;row<n;++row) if(inserted[row]) {
                            account(group[row],routes_[group[row]],-1);inserted[row]=false;
                        }
                        for(size_t row=0;row<n;++row) if(removed[row]) {
                            routes_[group[row]]=originals[row];account(group[row],routes_[group[row]],1);removed[row]=false;
                        }
                        best_metrics_=before;++counters_.v8_rematch_rollbacks;
                    };
                    try {
                        for(size_t row=0;row<n;++row) {
                            account(group[row],routes_[group[row]],-1);removed[row]=true;routes_[group[row]]=NetRoute{};
                        }
                        // Every affected net is removed before any replacement
                        // is routed, so congestion negotiation is joint.
                        for(size_t row=0;row<n;++row) {
                            if(timed_out()) throw std::runtime_error("TSV rematch deadline");
                            auto route=build_route(group[row],slots[permutation[row]]);
                            std::string error;
                            if(route.slot!=slots[permutation[row]] || route.measured.tsv_cells.size()!=1 ||
                               !policy_.validate_mapping(def_.nets()[group[row]],route.guides,route.slot,error) ||
                               !validate_route(group[row],route.guides,error))
                                throw std::runtime_error("TSV rematch invalid encoded route: "+error);
                            routes_[group[row]]=std::move(route);account(group[row],routes_[group[row]],1);inserted[row]=true;
                        }
                        auto after=recovery_metrics(before);
                        for(size_t row=0;row<n;++row) {
                            after.physical_length+=routes_[group[row]].length-originals[row].length;
                            after.vias=after.vias-originals[row].vias+routes_[group[row]].vias;
                        }
                        if(!timed_out() && after.official.zero_capacity_cells<=before.official.zero_capacity_cells &&
                           after.guide.quality>before.guide.quality+1e-12) {
                            for(size_t row=0;row<n;++row) journal.try_emplace(group[row],originals[row]);
                            best_metrics_=after;++phase_accepted;++counters_.v8_rematch_accepted;
                            if(options_.v9_access_rematch) {skeletons.clear();access_cache.clear();}
                            // Swap commits change owner coordinates. Restore
                            // ordering and positions before any lower_bound or
                            // spatial-neighbor query for the next group.
                            rebuild_spatial();
                            break;
                        }
                        rollback();
                    } catch(const std::exception& e) {
                        rollback();++counters_.v8_rematch_failures;
                        ++rejection_reasons_[std::string("TSV rematch candidate: ")+e.what()];
                    }
                }
            }
        }
        complete_validation_cached_=false;validate_complete();
        const auto exact=measure();verify_official_ledger(exact);
        if(std::abs(exact.official.thermal_score-baseline.official.thermal_score)>1e-9 ||
           std::abs(exact.guide.quality-best_metrics_.guide.quality)>1e-9 ||
           exact.guide.quality+1e-10<baseline.guide.quality)
            throw std::logic_error("TSV rematch thermal/quality checkpoint mismatch");
        best_metrics_=exact;
    } catch(const std::exception& e) {
        // Phase validation protects accepted candidates as well as search work.
        for(auto& [i,old]:journal) account(i,routes_[i],-1);
        for(auto& [i,old]:journal) {routes_[i]=std::move(old);account(i,routes_[i],1);}
        counters_.v8_rematch_accepted-=std::min(counters_.v8_rematch_accepted,phase_accepted);
        ++counters_.v8_rematch_rollbacks;++counters_.v8_rematch_failures;
        ++rejection_reasons_[std::string("TSV rematch phase restored: ")+e.what()];
        complete_validation_cached_=false;validate_complete();best_metrics_=measure();verify_official_ledger(best_metrics_);
        if(std::abs(best_metrics_.guide.quality-baseline.guide.quality)>1e-9)
            throw std::logic_error("TSV rematch rollback did not restore phase baseline");
    }
    std::cout<<"[TSV_REMATCH] groups="<<counters_.v8_rematch_groups<<" candidates="<<counters_.v8_rematch_candidates
             <<" accepted="<<counters_.v8_rematch_accepted<<" S="<<100*best_metrics_.guide.quality<<'\n';
}
} // namespace eda
