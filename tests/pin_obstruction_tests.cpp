#include "router/routing_graph.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace eda;
namespace fs=std::filesystem;
int main(int argc,char** argv) {
 try {
  if(argc!=2) throw std::runtime_error("fixture directory required");
  fs::path folder=argv[1];fs::create_directories(folder);int checks=0;
  auto require=[&](bool okay,const char* message){++checks;if(!okay) throw std::runtime_error(message);};
  auto write=[&](const char* name,const std::string& data){auto p=folder/name;std::ofstream f(p);f<<data;f.close();if(!f) throw std::runtime_error("write failed");return p.string();};
  std::string tech;
  for(int l=1;l<=20;++l) tech+="LAYER "+get_layer_name(l)+" TYPE ROUTING ; DIRECTION "+(l%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.2 ; END "+get_layer_name(l)+"\n";
  tech+="LAYER cut1 TYPE CUT ; END cut1 VIA v12 DEFAULT LAYER metal1 ; RECT -0.1 -0.1 0.1 0.1 ; LAYER cut1 ; RECT -0.05 -0.05 0.05 0.05 ; LAYER metal2 ; RECT -0.1 -0.1 0.1 0.1 ; END v12 ";
  tech+="MACRO owner CLASS BLOCK ; SIZE 20 BY 20 ; PIN A PORT LAYER metal1 ; RECT 10 10 12 12 ; END END A OBS LAYER metal1 ; RECT 0 0 20 20 ; END END owner ";
  tech+="MACRO other CLASS BLOCK ; SIZE 20 BY 20 ; OBS LAYER metal1 ; RECT 0 0 20 20 ; END END other ";
  LefParser lef;require(lef.parse(write("port.lef",tech)),"LEF parse");
  for(bool foreign:{false,true}) {
   std::string deftext="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 300 300 ) ; GCELLGRID X 0 DO 4 STEP 100 ; GCELLGRID Y 0 DO 4 STEP 100 ; ";
   for(int l=1;l<=20;++l) deftext+="TRACKS X 5 DO 30 STEP 10 LAYER "+get_layer_name(l)+" ; TRACKS Y 5 DO 30 STEP 10 LAYER "+get_layer_name(l)+" ; ";
   deftext+=foreign?"COMPONENTS 2 ; - u owner + PLACED ( 0 0 ) N ; - v other + PLACED ( 0 0 ) N ; END COMPONENTS ":"COMPONENTS 1 ; - u owner + PLACED ( 0 0 ) N ; END COMPONENTS ";
   deftext+="PINS 1 ; - io + LAYER metal2 ( -1 -1 ) ( 1 1 ) + FIXED ( 250 250 ) N ; END PINS NETS 1 ; - n ( u A ) ( PIN io ) ; END NETS END DESIGN";
   DefParser def;require(def.parse(write("port.def",deftext),lef),"DEF parse");RoutingGraph graph(def,lef);
   const Pin* pin=nullptr;for(const auto& p:def.nets()[0].pins)if(p.instance_name=="u") pin=&p;
   require(pin!=nullptr,"owner metadata");
   require(!graph.legal_rect({109,109,111,111},1),"generic routing must remain blocked");
   require(graph.legal_pin_rect({109,109,111,111},1,*pin)==!foreign,"only own PORT overrules OBS");
   require(!graph.legal_pin_rect({99,109,111,111},1,*pin),"one DBU outside PORT rejected");
   require(!graph.legal_via({110,110},1),"generic via must remain blocked");
   require(graph.legal_pin_via({110,110},1,*pin)==!foreign,"enclosed own PORT via and foreign OBS");
   require(graph.pin_via_rects({110,110},1,*pin).empty()==foreign,"export same selected pin via");
   require(!graph.legal_pin_via({100,110},1,*pin),"via landing partly outside PORT rejected");
   require(!graph.legal_pin_rect({-1,109,111,111},1,*pin),"DIE never exempted");
   Pin wrong=*pin;wrong.instance_name="absent";
   require(!graph.legal_pin_rect({109,109,111,111},1,wrong),"wrong owner rejected");
   Pin split=*pin;split.port_rects={{1,{100,100,110,120}},{1,{110,100,120,120}}};
   require(graph.legal_pin_via({110,110},1,split)==!foreign,"connected PORT union covers landing across rectangle seam");
   split.port_rects={{1,{100,100,109,120}},{1,{111,100,120,120}}};
   require(!graph.legal_pin_via({110,110},1,split),"gap in PORT union cannot cover via");
   for(int n=0;n<int(graph.size());++n) require(graph.has_neighbor(n)==!graph.neighbors(n).empty(),"neighbor existence agrees with full adjacency");
  }
  std::cout<<"Pin obstruction tests passed: "<<checks<<" checks\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
