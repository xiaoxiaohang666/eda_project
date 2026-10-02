#include "objectives/guide_metrics.hpp"
#include "router/tsv_policy.hpp"
#include <filesystem>
#include <numeric>
#include <set>
#include <sstream>

using namespace eda;
namespace fs = std::filesystem;
namespace {
int checks=0;
void require(bool ok,const std::string& why) {
    ++checks;
    if(!ok) throw std::runtime_error(why);
}
template<class F> void rejects(F operation,const std::string& why) {
    bool failed=false;
    try { operation(); } catch(const std::exception&) { failed=true; }
    require(failed,why);
}
double recompute(const std::vector<double>& heat,const std::vector<int>& counts) {
    const double hs=std::accumulate(heat.begin(),heat.end(),0.0);
    const double ts=std::accumulate(counts.begin(),counts.end(),0.0);
    if(hs<=0||ts<=0) return 0;
    const double mean=1.0/heat.size();
    double numerator=0,hh=0,tt=0;
    for(size_t i=0;i<heat.size();++i) {
        const double h=heat[i]/hs-mean,t=counts[i]/ts-mean;
        numerator+=h*t;hh+=h*h;tt+=t*t;
    }
    const double denominator=std::sqrt(hh)*std::sqrt(tt);
    return denominator<=1e-15?0:numerator/denominator;
}
void test_pearson() {
    const std::vector<double> heat{0,3,7,1,9,10,4,2,6,8,5,12,2,4,11,14};
    std::vector<int> counts{2,0,1,0,0,2,0,0,1,0,0,0,0,0,1,0};
    PearsonState state(heat,counts);
    require(std::abs(state.correlation()-recompute(heat,counts))<1e-12,"initial Pearson parity");
    for(int step=0;step<100;++step) {
        int from=(step*7)%static_cast<int>(counts.size());
        while(counts[from]==0) from=(from+1)%static_cast<int>(counts.size());
        const int to=(step*11+3)%static_cast<int>(counts.size());
        state.move(from,to);
        if(from!=to) {--counts[from];++counts[to];}
        require(state.counts()==counts,"incremental count transaction");
        require(std::abs(state.correlation()-recompute(heat,counts))<1e-12,"incremental Pearson parity");
    }
    PearsonState flat({2,2,2},{1,0,1});
    require(flat.correlation()==0,"constant heat correlation");
    PearsonState uniform({1,2,3},{1,1,1});
    require(uniform.correlation()==0,"constant TSV correlation");
    rejects([&]{PearsonState bad({1},{1,2});},"Pearson dimension mismatch");
    rejects([&]{PearsonState bad({-1,2},{1,0});},"Pearson negative heat");
    rejects([&]{PearsonState bad({1,2},{1,-1});},"Pearson negative TSV count");
    rejects([&]{state.move(-1,0);},"Pearson invalid index");
    int empty=0;while(counts[empty]>0) ++empty;
    rejects([&]{state.move(empty,0);},"Pearson move from empty cell");
}
Pin pin(const std::string& name,Point point,int layer,const GCellGrid& grid) {
    Pin result;
    result.name=name;result.coord=point;result.layer_idx=layer;result.layer=get_layer_name(layer);
    result.gcell_col=grid.get_col(point.x);result.gcell_row=grid.get_row(point.y);
    return result;
}
GuideRect pad(const Rect& rect,int layer) {
    return {rect.x1,rect.y1,rect.x2,rect.y2,get_layer_name(layer)};
}
void test_sample_policy(const fs::path& root) {
    const auto base=root/"benchmarks/sample001";
    LefParser lef;
    DefParser def;
    ThermalParser thermal;
    require(lef.parse((base/"sample001.input.lef").string()),"sample LEF");
    require(def.parse((base/"sample001.input.def").string(),lef),"sample DEF");
    require(thermal.parse_json((base/"sample001.thermal.input.json").string(),
        def.gcell_grid(),def.dbu_per_micron(),def.die_area()),"sample heat");
    RoutingGraph graph(def,lef);
    TsvPolicy policy(def,lef,graph);
    require(policy.slots().size()==6,"sample geometry yields six paired slots");
    std::set<std::string> bottom,top;
    bool different_suffix=false;
    for(const auto& s:policy.slots()) {
        require(bottom.insert(s.bottom_name).second&&top.insert(s.top_name).second,"one-to-one geometric pairing");
        require(s.region.contains(s.point),"actual port center lies inside reservation");
        require(s.lower_node==graph.node(graph.grid().get_col(s.point.x),graph.grid().get_row(s.point.y),10),"lower access GCell");
        require(s.upper_node==graph.node(graph.grid().get_col(s.point.x),graph.grid().get_row(s.point.y),11),"upper access GCell");
        different_suffix=different_suffix || s.bottom_name.substr(0,s.bottom_name.rfind('_') )!=s.top_name.substr(0,s.top_name.rfind('_') );
    }
    require(different_suffix,"pairing is geometric rather than slot numbering");
    const auto assigned=policy.assign(def.nets(),thermal);
    std::set<int> used;
    int crossed=0,rejected=0;
    for(size_t i=0;i<def.nets().size();++i) {
        const auto& net=def.nets()[i];
        if(!net.is_cross_die) {require(assigned[i]==-1,"no intra-die TSV assignment");continue;}
        ++crossed;
        require(used.insert(assigned[i]).second,"one net per selected slot");
        const auto& s=policy.slots().at(assigned[i]);
        require(policy.compatible(net,s.id),"selected slot passes fixed-pin policy");
        std::vector<GuideRect> witness{pad(s.envelope,10),pad(s.envelope,11)};
        std::string error;
        require(policy.validate_mapping(net,witness,s.id,error),"VIA-envelope witness accepted: "+error);
        const auto one=measure_net_guides(witness,graph,lef);
        require(one.tsv_points.size()==1,"one inferred TSV witness");
        require(one.tsv_points[0].distance_to(s.envelope.center())<1e-9,"TSV inference uses overlap center");
        auto duplicated=witness;duplicated.push_back(witness[0]);
        const auto deduped=measure_net_guides(duplicated,graph,lef);
        require(deduped.demand==one.demand&&deduped.wirelength==one.wirelength,"exact rectangle deduplication matches scorer parser");
        for(const auto& alternative:policy.slots()) if(alternative.id!=s.id) {
            require(!policy.compatible(net,alternative.id),"fixed vlink move must be rejected");
            require(!policy.validate_mapping(net,witness,alternative.id,error),"incompatible selected slot cannot pass mapping");
            auto moved=witness;
            moved.push_back(pad(alternative.envelope,10));
            moved.push_back(pad(alternative.envelope,11));
            require(!policy.validate_mapping(net,moved,s.id,error),"extra planned/unplanned positive overlap rejected");
            ++rejected;
        }
        auto edge_touch=witness;
        const double x=s.region.x2+1000,y=s.region.y2+1000;
        edge_touch.emplace_back(x,y,x+100,y+100,"metal10");
        edge_touch.emplace_back(x+100,y,x+200,y+100,"metal11");
        require(!policy.validate_mapping(net,edge_touch,s.id,error),"extra edge-touch electrical TSV cannot evade policy");
    }
    require(crossed==3&&used.size()==3&&rejected==15,"sample cross-net/slot/rejected-move counts");

    // A flexible net strongly prefers a slot reserved by an expensive forced
    // net. An augmenting path must release the first assignment rather than
    // failing because a sequential greedy choice consumed the unique slot.
    const auto& slot=policy.slots().front();
    Net flexible;flexible.name="A_flexible";flexible.is_cross_die=true;
    flexible.pins={pin("lo",slot.point,9,graph.grid()),pin("hi",slot.point,12,graph.grid())};
    Net forced;forced.name="B_forced";forced.is_cross_die=true;
    forced.pins={pin("fixed_lo",slot.point,10,graph.grid()),pin("fixed_hi",slot.point,11,graph.grid()),
                 pin("far",policy.slots().back().point,1,graph.grid())};
    const auto competition=policy.assign({flexible,forced},thermal);
    require(competition.size()==2&&competition[1]==slot.id&&competition[0]!=slot.id,
            "global min-cost flow preserves a forced slot and reroutes the flexible net");
    auto forced2=forced;forced2.name="C_forced";
    rejects([&]{policy.assign({forced,forced2},thermal);},"slot capacity conflict fails instead of overallocating");

    // Per-rectangle demand is intentionally not the same as per-net cell union.
    const auto cell=graph.grid().get_cell_rect(3,3);
    std::vector<GuideRect> overlap{pad(cell,1),
        {cell.x1+1,cell.y1+1,cell.x2-1,cell.y2-1,"metal1"}};
    const auto metric=measure_net_guides(overlap,graph,lef);
    require(metric.demand.size()==1&&metric.demand.front().second==2,"overlapping different rectangles contribute twice");
    require(metric.tsv_points.empty(),"ordinary same-die guide has no TSV");
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("expected source directory");
        test_pearson();test_sample_policy(argv[1]);
        std::cout<<"PASS: "<<checks<<" metrics/policy checks\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL after "<<checks<<" checks: "<<error.what()<<"\n";
        return 1;
    }
}