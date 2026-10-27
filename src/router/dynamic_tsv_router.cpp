#include "router/negotiated_router.hpp"
#include <iostream>
#include <queue>
#include <unordered_set>

namespace eda {
namespace detail {
// The broad raw pool must survive until live demand has been priced. Asking
// the geometry/thermal index for only the final count can permanently hide a
// slightly weaker geometric candidate with much better actual landing access.
std::vector<TsvMoveCandidate> dynamic_tsv_candidates(const TsvMoveSearch& proposals,
        const RoutingGraph& graph,const TsvPolicy& policy,const std::vector<int>& demand,
        size_t index,int old_slot,double squared_overflow,TsvMoveSearchOptions options,bool probe) {
    const size_t final_limit=options.max_candidates;
    options.max_candidates=probe?std::min(size_t(8),2*final_limit):std::min(size_t(64),4*final_limit);
    if(probe) options.max_pins=32;
    auto candidates=proposals.candidates(index,options);
    const auto landing_q=[&](int slot) {
        const auto& s=policy.slots().at(size_t(slot));
        double risk=0;
        for(int node:{s.lower_node,s.upper_node,graph.node(graph.col(s.lower_node),graph.row(s.lower_node),9)}) {
            const double capacity=graph.official_capacity(node);
            if(capacity<=0) {risk+=10000;continue;}
            const double total=demand.at(size_t(node))+graph.official_obstacle_demand(node);
            const double before=0.1*std::max(0.0,total-int(capacity))/capacity;
            const double after=0.1*std::max(0.0,total+1-int(capacity))/capacity;
            risk+=after*after-before*before;
        }
        return risk;
    };
    const double price=15/std::pow(1+squared_overflow,1.5),old=landing_q(old_slot);
    for(auto& candidate:candidates)
        candidate.estimated_score_delta-=price*(landing_q(candidate.slot)-old);
    std::stable_sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b) {
        if(a.estimated_score_delta!=b.estimated_score_delta) return a.estimated_score_delta>b.estimated_score_delta;
        return a.slot<b.slot;
    });
    if(candidates.size()>final_limit) candidates.resize(final_limit);
    return candidates;
}
}
void NegotiatedRouter::optimize_dynamic_tsv() {
    if(!options_.v8_tsv_dynamic || options_.tsv_rounds<=0 || timed_out()) return;
    if(!options_.official_model || !options_.official_metrics.score_20261020 ||
       options_.tsv_policy.resource_mode!=TsvResourceMode::GLOBAL_GRID)
        throw std::invalid_argument("dynamic TSV scheduling requires official 20261020 global-grid routing");
    using Clock=std::chrono::steady_clock;
    const auto started=Clock::now(),outer_deadline=deadline_;
    const bool outer_trial=v5_topology_trial_;
    const auto baseline=measure();
    std::map<size_t,NetRoute> journal;
    const size_t accepted_start=counters_.v7_tsv_accepted;
    const auto queue_gain_start=counters_.v8_tsv_queue_gain;
    struct Restore {
        std::function<void()> action;
        ~Restore(){action();}
    } restore{[&] {
        deadline_=outer_deadline;v5_topology_trial_=outer_trial;
        counters_.v7_tsv_seconds+=std::chrono::duration<double>(Clock::now()-started).count();
    }};
    deadline_=std::min(outer_deadline,started+std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(options_.v7_tsv_seconds)));
    v5_topology_trial_=true;
    if(timed_out()) {++counters_.v7_tsv_budget_stops;best_metrics_=baseline;return;}
    try {
        std::vector<int> assignment;
        std::vector<size_t> scope;
        for(size_t i=0;i<routes_.size();++i) {
            assignment.push_back(routes_[i].slot);
            if(routes_[i].slot>=0) scope.push_back(i);
        }
        TsvMoveSearch proposals(def_,graph_,policy_,thermal_,assignment);
        if(std::abs(proposals.current_correlation()-baseline.guide.correlation)>1e-9)
            throw std::logic_error("dynamic TSV initial occupancy differs from encoded Guide");
        if(scope.empty()) {best_metrics_=baseline;return;}

        // All estimates below use weighted 0..100 quality units. The landing
        // proxy sees live normalized Q, including fixed obstacles. It is only
        // a shortlist filter: complete encoded W/C/T decides every commit.
        const auto settings=[&](size_t candidates) {
            const auto current=recovery_metrics(best_metrics_);
            TsvMoveSearchOptions o;o.max_candidates=candidates;
            o.thermal_gain=options_.official_metrics.thermal_gain;
            o.length_price=current.guide.wirelength>current.guide.reference?
                45.0*current.guide.reference/(current.guide.wirelength*current.guide.wirelength):0;
            o.risk_price=0.15/std::pow(1+current.official.squared_normalized_overflow,1.5);
            return o;
        };
        const auto query=[&](size_t i,TsvMoveSearchOptions options,bool cheap) {
            const auto current=recovery_metrics(best_metrics_);
            return detail::dynamic_tsv_candidates(proposals,graph_,policy_,demand_,i,routes_[i].slot,
                current.official.squared_normalized_overflow,options,cheap);
        };
        struct Estimate {std::array<double,3> value{};double work{1};};
        std::vector<Estimate> estimates(routes_.size());
        std::vector<double> measured_work(routes_.size(),0);
        const auto probe=[&](size_t i,bool initial) {
            const auto begin=Clock::now();
            auto o=settings(options_.v8_tsv_probe_candidates);
            // The all-net pass must not spend quadratic MST work on a few
            // high-fanout nets. Those still receive the cached W/Q estimates
            // and remain eligible for the full bounded query on selection.
            auto candidates=query(i,o,true);
            ++counters_.v8_tsv_probe_queries;
            if(initial) ++counters_.v8_tsv_probed_nets;
            const double excess=std::max(0.0,routes_[i].measured.wirelength-routes_[i].measured.reference);
            const double qprice=15/std::pow(1+best_metrics_.official.squared_normalized_overflow,1.5);
            Estimate e;
            // Fanout is a deterministic initial runtime proxy; real elapsed
            // full-query/routing time replaces it after the net is visited.
            const double pins=double(def_.nets()[i].pins.size());
            e.work=measured_work[i]>0?measured_work[i]:1e-3*(1+pins*pins/16);
            e.value[0]=o.length_price*excess;
            e.value[2]=qprice*official_movable_priority(i);
            for(const auto& c:candidates) {
                e.value[0]=std::max(e.value[0],-o.length_price*c.proxy_length_delta_dbu);
                e.value[1]=std::max(e.value[1],c.thermal_score_delta-o.length_price*std::max(0.0,c.proxy_length_delta_dbu));
                e.value[2]=std::max(e.value[2],c.estimated_score_delta-c.thermal_score_delta);
            }
            estimates[i]=e;
            counters_.v8_tsv_probe_seconds+=std::chrono::duration<double>(Clock::now()-begin).count();
        };
        // There is no top-N truncation here. Every assigned net receives a
        // small spatial/thermal query unless the shared deadline expires.
        for(size_t i:scope) {if(timed_out()) break;probe(i,true);}

        struct Entry {
            double priority;
            size_t net;
            bool operator<(const Entry& other) const {
                return priority!=other.priority?priority<other.priority:net>other.net;
            }
        };
        std::array<std::priority_queue<Entry>,3> queues;
        std::array<double,3> recent_gain{},recent_seconds{};
        std::array<size_t,3> local_visits{};
        std::vector<unsigned char> visited(routes_.size(),0),ever_visited(routes_.size(),0);
        size_t visited_count=0,explore_cursor=0,refresh_cursor=0,picks=0;
        const auto rebuild=[&] {
            for(auto& queue:queues) queue={};
            for(size_t i:scope) if(!visited[i]) for(size_t q=0;q<3;++q)
                queues[q].push({estimates[i].value[q]/estimates[i].work,i});
        };
        rebuild();
        const auto choose_queue=[&] {
            // Each queue receives an initial measurement. Thereafter use
            // recent measured delta(S)/second with a small exploration term.
            for(size_t q=0;q<3;++q) if(!local_visits[q]) return q;
            size_t best=0;double best_rate=-1;
            for(size_t q=0;q<3;++q) {
                const double rate=recent_gain[q]/std::max(1e-6,recent_seconds[q]);
                const double explore=1e-4/std::sqrt(double(local_visits[q]));
                if(rate+explore>best_rate) {best=q;best_rate=rate+explore;}
            }
            return best;
        };
        for(int batch=0;batch<options_.tsv_rounds && !timed_out();++batch) {
            size_t batch_accepted=0;
            std::set<size_t> batch_moved;
            std::unordered_set<int> batch_cells;
            for(size_t n=0;n<options_.v7_tsv_nets && !timed_out();++n) {
                if(visited_count==scope.size()) {
                    std::fill(visited.begin(),visited.end(),0);visited_count=0;rebuild();
                }
                const size_t q=choose_queue();size_t i=routes_.size();
                // A reserved rotation prevents a strong early queue from
                // permanently hiding cheap moves on previously unseen nets.
                if(picks++%4==3) {
                    for(size_t k=0;k<scope.size();++k) {
                        const size_t candidate=scope[explore_cursor++%scope.size()];
                        if(!visited[candidate]) {i=candidate;break;}
                    }
                } else {
                    auto& queue=queues[q];
                    while(!queue.empty()&&visited[queue.top().net]) queue.pop();
                    if(!queue.empty()) {i=queue.top().net;queue.pop();}
                }
                if(i==routes_.size()) break;
                visited[i]=1;++visited_count;
                if(ever_visited[i]) ++counters_.v8_tsv_repeat_visits;
                else {ever_visited[i]=1;++counters_.v8_tsv_unique_nets;}
                ++counters_.v8_tsv_queue_visits[q];
                ++local_visits[q];
                const auto visit_started=Clock::now();const double quality_before=best_metrics_.guide.quality;
                auto candidates=query(i,settings(options_.v7_tsv_candidates),false);
                counters_.v7_tsv_candidates+=candidates.size();
                for(const auto& candidate:candidates) {
                    if(timed_out()) break;
                    ++counters_.v7_tsv_attempted;
                    const auto before=recovery_metrics(best_metrics_);NetRoute old=routes_[i];
                    bool removed=false,inserted=false;
                    const auto rollback=[&] {
                        if(inserted) account(i,routes_[i],-1);
                        if(removed) {routes_[i]=old;account(i,old,1);}
                        inserted=removed=false;best_metrics_=before;++counters_.v7_tsv_rollbacks;
                    };
                    try {
                        account(i,old,-1);removed=true;routes_[i]=NetRoute{};
                        auto trial=build_route(i,candidate.slot);
                        std::string error;
                        if(trial.measured.tsv_cells.size()!=1 ||
                           !policy_.validate_mapping(def_.nets()[i],trial.guides,trial.slot,error) ||
                           !validate_route(i,trial.guides,error))
                            throw std::logic_error("dynamic TSV invalid encoded candidate: "+error);
                        routes_[i]=std::move(trial);account(i,routes_[i],1);inserted=true;
                        auto after=recovery_metrics(before);
                        after.physical_length+=routes_[i].length-old.length;
                        after.vias=after.vias-old.vias+routes_[i].vias;
                        after.guide.correlation=candidate.correlation_after;
                        after.official.thermal_score=50*(1+std::tanh(options_.official_metrics.thermal_gain*after.guide.correlation));
                        after.guide.quality=(.45*after.official.wirelength_score+.30*after.official.congestion_score+.25*after.official.thermal_score)/100;
                        after.official.guide=after.guide;
                        if(!timed_out() && after.official.zero_capacity_cells<=before.official.zero_capacity_cells &&
                           after.guide.quality>before.guide.quality+1e-12) {
                            journal.try_emplace(i,old);
                            // Collect invalidated routing footprints before
                            // mutating proposal occupancy, so allocation errors
                            // still leave a transaction that can be rolled back.
                            batch_moved.insert(i);
                            for(auto [node,amount]:old.measured.demand) {(void)amount;batch_cells.insert(node);}
                            for(auto [node,amount]:routes_[i].measured.demand) {(void)amount;batch_cells.insert(node);}
                            if(!proposals.commit_move(i,candidate.slot)) throw std::logic_error("dynamic TSV occupancy commit rejected");
                            removed=inserted=false;best_metrics_=after;
                            ++counters_.v7_tsv_accepted;++batch_accepted;break;
                        }
                        rollback();
                    } catch(const std::exception& e) {
                        rollback();++counters_.v7_tsv_failures;
                        ++rejection_reasons_[std::string("dynamic TSV trial: ")+e.what()];
                    }
                }
                const double elapsed=std::chrono::duration<double>(Clock::now()-visit_started).count();
                const double gain=100*std::max(0.0,best_metrics_.guide.quality-quality_before);
                counters_.v8_tsv_queue_seconds[q]+=elapsed;
                counters_.v8_tsv_queue_gain[q]+=gain;
                recent_gain[q]=.75*recent_gain[q]+.25*gain;
                recent_seconds[q]=.75*recent_seconds[q]+.25*elapsed;
                measured_work[i]=measured_work[i]>0?.75*measured_work[i]+.25*elapsed:elapsed;
                estimates[i].work=std::max(1e-6,measured_work[i]);
            }
            // Refresh moved nets and current TSV owners touching either full
            // footprint, including cells released by a move. A bounded rotating
            // sample also sees global thermal/free-slot changes. Other queue
            // keys remain cached estimates; every selected net gets a fresh
            // candidate query against current occupancy and live demand.
            if(batch_accepted && !timed_out()) {
                auto affected=batch_moved;
                for(int node:batch_cells) {
                    if(timed_out()) break;
                    for(int owner:cell_owners(node)) if(routes_[size_t(owner)].slot>=0) affected.insert(size_t(owner));
                }
                const size_t rotating_limit=std::clamp(options_.v7_tsv_nets,size_t(32),size_t(128));
                size_t rotating=0;
                for(size_t k=0;k<scope.size() && rotating<rotating_limit && !timed_out();++k) {
                    const size_t i=scope[refresh_cursor++%scope.size()];
                    if(!visited[i]) {affected.insert(i);++rotating;}
                }
                for(size_t i:affected) if(routes_[i].slot>=0 && !timed_out()) {probe(i,false);++counters_.v8_tsv_refreshes;}
                rebuild();
            }
            std::cout<<"[dynamic TSV] batch="<<batch+1<<" accepted="<<batch_accepted
                     <<" unique="<<counters_.v8_tsv_unique_nets<<" repeated="<<counters_.v8_tsv_repeat_visits
                     <<" quality="<<best_metrics_.guide.quality*100<<'\n';
        }
        if(timed_out()) ++counters_.v7_tsv_budget_stops;
        complete_validation_cached_=false;validate_complete();
        const auto exact=measure();verify_official_ledger(exact);
        if(std::abs(exact.guide.correlation-best_metrics_.guide.correlation)>1e-9 ||
           std::abs(exact.guide.quality-best_metrics_.guide.quality)>1e-9 ||
           exact.guide.quality+1e-10<baseline.guide.quality ||
           exact.official.zero_capacity_cells>baseline.official.zero_capacity_cells)
            throw std::logic_error("dynamic TSV final exact checkpoint mismatch");
        best_metrics_=exact;
    } catch(const std::exception& e) {
        for(auto& [i,old]:journal) {account(i,routes_[i],-1);routes_[i]=std::move(old);account(i,routes_[i],1);}
        counters_.v7_tsv_accepted=accepted_start;counters_.v8_tsv_queue_gain=queue_gain_start;
        ++counters_.v7_checkpoint_restores;
        ++rejection_reasons_[std::string("dynamic TSV phase restored: ")+e.what()];
        complete_validation_cached_=false;validate_complete();best_metrics_=measure();verify_official_ledger(best_metrics_);
        if(std::abs(best_metrics_.guide.quality-baseline.guide.quality)>1e-9)
            throw std::logic_error("dynamic TSV rollback did not restore phase baseline");
    }
}
} // namespace eda
