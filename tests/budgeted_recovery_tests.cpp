#include "router/negotiated_router.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace eda;
namespace fs=std::filesystem;
namespace {
size_t checks=0;
void require(bool value,const std::string& reason) {++checks;if(!value)throw std::runtime_error(reason);}
void near(double a,double b,const std::string& reason) {
    require(std::abs(a-b)<=1e-10*std::max({1.0,std::abs(a),std::abs(b)}),reason);
}
std::string write(const fs::path& path,const std::string& text) {
    std::ofstream out(path);out<<text;out.close();require(bool(out),"fixture write failed");return path.string();
}
std::string technology() {
    std::string text;
    for(int layer=1;layer<=20;++layer) {
        const auto name=get_layer_name(layer);
        text+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(layer%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.2 ; END "+name+"\n";
    }
    return text+"MACRO RESOURCE_SLOT_bottom SIZE 2 BY 2 ; END RESOURCE_SLOT_bottom "
        "MACRO RESOURCE_SLOT_top SIZE 2 BY 2 ; END RESOURCE_SLOT_top "
        "MACRO TSV_BOTTOM SIZE 2 BY 2 ; PIN TSV_PORT PORT LAYER metal10 ; RECT 0.5 0.5 1.5 1.5 ; END END TSV_PORT END TSV_BOTTOM "
        "MACRO TSV_TOP SIZE 2 BY 2 ; PIN TSV_PORT PORT LAYER metal11 ; RECT 0.5 0.5 1.5 1.5 ; END END TSV_PORT END TSV_TOP "
        "LAYER tsv TYPE CUT ; END tsv VIA tsv_v DEFAULT LAYER metal10 ; RECT -0.25 -0.25 0.25 0.25 ; "
        "LAYER tsv ; RECT -0.25 -0.25 0.25 0.25 ; LAYER metal11 ; RECT -0.25 -0.25 0.25 0.25 ; END tsv_v\n";
}
std::string prefix(bool rectangular=false,int height=200) {
    std::string text="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 200 "+std::to_string(height)+" ) ; GCELLGRID X 0 DO 21 STEP 10 ; ";
    const int row_pitch=rectangular?20:10;
    text+="GCELLGRID Y 0 DO "+std::to_string(height/row_pitch+1)+" STEP "+std::to_string(row_pitch)+" ; ";
    for(int layer=1;layer<=20;++layer) {
        const auto name=get_layer_name(layer);
        if(layer==2 && !rectangular) text+="TRACKS X 195 DO 1 STEP 1 LAYER "+name+" ; TRACKS Y 195 DO 1 STEP 1 LAYER "+name+" ; ";
        else text+="TRACKS X 0.5 DO 200 STEP 1 LAYER "+name+" ; TRACKS Y 0.5 DO "+std::to_string(height)+" STEP 1 LAYER "+name+" ; ";
    }
    return text+"COMPONENTS 4 ; - b0 RESOURCE_SLOT_bottom + PLACED ( 40 40 ) N ; "
        "- t0 RESOURCE_SLOT_top + PLACED ( 40 40 ) N ; - b1 RESOURCE_SLOT_bottom + PLACED ( 140 140 ) N ; "
        "- t1 RESOURCE_SLOT_top + PLACED ( 140 140 ) N ; END COMPONENTS ";
}
std::string design(bool rectangular=false) {
    return prefix(rectangular)+"PINS 4 ; "
        "- a + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 5 5 ) N ; "
        "- b + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 195 5 ) N ; "
        "- lo + LAYER metal3 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; "
        "- hi + LAYER metal14 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; END PINS "
        "NETS 2 ; - local ( PIN a ) ( PIN b ) ; - cross ( PIN lo ) ( PIN hi ) ; END NETS END DESIGN";
}
std::string fanout_design() {
    std::ostringstream out;out<<prefix(true)<<"PINS 33 ; ";
    const std::vector<Point> points{{25,25},{55,25},{85,25},{115,55},{25,85},{55,115},{145,145},{175,175}};
    for(int side=0;side<2;++side)for(size_t i=0;i<points.size();++i)
        out<<"- s"<<side<<'_'<<i<<" + LAYER "<<get_layer_name(side?14:3)
           <<" ( -1 -1 ) ( 1 1 ) + FIXED ( "<<points[i].x<<' '<<points[i].y<<" ) N ; ";
    for(int i=0;i<17;++i)out<<"- p"<<i<<" + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( "<<5+10*i<<" 5 ) N ; ";
    out<<"END PINS NETS 2 ; - shared ";
    for(int side=0;side<2;++side)for(size_t i=0;i<points.size();++i)out<<"( PIN s"<<side<<'_'<<i<<" ) ";
    out<<"; - many ";for(int i=0;i<17;++i)out<<"( PIN p"<<i<<" ) ";
    out<<"; END NETS END DESIGN";return out.str();
}
std::string subtree_design() {
    // Preserve the real native FLUTE reference for nineteen terminals, while
    // leaving enough space for a deliberately expensive, legal internal detour.
    std::ostringstream out;out<<prefix(true,2000)<<"PINS 19 ; ";
    for(int i=0;i<18;++i) {
        const int column=i<9?i:i+2;
        out<<"- branch"<<i<<" + LAYER metal3 ( -1 -1 ) ( 1 1 ) + FIXED ( "<<5+10*column<<" 70 ) N ; ";
    }
    out<<"- upper + LAYER metal14 ( -1 -1 ) ( 1 1 ) + FIXED ( 5 70 ) N ; END PINS NETS 1 ; - tree ";
    for(int i=0;i<18;++i) out<<"( PIN branch"<<i<<" ) ";
    out<<"( PIN upper ) ; END NETS END DESIGN";return out.str();
}
std::string heat() {
    std::ostringstream out;out<<"{\"meta\":{\"ambientTemp\":25},\"nodes\":[";
    for(int r=0;r<20;++r)for(int c=0;c<20;++c) {
        if(r||c)out<<',';
        out<<'['<<c+0.5<<','<<r+0.5<<",0,"<<26+c+2*r<<']';
    }
    out<<"]}";return out.str();
}
std::string signature(const NetRoute& route) {
    std::ostringstream out;out<<std::setprecision(20)<<route.explicit_geometry<<' '<<route.slot<<' '<<route.length<<' '<<route.vias;
    for(int n:route.nodes)out<<" n"<<n;
    for(auto e:route.edges)out<<" e"<<e;
    for(const auto& g:route.guides)out<<" g"<<g.layer<<' '<<g.x1<<' '<<g.y1<<' '<<g.x2<<' '<<g.y2;
    out<<" m"<<route.measured.wirelength<<' '<<route.measured.reference;
    for(auto [n,m]:route.measured.demand)out<<' '<<n<<':'<<m;
    for(auto p:route.measured.tsv_points)out<<" t"<<p.x<<' '<<p.y;
    for(int c:route.measured.tsv_cells)out<<" c"<<c;
    return out.str();
}
}
namespace eda {
struct PinLocalTestAccess {
    static size_t index(const NegotiatedRouter& router,const std::string& name) {
        for(size_t i=0;i<router.def_.nets().size();++i)if(router.def_.nets()[i].name==name)return i;
        throw std::runtime_error("missing recovery fixture net");
    }
    static NetRoute measured(NegotiatedRouter& router,size_t i,std::vector<GuideRect> guides,int slot=-1) {
        NetRoute route;route.explicit_geometry=true;route.slot=slot;route.guides=std::move(guides);route.length=190;
        route.measured=measure_official_net_guides(router.def_.nets()[i],route.guides,router.graph_,router.lef_,
            router.options_.official_metrics,router.reference_lengths_[i]);return route;
    }
    static void initialize(NegotiatedRouter& router,const ThermalParser& thermal) {
        const auto assignment=router.policy_.assign(router.def_.nets(),thermal);router.routes_.resize(router.def_.nets().size());
        const size_t local=index(router,"local"),cross=index(router,"cross");
        router.routes_[local]=measured(router,local,{{1,1,199,199,"metal1"}});router.routes_[local].length=600;
        const auto& slot=router.policy_.slots()[assignment[cross]];
        require(slot.point.distance_to({50,50})==0,"fixture chooses the expected real TSV near the crossing pins");
        std::vector<GuideRect> guides;
        for(int layer=3;layer<=14;++layer)guides.emplace_back(47,47,53,53,get_layer_name(layer));
        router.routes_[cross]=measured(router,cross,std::move(guides),assignment[cross]);
        for(size_t i=0;i<router.routes_.size();++i)router.account(i,router.routes_[i],1);
        router.initial_build_=false;router.complete_=true;router.validate_complete();router.best_metrics_=router.measure();
        require(router.best_metrics_.official.zero_capacity_cells==0,"valid incumbent avoids the zero-capacity layer");
    }
    static auto ledger(const NegotiatedRouter& router) {
        auto usage=router.usage_;for(auto it=usage.begin();it!=usage.end();)if(!it->second)it=usage.erase(it);else++it;
        std::vector<std::string> routes;for(const auto& route:router.routes_)routes.push_back(signature(route));
        return std::tuple(routes,router.demand_,usage,router.cell_nets_,router.edge_nets_,router.official_wirelength_,
            router.official_reference_,router.official_squared_overflow_,router.official_overflow_,router.official_zero_capacity_cells_);
    }
    static void parity(NegotiatedRouter& router,const std::string& reason) {
        const auto full=router.measure();
        near(router.best_metrics_.guide.quality,full.guide.quality,reason+" complete score");
        near(router.best_metrics_.guide.wirelength,full.guide.wirelength,reason+" exact Guide length");
        near(router.best_metrics_.guide.reference,full.guide.reference,reason+" fixed reference");
        near(router.best_metrics_.official.squared_normalized_overflow,full.official.squared_normalized_overflow,reason+" congestion ledger");
        near(router.best_metrics_.official.thermal_score,full.official.thermal_score,reason+" unchanged thermal score");
        router.complete_validation_cached_=false;router.validate_complete();
    }
    static void transactions(NegotiatedRouter& router) {
        const size_t local=index(router,"local"),cross=index(router,"cross");
        const auto initial=ledger(router);const auto initial_metrics=router.best_metrics_;const auto old=router.routes_[local];
        const auto short_route=measured(router,local,{{1,1,199,9,"metal1"}});
        auto reject=[&](auto propose,const std::string& reason) {
            const auto before=ledger(router);const auto generation=router.state_generation_;
            require(!router.recovery_transaction(local,propose),reason+" rejected");
            require(ledger(router)==before,reason+" restores all routes, demand, ownership and exact score ledger");
            require(router.state_generation_>generation,reason+" uses real account rollback and invalidates generation");parity(router,reason);
        };
        reject([](const auto&,const auto&)->std::optional<NetRoute>{throw std::runtime_error("injected candidate exception");},"callback exception");
        reject([](const auto&,const auto&)->std::optional<NetRoute>{return std::nullopt;},"empty proposal");
        auto invalid=short_route;invalid.guides.front().x1=1.5;
        reject([&](const auto&,const auto&){return invalid;},"noninteger candidate");
        invalid=short_route;invalid.guides.front().x1=10;
        reject([&](const auto&,const auto&){return invalid;},"lost real Pin");
        invalid=short_route;invalid.guides.push_back({1,100,9,109,"metal1"});
        reject([&](const auto&,const auto&){return invalid;},"disconnected candidate");
        invalid=short_route;invalid.nodes={router.graph_.node(0,0,1),router.graph_.node(2,0,1)};
        invalid.edges={route_edge(*invalid.nodes.begin(),*invalid.nodes.rbegin())};
        reject([&](const auto&,const auto&){return invalid;},"illegal reserved graph edge");
        auto worse=measured(router,local,{{1,1,9,9,"metal1"},{191,1,199,9,"metal1"},{1,1,199,149,"metal2"}});
        require(worse.measured.wirelength<old.measured.wirelength,"score-degrading candidate is genuinely shorter in the official Guide metric");
        worse.measured=short_route.measured; // Forged optimistic cache must never authorize its real geometry.
        reject([&](const auto&,const auto&){return worse;},"shorter but zero-capacity score-degrading candidate");
        auto cross_candidate=router.routes_[cross];const auto cross_before=ledger(router);
        cross_candidate.slot=(cross_candidate.slot+1)%int(router.policy_.slots().size());
        require(cross_candidate.slot!=router.routes_[cross].slot,"fixture supplies a distinct alternative TSV slot");
        require(!router.recovery_transaction(cross,[&](const auto&,const auto&){return cross_candidate;}),"changed fixed TSV slot rejected");
        require(ledger(router)==cross_before,"fixed TSV rejection preserves slot, selected cell and owner ledgers");

        auto fake=short_route;fake.measured.wirelength=1e30;fake.measured.reference=1e30;fake.measured.demand={{-1,100}};
        const double before_score=router.best_metrics_.guide.quality;
        require(router.recovery_transaction(local,[&](const auto&,const auto&){return fake;}),"legitimate encoded-Guide improvement is accepted after discarding forged cached metrics");
        require(router.best_metrics_.guide.quality>before_score && router.routes_[local].measured.wirelength==short_route.measured.wirelength,
            "accepted route uses the independently recomputed Guide length and improves complete score");
        require(router.routes_[local].measured.reference==router.reference_lengths_[local],"accepted cache retains the exact original native reference");
        parity(router,"accepted transaction");
        auto reference_attack=old;reference_attack.measured.reference=1e30;reference_attack.measured.wirelength=1e30;
        reject([&](const auto&,const auto&){return reference_attack;},"fake-reference length regression");

        // Geometry-only damage after validation must not reuse a success cache.
        std::vector<std::pair<size_t,NetRoute>> journal{{local,old}};auto checkpoint=initial_metrics;
        router.validate_complete();const auto cached_generation=router.state_generation_;
        router.routes_[local].guides.front().x2=router.routes_[local].guides.front().x1;
        require(router.complete_validation_cached_ && router.state_generation_==cached_generation,"injected geometry damage does not itself change the generation proof");
        require(!router.recovery_checkpoint(journal,checkpoint),"checkpoint detects damaged Guide despite a successful cached generation");
        require(journal.empty() && ledger(router)==initial,"checkpoint restores the entire last fully validated incumbent");
        require(router.recovery_stats_.checkpoint_rollbacks==1 && router.recovery_stats_.accepted==0,"checkpoint removes rolled-back acceptance counts");
        parity(router,"checkpoint rollback");
        require(router.recovery_checkpoint(journal,checkpoint) && journal.empty(),"clean checkpoint succeeds after rollback");

        // A structurally valid Guide can coexist with a damaged aggregate
        // cache. Full checkpoint measurement must reject that drift too.
        const auto old_again=router.routes_[local];checkpoint=router.best_metrics_;
        require(router.recovery_transaction(local,[&](const auto&,const auto&){return short_route;}),
            "second legitimate improvement prepares a metric-drift checkpoint");
        journal={{local,old_again}};
        router.official_wirelength_+=1000;router.official_squared_overflow_+=0.25L;
        require(!router.recovery_checkpoint(journal,checkpoint),
            "checkpoint detects aggregate wirelength/congestion drift despite unchanged valid Guides");
        require(journal.empty() && ledger(router)==initial,
            "aggregate drift rollback restores actual old routes and the last checkpoint's exact official ledger");
        require(router.recovery_stats_.checkpoint_rollbacks==2 && router.recovery_stats_.accepted==0,
            "metric-drift checkpoint removes its reverted acceptance and records the recovery");
        parity(router,"aggregate-drift rollback");

        checkpoint=router.best_metrics_;
        require(router.recovery_transaction(local,[&](const auto&,const auto&){return short_route;}),
            "third legitimate improvement prepares a valid-geometry/cache drift checkpoint");
        journal={{local,old_again}};
        router.routes_[local].guides=old_again.guides; // Still connected, integer, inside DIE and covering every real Pin.
        router.complete_validation_cached_=false;router.validate_complete();
        require(router.routes_[local].measured.wirelength==short_route.measured.wirelength,
            "structural validation alone cannot detect a valid wider Guide with a stale shorter measurement");
        require(!router.recovery_checkpoint(journal,checkpoint),
            "checkpoint remeasures changed-net Guides and detects valid geometric drift hidden by cached metrics");
        require(journal.empty() && ledger(router)==initial && router.recovery_stats_.checkpoint_rollbacks==3 &&
            router.recovery_stats_.accepted==0,"valid-geometry/cache drift restores the last validated complete incumbent");
        parity(router,"valid-geometry/cache drift rollback");
    }
    static void budget(NegotiatedRouter& router) {
        const size_t zero=index(router,"cross");const double original_reference=router.routes_[zero].measured.reference;
        router.routes_[zero].measured.reference=0;router.options_.large_recovery_nets=4;
        const auto with_zero=router.recovery_selection();
        require(with_zero.size()==2 && std::find(with_zero.begin(),with_zero.end(),zero)!=with_zero.end(),
            "zero-reference nets enter a separately bounded tail without ratio division or losing positive-reference candidates");
        router.routes_[zero].measured.reference=original_reference;
        router.options_.large_recovery_nets=1;const auto selected=router.recovery_selection();
        require(selected.size()==1 && selected.front()==index(router,"local"),"top excess-length selection obeys its independent net budget");
        const auto before=ledger(router);const auto deadline=std::chrono::steady_clock::now()-std::chrono::seconds(1);
        router.deadline_=deadline;router.present_=2.5;router.history_scale_=7;router.recovery_cell_cost_=false;
        router.options_.large_recovery_seconds=1e-12;router.options_.large_recovery_rounds=2;
        router.recover_budgeted_wirelength();
        require(router.recovery_stats_.timed_out && router.recovery_stats_.attempted==0,"independent expired phase budget preserves the incumbent before starting a candidate");
        require(ledger(router)==before,"soft phase timeout leaves all completed route and resource state intact");
        require(router.deadline_==deadline && router.present_==2.5 && router.history_scale_==7 && !router.recovery_cell_cost_,
            "phase exit restores the prior deadline, negotiated prices and search cost mode");
        parity(router,"budgeted early exit");
        router.options_.unified_budget=true;router.options_.large_recovery_seconds=10;
        router.recover_budgeted_wirelength();
        require(router.recovery_stats_.timed_out && router.recovery_stats_.attempted==0,
            "expired shared deadline cannot be extended by a positive recovery stage budget");
        require(ledger(router)==before && router.deadline_==deadline,"shared recovery timeout preserves incumbent and outer cutoff");
        router.options_.unified_budget=false;
        router.options_.v8_recovery=true;router.options_.large_recovery_nets=2;
        router.pre_tsv_lengths_.resize(router.routes_.size());
        for(size_t i=0;i<router.routes_.size();++i) router.pre_tsv_lengths_[i]=router.routes_[i].measured.wirelength;
        router.pre_tsv_lengths_[zero]=0;
        const auto migration=router.recovery_selection();
        require(!migration.empty()&&migration.front()==zero,"TSV migration increment outranks preexisting excess length");
        router.options_.v8_recovery=false;
    }
    static void cell_cost(NegotiatedRouter& router) {
        router.recovery_cell_cost_=true;router.present_=0;router.history_scale_=0;
        require(router.options_.official_metrics.gcell_width_dbu==10 && router.options_.official_metrics.gcell_height_dbu==20,
            "cell pricing fixture is non-square");
        const int source=router.graph_.node(2,2,1),new_via=router.graph_.node(2,2,2),target=router.graph_.node(10,8,2);
        near(router.edge_cost(source,new_via,NetRoute{}),8,"via cost includes new vertical-layer cell and via scaled by max pitch");
        NetRoute own;own.nodes.insert(target);PinAccess access;access.node=source;
        const auto a=router.search({access},{target},own,1,2,false),d=router.search({access},{target},own,1,2,true);
        require(a.status==PathResult::Status::FOUND && d.status==PathResult::Status::FOUND,"cell-priced non-square search remains reachable");
        near(a.cost,d.cost,"cell-priced non-square A* agrees with Dijkstra");near(d.cost,20,"independent cell-count and via arithmetic matches the oracle");
        require(a.nodes.front()==source && a.nodes.back()==target,"cell pricing preserves real search endpoints");
    }
    static void subtree(NegotiatedRouter& router) {
        const size_t i=index(router,"tree");
        require(router.def_.nets()[i].pins.size()==19,"subtree regression exceeds the old whole-net fanout cutoff");
        NetRoute old;old.net_index=i;
        for(size_t slot=0;slot<router.policy_.slots().size();++slot)
            if(router.policy_.slots()[slot].point.distance_to({5,70})==0 &&
               router.policy_.compatible(router.def_.nets()[i],int(slot))) {old.slot=int(slot);break;}
        require(old.slot>=0,"subtree fixture has a compatible real global-grid TSV");
        auto node=[&](int c,int r,int l) {return router.graph_.node(c,r,l);};
        auto edge=[&](int a,int b) {
            require(router.graph_.legal_edge(a,b),"constructed subtree fixture edge obeys preferred direction");
            old.nodes.insert(a);old.nodes.insert(b);old.edges.insert(route_edge(a,b));
        };
        auto horizontal=[&](int a,int b,int row,int layer) {
            for(int c=a;c<b;++c) edge(node(c,row,layer),node(c+1,row,layer));
        };
        auto vertical=[&](int column,int a,int b,int layer) {
            for(int row=a;row<b;++row) edge(node(column,row,layer),node(column,row+1,layer));
        };
        horizontal(0,8,3,3);horizontal(11,19,3,3);
        const int detour_row=98;
        edge(node(8,3,3),node(8,3,4));vertical(8,3,detour_row,4);
        for(int l=4;l<9;++l) edge(node(8,detour_row,l),node(8,detour_row,l+1));
        horizontal(8,11,detour_row,9);
        for(int l=4;l<9;++l) edge(node(11,detour_row,l),node(11,detour_row,l+1));
        vertical(11,3,detour_row,4);edge(node(11,3,4),node(11,3,3));
        for(int l=3;l<9;++l) edge(node(0,3,l),node(0,3,l+1));
        for(int l=11;l<14;++l) edge(node(0,3,l),node(0,3,l+1));
        for(const auto& pin:router.def_.nets()[i].pins) {
            const bool top=pin.layer_idx>10;
            auto choices=router.accesses(pin,top?11:1,top?20:9);old.accesses.push_back(choices.front());
            old.nodes.insert(choices.front().node);
        }
        const auto& slot=router.policy_.slots()[old.slot];
        for(int layer:{10,11}) {
            auto choices=router.accesses(slot.point,layer,layer==10?1:11,layer==10?9:20);
            old.accesses.push_back(choices.front());old.nodes.insert(choices.front().node);
        }
        router.encode(i,old);router.routes_.resize(router.def_.nets().size());router.routes_[i]=old;
        router.account(i,old,1);router.initial_build_=false;router.complete_=true;router.validate_complete();router.best_metrics_=router.measure();
        require(old.measured.wirelength>old.measured.reference,"detoured internal bridge lies above its native reference so shorter geometry can improve W: L="+
                std::to_string(old.measured.wirelength)+", R="+std::to_string(old.measured.reference));
        router.recovery_cell_cost_=true;router.present_=0;router.history_scale_=0;
        const auto before=ledger(router);const auto baseline=router.best_metrics_;
        require(router.recovery_subtree_candidates(i,old).empty(),"subtree mechanism is explicitly opt-in");
        router.options_.v9_subtree_recovery=true;
        std::optional<NetRoute> candidate;
        require(router.recovery_transaction(i,[&](const NetRoute& source,const CompleteMetrics&)->std::optional<NetRoute> {
            auto candidates=router.recovery_subtree_candidates(i,source);
            require(!candidates.empty(),"non-leaf U-shaped internal bridge produces bounded multi-source candidates");
            auto best=std::min_element(candidates.begin(),candidates.end(),[](const auto& a,const auto& b) {
                return a.measured.wirelength<b.measured.wirelength;
            });
            candidate=*best;return candidate;
        }),"internal two-subtree reconnect improves actual encoded Guide quality");
        require(candidate.has_value() && candidate->recovery_subtree &&
                candidate->measured.wirelength<old.measured.wirelength,"shorter result is identified as a subtree candidate");
        require(router.counters_.v9_subtree_searches>0 && router.counters_.v9_subtree_candidates>0 &&
                router.counters_.v9_subtree_accepted==1,"candidate/search/committed-subtree counters are separate");
        require(router.routes_[i].slot==old.slot && router.routes_[i].measured.tsv_cells==old.measured.tsv_cells,
                "subtree recovery preserves occupied TSV cell and network ownership");
        near(router.routes_[i].measured.tsv_points.front().distance_to(old.measured.tsv_points.front()),0,"physical TSV point stays fixed");
        near(router.best_metrics_.official.thermal_score,baseline.official.thermal_score,"fixed TSV subtree repair preserves thermal score");
        parity(router,"accepted internal subtree repair");
        std::vector<std::pair<size_t,NetRoute>> journal{{i,old}};auto checkpoint=baseline;
        router.routes_[i].guides.front().x2=router.routes_[i].guides.front().x1;
        require(!router.recovery_checkpoint(journal,checkpoint),"damaged subtree output triggers full checkpoint rollback");
        require(ledger(router)==before && router.counters_.v9_subtree_accepted==0,
                "checkpoint rollback restores route and removes reverted subtree accept count");
        parity(router,"subtree checkpoint rollback");
        const auto deadline=router.deadline_;
        router.options_.unified_budget=true;router.deadline_=std::chrono::steady_clock::now()-std::chrono::seconds(1);
        const auto attempts=router.counters_.v9_subtree_searches;
        require(!router.recovery_transaction(i,[&](const auto& source,const auto&)->std::optional<NetRoute> {
            const auto candidates=router.recovery_subtree_candidates(i,source);
            require(candidates.empty(),"expired subtree deadline emits no candidate");return std::nullopt;
        }),"expired subtree transaction is rejected");
        require(ledger(router)==before && router.counters_.v9_subtree_searches==attempts,
                "expired internal-tree work preserves complete incumbent without starting a search");
        router.deadline_=deadline;parity(router,"subtree deadline rollback");
    }
    static void branch_budgets(NegotiatedRouter& router,const ThermalParser& thermal) {
        const auto assignment=router.policy_.assign(router.def_.nets(),thermal);
        const size_t shared=index(router,"shared"),many=index(router,"many");NetRoute old;old.slot=assignment[shared];
        for(int layer:{3,14})for(auto [dc,dr]:std::vector<std::pair<int,int>>{{1,0},{-1,0},{0,1}})
            old.edges.insert(route_edge(router.graph_.node(10,5,layer),router.graph_.node(10+dc,5+dr,layer)));
        const auto seeds=router.recovery_seeds(shared,old);
        require(seeds.size()==8 && seeds==router.recovery_seeds(shared,old),"shared seed budget is at most eight and deterministic");
        require(std::count_if(seeds.begin(),seeds.end(),[](auto p){return p.first==0;})==4,"round-robin seeds preserve both die sides within the shared budget");
        for(auto [side,node]:seeds)require(router.graph_.legal_node(node) && (router.graph_.layer(node)>10)==bool(side),"shared branch seed is legal and on its declared die");
        router.options_.v8_recovery=true;
        const auto varied=router.recovery_seeds(shared,old);
        for(int side=0;side<2;++side) {
            std::set<int> layers,positions;
            for(auto [die,node]:varied) if(die==side) {layers.insert(router.graph_.layer(node));positions.insert(router.graph_.row(node)*router.graph_.grid().cols+router.graph_.col(node));}
            require(layers.size()>1&&positions.size()>1,"default V8 seeds include both trunk location and layer alternatives on each die");
        }
        router.options_.v8_recovery=false;
        router.options_.large_recovery_branches=3;require(router.recovery_seeds(shared,old).size()==3,"smaller explicit branch budget is respected");
        router.options_.large_recovery_branches=8;const auto crowded=router.build_candidate(many,-1,0);
        require(router.def_.nets()[many].pins.size()==17 && router.recovery_seeds(many,crowded).empty(),"more than sixteen terminals bypass global shared-seed enumeration");
        router.recovery_stats_={};router.recovery_candidates(many,crowded,router.official_candidate_base());
        require(router.recovery_stats_.shared_candidates==0 && router.recovery_stats_.candidates==0,
            "adjacent seventeen-terminal chain has no local leaf branch and never falls back to whole-net rebuild");
    }
};
}
int main(int argc,char** argv) {
    try {
        require(argc==2 || argc==3,"fixture directory and optional official runtime required");
        const fs::path folder=argv[1];fs::create_directories(folder);
        const auto runtime=argc==3?fs::path(argv[2]):fs::path(__FILE__).parent_path().parent_path()/"output/runtime/official-flute";
        for(const auto* asset:{"libflute.so","tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so","POWV9.dat","POST9.dat"})
            if(!fs::is_regular_file(runtime/asset)){std::cout<<"SKIP budgeted recovery: official native runtime unavailable\n";return 0;}
        LefParser lef;DefParser def;ThermalParser thermal;
        require(lef.parse(write(folder/"recovery.lef",technology())) && def.parse(write(folder/"recovery.def",design()),lef,DefGridMapping::OFFICIAL_FULL),"transaction fixture parses");
        require(thermal.parse_json(write(folder/"heat.json",heat()),def.gcell_grid(),def.dbu_per_micron(),def.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),"recovery thermal profile parses");
        RouterOptions options;options.official_model=true;options.official_runtime=runtime.string();options.time_limit=60;
        options.graph_options.geometry=RoutingGeometryMode::GLOBAL_GUIDE;options.official_metrics.gcell_width_dbu=10;options.official_metrics.gcell_height_dbu=10;
        NegotiatedRouter router(def,lef,thermal,options);PinLocalTestAccess::initialize(router,thermal);
        PinLocalTestAccess::transactions(router);PinLocalTestAccess::budget(router);
        DefParser rectangular;require(rectangular.parse(write(folder/"rectangular.def",design(true)),lef,DefGridMapping::OFFICIAL_FULL),"non-square fixture parses");
        options.official_metrics.gcell_height_dbu=20;NegotiatedRouter cell(rectangular,lef,thermal,options);PinLocalTestAccess::cell_cost(cell);
        DefParser fanout;require(fanout.parse(write(folder/"fanout.def",fanout_design()),lef,DefGridMapping::OFFICIAL_FULL),"fanout budget fixture parses");
        NegotiatedRouter branches(fanout,lef,thermal,options);PinLocalTestAccess::branch_budgets(branches,thermal);
        DefParser subtree_def;require(subtree_def.parse(write(folder/"subtree.def",subtree_design()),lef,DefGridMapping::OFFICIAL_FULL),"subtree fixture parses");
        RouterOptions subtree_options=options;subtree_options.graph_options={RoutingGeometryMode::GLOBAL_GUIDE,false,true,false,true};
        subtree_options.official_metrics.score_20261020=true;subtree_options.tsv_policy.resource_mode=TsvResourceMode::GLOBAL_GRID;
        NegotiatedRouter subtrees(subtree_def,lef,thermal,subtree_options);PinLocalTestAccess::subtree(subtrees);
        std::cout<<"PASS budgeted recovery transactions, real Guide metrics, rollback/checkpoint, phase budgets, branch budgets and non-square cell cost: "<<checks<<" checks\n";
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
