#include "router/tsv_policy.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>

using namespace eda;
namespace fs=std::filesystem;
namespace {
int checks=0;
void require(bool value,const std::string& why) {
    ++checks;if (!value) throw std::runtime_error(why);
}
void rejects(const std::function<void()>& action,const std::string& why) {
    bool failed=false;try {action();} catch(const std::runtime_error&) {failed=true;}
    require(failed,why);
}
std::string write(const fs::path& dir,const std::string& name,const std::string& text) {
    const auto file=dir/name;std::ofstream out(file);out<<text;out.close();
    if (!out) throw std::runtime_error("cannot write global TSV fixture");
    return file.string();
}
std::string technology() {
    std::ostringstream out;
    for (int layer=1;layer<=20;++layer) {
        const auto name=get_layer_name(layer);
        out<<"LAYER "<<name<<" TYPE ROUTING ; DIRECTION "<<(layer%2?"HORIZONTAL":"VERTICAL")
           <<" ; WIDTH 0.2 ; END "<<name<<'\n';
    }
    // Intentionally no RESOURCE_SLOT, TSV_PORT or physical VIA declarations.
    return out.str();
}
std::string design(size_t nets,const Rect& die={17.25,23.5,284.75,276.25},int grid_lines=4) {
    std::ostringstream out;
    out<<"UNITS DISTANCE MICRONS 10 ; DIEAREA ( "<<die.x1<<' '<<die.y1<<" ) ( "
       <<die.x2<<' '<<die.y2<<" ) ; GCELLGRID X 0 DO "<<grid_lines
       <<" STEP 100 ; GCELLGRID Y 0 DO "<<grid_lines<<" STEP 100 ; ";
    for (int layer=1;layer<=20;++layer)
        out<<"TRACKS X 25 DO 26 STEP 10 LAYER "<<get_layer_name(layer)<<" ; "
           <<"TRACKS Y 25 DO 26 STEP 10 LAYER "<<get_layer_name(layer)<<" ; ";
    out<<"COMPONENTS 0 ; END COMPONENTS PINS "<<nets*2<<" ; ";
    for (size_t n=0;n<nets;++n) for (int side=0;side<2;++side)
        out<<"- p"<<n<<(side?'t':'b')<<" + LAYER "<<get_layer_name(side?14:3)
           <<" ( -1 -1 ) ( 1 1 ) + FIXED ( "<<(side?260:40)<<' '<<(side?260:40)<<" ) N ; ";
    out<<"END PINS NETS "<<nets<<" ; ";
    for (size_t n=0;n<nets;++n) out<<"- n"<<n<<" ( PIN p"<<n<<"b ) ( PIN p"<<n<<"t ) ; ";
    out<<"END NETS END DESIGN";return out.str();
}
Net fixed_net(const std::vector<Point>& crossings) {
    Net net;net.name="fixed";net.is_cross_die=true;
    for (auto point:crossings) for (int layer:{10,11}) {
        Pin pin;pin.name="p"+std::to_string(net.pins.size());pin.layer=get_layer_name(layer);
        pin.layer_idx=layer;pin.coord=point;net.pins.push_back(pin);
    }
    return net;
}
bool equal(const Rect& a,const Rect& b) {
    return a.x1==b.x1 && a.y1==b.y1 && a.x2==b.x2 && a.y2==b.y2;
}
bool positive_overlap(const Rect& a,const Rect& b) {
    return std::min(a.x2,b.x2)>std::max(a.x1,b.x1) &&
           std::min(a.y2,b.y2)>std::max(a.y1,b.y1);
}
std::vector<GuideRect> witness(const SlotCandidate& slot) {
    const auto& r=slot.region;
    return {{r.x1,r.y1,r.x2,r.y2,"metal10"},{r.x1,r.y1,r.x2,r.y2,"metal11"}};
}
void capacity_and_geometry(const fs::path& dir,const LefParser& lef) {
    DefParser def;require(def.parse(write(dir,"global.def",design(9)),lef,DefGridMapping::OFFICIAL_FULL),"parse global fixture");
    require(TsvPolicyOptions{}.resource_mode==TsvResourceMode::AUTO,"global grid remains opt-in");
    for (auto mode:{TsvResourceMode::AUTO,TsvResourceMode::PAIRED,TsvResourceMode::PROJECTED}) {
        TsvPolicyOptions old;old.resource_mode=mode;
        rejects([&] {TsvPolicy::preflight(def,lef,old);},"legacy modes still reject insufficient reservations");
    }
    TsvPolicyOptions options;options.resource_mode=TsvResourceMode::GLOBAL_GRID;
    options.nearest_candidates=options.max_candidates=1;
    TsvPolicy::preflight(def,lef,options);
    RoutingGraphOptions go;go.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
    RoutingGraph graph(def,lef,go);TsvPolicy policy(def,lef,graph,options);ThermalParser thermal;
    require(std::string(policy.pairing_mode())=="global-grid","diagnostic states abstract global-grid mode");
    require(policy.slots().size()==9 && policy.enabled_slots()==9,"all positive-area clipped cells are available");
    const auto chosen=policy.assign(def.nets(),thermal);
    require(std::set<int>(chosen.begin(),chosen.end()).size()==9 &&
            std::find(chosen.begin(),chosen.end(),-1)==chosen.end(),"global completion assigns one net per distinct cell");
    require(policy.assignment_global_completions()==8,"bounded nearest pool completes from free global cells");
    for (size_t n=0;n<chosen.size();++n) {
        const auto& slot=policy.slots().at(chosen[n]);
        require(slot.id==chosen[n] && slot.projected && policy.slot_capacity(slot.id)==1,"unit candidate identity");
        require(slot.bottom_name.empty() && slot.top_name.empty() && slot.via_name.empty(),"no instance or physical VIA invented");
        require(slot.conflicts.empty(),"independent GCell interiors need no keepout conflicts");
        require(equal(slot.region,slot.lower_pad) && equal(slot.region,slot.upper_pad) &&
                equal(slot.region,slot.envelope),"single exact M10/M11 landing witness");
        require(slot.region.x1==std::ceil(slot.region.x1) && slot.region.y1==std::ceil(slot.region.y1) &&
                slot.region.x2==std::floor(slot.region.x2) && slot.region.y2==std::floor(slot.region.y2),
                "landing Guide uses integral DBU");
        require(graph.legal_rect(slot.region,10) && graph.legal_rect(slot.region,11),"clipped landing stays in DIEAREA");
        require(graph.layer(slot.lower_node)==10 && graph.layer(slot.upper_node)==11 &&
                graph.col(slot.lower_node)==def.gcell_grid().get_col(slot.point.x) &&
                graph.row(slot.upper_node)==def.gcell_grid().get_row(slot.point.y),"point and nodes share the landing cell");
        std::string error;
        require(policy.validate_mapping(def.nets()[n],witness(slot),chosen[n],error),"one positive-area crossing passes mapping: "+error);
        for (const auto& other:policy.slots()) if (other.id!=slot.id)
            require(!positive_overlap(slot.region,other.region),"neighboring landings have disjoint interiors");
    }
    require(equal(policy.slots().front().region,{18,24,100,100}) &&
            equal(policy.slots().back().region,{200,200,284,276}),"both DIE boundary cells clip inward");
    require(policy.assign(def.nets(),thermal)==chosen,"global-grid matching replays deterministically");
    const auto fixed=fixed_net({{40,40},{70,80}});
    const auto fixed_choice=policy.assign({fixed},thermal).front();
    const auto& landing=policy.slots().at(fixed_choice);
    require(equal(landing.region,{18,24,100,100}) && landing.point.distance_to(fixed.pins[0].coord)>0,
            "aligned fixed pairs may share a cell away from its center");
    for (const auto& pin:fixed.pins) require(landing.region.contains(pin.coord),"fixed pin coordinates remain covered");
    const auto edge=fixed_net({{100,100}});
    const int edge_choice=policy.assign({edge},thermal).front();
    require(equal(policy.slots()[edge_choice].region,{100,100,200,200}),"grid boundary uses the declared get_col/get_row convention");
    require(!policy.compatible(edge,fixed_choice),"touching neighboring cell does not override boundary ownership");
    const auto split=fixed_net({{40,40},{140,140}});
    for (const auto& slot:policy.slots()) require(!policy.compatible(split,slot.id),"multiple fixed crossing cells cannot share one TSV");
    rejects([&] {policy.assign({split},thermal);},"different fixed crossing cells fail instead of moving a pin");
    rejects([&] {policy.assign({fixed,fixed},thermal);},"two fixed nets cannot claim one cell");
    auto bad_guides=witness(landing);bad_guides.push_back({200,200,250,250,"metal10"});
    bad_guides.push_back({200,200,250,250,"metal11"});std::string error;
    require(!policy.validate_mapping(fixed,bad_guides,fixed_choice,error),"multiple positive-area TSVs remain rejected");
    RoutingGraph strict(def,lef);
    rejects([&] {TsvPolicy invalid(def,lef,strict,options);},"physical routing geometry cannot use abstract global landings");
    auto bad=options;bad.max_candidates=0;
    rejects([&] {TsvPolicy::preflight(def,lef,bad);},"global mode retains existing option validation");
    DefParser excess;require(excess.parse(write(dir,"excess.def",design(10)),lef,DefGridMapping::OFFICIAL_FULL),"parse excess demand");
    rejects([&] {TsvPolicy::preflight(excess,lef,options);},"cross nets beyond legal cells fail in geometry-free preflight");
    DefParser large;require(large.parse(write(dir,"large.def",design(1,{0,0,300,300},2002)),lef,DefGridMapping::OFFICIAL_FULL),"parse over-budget grid");
    rejects([&] {TsvPolicy::preflight(large,lef,options);},"candidate allocation is bounded before graph construction");
}
void empty_clipped_cells(const fs::path& dir,const LefParser& lef) {
    DefParser def;require(def.parse(write(dir,"thin.def",design(1,{99.5,99.5,200.5,200.5})),lef,DefGridMapping::OFFICIAL_FULL),"parse narrow boundary fixture");
    TsvPolicyOptions options;options.resource_mode=TsvResourceMode::GLOBAL_GRID;
    RoutingGraphOptions go;go.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
    RoutingGraph graph(def,lef,go);TsvPolicy policy(def,lef,graph,options);
    require(policy.slots().size()==1 && equal(policy.slots().front().region,{100,100,200,200}),
            "sub-DBU clipped border cells cannot create zero-area Guide landings");
    DefParser excess;require(excess.parse(write(dir,"thin-excess.def",design(2,{99.5,99.5,200.5,200.5})),lef,DefGridMapping::OFFICIAL_FULL),"parse narrow excess fixture");
    rejects([&] {TsvPolicy::preflight(excess,lef,options);},"preflight counts integer-positive cells consistently with construction");
}
}
int main(int argc,char** argv) {
    try {
        if (argc!=2) throw std::runtime_error("usage: global_tsv_tests artifact-directory");
        const fs::path dir=argv[1];fs::create_directories(dir);LefParser lef;
        require(lef.parse(write(dir,"global.lef",technology())),"parse technology without physical TSVs");
        capacity_and_geometry(dir,lef);empty_clipped_cells(dir,lef);
        std::cout<<"Global TSV checks: "<<checks<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
