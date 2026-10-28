#include "router/negotiated_router.hpp"
#include "scoring/compat_validation.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace eda;
namespace fs=std::filesystem;
namespace {
size_t checks=0;
void require(bool ok,const std::string& message) {++checks;if(!ok) throw std::runtime_error(message);}
void near(double a,double b,const std::string& message) {
    require(std::abs(a-b)<=1e-10*std::max({1.0,std::abs(a),std::abs(b)}),message);
}
std::string write(const fs::path& path,const std::string& value) {
    std::ofstream out(path);out<<value;out.close();require(bool(out),"fixture write failed");return path.string();
}
}
namespace eda {
struct PinLocalTestAccess {
    static void check(NegotiatedRouter& router) {
        require(router.route_all(),"new-profile fixture routes completely");
        require(compat::validate(router.def_,router.lef_,router.guides()).error_count==0,"new exported geometry is legal");
        const auto before=router.measure();
        near(before.guide.wirelength,double(router.official_wirelength_),"full and incremental wirelength agree");
        near(before.official.squared_normalized_overflow,double(router.official_squared_overflow_),"full and incremental congestion agree");
        const size_t index=1;
        const auto old=router.routes_.at(index);
        const auto demand=router.demand_;
        router.account(index,old,-1);router.routes_[index]=NetRoute{};
        const int pin=router.graph_.node(0,0,1),neighbor=router.graph_.node(1,0,1);
        require(router.pin_exempt(index,pin),"actual same-layer pin cell is exempt");
        require(!router.pin_exempt(0,pin),"another net cannot borrow the pin exemption");
        require(!router.pin_exempt(index,router.graph_.node(0,0,2)),"exemption does not leak into other layers");
        router.guide_history_[pin]=10;
        NetRoute own;own.net_index=index;
        const double exempt_cost=router.edge_cost(neighbor,pin,own);
        near(exempt_cost,router.graph_.edge_length(neighbor,pin)/router.graph_.ell0(),"own-pin cost retains wirelength only");
        require(router.edge_cost(neighbor,pin,NetRoute{})>exempt_cost+9,"other net still pays congestion history");
        PinAccess access;access.length=7;access.guides.emplace_back(0,0,100,100,"metal1");
        near(router.access_cost(access,&own),7/router.graph_.ell0(),"own-pin access retains length and skips demand cost");
        require(router.access_cost(access)>router.access_cost(access,&own)+9,"unowned access pays the same history");
        router.guide_history_[pin]=0;
        router.routes_[index]=old;router.account(index,old,1);
        require(router.demand_==demand,"rip-up and restore exactly recover per-cell demand");
        router.validate_complete();
        near(router.measure().guide.quality,before.guide.quality,"rip-up and restore recover full quality");

        // Test a real score tradeoff: a longer candidate removes overflow while
        // both remain below the complete design's FLUTE reference.
        const int cell=router.graph_.node(1,1,1),saved_demand=router.demand_[cell];
        router.demand_[cell]=int(router.graph_.official_capacity(cell));
        CompleteMetrics base;base.official_model=true;base.score_20261020=true;
        base.guide.reference=double(router.full_reference_)-100;
        NetRoute short_congested,long_clear;
        short_congested.measured.reference=long_clear.measured.reference=100;
        short_congested.measured.wirelength=100;long_clear.measured.wirelength=200;
        short_congested.measured.demand={{cell,10}};
        require(router.official_candidate_key(long_clear,base)<router.official_candidate_key(short_congested,base),
                "platform candidate accepts longer wire for lower congestion");
        router.demand_[cell]=saved_demand;
        CompleteMetrics better=before,worse=before;
        better.guide.quality=.8;worse.guide.quality=.7;
        better.official.zero_capacity_cells=2;worse.official.zero_capacity_cells=1;
        require(NegotiatedRouter::better(better,worse),"new complete comparison follows actual quality");
        better.score_20261020=worse.score_20261020=false;
        require(!NegotiatedRouter::better(better,worse),"legacy zero-capacity priority is preserved");
    }
};
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("v6_router_tests OUTPUT_DIRECTORY OFFICIAL_RUNTIME");
        const fs::path directory=argv[1];fs::create_directories(directory);
        std::string tech,def="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 300 300 ) ; GCELLGRID X 0 DO 4 STEP 100 ; GCELLGRID Y 0 DO 4 STEP 100 ; ";
        for(int l=1;l<=20;++l) {
            const auto name=get_layer_name(l);
            tech+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(l%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.2 ; END "+name+"\n";
            def+="TRACKS X 5 DO 30 STEP 10 LAYER "+name+" ; TRACKS Y 5 DO 30 STEP 10 LAYER "+name+" ; ";
        }
        def+="COMPONENTS 0 ; END COMPONENTS PINS 4 ; "
             "- lo + LAYER metal10 ( -1 -1 ) ( 1 1 ) + FIXED ( 150 150 ) N ; "
             "- hi + LAYER metal11 ( -1 -1 ) ( 1 1 ) + FIXED ( 150 150 ) N ; "
             "- a + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; "
             "- b + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 250 50 ) N ; END PINS "
             "NETS 2 ; - cross ( PIN lo ) ( PIN hi ) ; - local ( PIN a ) ( PIN b ) ; END NETS END DESIGN";
        LefParser lef;require(lef.parse(write(directory/"fixture.lef",tech)),"parse LEF");
        DefParser parsed;require(parsed.parse(write(directory/"fixture.def",def),lef,DefGridMapping::OFFICIAL_FULL),"parse DEF");
        ThermalParser thermal;require(thermal.parse_json(write(directory/"heat.json",
            R"({"meta":{"ambientTemp":25},"nodes":[[5,5,0,30],[15,5,0,31],[25,5,0,32],[5,15,0,33],[15,15,0,34],[25,15,0,35],[5,25,0,36],[15,25,0,37],[25,25,0,38]]})"),
            parsed.gcell_grid(),parsed.dbu_per_micron(),parsed.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),"parse heat");
        RouterOptions options;options.official_model=true;options.official_runtime=argv[2];
        options.graph_options={RoutingGeometryMode::GLOBAL_GUIDE,false,true};
        options.official_metrics.score_20261020=true;
        options.official_metrics.gcell_width_dbu=parsed.gcell_pitch_x();options.official_metrics.gcell_height_dbu=parsed.gcell_pitch_y();
        options.tsv_policy.resource_mode=TsvResourceMode::GLOBAL_GRID;
        options.rrr_rounds=options.tsv_rounds=options.wirelength_rounds=0;options.tsv_local=false;
        NegotiatedRouter router(parsed,lef,thermal,options);PinLocalTestAccess::check(router);
        std::cout<<"v6_router_tests: "<<checks<<" checks PASS\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
