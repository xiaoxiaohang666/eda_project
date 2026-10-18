#include "router/routing_graph.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
using namespace eda;
namespace fs=std::filesystem;
namespace {
int checks=0;
void require(bool condition,const std::string& reason){++checks;if(!condition)throw std::runtime_error(reason);}
std::string write(const fs::path& path,const std::string& text){std::ofstream out(path);out<<text;out.close();if(!out)throw std::runtime_error("fixture write failed");return path.string();}
std::string technology(){
 std::string text;
 for(int l=1;l<=20;++l){const auto name=get_layer_name(l);text+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(l%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.2 ; END "+name+"\n";}
 // No VIA definitions: abstract Guide-layer adjacency must not depend on CUT
 // enclosure feasibility. Every Pin shape is still counted as fixed demand.
 text+="MACRO block CLASS BLOCK ; ORIGIN 0 0 ; SIZE 30 BY 30 ; PIN p PORT LAYER metal1 ; RECT 4 0 6 10 ; LAYER metal2 ; RECT 12 10.2 13 12.9 ; END PORT LAYER metal20 ; RECT 22 21.2 23 24.7 ; END END p OBS LAYER metal1 ; RECT 3 0 6 10 ; RECT 3 0 6 10 ; RECT 11 11.5 15 12 ; END END block\n";
 return text;
}
std::string design(bool duplicate=false,bool outside=false){
 std::string text="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 300 300 ) ; GCELLGRID X 0 DO 4 STEP 100 ; GCELLGRID Y 0 DO 4 STEP 100 ; ";
 for(int l=1;l<=20;++l){const auto name=get_layer_name(l);text+="TRACKS X 10 DO 15 STEP 20 LAYER "+name+" ; TRACKS Y 5 DO 30 STEP 10 LAYER "+name+" ; ";}
 if(duplicate)text+="TRACKS Y 5 DO 30 STEP 10 LAYER metal1 ; ";
 if(outside)text+="TRACKS Y -5 DO 1 STEP 10 LAYER metal3 ; TRACKS Y 305 DO 1 STEP 10 LAYER metal3 ; ";
 text+="COMPONENTS 1 ; - b block + PLACED ( 0 0 ) N ; END COMPONENTS PINS 2 ; - a + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; - z + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( 250 250 ) N ; END PINS NETS 1 ; - n ( PIN a ) ( PIN z ) ; END NETS END DESIGN";
 return text;
}
void test_mode(const fs::path& folder){
 LefParser lef;DefParser def;
 require(lef.parse(write(folder/"global.lef",technology())),"LEF parsed");
 require(def.parse(write(folder/"global.def",design()),lef),"DEF parsed");
 RoutingGraph strict(def,lef),global(def,lef,RoutingGraphOptions{RoutingGeometryMode::GLOBAL_GUIDE});
 const int a=global.node(0,0,1),x=global.node(1,0,1),y=global.node(0,1,1);
 require(!strict.legal_node(a)&&global.legal_node(a),"OBS must not block abstract Guide nodes");
 require(!strict.legal_rect({40,40,60,60},1)&&global.legal_rect({40,40,60,60},1),"Guide envelope overlap with OBS is allowed only in global mode");
 require(!global.legal_rect({-1,1,10,10},1)&&!global.legal_rect({1,1,301,10},1),"global rectangles still stay in DIE");
 require(global.legal_edge(a,x)&&global.legal_edge(a,y),"global Guide graph permits both planar axes");
 require(global.legal_edge(a,global.node(0,0,2))&&!strict.legal_edge(a,strict.node(0,0,2)),"ordinary abstract via need not have a physical LEF VIA");
 require(!global.legal_edge(global.node(0,0,10),global.node(0,0,11)),"TSV cannot become unrestricted ordinary via");
 require(global.legal_via({50,50},1)&&!strict.legal_via({50,50},1),"via geometry semantics are explicit by mode");
 auto via=global.via_rects({0,0},1);require(via.size()==2,"abstract via emits two routing Guide rectangles");
 for(const auto& shape:via)require(global.legal_rect(shape.rect,get_layer_index(shape.layer)),"abstract edge via remains inside DIE");
 require(global.legal_segment({0,0},{100,0},1)&&!global.legal_segment({-1,0},{100,0},1),"Guide boundary access clips its cap but cannot move endpoint outside DIE");
 RoutingGraph preferred(def,lef,RoutingGraphOptions{RoutingGeometryMode::GLOBAL_GUIDE,false});
 require(preferred.legal_edge(a,x)&&!preferred.legal_edge(a,y)&&!preferred.allows_nonpreferred(),"nonpreferred Guide adjacency can be disabled explicitly");
 const int middle=global.node(1,1,2);require(global.neighbors(middle).size()==6&&global.has_neighbor(middle,2,2),"four planar and two adjacent-layer neighbors are retained");
 require(global.official_capacity(a)==10&&global.official_capacity(global.node(0,0,2))==5,"official H uses Y and V uses X even when both axes exist");
 require(strict.guide_capacity(strict.node(1,1,1))==5,"strict legacy scorer-axis capacity remains unchanged");
 require(global.official_obstacle_demand(a)==10,"duplicate OBS and overlapping Pin indices are unioned per cell");
 require(global.official_obstacle_demand(global.node(1,1,1))==4,"official track-interval index includes preceding index at boundary");
 require(global.official_obstacle_demand(global.node(1,1,2))==0,"all Pin PORT geometry follows scorer first-layer fixed-demand rule");
 require(global.official_obstacle_demand(global.node(2,2,1))==5,"every separate PORT of an unconnected macro Pin contributes fixed demand on first layer");
 require(global.official_obstacle_demand(global.node(2,0,1))==0,"IO terminal rectangles are not fixed macro Pin demand");
 require(global.guide_capacity(a)==0&&global.legal_edge(a,x),"zero residual capacity adds overflow cost instead of deleting Guide adjacency");
 require(global.official_obstacle_demands().size()==global.size(),"fixed demand dimensions equal graph states");
 bool rejected=false;try{(void)strict.official_capacity(a);}catch(const std::logic_error&){rejected=true;}
 require(rejected,"strict legacy proxy is not reported as official capacity");
}
void test_extended_grid_domain(const fs::path& folder){
 LefParser lef;DefParser def;require(lef.parse(write(folder/"extended.lef",technology())),"extended LEF parsed");
 std::string text=design();const auto pos=text.find("DIEAREA ( 0 0 ) ( 300 300 )");
 text.replace(pos,std::string("DIEAREA ( 0 0 ) ( 300 300 )").size(),"DIEAREA ( 0 0 ) ( 235 235 )");
 const auto terminal=text.find("( 250 250 )");text.replace(terminal,std::string("( 250 250 )").size(),"( 225 225 )");
 for(const std::string axis:{"X","Y"}) {
  const auto old="GCELLGRID "+axis+" 0 DO 4 STEP 100",replacement="GCELLGRID "+axis+" 0 DO 5 STEP 100";
  text.replace(text.find(old),old.size(),replacement);
 }
 require(def.parse(write(folder/"extended.def",text),lef,DefGridMapping::OFFICIAL_FULL),"extended official-domain DEF parsed");
 RoutingGraph global(def,lef,RoutingGraphOptions{RoutingGeometryMode::GLOBAL_GUIDE});
 const int border=global.node(2,2,1),outside=global.node(3,2,1);
 require(global.size()==320&&global.legal_node(border)&&!global.legal_node(outside),"official grid domain is retained but only cells intersecting DIE can route");
 require(global.point(border).x==217.5&&global.point(border).y==217.5,"partial border GCell uses center of its actual DIE intersection");
 require(!global.legal_edge(border,outside)&&global.legal_via(global.point(border),1),"DIE-outside detours are prohibited while partial-cell vias remain possible");
 require(global.die_area().x2==235&&global.die_area().y2==235,"scoring geometry can inspect actual DIE separately from declared grid domain");
}
void test_raw_track_contract(const fs::path& folder){
 LefParser lef;DefParser def;require(lef.parse(write(folder/"raw.lef",technology())),"raw LEF parsed");
 require(def.parse(write(folder/"raw.def",design(true,true)),lef),"raw DEF parsed");
 RoutingGraph global(def,lef,RoutingGraphOptions{RoutingGeometryMode::GLOBAL_GUIDE});
 const int a=global.node(0,0,1);require(global.official_capacity(a)==20,"official raw TRACKS patterns retain duplicate capacity");
 // Official build_track_list concatenates patterns without sorting. Python
 // bisect over [5,...295,5,...295] yields range(0,40) for an OBS y=[0,100].
 require(global.official_obstacle_demand(a)==40,"official unsorted-pattern bisect behavior is preserved without repairing scorer");
 require(global.guide_capacity(a)==-20&&global.legal_edge(a,global.node(1,0,1)),"negative residual capacity does not make global routing unreachable");
 require(global.official_capacity(global.node(0,0,3))==11&&global.official_capacity(global.node(0,2,3))==11,"outside TRACKS coordinates are clamped to official first/last GCell capacity");
}
}
int main(int argc,char**argv){try{if(argc!=2)throw std::runtime_error("fixture folder required");fs::path folder=argv[1];fs::create_directories(folder);test_mode(folder);test_raw_track_contract(folder);test_extended_grid_domain(folder);std::cout<<"PASS global Guide/strict separation, official capacity and fixed demand: "<<checks<<" checks\n";}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}