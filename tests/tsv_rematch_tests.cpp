#include "router/negotiated_router.hpp"
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
std::string write(const fs::path& p,const std::string& text) {
    std::ofstream out(p);out<<text;out.close();require(bool(out),"write fixture");return p.string();
}
std::string geometry(const std::vector<NetRoute>& routes) {
    std::ostringstream out;
    for(const auto& route:routes) {
        out<<route.slot<<';';
        for(const auto& g:route.guides) out<<g.layer<<','<<g.x1<<','<<g.y1<<','<<g.x2<<','<<g.y2<<';';
        out<<'\n';
    }
    return out.str();
}
}
namespace eda {
struct TsvRematchTestAccess {
    static std::set<int> occupied(const NegotiatedRouter& r) {
        std::set<int> result;for(const auto& route:r.routes_) if(route.slot>=0) result.insert(route.slot);return result;
    }
    static void exact(NegotiatedRouter& r) {
        r.complete_validation_cached_=false;r.validate_complete();const auto full=r.measure();r.verify_official_ledger(full);
        near(full.guide.quality,r.best_metrics_.guide.quality,"checkpoint score equals actual aggregate");
        near(full.official.thermal_score,r.best_metrics_.official.thermal_score,"checkpoint thermal score equals aggregate");
        std::vector<int> demand(r.graph_.size());
        for(size_t i=0;i<r.routes_.size();++i) {
            const auto actual=measure_official_net_guides(r.def_.nets()[i],r.routes_[i].guides,r.graph_,r.lef_,
                r.options_.official_metrics,r.reference_lengths_[i]);
            require(actual.demand==r.routes_[i].measured.demand,"encoded demand agrees with route cache");
            near(actual.wirelength,r.routes_[i].measured.wirelength,"encoded length agrees with route cache");
            for(auto [node,count]:actual.demand) demand[node]+=count;
        }
        require(demand==r.demand_,"aggregate ledger agrees with encoded geometry");
    }
    static std::vector<int> target_slots(const NegotiatedRouter& r) {
        std::vector<int> result;
        for(size_t i=0;i<r.routes_.size();++i) {
            const auto p=r.def_.nets()[i].pins.front().coord;int slot=-1;
            for(const auto& candidate:r.policy_.slots())
                if(r.graph_.col(candidate.lower_node)==r.graph_.grid().get_col(p.x) &&
                   r.graph_.row(candidate.lower_node)==r.graph_.grid().get_row(p.y)) slot=candidate.id;
            require(slot>=0,"pin cell has a global TSV candidate");result.push_back(slot);
        }
        return result;
    }
    static void install_cycle(NegotiatedRouter& r,const std::vector<int>& slots) {
        for(size_t i=0;i<r.routes_.size();++i) r.account(i,r.routes_[i],-1);
        for(size_t i=0;i<r.routes_.size();++i) {
            r.routes_[i]=r.build_route(i,slots[(i+1)%slots.size()]);r.account(i,r.routes_[i],1);
        }
        r.best_metrics_=r.measure();exact(r);
    }
    static void run(NegotiatedRouter& r) {
        require(r.route_all(),"rematch fixture routes completely");
        const auto slots=target_slots(r);
        install_cycle(r,slots);
        const auto before=r.best_metrics_;const auto occupied_before=occupied(r);
        std::vector<int> old;for(const auto& route:r.routes_) old.push_back(route.slot);
        const auto deadline=r.deadline_;
        r.options_.v8_tsv_rematch=true;r.options_.v8_tsv_rematch_group_size=6;
        r.options_.v8_tsv_rematch_nets=64;r.options_.v8_tsv_rematch_seconds=20;
        r.rematch_tsv_ownership();
        require(r.counters_.v8_rematch_groups==1,"three-net group is considered jointly");
        require(r.counters_.v8_rematch_candidates>0,"real candidate routes were built");
        require(r.counters_.v8_rematch_accepted==1,"one atomic group improvement is accepted");
        size_t changed=0;for(size_t i=0;i<old.size();++i) changed+=r.routes_[i].slot!=old[i];
        require(changed==3,"exact assignment accepts a three-net cycle in one transaction");
        require(occupied(r)==occupied_before,"accepted rematch preserves occupied TSV set");
        require(r.best_metrics_.guide.wirelength<before.guide.wirelength,"accepted rematch recovers actual encoded length");
        require(r.best_metrics_.guide.quality>before.guide.quality,"accepted rematch improves full weighted score");
        near(r.best_metrics_.official.thermal_score,before.official.thermal_score,"rematch preserves exact thermal score");
        require(r.deadline_==deadline && !r.v5_topology_trial_,"phase restores outer deadline and active flag");
        exact(r);

        // Bounded actual access cost must also recover the deliberately
        // permuted assignment without changing occupancy or thermal score.
        install_cycle(r,slots);const auto v9_before=r.best_metrics_;
        const auto v9_accepted=r.counters_.v8_rematch_accepted;
        r.options_.v9_access_rematch=true;r.options_.v9_rematch_edges=12;
        // Sanitizers must validate matching semantics without a Release-speed assumption.
        r.options_.v9_rematch_access_seconds=.2;
        r.options_.v8_tsv_rematch_seconds=40;
        r.rematch_tsv_ownership();
        require(r.counters_.v9_rematch_access_queries>0,"V9 ranks candidates using bounded two-sided access searches");
        require(r.counters_.v8_rematch_accepted>v9_accepted,"V9 access rematching accepts an actual group improvement");
        require(r.best_metrics_.guide.wirelength<v9_before.guide.wirelength,"V9 rematch reduces actual encoded length");
        require(occupied(r)==occupied_before,"V9 rematch preserves occupied TSV set");
        near(r.best_metrics_.official.thermal_score,v9_before.official.thermal_score,"V9 rematch preserves exact thermal score");
        exact(r);r.options_.v9_access_rematch=false;

        // Construction fails after group rip-up, requiring the entire set of
        // routes, reverse owners and score demand to be restored together.
        install_cycle(r,slots);
        const auto original=geometry(r.routes_);const auto demand=r.demand_;
        const auto cells=r.cell_nets_;const auto edges=r.edge_nets_;
        const auto saved=r.options_;const auto rollback_count=r.counters_.v8_rematch_rollbacks;
        r.options_.search_limit=0;r.options_.v8_tsv_rematch_seconds=40;
        r.rematch_tsv_ownership();
        require(r.counters_.v8_rematch_rollbacks>rollback_count,"failed construction executes group rollback");
        require(geometry(r.routes_)==original && r.demand_==demand,"failed group restores exact routes and demand");
        require(r.cell_nets_==cells && r.edge_nets_==edges,"failed group restores reverse ownership");
        require(occupied(r)==occupied_before,"failed group restores occupied TSV set");exact(r);
        r.options_=saved;

        const auto count=r.counters_.v8_rematch_candidates;
        r.options_.v8_tsv_rematch_seconds=r.counters_.v8_rematch_seconds;
        r.rematch_tsv_ownership();
        require(r.counters_.v8_rematch_candidates==count,"exhausted stage budget starts no trial");
        require(geometry(r.routes_)==original && r.demand_==demand,"exhausted stage preserves complete incumbent");
        r.options_=saved;r.deadline_=std::chrono::steady_clock::now()-std::chrono::seconds(1);
        r.rematch_tsv_ownership();
        require(r.counters_.v8_rematch_candidates==count,"expired global deadline starts no trial");
        require(geometry(r.routes_)==original && r.demand_==demand,"expired global deadline preserves incumbent");
        r.deadline_=deadline;exact(r);
    }
};
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("tsv_rematch_tests OUTPUT_DIRECTORY OFFICIAL_RUNTIME");
        const fs::path dir=argv[1];fs::create_directories(dir);
        std::string tech,def="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 600 600 ) ; GCELLGRID X 0 DO 7 STEP 100 ; GCELLGRID Y 0 DO 7 STEP 100 ; ";
        for(int l=1;l<=20;++l) {
            const auto name=get_layer_name(l);
            tech+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(l%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.2 ; END "+name+"\n";
            def+="TRACKS X 5 DO 60 STEP 10 LAYER "+name+" ; TRACKS Y 5 DO 60 STEP 10 LAYER "+name+" ; ";
        }
        def+="COMPONENTS 0 ; END COMPONENTS PINS 6 ; ";
        const std::array<std::pair<int,int>,3> positions{{{50,50},{450,50},{250,450}}};
        for(size_t i=0;i<positions.size();++i) for(int side=0;side<2;++side) {
            const auto [x,y]=positions[i];
            def+="- p"+std::to_string(i)+(side?"t":"b")+" + LAYER metal"+std::to_string(side?14:3)+
                 " ( -1 -1 ) ( 1 1 ) + FIXED ( "+std::to_string(x)+" "+std::to_string(y)+" ) N ; ";
        }
        def+="END PINS NETS 3 ; ";
        for(size_t i=0;i<positions.size();++i)
            def+="- cross"+std::to_string(i)+" ( PIN p"+std::to_string(i)+"b ) ( PIN p"+std::to_string(i)+"t ) ; ";
        def+="END NETS END DESIGN";
        LefParser lef;require(lef.parse(write(dir/"fixture.lef",tech)),"parse LEF");
        DefParser parsed;require(parsed.parse(write(dir/"fixture.def",def),lef,DefGridMapping::OFFICIAL_FULL),"parse DEF");
        std::ostringstream heat;heat<<"{\"meta\":{\"ambientTemp\":25},\"nodes\":[";
        for(int y=0;y<6;++y) for(int x=0;x<6;++x) {
            if(x||y) heat<<',';
            heat<<'['<<5+10*x<<','<<5+10*y<<",0,"<<26+x+6*y<<']';
        }
        heat<<"]}";
        ThermalParser thermal;require(thermal.parse_json(write(dir/"heat.json",heat.str()),parsed.gcell_grid(),
            parsed.dbu_per_micron(),parsed.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),"parse heat");
        RouterOptions options;options.official_model=true;options.official_runtime=argv[2];
        options.graph_options={RoutingGeometryMode::GLOBAL_GUIDE,false,true,false,true};
        options.official_metrics.score_20261020=true;options.official_metrics.thermal_gain=3;
        options.official_metrics.gcell_width_dbu=parsed.gcell_pitch_x();options.official_metrics.gcell_height_dbu=parsed.gcell_pitch_y();
        options.tsv_policy.resource_mode=TsvResourceMode::GLOBAL_GRID;
        options.rrr_rounds=options.tsv_rounds=options.wirelength_rounds=0;options.tsv_local=false;
        options.v7_cell_cost=true;options.time_limit=120;
        NegotiatedRouter router(parsed,lef,thermal,options);TsvRematchTestAccess::run(router);
        std::cout<<"tsv_rematch_tests: "<<checks<<" checks PASS\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
