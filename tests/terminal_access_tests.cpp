#include "router/negotiated_router.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace eda;
namespace fs=std::filesystem;
namespace {
size_t checks=0;
void require(bool pass,const std::string& message) {
    ++checks;if(!pass) throw std::runtime_error(message);
}
std::string write(const fs::path& path,const std::string& text) {
    std::ofstream out(path);out<<text;out.close();
    require(bool(out),"fixture write");return path.string();
}
std::string technology(double via_half=0.4) {
    std::string text;
    for(int layer=1;layer<=20;++layer) {
        const auto name=get_layer_name(layer);
        text+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(layer%2?"HORIZONTAL":"VERTICAL")+
            " ; WIDTH 1 ; END "+name+"\n";
    }
    for(int layer=1;layer<20;++layer) {
        const auto name="v"+std::to_string(layer),cut="cut"+std::to_string(layer);
        const auto half=std::to_string(via_half);
        text+="LAYER "+cut+" TYPE CUT ; END "+cut+
            "\nVIA "+name+" DEFAULT LAYER "+get_layer_name(layer)+
            " ; RECT -"+half+" -"+half+" "+half+" "+half+
            " ; LAYER "+cut+" ; RECT -0.1 -0.1 0.1 0.1 ; LAYER "+
            get_layer_name(layer+1)+" ; RECT -"+half+" -"+half+" "+half+" "+half+
            " ; END "+name+"\n";
    }
    return text;
}
std::string macro(bool tiny,bool alternate) {
    const std::string rect=tiny?"49.95 49.95 50.05 50.05":
        (alternate?"49.5 49.5 50.5 50.5":"49 49 51 51");
    std::string text="MACRO terminal CLASS BLOCK ; SIZE 100 BY 100 ; PIN A PORT LAYER metal1 ; RECT "+rect+" ; ";
    if(alternate) text+="RECT 40 40 49.6 60 ; ";
    else text+="LAYER metal2 ; RECT "+rect+" ; ";
    text+="END END A OBS LAYER metal1 ; RECT 0 0 100 100 ; ";
    if(!alternate) text+="LAYER metal2 ; RECT 0 0 100 100 ; ";
    return text+"END END terminal\n";
}
std::string design(const std::string& wall="") {
    std::string text="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 3000 3000 ) ; "
        "GCELLGRID X 0 DO 31 STEP 100 ; GCELLGRID Y 0 DO 31 STEP 100 ; ";
    for(int layer=1;layer<=20;++layer) text+="TRACKS X 10 DO 150 STEP 20 LAYER "+get_layer_name(layer)+
        " ; TRACKS Y 10 DO 150 STEP 20 LAYER "+get_layer_name(layer)+" ; ";
    text+="COMPONENTS "+std::string(wall.empty()?"1":"2")+" ; - u terminal + PLACED ( 1000 1000 ) N ; ";
    if(!wall.empty()) text+="- foreign wall + PLACED ( 1000 1000 ) N ; ";
    return text+"END COMPONENTS PINS 0 ; END PINS NETS 1 ; - n ( u A ) ; END NETS END DESIGN";
}
}
namespace eda {
struct PinLocalTestAccess {
    static std::vector<PinAccess> accesses(NegotiatedRouter& router,const Pin& pin) {
        return router.accesses(pin,1,10);
    }
    static const RoutingGraph& graph(const NegotiatedRouter& router) { return router.graph_; }
};
}
int main(int argc,char**argv) {
    try {
        if(argc!=2) throw std::runtime_error("usage: terminal_access_tests output_directory");
        fs::path output=argv[1];fs::create_directories(output);
        for(int kind=0;kind<5;++kind) {
            const bool foreign=kind==1,upper=kind==2,tiny=kind==3,alternate=kind==4;
            std::string wall;
            if(foreign || upper) wall="MACRO wall CLASS BLOCK ; SIZE 100 BY 100 ; OBS LAYER "+
                std::string(foreign?"metal1":"metal3")+" ; RECT 48 48 52 52 ; END END wall\n";
            LefParser lef;DefParser def;ThermalParser thermal;
            const auto name=std::to_string(kind);
            require(lef.parse(write(output/(name+".lef"),technology(alternate?1.2:0.4)+macro(tiny,alternate)+wall)),"LEF parse");
            require(def.parse(write(output/(name+".def"),design(wall)),lef),"DEF parse");
            RouterOptions options;options.time_limit=30;options.cell_pin_access=false;
            NegotiatedRouter router(def,lef,thermal,options);
            const auto& pin=def.nets()[0].pins[0];
            const auto& graph=PinLocalTestAccess::graph(router);
            require(!graph.legal_segment(pin.coord,pin.coord,pin.layer_idx),"ordinary access sees real OBS");
            std::vector<PinAccess> access;std::string failure;
            try { access=PinLocalTestAccess::accesses(router,pin); }
            catch(const std::runtime_error& e) { failure=e.what(); }
            if(foreign || upper || tiny) {
                require(access.empty() && !failure.empty(), "foreign OBS, upper-layer OBS, and oversize via remain rejected");
                continue;
            }
            require(!access.empty(),"actual PORT metal provides a legal via access");
            require(router.counters().port_terminal_accesses==1 && router.counters().abstract_terminal_accesses==0,
                    "standard PORT override is distinct from abstract CORE escape");
            for(const auto& candidate:access) {
                bool pin_covered=false,free_layer=false;
                for(const auto& g:candidate.guides) {
                    const int layer=get_layer_index(g.layer);const Rect rect(g.x1,g.y1,g.x2,g.y2);
                    require(graph.legal_pin_rect(rect,layer,pin),"every guide retains foreign OBS and exact own-PORT limits");
                    pin_covered|=layer==pin.layer_idx && rect.contains(pin.coord);
                    free_layer|=layer>=(alternate?2:3);
                }
                require(pin_covered && free_layer,"guides cover original terminal and escape to an unobstructed layer");
            }
            require(!graph.legal_pin_rect(Rect(1001,1001,1999,1999),1,pin),"owner bbox cannot exempt non-PORT OBS");
            if(alternate) require(!graph.legal_pin_via(pin.coord,1,pin),
                                   "alternate connected PORT works when the first PORT cannot hold the via");
        }
        std::cout<<"Terminal access tests passed: "<<checks<<" checks\n";return 0;
    } catch(const std::exception& e) {
        std::cerr<<"Terminal access tests failed: "<<e.what()<<"\n";return 1;
    }
}
