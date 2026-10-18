#include "router/negotiated_router.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <tuple>

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
std::string write(const fs::path& file,const std::string& text) {
    std::ofstream out(file);out<<text;out.close();require(bool(out),"could not write RRR score fixture");return file.string();
}
std::string technology() {
    std::string text;
    for(int layer=1;layer<=20;++layer) {
        const auto name=get_layer_name(layer);
        text+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(layer%2?"HORIZONTAL":"VERTICAL")+
            " ; WIDTH 0.2 ; END "+name+"\n";
    }
    text+="MACRO RESOURCE_SLOT_bottom SIZE 10 BY 10 ; END RESOURCE_SLOT_bottom "
        "MACRO RESOURCE_SLOT_top SIZE 10 BY 10 ; END RESOURCE_SLOT_top "
        "MACRO TSV_BOTTOM SIZE 10 BY 10 ; PIN TSV_PORT PORT LAYER metal10 ; RECT 4.5 4.5 5.5 5.5 ; "
        "END END TSV_PORT END TSV_BOTTOM "
        "MACRO TSV_TOP SIZE 10 BY 10 ; PIN TSV_PORT PORT LAYER metal11 ; RECT 4.5 4.5 5.5 5.5 ; "
        "END END TSV_PORT END TSV_TOP "
        "LAYER tsv TYPE CUT ; END tsv VIA tsv_v DEFAULT "
        "LAYER metal10 ; RECT -0.25 -0.25 0.25 0.25 ; "
        "LAYER tsv ; RECT -0.25 -0.25 0.25 0.25 ; "
        "LAYER metal11 ; RECT -0.25 -0.25 0.25 0.25 ; END tsv_v\n";
    return text;
}
std::string design() {
    std::string text="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 1000 1000 ) ; "
        "GCELLGRID X 0 DO 11 STEP 100 ; GCELLGRID Y 0 DO 11 STEP 100 ; ";
    for(int layer=1;layer<=20;++layer) {
        const auto name=get_layer_name(layer);
        if(layer==2) text+="TRACKS X 950 DO 1 STEP 10 LAYER "+name+" ; TRACKS Y 950 DO 1 STEP 10 LAYER "+name+" ; ";
        else if(layer==3) text+="TRACKS X 0.5 DO 1000 STEP 1 LAYER "+name+" ; TRACKS Y 0.5 DO 1000 STEP 1 LAYER "+name+" ; ";
        else text+="TRACKS X 5 DO 100 STEP 10 LAYER "+name+" ; TRACKS Y 5 DO 100 STEP 10 LAYER "+name+" ; ";
    }
    text+="COMPONENTS 2 ; - rb RESOURCE_SLOT_bottom + PLACED ( 500 500 ) N ; "
        "- rt RESOURCE_SLOT_top + PLACED ( 500 500 ) N ; END COMPONENTS PINS 8 ; "
        "- lower + LAYER metal10 ( -1 -1 ) ( 1 1 ) + FIXED ( 550 550 ) N ; "
        "- upper + LAYER metal11 ( -1 -1 ) ( 1 1 ) + FIXED ( 550 550 ) N ; "
        "- a + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 250 250 ) N ; "
        "- b + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 850 850 ) N ; "
        "- edge_a + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 200 350 ) N ; "
        "- edge_b + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 800 350 ) N ; "
        "- zero + LAYER metal2 ( -1 -1 ) ( 1 1 ) + FIXED ( 450 450 ) N ; "
        "- third + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 350 750 ) N ; "
        "END PINS NETS 1001 ; "
        "- cross ( PIN lower ) ( PIN upper ) ; "
        "- interior ( PIN a ) ( PIN b ) ; "
        "- boundary ( PIN edge_a ) ( PIN edge_b ) ; "
        "- fixed_zero ( PIN zero ) ( PIN b ) ; "
        "- fanout ( PIN a ) ( PIN b ) ( PIN third ) ; ";
    for(int i=0;i<996;++i) text+="- filler"+std::to_string(i)+" ( PIN a ) ( PIN b ) ; ";
    return text+"END NETS END DESIGN";
}
NetRoute cached_route(std::vector<std::pair<int,int>> demand) {
    NetRoute result;std::sort(demand.begin(),demand.end());result.measured.demand=std::move(demand);
    result.measured.wirelength=100;result.measured.reference=100;return result;
}
int amount(const std::vector<std::pair<int,int>>& demand,int node) {
    const auto it=std::lower_bound(demand.begin(),demand.end(),node,[](const auto& entry,int n){return entry.first<n;});
    return it!=demand.end() && it->first==node?it->second:0;
}
double squared_overflow(double total,double capacity) {
    const double excess=std::max(0.0,total-capacity);return 0.01*excess*excess/(capacity*capacity);
}
}
namespace eda {
struct PinLocalTestAccess {
    static size_t index(const NegotiatedRouter& router,const std::string& name) {
        for(size_t i=0;i<router.def_.nets().size();++i)if(router.def_.nets()[i].name==name)return i;
        throw std::runtime_error("RRR fixture net missing: "+name);
    }
    static auto snapshot(const NegotiatedRouter& router) {
        return std::tuple(router.demand_,router.usage_,router.cell_nets_,router.edge_nets_,
            router.guide_history_,router.edge_history_,router.official_wirelength_,router.official_reference_,
            router.official_squared_overflow_,router.official_overflow_,router.official_zero_capacity_cells_,
            router.state_generation_,router.validated_generation_,router.complete_validation_cached_);
    }
    static void install(NegotiatedRouter& router,size_t i,NetRoute route) {
        require(router.routes_[i].measured.demand.empty(),"fixture installs only an unoccupied cached route");
        router.routes_[i]=std::move(route);router.account(i,router.routes_[i],1);
    }
    static void clear(NegotiatedRouter& router) {
        for(size_t i=0;i<router.routes_.size();++i)if(!router.routes_[i].measured.demand.empty()) {
            router.account(i,router.routes_[i],-1);router.routes_[i]=NetRoute{};
        }
        require(std::all_of(router.demand_.begin(),router.demand_.end(),[](int v){return v==0;}) &&
            router.cell_nets_.empty() && router.edge_nets_.empty() && router.usage_.empty(),
            "account ripup restores the actual unmodified demand and owner ledgers");
        near(double(router.official_wirelength_),0,"read-only ranking did not corrupt account wirelength rollback");
        near(double(router.official_reference_),0,"read-only ranking did not corrupt account reference rollback");
    }
    static void protected_tests(NegotiatedRouter& router) {
        const auto& graph=router.graph_;
        const size_t interior=index(router,"interior"),boundary=index(router,"boundary"),fanout=index(router,"fanout");
        const int a=graph.node(2,2,1),b=graph.node(8,8,1),extra=graph.node(4,4,3);
        NetRoute trial=cached_route({{a,1},{b,1},{extra,1}});
        PinAccess access;access.node=extra;access.guides={{440,440,460,460,"metal3"}};trial.accesses={access};
        auto protected_cells=router.official_protected_demand(interior,trial);
        require(amount(protected_cells,a)==1 && amount(protected_cells,b)==1 && amount(protected_cells,extra)==0,
            "interior Pins remain required but an available two-Pin local replacement prevents freezing old access geometry");
        trial.explicit_geometry=true;
        require(router.official_protected_demand(interior,trial)==protected_cells,
            "explicit PinAccess containing routed geometry never freezes its whole path");
        router.options_.pin_local=false;trial.explicit_geometry=false;
        require(amount(router.official_protected_demand(interior,trial),extra)==1,
            "fixed graph access remains protected when two-Pin local replacement is disabled");
        router.options_.pin_local=true;
        require(amount(router.official_protected_demand(fanout,trial),extra)==1,
            "a non-explicit multi-Pin route retains its fixed access node and guide");
        trial.explicit_geometry=true;
        require(amount(router.official_protected_demand(fanout,trial),extra)==0,
            "even multi-Pin explicit geometry does not make a whole access path immutable");
        NetRoute edge=cached_route({{graph.node(2,3,1),1},{graph.node(8,3,1),1},{extra,1}});
        edge.explicit_geometry=true;edge.accesses={access};
        require(router.official_protected_demand(boundary,edge).empty(),
            "a real Pin exactly on a GCell boundary is not forced into just the floor-side cell");
    }
    static void priority_tests(NegotiatedRouter& router) {
        const auto& graph=router.graph_;
        const size_t a=index(router,"interior"),b=index(router,"boundary");
        const int low=graph.node(4,4,1),high=graph.node(4,4,3),pin_a=graph.node(2,2,1),pin_b=graph.node(8,8,1);
        require(graph.official_capacity(low)==10 && graph.official_capacity(high)==100,
            "priority fixture independently supplies raw capacities 10 and 100");
        install(router,a,cached_route({{low,1},{pin_a,1},{pin_b,1}}));
        install(router,b,cached_route({{high,1}}));
        for(size_t j=0;j<138;++j) {
            const int node=j<19?low:high;
            install(router,index(router,"filler"+std::to_string(j)),cached_route({{node,1},{pin_a,1},{pin_b,1}}));
        }
        require(router.demand_[low]==20 && router.demand_[high]==120 && router.demand_[pin_a]>100,
            "positive capacity fixture has independently known overflow and much larger forced-terminal demand");
        const double expected_low=squared_overflow(20,10)-squared_overflow(19,10);
        const double expected_high=squared_overflow(120,100)-squared_overflow(119,100);
        const auto before=snapshot(router);
        const double low_priority=router.official_movable_priority(a),high_priority=router.official_movable_priority(b);
        near(low_priority,expected_low,"movable priority excludes unavoidable terminal contribution and equals exact squared-score relief");
        near(high_priority,expected_high,"score relief divides by raw capacity squared");
        require((120-100)>(20-10) && low_priority>40*high_priority,
            "official marginal ranking reverses the misleading raw-overflow order");
        for(int repeat=0;repeat<3;++repeat) {
            router.official_protected_demand(a,router.routes_[a]);router.official_movable_priority(a);
        }
        require(snapshot(router)==before,"pure priority/protection queries do not mutate generation, cache proof, demand, owners or score ledger");
        clear(router);
    }
    static void tsv_tests(NegotiatedRouter& router,const ThermalParser& thermal) {
        const auto& graph=router.graph_;const size_t cross=index(router,"cross");
        const auto assignment=router.policy_.assign(router.def_.nets(),thermal);
        require(assignment[cross]>=0,"fixed-interface crossing has a real legal TSV assignment");
        const auto& slot=router.policy_.slots()[assignment[cross]];
        const int col=graph.grid().get_col(slot.point.x),row=graph.grid().get_row(slot.point.y);
        const int lower=graph.node(col,row,10),upper=graph.node(col,row,11),access_node=graph.node(col,row,9),moving=graph.node(col+1,row,11);
        NetRoute route=cached_route({{lower,2},{upper,2},{access_node,1},{moving,1}});
        route.slot=assignment[cross];route.measured.tsv_points={slot.point};route.measured.tsv_cells={row*graph.grid().cols+col};
        PinAccess access;access.node=access_node;
        access.guides={{slot.point.x-1,slot.point.y-1,slot.point.x+1,slot.point.y+1,"metal9"},
                       {slot.point.x-1,slot.point.y-1,slot.point.x+1,slot.point.y+1,"metal10"}};
        route.accesses={access};
        const auto required=router.official_protected_demand(cross,route);
        require(amount(required,lower)==2 && amount(required,upper)==2,
            "fixed TSV protects covered-cell demand plus the additional selected-TSV congestion ratio on both layers");
        require(amount(required,access_node)==1 && amount(required,moving)==0,
            "fixed lower TSV access is protected while a neighboring M11 path cell stays movable");
        install(router,cross,route);
        for(size_t j=0;j<19;++j)
            install(router,index(router,"filler"+std::to_string(j)),cached_route({{lower,1},{upper,1},{moving,1}}));
        const double capacity=graph.official_capacity(moving),total=router.demand_[moving]+graph.official_obstacle_demand(moving);
        const auto before=snapshot(router);
        near(router.official_movable_priority(cross),squared_overflow(total,capacity)-squared_overflow(total-1,capacity),
            "fixed TSV demand never creates fictitious reroute relief despite its overloaded shared cell");
        require(snapshot(router)==before,"TSV priority calculation leaves full actual TSV demand in the account ledger");
        router.account(cross,router.routes_[cross],-1);
        route.measured.demand.erase(std::remove_if(route.measured.demand.begin(),route.measured.demand.end(),[&](const auto& p){return p.first==moving;}),route.measured.demand.end());
        router.routes_[cross]=route;router.account(cross,route,1);
        near(router.official_movable_priority(cross),0,"an entirely fixed TSV/access route consumes no congestion-reroute budget");
        clear(router);
    }
    static void zero_capacity_tests(NegotiatedRouter& router) {
        const int zero=router.graph_.node(4,4,2);
        require(router.graph_.official_capacity(zero)==0 && router.graph_.official_obstacle_demand(zero)==0,
            "zero-capacity group fixture has no pre-existing fixed obstacle demand");
        const size_t a=index(router,"interior"),b=index(router,"boundary"),fixed=index(router,"fixed_zero");
        install(router,a,cached_route({{zero,1}}));install(router,b,cached_route({{zero,1}}));
        near(router.official_movable_priority(a),5000,"first removable zero-capacity owner receives a joint-clear share");
        near(router.official_movable_priority(b),5000,"second removable zero-capacity owner receives a joint-clear share");
        std::vector<size_t> group{a};router.complete_zero_capacity_scope(group);
        require(group.size()==2 && std::find(group.begin(),group.end(),b)!=group.end(),
            "scope completion includes both movable owners of a shared zero-capacity cell");
        install(router,fixed,cached_route({{zero,1}}));
        const auto before=snapshot(router);
        near(router.official_movable_priority(fixed),0,"a real internal Pin on a zero-capacity cell cannot move");
        near(router.official_movable_priority(a),0,"a protected co-owner makes the shared zero-capacity penalty impossible to clear");
        near(router.official_movable_priority(b),0,"all movable co-owners avoid fictitious joint-clear relief");
        group={a};router.complete_zero_capacity_scope(group);
        require(group==std::vector<size_t>{a},"new scope completion does not spend budget on an uncleareable protected zero-capacity group");
        require(snapshot(router)==before,"zero-capacity group inspection leaves owners and official zero-cell count untouched");
        router.options_.rrr_score_aware=false;group={a};router.complete_zero_capacity_scope(group);
        require(group.size()==3,"disabling score-aware scheduling preserves legacy zero-capacity ownership completion");
        router.options_.rrr_score_aware=true;clear(router);

        for(size_t j=0;j<130;++j)install(router,index(router,"filler"+std::to_string(j)),cached_route({{zero,1}}));
        router.options_.rrr_net_limit=256;group={index(router,"filler0")};router.complete_zero_capacity_scope(group);
        require(group.size()==130,"explicit larger official score-aware budget can complete a group beyond legacy 128 owners");
        router.options_.rrr_score_aware=false;group={index(router,"filler0")};router.complete_zero_capacity_scope(group);
        require(group.size()==128,"legacy switch preserves its original 128-owner completion bound");
        router.options_.rrr_score_aware=true;router.options_.rrr_net_limit=128;clear(router);
    }
    static void run(NegotiatedRouter& router,const ThermalParser& thermal) {
        require(router.large_design(),"RRR fixture crosses the large-design scheduling boundary");
        router.routes_.resize(router.def_.nets().size());
        protected_tests(router);priority_tests(router);tsv_tests(router,thermal);zero_capacity_tests(router);
    }
};
}
int main(int argc,char** argv) {
    try {
        require(argc==2 || argc==3,"fixture directory and optional official runtime required");
        const fs::path folder=argv[1];fs::create_directories(folder);
        const auto runtime=argc==3?fs::path(argv[2]):fs::path(__FILE__).parent_path().parent_path()/"output/runtime/official-flute";
        for(const auto* asset:{"libflute.so","tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so","POWV9.dat","POST9.dat"})
            if(!fs::is_regular_file(runtime/asset)) {
                std::cout<<"SKIP RRR official score integration: native runtime unavailable at "<<runtime<<'\n';return 0;
            }
        require(!RouterOptions{}.rrr_score_aware && RouterOptions{}.rrr_net_limit==128,
            "score-aware scheduling remains opt-in and preserves the default legacy net budget");
        LefParser lef;DefParser def;ThermalParser thermal;
        require(lef.parse(write(folder/"rrr-score.lef",technology())) &&
            def.parse(write(folder/"rrr-score.def",design()),lef,DefGridMapping::OFFICIAL_FULL),"RRR score fixture parses");
        RouterOptions options;options.official_model=true;options.official_runtime=runtime.string();options.rrr_score_aware=true;
        options.graph_options.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
        options.official_metrics.gcell_width_dbu=def.gcell_pitch_x();options.official_metrics.gcell_height_dbu=def.gcell_pitch_y();
        NegotiatedRouter router(def,lef,thermal,options);
        // These tests install controlled per-net metric caches through the real
        // account transaction. They test ranking/ownership, not full Guide validity.
        PinLocalTestAccess::run(router,thermal);
        std::cout<<"PASS RRR score/protection, exact marginal capacity normalization, fixed TSV, zero-capacity groups, legacy bounds and read-only accounting: "<<checks<<" checks\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
