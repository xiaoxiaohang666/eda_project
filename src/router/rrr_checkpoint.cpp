#include "router/negotiated_router.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace eda {
namespace {
bool close_score(double a,double b) {
    return std::isfinite(a)&&std::isfinite(b)&&
        std::abs(a-b)<=2e-10*std::max({1.0,std::abs(a),std::abs(b)});
}
bool same_net_measurement(const NetGuideMetrics& a,const NetGuideMetrics& b) {
    if(a.wirelength!=b.wirelength || a.reference!=b.reference || a.demand!=b.demand ||
       a.tsv_cells!=b.tsv_cells || a.tsv_points.size()!=b.tsv_points.size()) return false;
    for(size_t k=0;k<a.tsv_points.size();++k)
        if(a.tsv_points[k].distance_to(b.tsv_points[k])!=0) return false;
    return true;
}
}
void NegotiatedRouter::begin_rrr_checkpoint() {
    if(v9_rrr_checkpoint_active_) throw std::logic_error("nested RRR checkpoint session");
    const auto start=std::chrono::steady_clock::now();
    complete_validation_cached_=false;validate_complete();
    best_metrics_=measure();verify_official_ledger(best_metrics_);
    v9_rrr_checkpoint_metrics_=best_metrics_;
    v9_rrr_journal_.clear();v9_rrr_pending_groups_=0;
    v9_rrr_accepted_checkpoint_=counters_.rrr_accepted;
    v9_hotspot_accepted_checkpoint_=counters_.v8_rrr_groups_accepted;
    v9_rrr_checkpoint_time_=std::chrono::steady_clock::now();
    const double seconds=std::chrono::duration<double>(v9_rrr_checkpoint_time_-start).count();
    counters_.v9_full_validation_seconds+=seconds;counters_.v7_validation_seconds+=seconds;
    ++counters_.v9_full_checkpoints;v9_rrr_checkpoint_active_=true;
}
void NegotiatedRouter::journal_rrr_scope(const std::vector<size_t>& scope) {
    // Capture before the group's first ledger mutation. A failed local rollback
    // can therefore still recover the previously validated complete solution.
    for(size_t i:scope) v9_rrr_journal_.try_emplace(i,routes_.at(i));
}
void NegotiatedRouter::validate_rrr_group(const std::vector<std::pair<size_t,NetRoute>>& saved) {
    const auto start=std::chrono::steady_clock::now();
    struct RecordTime {RouterCounters& c;std::chrono::steady_clock::time_point start;
        ~RecordTime(){c.v9_local_validation_seconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();}
    } record{counters_,start};
    ++counters_.v9_local_validations;
    std::set<int> touched_cells;
    std::set<EdgeKey> touched_edges;
    for(const auto& [index,old]:saved) {
        const auto& route=routes_.at(index);std::string error;
        if(route.slot!=old.slot || !policy_.validate_mapping(def_.nets()[index],route.guides,route.slot,error) ||
           !validate_route(index,route.guides,error))
            throw std::logic_error("RRR local geometry/mapping validation: "+error);
        const auto measured=measure_official_net_guides(def_.nets()[index],route.guides,graph_,lef_,
            options_.official_metrics,reference_lengths_.at(index));
        if(!same_net_measurement(measured,route.measured))
            throw std::logic_error("RRR local Guide/measurement cache mismatch");
        if(measured.tsv_cells!=old.measured.tsv_cells || measured.tsv_points.size()!=old.measured.tsv_points.size())
            throw std::logic_error("RRR local transaction changed TSV occupancy");
        for(size_t k=0;k<measured.tsv_points.size();++k)
            if(measured.tsv_points[k].distance_to(old.measured.tsv_points[k])!=0)
                throw std::logic_error("RRR local transaction changed TSV location");
        for(int node:route.nodes) if(!graph_.legal_node(node))
            throw std::logic_error("RRR local transaction retained an illegal node");
        for(auto edge:route.edges) {
            const auto [a,b]=edge_nodes(edge);
            if(!route.nodes.count(a)||!route.nodes.count(b)||!graph_.legal_edge(a,b))
                throw std::logic_error("RRR local transaction retained an illegal edge");
            const auto owner=edge_nets_.find(edge);
            if(owner==edge_nets_.end() || !std::binary_search(owner->second.begin(),owner->second.end(),int(index)))
                throw std::logic_error("RRR local missing forward edge owner");
        }
        for(auto [node,amount]:measured.demand) {
            if(amount<=0 || (graph_.documented_rules()&&graph_.official_capacity(node)<=0))
                throw std::logic_error("RRR local invalid non-exempt resource demand");
            const auto owner=cell_nets_.find(node);
            if(owner==cell_nets_.end() || !std::binary_search(owner->second.begin(),owner->second.end(),int(index)))
                throw std::logic_error("RRR local missing forward cell owner");
        }
        for(const auto* candidate:{&old,&route}) {
            touched_edges.insert(candidate->edges.begin(),candidate->edges.end());
            for(auto [node,amount]:candidate->measured.demand) {(void)amount;touched_cells.insert(node);}
        }
    }
    const auto valid_owners=[&](const std::vector<int>& owners) {
        if(owners.empty() || std::adjacent_find(owners.begin(),owners.end(),std::greater_equal<int>())!=owners.end())
            throw std::logic_error("RRR local owners are not sorted and unique");
        for(int owner:owners) if(owner<0 || size_t(owner)>=routes_.size())
            throw std::logic_error("RRR local owner outside route array");
    };
    for(auto edge:touched_edges) {
        const auto owners=edge_nets_.find(edge);const auto used=usage_.find(edge);
        const int count=used==usage_.end()?0:used->second;
        if(count<0 || (owners==edge_nets_.end()?count!=0:size_t(count)!=owners->second.size()))
            throw std::logic_error("RRR local edge usage mismatch");
        if(owners!=edge_nets_.end()) {
            valid_owners(owners->second);
            for(int owner:owners->second) if(!routes_[size_t(owner)].edges.count(edge))
                throw std::logic_error("RRR local extraneous edge owner");
        }
    }
    for(int node:touched_cells) {
        long long expected=0;const auto owners=cell_nets_.find(node);
        if(owners!=cell_nets_.end()) {
            valid_owners(owners->second);
            for(int owner:owners->second) {
                const auto& demand=routes_[size_t(owner)].measured.demand;
                const auto found=std::lower_bound(demand.begin(),demand.end(),node,
                    [](const auto& entry,int id){return entry.first<id;});
                if(found==demand.end()||found->first!=node||found->second<=0)
                    throw std::logic_error("RRR local extraneous cell owner");
                expected+=found->second;
            }
        }
        if(expected!=demand_.at(size_t(node))) throw std::logic_error("RRR local Guide demand conservation mismatch");
    }
}
void NegotiatedRouter::restore_rrr_checkpoint(const std::string& reason) {
    ++rejection_reasons_["RRR checkpoint restored: "+reason];
    for(auto& [index,old]:v9_rrr_journal_) routes_[index]=std::move(old);
    v9_rrr_journal_.clear();
    // Rebuild rather than applying inverse deltas to a ledger whose consistency
    // just failed. This recovery-only scan is never part of ordinary group work.
    std::fill(demand_.begin(),demand_.end(),0);usage_.clear();cell_nets_.clear();edge_nets_.clear();
    official_wirelength_=0;official_reference_=0;official_squared_overflow_=0;
    official_overflow_=0;official_zero_capacity_cells_=0;
    for(size_t node=0;node<graph_.size();++node) {
        const auto fixed=graph_.official_obstacle_demand(int(node));if(!fixed) continue;
        const double capacity=graph_.official_capacity(int(node));
        if(capacity<0 || !std::isfinite(capacity) || capacity>std::numeric_limits<int>::max())
            throw std::logic_error("invalid capacity while restoring RRR checkpoint");
        const auto excess=std::max(0LL,static_cast<long long>(fixed)-static_cast<long long>(capacity));
        official_overflow_+=uint64_t(excess);
        if(capacity<=0) {official_squared_overflow_+=10000; ++official_zero_capacity_cells_;}
        else {const long double ratio=.1L*excess/capacity;official_squared_overflow_+=ratio*ratio;}
    }
    for(size_t i=0;i<routes_.size();++i) {
        routes_[i].measured=measure_official_net_guides(def_.nets()[i],routes_[i].guides,graph_,lef_,
            options_.official_metrics,reference_lengths_.at(i));
        account(i,routes_[i],1);
    }
    counters_.rrr_accepted=v9_rrr_accepted_checkpoint_;
    counters_.v8_rrr_groups_accepted=v9_hotspot_accepted_checkpoint_;
    ++counters_.v9_checkpoint_restores;++counters_.v7_checkpoint_restores;
    v9_rrr_pending_groups_=0;v9_rrr_checkpoint_active_=false;
    complete_validation_cached_=false;validate_complete();best_metrics_=measure();verify_official_ledger(best_metrics_);
    if(!close_score(best_metrics_.guide.quality,v9_rrr_checkpoint_metrics_.guide.quality) ||
       !close_score(best_metrics_.guide.wirelength,v9_rrr_checkpoint_metrics_.guide.wirelength) ||
       !close_score(best_metrics_.official.squared_normalized_overflow,v9_rrr_checkpoint_metrics_.official.squared_normalized_overflow) ||
       !close_score(best_metrics_.official.thermal_score,v9_rrr_checkpoint_metrics_.official.thermal_score))
        throw std::logic_error("RRR checkpoint recovery did not restore the verified complete solution");
}
bool NegotiatedRouter::finish_rrr_checkpoint(bool force) {
    if(!v9_rrr_checkpoint_active_) return true;
    const auto now=std::chrono::steady_clock::now();
    if(!force && v9_rrr_pending_groups_<std::max(size_t(1),options_.v9_checkpoint_groups) &&
       std::chrono::duration<double>(now-v9_rrr_checkpoint_time_).count()<options_.v9_checkpoint_seconds) return true;
    const auto start=now;
    try {
        for(const auto& [index,old]:v9_rrr_journal_) {
            (void)old;
            const auto& route=routes_[index];
            const auto actual=measure_official_net_guides(def_.nets()[index],route.guides,graph_,lef_,
                options_.official_metrics,reference_lengths_[index]);
            if(!same_net_measurement(actual,route.measured))
                throw std::logic_error("RRR full checkpoint Guide/cache mismatch");
        }
        complete_validation_cached_=false;validate_complete();
        const auto exact=measure();verify_official_ledger(exact);
        if(!close_score(exact.guide.quality,best_metrics_.guide.quality) ||
           !close_score(exact.official.thermal_score,v9_rrr_checkpoint_metrics_.official.thermal_score) ||
           exact.guide.quality+1e-10<v9_rrr_checkpoint_metrics_.guide.quality)
            throw std::logic_error("RRR full checkpoint score/thermal mismatch");
        best_metrics_=v9_rrr_checkpoint_metrics_=exact;v9_rrr_journal_.clear();v9_rrr_pending_groups_=0;
        v9_rrr_accepted_checkpoint_=counters_.rrr_accepted;
        v9_hotspot_accepted_checkpoint_=counters_.v8_rrr_groups_accepted;
        v9_rrr_checkpoint_time_=std::chrono::steady_clock::now();
        ++counters_.v9_full_checkpoints;
    } catch(const std::exception& error) {
        restore_rrr_checkpoint(error.what());
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        counters_.v9_full_validation_seconds+=seconds;counters_.v7_validation_seconds+=seconds;
        return false;
    }
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    counters_.v9_full_validation_seconds+=seconds;counters_.v7_validation_seconds+=seconds;
    return true;
}
} // namespace eda
