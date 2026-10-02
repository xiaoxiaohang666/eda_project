#include "router/negotiated_router.hpp"
#include <filesystem>
#include <iostream>
using namespace eda;
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("source directory required");
        const auto base=std::filesystem::path(argv[1])/"benchmarks/sample001";
        LefParser lef;DefParser def;ThermalParser thermal;
        if(!lef.parse((base/"sample001.input.lef").string())||!def.parse((base/"sample001.input.def").string(),lef)||
           !thermal.parse_json((base/"sample001.thermal.input.json").string(),def.gcell_grid(),def.dbu_per_micron(),def.die_area()))
            throw std::runtime_error("sample parse failed");
        RoutingGraph graph(def,lef);NegotiatedRouter router(def,lef,thermal);
        int tested=0;
        for(int layer:{3,7,13,17}) {
            const int a=graph.node(2,2,layer),b=graph.node(10,11,layer+1),c=graph.node(3,2,layer);
            if(!graph.legal_node(a)||!graph.legal_node(b)||!graph.legal_node(c)) continue;
            PinAccess first;first.node=a;PinAccess alternative;alternative.node=c;
            const int lo=layer<=10?1:11,hi=layer<=10?10:20;
            for(bool multi:{false,true}) {
                std::vector<PinAccess> sources{first};if(multi) sources.push_back(alternative);
                const auto astar=router.search(sources,{b},NetRoute{},lo,hi,false);
                const auto dijkstra=router.search(sources,{b},NetRoute{},lo,hi,true);
                if(astar.status!=PathResult::Status::FOUND||dijkstra.status!=PathResult::Status::FOUND||
                   std::abs(astar.cost-dijkstra.cost)>1e-8)
                    throw std::runtime_error("A*/Dijkstra snapshot cost mismatch");
                for(size_t i=1;i<astar.nodes.size();++i) if(!graph.legal_edge(astar.nodes[i-1],astar.nodes[i]))
                    throw std::runtime_error("invalid search edge");
                if(astar.nodes.back()!=b || astar.nodes.front()!=sources.at(astar.source).node)
                    throw std::runtime_error("lost source or target");
                ++tested;
            }
        }
        if(tested<4) throw std::runtime_error("insufficient reachable oracle cases");
        RouterOptions limited;limited.search_limit=1;
        NegotiatedRouter bounded(def,lef,thermal,limited);
        PinAccess start;start.node=graph.node(2,2,7);
        if(bounded.search({start},{graph.node(10,11,8)},NetRoute{},1,10,false).status!=PathResult::Status::SEARCH_LIMIT)
            throw std::runtime_error("budget exhaustion mislabeled unreachable");
        std::cout<<"PASS: "<<tested<<" A*/Dijkstra cost oracles, legal paths, bounded-search status\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
