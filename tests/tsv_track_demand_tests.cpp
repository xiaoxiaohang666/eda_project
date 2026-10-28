#include "objectives/official_metrics.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

using namespace eda;
namespace fs=std::filesystem;
namespace {
size_t checks=0;
void require(bool value,const std::string& why) {
    ++checks;if(!value) throw std::runtime_error(why);
}
template<class F> void rejects(F action,const std::string& why) {
    bool failed=false;try {action();} catch(const std::exception&) {failed=true;}
    require(failed,why);
}
std::string write(const fs::path& file,const std::string& text) {
    std::ofstream out(file);out<<text;out.close();
    if(!out) throw std::runtime_error("cannot write TSV track fixture");
    return file.string();
}
std::string technology() {
    std::ostringstream out;
    for(int l=1;l<=20;++l) {
        const auto name=get_layer_name(l);
        out<<"LAYER "<<name<<" TYPE ROUTING ; DIRECTION "<<(l%2?"HORIZONTAL":"VERTICAL")
           <<" ; WIDTH 0.2 ; END "<<name<<'\n';
    }
    return out.str();
}
std::string design(int dbu,int pitch,bool missing_top=false) {
    const int scale=dbu/10;
    std::ostringstream out;
    out<<"UNITS DISTANCE MICRONS "<<dbu<<" ; DIEAREA ( 0 0 ) ( "<<3*pitch<<' '<<3*pitch
       <<" ) ; GCELLGRID X 0 DO 4 STEP "<<pitch<<" ; GCELLGRID Y 0 DO 4 STEP "<<pitch<<" ; ";
    for(int l=1;l<=20;++l) {
        const auto name=get_layer_name(l);
        out<<"TRACKS X "<<3*scale<<" DO 100 STEP "<<10*scale<<" LAYER "<<name<<" ; ";
        if(l!=11 || !missing_top)
            out<<"TRACKS Y "<<3*scale<<" DO 100 STEP "<<20*scale<<" LAYER "<<name<<" ; ";
    }
    // Repeated declarations are the same physical tracks, never extra capacity.
    out<<"TRACKS X "<<3*scale<<" DO 100 STEP "<<10*scale<<" LAYER metal10 ; ";
    if(!missing_top) out<<"TRACKS Y "<<3*scale<<" DO 100 STEP "<<20*scale<<" LAYER metal11 ; ";
    out<<"COMPONENTS 0 ; END COMPONENTS PINS 3 ; "
       <<"- a + LAYER metal3 ( -1 -1 ) ( 1 1 ) + FIXED ( "<<pitch/2<<' '<<pitch/2<<" ) N ; "
       <<"- b + LAYER metal14 ( -1 -1 ) ( 1 1 ) + FIXED ( "<<5*pitch/2<<' '<<5*pitch/2<<" ) N ; "
       <<"- foreign + LAYER metal10 ( -5 -5 ) ( 5 5 ) + FIXED ( "<<3*pitch/2<<' '<<3*pitch/2<<" ) N ; "
       <<"END PINS NETS 1 ; - n ( PIN a ) ( PIN b ) ; END NETS END DESIGN";
    return out.str();
}
struct Fixture {
    LefParser lef;DefParser def;
    Fixture(const fs::path& dir,int dbu,int pitch,bool missing_top=false) {
        fs::create_directories(dir);
        require(lef.parse(write(dir/"tech.lef",technology())),"parse TSV fixture technology");
        require(def.parse(write(dir/"design.def",design(dbu,pitch,missing_top)),lef,DefGridMapping::OFFICIAL_FULL),
                "parse TSV fixture design");
    }
};
RoutingGraphOptions graph_options() {
    RoutingGraphOptions o;o.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
    o.score_20261020=true;o.documented_rules=true;return o;
}
OfficialMetricOptions metric_options(int pitch) {
    OfficialMetricOptions o;o.score_20261020=true;o.tsv_track_demand=true;
    o.gcell_width_dbu=pitch;o.gcell_height_dbu=pitch;return o;
}
int demand(const NetGuideMetrics& measured,int node) {
    for(auto [id,count]:measured.demand) if(id==node) return count;
    return 0;
}
std::vector<GuideRect> landing(const RoutingGraph& graph,int c=1,int r=1) {
    const auto box=graph.grid().get_cell_rect(c,r);
    return {{box.x1,box.y1,box.x2,box.y2,"metal10"},
            {box.x1,box.y1,box.x2,box.y2,"metal11"}};
}
void track_geometry(const fs::path& dir) {
    Fixture wide(dir/"wide",10,100);RoutingGraph graph(wide.def,wide.lef,graph_options());
    const Point point{150,150};
    require(graph.official_capacity(graph.node(1,1,10))==10,"duplicate M10 tracks do not enlarge raw capacity");
    require(graph.official_capacity(graph.node(1,1,11))==5,"M11 uses Y tracks because it is horizontal");
    require(graph.tsv_landing_tracks(point,10)==5,"5um TSV counts vertical M10 X tracks using DEF units");
    require(graph.tsv_landing_tracks(point,11)==2,"5um TSV counts horizontal M11 Y tracks");
    require(graph.tsv_landing_tracks(point,10,1.0)==1,"explicit physical width changes reservation");
    require(graph.tsv_landing_tracks({128,150},10)==6,"documented shape boundary tracks are included explicitly");
    // A shifted witness within a cell uses its real location, rather than a
    // table indexed only by the GCell center.
    require(graph.tsv_landing_tracks({102,150},10)==3,"off-center reservation clips to its landing band");
    Fixture small(dir/"small",10,30);RoutingGraph narrow(small.def,small.lef,graph_options());
    const Point center{45,45};
    require(narrow.tsv_landing_tracks(center,10)==narrow.official_capacity(narrow.node(1,1,10)),
            "TSV wider than GCell is capped by landing M10 tracks");
    require(narrow.tsv_landing_tracks(center,11)==narrow.official_capacity(narrow.node(1,1,11)),
            "TSV wider than GCell is capped by landing M11 tracks");
    Fixture scaled(dir/"scaled",20,200);RoutingGraph units(scaled.def,scaled.lef,graph_options());
    require(units.tsv_landing_tracks({300,300},10)==5 && units.tsv_landing_tracks({300,300},11)==2,
            "doubling DBU per micron preserves physical demand");
    rejects([&]{graph.tsv_landing_tracks(point,9);},"noninterface layer rejected");
    rejects([&]{graph.tsv_landing_tracks(point,10,0);},"zero physical width rejected");
    rejects([&]{graph.tsv_landing_tracks(point,10,std::numeric_limits<double>::infinity());},"infinite physical width rejected");
    rejects([&]{graph.tsv_landing_tracks({-1,0},10);},"out-of-DIE landing rejected");
    RoutingGraphOptions historical;historical.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
    RoutingGraph old(wide.def,wide.lef,historical);
    rejects([&]{old.tsv_landing_tracks(point,10);},"historical resource profile cannot silently enable new reservation");
}
void metric_union(const fs::path& dir) {
    Fixture fixture(dir/"metrics",10,100);RoutingGraph graph(fixture.def,fixture.lef,graph_options());
    const auto guides=landing(graph);const auto& net=fixture.def.nets().front();
    auto o=metric_options(100);
    const auto measured=measure_official_net_guides(net,guides,graph,fixture.lef,o,1000);
    require(measured.demand.size()==2,"physical footprint does not add demand in neighboring GCells");
    require(demand(measured,graph.node(1,1,10))==5 && demand(measured,graph.node(1,1,11))==2,
            "same-net Guide demand is merged into TSV reserved tracks");
    require(graph.official_obstacle_demand(graph.node(1,1,10))>0,
            "fixture contains a foreign fixed PIN at the TSV landing");
    require(demand(measured,graph.node(1,1,10))==5,"foreign fixed PIN never receives own-net TSV credit");
    auto longer=guides;longer[0].y2=300;
    const auto expanded=measure_official_net_guides(net,longer,graph,fixture.lef,o,1000);
    require(demand(expanded,graph.node(1,2,10))==1,"ordinary Guide extension retains one demand per cell");
    require(demand(expanded,graph.node(1,1,10))==5,"long Guide does not double-count TSV cell");
    auto duplicate=guides;duplicate.push_back(guides.front());
    const auto redundant=measure_official_net_guides(net,duplicate,graph,fixture.lef,o,1000);
    require(redundant.demand==measured.demand,"repeated same-net rectangle cannot enlarge reservation");
    auto legacy=o;legacy.tsv_track_demand=false;
    const auto original=measure_official_net_guides(net,guides,graph,fixture.lef,legacy,1000);
    require(demand(original,graph.node(1,1,10))==2 && demand(original,graph.node(1,1,11))==2,
            "opt-out preserves legacy Guide plus constant TSV demand");
    const auto geometry=measure_official_net_guides(net,guides,graph.grid(),graph.die_area(),fixture.lef,o,1000);
    require(geometry.demand==original.demand,"geometry-only overload remains an unmasked legacy resource proxy");
    require(measured.wirelength==original.wirelength && measured.tsv_cells==original.tsv_cells,
            "new demand model does not alter wirelength or selected TSV");
    auto own=net;Pin pin;pin.name="own";pin.layer="metal10";pin.layer_idx=10;
    pin.coord={150,150};pin.port_rects={{10,{145,145,155,155}}};own.pins.push_back(pin);
    const auto exempt=measure_official_net_guides(own,guides,graph,fixture.lef,o,1000);
    require(demand(exempt,graph.node(1,1,10))==0 && demand(exempt,graph.node(1,1,11))==2,
            "actual own PIN layer keeps documented Guide/TSV exemption without exempting other layers");
    auto zero=o;zero.tsv_congestion_ratio=0;
    const auto guide_only=measure_official_net_guides(net,guides,graph,fixture.lef,zero,1000);
    require(demand(guide_only,graph.node(1,1,10))==1 && demand(guide_only,graph.node(1,1,11))==1,
            "zero TSV ratio leaves the obligatory landing Guide demand");
    auto invalid=o;invalid.score_20261020=false;
    rejects([&]{measure_official_net_guides(net,guides,graph,fixture.lef,invalid,1000);},
            "new demand requires matching graph and metric profile");
    Fixture empty(dir/"zero-capacity",10,100,true);RoutingGraph missing(empty.def,empty.lef,graph_options());
    require(missing.tsv_landing_tracks({150,150},11)==0,"zero-capacity layer reserves no nonexistent physical tracks");
    const auto blocked=measure_official_net_guides(empty.def.nets().front(),landing(missing),missing,empty.lef,o,1000);
    require(demand(blocked,missing.node(1,1,11))==1,"zero-capacity TSV still retains its landing Guide for congestion accounting");
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("usage: tsv_track_demand_tests artifact-directory");
        const fs::path dir=argv[1];fs::create_directories(dir);
        track_geometry(dir);metric_union(dir);
        std::cout<<"TSV track demand checks: "<<checks<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
