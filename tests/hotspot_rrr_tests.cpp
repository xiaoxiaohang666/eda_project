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
    std::ostringstream out;
    for(const auto& r:routes) {
        out<<r.slot<<';';
        for(const auto& g:r.guides) out<<g.layer<<','<<g.x1<<','<<g.y1<<','<<g.x2<<','<<g.y2<<';';
        out<<'\n';
    }
    return out.str();
}
}
namespace eda {
struct HotspotRrrTestAccess {
    static void exact(NegotiatedRouter& r) {
        r.complete_validation_cached_=false;r.validate_complete();
        std::vector<NetGuideMetrics> measured;measured.reserve(r.routes_.size());
        std::vector<int> demand(r.graph_.size());GuideMap guides;
        for(size_t i=0;i<r.routes_.size();++i) {
            const auto& route=r.routes_[i];guides[r.def_.nets()[i].name]=route.guides;
            measured.push_back(measure_official_net_guides(r.def_.nets()[i],route.guides,r.graph_,r.lef_,
                r.options_.official_metrics,r.reference_lengths_[i]));
            require(measured.back().demand==route.measured.demand,"fresh geometry equals cached net demand");
            for(auto [node,amount]:measured.back().demand) demand[node]+=amount;
        }
        require(demand==r.demand_,"fresh guide demand equals transactional ledger");
        require(compat::validate(r.def_,r.lef_,guides).error_count==0,"encoded complete solution is legal");
        std::vector<const NetGuideMetrics*> views;for(const auto& net:measured) views.push_back(&net);
        const auto fresh=aggregate_official_metrics(views,demand,r.graph_,r.thermal_,r.options_.official_metrics,false);
        const auto cached=r.measure();r.verify_official_ledger(cached);
        near(fresh.guide.quality,cached.guide.quality,"fresh score equals live score");
        near(cached.guide.quality,r.best_metrics_.guide.quality,"incumbent score equals live score");
    }
    static void wasteful(NegotiatedRouter& r) {
        // The two same-die nets share a removable branch on low-capacity M9.
        // Their terminals and TSV reservations stay fixed, so any improvement
        // must come from real encoded route replacement.
        for(size_t i:{3U,4U}) {
            r.account(i,r.routes_[i],-1);
            auto trial=r.build_candidate(i,-1,0);
            int previous=r.graph_.node(0,0,1);trial.nodes.insert(previous);
            for(int layer=2;layer<=9;++layer) {
                const int node=r.graph_.node(0,0,layer);trial.nodes.insert(node);
                trial.edges.insert(route_edge(previous,node));previous=node;
            }
            for(int c=1;c<6;++c) {
                const int node=r.graph_.node(c,0,9);trial.nodes.insert(node);
                trial.edges.insert(route_edge(previous,node));previous=node;
            }
            r.encode(i,trial);r.routes_[i]=std::move(trial);r.account(i,r.routes_[i],1);
        }
        r.best_metrics_=r.measure();exact(r);
        require(r.official_movable_priority(3)>0 && r.official_movable_priority(4)>0,
                "fixture has real shared removable congestion");
    }
    static void run(NegotiatedRouter& r) {
        require(r.route_all(),"fixture routes completely");exact(r);
        r.options_.local_repair=false;r.options_.pin_local=false;
        r.options_.rrr_score_aware=true;r.options_.v8_rrr_group_size=1;
        r.options_.rrr_net_limit=16;r.options_.v7_rrr_net_limit=16;
        r.options_.v7_rrr_astar_net_limit=0;
        wasteful(r);
        const auto before=r.best_metrics_;
        const auto outer_options=r.options_;
        const auto outer_scope=r.v7_scope_limit_;
        const auto outer_topology_budget=r.v5_topology_trial_;
        const auto deadline=r.deadline_;
        r.run_hotspot_rrr(2);
        require(r.counters_.v8_rrr_groups>0,"hotspot scheduler creates real transactions");
        require(r.counters_.v8_rrr_groups_accepted>0,"a local transaction is retained");
        require(r.best_metrics_.guide.quality>before.guide.quality+1e-10,"group replacement improves exact quality");
        near(r.best_metrics_.official.thermal_score,before.official.thermal_score,"fixed-slot groups retain thermal score");
        require(r.options_.v7_rrr==outer_options.v7_rrr &&
                r.options_.v7_rrr_astar_net_limit==outer_options.v7_rrr_astar_net_limit &&
                r.options_.v7_rrr_astar_seconds==outer_options.v7_rrr_astar_seconds &&
                r.v7_scope_limit_==outer_scope && !r.v7_refinement_active_ &&
                r.v5_topology_trial_==outer_topology_budget,"group phase restores outer search settings and topology budget flag");
        require(r.deadline_==deadline,"group phase does not extend global deadline");exact(r);

        // Independent failed groups must restore all demand and ownership,
        // leaving the previously accepted complete checkpoint available.
        wasteful(r);const auto checkpoint=geometry(r.routes_);const auto demand=r.demand_;
        const auto cells=r.cell_nets_;const auto edges=r.edge_nets_;const auto stable=r.best_metrics_;
        const auto rejected=r.counters_.v8_rrr_groups_rejected;
        const auto limit=r.options_.search_limit;r.options_.search_limit=0;
        r.run_hotspot_rrr(1);
        require(r.counters_.v8_rrr_groups_rejected>rejected,"failed local groups are independently rejected");
        require(geometry(r.routes_)==checkpoint && r.demand_==demand && r.cell_nets_==cells && r.edge_nets_==edges,
                "failed groups restore routes and every demand/ownership ledger");
        near(r.best_metrics_.guide.quality,stable.guide.quality,"failed groups retain incumbent score");
        r.options_.search_limit=limit;exact(r);
        // Nested callers may already have a topology budget active. A failed
        // group must preserve that active flag just as it restores a false one.
        r.v5_topology_trial_=true;r.options_.search_limit=0;
        r.run_hotspot_rrr(1);
        require(r.v5_topology_trial_,"phase restores an already active topology budget flag");
        require(geometry(r.routes_)==checkpoint && r.demand_==demand,"nested failed groups preserve complete geometry");
        r.v5_topology_trial_=outer_topology_budget;r.options_.search_limit=limit;exact(r);

        // No work is permitted after the shared cutoff, even if the phase has
        // rounds and candidate nets remaining.
        const auto attempted=r.counters_.v8_rrr_groups;
        r.deadline_=std::chrono::steady_clock::now()-std::chrono::seconds(1);
        r.run_hotspot_rrr(3);
        require(r.counters_.v8_rrr_groups==attempted,"expired global deadline prevents new groups");
        require(geometry(r.routes_)==checkpoint && r.demand_==demand,"expired phase preserves complete checkpoint");
        r.deadline_=deadline;exact(r);
    }
};
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("hotspot_rrr_tests OUTPUT_DIRECTORY OFFICIAL_RUNTIME");
        const fs::path directory=argv[1];fs::create_directories(directory);
        std::string tech,def="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 600 600 ) ; GCELLGRID X 0 DO 7 STEP 100 ; GCELLGRID Y 0 DO 7 STEP 100 ; ";
        for(int layer=1;layer<=20;++layer) {
            const auto name=get_layer_name(layer);
            tech+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(layer%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.2 ; END "+name+"\n";
            const std::string track=layer==9?"5 DO 6 STEP 100":"5 DO 60 STEP 10";
            def+="TRACKS X "+track+" LAYER "+name+" ; TRACKS Y "+track+" LAYER "+name+" ; ";
        }
        def+="COMPONENTS 0 ; END COMPONENTS PINS 13 ; ";
        auto pin=[&](std::string name,int layer,int x,int y) {
            def+="- "+name+" + LAYER metal"+std::to_string(layer)+" ( -1 -1 ) ( 1 1 ) + FIXED ( "+std::to_string(x)+" "+std::to_string(y)+" ) N ; ";
        };
        for(int i=0;i<3;++i) {pin("a"+std::to_string(i),1,50+i*100,250);pin("b"+std::to_string(i),1,450,350+i*100);pin("c"+std::to_string(i),12,150+i*100,450);}
        pin("local_a",1,50,50);pin("local_b",1,550,550);pin("local_c",1,50,50);pin("local_d",1,550,550);
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
        heat<<"]}";ThermalParser thermal;
        require(thermal.parse_json(write(directory/"heat.json",heat.str()),parsed.gcell_grid(),parsed.dbu_per_micron(),
                parsed.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),"parse heat");
        RouterOptions options;options.official_model=true;options.official_runtime=argv[2];
        options.graph_options={RoutingGeometryMode::GLOBAL_GUIDE,false,true,true};
        options.official_metrics.score_20261020=true;options.official_metrics.thermal_gain=3;
        options.official_metrics.gcell_width_dbu=parsed.gcell_pitch_x();options.official_metrics.gcell_height_dbu=parsed.gcell_pitch_y();
        options.tsv_policy.resource_mode=TsvResourceMode::GLOBAL_GRID;
        options.rrr_rounds=options.tsv_rounds=options.wirelength_rounds=0;options.tsv_local=false;
        options.v7_cell_cost=true;options.v8_rrr=true;options.time_limit=120;
        NegotiatedRouter router(parsed,lef,thermal,options);HotspotRrrTestAccess::run(router);
        std::cout<<"hotspot_rrr_tests: "<<checks<<" checks PASS\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}