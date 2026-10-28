#include "objectives/official_metrics.hpp"
#include "scoring/compat_resources.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

using namespace eda;
namespace fs=std::filesystem;
namespace {
int checks=0;
void require(bool condition,const std::string& reason) {
    ++checks;
    if(!condition) throw std::runtime_error(reason);
}
void near(double actual,double expected,const std::string& reason) {
    require(std::abs(actual-expected)<=1e-10*std::max({1.0,std::abs(actual),std::abs(expected)}),reason);
}
template<class F> void rejects(F&& function,const std::string& reason) {
    bool failed=false;
    try { function(); } catch(const std::exception&) { failed=true; }
    require(failed,reason);
}
std::string write(const fs::path& path,const std::string& text) {
    std::ofstream stream(path);
    stream<<text;
    stream.close();
    if(!stream) throw std::runtime_error("cannot write V6 scoring fixture");
    return path.string();
}
std::string technology() {
    std::string text;
    for(int layer=1;layer<=20;++layer) {
        const auto name=get_layer_name(layer);
        text+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+
            (layer%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.2 ; END "+name+"\n";
    }
    return text+"MACRO block SIZE 300 BY 300 ; OBS LAYER metal1 ; RECT 0 101 100 104 ; END "
        "PIN p PORT LAYER metal1 ; RECT 90 10 110 20 ; "
        "LAYER metal2 ; RECT 205 125 215 135 ; END END p "
        "PIN unconnected PORT LAYER metal1 ; RECT 200 10 210 20 ; END END unconnected END block";
}
std::string design(bool all_tracks=true) {
    std::string text="UNITS DISTANCE MICRONS 1 ; DIEAREA ( 0 0 ) ( 300 300 ) ; "
        "GCELLGRID X 0 DO 4 STEP 100 ; GCELLGRID Y 0 DO 4 STEP 100 ; ";
    if(all_tracks) for(int layer=1;layer<=20;++layer) {
        const auto name=get_layer_name(layer);
        text+="TRACKS X 10 DO 15 STEP 20 LAYER "+name+" ; "
              "TRACKS Y 5 DO 30 STEP 10 LAYER "+name+" ; ";
    }
    return text+"COMPONENTS 1 ; - u block + PLACED ( 0 0 ) N ; END COMPONENTS "
        "PINS 2 ; - a + LAYER metal10 ( -6 -6 ) ( 6 6 ) + FIXED ( 50 50 ) N ; "
        "- b + LAYER metal11 ( -6 -6 ) ( 6 6 ) + FIXED ( 50 50 ) N ; END PINS "
        "NETS 2 ; - crossing ( PIN a ) ( PIN b ) ; - local ( u p ) ; END NETS END DESIGN";
}
OfficialMetricOptions metrics(bool updated) {
    OfficialMetricOptions options;
    options.gcell_width_dbu=100;
    options.gcell_height_dbu=100;
    options.score_20261020=updated;
    return options;
}
void test_profiles(const fs::path& directory) {
    LefParser lef;
    DefParser def;
    require(lef.parse(write(directory/"v6.lef",technology())),"parse V6 LEF");
    require(def.parse(write(directory/"v6.def",design()),lef,DefGridMapping::OFFICIAL_FULL),"parse V6 DEF");
    RoutingGraph legacy(def,lef,{RoutingGeometryMode::GLOBAL_GUIDE});
    RoutingGraph updated(def,lef,{RoutingGeometryMode::GLOBAL_GUIDE,true,true});
    compat::Resources expected(def,lef);
    require(!legacy.score_20261020() && updated.score_20261020(),"profiles are explicit and legacy stays default");
    require(legacy.allows_nonpreferred() && !updated.allows_nonpreferred(),
            "V6 profile forces preferred-direction adjacency even when caller enables nonpreferred");
    const int start=updated.node(0,0,1);
    require(updated.legal_edge(start,updated.node(1,0,1)) &&
            !updated.legal_edge(start,updated.node(0,1,1)),"V6 horizontal layer cannot route vertically");
    require(updated.legal_edge(updated.node(0,0,2),updated.node(0,1,2)) &&
            !updated.legal_edge(updated.node(0,0,2),updated.node(1,0,2)),"V6 vertical layer cannot route horizontally");
    require(std::string(updated.capacity_model()).find("20261020-compat")!=std::string::npos,
            "capacity diagnostic identifies reconstructed October 20 model");
    for(int id=0;id<static_cast<int>(updated.size());++id) {
        require(updated.official_capacity(id)==expected.capacity(id),"V6 capacity exactly matches compat resources");
        require(updated.official_obstacle_demand(id)==expected.fixed_demand(id),"V6 fixed demand exactly matches compat resources");
    }
    require(updated.official_obstacle_demands()==expected.fixed_demands(),"bulk fixed-demand access uses updated resource data");
    require(legacy.official_obstacle_demand(updated.node(0,1,1))==1 &&
            updated.official_obstacle_demand(updated.node(0,1,1))==0,
            "V6 removes the predecessor track lying outside an obstacle");
    require(legacy.official_obstacle_demand(updated.node(0,0,10))==0 &&
            updated.official_obstacle_demand(updated.node(0,0,10))==1 &&
            updated.official_obstacle_demand(updated.node(0,0,11))==2,
            "V6 counts IO pin track occupancy once in fixed demand");
    const auto& crossing=def.nets()[0];
    const auto& local=def.nets()[1];
    require(legacy.own_pin_nodes(crossing).empty(),"legacy demand has no own-pin exemption");
    require(updated.own_pin_nodes(crossing)==std::vector<int>{updated.node(0,0,10),updated.node(0,0,11)},
            "IO mask includes exactly the net's two interface layers");
    require(updated.own_pin_nodes(local)==std::vector<int>{updated.node(0,0,1),updated.node(1,0,1),updated.node(2,1,1)},
            "macro pin mask covers multiple cells and preserves configured first-layer convention");

    const std::vector<GuideRect> guides{{45,45,55,55,"metal9"},{45,45,55,55,"metal10"},
                                        {45,45,55,55,"metal11"}};
    auto old_metrics=metrics(false),new_metrics=metrics(true);
    const auto old_result=measure_official_net_guides(crossing,guides,legacy,lef,old_metrics,1500);
    const auto new_result=measure_official_net_guides(crossing,guides,updated,lef,new_metrics,1500);
    const auto geometry_result=measure_official_net_guides(crossing,guides,def.gcell_grid(),def.die_area(),lef,new_metrics,1500);
    require(old_result.demand==geometry_result.demand,"geometry-only overload leaves demand unmasked for standalone scorer");
    require(new_result.demand==std::vector<std::pair<int,int>>{{updated.node(0,0,9),1}},
            "V6 skips both guide and TSV demand on own-pin cells, preserving another layer");
    near(new_result.wirelength,old_result.wirelength,"pin demand exemption never removes wirelength");
    require(new_result.tsv_cells==old_result.tsv_cells,"pin demand exemption never removes TSV thermal occupancy");
    Net foreign;
    foreign.name="foreign";
    Pin pin;
    pin.layer="metal10";pin.layer_idx=10;pin.coord={250,50};pin.port_rects={{10,{245,45,255,55}}};
    foreign.pins.push_back(pin);
    const auto foreign_result=measure_official_net_guides(foreign,{{45,45,55,55,"metal10"}},updated,lef,new_metrics,100);
    require(foreign_result.demand==std::vector<std::pair<int,int>>{{updated.node(0,0,10),1}},
            "other net's pin does not exempt this net's guide demand");
    rejects([&]{measure_official_net_guides(crossing,guides,legacy,lef,new_metrics,1500);},
            "mismatched graph and metric profiles fail rather than silently mixing conventions");

    ThermalParser heat;
    require(heat.parse_json(write(directory/"heat.json",
        R"({"meta":{"ambientTemp":25},"nodes":[[50,50,0,35],[150,50,0,40],[250,50,0,45],[50,150,0,30],[150,150,0,55],[250,150,0,40],[50,250,0,40],[150,250,0,35],[250,250,0,30]]})"),
        def.gcell_grid(),1,def.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),"parse V6 heat fixture");
    std::vector<int> demand(updated.size());
    for(const auto& [id,amount]:new_result.demand) demand[id]+=amount;
    const auto total=aggregate_official_metrics({&new_result},demand,updated,heat,new_metrics,true);
    near(total.wirelength_score,100,"aggregate uses plateau below the reference");
    require(total.guide.demand[updated.node(0,0,10)]==1 && total.guide.demand[updated.node(0,0,11)]==2,
            "aggregate retains fixed pin occupancy after own-pin dynamic-demand exemption");
    near(total.wirelength_score,official_wirelength_score(new_result.wirelength,1500,new_metrics),
         "aggregate and shared candidate wirelength helper agree");
}

void test_platform_resources(const fs::path& directory) {
    std::string layers;
    for(int l=1;l<=20;++l) {
        const auto name=get_layer_name(l);
        layers+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+
            (l%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.2 ; END "+name+"\n";
    }
    const std::string macro="MACRO block SIZE 300 BY 300 ; PIN p PORT "
        "LAYER metal1 ; RECT 10 10 20 20 ; "
        "LAYER metal2 ; RECT 210 210 220 220 ; END END p END block";
    std::string input="UNITS DISTANCE MICRONS 1 ; DIEAREA ( 0 0 ) ( 300 300 ) ; "
        "GCELLGRID X 0 DO 4 STEP 100 ; GCELLGRID Y 0 DO 4 STEP 100 ; ";
    for(int l=1;l<=20;++l) input+="TRACKS "+std::string(l%2?"Y":"X")+
        " 0 DO 31 STEP 10 LAYER "+get_layer_name(l)+" ; ";
    input+="COMPONENTS 1 ; - u block + PLACED ( 0 0 ) N ; END COMPONENTS "
        "PINS 0 ; END PINS NETS 1 ; - local ( u p ) ; END NETS END DESIGN";
    LefParser lef;
    DefParser def;
    require(lef.parse(write(directory/"platform.lef",layers+macro)),"parse platform LEF");
    require(def.parse(write(directory/"platform.def",input),lef,DefGridMapping::OFFICIAL_FULL),"parse platform DEF");
    RoutingGraph historical(def,lef,{RoutingGeometryMode::GLOBAL_GUIDE,false,true});
    RoutingGraph platform(def,lef,{RoutingGeometryMode::GLOBAL_GUIDE,false,true,true});
    compat::Resources expected(def,lef,{true,false,true});
    require(!historical.platform_resources()&&platform.platform_resources(),"resource contract is opt-in and observable");
    require(std::string(platform.capacity_model()).find("20261024-platform")!=std::string::npos,
            "capacity diagnostics identify platform contract");
    for(int n=0;n<static_cast<int>(platform.size());++n) {
        require(platform.official_capacity(n)==expected.capacity(n),"graph platform capacity agrees at every cell");
        require(platform.official_obstacle_demand(n)==expected.fixed_demand(n),"graph platform fixed demand agrees at every cell");
    }
    const int projected=platform.node(2,2,1),physical=platform.node(2,2,2);
    require(historical.official_obstacle_demand(projected)==2&&platform.official_obstacle_demand(projected)==1,
            "platform fixed shape excludes its upper track without changing projection");
    require(platform.official_obstacle_demand(physical)==0,"physical secondary-layer PIN does not add fixed demand there");
    const auto& net=def.nets().front();
    require(platform.own_pin_nodes(net)==std::vector<int>{platform.node(0,0,1)},
            "graph excludes projected-only cells from own-PIN exemptions");
    const std::vector<GuideRect> guides{{200,200,300,300,"metal1"}};
    const auto options=metrics(true);
    const auto old_result=measure_official_net_guides(net,guides,historical,lef,options,1000);
    const auto new_result=measure_official_net_guides(net,guides,platform,lef,options,1000);
    require(old_result.demand.empty(),"historical projected PIN credit is retained in old profile");
    require(new_result.demand==std::vector<std::pair<int,int>>{{projected,1}},
            "platform charges Guide demand at projected-only PIN cell");
    near(old_result.wirelength,new_result.wirelength,"resource profile does not change geometric wirelength");
    require(net.pins.front().port_rects.size()==2&&net.pins.front().port_rects.back().layer_idx==2,
            "resource projection never rewrites physical PIN geometry");
    rejects([&]{RoutingGraph invalid(def,lef,{RoutingGeometryMode::GLOBAL_GUIDE,false,false,true});},
            "platform graph cannot silently use legacy scoring");
}

void test_documented_resources(const fs::path& directory) {
    LefParser lef;
    DefParser def, duplicate_def;
    require(lef.parse(write(directory/"documented.lef",technology())),"parse documented LEF");
    const auto input=design();
    require(def.parse(write(directory/"documented.def",input),lef,DefGridMapping::OFFICIAL_FULL),
            "parse documented DEF");
    auto duplicated=input;
    duplicated.insert(duplicated.find("COMPONENTS"),
        "TRACKS Y 5 DO 30 STEP 10 LAYER metal1 ; TRACKS X 10 DO 15 STEP 20 LAYER metal2 ; ");
    require(duplicate_def.parse(write(directory/"documented-duplicate-tracks.def",duplicated),lef,
            DefGridMapping::OFFICIAL_FULL),"parse duplicate-track DEF");
    const RoutingGraphOptions options{RoutingGeometryMode::GLOBAL_GUIDE,false,true,false,true};
    RoutingGraph graph(def,lef,options),duplicate_graph(duplicate_def,lef,options);
    require(graph.documented_rules() && !graph.platform_resources(),
            "documented resources are distinct from historical scorer diagnostics");
    require(std::string(graph.capacity_model())=="20261020-documented-actual-pin-layers-unique-tracks",
            "capacity diagnostics disclose the documented resource model");
    const int actual=graph.node(2,1,2),projected=graph.node(2,1,1);
    require(graph.official_obstacle_demand(actual)==1,
            "secondary-layer PIN occupies its actual layer's physical track");
    require(graph.official_obstacle_demand(projected)==0,
            "secondary-layer PIN cannot create projected fixed demand on the first layer");
    require(graph.own_pin_nodes(def.nets()[1])==
            std::vector<int>{graph.node(0,0,1),graph.node(1,0,1),actual},
            "own-PIN exemptions cover all and only actual PIN layers and cells");
    require(graph.official_capacity(graph.node(0,0,1))==10 &&
            graph.official_capacity(graph.node(0,0,2))==5,
            "capacity counts unique preferred-direction physical tracks");
    for(int id=0;id<static_cast<int>(graph.size());++id) {
        require(duplicate_graph.official_capacity(id)==graph.official_capacity(id),
                "repeated TRACKS declarations do not manufacture capacity");
        require(duplicate_graph.official_obstacle_demand(id)==graph.official_obstacle_demand(id),
                "repeated TRACKS declarations do not multiply fixed PIN/OBS demand");
    }
    require(duplicate_graph.own_pin_nodes(duplicate_def.nets()[1])==graph.own_pin_nodes(def.nets()[1]),
            "repeated TRACKS declarations leave actual own-PIN exemptions unchanged");
    rejects([&]{RoutingGraph invalid(def,lef,{RoutingGeometryMode::GLOBAL_GUIDE,false,true,true,true});},
            "documented optimization cannot activate historical platform projection");
    rejects([&]{RoutingGraph invalid(def,lef,{RoutingGeometryMode::GLOBAL_GUIDE,false,false,false,true});},
            "documented resources cannot silently use the old score profile");
}

void test_plateau() {
    const auto old_options=metrics(false),new_options=metrics(true);
    for(const auto& [length,expected]:std::vector<std::pair<double,double>>{{0,100},{80,100},{100,100},{120,100.0/1.2}})
        near(official_wirelength_score(length,100,new_options),expected,"documented plateau or reciprocal score");
    near(official_wirelength_score(80,100,old_options),80,"legacy tent below reference remains unchanged");
    near(official_wirelength_score(120,100,old_options),100.0/1.2,"legacy reciprocal above reference remains unchanged");
    near(official_wirelength_score(1,0,old_options),1,"legacy zero-reference fallback remains unchanged");
    rejects([&]{official_wirelength_score(1,0,new_options);},"updated zero-reference convention is not fabricated");
    rejects([&]{official_wirelength_score(-1,100,new_options);},"negative wirelength rejected");
    rejects([&]{official_wirelength_score(std::numeric_limits<double>::quiet_NaN(),100,new_options);},"non-finite wirelength rejected");
}
void test_no_tracks(const fs::path& directory) {
    LefParser lef;
    DefParser def;
    require(lef.parse(write(directory/"zero.lef",technology())),"parse zero-track LEF");
    require(def.parse(write(directory/"zero.def",design(false)),lef,DefGridMapping::OFFICIAL_FULL),"parse zero-track DEF");
    RoutingGraph graph(def,lef,{RoutingGeometryMode::GLOBAL_GUIDE,true,true});
    const int first=graph.node(0,0,1),next=graph.node(1,0,1);
    require(graph.official_capacity(first)==0 && graph.legal_node(first) && graph.legal_edge(first,next),
            "zero-track cells retain legal preferred adjacency and expose zero capacity for penalty");
    require(graph.official_obstacle_demand(first)==0,"no physical tracks means no fixed occupied-track demand");
    rejects([&]{RoutingGraph legacy(def,lef,{RoutingGeometryMode::GLOBAL_GUIDE});},
            "legacy missing-track checks remain unchanged");
    rejects([&]{RoutingGraph strict(def,lef,{RoutingGeometryMode::PHYSICAL_STRICT,true,true});},
            "updated score cannot silently enable an incompatible strict geometry profile");
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("v6_scoring_tests OUTPUT_DIRECTORY");
        const fs::path directory=argv[1];
        fs::create_directories(directory);
        test_profiles(directory);
        test_platform_resources(directory);
        test_documented_resources(directory);
        test_plateau();
        test_no_tracks(directory);
        std::cout<<"v6_scoring_tests: "<<checks<<" checks PASS\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"v6_scoring_tests: "<<error.what()<<"\n";
        return 1;
    }
}
