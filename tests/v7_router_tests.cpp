#include "router/negotiated_router.hpp"
#include "scoring/compat_validation.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace eda;
namespace fs=std::filesystem;
namespace {
size_t checks=0;
void require(bool ok,const std::string& message) {++checks;if(!ok) throw std::runtime_error(message);}
void near(double a,double b,const std::string& message) {
    require(std::abs(a-b)<=1e-10*std::max({1.0,std::abs(a),std::abs(b)}),message);
}
std::string write(const fs::path& path,const std::string& contents) {
    std::ofstream out(path);out<<contents;out.close();require(bool(out),"write fixture");return path.string();
}
std::string geometry(const std::vector<NetRoute>& routes) {
    std::ostringstream text;
    for(const auto& route:routes) {
        text<<route.slot<<';';
        for(const auto& g:route.guides) text<<g.layer<<','<<g.x1<<','<<g.y1<<','<<g.x2<<','<<g.y2<<';';
        text<<'\n';
    }
    return text.str();
}
}
namespace eda {
struct V7RouterTestAccess {
    static void exact(NegotiatedRouter& r) {
        r.validate_complete();
        std::vector<NetGuideMetrics> measured;measured.reserve(r.routes_.size());
        std::vector<int> demand(r.graph_.size());
        GuideMap guides;
        std::set<int> slots;
        for(size_t i=0;i<r.routes_.size();++i) {
            const auto& route=r.routes_[i];
            if(route.slot>=0) require(slots.insert(route.slot).second,"one net per TSV slot");
            guides[r.def_.nets()[i].name]=route.guides;
            measured.push_back(measure_official_net_guides(r.def_.nets()[i],route.guides,r.graph_,r.lef_,
                r.options_.official_metrics,r.reference_lengths_[i]));
            require(measured.back().demand==route.measured.demand,"fresh geometry demand equals net cache");
            require(measured.back().tsv_cells==route.measured.tsv_cells,"fresh TSV cells equal net cache");
            near(measured.back().wirelength,route.measured.wirelength,"fresh geometry wirelength equals cache");
            for(auto [n,m]:measured.back().demand) demand[n]+=m;
        }
        require(demand==r.demand_,"fresh geometry demand equals live ledger");
        require(compat::validate(r.def_,r.lef_,guides).error_count==0,"complete encoded guide is legal");
        std::vector<const NetGuideMetrics*> views;for(const auto& net:measured) views.push_back(&net);
        const auto fresh=aggregate_official_metrics(views,demand,r.graph_,r.thermal_,r.options_.official_metrics,false);
        const auto cached=r.measure();r.verify_official_ledger(cached);
        near(fresh.guide.quality,cached.guide.quality,"fresh geometry quality equals cache");
        near(fresh.guide.correlation,cached.guide.correlation,"fresh Pearson equals cache");
        near(cached.guide.quality,r.best_metrics_.guide.quality,"best checkpoint equals live score");
    }
    static void topology_budget(NegotiatedRouter& r) {
        const auto options=r.options_;const auto elapsed=r.counters_.v7_topology_seconds;
        const auto deadline=r.deadline_;const auto demand=r.demand_;const auto guide=geometry(r.routes_);
        r.options_.v7_topology=false;
        const auto baseline=r.build_route(0,r.routes_[0].slot);
        r.options_.v7_topology=true;
        const double limit=std::min(120.0,.05*r.options_.time_limit);
        // A positive sub-clock-tick remainder enters the candidate then expires
        // deterministically, without sleeps or machine-speed assumptions.
        r.counters_.v7_topology_seconds=std::nextafter(limit,0.0);
        const auto failures=r.counters_.v7_topology_failures;
        const auto fallback=r.build_route(0,r.routes_[0].slot);
        require(r.counters_.v7_topology_failures>failures,"topology construction is interrupted by its stage budget");
        require(geometry({fallback})==geometry({baseline}),"interrupted topology retains valid incumbent geometry");
        require(r.deadline_==deadline && !r.v5_topology_trial_,"topology budget restores outer deadline and active flag");
        require(r.demand_==demand && geometry(r.routes_)==guide,"topology interruption leaves complete ledger untouched");
        r.counters_.v7_topology_seconds=elapsed;r.options_=options;exact(r);
    }
    static void maze(NegotiatedRouter& r) {
        PinAccess source;source.node=r.graph_.node(0,0,1);
        const int target=r.graph_.node(5,5,1);
        NetRoute own;own.net_index=3;
        // Free tree cells/edges would make an ordinary geometric heuristic inadmissible.
        for(int c=1;c<5;++c) own.nodes.insert(r.graph_.node(c,2,1));
        const auto old=r.options_;
        r.options_.v7_cell_cost=true;
        r.options_.v7_rrr_astar_expansions=10000;
        r.v7_refinement_active_=true;
        r.v7_refinement_expansion_start_=r.counters_.expansions;
        r.v7_refinement_deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        const auto start=r.counters_.expansions;
        const auto path=r.search({source},{target},own,1,3,false);
        require(path.status==PathResult::Status::FOUND,"V7 maze finds real route");
        require(r.counters_.expansions>start,"V7 maze actually expands nodes");
        r.v7_refinement_active_=false;
        const auto oracle=r.search({source},{target},own,1,3,true);
        require(oracle.status==PathResult::Status::FOUND,"Dijkstra oracle finds route");
        near(path.cost,oracle.cost,"V7 maze agrees with Dijkstra when existing cells are free");
        r.options_.v7_rrr_astar_expansions=1;
        r.v7_refinement_active_=true;
        r.v7_refinement_expansion_start_=r.counters_.expansions;
        r.v7_refinement_deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        const auto fallbacks=r.counters_.v7_astar_fallbacks;
        const auto limited=r.search({source},{target},own,1,3,false);
        require(limited.status==PathResult::Status::FOUND,"exhausted maze retains valid template");
        require(r.counters_.expansions-r.v7_refinement_expansion_start_<=1,"per-net expansion budget is respected");
        require(r.counters_.v7_astar_fallbacks>fallbacks,"budget fallback path was executed");
        require(limited.nodes.front()==source.node && limited.nodes.back()==target,"fallback retains endpoints");
        for(size_t i=1;i<limited.nodes.size();++i) {
            const auto adjacent=r.graph_.neighbors(limited.nodes[i-1]);
            require(std::find(adjacent.begin(),adjacent.end(),limited.nodes[i])!=adjacent.end(),"fallback consists of legal edges");
        }
        r.v7_refinement_active_=false;r.options_=old;
    }
    static void rrr(NegotiatedRouter& r) {
        const auto saved=r.options_;
        r.options_.v7_rrr=true;r.options_.local_repair=false;r.options_.pin_local=false;
        r.options_.v7_rrr_astar_net_limit=0;
        // Zero global search work deliberately fails candidate construction after rip-up.
        r.options_.search_limit=0;
        const auto before=r.measure();const auto demand=r.demand_;const auto cells=r.cell_nets_;
        const auto edges=r.edge_nets_;const auto guide=geometry(r.routes_);
        const auto restores=r.counters_.v7_checkpoint_restores;
        require(!r.rrr_round({0,1,3}),"failed candidates reject entire RRR batch");
        require(r.counters_.v7_rrr_nets>=3,"RRR actually processed selected nets");
        require(r.counters_.v7_checkpoint_restores>restores,"RRR batch rollback executed");
        require(r.demand_==demand && r.cell_nets_==cells && r.edge_nets_==edges,"RRR restores all owner and demand ledgers");
        require(geometry(r.routes_)==guide,"RRR restores exact routes and TSV slots");
        near(r.measure().guide.quality,before.guide.quality,"RRR restores original complete quality");
        exact(r);
        // An expired stage cannot discard its last complete solution.
        const auto deadline=r.deadline_;
        r.deadline_=std::chrono::steady_clock::now()-std::chrono::seconds(1);
        require(!r.rrr_round({0}),"expired RRR returns without modifying complete solution");
        require(geometry(r.routes_)==guide && r.demand_==demand,"expired RRR preserves geometry and ledger");
        r.deadline_=deadline;r.options_=saved;exact(r);

        // Give a same-die net a connected but wasteful extra branch. A real
        // replacement must remove it, commit the score improvement, then reject
        // an identical second batch and restore its already staged journal.
        r.options_.v7_rrr=true;r.options_.local_repair=false;r.options_.pin_local=false;
        for(size_t index:{3U,4U}) {
            r.account(index,r.routes_[index],-1);
            auto wasteful=r.build_candidate(index,-1,0);
            int previous=r.graph_.node(0,0,1);wasteful.nodes.insert(previous);
            for(int layer=2;layer<=9;++layer) {
                const int next=r.graph_.node(0,0,layer);
                wasteful.nodes.insert(next);wasteful.edges.insert(route_edge(previous,next));previous=next;
            }
            for(int c=1;c<6;++c) {
                const int next=r.graph_.node(c,0,9);
                wasteful.nodes.insert(next);wasteful.edges.insert(route_edge(previous,next));previous=next;
            }
            r.encode(index,wasteful);r.routes_[index]=std::move(wasteful);r.account(index,r.routes_[index],1);
        }
        r.best_metrics_=r.measure();exact(r);
        require(r.official_movable_priority(3)>0,"two removable branches create real movable congestion");
        const auto bad_quality=r.best_metrics_.guide.quality;
        const auto maze_nets=r.counters_.v7_astar_nets;
        const auto maze_searches=r.counters_.v7_astar_searches;
        r.options_.v7_rrr_astar_seconds=10;
        require(r.rrr_round({3}),"RRR accepts an actual route replacement");
        require(r.counters_.v7_astar_nets>maze_nets && r.counters_.v7_astar_searches>maze_searches,
            "RRR selects congestion net and executes bounded maze refinement");
        require(r.best_metrics_.guide.quality>bad_quality,"accepted RRR improves encoded score");
        exact(r);
        const auto stable=geometry(r.routes_);const auto stable_demand=r.demand_;
        const auto staged_generation=r.state_generation_;
        require(!r.rrr_round({3}),"identical staged RRR replacement is rejected");
        require(r.state_generation_>=staged_generation+4,"rejected batch inserted its candidate and restored journal");
        require(geometry(r.routes_)==stable && r.demand_==stable_demand,"staged RRR rollback recovers complete checkpoint");
        r.options_=saved;exact(r);
    }
    static void tsv(NegotiatedRouter& r) {
        // Install deliberately cold legal reservations so the real phase must build
        // and accept at least one changed route, independent of assignment heuristics.
        for(size_t i=0;i<3;++i) r.account(i,r.routes_[i],-1);
        for(size_t i=0;i<3;++i) {
            r.routes_[i]=r.build_route(i,int(i));r.account(i,r.routes_[i],1);
        }
        r.best_metrics_=r.measure();exact(r);
        const auto before=r.best_metrics_;const auto original=geometry(r.routes_);
        const auto saved=r.options_;
        r.options_.v7_tsv=true;r.options_.tsv_rounds=3;
        r.options_.v7_tsv_candidates=12;r.options_.v7_tsv_nets=3;r.options_.v7_tsv_seconds=20;
        r.optimize_v7_tsv();
        require(r.counters_.v7_tsv_attempted>0,"TSV phase reroutes real candidates");
        require(r.counters_.v7_tsv_accepted>0,"TSV phase accepts an actual route and slot change");
        require(geometry(r.routes_)!=original,"accepted TSV move changes encoded routes");
        require(r.best_metrics_.guide.quality>before.guide.quality+1e-10,"joint TSV improvement increases exact score");
        exact(r);
        const auto checkpoint=geometry(r.routes_);const auto demand=r.demand_;
        const auto rollbacks=r.counters_.v7_tsv_rollbacks;
        r.options_.search_limit=0;
        r.optimize_v7_tsv();
        require(r.counters_.v7_tsv_rollbacks>rollbacks,"invalid TSV route construction really rolls back");
        require(geometry(r.routes_)==checkpoint && r.demand_==demand,"failed TSV candidates restore full geometry and resource demand");
        exact(r);
        const auto budget_stops=r.counters_.v7_tsv_budget_stops;
        r.options_.v7_tsv_seconds=0;
        r.optimize_v7_tsv();
        require(r.counters_.v7_tsv_budget_stops>budget_stops,"expired TSV phase budget is observed");
        require(geometry(r.routes_)==checkpoint && r.demand_==demand,"expired TSV phase keeps last complete solution");
        r.options_=saved;exact(r);
    }
    static void run(NegotiatedRouter& r) {
        require(r.route_all(),"V7 fixture routes completely");
        require(r.counters_.v7_topology_attempted>0 && r.counters_.v7_topology_candidates>0,"shared-tree topology actually builds alternatives");
        exact(r);topology_budget(r);maze(r);rrr(r);tsv(r);
        std::cout<<"V7 exercised: maze="<<r.counters_.v7_astar_searches<<" fallback="<<r.counters_.v7_astar_fallbacks
                 <<" topology="<<r.counters_.v7_topology_candidates<<" TSV="<<r.counters_.v7_tsv_accepted
                 <<" rollbacks="<<r.counters_.v7_tsv_rollbacks<<'\n';
    }
};
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("v7_router_tests OUTPUT_DIRECTORY OFFICIAL_RUNTIME");
        const fs::path directory=argv[1];fs::create_directories(directory);
        std::string tech,def="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 600 600 ) ; GCELLGRID X 0 DO 7 STEP 100 ; GCELLGRID Y 0 DO 7 STEP 100 ; ";
        for(int l=1;l<=20;++l) {
            const auto name=get_layer_name(l);
            tech+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(l%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.2 ; END "+name+"\n";
            const std::string track=l==9?"5 DO 6 STEP 100":"5 DO 60 STEP 10";
            def+="TRACKS X "+track+" LAYER "+name+" ; TRACKS Y "+track+" LAYER "+name+" ; ";
        }
        def+="COMPONENTS 0 ; END COMPONENTS PINS 13 ; ";
        const auto pin=[&](std::string name,int layer,int x,int y) {
            def+="- "+name+" + LAYER metal"+std::to_string(layer)+" ( -1 -1 ) ( 1 1 ) + FIXED ( "+std::to_string(x)+" "+std::to_string(y)+" ) N ; ";
        };
        for(int i=0;i<3;++i) {pin("a"+std::to_string(i),1,50+i*100,250);pin("b"+std::to_string(i),1,450,350+i*100);pin("c"+std::to_string(i),12,150+i*100,450);}
        pin("local_a",1,50,50);pin("local_b",1,550,550);
        pin("local_c",1,50,50);pin("local_d",1,550,550);
        def+="END PINS NETS 5 ; ";
        for(int i=0;i<3;++i) def+="- cross"+std::to_string(i)+" ( PIN a"+std::to_string(i)+" ) ( PIN b"+std::to_string(i)+" ) ( PIN c"+std::to_string(i)+" ) ; ";
        def+="- local ( PIN local_a ) ( PIN local_b ) ; - duplicate ( PIN local_c ) ( PIN local_d ) ; END NETS END DESIGN";
        LefParser lef;require(lef.parse(write(directory/"fixture.lef",tech)),"parse LEF");
        DefParser parsed;require(parsed.parse(write(directory/"fixture.def",def),lef,DefGridMapping::OFFICIAL_FULL),"parse DEF");
        std::ostringstream heat;heat<<"{\"meta\":{\"ambientTemp\":25},\"nodes\":[";
        for(int y=0;y<6;++y) for(int x=0;x<6;++x) {
            if(x||y) heat<<',';
            heat<<'['<<5+x*10<<','<<5+y*10<<",0,"<<26+x+6*y<<']';
        }
        heat<<"]}";
        ThermalParser thermal;require(thermal.parse_json(write(directory/"heat.json",heat.str()),parsed.gcell_grid(),
            parsed.dbu_per_micron(),parsed.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),"parse heat");
        RouterOptions options;options.official_model=true;options.official_runtime=argv[2];
        options.graph_options={RoutingGeometryMode::GLOBAL_GUIDE,false,true,false,true};
        options.official_metrics.score_20261020=true;options.official_metrics.thermal_gain=3;
        options.official_metrics.gcell_width_dbu=parsed.gcell_pitch_x();options.official_metrics.gcell_height_dbu=parsed.gcell_pitch_y();
        options.tsv_policy.resource_mode=TsvResourceMode::GLOBAL_GRID;
        options.rrr_rounds=options.tsv_rounds=options.wirelength_rounds=0;options.tsv_local=false;
        options.v7_cell_cost=true;options.v7_topology=true;options.time_limit=120;
        NegotiatedRouter router(parsed,lef,thermal,options);V7RouterTestAccess::run(router);
        std::cout<<"v7_router_tests: "<<checks<<" checks PASS\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
