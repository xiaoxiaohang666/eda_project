#include "router/negotiated_router.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

using namespace eda;
namespace fs=std::filesystem;
namespace {
size_t checks=0;
void require(bool value,const std::string& reason) {
    ++checks;if(!value)throw std::runtime_error(reason);
}
std::string write(const fs::path& path,const std::string& text) {
    std::ofstream out(path);out<<text;out.close();require(bool(out),"could not write layer-pruning fixture");return path.string();
}
std::string technology() {
    std::string text;
    for(int l=1;l<=20;++l) {
        const auto name=get_layer_name(l);
        text+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(l%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.1 ; END "+name+"\n";
    }
    return text;
}
std::string design() {
    std::string text="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 1600 1200 ) ; "
        "GCELLGRID X 0 DO 17 STEP 100 ; GCELLGRID Y 0 DO 13 STEP 100 ; ";
    for(int l=1;l<=20;++l) {
        const auto name=get_layer_name(l);
        text+="TRACKS X 5 DO 160 STEP 10 LAYER "+name+" ; TRACKS Y 5 DO 120 STEP 10 LAYER "+name+" ; ";
    }
    text+="COMPONENTS 0 ; END COMPONENTS PINS 2 ; "
        "- a + LAYER metal3 ( -1 -1 ) ( 1 1 ) + FIXED ( 250 250 ) N ; "
        "- b + LAYER metal4 ( -1 -1 ) ( 1 1 ) + FIXED ( 850 850 ) N ; END PINS NETS 1001 ; ";
    for(int i=0;i<1001;++i)text+="- n"+std::to_string(i)+" ( PIN a ) ( PIN b ) ; ";
    return text+"END NETS END DESIGN";
}
void equal_path(const PathResult& a,const PathResult& b,const std::string& context) {
    require(a.status==b.status && a.source==b.source && a.cost==b.cost && a.nodes==b.nodes,
        context+": layer pruning changed path status, source, exact cost or node sequence");
}
}
namespace eda {
struct PinLocalTestAccess {
    static void state(NegotiatedRouter& router,const std::vector<int>& demand,const std::vector<double>& history,
                      double present,double history_scale,double via) {
        router.initial_build_=false;router.demand_=demand;router.guide_history_=history;
        router.present_=present;router.history_scale_=history_scale;router.options_.official_metrics.via_cost_ratio=via;
    }
    static double cost(const NegotiatedRouter& router,const PathResult& path,const std::vector<PinAccess>& sources,const NetRoute& own) {
        double result=router.access_cost(sources.at(path.source));
        for(size_t i=1;i<path.nodes.size();++i)result+=router.edge_cost(path.nodes[i-1],path.nodes[i],own);
        return result;
    }
};
}
int main(int argc,char** argv) {
    try {
        require(argc==2 || argc==3,"fixture directory and optional official runtime required");
        const fs::path folder=argv[1];fs::create_directories(folder);
        const auto runtime=argc==3?fs::path(argv[2]):fs::path(__FILE__).parent_path().parent_path()/"output/runtime/official-flute";
        for(const auto* asset:{"libflute.so","tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so","POWV9.dat","POST9.dat"})
            if(!fs::is_regular_file(runtime/asset)) {
                std::cout<<"SKIP layer-pruning official integration: native runtime unavailable at "<<runtime<<'\n';return 0;
            }
        LefParser lef;DefParser def;ThermalParser thermal;
        require(lef.parse(write(folder/"pruning.lef",technology())) &&
            def.parse(write(folder/"pruning.def",design()),lef,DefGridMapping::OFFICIAL_FULL),"layer-pruning fixture parses");
        RouterOptions enabled;enabled.official_model=true;enabled.official_runtime=runtime.string();
        enabled.graph_options.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
        enabled.official_metrics.gcell_width_dbu=def.gcell_pitch_x();enabled.official_metrics.gcell_height_dbu=def.gcell_pitch_y();
        enabled.fast_quality=true;enabled.fast_initial=true;enabled.fast_layer_pruning=true;
        enabled.time_limit=600;enabled.search_limit=200000;
        RouterOptions disabled=enabled;disabled.fast_layer_pruning=false;
        RoutingGraph graph(def,lef,enabled.graph_options);
        NegotiatedRouter pruned(def,lef,thermal,enabled),full(def,lef,thermal,disabled);
        std::vector<int> demand(graph.size(),0);std::vector<double> history(graph.size(),0);
        auto set_state=[&](double present=1,double scale=1,double via=3) {
            PinLocalTestAccess::state(pruned,demand,history,present,scale,via);
            PinLocalTestAccess::state(full,demand,history,present,scale,via);
        };
        size_t oracle_queries=0;
        auto compare=[&](const std::vector<PinAccess>& sources,const std::set<int>& targets,const NetRoute& own,
                         int low,int high,bool oracle,const std::string& label) {
            const auto before_prunes=pruned.counters().fast_layer_prunes;
            const auto a=pruned.search(sources,targets,own,low,high,false);
            const auto b=full.search(sources,targets,own,low,high,false);
            equal_path(a,b,label);
            require(a.status==PathResult::Status::FOUND && !a.nodes.empty(),label+": legal global template was lost");
            require(a.nodes.front()==sources.at(a.source).node && targets.count(a.nodes.back()),label+": endpoint lost");
            for(size_t i=1;i<a.nodes.size();++i)require(graph.legal_edge(a.nodes[i-1],a.nodes[i]),label+": illegal edge");
            require(PinLocalTestAccess::cost(pruned,a,sources,own)==a.cost,label+": stored template cost is not its full snapshot cost");
            if(oracle) {
                const auto after_fast_prunes=pruned.counters().fast_layer_prunes;
                const auto da=pruned.search(sources,targets,own,low,high,true);
                const auto db=full.search(sources,targets,own,low,high,true);
                equal_path(da,db,label+" explicit Dijkstra");++oracle_queries;
                require(da.status==PathResult::Status::FOUND && da.cost<=a.cost+1e-8,label+": Dijkstra is not a lower-cost oracle");
                require(pruned.counters().fast_layer_prunes==after_fast_prunes,label+": explicit Dijkstra used fast layer pruning");
            }
            require(pruned.counters().fast_layer_prunes>=before_prunes,label+": pruning counter decreased");
            return a;
        };

        // Equal-cost layer minima and both L orientations must keep their
        // deterministic tie order; more distant layers can still be pruned.
        set_state();PinAccess source;source.node=graph.node(2,2,3);
        const auto tie=compare({source},{graph.node(8,8,4)},NetRoute{},1,10,true,"open grid with layer ties");
        require(tie.cost==15,"open-grid fixture has known 12 planar steps plus one ordinary via");
        require(pruned.counters().fast_layer_prunes>0,"controlled layer-dominance fixture performed no actual pruning");

        // Existing graph branches have exactly zero edge cost. They invalidate
        // a strictly-positive-edge assumption but preserve nonnegative pruning.
        NetRoute own;
        for(int c=2;c<6;++c) {
            const int a=graph.node(c,2,3),b=graph.node(c+1,2,3);
            own.edges.insert(route_edge(a,b));own.nodes.insert(a);own.nodes.insert(b);
        }
        const std::set<int> tree{graph.node(8,2,3),graph.node(10,2,3)};
        own.nodes.insert(tree.begin(),tree.end());
        const auto free_branch=compare({source},tree,own,1,10,true,"owned zero-cost branch");
        require(free_branch.cost==2,"owned branch fixture retains four free edges and two paid edges");

        std::mt19937 random(0x4c4c4834U);
        size_t multiple_sources=0,multiple_targets=0,owned_queries=0;
        for(int sample=0;sample<96;++sample) {
            for(size_t n=0;n<graph.size();++n) {
                demand[n]=sample%4==0?0:int(random()%45);
                history[n]=sample%3==0?0:double(random()%17)/4.0;
            }
            const double present_values[]{0,0.25,1,4},via_values[]{0,0.25,3,9};
            set_state(present_values[sample%4],sample%5==0?0:1,via_values[(sample/4)%4]);
            const int low=sample%2?11:1,high=low+9;
            auto random_node=[&]() {return graph.node(1+int(random()%14),1+int(random()%10),low+int(random()%10));};
            std::vector<PinAccess> sources(1+sample%3);
            for(auto& access:sources) {
                access.node=random_node();access.length=graph.ell0()*double(random()%3);access.vias=int(random()%3);
                if(sample%4==1) {
                    const auto box=graph.cell(access.node);
                    access.guides={{box.x1+10,box.y1+10,box.x2-10,box.y2-10,get_layer_name(graph.layer(access.node))}};
                }
            }
            std::set<int> targets;for(int j=0;j<1+sample%5;++j)targets.insert(random_node());
            NetRoute existing;
            if(sample%2==0) {
                const int col=1+int(random()%9),row=1+int(random()%10),layer=low+int(random()%10);
                for(int c=col;c<col+4;++c) {
                    const int a=graph.node(c,row,layer),b=graph.node(c+1,row,layer);
                    existing.edges.insert(route_edge(a,b));existing.nodes.insert(a);existing.nodes.insert(b);
                }
                sources.front().node=graph.node(col,row,layer);targets.insert(graph.node(col+6,row,layer));++owned_queries;
            }
            existing.nodes.insert(targets.begin(),targets.end());
            multiple_sources+=sources.size()>1;multiple_targets+=targets.size()>1;
            compare(sources,targets,existing,low,high,sample%8==0,"deterministic random snapshot "+std::to_string(sample));
        }
        require(multiple_sources>0 && multiple_targets>0 && owned_queries>0 && oracle_queries>10,
            "random regression exercises source/target shortlists, existing free edges and independent exact queries");
        require(full.counters().fast_layer_prunes==0 && pruned.counters().fast_layer_prunes>0,
            "only the enabled equivalent-pruning mode records layer prunes");
        require(pruned.counters().fast_z_candidates==full.counters().fast_z_candidates &&
            pruned.counters().fast_z_paths==full.counters().fast_z_paths,
            "safe L pruning changed the top-two layer Z budget or its selected path");
        std::cout<<"PASS equivalent fast layer pruning: 98 matching template paths, "<<oracle_queries
            <<" matching Dijkstra paths, prunes="<<pruned.counters().fast_layer_prunes<<", checks="<<checks<<'\n';
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
