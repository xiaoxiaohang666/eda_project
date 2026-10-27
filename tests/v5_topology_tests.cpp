#include "router/negotiated_router.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>

using namespace eda;
namespace fs=std::filesystem;
namespace {
size_t checks=0;
void require(bool ok,const std::string& reason) {++checks;if(!ok)throw std::runtime_error(reason);}
std::string write(const fs::path& path,const std::string& text) {
    std::ofstream out(path);out<<text;out.close();require(bool(out),"fixture write failed");return path.string();
}
std::string technology() {
    std::string text;
    for(int layer=1;layer<=20;++layer) {
        const auto name=get_layer_name(layer);
        text+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(layer%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.2 ; END "+name+"\n";
    }
    return text+"MACRO RESOURCE_SLOT_bottom SIZE 2 BY 2 ; END RESOURCE_SLOT_bottom "
        "MACRO RESOURCE_SLOT_top SIZE 2 BY 2 ; END RESOURCE_SLOT_top "
        "MACRO TSV_BOTTOM SIZE 2 BY 2 ; PIN TSV_PORT PORT LAYER metal10 ; RECT 0.5 0.5 1.5 1.5 ; END END TSV_PORT END TSV_BOTTOM "
        "MACRO TSV_TOP SIZE 2 BY 2 ; PIN TSV_PORT PORT LAYER metal11 ; RECT 0.5 0.5 1.5 1.5 ; END END TSV_PORT END TSV_TOP "
        // Match the 10-DBU GCell and TSV_PORT aperture exactly. Fractional-DBU
        // pads exercise the legacy encoder's rounding instead of topology.
        "LAYER tsv TYPE CUT ; END tsv VIA tsv_v DEFAULT LAYER metal10 ; RECT -0.5 -0.5 0.5 0.5 ; "
        "LAYER tsv ; RECT -0.5 -0.5 0.5 0.5 ; LAYER metal11 ; RECT -0.5 -0.5 0.5 0.5 ; END tsv_v\n";
}
std::string design(bool sparse_layer20=false) {
    constexpr int cases=24;
    std::ostringstream out;out<<"UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 300 300 ) ; "
        "GCELLGRID X 0 DO 31 STEP 10 ; GCELLGRID Y 0 DO 31 STEP 10 ; ";
    for(int layer=1;layer<=20;++layer) {
        if(sparse_layer20 && layer==20)
            out<<"TRACKS X 250.5 DO 1 STEP 1 LAYER metal20 ; TRACKS Y 250.5 DO 1 STEP 1 LAYER metal20 ; ";
        else out<<"TRACKS X 0.5 DO 300 STEP 1 LAYER "<<get_layer_name(layer)<<" ; TRACKS Y 0.5 DO 300 STEP 1 LAYER "<<get_layer_name(layer)<<" ; ";
    }
    out<<"COMPONENTS "<<2*(cases+1)<<" ; ";
    for(int c=0;c<=cases;++c) for(int side=0;side<2;++side)
        out<<"- slot_"<<c<<'_'<<side<<" RESOURCE_SLOT_"<<(side?"top":"bottom")
           <<" + PLACED ( "<<45+40*(c%5)<<' '<<45+40*(c/5)<<" ) N ; ";
    out<<"END COMPONENTS PINS "<<cases*6+4<<" ; ";
    // Deterministic asymmetric fanouts vary the local tree's shape, layer and
    // orientation relative to one distant landing without using random seeds.
    for(int c=0;c<cases;++c) for(int side=0;side<2;++side) for(int p=0;p<3;++p) {
        const int x=75+10*((c*7+p*11+side*3)%21),y=75+10*((c*13+p*7+side*5)%21);
        const int layer=(side?11:1)+((c+p*2)%7);
        out<<"- p"<<c<<'_'<<side<<'_'<<p<<" + LAYER "<<get_layer_name(layer)
           <<" ( -1 -1 ) ( 1 1 ) + FIXED ( "<<x<<' '<<y<<" ) N ; ";
    }
    out<<"- a + LAYER metal3 ( -1 -1 ) ( 1 1 ) + FIXED ( 15 15 ) N ; "
        "- b + LAYER metal3 ( -1 -1 ) ( 1 1 ) + FIXED ( 25 15 ) N ; "
        "- lo + LAYER metal3 ( -1 -1 ) ( 1 1 ) + FIXED ( 175 175 ) N ; "
        "- hi + LAYER metal13 ( -1 -1 ) ( 1 1 ) + FIXED ( 225 225 ) N ; END PINS NETS 1001 ; ";
    for(int c=0;c<cases;++c) {
        out<<"- cross"<<c<<' ';
        for(int side=0;side<2;++side)for(int p=0;p<3;++p)out<<"( PIN p"<<c<<'_'<<side<<'_'<<p<<" ) ";
        out<<"; ";
    }
    out<<"- pair ( PIN lo ) ( PIN hi ) ; ";
    for(int c=cases+1;c<1001;++c)out<<"- filler"<<c<<" ( PIN a ) ( PIN b ) ; ";
    return out.str()+"END NETS END DESIGN";
}
std::string signature(const NetRoute& route) {
    std::ostringstream out;out<<std::setprecision(20)<<route.slot<<' '<<route.length<<' '<<route.vias;
    for(int n:route.nodes)out<<" n"<<n;
    for(auto e:route.edges)out<<" e"<<e;
    for(const auto& g:route.guides)out<<" g"<<g.layer<<' '<<g.x1<<' '<<g.y1<<' '<<g.x2<<' '<<g.y2;
    out<<" m"<<route.measured.wirelength<<' '<<route.measured.reference;
    for(auto [n,m]:route.measured.demand)out<<' '<<n<<':'<<m;
    return out.str();
}
}
namespace eda {
struct PinLocalTestAccess {
    static auto ledger(const NegotiatedRouter& router) {
        return std::tuple(router.demand_,router.usage_,router.cell_nets_,router.edge_nets_,router.official_wirelength_,
            router.official_reference_,router.official_squared_overflow_,router.official_overflow_,
            router.official_zero_capacity_cells_,router.state_generation_);
    }
    static void quality_regression(NegotiatedRouter& router) {
        const int node=router.graph_.node(0,0,20);
        require(router.graph_.official_capacity(node)==0 && router.graph_.official_obstacle_demand(node)==0 &&
                router.demand_[node]==0,"score regression requires a real empty zero-capacity resource");
        // A route that avoids one zero cell can still lose far more wire score
        // than it gains in congestion once other nets already occupy 99 cells.
        // This snapshot is intentionally independent of topology generation.
        CompleteMetrics base;base.guide.wirelength=base.guide.reference=1000;
        base.official.zero_capacity_cells=99;base.official.squared_normalized_overflow=990000;
        NetRoute baseline;baseline.measured.wirelength=baseline.measured.reference=1000;
        baseline.measured.demand={{node,1}};
        NetRoute longer;longer.measured.reference=1000;longer.measured.wirelength=1200;
        const auto old_key=router.official_candidate_key(baseline,base),new_key=router.official_candidate_key(longer,base);
        require(new_key<old_key && std::get<1>(new_key)>std::get<1>(old_key),
                "fixture must expose a legacy lexicographic win that loses actual W/C quality");
        require(!router.v5_topology_better(longer,baseline,base),"V5 accepts a zero-cell reduction that lowers real quality");
        longer.measured.wirelength=1000;
        require(router.v5_topology_better(longer,baseline,base),"V5 rejects a real congestion improvement with equal wire cost");
    }
    static void budget_regression(NegotiatedRouter& router) {
        using Clock=std::chrono::steady_clock;
        const auto index=static_cast<size_t>(std::find_if(router.def_.nets().begin(),router.def_.nets().end(),
            [](const Net& net){return net.is_cross_die && net.pins.size()>2;})-router.def_.nets().begin());
        const int slot=router.policy_.slots().front().id;
        router.options_.v5_pin_rooted=false;
        const auto baseline=router.build_route(index,slot);
        const auto snapshot=ledger(router);
        const auto deadline=router.deadline_;
        const double used=router.counters_.v5_topology_seconds,limit=router.v5_topology_seconds_limit();
        require(limit==std::min(120.0,router.options_.time_limit*0.05),"V5 cumulative limit must reserve the routing budget");
        require(used>0 && used<limit,"successful trials must charge their elapsed work without exhausting this fixture");
        router.options_.v5_pin_rooted=true;
        // Less than one clock tick remains: the real trial enters its scoped
        // deadline, fails before altering the incumbent, and restores the
        // original deadline. No sleeps or wall-clock race are required.
        router.counters_.v5_topology_seconds=std::nextafter(limit,0.0);
        const auto attempts=router.counters_.v5_topology_attempted,interruptions=router.counters_.v5_topology_interruptions;
        const auto failures=router.counters_.v5_topology_failures;
        require(signature(router.build_route(index,slot))==signature(baseline),"expired extra-work trial discarded its legal incumbent");
        require(router.counters_.v5_topology_attempted==attempts+1 && router.counters_.v5_topology_interruptions==interruptions+1 &&
                router.counters_.v5_topology_failures==failures,"work-budget interruption was not classified separately");
        require(router.deadline_==deadline && !router.v5_topology_trial_ && !router.timed_out(),
                "optional trial leaked its temporary deadline or active flag into baseline routing");
        require(ledger(router)==snapshot && router.counters_.v5_topology_seconds>=limit,
                "interrupted trial changed ownership or failed to charge its actual elapsed time");
        const auto skips=router.counters_.v5_topology_budget_skips;
        require(signature(router.build_route(index,slot))==signature(baseline) &&
                router.counters_.v5_topology_attempted==attempts+1 && router.counters_.v5_topology_budget_skips==skips+1,
                "exhausted cumulative budget did not leave subsequent baseline construction usable");
        // Fast-template and encoding entry points also observe the scoped
        // budget; unlike A*, these paths previously did not test its clock.
        router.v5_topology_trial_=true;router.deadline_=Clock::now()-std::chrono::seconds(1);
        bool search_stopped=false,encode_stopped=false;
        try {router.search(baseline.accesses,baseline.nodes,NetRoute{},1,9,false);}catch(const std::runtime_error&) {search_stopped=true;}
        auto copy=baseline;
        try {router.encode(index,copy);}catch(const std::runtime_error&) {encode_stopped=true;}
        router.v5_topology_trial_=false;router.deadline_=deadline;
        require(search_stopped && encode_stopped,"template or encoding work ignored the optional deadline");
        require(ledger(router)==snapshot,"budget enforcement mutated an ownership or score ledger");
        router.counters_.v5_topology_seconds=used;
    }
    static void run(NegotiatedRouter& router) {
        require(router.large_design(),"fixture must exercise large-design fast initial paths");
        require(!router.options_.v5_pin_rooted,"new topology must default off");
        require(!router.policy_.slots().empty(),"fixture must expose a physical landing");
        const int slot=router.policy_.slots().front().id;
        size_t improvements=0,kept_baseline=0;
        for(size_t i=0;i<router.def_.nets().size();++i) {
            const auto& net=router.def_.nets()[i];
            if(!net.is_cross_die)continue;
            const auto snapshot=ledger(router);
            router.options_.v5_pin_rooted=false;
            auto baseline=router.build_route(i,slot);
            require(signature(baseline)==signature(router.build_candidate(i,slot,0)),"disabled flag changes baseline output");
            router.options_.v5_pin_rooted=true;
            const auto before=router.counters_;
            auto chosen=router.build_route(i,slot);
            require(ledger(router)==snapshot,"candidate construction mutated ownership or exact score ledger");
            const auto base=router.official_candidate_base();
            require(router.official_candidate_key(chosen,base)<=router.official_candidate_key(baseline,base),
                "enabled topology worsens exact official W/C acceptance key");
            require(std::get<1>(router.official_candidate_key(chosen,base))<=std::get<1>(router.official_candidate_key(baseline,base)),
                "enabled topology lowered actual W/C quality while reducing zero-capacity cells");
            require(chosen.slot==baseline.slot && chosen.measured.tsv_cells==baseline.measured.tsv_cells &&
                chosen.measured.tsv_points.size()==baseline.measured.tsv_points.size(),"topology changed landing or thermal cells");
            for(size_t k=0;k<chosen.measured.tsv_points.size();++k)
                require(chosen.measured.tsv_points[k].distance_to(baseline.measured.tsv_points[k])==0,"topology changed serialized TSV point");
            std::string error;
            require(validate_net_guides(net,router.def_.die_area(),router.lef_,chosen.guides,error),"candidate loses legal connectivity: "+error);
            for(auto e:chosen.edges) {auto [a,b]=edge_nodes(e);
                require(chosen.nodes.count(a)&&chosen.nodes.count(b)&&router.graph_.legal_edge(a,b),"candidate contains invalid graph edge");}
            if(net.pins.size()==2) {
                require(router.counters_.v5_topology_attempted==before.v5_topology_attempted,"two-pin crossing needlessly enumerates a shared tree");
                require(signature(chosen)==signature(baseline),"two-pin crossing changed");
            } else {
                require(router.counters_.v5_topology_attempted==before.v5_topology_attempted+1,"fanout candidate was not attempted exactly once");
                require(router.counters_.v5_topology_candidates==before.v5_topology_candidates+1,"bounded candidate failed to encode");
                improvements+=router.official_candidate_key(chosen,base)<router.official_candidate_key(baseline,base);
                kept_baseline+=signature(chosen)==signature(baseline);
                router.options_.v5_topology_max_pins=3;
                const auto bounded_before=router.counters_.v5_topology_attempted;
                require(signature(router.build_route(i,slot))==signature(baseline),"fanout bound did not retain baseline");
                require(router.counters_.v5_topology_attempted==bounded_before,"fanout bound did not skip expensive candidate");
                router.options_.v5_topology_max_pins=32;
            }
        }
        require(improvements>0,"representative fanouts show no real exact-score improvement");
        require(kept_baseline>0,"fixture does not cover rejection in favor of baseline");
        require(router.counters_.v5_topology_failures==0,"candidate path or connectivity unexpectedly fails");
        budget_regression(router);
        std::cout<<"Topology improvements="<<improvements<<", baseline retained="<<kept_baseline<<'\n';
    }
};
}
int main(int argc,char** argv) {
    try {
        require(argc==2||argc==3,"fixture directory and optional official runtime required");
        const fs::path folder=argv[1];fs::create_directories(folder);
        const auto runtime=argc==3?fs::path(argv[2]):fs::path(__FILE__).parent_path().parent_path()/"output/runtime/official-flute";
        for(const auto* asset:{"libflute.so","tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so","POWV9.dat","POST9.dat"})
            if(!fs::is_regular_file(runtime/asset)){std::cout<<"SKIP v5 topology: official native runtime unavailable\n";return 0;}
        LefParser lef;DefParser def;ThermalParser thermal;
        require(lef.parse(write(folder/"topology.lef",technology())) &&
            def.parse(write(folder/"topology.def",design()),lef,DefGridMapping::OFFICIAL_FULL),"topology fixture parses");
        RouterOptions options;options.official_model=true;options.official_runtime=runtime.string();options.time_limit=120;
        options.graph_options.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
        options.official_metrics.gcell_width_dbu=10;options.official_metrics.gcell_height_dbu=10;
        NegotiatedRouter router(def,lef,thermal,options);PinLocalTestAccess::run(router);
        DefParser sparse;
        require(sparse.parse(write(folder/"topology-zero.def",design(true)),lef,DefGridMapping::OFFICIAL_FULL),"zero-capacity fixture parses");
        NegotiatedRouter zero(sparse,lef,thermal,options);PinLocalTestAccess::quality_regression(zero);
        std::cout<<"PASS v5 pin-rooted topology, exact-score acceptance, TSV invariance, ledger immutability, off and fanout guards: "<<checks<<" checks\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
