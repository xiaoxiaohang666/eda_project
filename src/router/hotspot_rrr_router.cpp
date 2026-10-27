#include "router/negotiated_router.hpp"
#include <cmath>
#include <iostream>
#include <queue>
#include <unordered_set>

namespace eda {
void NegotiatedRouter::run_hotspot_rrr(int rounds) {
    if(!options_.official_model || rounds<=0 || timed_out()) return;
    using Clock=std::chrono::steady_clock;
    const auto started=Clock::now();
    // rrr_round already provides the atomic route/owner/score journal. Reuse
    // its exact W/C acceptance and bounded maze implementation per group.
    const bool saved_incremental=options_.v7_rrr;
    const bool saved_topology_budget=v5_topology_trial_;
    const size_t saved_maze_nets=options_.v7_rrr_astar_net_limit;
    const double saved_maze_seconds=options_.v7_rrr_astar_seconds;
    const size_t saved_scope=v7_scope_limit_;
    struct Restore {
        NegotiatedRouter& router;
        bool incremental,topology_budget;
        size_t maze_nets,scope;
        double maze_seconds;
        Clock::time_point started;
        ~Restore() {
            router.options_.v7_rrr=incremental;
            router.v5_topology_trial_=topology_budget;
            router.options_.v7_rrr_astar_net_limit=maze_nets;
            router.options_.v7_rrr_astar_seconds=maze_seconds;
            router.v7_scope_limit_=scope;
            router.v7_refinement_active_=false;
            const double seconds=std::chrono::duration<double>(Clock::now()-started).count();
            router.counters_.v8_rrr_seconds+=seconds;
            router.counters_.v7_rrr_seconds+=seconds;
        }
    } restore{*this,saved_incremental,saved_topology_budget,saved_maze_nets,saved_scope,saved_maze_seconds,started};
    options_.v7_rrr=true;
    v5_topology_trial_=true;
    const size_t group_limit=std::clamp<size_t>(options_.v8_rrr_group_size,1,256);
    const size_t ceiling=std::max(group_limit,options_.v7_rrr_net_limit);
    // Cost estimates start from net geometry and learn actual group work.
    // This changes both group ordering and the number of affordable maze nets.
    std::vector<double> cost(routes_.size());
    for(size_t i=0;i<routes_.size();++i)
        cost[i]=std::max(0.0001,0.00001*(routes_[i].measured.demand.size()+
            8*def_.nets()[i].pins.size()));
    int stale=0;
    for(int round=0;round<rounds && !timed_out();++round) {
        for(size_t node=0;node<demand_.size();++node) {
            if((node&1023U)==0 && timed_out()) break;
            const double capacity=graph_.official_capacity(int(node));
            const double excess=std::max(0.0,graph_.official_obstacle_demand(int(node))+demand_[node]-capacity);
            guide_history_[node]+=excess/std::max(1.0,capacity);
        }
        if(timed_out()) break;
        struct Entry {
            double payoff;
            size_t net,generation;
            bool operator<(const Entry& other) const {
                if(payoff!=other.payoff) return payoff<other.payoff;
                return net>other.net;
            }
        };
        std::priority_queue<Entry> pending;
        std::vector<double> priority(routes_.size());
        std::vector<size_t> generation(routes_.size());
        std::vector<bool> visited(routes_.size());
        auto refresh=[&](size_t i) {
            priority[i]=official_movable_priority(i);
            ++generation[i];
            if(!visited[i] && priority[i]>0)
                pending.push({priority[i]/cost[i],i,generation[i]});
        };
        for(size_t i=0;i<routes_.size() && !timed_out();++i) refresh(i);
        const size_t growth=size_t(std::min(round+1,8));
        const size_t round_limit=std::min(ceiling,
            std::max(group_limit,std::max<size_t>(1,options_.rrr_net_limit)*growth));
        size_t processed=0,accepted_groups=0;
        while(!pending.empty() && processed<round_limit && !timed_out()) {
            const auto seed=pending.top();pending.pop();
            if(visited[seed.net] || generation[seed.net]!=seed.generation || priority[seed.net]<=0) continue;
            std::vector<size_t> group{seed.net};
            std::unordered_set<size_t> included{seed.net};
            const size_t target=std::min(group_limit,round_limit-processed);
            // Grow a connected component using shared congested cells, rather
            // than combining unrelated hotspots into one rollback scope.
            for(size_t k=0;k<group.size() && group.size()<target && !timed_out();++k) {
                // Each owner enters at most once, even when it shares a long
                // corridor with the seed. Dedup before allocation/sorting so
                // memory scales with neighboring nets rather than cell pairs.
                std::unordered_set<size_t> neighbor_set;
                size_t examined=0;
                for(auto [node,amount]:routes_[group[k]].measured.demand) {
                    (void)amount;
                    if((++examined&255U)==0 && timed_out()) break;
                    const double cap=graph_.official_capacity(node);
                    if(graph_.official_obstacle_demand(node)+demand_[node]<=cap) continue;
                    for(int owner:cell_owners(node)) {
                        if((++examined&255U)==0 && timed_out()) break;
                        const size_t i=size_t(owner);
                        if(!visited[i] && priority[i]>0 && !included.count(i)) neighbor_set.insert(i);
                    }
                    if(timed_out()) break;
                }
                if(timed_out()) break;
                std::vector<size_t> neighbors(neighbor_set.begin(),neighbor_set.end());
                std::sort(neighbors.begin(),neighbors.end(),[&](size_t a,size_t b) {
                    const double lhs=priority[a]/cost[a],rhs=priority[b]/cost[b];
                    return lhs!=rhs?lhs>rhs:a<b;
                });
                for(size_t i:neighbors) {
                    if(included.insert(i).second) group.push_back(i);
                    if(group.size()>=target) break;
                }
            }
            if(timed_out()) break;
            // Shared zero-capacity occupancy has no one-owner marginal relief.
            // Include its other movable owners before rerouting the transaction.
            // Closure may exceed the nominal group size, but remains under the
            // remaining round ceiling. An oversized closure is still safe: exact
            // score acceptance rejects a partial move that fails to help.
            for(size_t k=0;k<group.size() && group.size()<round_limit-processed && !timed_out();++k) {
                size_t examined=0;
                for(auto [node,amount]:routes_[group[k]].measured.demand) {
                    (void)amount;
                    if((++examined&255U)==0 && timed_out()) break;
                    if(graph_.official_capacity(node)>0 || official_zero_capacity_blocked(node)) continue;
                    for(int owner:cell_owners(node)) {
                        if((++examined&255U)==0 && timed_out()) break;
                        const size_t i=size_t(owner);
                        if(!visited[i] && included.insert(i).second) group.push_back(i);
                        if(group.size()>=round_limit-processed) break;
                    }
                    if(group.size()>=round_limit-processed || timed_out()) break;
                }
            }
            if(timed_out()) break;
            std::stable_sort(group.begin(),group.end(),[&](size_t a,size_t b) {
                const double lhs=priority[a]/cost[a],rhs=priority[b]/cost[b];
                return lhs!=rhs?lhs>rhs:a<b;
            });
            std::unordered_set<int> changed_cells;
            for(size_t i:group) {
                if(timed_out()) break;
                visited[i]=true;size_t examined=0;
                for(auto [node,amount]:routes_[i].measured.demand) {
                    (void)amount;if((++examined&255U)==0 && timed_out()) break;changed_cells.insert(node);
                }
            }
            if(timed_out()) break;
            const double remaining=std::max(0.0,std::chrono::duration<double>(deadline_-Clock::now()).count());
            double predicted=0;
            for(size_t i:group) predicted+=cost[i];
            const double per_net=predicted/std::max<size_t>(1,group.size());
            const size_t affordable=size_t(std::max(0.0,std::floor(remaining/std::max(0.0001,per_net+saved_maze_seconds))));
            options_.v7_rrr_astar_net_limit=std::min({saved_maze_nets,group.size(),affordable});
            options_.v7_rrr_astar_seconds=std::min(saved_maze_seconds,
                remaining/std::max<size_t>(1,round_limit-processed));
            v7_scope_limit_=group.size();
            ++counters_.v8_rrr_groups;counters_.v8_rrr_group_nets+=group.size();
            counters_.v7_rrr_peak_scope=std::max(counters_.v7_rrr_peak_scope,group.size());
            const auto group_started=Clock::now();
            const bool accepted=rrr_round(group);
            const double elapsed=std::chrono::duration<double>(Clock::now()-group_started).count();
            const double measured=elapsed/std::max<size_t>(1,group.size());
            for(size_t i:group) cost[i]=0.5*cost[i]+0.5*std::max(0.0001,measured);
            processed+=group.size();
            if(accepted) {++accepted_groups;++counters_.v8_rrr_groups_accepted;}
            else ++counters_.v8_rrr_groups_rejected;
            if(accepted && !timed_out()) {
                for(size_t i:group) {
                    if(timed_out()) break;
                    size_t examined=0;
                    for(auto [node,amount]:routes_[i].measured.demand) {
                        (void)amount;if((++examined&255U)==0 && timed_out()) break;changed_cells.insert(node);
                    }
                }
                std::unordered_set<size_t> affected;
                for(int node:changed_cells) {
                    if(timed_out()) break;
                    for(int owner:cell_owners(node)) if(!visited[size_t(owner)]) affected.insert(size_t(owner));
                }
                // Each next group observes the updated demand ledger. Nets not
                // touching a changed cell retain their exact marginal priority.
                for(size_t i:affected) {if(timed_out()) break;refresh(i);}
            }
        }
        std::cout<<"[RRR-hotspot] round="<<round+1<<" nets="<<processed<<" accepted_groups="<<accepted_groups
                 <<" quality="<<best_metrics_.guide.quality<<'\n';
        stale=accepted_groups?0:stale+1;
        if(!processed || stale>=3) break;
        present_=std::min(64.0,present_*1.5);
    }
    const auto validation_started=Clock::now();
    validate_complete();best_metrics_=measure();verify_official_ledger(best_metrics_);
    counters_.v7_validation_seconds+=std::chrono::duration<double>(Clock::now()-validation_started).count();
}
}
