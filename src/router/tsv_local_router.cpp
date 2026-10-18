#include "router/negotiated_router.hpp"
#include <optional>

namespace eda {

void NegotiatedRouter::recover_tsv_local() {
    if(!options_.tsv_local) return;
    for(size_t index=0;index<routes_.size()&&!timed_out();++index) {
        const auto old=routes_[index];
        if(old.slot<0) continue;
        const auto& net=def_.nets()[index];
        const auto& slot=policy_.slots().at(old.slot);
        std::vector<NetRoute> sides[2];
        bool supported=true;
        for(int side=0;side<2;++side) {
            Pin landing;landing.name="planned_tsv";landing.coord=slot.point;
            landing.layer_idx=side?11:10;landing.layer=get_layer_name(landing.layer_idx);
            std::vector<Pin> terminals{landing};
            for(const auto& pin:net.pins) if((pin.layer_idx>10)==bool(side)) {
                bool duplicate=false;
                for(const auto& p:terminals)
                    if(pin.layer_idx==p.layer_idx&&pin.coord.distance_to(p.coord)<1e-8) duplicate=true;
                if(!duplicate) terminals.push_back(pin);
            }
            // General fanout trees retain the searched incumbent. Two-terminal
            // sides need no geometric union/dedup accounting of shared branches.
            if(terminals.size()!=2) {supported=false;break;}
            sides[side]=terminal_pair_candidates(terminals[0],terminals[1],side?12:1,side?20:9);
            if(sides[side].empty()) {supported=false;break;}
        }
        if(!supported||timed_out()) continue;
        const auto before=best_metrics_;
        CompleteMetrics chosen_metrics=before;
        std::optional<NetRoute> chosen;
        account(index,old,-1);routes_[index]=NetRoute{};
        for(const auto& lower:sides[0]) {
            if(timed_out()) break;
            for(const auto& upper:sides[1]) {
                if(timed_out()) break;
                NetRoute trial=lower;trial.slot=old.slot;
                trial.nodes.insert(upper.nodes.begin(),upper.nodes.end());
                trial.edges.insert(upper.edges.begin(),upper.edges.end());
                trial.accesses.insert(trial.accesses.end(),upper.accesses.begin(),upper.accesses.end());
                bool accounted=false;
                try {
                    // Encode both sides together: all original pins and the
                    // one fixed interface witness must survive mapping checks.
                    encode(index,trial);++counters_.tsv_local_candidates;
                    if(!options_.official_model && (trial.length>old.length+1e-6||trial.vias>old.vias||
                       trial.measured.wirelength>old.measured.wirelength+1e-6)) continue;
                    if(timed_out()) break;
                    ++counters_.tsv_local_trials;
                    routes_[index]=trial;account(index,trial,1);accounted=true;
                    const auto after=measure();
                    if((options_.official_model || (after.guide.overflow<=before.guide.overflow&&
                       after.physical_overflow<=before.physical_overflow&&
                       after.guide.wirelength<=before.guide.wirelength+1e-6&&
                       after.physical_length<=before.physical_length+1e-6&&after.vias<=before.vias&&
                       after.guide.quality+1e-12>=before.guide.quality)) && better(after,chosen_metrics)) {
                        validate_complete();chosen=std::move(trial);chosen_metrics=after;
                    }
                } catch(const std::exception& e) {++rejection_reasons_[e.what()];}
                if(accounted) account(index,routes_[index],-1);
                routes_[index]=NetRoute{};
            }
        }
        // Every attempted transaction is removed before committing a winner.
        // A deadline commits the best validated candidate, if any; otherwise
        // rejected candidates leave the exact incumbent in place.
        routes_[index]=chosen?std::move(*chosen):old;account(index,routes_[index],1);
        if(chosen) {best_metrics_=chosen_metrics;++counters_.tsv_local_accepted;}
        else ++counters_.rollbacks;
        validate_complete();
    }
    std::cout<<"[TSV_LOCAL] accepted="<<counters_.tsv_local_accepted
             <<" physical_length="<<best_metrics_.physical_length<<" vias="<<best_metrics_.vias<<"\n";
}

} // namespace eda
