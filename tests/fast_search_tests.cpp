#include "router/negotiated_router.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace eda;
namespace fs=std::filesystem;
namespace eda {
struct PinLocalTestAccess {
 static void begin_refinement(NegotiatedRouter& router) { router.initial_build_=false; }
 static void congest_lower_layers(NegotiatedRouter& router,const RoutingGraph& graph) {
  for(size_t n=0;n<graph.size();++n) if(graph.layer(int(n))<=4) router.demand_[n]=100;
 }
};
}
int main(int argc,char**argv) {
 try {
  if(argc!=2) throw std::runtime_error("fixture directory required");
  const fs::path folder=argv[1];fs::create_directories(folder);
  auto write=[&](const char*name,const std::string&text) {const auto path=folder/name;std::ofstream out(path);out<<text;out.close();if(!out)throw std::runtime_error("fixture write failed");return path.string();};
  std::string tech;
  for(int l=1;l<=20;++l) {
   const auto name=get_layer_name(l);
   tech+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(l%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.1 ; END "+name+"\n";
  }
  for(int l=1;l<20;++l) {
   const auto name="v"+std::to_string(l),cut="cut"+std::to_string(l);
   tech+="LAYER "+cut+" TYPE CUT ; END "+cut+" VIA "+name+" DEFAULT LAYER "+get_layer_name(l)+
    " ; RECT -0.05 -0.05 0.05 0.05 ; LAYER "+cut+" ; RECT -0.02 -0.02 0.02 0.02 ; LAYER "+
    get_layer_name(l+1)+" ; RECT -0.05 -0.05 0.05 0.05 ; END "+name+"\n";
  }
  // The center (255,275) on M11 fits WIDTH, while both planar corridors
  // and the M11/M12 via pad are obstructed. A legal node alone is not
  // evidence that a terminal can reach the rest of the resource graph.
  tech+="MACRO cavity CLASS BLOCK ; SIZE 160 BY 100 ; OBS LAYER metal11 ; RECT 25.0 27.0 25.4 28.0 ; RECT 25.6 27.0 26.0 28.0 ; LAYER metal12 ; RECT 25.49 27.49 25.51 27.51 ; END END cavity\n";
  std::string design="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 1600 1000 ) ; GCELLGRID X 0 DO 161 STEP 10 ; GCELLGRID Y 0 DO 101 STEP 10 ; ";
  for(int l=1;l<=20;++l) design+="TRACKS X 1 DO 800 STEP 2 LAYER "+get_layer_name(l)+" ; TRACKS Y 1 DO 500 STEP 2 LAYER "+get_layer_name(l)+" ; ";
  design+="COMPONENTS 1 ; - block cavity + FIXED ( 0 0 ) N ; END COMPONENTS PINS 2 ; - a + LAYER metal3 ( -1 -1 ) ( 1 1 ) + FIXED ( 205 205 ) N ; - b + LAYER metal4 ( -1 -1 ) ( 1 1 ) + FIXED ( 255 275 ) N ; END PINS NETS 1001 ; ";
  for(int i=0;i<1001;++i)design+="- n"+std::to_string(i)+" ( PIN a ) ( PIN b ) ; ";
  design+="END NETS END DESIGN";
  LefParser lef;DefParser def;ThermalParser thermal;
  if(!lef.parse(write("fast.lef",tech))||!def.parse(write("fast.def",design),lef)) throw std::runtime_error("fixture parse failed");
  RoutingGraph graph(def,lef);RouterOptions options;options.search_limit=1;
  NegotiatedRouter fast(def,lef,thermal,options);
  PinAccess source;source.node=graph.node(20,20,3);const int target=graph.node(25,27,4);
  const auto path=fast.search({source},{target},NetRoute{},1,10,false);
  if(path.status!=PathResult::Status::FOUND||fast.counters().fast_initial_paths!=1||fast.counters().expansions!=0)
   throw std::runtime_error("fast template did not bypass the exact-search budget");
  if(path.nodes.front()!=source.node||path.nodes.back()!=target||path.source!=0) throw std::runtime_error("lost terminal");
  double cost=0;
  auto marginal=[](double cap){return std::pow(std::max(0.0,1-cap),2)+std::pow(1/std::max(1.0,cap),4);};
  for(size_t i=1;i<path.nodes.size();++i) {
   const int a=path.nodes[i-1],b=path.nodes[i];
   if(!graph.legal_edge(a,b)) throw std::runtime_error("template fabricated an illegal edge");
   cost+=graph.edge_length(a,b)/graph.ell0()+(graph.layer(a)!=graph.layer(b)?0.2:0);
   const double capacity=graph.edge_capacity(a,b);if(capacity>=0)cost+=marginal(capacity);
   cost+=marginal(graph.guide_capacity(b));
  }
  if(std::abs(cost-path.cost)>1e-8) throw std::runtime_error("fast path did not retain the true congestion cost");
  NegotiatedRouter congested(def,lef,thermal,options);
  PinLocalTestAccess::congest_lower_layers(congested,graph);
  const auto distributed=congested.search({source},{target},NetRoute{},1,10,false);
  if(distributed.status!=PathResult::Status::FOUND ||
     std::none_of(distributed.nodes.begin(),distributed.nodes.end(),[&](int n){return graph.layer(n)>4;}))
   throw std::runtime_error("fast template budget failed to evaluate uncongested routing layers");
  for(size_t i=1;i<distributed.nodes.size();++i) if(!graph.legal_edge(distributed.nodes[i-1],distributed.nodes[i]))
   throw std::runtime_error("congestion detour fabricated an illegal edge");
  const int isolated=graph.node(25,27,11);PinAccess upper;upper.node=graph.node(20,20,13);
  if(!graph.legal_node(isolated)||!graph.neighbors(isolated).empty())throw std::runtime_error("fixture cavity is not isolated");
  const auto absent=fast.search({upper},{isolated},NetRoute{},11,20,false);
  if(absent.status!=PathResult::Status::UNREACHABLE||fast.counters().expansions!=0)
   throw std::runtime_error("isolated target consumed a search budget or was labeled reachable");
  PinAccess coincident;coincident.node=isolated;
  const auto direct=fast.search({coincident},{isolated},NetRoute{},11,20,false);
  if(direct.status!=PathResult::Status::FOUND||direct.nodes!=std::vector<int>{isolated})throw std::runtime_error("coincident isolated terminals were rejected");
  RouterOptions exact;exact.fast_initial=false;exact.search_limit=50000;
  NegotiatedRouter oracle(def,lef,thermal,exact);
  const auto astar=oracle.search({source},{target},NetRoute{},1,10,false);
  const auto dijkstra=oracle.search({source},{target},NetRoute{},1,10,true);
  if(astar.status!=PathResult::Status::FOUND||dijkstra.status!=PathResult::Status::FOUND||std::abs(astar.cost-dijkstra.cost)>1e-8)
   throw std::runtime_error("disabling fast initial lost exact A*/Dijkstra equality");
  RouterOptions enabled=exact;enabled.fast_initial=true;
  NegotiatedRouter explicit_dijkstra(def,lef,thermal,enabled);
  const auto enabled_dijkstra=explicit_dijkstra.search({source},{target},NetRoute{},1,10,true);
  if(enabled_dijkstra.status!=PathResult::Status::FOUND||std::abs(enabled_dijkstra.cost-dijkstra.cost)>1e-8||
     explicit_dijkstra.counters().fast_initial_paths!=0||explicit_dijkstra.counters().expansions==0)
   throw std::runtime_error("Dijkstra with fast initial enabled bypassed the exact-search oracle");
  NegotiatedRouter legacy_refinement(def,lef,thermal,options);
  PinLocalTestAccess::begin_refinement(legacy_refinement);
  const auto legacy_limited=legacy_refinement.search({source},{target},NetRoute{},1,10,false);
  if(legacy_limited.status!=PathResult::Status::SEARCH_LIMIT||legacy_refinement.counters().fast_initial_paths!=0)
   throw std::runtime_error("legacy refinement unexpectedly changed to approximate fast search");

  // Native assets are prepared independently; these tests never copy or alter
  // the official package. A missing runtime skips only this integration block.
  const auto runtime=fs::path(__FILE__).parent_path().parent_path()/"output/runtime/official-flute";
  bool have_runtime=true;
  for(const auto* asset:{"libflute.so","tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so","POWV9.dat","POST9.dat"})
   have_runtime=have_runtime&&fs::is_regular_file(runtime/asset);
  if(!have_runtime) std::cout<<"SKIP official refinement fast-search integration: native runtime unavailable at "<<runtime<<'\n';
  else {
   // Keep the full declared grid, including partial boundary cells, while
   // abstract GLOBAL_GUIDE adjacency replaces physical OBS/VIA feasibility.
   std::string official_design=design;
   const std::string old_die="DIEAREA ( 0 0 ) ( 1600 1000 )";
   official_design.replace(official_design.find(old_die),old_die.size(),"DIEAREA ( 0 0 ) ( 1595 995 )");
   DefParser official_def;
   if(!official_def.parse(write("fast-official.def",official_design),lef,DefGridMapping::OFFICIAL_FULL))
    throw std::runtime_error("official full-grid fixture parse failed");
   RouterOptions official=options;official.official_model=true;official.official_runtime=runtime.string();
   official.graph_options.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
   official.official_metrics.gcell_width_dbu=official_def.gcell_pitch_x();
   official.official_metrics.gcell_height_dbu=official_def.gcell_pitch_y();
   RoutingGraph global(official_def,lef,official.graph_options);
   PinAccess official_source;official_source.node=global.node(20,20,3);
   const int official_target=global.node(25,27,4);
   NegotiatedRouter refinement(official_def,lef,thermal,official);
   PinLocalTestAccess::begin_refinement(refinement);
   const auto quick=refinement.search({official_source},{official_target},NetRoute{},1,10,false);
   if(quick.status!=PathResult::Status::FOUND||refinement.counters().fast_initial_paths!=1||refinement.counters().expansions!=0)
    throw std::runtime_error("official refinement failed to return a legal template before exact search");
   if(quick.nodes.front()!=official_source.node||quick.nodes.back()!=official_target||quick.source!=0)
    throw std::runtime_error("official refinement template lost its endpoints");
   for(size_t i=1;i<quick.nodes.size();++i) if(!global.legal_edge(quick.nodes[i-1],quick.nodes[i]))
    throw std::runtime_error("official refinement template fabricated an illegal edge");
   // The exact modes are checked at the partial DIE boundary as well as
   // against each other. Fast=true must not affect an explicit Dijkstra call.
   PinAccess border_source;border_source.node=global.node(157,97,3);
   const int border_target=global.node(159,99,4);
   if(global.point(border_target).x!=1592.5||global.point(border_target).y!=992.5)
    throw std::runtime_error("official boundary fixture did not retain a partial GCell");
   RouterOptions official_exact=official;official_exact.fast_initial=false;official_exact.search_limit=50000;
   NegotiatedRouter official_oracle(official_def,lef,thermal,official_exact);
   PinLocalTestAccess::begin_refinement(official_oracle);
   const auto exact_astar=official_oracle.search({border_source},{border_target},NetRoute{},1,10,false);
   const auto exact_dijkstra=official_oracle.search({border_source},{border_target},NetRoute{},1,10,true);
   RouterOptions official_enabled=official_exact;official_enabled.fast_initial=true;
   NegotiatedRouter official_dijkstra(official_def,lef,thermal,official_enabled);
   PinLocalTestAccess::begin_refinement(official_dijkstra);
   const auto selected_dijkstra=official_dijkstra.search({border_source},{border_target},NetRoute{},1,10,true);
   if(exact_astar.status!=PathResult::Status::FOUND||exact_dijkstra.status!=PathResult::Status::FOUND||
      selected_dijkstra.status!=PathResult::Status::FOUND||std::abs(exact_astar.cost-exact_dijkstra.cost)>1e-8||
      std::abs(selected_dijkstra.cost-exact_dijkstra.cost)>1e-8||official_oracle.counters().fast_initial_paths!=0||
      official_dijkstra.counters().fast_initial_paths!=0||official_dijkstra.counters().expansions==0)
    throw std::runtime_error("official boundary exact-mode/Dijkstra override disagrees with oracle");
   std::cout<<"PASS official refinement shortcut, full-grid boundary exact oracle, fast-enabled Dijkstra override\n";
  }
  std::cout<<"PASS fast template legality, true cost and layer-diverse congestion detour, isolated-target early rejection, legacy refinement preservation, exact-mode oracle\n";
 } catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
