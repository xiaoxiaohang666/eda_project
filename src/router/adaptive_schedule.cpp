#include "router/negotiated_router.hpp"
#include <iostream>
namespace eda {
void NegotiatedRouter::run_v9_schedule() {
    using Clock=std::chrono::steady_clock;
    const auto outer_deadline=deadline_;
    const auto original=options_;
    struct Restore {std::function<void()> f;~Restore(){f();}} restore{[&]{deadline_=outer_deadline;options_=original;}};
    std::array<bool,4> enabled{options_.rrr_rounds>0,options_.v7_tsv&&options_.tsv_rounds>0,
                              options_.v8_tsv_rematch,options_.large_recovery};
    std::array<size_t,4> visits{},stale{};
    std::array<double,4> gain{},work{};
    int rrr_remaining=options_.rrr_rounds;
    BudgetedRecoveryStats recovery_total{};
    bool recovery_seen=false,tsv_started=false;
    pre_recovery_metrics_=best_metrics_;
    // Bounded batches prevent indefinitely revisiting an exhausted candidate
    // family. Every enabled family receives a sample before gain/time selection.
    for(size_t dispatch=0;dispatch<64 && Clock::now()<outer_deadline;++dispatch) {
        size_t chosen=4;double best=-1;
        for(size_t k=0;k<4;++k) {
            if(!enabled[k]||stale[k]>=2||(k==0&&rrr_remaining<=0)) continue;
            if(!visits[k]) {chosen=k;break;}
            const double score=gain[k]/std::max(.001,work[k])+1e-5/std::sqrt(double(visits[k]));
            if(score>best) {best=score;chosen=k;}
        }
        if(chosen==4) break;
        const double remaining=std::max(0.0,std::chrono::duration<double>(outer_deadline-Clock::now()).count());
        size_t unseen=0;for(size_t k=0;k<4;++k) unseen+=enabled[k]&&!visits[k];
        const double slice=std::min(remaining,std::max(.02,std::min(60.0,remaining/double(std::max(size_t(2),unseen+1)))));
        deadline_=std::min(outer_deadline,Clock::now()+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(slice)));
        const auto start=Clock::now();const double before=best_metrics_.guide.quality;
        const auto tsv_sweeps_before=counters_.v9_tsv_sweeps;
        try {
            if(chosen==0) {
                counters_.v9_rrr_budget+=slice;
                const int rounds=std::min(2,rrr_remaining);rrr_remaining-=rounds;
                if(options_.v8_rrr)run_hotspot_rrr(rounds);else run_rrr(rounds);
            } else if(chosen==1) {
                counters_.v9_tsv_budget+=slice;
                if(!tsv_started) {pre_tsv_lengths_.clear();for(const auto& route:routes_)pre_tsv_lengths_.push_back(route.measured.wirelength);tsv_started=true;}
                options_.v7_tsv_seconds=slice;
                if(options_.v8_tsv_dynamic)optimize_dynamic_tsv();else optimize_v7_tsv();
            } else if(chosen==2) {
                counters_.v9_rematch_budget+=slice;
                options_.v8_tsv_rematch_seconds=counters_.v8_rematch_seconds+slice;
                rematch_tsv_ownership();
            } else {
                counters_.v9_recovery_budget+=slice;
                options_.large_recovery_seconds=slice;
                if(!recovery_seen)pre_recovery_metrics_=best_metrics_;
                recover_budgeted_wirelength();
                const auto& s=recovery_stats_;
                if(!recovery_seen)recovery_total.before=s.before;
                recovery_seen=true;recovery_total.after=s.after;
                recovery_total.selected+=s.selected;recovery_total.zero_reference+=s.zero_reference;
                recovery_total.attempted+=s.attempted;recovery_total.candidates+=s.candidates;
                recovery_total.shared_candidates+=s.shared_candidates;recovery_total.local_candidates+=s.local_candidates;
                recovery_total.accepted+=s.accepted;recovery_total.rejected+=s.rejected;
                recovery_total.checkpoints+=s.checkpoints;recovery_total.checkpoint_rollbacks+=s.checkpoint_rollbacks;
                recovery_total.candidate_failures+=s.candidate_failures;recovery_total.seconds+=s.seconds;
                recovery_total.timed_out=recovery_total.timed_out||s.timed_out;
            }
        } catch(const std::exception& e) {
            // Each stage owns its transaction rollback. A full check is required
            // before another stage may consume the restored state.
            ++rejection_reasons_[std::string("V9 stage stopped: ")+e.what()];
            complete_validation_cached_=false;validate_complete();best_metrics_=measure();verify_official_ledger(best_metrics_);
            if(best_metrics_.guide.quality+1e-10<before)
                throw std::logic_error("V9 stage failed to restore its accepted complete solution");
        }
        deadline_=outer_deadline;
        const double elapsed=std::chrono::duration<double>(Clock::now()-start).count();
        const double delta=100*std::max(0.0,best_metrics_.guide.quality-before);
        gain[chosen]=visits[chosen]?.5*gain[chosen]+.5*delta:delta;
        work[chosen]=visits[chosen]?.5*work[chosen]+.5*elapsed:elapsed;
        ++visits[chosen];
        // A short slice can end during probing or partway through the current
        // coverage epoch. No gain is not exhaustion until the whole TSV set
        // has received an expensive visit; the next slice resumes that epoch.
        const bool unfinished_tsv=chosen==1 && options_.v9_tsv_coverage &&
            counters_.v9_tsv_sweeps==tsv_sweeps_before;
        if(delta>1e-9) stale[chosen]=0;
        else if(!unfinished_tsv) ++stale[chosen];
        // A changed design can reopen opportunities in a previously stale arm.
        if(delta>1e-9)for(size_t k=0;k<4;++k)if(k!=chosen&&stale[k])--stale[k];
        std::cout<<"[V9 budget] stage="<<chosen<<" allocated="<<slice<<" used="<<elapsed<<" gain="<<delta<<'\n';
    }
    if(recovery_seen)recovery_stats_=recovery_total;
    complete_validation_cached_=false;validate_complete();best_metrics_=measure();verify_official_ledger(best_metrics_);
    pre_tsv_local_metrics_=best_metrics_;
}
}
