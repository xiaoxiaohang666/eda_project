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
void require(bool value,const std::string& reason) {++checks;if(!value) throw std::runtime_error(reason);}
void near(double a,double b,const std::string& reason) {
    require(std::abs(a-b)<=1e-10*std::max({1.0,std::abs(a),std::abs(b)}),reason);
}
std::string write(const fs::path& path,const std::string& text) {
    std::ofstream out(path);out<<text;out.close();require(bool(out),"write fixture");return path.string();
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
struct TsvLandingRrrTestAccess {
    static int amount(const std::vector<std::pair<int,int>>& demand,int node) {
        const auto found=std::lower_bound(demand.begin(),demand.end(),node,
            [](const auto& entry,int id){return entry.first<id;});
        return found!=demand.end()&&found->first==node?found->second:0;
    }
    static void exact(NegotiatedRouter& r) {
        r.complete_validation_cached_=false;r.validate_complete();
        std::vector<int> demand(r.graph_.size());GuideMap guides;
        for(size_t i=0;i<r.routes_.size();++i) {
            const auto fresh=measure_official_net_guides(r.def_.nets()[i],r.routes_[i].guides,
                r.graph_,r.lef_,r.options_.official_metrics,r.reference_lengths_[i]);
            require(fresh.demand==r.routes_[i].measured.demand,"fresh demand equals cached demand");
            for(auto [node,count]:fresh.demand) demand[size_t(node)]+=count;
            guides[r.def_.nets()[i].name]=r.routes_[i].guides;
        }
        require(demand==r.demand_,"transactional demand equals fresh geometry");
        require(compat::validate(r.def_,r.lef_,guides).error_count==0,"complete exported geometry is legal");
        const auto current=r.measure();r.verify_official_ledger(current);
        near(current.guide.quality,r.best_metrics_.guide.quality,"live score equals incumbent");
    }
    static void inject_passer(NegotiatedRouter& r,size_t passer,int root) {
        r.account(passer,r.routes_[passer],-1);
        auto route=r.routes_[passer];
        require(!route.nodes.empty(),"passer contains a real route");
        // Append a connected, legal but wasteful top-die branch through another
        // net's reserved M11 landing. The phase must reroute the passer, while
        // preserving the TSV owner's complete root reservation and fixed slot.
        const int source=*route.nodes.begin();
        std::vector<int> parent(r.graph_.size(),-1),queue{source};parent[size_t(source)]=source;
        for(size_t next=0;next<queue.size()&&parent[size_t(root)]<0;++next) {
            const int node=queue[next];
            for(int neighbor:r.graph_.neighbors(node)) {
                if(r.graph_.layer(neighbor)<11||parent[size_t(neighbor)]>=0) continue;
                parent[size_t(neighbor)]=node;queue.push_back(neighbor);
            }
        }
        require(parent[size_t(root)]>=0,"passer can reach landing on legal top-die layers");
        int current=root;route.nodes.insert(current);
        while(current!=source) {
            const int previous=parent[size_t(current)];route.nodes.insert(previous);
            route.edges.insert(route_edge(current,previous));current=previous;
        }
        r.encode(passer,route);r.routes_[passer]=std::move(route);r.account(passer,r.routes_[passer],1);
        r.best_metrics_=r.measure();exact(r);
    }
    static void run(NegotiatedRouter& r) {
        require(r.route_all(),"fixture initially routes completely");exact(r);
        size_t cross=r.routes_.size(),passer=r.routes_.size();
        for(size_t i=0;i<r.routes_.size();++i) {
            if(r.routes_[i].slot>=0) cross=i;
            else if(!r.def_.nets()[i].pins.empty()&&r.def_.nets()[i].pins.front().layer_idx>10) passer=i;
        }
        require(cross<r.routes_.size()&&passer<r.routes_.size(),"fixture has a TSV owner and a top-die passer");
        const int slot=r.routes_[cross].slot;
        const int root=r.policy_.slots()[size_t(slot)].upper_node;
        const int reserved=amount(r.routes_[cross].measured.demand,root);
        require(reserved>2,"real track reservation exceeds the historical two-unit root");
        require(amount(r.official_protected_demand(cross,r.routes_[cross]),root)==reserved,
                "fixed-slot ranking protects the complete required TSV track demand");
        inject_passer(r,passer,root);
        require(amount(r.routes_[passer].measured.demand,root)>0,"ordinary passer occupies TSV root");
        require(r.graph_.official_obstacle_demand(root)+r.demand_[size_t(root)]>r.graph_.official_capacity(root),
                "fixture has actual TSV landing congestion");
        const auto owner_geometry=geometry({r.routes_[cross]});
        const auto baseline=geometry(r.routes_);const auto demand=r.demand_;
        const auto cells=r.cell_nets_;const auto edges=r.edge_nets_;
        const auto before=r.best_metrics_;const auto deadline=r.deadline_;
        r.options_.tsv_landing_rrr=true;r.options_.local_repair=false;r.options_.pin_local=false;
        r.options_.fast_initial=false;r.options_.v7_rrr_astar_net_limit=0;
        r.options_.v8_rrr_group_size=8;r.options_.rrr_net_limit=16;r.options_.v7_rrr_net_limit=16;
        const auto limit=r.options_.search_limit;r.options_.search_limit=0;
        const auto rejected=r.counters_.v8_rrr_groups_rejected;
        r.run_hotspot_rrr(1);
        require(r.counters_.v8_rrr_groups_rejected>rejected,"failed focused transaction is actually attempted");
        require(geometry(r.routes_)==baseline&&r.demand_==demand&&r.cell_nets_==cells&&r.edge_nets_==edges,
                "failed focused reroute restores geometry and all resource owners");
        near(r.best_metrics_.guide.quality,before.guide.quality,"failure preserves complete score");exact(r);
        r.options_.search_limit=limit;
        const auto nets=r.counters_.v8_rrr_group_nets,accepted=r.counters_.v8_rrr_groups_accepted;
        r.run_hotspot_rrr(2);
        require(r.counters_.v8_rrr_groups_accepted>accepted,"focused phase accepts real ordinary-Guide repair");
        require(r.counters_.v8_rrr_group_nets-nets==1,"only removable passer enters focused groups");
        require(geometry({r.routes_[cross]})==owner_geometry&&r.routes_[cross].slot==slot,
                "fixed TSV owner and its complete reservation remain unchanged");
        require(amount(r.routes_[passer].measured.demand,root)==0,"accepted repair clears the ordinary root conflict");
        require(r.best_metrics_.official.squared_normalized_overflow<before.official.squared_normalized_overflow,
                "accepted repair reduces actual normalized congestion");
        require(r.best_metrics_.guide.quality>before.guide.quality,"actual complete score improves");
        near(r.best_metrics_.guide.correlation,before.guide.correlation,"fixed TSV repair preserves Pearson correlation");
        require(r.deadline_==deadline,"focused phase preserves enclosing deadline");exact(r);
        const auto groups=r.counters_.v8_rrr_groups;
        r.run_hotspot_rrr(1);
        require(r.counters_.v8_rrr_groups==groups,"clean landing does not queue unrelated nets");exact(r);
    }
};
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("tsv_landing_rrr_tests OUTPUT_DIRECTORY OFFICIAL_RUNTIME");
        const fs::path directory=argv[1];fs::create_directories(directory);
        std::string tech,def="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 180 180 ) ; GCELLGRID X 0 DO 7 STEP 30 ; GCELLGRID Y 0 DO 7 STEP 30 ; ";
        for(int layer=1;layer<=20;++layer) {
            const auto name=get_layer_name(layer);
            tech+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(layer%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.2 ; END "+name+"\n";
            const std::string tracks=layer==11?"5 DO 18 STEP 10":"1 DO 180 STEP 1";
            def+="TRACKS X "+tracks+" LAYER "+name+" ; TRACKS Y "+tracks+" LAYER "+name+" ; ";
        }
        def+="COMPONENTS 0 ; END COMPONENTS PINS 4 ; ";
        const auto pin=[&](const std::string& name,int layer,int x,int y) {
            def+="- "+name+" + LAYER metal"+std::to_string(layer)+" ( -1 -1 ) ( 1 1 ) + FIXED ( "+std::to_string(x)+" "+std::to_string(y)+" ) N ; ";
        };
        pin("bottom",1,15,15);pin("top",13,165,165);
        pin("passer_a",13,15,165);pin("passer_b",13,165,15);
        def+="END PINS NETS 2 ; - cross ( PIN bottom ) ( PIN top ) ; - passer ( PIN passer_a ) ( PIN passer_b ) ; END NETS END DESIGN";
        LefParser lef;require(lef.parse(write(directory/"fixture.lef",tech)),"parse LEF");
        DefParser parsed;require(parsed.parse(write(directory/"fixture.def",def),lef,DefGridMapping::OFFICIAL_FULL),"parse DEF");
        std::ostringstream heat;heat<<"{\"meta\":{\"ambientTemp\":25},\"nodes\":[";
        for(int y=0;y<6;++y) for(int x=0;x<6;++x) {
            if(x||y) heat<<',';
            heat<<'['<<1.5+x*3<<','<<1.5+y*3<<",0,"<<26+x+6*y<<']';
        }
        heat<<"]}";ThermalParser thermal;
        require(thermal.parse_json(write(directory/"heat.json",heat.str()),parsed.gcell_grid(),parsed.dbu_per_micron(),
                parsed.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),"parse heat");
        RouterOptions options;options.official_model=true;options.official_runtime=argv[2];
        options.graph_options={RoutingGeometryMode::GLOBAL_GUIDE,false,true,false,true};
        options.official_metrics.score_20261020=true;options.official_metrics.tsv_track_demand=true;
        options.official_metrics.thermal_gain=3;options.official_metrics.gcell_width_dbu=parsed.gcell_pitch_x();
        options.official_metrics.gcell_height_dbu=parsed.gcell_pitch_y();options.tsv_policy.resource_mode=TsvResourceMode::GLOBAL_GRID;
        options.rrr_rounds=options.tsv_rounds=options.wirelength_rounds=0;options.tsv_local=false;
        options.v7_cell_cost=true;options.v8_rrr=true;options.time_limit=120;
        NegotiatedRouter router(parsed,lef,thermal,options);TsvLandingRrrTestAccess::run(router);
        options.v9_incremental_rrr=true;NegotiatedRouter checkpointed(parsed,lef,thermal,options);
        TsvLandingRrrTestAccess::run(checkpointed);
        std::cout<<"tsv_landing_rrr_tests: "<<checks<<" checks PASS\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
