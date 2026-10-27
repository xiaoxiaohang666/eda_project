#include "router/negotiated_router.hpp"
#include <numeric>

namespace eda {
// Fixed TSVs make T invariant. Reconstruct complete W/C from the transactional
// exact Guide ledger without scanning the graph or measuring other nets.
CompleteMetrics NegotiatedRouter::recovery_metrics(const CompleteMetrics& invariant) const {
    auto m=invariant;
    m.guide.wirelength=double(official_wirelength_);
    m.guide.reference=double(official_reference_);
    m.guide.normalized=m.guide.reference>0?m.guide.wirelength/m.guide.reference:100;
    m.guide.overflow=int(official_overflow_);
    m.official.squared_normalized_overflow=double(std::max(0.0L,official_squared_overflow_));
    m.official.zero_capacity_cells=official_zero_capacity_cells_;
    m.official.wirelength_score=official_wirelength_score(m.guide.wirelength,m.guide.reference,options_.official_metrics);
    m.official.congestion_score=100/std::sqrt(1+m.official.squared_normalized_overflow);
    m.guide.quality=(.45*m.official.wirelength_score+.30*m.official.congestion_score+.25*m.official.thermal_score)/100;
    m.official.guide=m.guide;
    return m;
}

std::vector<size_t> NegotiatedRouter::recovery_selection() const {
    std::vector<size_t> positive,zero;
    for(size_t i=0;i<routes_.size();++i) {
        const auto& m=routes_[i].measured;
        if(m.reference>0) {if(m.wirelength>m.reference) positive.push_back(i);}
        else if(m.wirelength>0) zero.push_back(i);
    }
    auto rank=[&](size_t a,size_t b) {
        const double x=std::max(0.0,routes_[a].measured.wirelength-routes_[a].measured.reference);
        const double y=std::max(0.0,routes_[b].measured.wirelength-routes_[b].measured.reference);
        return x!=y?x>y:def_.nets()[a].name<def_.nets()[b].name;
    };
    std::sort(positive.begin(),positive.end(),rank);std::sort(zero.begin(),zero.end(),rank);
    // Zero-reference nets have no meaningful ratio: a small separate tail
    // budget prevents them from displacing the entire excess-length shortlist.
    const size_t z=std::min({zero.size(),size_t(32),std::max(size_t(1),options_.large_recovery_nets/16)});
    positive.resize(std::min(positive.size(),options_.large_recovery_nets-z));
    const size_t take=std::min(z,options_.large_recovery_nets-positive.size());
    positive.insert(positive.end(),zero.begin(),zero.begin()+take);return positive;
}

bool NegotiatedRouter::recovery_transaction(size_t index,
        const std::function<std::optional<NetRoute>(const NetRoute&,const CompleteMetrics&)>& propose) {
    const auto old=routes_.at(index);const auto before=recovery_metrics(best_metrics_);
    account(index,old,-1);routes_[index]=NetRoute{};
    bool installed=false;
    auto restore=[&] {
        if(installed) account(index,routes_[index],-1);
        routes_[index]=old;account(index,old,1);best_metrics_=before;
        ++recovery_stats_.rejected;
    };
    try {
        const auto base=official_candidate_base();auto proposed=propose(old,base);
        if(!proposed) {restore();return false;}
        auto trial=std::move(*proposed);
        std::string error;
        if(trial.slot!=old.slot || !policy_.validate_mapping(def_.nets()[index],trial.guides,trial.slot,error))
            throw std::runtime_error("budgeted recovery changed TSV mapping: "+error);
        if(!validate_route(index,trial.guides,error))
            throw std::runtime_error("budgeted recovery invalid net: "+error);
        // The serialized rectangles, not the search proxy or caller's cached
        // metrics, are the acceptance authority.
        trial.measured=measure_official_net_guides(def_.nets()[index],trial.guides,graph_,lef_,
            options_.official_metrics,reference_lengths_[index]);
        if(trial.measured.tsv_cells!=old.measured.tsv_cells || trial.measured.tsv_points.size()!=old.measured.tsv_points.size())
            throw std::runtime_error("budgeted recovery changed selected TSV");
        for(size_t k=0;k<trial.measured.tsv_points.size();++k)
            if(trial.measured.tsv_points[k].distance_to(old.measured.tsv_points[k])!=0)
                throw std::runtime_error("budgeted recovery changed selected TSV point");
        for(int n:trial.nodes) if(!graph_.legal_node(n)) throw std::runtime_error("budgeted recovery illegal node");
        for(auto e:trial.edges) {auto [a,b]=edge_nodes(e);
            if(!trial.nodes.count(a)||!trial.nodes.count(b)||!graph_.legal_edge(a,b))
                throw std::runtime_error("budgeted recovery illegal edge");
        }
        if(trial.measured.wirelength>=old.measured.wirelength ||
           !(official_candidate_key(trial,base)<official_candidate_key(old,base))) {restore();return false;}
        routes_[index]=std::move(trial);account(index,routes_[index],1);installed=true;
        auto after=recovery_metrics(before);
        if(after.official.zero_capacity_cells>before.official.zero_capacity_cells ||
           after.guide.quality<=before.guide.quality+1e-12) {restore();return false;}
        after.physical_length+=routes_[index].length-old.length;
        after.vias=after.vias-old.vias+routes_[index].vias;
        best_metrics_=after;++recovery_stats_.accepted;return true;
    } catch(const std::exception& e) {
        ++recovery_stats_.candidate_failures;++rejection_reasons_[e.what()];restore();return false;
    }
}

bool NegotiatedRouter::recovery_checkpoint(std::vector<std::pair<size_t,NetRoute>>& journal,CompleteMetrics& checkpoint) {
    try {
        std::set<size_t> changed;
        for(const auto& [index,old]:journal) {
            (void)old;
            if(!changed.insert(index).second) continue;
            const auto& r=routes_[index];
            const auto actual=measure_official_net_guides(def_.nets()[index],r.guides,graph_,lef_,
                options_.official_metrics,reference_lengths_[index]);
            if(actual.wirelength!=r.measured.wirelength || actual.reference!=r.measured.reference ||
               actual.demand!=r.measured.demand || actual.tsv_cells!=r.measured.tsv_cells ||
               actual.tsv_points.size()!=r.measured.tsv_points.size())
                throw std::runtime_error("budgeted recovery Guide/cache checkpoint mismatch");
            for(size_t k=0;k<actual.tsv_points.size();++k)
                if(actual.tsv_points[k].distance_to(r.measured.tsv_points[k])!=0)
                    throw std::runtime_error("budgeted recovery selected TSV/cache checkpoint mismatch");
        }
        // Do not reuse a generation proof when testing the checkpoint: this
        // also checks geometry and all reverse ownership after the batch.
        complete_validation_cached_=false;validate_complete();
        const auto predicted=recovery_metrics(best_metrics_);const auto full=measure();
        auto same=[](double a,double b) {return std::isfinite(a)&&std::isfinite(b)&&
            std::abs(a-b)<=2e-10*std::max({1.0,std::abs(a),std::abs(b)});};
        if(!same(predicted.guide.wirelength,full.guide.wirelength)||!same(predicted.guide.reference,full.guide.reference)||
           !same(predicted.guide.quality,full.guide.quality)||
           !same(predicted.official.squared_normalized_overflow,full.official.squared_normalized_overflow)||
           predicted.guide.overflow!=full.guide.overflow ||
           predicted.official.zero_capacity_cells!=full.official.zero_capacity_cells ||
           full.guide.quality+1e-12<checkpoint.guide.quality || full.guide.wirelength>checkpoint.guide.wirelength ||
           full.official.zero_capacity_cells>checkpoint.official.zero_capacity_cells ||
           !same(full.official.thermal_score,checkpoint.official.thermal_score))
            throw std::runtime_error("budgeted recovery exact checkpoint score mismatch/regression");
        best_metrics_=checkpoint=full;journal.clear();++recovery_stats_.checkpoints;return true;
    } catch(const std::exception& e) {
        ++rejection_reasons_[std::string("budgeted recovery checkpoint: ")+e.what()];
        const size_t undone=journal.size();
        for(auto it=journal.rbegin();it!=journal.rend();++it) {
            const size_t i=it->first;account(i,routes_[i],-1);routes_[i]=std::move(it->second);account(i,routes_[i],1);
        }
        journal.clear();recovery_stats_.accepted-=std::min(recovery_stats_.accepted,undone);
        ++recovery_stats_.checkpoint_rollbacks;
        // Restore the validated aggregate as well as route/demand/ownership
        // transactions. A detected score-cache drift must not survive rollback.
        official_wirelength_=checkpoint.guide.wirelength;official_reference_=checkpoint.guide.reference;
        official_squared_overflow_=checkpoint.official.squared_normalized_overflow;
        official_overflow_=checkpoint.guide.overflow;official_zero_capacity_cells_=checkpoint.official.zero_capacity_cells;
        ++state_generation_;complete_validation_cached_=false;validate_complete();best_metrics_=measure();
        if(std::abs(best_metrics_.guide.quality-checkpoint.guide.quality)>2e-10 ||
           best_metrics_.guide.wirelength!=checkpoint.guide.wirelength ||
           best_metrics_.guide.reference!=checkpoint.guide.reference ||
           best_metrics_.official.zero_capacity_cells!=checkpoint.official.zero_capacity_cells)
            throw std::runtime_error("budgeted recovery checkpoint rollback did not restore validated score");
        checkpoint=best_metrics_;
        return false;
    }
}

std::vector<std::pair<int,int>> NegotiatedRouter::recovery_seeds(size_t index,const NetRoute& old) const {
    std::vector<std::pair<int,int>> result;
    std::map<int,int> degree;for(auto e:old.edges) {auto [a,b]=edge_nodes(e);++degree[a];++degree[b];}
    for(int side=0;side<2;++side) {
        std::vector<int> cols,rows,layers;
        for(const auto& p:def_.nets()[index].pins) if((p.layer_idx>10)==bool(side)) {
            cols.push_back(graph_.grid().get_col(p.coord.x));rows.push_back(graph_.grid().get_row(p.coord.y));
            layers.push_back(std::clamp(p.layer_idx,side?11:1,side?20:(old.slot>=0?9:10)));
        }
        if(old.slot>=0 && !cols.empty()) {
            const auto p=policy_.slots()[old.slot].point;cols.push_back(graph_.grid().get_col(p.x));
            rows.push_back(graph_.grid().get_row(p.y));layers.push_back(side?11:9);
        }
        if(cols.size()<3||cols.size()>16) continue;
        std::sort(cols.begin(),cols.end());std::sort(rows.begin(),rows.end());std::sort(layers.begin(),layers.end());
        const int c=cols[cols.size()/2],r=rows[rows.size()/2],l=layers[layers.size()/2];
        std::vector<int> candidates{graph_.node(c,r,l)};
        // Existing junctions preserve useful trunk locations. Bound the list
        // before Hanan probes; use coordinates rather than all grid cells.
        std::vector<std::pair<int,int>> junctions;
        for(auto [n,d]:degree) if(d>=3 && (graph_.layer(n)>10)==bool(side))
            junctions.emplace_back(std::abs(graph_.col(n)-c)+std::abs(graph_.row(n)-r),n);
        std::sort(junctions.begin(),junctions.end());
        for(size_t k=0;k<std::min(size_t(2),junctions.size());++k) candidates.push_back(junctions[k].second);
        for(auto [x,y]:std::vector<std::pair<int,int>>{{cols.front(),r},{cols.back(),r},{c,rows.front()},{c,rows.back()}})
            candidates.push_back(graph_.node(x,y,l));
        std::set<int> unique;
        for(int n:candidates) if(graph_.legal_node(n)&&unique.insert(n).second) result.emplace_back(side,n);
    }
    // Round-robin sides so one die cannot consume the entire branch budget.
    std::array<std::vector<std::pair<int,int>>,2> sides;
    for(auto item:result) sides[item.first].push_back(item);
    result.clear();
    for(size_t k=0;k<std::max(sides[0].size(),sides[1].size());++k)
        for(int side=0;side<2;++side) if(k<sides[side].size()) result.push_back(sides[side][k]);
    if(result.size()>size_t(options_.large_recovery_branches)) result.resize(size_t(options_.large_recovery_branches));
    return result;
}

std::vector<NetRoute> NegotiatedRouter::recovery_local_candidates(size_t index,const NetRoute& old) {
    std::vector<NetRoute> result;if(old.explicit_geometry) return result;
    std::map<int,std::vector<int>> adjacency;
    for(auto e:old.edges) {auto [a,b]=edge_nodes(e);adjacency[a].push_back(b);adjacency[b].push_back(a);}
    std::set<int> terminals;for(const auto& a:old.accesses) terminals.insert(a.node);
    std::vector<std::vector<int>> branches;
    for(int terminal:terminals) if(adjacency[terminal].size()==1) {
        std::vector<int> branch{terminal};int previous=-1,n=terminal;
        do {int next=adjacency[n][0];if(next==previous) next=adjacency[n][1];
            branch.push_back(next);previous=n;n=next;
        } while(adjacency[n].size()==2&&!terminals.count(n));
        if(branch.size()>2) branches.push_back(std::move(branch));
    }
    std::stable_sort(branches.begin(),branches.end(),[](const auto& a,const auto& b){return a.size()>b.size();});
    if(branches.size()>4) branches.resize(4);
    for(const auto& branch:branches) {
        if(timed_out()) break;
        auto trial=old;trial.guides.clear();
        for(size_t k=0;k+1<branch.size();++k) {trial.edges.erase(route_edge(branch[k],branch[k+1]));trial.nodes.erase(branch[k]);}
        const int side=graph_.layer(branch.front())>10;
        std::set<int> tree;for(int n:trial.nodes) if((graph_.layer(n)>10)==bool(side)) tree.insert(n);
        PinAccess a;a.node=branch.front();auto path=search({a},tree,trial,side?11:1,side?20:(old.slot>=0?9:10),options_.dijkstra);
        if(path.status!=PathResult::Status::FOUND) continue;
        for(size_t k=0;k<path.nodes.size();++k) {trial.nodes.insert(path.nodes[k]);if(k) trial.edges.insert(route_edge(path.nodes[k-1],path.nodes[k]));}
        encode(index,trial);result.push_back(std::move(trial));
    }
    return result;
}

std::optional<NetRoute> NegotiatedRouter::recovery_candidates(size_t index,const NetRoute& old,const CompleteMetrics& base) {
    NetRoute best=old;auto key=official_candidate_key(best,base);
    auto consider=[&](NetRoute trial,bool shared=false,bool local=false) {
        ++recovery_stats_.candidates;recovery_stats_.shared_candidates+=shared;recovery_stats_.local_candidates+=local;
        auto k=official_candidate_key(trial,base);
        if(trial.measured.wirelength<old.measured.wirelength && k<key) {best=std::move(trial);key=k;}
    };
    std::array<int,2> count{};for(const auto& p:def_.nets()[index].pins) ++count[p.layer_idx>10];
    if(old.slot>=0) for(auto& n:count) if(n) ++n;
    const bool local_only=*std::max_element(count.begin(),count.end())>16;
    const double current=present_;
    for(double price:std::array<double,3>{{current,0.01/std::max(1.0,official_congestion_price()),0}}) {
        if(timed_out()) break;
        present_=price;
        if(local_only) {
            try {for(auto& trial:recovery_local_candidates(index,old)) consider(std::move(trial),false,true);}
            catch(const std::exception&) {++recovery_stats_.candidate_failures;}
        } else for(int order=0;order<2&&!timed_out();++order) {
            try {consider(build_candidate(index,old.slot,order));} catch(const std::exception&) {++recovery_stats_.candidate_failures;}
        }
    }
    // Connect every terminal to an explicitly seeded shared tree. TSV stays
    // fixed and each opposite-die branch is copied without rebuilding it.
    present_=0;
    if(!local_only) for(auto [side,seed]:recovery_seeds(index,old)) for(int order=0;order<2;++order) {
        if(timed_out()) break;
        try {
            std::array<int,2> seeds{{-1,-1}};seeds[side]=seed;
            auto trial=build_tree(index,old.slot,order,seeds,side);
            for(int n:old.nodes) if((graph_.layer(n)>10)!=bool(side)) trial.nodes.insert(n);
            for(auto e:old.edges) if((graph_.layer(edge_nodes(e).first)>10)!=bool(side)) trial.edges.insert(e);
            for(const auto& a:old.accesses) if((graph_.layer(a.node)>10)!=bool(side)) trial.accesses.push_back(a);
            encode(index,trial);consider(std::move(trial),true);
        } catch(const std::exception&) {++recovery_stats_.candidate_failures;}
    }
    present_=current;
    if(best.measured.wirelength>=old.measured.wirelength) return std::nullopt;
    return best;
}

void NegotiatedRouter::recover_budgeted_wirelength() {
    if(!options_.official_model) throw std::runtime_error("budgeted recovery requires official model");
    if(!options_.large_recovery_nets||options_.large_recovery_nets>4096||options_.large_recovery_rounds<1||
       options_.large_recovery_rounds>2||options_.large_recovery_branches<0||options_.large_recovery_branches>8||
       !std::isfinite(options_.large_recovery_seconds)||options_.large_recovery_seconds<=0||options_.large_recovery_seconds>3600)
        throw std::runtime_error("invalid budgeted recovery budget");
    const auto phase_start=std::chrono::steady_clock::now();
    const auto previous_deadline=deadline_;const double previous_present=present_,previous_history=history_scale_;
    const bool previous_cell=recovery_cell_cost_;
    struct RestorePhase {std::function<void()> action;~RestorePhase(){action();}} restore{[&] {
        deadline_=previous_deadline;present_=previous_present;history_scale_=previous_history;recovery_cell_cost_=previous_cell;
    }};
    // Independent phase budget, additional to initial routing/RRR. Candidate
    // searches check this deadline too; the benchmark supplies a process limit.
    deadline_=phase_start+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(options_.large_recovery_seconds));
    history_scale_=0;recovery_cell_cost_=true;recovery_stats_={};
    validate_complete();best_metrics_=measure();recovery_stats_.before=best_metrics_;
    CompleteMetrics checkpoint=best_metrics_;std::vector<std::pair<size_t,NetRoute>> journal;
    bool valid=true;
    for(int pass=0;pass<options_.large_recovery_rounds&&valid&&!timed_out();++pass) {
        const auto selected=recovery_selection();recovery_stats_.selected+=selected.size();
        for(size_t index:selected) {
            if(timed_out()) break;
            ++recovery_stats_.attempted;recovery_stats_.zero_reference+=routes_[index].measured.reference==0;
            const auto old=routes_[index];
            if(recovery_transaction(index,[&](const auto& route,const auto& base){return recovery_candidates(index,route,base);})) {
                journal.emplace_back(index,old);
                if(journal.size()>=64) valid=recovery_checkpoint(journal,checkpoint);
            }
            if(recovery_stats_.attempted%128==0) std::cout<<"[BUDGETED_RECOVERY] nets="<<recovery_stats_.attempted
                <<" accepted="<<recovery_stats_.accepted<<" L="<<best_metrics_.guide.wirelength
                <<" S="<<100*best_metrics_.guide.quality<<std::endl;
            if(!valid) break;
        }
    }
    recovery_stats_.timed_out=timed_out();
    if(valid) recovery_checkpoint(journal,checkpoint);
    recovery_stats_.after=best_metrics_;
    recovery_stats_.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-phase_start).count();
    std::cout<<"[BUDGETED_RECOVERY] final attempted="<<recovery_stats_.attempted<<" accepted="<<recovery_stats_.accepted
        <<" candidates="<<recovery_stats_.candidates<<" seconds="<<recovery_stats_.seconds<<std::endl;
}
} // namespace eda
