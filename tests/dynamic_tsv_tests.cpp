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
namespace detail {
// Exercise the production pool retrieval and live-Q filter, without exposing
// this implementation detail as part of NegotiatedRouter's public interface.
std::vector<TsvMoveCandidate> dynamic_tsv_candidates(const TsvMoveSearch&,
    const RoutingGraph&,const TsvPolicy&,const std::vector<int>&,size_t,int,double,TsvMoveSearchOptions,bool);
}
struct DynamicTsvTestAccess {
    static void exact(NegotiatedRouter& r) {
        r.complete_validation_cached_=false;r.validate_complete();
        std::vector<NetGuideMetrics> measured;std::vector<int> demand(r.graph_.size());GuideMap guides;
        std::set<int> slots;
        for(size_t i=0;i<r.routes_.size();++i) {
            const auto& route=r.routes_[i];
            if(route.slot>=0) require(slots.insert(route.slot).second,"unique TSV ownership");
            guides[r.def_.nets()[i].name]=route.guides;
            measured.push_back(measure_official_net_guides(r.def_.nets()[i],route.guides,r.graph_,r.lef_,
                r.options_.official_metrics,r.reference_lengths_[i]));
            require(measured.back().demand==route.measured.demand,"fresh demand equals route cache");
            require(measured.back().tsv_cells==route.measured.tsv_cells,"fresh TSV cells equal route cache");
            for(auto [n,d]:measured.back().demand) demand[size_t(n)]+=d;
        }
        require(demand==r.demand_,"fresh geometry demand equals aggregate ledger");
        require(compat::validate(r.def_,r.lef_,guides).error_count==0,"serialized Guides remain legal");
        std::vector<const NetGuideMetrics*> views;for(const auto& net:measured) views.push_back(&net);
        const auto fresh=aggregate_official_metrics(views,demand,r.graph_,r.thermal_,r.options_.official_metrics,false);
        const auto current=r.measure();r.verify_official_ledger(current);
        near(fresh.guide.quality,current.guide.quality,"independent full score equals ledger");
        near(fresh.guide.correlation,current.guide.correlation,"independent Pearson equals ledger");
        near(current.guide.quality,r.best_metrics_.guide.quality,"best checkpoint matches current solution");
    }
    static void risk_order(NegotiatedRouter& r) {
        std::vector<int> assignment;size_t index=r.routes_.size();
        for(size_t i=0;i<r.routes_.size();++i) {
            assignment.push_back(r.routes_[i].slot);
            if(index==r.routes_.size() && r.routes_[i].slot>=0) index=i;
        }
        require(index<r.routes_.size(),"risk test has a movable TSV owner");
        TsvMoveSearch search(r.def_,r.graph_,r.policy_,r.thermal_,assignment);
        TsvMoveSearchOptions options;options.max_candidates=1;
        options.thermal_gain=r.options_.official_metrics.thermal_gain;
        for(bool probe:{false,true}) {
            const auto before=detail::dynamic_tsv_candidates(search,r.graph_,r.policy_,r.demand_,index,
                assignment[index],r.best_metrics_.official.squared_normalized_overflow,options,probe);
            require(before.size()==1,"final candidate cap is respected");
            const int risky=before.front().slot;
            const int node=r.policy_.slots()[size_t(risky)].upper_node;
            require(r.graph_.official_capacity(node)>0,"risk fixture has positive landing capacity");
            auto congestion=r.demand_;congestion[size_t(node)]+=1000000;
            const auto after=detail::dynamic_tsv_candidates(search,r.graph_,r.policy_,congestion,index,
                assignment[index],r.best_metrics_.official.squared_normalized_overflow,options,probe);
            require(after.size()==1 && after.front().slot!=risky,
                "live congestion selects a safer raw-pool candidate before final truncation");
        }
        exact(r);
    }
    static void run(NegotiatedRouter& r) {
        require(r.route_all(),"fixture routes completely");exact(r);risk_order(r);
        const auto saved=r.options_;const auto outer_deadline=r.deadline_;
        const auto baseline=geometry(r.routes_);const auto demand=r.demand_;
        const auto owners=r.cell_nets_;const auto edge_owners=r.edge_nets_;
        r.options_.v8_tsv_dynamic=true;r.options_.tsv_rounds=3;
        r.options_.v7_tsv_nets=1;r.options_.v7_tsv_candidates=12;
        r.options_.v8_tsv_probe_candidates=2;r.options_.v7_tsv_seconds=20;
        r.options_.search_limit=0;
        r.optimize_dynamic_tsv();
        require(r.counters_.v8_tsv_probed_nets==3,"initial probe covers every TSV net, not just batch size");
        require(r.counters_.v8_tsv_unique_nets==3,"three one-net batches visit three different nets");
        require(r.counters_.v8_tsv_repeat_visits==0,"visited mask avoids repeating static shortlist");
        for(size_t q=0;q<3;++q) require(r.counters_.v8_tsv_queue_visits[q]>0,"each objective queue receives measured routing work");
        require(r.counters_.v7_tsv_attempted>0 && r.counters_.v7_tsv_rollbacks>0,"failed routed candidates exercise rollback");
        require(geometry(r.routes_)==baseline && r.demand_==demand,"failed candidates restore geometry and demand");
        require(r.cell_nets_==owners && r.edge_nets_==edge_owners,"failed candidates restore reverse ownership");
        require(r.deadline_==outer_deadline && !r.v5_topology_trial_,"phase restores enclosing budget");exact(r);

        r.options_.search_limit=saved.search_limit;r.options_.tsv_rounds=3;r.options_.v7_tsv_nets=3;
        const auto before=r.best_metrics_;const auto accepted=r.counters_.v7_tsv_accepted;
        r.optimize_dynamic_tsv();
        require(r.counters_.v7_tsv_accepted>accepted,"dynamic scheduling accepts an actual rerouted thermal move");
        require(r.best_metrics_.guide.quality>before.guide.quality+1e-10,"accepted moves improve exact complete score");
        require(r.counters_.v8_tsv_refreshes>0,"accepted occupancy changes refresh related net probes");exact(r);

        const auto checkpoint=geometry(r.routes_);const auto live_demand=r.demand_;
        const auto stops=r.counters_.v7_tsv_budget_stops;
        r.options_.v7_tsv_seconds=0;r.optimize_dynamic_tsv();
        require(r.counters_.v7_tsv_budget_stops>stops,"zero phase budget is reported");
        require(geometry(r.routes_)==checkpoint && r.demand_==live_demand,"expired phase retains last complete solution");
        require(r.deadline_==outer_deadline && !r.v5_topology_trial_,"timeout restores outer deadline and flags");exact(r);
        r.options_=saved;
    }
};
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("dynamic_tsv_tests OUTPUT_DIRECTORY OFFICIAL_RUNTIME");
        const fs::path dir=argv[1];fs::create_directories(dir);
        std::string tech,def="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 600 600 ) ; GCELLGRID X 0 DO 7 STEP 100 ; GCELLGRID Y 0 DO 7 STEP 100 ; ";
        for(int layer=1;layer<=20;++layer) {
            const auto name=get_layer_name(layer);
            tech+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(layer%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.2 ; END "+name+"\n";
            const std::string tracks=layer==9?"5 DO 6 STEP 100":"5 DO 60 STEP 10";
            def+="TRACKS X "+tracks+" LAYER "+name+" ; TRACKS Y "+tracks+" LAYER "+name+" ; ";
        }
        def+="COMPONENTS 0 ; END COMPONENTS PINS 13 ; ";
        const auto pin=[&](const std::string& name,int layer,int x,int y) {
            def+="- "+name+" + LAYER metal"+std::to_string(layer)+" ( -1 -1 ) ( 1 1 ) + FIXED ( "+std::to_string(x)+" "+std::to_string(y)+" ) N ; ";
        };
        for(int i=0;i<3;++i) {pin("a"+std::to_string(i),1,50+i*100,250);pin("b"+std::to_string(i),1,450,350+i*100);pin("c"+std::to_string(i),12,150+i*100,450);}
        pin("local_a",1,50,50);pin("local_b",1,550,550);pin("local_c",1,50,50);pin("local_d",1,550,550);
        def+="END PINS NETS 5 ; ";
        for(int i=0;i<3;++i) def+="- cross"+std::to_string(i)+" ( PIN a"+std::to_string(i)+" ) ( PIN b"+std::to_string(i)+" ) ( PIN c"+std::to_string(i)+" ) ; ";
        def+="- local ( PIN local_a ) ( PIN local_b ) ; - duplicate ( PIN local_c ) ( PIN local_d ) ; END NETS END DESIGN";
        LefParser lef;require(lef.parse(write(dir/"fixture.lef",tech)),"parse technology");
        DefParser parsed;require(parsed.parse(write(dir/"fixture.def",def),lef,DefGridMapping::OFFICIAL_FULL),"parse design");
        std::ostringstream heat;heat<<"{\"meta\":{\"ambientTemp\":25},\"nodes\":[";
        for(int y=0;y<6;++y) for(int x=0;x<6;++x) {
            if(x||y) heat<<',';
            heat<<'['<<5+x*10<<','<<5+y*10<<",0,"<<26+x+6*y<<']';
        }
        heat<<"]}";
        ThermalParser thermal;require(thermal.parse_json(write(dir/"heat.json",heat.str()),parsed.gcell_grid(),
            parsed.dbu_per_micron(),parsed.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),"parse heat");
        RouterOptions options;options.official_model=true;options.official_runtime=argv[2];
        options.graph_options={RoutingGeometryMode::GLOBAL_GUIDE,false,true,true};
        options.official_metrics.score_20261020=true;options.official_metrics.thermal_gain=3;
        options.official_metrics.gcell_width_dbu=parsed.gcell_pitch_x();options.official_metrics.gcell_height_dbu=parsed.gcell_pitch_y();
        options.tsv_policy.resource_mode=TsvResourceMode::GLOBAL_GRID;
        options.rrr_rounds=options.tsv_rounds=options.wirelength_rounds=0;options.tsv_local=false;
        options.v7_cell_cost=true;options.time_limit=120;
        NegotiatedRouter router(parsed,lef,thermal,options);DynamicTsvTestAccess::run(router);
        std::cout<<"dynamic_tsv_tests: "<<checks<<" checks PASS\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
