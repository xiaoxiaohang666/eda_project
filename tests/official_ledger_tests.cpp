#include "router/negotiated_router.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace eda;
namespace fs=std::filesystem;
namespace {
size_t checks=0;
void require(bool value,const std::string& reason) {
    ++checks;if(!value) throw std::runtime_error(reason);
}
void near(double a,double b,const std::string& reason) {
    require(std::abs(a-b)<=1e-11*std::max({1.0,std::abs(a),std::abs(b)}),reason);
}
template<class F> void rejects(F operation,const std::string& reason) {
    bool rejected=false;try {operation();} catch(const std::exception&) {rejected=true;}
    require(rejected,reason);
}
std::string write(const fs::path& path,const std::string& contents) {
    std::ofstream out(path);out<<contents;out.close();
    if(!out) throw std::runtime_error("fixture write failed");
    return path.string();
}
std::string technology() {
    std::string text;
    for(int l=1;l<=20;++l) {
        const auto name=get_layer_name(l);
        text+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(l%2?"HORIZONTAL":"VERTICAL")+
              " ; WIDTH 0.2 ; END "+name+"\n";
    }
    // Duplicated unsorted TRACKS below make this fixed-only cell overfull:
    // raw capacity 20, fixed demand 40, independently covered by graph tests.
    text+="MACRO RESOURCE_SLOT_bottom SIZE 10 BY 10 ; END RESOURCE_SLOT_bottom MACRO RESOURCE_SLOT_top SIZE 10 BY 10 ; END RESOURCE_SLOT_top LAYER tsv TYPE CUT ; END tsv VIA tsv_v DEFAULT LAYER metal10 ; RECT -0.25 -0.25 0.25 0.25 ; LAYER tsv ; RECT -0.25 -0.25 0.25 0.25 ; LAYER metal11 ; RECT -0.25 -0.25 0.25 0.25 ; END tsv_v\n";
    text+="MACRO TSV_BOTTOM SIZE 10 BY 10 ; PIN TSV_PORT PORT LAYER metal10 ; RECT 4.5 4.5 5.5 5.5 ; END END TSV_PORT END TSV_BOTTOM MACRO TSV_TOP SIZE 10 BY 10 ; PIN TSV_PORT PORT LAYER metal11 ; RECT 4.5 4.5 5.5 5.5 ; END END TSV_PORT END TSV_TOP\n";
    text+="MACRO block CLASS BLOCK ; SIZE 30 BY 30 ; OBS LAYER metal1 ; RECT 3 0 6 10 ; END END block\n";
    return text;
}
std::string design(size_t net_count) {
    std::string text="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 300 300 ) ; "
        "GCELLGRID X 0 DO 4 STEP 100 ; GCELLGRID Y 0 DO 4 STEP 100 ; ";
    for(int l=1;l<=20;++l) if(l!=2) {
        const auto name=get_layer_name(l);
        text+="TRACKS X 10 DO 15 STEP 20 LAYER "+name+" ; TRACKS Y 5 DO 30 STEP 10 LAYER "+name+" ; ";
    }
    text+="TRACKS X 210 DO 1 STEP 20 LAYER metal2 ; TRACKS Y 205 DO 1 STEP 10 LAYER metal2 ; TRACKS Y 5 DO 30 STEP 10 LAYER metal1 ; "
        "COMPONENTS 3 ; - b block + PLACED ( 0 0 ) N ; - rb RESOURCE_SLOT_bottom + PLACED ( 0 0 ) N ; - rt RESOURCE_SLOT_top + PLACED ( 0 0 ) N ; END COMPONENTS PINS 4 ; "
        "- a + LAYER metal10 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; "
        "- z + LAYER metal11 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; "
        "- c + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; "
        "- d + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 250 50 ) N ; END PINS NETS "+std::to_string(net_count)+" ; ";
    text+="- cross ( PIN a ) ( PIN z ) ; ";
    for(size_t i=1;i<net_count;++i) text+="- local"+std::to_string(i)+" ( PIN c ) ( PIN d ) ; ";
    return text+"END NETS END DESIGN";
}
}
namespace eda {
struct PinLocalTestAccess {
    static void check_official_target_shortlist(NegotiatedRouter& router) {
        require(router.options_.official_model && router.large_design() && router.routes_.empty() && router.state_generation_==0,
                "target shortlist regression starts with a fresh large official router");
        require(std::all_of(router.demand_.begin(),router.demand_.end(),[](int demand){return demand==0;}),
                "target shortlist regression has no accounted net demand");
        PinAccess source;source.node=router.graph_.node(0,0,9);
        const int same_layer=router.graph_.node(2,0,9);
        const std::set<int> targets{router.graph_.node(0,0,1),router.graph_.node(1,0,2),same_layer};
        // XY-only ranking excludes the third target despite the cost of seven
        // or eight ordinary vias to the first two. The actual graph oracle
        // proves that the same-M9 target is reachable in two planar steps.
        const auto fast=router.search({source},targets,NetRoute{},1,10,false);
        require(fast.status==PathResult::Status::FOUND && fast.nodes.front()==source.node && fast.nodes.back()==same_layer,
                "official fast shortlist keeps the cheapest same-layer target");
        require(router.counters_.fast_initial_paths==1 && router.counters_.expansions==0,
                "shortlist regression exercises the bounded fast template path");
        const auto oracle=router.search({source},targets,NetRoute{},1,10,true);
        require(oracle.status==PathResult::Status::FOUND && oracle.nodes.front()==source.node && oracle.nodes.back()==same_layer,
                "Dijkstra independently selects the same-layer target");
        near(fast.cost,2.0,"official fast path has two planar-step cost");
        near(fast.cost,oracle.cost,"official fast target choice agrees with actual Dijkstra cost");
        for(const auto* path:{&fast,&oracle}) for(size_t i=1;i<path->nodes.size();++i)
            require(router.graph_.legal_edge(path->nodes[i-1],path->nodes[i]),"shortlist and oracle paths retain legal graph edges");
        require(router.state_generation_==0 && router.routes_.empty(),"search-only shortlist regression leaves the ledger untouched");
    }
    static void check_validation_cache(NegotiatedRouter& router,const fs::path& folder) {
        require(router.route_all(),"real fixture route passes complete validation");
        require(router.complete_validations_==1 && router.complete_validation_cache_hits_>=1,
                "unchanged route_all final state reuses its first full validation");
        const auto baseline=router.complete_validations_;
        const auto hits=router.complete_validation_cache_hits_;
        router.validate_complete();
        require(router.complete_validations_==baseline && router.complete_validation_cache_hits_==hits+1,
                "repeated complete validation skips its full scan");
        const auto old=router.routes_.at(1);
        const auto proof=std::tuple(router.state_generation_,router.validated_generation_,router.complete_validation_cached_);
        rejects([&]{router.account(1,old,1);},"failed duplicate account does not invalidate a valid generation");
        require(std::tuple(router.state_generation_,router.validated_generation_,router.complete_validation_cached_)==proof,
                "rejected account preserves generation and validation proof");
        router.validate_complete();
        require(router.complete_validations_==baseline,"rejected account permits reuse of unchanged validation");
        const auto generation=router.state_generation_;
        router.account(1,old,-1);router.routes_[1]=NetRoute{};
        require(router.state_generation_==generation+1 && !router.complete_validation_cached_,
                "successful ledger mutation invalidates complete validation");
        rejects([&]{router.validate_complete();},"mutation with a missing net fails independent complete validation");
        require(!router.complete_validation_cached_ && router.complete_validations_==baseline+1,
                "failed complete validation never caches its generation");
        rejects([&]{router.validate_complete();},"failed validation retries the full checks and fails again");
        require(!router.complete_validation_cached_ && router.complete_validations_==baseline+2,
                "failed validation retry is counted as another full attempt");
        const auto invalid_proof=std::tuple(router.state_generation_,router.validated_generation_,router.complete_validation_cached_);
        rejects([&]{router.account(1,old,-1);},"missing ripup is rejected while the validation cache is invalid");
        require(std::tuple(router.state_generation_,router.validated_generation_,router.complete_validation_cached_)==invalid_proof,
                "rejected account never advances an invalid generation");
        router.routes_[1]=old;router.account(1,old,1);
        router.validate_complete();
        require(router.complete_validation_cached_ && router.validated_generation_==router.state_generation_ &&
                router.complete_validations_==baseline+3,"restored transaction requires a successful new full validation");
        router.validate_complete();
        require(router.complete_validations_==baseline+3,"restored validated state supports subsequent reuse");
        const auto name=router.options_.official_model?"official":"legacy";
        require(router.export_guide((folder/(std::string(name)+"-validated.guide")).string()),
                "unchanged valid guide still passes independent serialized readback");
        const auto saved_guides=router.guides_;
        auto& corrupt=router.guides_.begin()->second.front();corrupt.x2=corrupt.x1;
        require(!router.export_guide((folder/(std::string(name)+"-invalid-readback.guide")).string()),
                "serialized readback rejects invalid output even when internal validation is cached");
        router.guides_=saved_guides;
        require(router.complete_validations_==baseline+3,"export reuses only internal validation, not serialized readback");
    }
    static void check_physical_validation_faults(NegotiatedRouter& router) {
        require(router.complete_validation_cached_ && router.validated_generation_==router.state_generation_,
                "physical corruption tests start with a fully validated real route");
        const auto saved_usage=router.usage_;
        const auto saved_owners=router.edge_nets_;
        std::vector<std::set<EdgeKey>> saved_edges;
        for(const auto& route:router.routes_) saved_edges.push_back(route.edges);
        const auto shared=std::find_if(saved_owners.begin(),saved_owners.end(),
            [](const auto& one){return one.second.size()>=2;});
        require(shared!=saved_owners.end(),"real multi-net fixture shares a physical edge");
        const EdgeKey edge=shared->first;
        const int owner=shared->second.front();
        int ghost=-1;
        for(size_t i=0;i<router.routes_.size();++i) if(!router.routes_[i].edges.count(edge)) {ghost=int(i);break;}
        require(ghost>=0,"physical edge has an unrelated real net for ghost-owner injection");
        EdgeKey unused=0;bool found_unused=false;
        for(int node=0;node<int(router.graph_.size())&&!found_unused;++node)
            for(int next:router.graph_.neighbors(node)) {
                const auto candidate=route_edge(node,next);
                if(!saved_usage.count(candidate) && !saved_owners.count(candidate)) {
                    unused=candidate;found_unused=true;break;
                }
            }
        require(found_unused,"real graph has an unused legal edge for zero-key regression");
        const auto illegal=route_edge(router.graph_.node(0,0,1),router.graph_.node(2,0,1));
        auto [bad_a,bad_b]=edge_nodes(illegal);
        require(!router.graph_.legal_edge(bad_a,bad_b) && !saved_owners.count(illegal),
                "two-cell jump is an absent illegal retained edge");
        auto restore=[&] {
            router.usage_=saved_usage;router.edge_nets_=saved_owners;
            for(size_t i=0;i<saved_edges.size();++i) router.routes_[i].edges=saved_edges[i];
            // Test-only direct mutations do not pass through account(). Never
            // let a prior generation proof mask a deliberately damaged ledger.
            router.complete_validation_cached_=false;
        };
        auto fault=[&](auto inject,const std::string& reason) {
            restore();inject();router.complete_validation_cached_=false;
            const auto attempts=router.complete_validations_,hits=router.complete_validation_cache_hits_;
            const auto generation=router.state_generation_;
            rejects([&]{router.validate_complete();},reason);
            require(router.complete_validations_==attempts+1 && router.complete_validation_cache_hits_==hits &&
                    !router.complete_validation_cached_ && router.state_generation_==generation,
                    "corruption executes a full validation and never caches or changes the generation");
            restore();router.validate_complete();
            require(router.complete_validation_cached_ && router.validated_generation_==router.state_generation_,
                    "restored physical ledger passes full validation again");
        };
        fault([&]{router.edge_nets_.erase(edge);},"missing reverse edge key rejected");
        fault([&]{auto& owners=router.edge_nets_.at(edge);owners.erase(owners.begin());},
              "missing one real reverse owner rejected");
        fault([&]{auto& owners=router.edge_nets_.at(edge);owners.push_back(ghost);std::sort(owners.begin(),owners.end());
                  router.usage_[edge]=int(owners.size());},"ghost real owner rejected even when usage matches reverse count");
        fault([&]{auto& owners=router.edge_nets_.at(edge);owners.insert(owners.begin(),owners.front());
                  router.usage_[edge]=int(owners.size());},"duplicate reverse owner rejected");
        fault([&]{auto& owners=router.edge_nets_.at(edge);std::swap(owners[0],owners[1]);},
              "unsorted real reverse owners rejected");
        fault([&]{auto& owners=router.edge_nets_.at(edge);owners.insert(owners.begin(),-1);
                  router.usage_[edge]=int(owners.size());},"negative reverse owner rejected before route access");
        fault([&]{auto& owners=router.edge_nets_.at(edge);owners.push_back(int(router.routes_.size()));
                  router.usage_[edge]=int(owners.size());},"out-of-range reverse owner rejected before route access");
        fault([&]{++router.usage_[edge];},"physical usage count mismatch rejected");
        fault([&]{router.usage_.erase(edge);},"missing physical usage key rejected");
        fault([&]{router.usage_[unused]=1;},"nonzero ownerless usage key rejected");
        fault([&]{router.edge_nets_[unused]={};router.usage_[unused]=0;},
              "empty reverse key remains invalid even with a zero usage key");
        fault([&]{router.edge_nets_[unused]={owner};router.usage_[unused]=1;},
              "extraneous reverse edge owner rejected");
        fault([&]{router.routes_[owner].edges.insert(illegal);router.edge_nets_[illegal]={owner};router.usage_[illegal]=1;},
              "illegal retained edge rejected despite consistent forward/reverse counts");
        restore();router.usage_[unused]=0;
        const auto attempts=router.complete_validations_;
        router.validate_complete();
        require(router.complete_validations_==attempts+1 && router.complete_validation_cached_ &&
                router.usage_.at(unused)==0 && !router.edge_nets_.count(unused),
                "legal zero usage residue stays accepted without a reverse key");
        restore();router.validate_complete();
    }
    static void check_cache(const NegotiatedRouter& router) {
        std::vector<const NetGuideMetrics*> nets;
        for(const auto& route:router.routes_) nets.push_back(&route.measured);
        NetGuideMetrics thermal_only;
        // Candidate bases intentionally support the prefix before its first TSV.
        // This zero-W/R thermal witness only permits the independent aggregator
        // to calculate C; it does not change any compared ledger statistic.
        if(std::none_of(nets.begin(),nets.end(),[](auto n){return !n->tsv_cells.empty();})) {
            thermal_only.tsv_cells={8};nets.push_back(&thermal_only);
        }
        const auto full=aggregate_official_metrics(nets,router.demand_,router.graph_,router.thermal_,router.options_.official_metrics,false);
        const auto cached=router.official_candidate_base();
        near(cached.guide.wirelength,full.guide.wirelength,"cached W equals independent full aggregate");
        near(cached.guide.reference,full.guide.reference,"cached R equals independent full aggregate");
        near(cached.official.squared_normalized_overflow,full.squared_normalized_overflow,"cached squared overflow equals independent full aggregate");
        require(cached.guide.overflow==full.guide.overflow && cached.official.zero_capacity_cells==full.zero_capacity_cells,
                "cached integer congestion totals equal independent full aggregate");
    }
    static auto snapshot(const NegotiatedRouter& router) {
        return std::tuple(router.demand_,router.usage_,router.cell_nets_,router.edge_nets_,
            router.official_wirelength_,router.official_reference_,router.official_squared_overflow_,
            router.official_overflow_,router.official_zero_capacity_cells_);
    }
    static OfficialGuideMetrics full(const NegotiatedRouter& router,const std::vector<int>& demand) {
        std::vector<const NetGuideMetrics*> nets;
        for(const auto& route:router.routes_) nets.push_back(&route.measured);
        return aggregate_official_metrics(nets,demand,router.graph_,router.thermal_,router.options_.official_metrics,false);
    }
    static void check_fixed_zero(NegotiatedRouter& router) {
        router.routes_.resize(router.def_.nets().size());
        NetRoute cross,probe;
        cross.measured=measure_official_net_guides(router.def_.nets()[0],{{45,45,55,55,"metal10"},{45,45,55,55,"metal11"}},
            router.graph_,router.lef_,router.options_.official_metrics,router.reference_lengths_[0]);
        probe.measured=measure_official_net_guides(router.def_.nets()[2],{{145,145,155,155,"metal2"}},
            router.graph_,router.lef_,router.options_.official_metrics,router.reference_lengths_[2]);
        const int node=probe.measured.demand.front().first;
        require(router.graph_.official_capacity(node)==0 && router.graph_.official_obstacle_demand(node)>0,
                "real OBS preceding-track fixture has fixed occupancy on zero raw capacity");
        router.account(0,cross,1);router.routes_[0]=cross;
        router.account(2,probe,1);router.routes_[2]=probe;
        router.account(3,probe,1);router.routes_[3]=probe;
        const auto before=full(router,router.demand_);
        require(router.official_reroute_priority(node,1)==0,"fixed occupied zero-capacity owners are non-actionable");
        std::vector<size_t> scope{2};router.complete_zero_capacity_scope(scope);
        require(scope==std::vector<size_t>{2},"fixed zero-capacity cell does not add its shared owner to scope");
        auto cleared=router.demand_;cleared[node]=0;
        require(full(router,cleared).zero_capacity_cells==before.zero_capacity_cells,
                "independent full score proves guide-owner removal cannot clear fixed bad cell");
        check_cache(router);
    }
    static void check(NegotiatedRouter& router,bool official,bool large) {
        require(router.large_design()==large,"small/large schedule boundary exercised");
        router.routes_.resize(router.def_.nets().size());
        NetRoute cross,local,zero;
        if(official) {
            check_cache(router);
            require(router.graph_.official_capacity(router.graph_.node(0,0,1))==20 &&
                    router.graph_.official_obstacle_demand(router.graph_.node(0,0,1))==40,
                    "fixed-only overflow fixture retains original track semantics");
            auto measured=[&](size_t i,const std::vector<GuideRect>& guides) {
                return measure_official_net_guides(router.def_.nets()[i],guides,router.graph_,router.lef_,
                    router.options_.official_metrics,router.reference_lengths_[i]);
            };
            cross.measured=measured(0,{{45,45,55,55,"metal10"},{45,45,55,55,"metal11"}});
            require(cross.measured.demand.size()==2 && cross.measured.demand[0].second==2 && cross.measured.demand[1].second==2,
                    "selected TSV adds demand two on each interface layer");
            local.measured=measured(1,{{45,45,255,55,"metal1"}});
            zero.measured=measured(2,{{45,45,55,55,"metal2"}});
            require(router.graph_.official_capacity(zero.measured.demand.front().first)==0,"zero raw capacity fixture");
        } else {
            local.measured.demand={{router.graph_.node(0,0,1),1},{router.graph_.node(1,0,1),1}};
            local.measured.wirelength=200;local.measured.reference=200;
        }
        local.edges.insert(route_edge(router.graph_.node(0,0,1),router.graph_.node(1,0,1)));
        local.length=200;
        if(official) {
            router.account(0,cross,1);router.routes_[0]=cross;check_cache(router);
            const auto cross_snapshot=snapshot(router);
            rejects([&]{router.account(0,cross,1);},"duplicate Guide-only owner rejected");
            require(snapshot(router)==cross_snapshot,"Guide-only duplicate rejection preserves every ledger");
            router.account(2,zero,1);router.routes_[2]=zero;check_cache(router);
            require(router.official_candidate_base().official.zero_capacity_cells==1,"first zero-capacity owner counted");
        }
        router.account(1,local,1);router.routes_[1]=local;
        if(official) {
            check_cache(router);
            const int node=local.measured.demand.front().first;
            const auto before=full(router,router.demand_);
            auto removed=router.demand_;--removed[node];
            near(router.official_reroute_priority(node,1),before.squared_normalized_overflow-full(router,removed).squared_normalized_overflow,
                 "positive raw capacity priority equals independent full-score congestion relief");
        }
        const auto full_snapshot=snapshot(router);
        rejects([&]{router.account(1,local,1);},"duplicate owner rejected");
        require(snapshot(router)==full_snapshot,"duplicate owner rejection preserves every ledger");
        router.account(1,local,-1);router.routes_[1]=NetRoute{};
        if(official) {
            check_cache(router);
            const auto base=router.official_candidate_base();
            near(base.guide.wirelength,cross.measured.wirelength+zero.measured.wirelength,"other-net W remains after ripup");
            near(base.guide.reference,cross.measured.reference+zero.measured.reference,"other-net R remains after ripup");
            const auto candidate=router.official_candidate_key(local,base);
            auto full_demand=router.demand_;for(auto[n,m]:local.measured.demand) full_demand[n]+=m;
            const auto full=aggregate_official_metrics({&cross.measured,&zero.measured,&local.measured},full_demand,
                router.graph_,router.thermal_,router.options_.official_metrics,false);
            near(-std::get<1>(candidate),(.45*full.wirelength_score+.30*full.congestion_score)/100,
                 "candidate delta ranking equals independent complete W/C score");
            require(std::get<0>(candidate)==full.zero_capacity_cells && std::get<2>(candidate)==full.guide.overflow,
                    "candidate integer key equals independent complete score");
        }
        const auto ripped_snapshot=snapshot(router);
        rejects([&]{router.account(1,local,-1);},"missing ripup rejected");
        require(snapshot(router)==ripped_snapshot,"missing ripup rejection preserves every ledger");
        router.account(1,local,1);router.routes_[1]=local;
        require(snapshot(router)==full_snapshot,"reverse ripup/reaccount restores all ledgers exactly");
        if(official) {
            check_cache(router);
            // Two owners of one zero-capacity cell still count one bad cell.
            const int zero_node=zero.measured.demand.front().first;
            require(router.official_reroute_priority(zero_node,1)>0,"single removable zero-capacity owner has actionable priority");
            router.account(3,zero,1);router.routes_[3]=zero;check_cache(router);
            require(router.official_candidate_base().official.zero_capacity_cells==1,"zero-capacity count is cells, not owners");
            const auto shared=full(router,router.demand_);
            auto one_removed=router.demand_;--one_removed[zero_node];
            near(full(router,one_removed).squared_normalized_overflow,shared.squared_normalized_overflow,
                 "independent score demonstrates flat penalty after removing only one shared owner");
            require(router.official_reroute_priority(zero_node,1)>0,"each shared zero-capacity owner receives actionable priority");
            std::vector<size_t> scope{2};router.complete_zero_capacity_scope(scope);
            require(scope==std::vector<size_t>({2,3}),"actual scope completion retains both shared owners after the 4-net percentage cutoff");
            auto group_removed=router.demand_;
            for(size_t i:scope) for(auto[node,m]:router.routes_[i].measured.demand) group_removed[node]-=m;
            require(full(router,group_removed).zero_capacity_cells+1==shared.zero_capacity_cells,
                    "independent full score proves selected group's removal clears the bad cell");
            if(large) {
                std::vector<size_t> budget{2};for(size_t i=4;i<130;++i) budget.push_back(i);
                router.complete_zero_capacity_scope(budget);
                require(budget.size()==128 && budget.back()==3,"scope completion preserves the 128-net ceiling");
            }
            router.account(2,zero,-1);router.routes_[2]=NetRoute{};check_cache(router);
            require(router.official_candidate_base().official.zero_capacity_cells==1,"shared zero-capacity cell remains occupied");
            router.account(3,zero,-1);router.routes_[3]=NetRoute{};check_cache(router);
            require(router.official_candidate_base().official.zero_capacity_cells==0,"last zero-capacity ripup removes bad cell");
            const auto zero_snapshot=snapshot(router);
            rejects([&]{router.account(3,zero,-1);},"missing Guide-only ripup rejected");
            require(snapshot(router)==zero_snapshot,"Guide-only missing ripup rejection preserves every ledger");
        } else {
            require(router.official_wirelength_==0 && router.official_reference_==0 && router.official_squared_overflow_==0 &&
                    router.official_overflow_==0 && router.official_zero_capacity_cells_==0,"legacy account leaves official cache unused");
        }
    }
};
}
int main(int argc,char** argv) {
    try {
        if(argc<2 || argc>3) throw std::runtime_error("official_ledger_tests OUTPUT [original-flute-runtime]");
        const fs::path folder=argv[1];fs::create_directories(folder);
        const bool native=argc==3 && fs::exists(fs::path(argv[2])/"libflute.so");
        // Real routed fixtures test the validation proof in both models. Keep
        // OBS out of the strict fixture and define ordinary vias for the TSV access.
        auto validation_tech=technology();
        const std::string block_obs="OBS LAYER metal1 ; RECT 3 0 6 10 ; END";
        validation_tech.erase(validation_tech.find(block_obs),block_obs.size());
        for(int l=1;l<20;++l) if(l!=10) {
            const auto cut="validation_cut"+std::to_string(l),via="validation_via"+std::to_string(l);
            validation_tech+="LAYER "+cut+" TYPE CUT ; END "+cut+" VIA "+via+" DEFAULT LAYER "+get_layer_name(l)+
                " ; RECT -0.1 -0.1 0.1 0.1 ; LAYER "+cut+" ; RECT -0.05 -0.05 0.05 0.05 ; LAYER "+get_layer_name(l+1)+
                " ; RECT -0.1 -0.1 0.1 0.1 ; END "+via+"\n";
        }
        LefParser validation_lef;DefParser validation_def;ThermalParser validation_heat;
        auto validation_design=design(4);
        validation_design.insert(validation_design.find("COMPONENTS"),
            "TRACKS X 10 DO 15 STEP 20 LAYER metal2 ; TRACKS Y 5 DO 30 STEP 10 LAYER metal2 ; ");
        require(validation_lef.parse(write(folder/"validation.lef",validation_tech)),"validation LEF parsed");
        require(validation_def.parse(write(folder/"validation.def",validation_design),validation_lef),"validation DEF parsed");
        require(validation_heat.parse_json(write(folder/"validation.json",R"({"meta":{"ambientTemp":25},"nodes":[[5,5,0,35],[15,5,0,40],[25,5,0,45],[5,15,0,30],[15,15,0,55],[25,15,0,40],[5,25,0,40],[15,25,0,35],[25,25,0,30]]})"),
                validation_def.gcell_grid(),10,validation_def.die_area()),"validation heat parsed");
        RouterOptions validation_options;
        validation_options.rrr_rounds=0;validation_options.tsv_rounds=0;validation_options.wirelength_rounds=0;
        validation_options.tsv_local=false;
        NegotiatedRouter legacy_validation(validation_def,validation_lef,validation_heat,validation_options);
        PinLocalTestAccess::check_validation_cache(legacy_validation,folder);
        PinLocalTestAccess::check_physical_validation_faults(legacy_validation);
        if(native) {
            // A sub-GCell landing exercises the guarded aperture repair as well as validation caching.
            auto official_validation_tech=validation_tech;
            LefParser official_validation_lef;DefParser official_validation_def;ThermalParser official_validation_heat;
            require(official_validation_lef.parse(write(folder/"validation-official.lef",official_validation_tech)),
                    "official validation LEF parsed");
            const auto official_def_path=write(folder/"validation-official.def",validation_design);
            require(official_validation_def.parse(official_def_path,official_validation_lef,DefGridMapping::OFFICIAL_FULL),
                    "official validation full-grid DEF parsed");
            require(official_validation_heat.parse_json((folder/"validation.json").string(),official_validation_def.gcell_grid(),
                    official_validation_def.dbu_per_micron(),official_validation_def.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),
                    "official validation thermal profile parsed");
            validation_options.official_model=true;validation_options.official_runtime=argv[2];
            validation_options.graph_options.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
            validation_options.official_metrics.gcell_width_dbu=official_validation_def.gcell_pitch_x();
            validation_options.official_metrics.gcell_height_dbu=official_validation_def.gcell_pitch_y();
            validation_options.official_metrics.thermal_gain=official_thermal_gain_for_def_file(official_def_path);
            NegotiatedRouter official_validation(official_validation_def,official_validation_lef,official_validation_heat,validation_options);
            PinLocalTestAccess::check_validation_cache(official_validation,folder);
            PinLocalTestAccess::check_physical_validation_faults(official_validation);
        }
        for(bool large:{false,true}) {
            LefParser lef;DefParser def;ThermalParser thermal;
            const auto prefix=large?"large":"small";
            require(lef.parse(write(folder/(std::string(prefix)+".lef"),technology())),"fixture LEF parsed");
            require(def.parse(write(folder/(std::string(prefix)+".def"),design(large?1001:4)),lef),"fixture DEF parsed");
            require(thermal.parse_json(write(folder/(std::string(prefix)+".json"),R"({"meta":{"ambientTemp":25},"nodes":[[5,5,0,35],[15,5,0,40],[25,5,0,45],[5,15,0,30],[15,15,0,55],[25,15,0,40],[5,25,0,40],[15,25,0,35],[25,25,0,30]]})"),
                    def.gcell_grid(),10,def.die_area()),"fixture heat parsed");
            DefParser legacy_def;
            auto legacy_text=design(large?1001:4);
            legacy_text.insert(legacy_text.find("COMPONENTS"),"TRACKS X 10 DO 15 STEP 20 LAYER metal2 ; TRACKS Y 5 DO 30 STEP 10 LAYER metal2 ; ");
            require(legacy_def.parse(write(folder/(std::string(prefix)+"-legacy.def"),legacy_text),lef),"strict legacy fixture retains physical TRACKS");
            RouterOptions legacy;NegotiatedRouter old_router(legacy_def,lef,thermal,legacy);
            PinLocalTestAccess::check(old_router,false,large);
            if(native) {
                RouterOptions options;options.official_model=true;options.official_runtime=argv[2];
                options.graph_options.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
                options.official_metrics.gcell_width_dbu=100;options.official_metrics.gcell_height_dbu=100;
                NegotiatedRouter router(def,lef,thermal,options);
                if(large) PinLocalTestAccess::check_official_target_shortlist(router);
                PinLocalTestAccess::check(router,true,large);
                if(!large) {
                    LefParser fixed_lef;DefParser fixed_def;
                    auto fixed_tech=technology();
                    const std::string obs="OBS LAYER metal1 ; RECT 3 0 6 10 ;";
                    fixed_tech.replace(fixed_tech.find(obs),obs.size(),obs+" LAYER metal2 ; RECT 14 10 15 20 ;");
                    auto fixed_design=design(4);
                    fixed_design.insert(fixed_design.find("TRACKS X 210"),"TRACKS X 10 DO 1 STEP 20 LAYER metal2 ; ");
                    require(fixed_lef.parse(write(folder/"fixed-zero.lef",fixed_tech)),"fixed-zero LEF parsed");
                    require(fixed_def.parse(write(folder/"fixed-zero.def",fixed_design),fixed_lef),"fixed-zero DEF parsed");
                    NegotiatedRouter fixed_router(fixed_def,fixed_lef,thermal,options);
                    PinLocalTestAccess::check_fixed_zero(fixed_router);
                }
            }
        }
        if(!native) std::cout<<"SKIP official cache parity: original native FLUTE assets absent or not supplied\n";
        std::cout<<"official_ledger_tests: "<<checks<<" checks PASS\n";
    } catch(const std::exception& error) {std::cerr<<"official_ledger_tests: "<<error.what()<<'\n';return 1;}
}
