#include "objectives/official_metrics.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
using namespace eda;
namespace fs=std::filesystem;
namespace {
size_t checks=0;
void require(bool c,const std::string& reason){++checks;if(!c)throw std::runtime_error(reason);}
void near(double a,double b,const std::string& reason){require(std::abs(a-b)<1e-10*std::max({1.0,std::abs(a),std::abs(b)}),reason);}
template<class F> void rejects(F f,const std::string& reason){bool failed=false;try{f();}catch(const std::exception&){failed=true;}require(failed,reason);}
std::string write(const fs::path& p,const std::string& s){std::ofstream f(p);f<<s;f.close();if(!f)throw std::runtime_error("fixture write failed");return p.string();}
}
int main(int argc,char**argv){try{
    if(argc<2)throw std::runtime_error("official_metrics_tests OUTPUT [original-flute-runtime]");
    const fs::path folder=argv[1];fs::create_directories(folder);
    std::string technology,tracks;
    for(int l=1;l<=20;++l){const auto name=get_layer_name(l);technology+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(l%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.2 ; END "+name+"\n";tracks+="TRACKS X 5 DO 30 STEP 10 LAYER "+name+" ; TRACKS Y 5 DO 30 STEP 10 LAYER "+name+" ; ";}
    technology+="MACRO block CLASS BLOCK ; SIZE 30 BY 30 ; OBS LAYER metal3 ; RECT 20 0 25 10 ; END PIN p PORT LAYER metal3 ; RECT 20 0 25 10 ; END END p END block";
    const std::string design="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 300 300 ) ; GCELLGRID X 0 DO 4 STEP 100 ; GCELLGRID Y 0 DO 4 STEP 100 ; "+tracks+"COMPONENTS 1 ; - block block + PLACED ( 0 0 ) N ; END COMPONENTS PINS 2 ; - a + LAYER metal10 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; - b + LAYER metal11 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; END PINS NETS 1 ; - n ( PIN a ) ( PIN b ) ; END NETS END DESIGN";
    LefParser lef;DefParser def;ThermalParser heat;
    require(lef.parse(write(folder/"tech.lef",technology)),"fixture LEF parse");
    require(def.parse(write(folder/"design.def",design),lef),"fixture DEF parse");
    require(heat.parse_json(write(folder/"heat.json",R"({"meta":{"ambientTemp":25},"nodes":[[5,5,0,35],[15,5,0,40],[25,5,0,45],[5,15,0,30],[15,15,0,55],[25,15,0,40],[5,25,0,40],[15,25,0,35],[25,25,0,30]]})"),def.gcell_grid(),10,def.die_area()),"complete fixture heat parse");
    RoutingGraph graph(def,lef,RoutingGraphOptions{RoutingGeometryMode::GLOBAL_GUIDE});
    OfficialMetricOptions o;o.gcell_width_dbu=100;o.gcell_height_dbu=100;o.thermal_gain=2;
    const auto& net=def.nets().front();
    const std::vector<GuideRect> pad{{45,45,55,55,"metal10"},{45,45,55,55,"metal11"}};
    const auto one=measure_official_net_guides(net,pad,graph,lef,o,1000);
    near(one.wirelength,800,"two GCells plus original TSV ratio cost");
    require(one.demand.size()==2 && one.demand[0].second==2 && one.demand[1].second==2,"guide dedup plus one TSV demand on each layer");
    require(one.tsv_cells==std::vector<int>{0},"single selected TSV cell");
    const auto contained=measure_official_net_guides(net,{{45,45,55,55,"metal10"},{47,47,53,53,"metal10"},{45,45,55,55,"metal11"}},graph,lef,o,1000);
    near(contained.wirelength,1400,"every submitted lower rectangle pays a cross-layer cost; only planar cells deduplicate");
    std::vector<int> demand(graph.size());for(auto[n,d]:one.demand)demand[n]+=d;
    const auto total=aggregate_official_metrics({&one},demand,graph,heat,o,true);
    near(total.wirelength_score,80,"sub-baseline length is penalized by L, not rewarded");
    near(total.congestion_score,100,"base tracks at capacity have no overflow");
    require(total.guide.demand.at(graph.node(2,0,3))==10,"OBS and all-port occupancy union included without guide");
    near(total.thermal_score,100*(.5+.5*std::tanh(2*total.guide.correlation)),"saturation heat gain");
    near(total.guide.quality,(.45*80+.30*100+.25*total.thermal_score)/100,"weighted quality uses unit interval");
    auto high=demand;high[graph.node(2,2,1)]=25;
    const auto congested=aggregate_official_metrics({&one},high,graph,heat,o,false);
    require(congested.guide.overflow==15 && congested.guide.max_overflow==15 && congested.overflow_entries==1,"analytic single overloaded GCell");
    near(congested.squared_normalized_overflow,.0225,"capacity-normalized quadratic overflow");
    near(congested.congestion_score,100/std::sqrt(1.0225),"analytic overflow score");
    require(congested.guide.demand.empty(),"large aggregate avoids dense copy");
    Net local;local.name="local";Pin p;p.name="a";p.layer="metal1";p.layer_idx=1;p.coord={50,50};local.pins={p};
    const auto planar=measure_official_net_guides(local,{{45,45,155,55,"metal1"},{145,45,255,55,"metal1"}},graph,lef,o,500);
    near(planar.wirelength,300,"overlapping guide rectangles cover three unique GCells");
    require(planar.demand.size()==3 && std::all_of(planar.demand.begin(),planar.demand.end(),[](auto x){return x.second==1;}),"one demand per net/layer/cell");
    // Two distinct lower-layer strips occupy the same GCell but both need a
    // connection to the upper strip. Planar dedup must not erase either via.
    std::vector<GuideRect> vias{{0,0,100,40,"metal1"},{0,60,100,100,"metal1"},{40,0,60,100,"metal2"}};
    const auto charged=measure_official_net_guides(local,vias,graph,lef,o,500);
    near(charged.wirelength,800,"same-cell lower rectangles retain both via charges");
    std::reverse(vias.begin(),vias.end());
    near(measure_official_net_guides(local,vias,graph,lef,o,500).wirelength,charged.wirelength,
         "rectangle order cannot change the documented charge");
    vias.push_back(vias.back());
    require(measure_official_net_guides(local,vias,graph,lef,o,500).wirelength>=charged.wirelength,
            "redundant geometry cannot reduce measured cost");
    rejects([&]{measure_official_net_guides(local,{{-1,0,100,100,"metal1"}},graph,lef,o,500);},
            "measurement cannot silently trim an out-of-DIE rectangle");

    const std::vector<GuideRect> two{{45,45,55,55,"metal10"},{45,45,55,55,"metal11"},{245,45,255,55,"metal10"},{245,45,255,55,"metal11"},{45,45,255,55,"metal9"},{45,45,255,55,"metal12"}};
    const auto two_result=measure_official_net_guides(net,two,graph,lef,o,1000);
    require(two_result.tsv_points.size()==1 && two_result.tsv_points.front().x==250 && two_result.tsv_points.front().y==50,"two candidates select the one not intersecting all interface pin boxes");
    auto three=two;three.push_back({145,45,155,55,"metal10"});three.push_back({145,45,155,55,"metal11"});
    rejects([&]{measure_official_net_guides(net,three,graph,lef,o,1000);},"three TSV candidates rejected");
    auto invalid=o;invalid.gcell_width_dbu=0;
    rejects([&]{measure_official_net_guides(net,pad,graph,lef,invalid,1000);},"unknown original GCell step rejected");
    rejects([&]{aggregate_official_metrics({&one},std::vector<int>(3),graph,heat,o);},"ledger dimensions checked");
    near(official_thermal_gain_for_def_file("/scratch/open_3d_case001.input.def"),11.684,"original DEF alias gain");
    near(official_thermal_gain_for_def_file("toy.input.def"),21.492,"toy gain");
    near(official_thermal_gain_for_def_file("renamed.def"),20,"unknown name falls back to original default");
    if(argc>2){
        NativeFluteBaseline flute(argv[2]);require(flute.max_degree()==150,"actual native degree limit");
        const auto line=flute.evaluate_points({{0,0},{10,0}});near(line.wirelength_dbu,10,"original FLUTE line cost");require(line.via_count==1,"original FLUTE line via count (not geometric corner guess)");
        const auto zig=flute.evaluate_points({{0,0},{10,0},{10,10},{20,10}});near(zig.wirelength_dbu,30,"original FLUTE zig cost");require(zig.via_count==3,"original native zig via count");
        require(flute.evaluate_points(std::vector<Point>(151)).skipped_degree_limit,"exact high-fanout baseline skip");
        Net crossing;crossing.is_cross_die=true;crossing.pins={p};crossing.pins[0].name="first";crossing.pins[0].coord={0,0};crossing.pins.push_back(p);crossing.pins.back().name="second";crossing.pins.back().coord={10,0};
        near(flute.measure_net(crossing,100,o),3910,"baseline adds ten cross-die via costs and one TSV cost");
        const auto reference=[&](const std::vector<Point>& points){const auto f=flute.evaluate_points(points);return f.wirelength_dbu+f.via_count*o.via_cost_ratio*100;};
        Net quantized;Pin macro=p;macro.name="macro.A";macro.instance_name="macro";
        macro.coord={839039.9999999999,1025530};macro.port_rects={{14,{838990,1025275,839090,1025785}}};
        Pin endpoint=p;endpoint.name="endpoint";endpoint.coord={0,0};quantized.pins={macro,endpoint};
        near(flute.measure_net(quantized,100,o),reference({{839040,1025530},{0,0}}),"baseline center is averaged from transformed DBU rectangle endpoints");
        require(flute.evaluate_points({macro.coord,endpoint.coord}).wirelength_dbu+1==flute.evaluate_points({{839040,1025530},endpoint.coord}).wirelength_dbu,"generic native coordinate conversion still truncates");
        require(quantized.pins[0].coord.x==839039.9999999999,"baseline leaves physical pin coordinate unchanged");
        Net shapes;macro.coord={500,500};macro.port_rects={{1,{-1,-1,1,1}},{1,{9,-1,11,1}},{2,{9,9,11,11}}};
        endpoint.coord={20,10};shapes.pins={macro,endpoint};
        near(flute.measure_net(shapes,100,o),reference({{0,0},{20,10}}),"instance baseline uses only the first rectangle across PORTs and layers");
        shapes.pins[0].port_rects.push_back(shapes.pins[0].port_rects.front());
        near(flute.measure_net(shapes,100,o),reference({{0,0},{20,10}}),"secondary colocated rectangles do not add baseline points");
        shapes.pins[0].instance_name.clear();
        near(flute.measure_net(shapes,100,o),reference({{0,0},{20,10}}),"IO baseline uses only the first PORT rectangle");
        shapes.pins[0].port_rects.clear();
        near(flute.measure_net(shapes,100,o),reference({{500,500},{20,10}}),"callers without PORT metadata retain point baseline");
    }
    std::cout<<"official_metrics_tests: "<<checks<<" checks PASS\n";return 0;
}catch(const std::exception&e){std::cerr<<"official_metrics_tests: "<<e.what()<<"\n";return 1;}}
