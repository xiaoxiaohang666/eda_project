#include "router/negotiated_router.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace eda;
namespace fs=std::filesystem;
namespace {
void require(bool condition,const std::string& reason) {
    if(!condition) throw std::runtime_error(reason);
}
void near(double a,double b,const std::string& reason) {
    require(std::abs(a-b)<=1e-8,reason);
}
std::string write(const fs::path& file,const std::string& text) {
    std::ofstream out(file);out<<text;out.close();
    require(bool(out),"could not write fast-quality fixture");return file.string();
}
void check_path(const PathResult& path,const RoutingGraph& graph,
                const std::vector<PinAccess>& sources,const std::set<int>& targets) {
    require(path.status==PathResult::Status::FOUND && !path.nodes.empty(),"quality search did not find a path");
    require(path.source<sources.size() && path.nodes.front()==sources[path.source].node,"quality path lost its source");
    require(targets.count(path.nodes.back()),"quality path lost its target tree");
    for(size_t i=1;i<path.nodes.size();++i)
        require(graph.legal_edge(path.nodes[i-1],path.nodes[i]),"quality path contains an illegal edge");
}
}
namespace eda {
struct PinLocalTestAccess {
    static void begin_refinement(NegotiatedRouter& router) { router.initial_build_=false; }
    static void set_demand(NegotiatedRouter& router,const RoutingGraph& graph,int col,int row,int layer,int amount) {
        router.demand_.at(size_t(graph.node(col,row,layer)))=amount;
    }
};
}
int main(int argc,char** argv) {
    try {
        require(argc==2 || argc==3,"fixture directory and optional official runtime required");
        const fs::path folder=argv[1];fs::create_directories(folder);
        const auto runtime=argc==3?fs::path(argv[2]):fs::path(__FILE__).parent_path().parent_path()/"output/runtime/official-flute";
        for(const auto* asset:{"libflute.so","tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so","POWV9.dat","POST9.dat"}) {
            if(!fs::is_regular_file(runtime/asset)) {
                std::cout<<"SKIP fast-quality official integration: native runtime unavailable at "<<runtime<<'\n';return 0;
            }
        }
        std::string technology;
        for(int layer=1;layer<=20;++layer) {
            const auto name=get_layer_name(layer);
            technology+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(layer%2?"HORIZONTAL":"VERTICAL")+
                " ; WIDTH 0.1 ; END "+name+"\n";
        }
        std::string design="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 1600 1200 ) ; "
            "GCELLGRID X 0 DO 17 STEP 100 ; GCELLGRID Y 0 DO 13 STEP 100 ; ";
        for(int layer=1;layer<=20;++layer) {
            const auto name=get_layer_name(layer);
            design+="TRACKS X 5 DO 160 STEP 10 LAYER "+name+" ; TRACKS Y 5 DO 120 STEP 10 LAYER "+name+" ; ";
        }
        design+="COMPONENTS 0 ; END COMPONENTS PINS 2 ; "
            "- a + LAYER metal3 ( -1 -1 ) ( 1 1 ) + FIXED ( 250 250 ) N ; "
            "- b + LAYER metal3 ( -1 -1 ) ( 1 1 ) + FIXED ( 850 850 ) N ; END PINS NETS 1001 ; ";
        for(int i=0;i<1001;++i) design+="- n"+std::to_string(i)+" ( PIN a ) ( PIN b ) ; ";
        design+="END NETS END DESIGN";
        LefParser lef;DefParser def;ThermalParser thermal;
        require(lef.parse(write(folder/"quality.lef",technology)) &&
                def.parse(write(folder/"quality.def",design),lef,DefGridMapping::OFFICIAL_FULL),"quality fixture parse failed");
        RouterOptions improved;improved.official_model=true;improved.official_runtime=runtime.string();
        improved.graph_options.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
        improved.official_metrics.gcell_width_dbu=def.gcell_pitch_x();
        improved.official_metrics.gcell_height_dbu=def.gcell_pitch_y();
        improved.fast_initial=true;improved.fast_quality=true;improved.search_limit=50000;
        RouterOptions legacy=improved;legacy.fast_quality=false;
        RoutingGraph graph(def,lef,improved.graph_options);
        PinAccess source;source.node=graph.node(2,2,3);

        // A terminal already on the tree needs no geometry or ordinary via.
        RouterOptions preferred=improved;preferred.graph_options.allow_nonpreferred=false;
        RouterOptions preferred_legacy=preferred;preferred_legacy.fast_quality=false;
        NegotiatedRouter coincident(def,lef,thermal,preferred),coincident_old(def,lef,thermal,preferred_legacy);
        PinLocalTestAccess::begin_refinement(coincident);
        PinLocalTestAccess::begin_refinement(coincident_old);
        NetRoute existing;existing.nodes.insert(source.node);
        const auto direct=coincident.search({source},{source.node},existing,1,10,false);
        check_path(direct,graph,{source},{source.node});
        require(direct.nodes==std::vector<int>{source.node},"coincident tree access emitted a useless tail or via cycle");
        near(direct.cost,0,"coincident tree access has nonzero cost");
        require(coincident.counters().expansions==0,"coincident fast access consumed exact-search expansions");
        const auto old_direct=coincident_old.search({source},{source.node},existing,1,10,false);
        require(old_direct.status==PathResult::Status::FOUND && old_direct.cost>direct.cost,
                "coincident regression did not expose the legacy preferred-only via cycle");

        // A zero-length orthogonal leg must not force a layer change either.
        const int straight_target=graph.node(8,2,3);NetRoute straight_tree;straight_tree.nodes.insert(straight_target);
        const auto straight=coincident.search({source},{straight_target},straight_tree,1,10,false);
        const auto old_straight=coincident_old.search({source},{straight_target},straight_tree,1,10,false);
        const auto straight_oracle=coincident.search({source},{straight_target},straight_tree,1,10,true);
        for(const auto* path:{&straight,&old_straight,&straight_oracle})check_path(*path,graph,{source},{straight_target});
        require(straight.nodes.size()==7 && std::all_of(straight.nodes.begin(),straight.nodes.end(),[&](int node){return graph.layer(node)==3;}),
                "zero-length orthogonal leg introduced an ordinary via");
        require(straight.cost<old_straight.cost,"zero-length leg simplification saved no cost");
        near(straight.cost,straight_oracle.cost,"straight preferred-only template disagrees with Dijkstra");

        // Both L corridors are legal but expensive. The intermediate-column
        // or intermediate-row Z avoids them without extra wire or vias.
        NegotiatedRouter z_router(def,lef,thermal,improved),l_router(def,lef,thermal,legacy);
        for(auto* router:{&z_router,&l_router}) {
            PinLocalTestAccess::begin_refinement(*router);
            PinLocalTestAccess::set_demand(*router,graph,7,2,3,10000);
            PinLocalTestAccess::set_demand(*router,graph,2,7,3,10000);
        }
        const int diagonal=graph.node(8,8,3);NetRoute diagonal_tree;diagonal_tree.nodes.insert(diagonal);
        const auto z=z_router.search({source},{diagonal},diagonal_tree,3,3,false);
        const auto l=l_router.search({source},{diagonal},diagonal_tree,3,3,false);
        const auto z_oracle=z_router.search({source},{diagonal},diagonal_tree,3,3,true);
        for(const auto* path:{&z,&l,&z_oracle})check_path(*path,graph,{source},{diagonal});
        require(z.cost+1<l.cost,"legal but congested L corridors suppressed a cheaper same-layer Z");
        near(z.cost,z_oracle.cost,"same-layer Z disagrees with the independent Dijkstra optimum");
        require(z.nodes.size()==13 && std::all_of(z.nodes.begin(),z.nodes.end(),[&](int node){return graph.layer(node)==3;}),
                "same-layer congestion detour added wire or ordinary vias");
        require(z_router.counters().fast_initial_paths==1 && l_router.counters().fast_initial_paths==1 &&
                l_router.counters().expansions==0,"Z comparison did not use the two fast template modes");

        // Geometry-only target ranking shortlists the two low-layer nodes.
        // Reaching them cheaply requires M9, whose corridor already crosses
        // a third tree node. Stop at that first intersection instead of
        // descending six layers and continuing to a shortlisted endpoint.
        NegotiatedRouter trimmed(def,lef,thermal,improved),untrimmed(def,lef,thermal,legacy);
        for(auto* router:{&trimmed,&untrimmed}) {
            PinLocalTestAccess::begin_refinement(*router);
            for(int layer=3;layer<=8;++layer)for(int row=0;row<graph.grid().rows;++row)for(int col=3;col<=7;++col)
                PinLocalTestAccess::set_demand(*router,graph,col,row,layer,10000);
        }
        const int first_crossing=graph.node(5,2,9);
        const std::set<int> tree{graph.node(8,2,3),graph.node(8,3,3),first_crossing};
        NetRoute own;own.nodes=tree;
        const auto short_path=trimmed.search({source},tree,own,3,9,false);
        const auto old_path=untrimmed.search({source},tree,own,3,9,false);
        const auto oracle=trimmed.search({source},tree,own,3,9,true);
        const auto old_oracle=untrimmed.search({source},tree,own,3,9,true);
        for(const auto* path:{&short_path,&old_path,&oracle,&old_oracle})check_path(*path,graph,{source},tree);
        require(short_path.nodes.back()==first_crossing,"template continued beyond its first target-tree intersection");
        for(size_t i=0;i+1<short_path.nodes.size();++i)
            require(!tree.count(short_path.nodes[i]),"trimmed template retains a tail after touching its target tree");
        require(short_path.cost+1<old_path.cost,"first-tree-intersection truncation saved no cost");
        near(short_path.cost,oracle.cost,"trimmed corridor disagrees with Dijkstra optimum");
        near(oracle.cost,old_oracle.cost,"fast-quality option changed explicit Dijkstra behavior");
        require(trimmed.counters().fast_initial_paths==1 && untrimmed.counters().fast_initial_paths==1 &&
                trimmed.counters().expansions>0 && untrimmed.counters().expansions>0,
                "explicit Dijkstra did not preserve its exact-search path");
        std::cout<<"PASS fast quality: coincident tree access, zero-length leg, legal-L congestion Z, first-tree intersection, legacy toggle, Dijkstra oracle\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
