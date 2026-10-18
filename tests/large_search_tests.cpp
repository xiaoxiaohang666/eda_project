#include "router/negotiated_router.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace eda;
namespace fs=std::filesystem;
int main(int argc,char** argv) {
 try {
  if(argc!=2) throw std::runtime_error("fixture directory required");
  fs::path folder=argv[1];fs::create_directories(folder);
  auto write=[&](const char* name,const std::string& data) {
   auto path=folder/name;std::ofstream out(path);out<<data;out.close();
   if(!out) throw std::runtime_error("fixture write failed");
   return path.string();
  };
  std::string tech;
  for(int l=1;l<=20;++l) {
   auto name=get_layer_name(l);
   tech+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(l%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.1 ; END "+name+"\n";
  }
  for(int l=1;l<20;++l) {
   auto name="v"+std::to_string(l),cut="cut"+std::to_string(l);
   tech+="LAYER "+cut+" TYPE CUT ; END "+cut+" VIA "+name+" DEFAULT LAYER "+get_layer_name(l)+
    " ; RECT -0.05 -0.05 0.05 0.05 ; LAYER "+cut+" ; RECT -0.02 -0.02 0.02 0.02 ; LAYER "+
    get_layer_name(l+1)+" ; RECT -0.05 -0.05 0.05 0.05 ; END "+name+"\n";
  }
  std::string design="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 1600 1000 ) ; GCELLGRID X 0 DO 161 STEP 10 ; GCELLGRID Y 0 DO 101 STEP 10 ; ";
  for(int l=1;l<=20;++l) design+="TRACKS X 1 DO 800 STEP 2 LAYER "+get_layer_name(l)+" ; TRACKS Y 1 DO 500 STEP 2 LAYER "+get_layer_name(l)+" ; ";
  design+="COMPONENTS 0 ; END COMPONENTS PINS 2 ; - a + LAYER metal3 ( -1 -1 ) ( 1 1 ) + FIXED ( 205 205 ) N ; - b + LAYER metal4 ( -1 -1 ) ( 1 1 ) + FIXED ( 255 275 ) N ; END PINS NETS 1 ; - n ( PIN a ) ( PIN b ) ; END NETS END DESIGN";
  LefParser lef;DefParser def;ThermalParser thermal;
  if(!lef.parse(write("large.lef",tech))||!def.parse(write("large.def",design),lef)) throw std::runtime_error("fixture parse failed");
  RoutingGraph graph(def,lef);RouterOptions options;options.search_limit=50000;
  NegotiatedRouter router(def,lef,thermal,options);
  if(graph.size()!=320000) throw std::runtime_error("fixture does not exercise sparse search");
  PinAccess a;a.node=graph.node(20,20,3);PinAccess alternate;alternate.node=graph.node(21,20,3);
  const int target=graph.node(25,27,4);
  for(bool multi:{false,true}) {
   std::vector<PinAccess> sources{a};if(multi) sources.push_back(alternate);
   const auto astar=router.search(sources,{target},NetRoute{},1,10,false);
   const auto oracle=router.search(sources,{target},NetRoute{},1,10,true);
   if(astar.status!=PathResult::Status::FOUND||oracle.status!=PathResult::Status::FOUND||std::abs(astar.cost-oracle.cost)>1e-8)
    throw std::runtime_error("large sparse A*/Dijkstra mismatch");
   if(astar.nodes.front()!=sources.at(astar.source).node||astar.nodes.back()!=target) throw std::runtime_error("source/target mismatch");
   for(size_t i=1;i<astar.nodes.size();++i) if(!graph.legal_edge(astar.nodes[i-1],astar.nodes[i])) throw std::runtime_error("illegal path");
  }
  if(router.counters().search_peak_states>=graph.size()/10) throw std::runtime_error("local sparse search allocated whole-graph state");
  std::cout<<"PASS sparse A*/Dijkstra: graph_states="<<graph.size()<<" peak_search_states="<<router.counters().search_peak_states<<'\n';
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}