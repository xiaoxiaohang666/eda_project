#include "router/negotiated_router.hpp"
#include <numeric>
#include <iostream>

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
            std::vector<bool> visited(routes_.size());size_t visited_count=0;
            for(size_t seed:eligible) {
                if(timed_out() || visited_count>=options_.v8_tsv_rematch_nets) break;
                if(visited[seed]) continue;
                std::set<size_t> nearby{seed};
                const size_t radius=std::max(size_t(32),4*group_limit);
                auto gather=[&](const std::vector<size_t>& order,size_t pos) {
                    const size_t first=pos>radius?pos-radius:0,last=std::min(order.size(),pos+radius+1);
                    for(size_t k=first;k<last;++k) if(!visited[order[k]]) nearby.insert(order[k]);
                };
                gather(by_x,xpos[seed]);gather(by_y,ypos[seed]);
                std::vector<size_t> group(nearby.begin(),nearby.end());
                const auto center=slot_point(seed);
                std::stable_sort(group.begin(),group.end(),[&](size_t a,size_t b) {
                    const auto x=slot_point(a),y=slot_point(b);
                    const double da=std::abs(x.x-center.x)+std::abs(x.y-center.y);
                    const double db=std::abs(y.x-center.x)+std::abs(y.y-center.y);
                    return da!=db?da<db:a<b;
                });
                group.resize(std::min({group.size(),group_limit,options_.v8_tsv_rematch_nets-visited_count}));
                for(size_t i:group) {visited[i]=true;++visited_count;}
                if(group.size()<2) continue;
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
                    const double incumbent=mst(group[row],slots[row]);
                    for(size_t col=0;col<n;++col)
                        cost[row][col]=policy_.compatible(def_.nets()[group[row]],slots[col])?
                            originals[row].measured.wirelength+mst(group[row],slots[col])-incumbent:
                            std::numeric_limits<double>::infinity();
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
