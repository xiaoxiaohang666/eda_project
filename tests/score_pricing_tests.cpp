#include "router/negotiated_router.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>

using namespace eda;
namespace fs=std::filesystem;
namespace {
size_t checks=0;
void require(bool value,const std::string& reason) {
    ++checks;if (!value) throw std::runtime_error(reason);
}
void near(long double a,long double b,const std::string& reason,long double tolerance=2e-10L) {
    require(std::isfinite(a) && std::isfinite(b) &&
        std::abs(a-b)<=tolerance*std::max({1.0L,std::abs(a),std::abs(b)}),reason);
}
std::string write(const fs::path& file,const std::string& contents) {
    std::ofstream out(file);out<<contents;out.close();
    require(bool(out),"could not write score pricing fixture");return file.string();
}
std::string technology() {
    std::string text;
    for (int layer=1;layer<=20;++layer) {
        const auto name=get_layer_name(layer);
        text+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(layer%2?"HORIZONTAL":"VERTICAL")+
            " ; WIDTH 0.2 ; END "+name+"\n";
    }
    return text;
}
std::string design() {
    std::string text="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 300 300 ) ; "
        "GCELLGRID X 0 DO 4 STEP 100 ; GCELLGRID Y 0 DO 4 STEP 100 ; ";
    for (int layer=1;layer<=20;++layer) {
        const auto name=get_layer_name(layer);
        text+="TRACKS X 5 DO 30 STEP 10 LAYER "+name+" ; TRACKS Y 5 DO 30 STEP 10 LAYER "+name+" ; ";
    }
    return text+"COMPONENTS 0 ; END COMPONENTS PINS 2 ; "
        "- a + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; "
        "- b + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 250 50 ) N ; "
        "END PINS NETS 1 ; - local ( PIN a ) ( PIN b ) ; END NETS END DESIGN";
}
// Independent public W/C formula, in score points. Finite differences below
// derive the price from score changes rather than duplicating its derivative.
long double score(long double reference,long double length,long double squared) {
    const long double ratio=length/reference;
    return 45*std::min(ratio,1/ratio)+30/std::sqrt(1+squared);
}
}
namespace eda {
struct PinLocalTestAccess {
    static void state(NegotiatedRouter& router,long double reference,long double routed_length,
                      long double routed_reference,long double squared,bool advance=true) {
        router.options_.official_model=true;router.options_.official_score_pricing=true;
        router.full_reference_=reference;router.official_wirelength_=routed_length;
        router.official_reference_=routed_reference;router.official_squared_overflow_=squared;
        if (advance) ++router.state_generation_;
    }
    static void algebra(NegotiatedRouter& router) {
        require(router.state_generation_==0 && router.pricing_updates_==0,"fresh router has not priced generation zero");
        state(router,1000000,1000000,1000000,0,false);
        near(router.official_congestion_price(),1000000.0L/(300*router.graph_.ell0()),"initial generation zero computes a nondefault price");
        require(router.pricing_updates_==1 && router.pricing_generation_==0,"sentinel prevents a false initial cache hit");
        struct Case { long double reference,wirelength,routed_reference,squared,estimated_length; };
        const Case cases[]{{1000000,1400000,1000000,.2L,1400000},
                           {1000000,400000,300000,8,1100000},
                           {1000000,2000000,1000000,200,2000000}};
        for (const auto& one:cases) {
            state(router,one.reference,one.wirelength,one.routed_reference,one.squared);
            const long double length=one.estimated_length,hl=length*1e-5L,hs=(1+one.squared)*1e-5L;
            const long double wl_loss=-(score(one.reference,length+hl,one.squared)-
                score(one.reference,length-hl,one.squared))/(2*hl);
            const long double congestion_loss=-(score(one.reference,length,one.squared+hs)-
                score(one.reference,length,one.squared-hs))/(2*hs);
            const long double finite_difference_price=congestion_loss/(100*wl_loss*router.graph_.ell0());
            require(finite_difference_price>.01L && finite_difference_price<10000,"derivative case avoids price clipping");
            near(router.official_congestion_price(),finite_difference_price,"price matches independent finite differences of official W/C",2e-7L);
        }
        state(router,1000000,100,1000000,0);
        const double below=router.official_congestion_price();
        state(router,1000000,1000000,1000000,0);
        near(router.official_congestion_price(),below,"estimated length below R clamps to the R boundary");
        state(router,1000000,1000000,1000000,-1e-12L);
        near(router.official_congestion_price(),below,"negative squared-overflow roundoff clamps to zero");
        state(router,0,1000000,0,100);
        near(router.official_congestion_price(),1,"zero total reference falls back to legacy price one");
        state(router,1000000,1000000,1000000,1e12L);
        near(router.official_congestion_price(),.01L,"severe aggregate congestion clips the price at its positive floor");
        state(router,1000000,1e12L,1000000,0);
        near(router.official_congestion_price(),10000,"large estimated wirelength clips the price at its ceiling");
        const auto updates=router.pricing_updates_;
        router.options_.official_score_pricing=false;
        near(router.official_congestion_price(),1,"disabled pricing returns the legacy multiplier");
        require(router.pricing_updates_==updates,"disabled pricing does not refresh the cache");
        router.options_.official_score_pricing=true;router.options_.official_model=false;
        near(router.official_congestion_price(),1,"physical model ignores official score pricing");
        require(router.pricing_updates_==updates,"physical model does not refresh the official price cache");
        require(router.congestion_price_min_==.01 && router.congestion_price_max_==10000,"reported price bounds reflect actual clipped queries");
        router.options_.official_model=true;router.options_.congestion_price_cap=1e9;
        state(router,1000000,1e8L,1000000,0);
        require(router.official_congestion_price()>10000,"higher experimental ceiling preserves previously compressed prices");
        router.options_.congestion_price_cap=10000;
        router.options_.score_unit_cost=true;
        state(router,1000000,1400000,1000000,.3L);
        const int n=router.graph_.node(1,0,1);
        router.demand_[n]=20;router.present_=1;router.guide_history_[n]=0;
        const long double dq=.01L*(1.1L*1.1L-1);
        near(router.official_cell_search_cost(n),30/std::sqrt(1.3L)-30/std::sqrt(1.3L+dq),
            "score-unit congestion cost equals independent finite C loss",2e-10L);
        near(router.official_search_scale(),45e6L*router.graph_.ell0()/(1400000.0L*1400000),
            "score-unit length scale is weighted W derivative per ell0");
        router.demand_[n]=0;router.options_.score_unit_cost=false;
    }
    static void generation(NegotiatedRouter& router) {
        state(router,1000000,0,0,0);
        const auto first_generation=router.state_generation_;
        const auto before=router.pricing_updates_;
        const double initial=router.official_congestion_price();
        near(router.official_congestion_price(),initial,"same generation returns an identical price");
        require(router.pricing_updates_==before+1,"repeated same-generation queries compute the price once");
        NetRoute probe;probe.measured.wirelength=2000000;probe.measured.reference=1000000;
        probe.measured.demand={{router.graph_.node(1,0,1),1}};
        router.account(0,probe,1);
        require(router.state_generation_==first_generation+1,"a successful real account advances the pricing generation");
        const double accounted=router.official_congestion_price();
        near(accounted,4*initial,"accounted wirelength updates the next generation's score derivative");
        require(router.pricing_updates_==before+2,"successful account triggers one lazy price refresh");
        const auto cached=std::tuple(router.state_generation_,router.pricing_generation_,router.pricing_updates_,router.congestion_price_);
        bool rejected=false;try {router.account(0,probe,1);} catch(const std::logic_error&) {rejected=true;}
        require(rejected,"duplicate ownership account is rejected before mutation");
        near(router.official_congestion_price(),accounted,"rejected account retains the same cached price");
        require(std::tuple(router.state_generation_,router.pricing_generation_,router.pricing_updates_,router.congestion_price_)==cached,
            "rejected account preserves generation and its cached price proof");
        router.account(0,probe,-1);
        near(router.official_congestion_price(),initial,"real ripup restores the original derivative at a new generation");
        require(router.state_generation_==first_generation+2 && router.pricing_updates_==before+3,
            "ripup invalidates only through its successful generation change");
    }
    static void costs_and_search(NegotiatedRouter& router) {
        state(router,10000,10000,10000,0);
        const int a=router.graph_.node(0,0,1),b=router.graph_.node(1,0,1),target=router.graph_.node(2,0,1);
        require(router.graph_.official_capacity(b)==10 && router.graph_.official_obstacle_demand(b)==0,"edge fixture has known raw capacity and no fixed demand");
        router.demand_[b]=20;router.guide_history_[b]=7;router.present_=2.5;router.history_scale_=1.3;
        const double price=router.official_congestion_price();
        require(price>0 && price!=1,"cost integration uses a nontrivial positive price");
        PinAccess access;access.node=b;access.length=100;access.vias=2;access.guides={{145,45,155,55,"metal1"}};
        router.options_.official_score_pricing=false;
        const double edge_old=router.edge_cost(a,b,NetRoute{}),access_old=router.access_cost(access);
        router.options_.official_score_pricing=true;
        const double edge_new=router.edge_cost(a,b,NetRoute{}),access_new=router.access_cost(access);
        const double marginal=100*2.5*.01*(1.1*1.1-1);
        near(edge_new-edge_old,(price-1)*marginal,"edge pricing scales only the current congestion marginal");
        near(access_new-access_old,(price-1)*marginal,"terminal access uses the same congestion price");
        near(edge_new,1+1.3*7+price*marginal,"planar geometry and history retain their original units");
        near(access_new,1+6+1.3*7+price*marginal,"access geometry and ordinary vias retain their original units");
        router.guide_history_[b]=9;
        near(router.edge_cost(a,b,NetRoute{})-edge_new,1.3*2,"history changes are not multiplied by the score price");
        NetRoute existing;existing.nodes.insert(target);PinAccess source;source.node=a;
        const auto updates=router.pricing_updates_,generation=router.state_generation_;
        const auto astar=router.search({source},{target},existing,1,1,false);
        const auto dijkstra=router.search({source},{target},existing,1,1,true);
        for (const auto* path:{&astar,&dijkstra}) {
            require(path->status==PathResult::Status::FOUND && !path->nodes.empty() &&
                path->nodes.front()==a && path->nodes.back()==target,"priced search preserves both terminal nodes");
            for (size_t k=1;k<path->nodes.size();++k) {
                require(router.graph_.legal_edge(path->nodes[k-1],path->nodes[k]),"priced search retains legal edges");
                require(router.edge_cost(path->nodes[k-1],path->nodes[k],existing)>=0,"priced search retains nonnegative edge costs");
            }
        }
        near(astar.cost,dijkstra.cost,"priced A* and explicit Dijkstra find the same exact cost");
        near(dijkstra.cost,4,"independent grid detour avoids the expensive direct corridor");
        require(router.state_generation_==generation && router.pricing_updates_==updates,"search snapshots reuse one price without ledger mutation");
    }
    static void constructor_reference(const NegotiatedRouter& router) {
        const long double total=std::accumulate(router.reference_lengths_.begin(),router.reference_lengths_.end(),0.0L);
        require(total>0,"native fixture has positive exact FLUTE reference");
        near(router.full_reference_,total,"constructor retains the full exact reference independently of accounted prefixes");
        require(router.official_reference_==0 && router.official_wirelength_==0,"native initialization has no accounted route prefix");
    }
};
}
int main(int argc,char** argv) {
    try {
        require(argc==2 || argc==3,"usage: score_pricing_tests artifact-directory [official-runtime]");
        const fs::path dir=argv[1];fs::create_directories(dir);LefParser lef;DefParser def;ThermalParser thermal;
        require(lef.parse(write(dir/"pricing.lef",technology())) &&
            def.parse(write(dir/"pricing.def",design()),lef,DefGridMapping::OFFICIAL_FULL),"pricing geometry fixture parses");
        // The algebra and resource tests need no native runtime: a friend sets
        // controlled official ledger snapshots on an actual GLOBAL_GUIDE graph.
        RouterOptions options;options.graph_options.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
        NegotiatedRouter router(def,lef,thermal,options);
        PinLocalTestAccess::algebra(router);PinLocalTestAccess::generation(router);
        PinLocalTestAccess::costs_and_search(router);
        if (argc==3) {
            const fs::path runtime=argv[2];bool available=true;
            for (const auto* name:{"libflute.so","tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so","POWV9.dat","POST9.dat"})
                available=available && fs::is_regular_file(runtime/name);
            if (available) {
                options.official_model=true;options.official_score_pricing=true;options.official_runtime=runtime.string();
                options.official_metrics.gcell_width_dbu=def.gcell_pitch_x();options.official_metrics.gcell_height_dbu=def.gcell_pitch_y();
                NegotiatedRouter native(def,lef,thermal,options);PinLocalTestAccess::constructor_reference(native);
            } else std::cout<<"SKIP native reference initialization: runtime unavailable\n";
        }
        std::cout<<"Score pricing checks: "<<checks<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
