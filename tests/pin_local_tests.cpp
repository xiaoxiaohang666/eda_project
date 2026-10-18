#include "router/negotiated_router.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace eda;
namespace fs=std::filesystem;
namespace {
int checks=0;
void require(bool value,const std::string& message) {
    ++checks;if(!value) throw std::runtime_error(message);
}
std::string write(const fs::path& path,const std::string& content) {
    std::ofstream out(path);out<<content;out.close();
    require(bool(out),"fixture write");return path.string();
}
std::string technology(bool blocked) {
    std::string text;
    for(int l=1;l<=20;++l) {
        auto name=get_layer_name(l);
        text+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(l%2?"HORIZONTAL":"VERTICAL")+
            " ; WIDTH 0.2 ; END "+name+"\n";
    }
    for(int l=1;l<20;++l) {
        auto cut="cut"+std::to_string(l),via="v"+std::to_string(l);
        text+="LAYER "+cut+" TYPE CUT ; END "+cut+"\nVIA "+via+" DEFAULT LAYER "+get_layer_name(l)+
            " ; RECT -0.2 -0.2 0.2 0.2 ; LAYER "+cut+" ; RECT -0.1 -0.1 0.1 0.1 ; LAYER "+
            get_layer_name(l+1)+" ; RECT -0.2 -0.2 0.2 0.2 ; END "+via+"\n";
    }
    if(blocked) {
        text+="MACRO wall SIZE 30 BY 30 ; OBS ";
        for(int l=1;l<=10;++l) text+="LAYER "+get_layer_name(l)+" ; RECT 11 8 13 22 ; ";
        text+="END END wall\n";
    }
    return text;
}
std::string design(Point a,Point b,int la,int lb,bool blocked) {
    std::string text="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 300 300 ) ; "
        "GCELLGRID X 0 DO 4 STEP 100 ; GCELLGRID Y 0 DO 4 STEP 100 ; ";
    for(int l=1;l<=20;++l) text+="TRACKS X 10 DO 15 STEP 20 LAYER "+get_layer_name(l)+
        " ; TRACKS Y 5 DO 30 STEP 10 LAYER "+get_layer_name(l)+" ; ";
    text+=blocked?"COMPONENTS 1 ; - w wall + PLACED ( 0 0 ) N ; END COMPONENTS ":
        "COMPONENTS 0 ; END COMPONENTS ";
    auto pin=[](const std::string& name,Point p,int l) {
        return "- "+name+" + LAYER "+get_layer_name(l)+" ( -1 -1 ) ( 1 1 ) + FIXED ( "+
            std::to_string(int(p.x))+" "+std::to_string(int(p.y))+" ) N ; ";
    };
    return text+"PINS 2 ; "+pin("a",a,la)+pin("b",b,lb)+
        "END PINS NETS 1 ; - local ( PIN a ) ( PIN b ) ; END NETS END DESIGN";
}
}
namespace eda {
struct PinLocalTestAccess {
    static void check(NegotiatedRouter& router,const DefParser& def,const LefParser& lef,
                      Point a,Point b,int la,int lb,bool blocked,bool crossing) {
        const auto candidates=router.pin_local_candidates(0);
        if(blocked) {
            require(candidates.empty(),"OBS wall rejects every monotone direct template");
            if(!router.graph_.legal_segment(a,a,la)) {
                const auto escapes=router.accesses(a,la,la<=10?1:11,la<=10?10:20);
                require(!escapes.empty(),"flat terminal cap escapes without shrinking LEF width");
                for(const auto& access:escapes) for(const auto& g:access.guides)
                    require(router.graph_.legal_rect({g.x1,g.y1,g.x2,g.y2},get_layer_index(g.layer)),
                            "every terminal escape guide avoids all OBS");
            }
            const auto searched=router.build_route(0,-1);
            require(!searched.explicit_geometry,"blocked local route falls back to grid search");
            require(searched.length>a.distance_to(b),"wall fallback must make a physical detour");
            GuideMap guide{{"local",searched.guides}};std::string error;
            require(validate_guides(def,lef,guide,error),"searched fallback connectivity: "+error);
            return;
        }
        require(!candidates.empty(),"local candidates exist");
        auto shortest=std::min_element(candidates.begin(),candidates.end(),[](const auto& x,const auto& y) {
            return std::tie(x.length,x.vias)<std::tie(y.length,y.vias);
        });
        require(shortest->explicit_geometry,"candidate retains explicit geometry mode");
        require(std::abs(shortest->length-a.distance_to(b))<1e-8,"direct route reaches Manhattan length");
        if(a.y==b.y&&la==1&&lb==1) require(shortest->vias==0,"zero-length template legs add no via excursion");
        if(la!=lb) require(shortest->vias>=std::abs(la-lb),"different-layer pins retain via stack");
        GuideMap guide{{"local",shortest->guides}};std::string error;
        require(validate_guides(def,lef,guide,error),"explicit guide connectivity/pin coverage: "+error);
        for(const auto& g:shortest->guides) {
            int l=get_layer_index(g.layer);
            require((l>10)==(la>10),"direct route stays inside its die");
            require(router.graph_.legal_rect({g.x1,g.y1,g.x2,g.y2},l),"explicit guides avoid OBS and die boundary");
        }
        int planar=0;
        for(auto e:shortest->edges) {
            auto [u,v]=edge_nodes(e);require(router.graph_.legal_edge(u,v),"reserved edge remains legal");
            planar+=router.graph_.layer(u)==router.graph_.layer(v);
        }
        require((planar>0)==crossing,"crossing cell boundary reserves physical planar resources");
        if(crossing&&a.y==b.y&&la==1&&lb==1) {
            auto e=route_edge(router.graph_.node(0,0,1),router.graph_.node(1,0,1));
            require(shortest->edges.count(e),"actual horizontal boundary reserves the expected edge");
            require(shortest->edges.size()==1,"single straight crossing consumes exactly one resource");
            const int capacity=int(router.graph_.edge_capacity(edge_nodes(e).first,edge_nodes(e).second));
            for(int i=capacity;i>=0;--i) router.account(i,*shortest,1);
            require(router.usage_.at(e)==capacity+1,"distinct local nets account physical overcapacity");
            require(router.edge_nets_.at(e).size()==size_t(capacity+1),"reverse ownership survives local routes");
            require(std::is_sorted(router.edge_nets_.at(e).begin(),router.edge_nets_.at(e).end()),
                    "reverse owners remain sorted after reverse-order insertion");
            const auto before_demand=router.demand_;
            bool duplicate_rejected=false;
            try {router.account(0,*shortest,1);} catch(const std::logic_error&) {duplicate_rejected=true;}
            require(duplicate_rejected&&router.demand_==before_demand&&router.usage_.at(e)==capacity+1,
                    "duplicate owner account rejects before changing either ledger");
            const int middle=capacity/2;
            router.account(middle,*shortest,-1);
            require(!std::binary_search(router.edge_nets_.at(e).begin(),router.edge_nets_.at(e).end(),middle),
                    "middle owner ripup removes only that owner");
            router.account(middle,*shortest,1);
            for(int i=capacity;i>=0;--i) router.account(i,*shortest,-1);
            require(router.usage_.at(e)==0&&!router.edge_nets_.contains(e),"ripup restores physical count and releases owner bucket");
            require(router.cell_nets_.empty(),"ripup releases empty reverse Guide ownership sets");
            require(std::all_of(router.demand_.begin(),router.demand_.end(),[](int x){return x==0;}),
                    "ripup restores exact Guide ledger");
            bool missing_rejected=false;
            try {router.account(0,*shortest,-1);} catch(const std::logic_error&) {missing_rejected=true;}
            require(missing_rejected&&router.usage_.at(e)==0&&router.cell_nets_.empty(),
                    "missing owner ripup rejects before changing ledgers");
        }
        router.routes_={*shortest};NetRoute repaired;
        require(!router.repair_forest(0,-1,repaired),"grid forest repair skips explicit terminal paths");
    }
};
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("source and fixture output directories required");
        fs::path folder=argv[2];fs::create_directories(folder);
        struct Case {std::string name;Point a,b;int la,lb;bool blocked,crossing;};
        for(const auto& c:std::vector<Case>{
            {"same_cell",{60,60},{65,70},1,1,false,false},
            {"horizontal_crossing",{60,60},{140,60},1,1,false,true},
            {"upper_die",{60,60},{65,70},20,20,false,false},
            {"via_stack",{60,60},{65,70},1,3,false,false},
            {"obstacle",{60,150},{180,150},1,1,true,true},
            {"flat_terminal_escape",{135,150},{180,150},1,1,true,false}}) {
            LefParser lef;DefParser def;ThermalParser thermal;
            auto tech=technology(c.blocked);
            if(c.name=="flat_terminal_escape") {
                size_t position=0;
                while((position=tech.find("WIDTH 0.2",position))!=std::string::npos) {
                    tech.replace(position,9,"WIDTH 1.6");position+=9;
                }
            }
            require(lef.parse(write(folder/(c.name+".lef"),tech)),"LEF fixture parse");
            require(def.parse(write(folder/(c.name+".def"),design(c.a,c.b,c.la,c.lb,c.blocked)),lef),"DEF fixture parse");
            NegotiatedRouter router(def,lef,thermal);
            PinLocalTestAccess::check(router,def,lef,c.a,c.b,c.la,c.lb,c.blocked,c.crossing);
        }
        std::cout<<"PASS: "<<checks<<" local geometry, OBS fallback, resource ledger and rollback checks\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
