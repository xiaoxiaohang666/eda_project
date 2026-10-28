#include "router/negotiated_router.hpp"
#include "validation/gcell_encoding.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace eda;
namespace fs=std::filesystem;
namespace {
size_t checks=0;
void require(bool ok,const std::string& why) {
    ++checks;if(!ok) throw std::runtime_error(why);
}
std::string write(const fs::path& path,const std::string& contents) {
    std::ofstream out(path);out<<contents;out.close();
    require(bool(out),"fixture write");return path.string();
}
}
namespace eda {
struct PinLocalTestAccess {
    static void check(NegotiatedRouter& router,const fs::path& directory) {
        for(size_t index=0;index<3;++index) {
            auto candidates=router.pin_local_candidates(index);
            require(!candidates.empty(),"grid-local candidates exist");
            for(const auto& candidate:candidates) {
                require(!candidate.explicit_geometry && candidate.accesses.empty(),
                        "global candidate has no physical geometry bypass");
                std::string error;
                require(validate_gcell_encoding(router.def_.nets()[index],router.def_.die_area(),
                            router.graph_.grid(),router.lef_,candidate.guides,error),error);
                double length=0;int vias=0;
                for(auto edge:candidate.edges) {
                    const auto [a,b]=edge_nodes(edge);
                    require(router.graph_.legal_edge(a,b),"local candidate reserves legal edges");
                    length+=router.graph_.edge_length(a,b);
                    vias+=router.graph_.layer(a)!=router.graph_.layer(b);
                }
                require(std::abs(length-candidate.length)<1e-8 && vias==candidate.vias,
                        "length and vias counted once from graph edges");
                const auto measured=measure_official_net_guides(router.def_.nets()[index],candidate.guides,
                    router.graph_,router.lef_,router.options_.official_metrics,router.reference_lengths_[index]);
                require(measured.demand==candidate.measured.demand &&
                        measured.wirelength==candidate.measured.wirelength,
                        "candidate ledger and historical model use normalized guides");
                if(index==0) require(candidate.guides.size()==1 && candidate.vias==0,
                                    "same-cell same-layer endpoints need one cell and no via detour");
            }
            if(index==0) {
                auto bad=candidates.front();PinAccess leftover;
                leftover.guides.emplace_back(3550,4250,3650,4350,"metal11");
                bad.accesses.push_back(leftover);
                bool rejected=false;
                try {router.encode(index,bad);} catch(const std::runtime_error&) {rejected=true;}
                require(rejected,"leftover physical access aperture cannot bypass candidate guard");
            }
        }
        require(router.route_all(),"full grid-local fixture routes");
        require(router.counters_.tsv_local_candidates>0,"shared helper exercised by TSV recovery");
        const auto demand=router.demand_;
        const auto before=router.measure();
        for(size_t i=0;i<router.routes_.size();++i) {
            const auto old=router.routes_[i];
            router.account(i,old,-1);router.routes_[i]=NetRoute{};
            router.routes_[i]=old;router.account(i,old,1);
            require(router.demand_==demand,"rip-up and restore exactly recover grid demand");
        }
        router.validate_complete();
        require(std::abs(router.measure().guide.quality-before.guide.quality)<1e-12,
                "rollback preserves historical model metric");
        for(size_t i=0;i<router.routes_.size();++i) {
            std::string error;
            require(validate_gcell_encoding(router.def_.nets()[i],router.def_.die_area(),router.graph_.grid(),
                    router.lef_,router.routes_[i].guides,error),error);
            require(router.policy_.validate_mapping(router.def_.nets()[i],router.routes_[i].guides,
                    router.routes_[i].slot,error),"TSV landing survives grid normalization: "+error);
        }
        require(router.export_guide((directory/"fixture.guide").string()),"serialized grid-guide readback");
    }
};
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("grid_local_tests OUTPUT_DIR FLUTE_RUNTIME");
        const fs::path directory=argv[1];fs::create_directories(directory);
        std::ostringstream tech,design,heat;
        design<<"UNITS DISTANCE MICRONS 1000 ; DIEAREA ( 0 0 ) ( 12000 12000 ) ; "
              <<"GCELLGRID X 0 DO 5 STEP 3000 ; GCELLGRID Y 0 DO 5 STEP 3000 ; ";
        for(int l=1;l<=20;++l) {
            const auto name=get_layer_name(l);
            tech<<"LAYER "<<name<<" TYPE ROUTING ; DIRECTION "<<(l%2?"VERTICAL":"HORIZONTAL")
                <<" ; WIDTH 0.1 ; END "<<name<<'\n';
            design<<"TRACKS X 50 DO 120 STEP 100 LAYER "<<name<<" ; "
                  <<"TRACKS Y 50 DO 120 STEP 100 LAYER "<<name<<" ; ";
        }
        design<<"COMPONENTS 0 ; END COMPONENTS PINS 8 ; ";
        struct Endpoint {const char* name;int x,y,layer;};
        for(const auto p:{Endpoint{"a",3600,4300,11},Endpoint{"b",3900,4700,11},
                         Endpoint{"c",2400,2700,11},Endpoint{"d",10300,7200,11},
                         Endpoint{"e",4300,2400,13},Endpoint{"f",7500,10300,16},
                         Endpoint{"lo",1400,7300,3},Endpoint{"hi",10400,2300,14}})
            design<<"- "<<p.name<<" + LAYER "<<get_layer_name(p.layer)
                  <<" ( -20 -20 ) ( 20 20 ) + FIXED ( "<<p.x<<' '<<p.y<<" ) N ; ";
        design<<"END PINS NETS 4 ; - same ( PIN a ) ( PIN b ) ; - bend ( PIN c ) ( PIN d ) ; "
              <<"- stack ( PIN e ) ( PIN f ) ; - cross ( PIN lo ) ( PIN hi ) ; END NETS END DESIGN";
        heat<<"{\"meta\":{\"ambientTemp\":25},\"nodes\":[";
        for(int r=0;r<4;++r) for(int c=0;c<4;++c) {
            if(r||c) heat<<',';
            heat<<'['<<1.5+3*c<<','<<1.5+3*r<<",0,"<<30+c+4*r<<']';
        }
        heat<<"]}";
        LefParser lef;DefParser def;ThermalParser thermal;
        require(lef.parse(write(directory/"fixture.lef",tech.str())),"parse grid fixture LEF");
        require(def.parse(write(directory/"fixture.def",design.str()),lef,DefGridMapping::OFFICIAL_FULL),"parse grid fixture DEF");
        require(thermal.parse_json(write(directory/"fixture.thermal.json",heat.str()),def.gcell_grid(),
                    def.dbu_per_micron(),def.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),"parse heat");
        RouterOptions options;options.official_model=true;options.official_runtime=argv[2];
        options.graph_options={RoutingGeometryMode::GLOBAL_GUIDE,false,true};
        options.official_metrics.score_20261020=true;
        options.official_metrics.gcell_width_dbu=options.official_metrics.gcell_height_dbu=3000;
        options.tsv_policy.resource_mode=TsvResourceMode::GLOBAL_GRID;
        options.rrr_rounds=options.tsv_rounds=options.wirelength_rounds=0;
        NegotiatedRouter router(def,lef,thermal,options);
        PinLocalTestAccess::check(router,directory);
        std::cout<<"grid_local_tests: "<<checks<<" engineering checks passed; official scorer not run\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
