#include "router/tsv_policy.hpp"
#include "objectives/guide_metrics.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>

using namespace eda;
namespace fs=std::filesystem;
namespace {
int checks=0;
void require(bool condition,const std::string& message) {
    ++checks;if(!condition) throw std::runtime_error(message);
}
void close(double a,double b,const std::string& message) {
    require(std::abs(a-b)<=1e-12*std::max({1.0,std::abs(a),std::abs(b)}),message);
}
void rejects(const std::function<void()>& f,const std::string& message) {
    bool failed=false;try {f();} catch(const std::exception&) {failed=true;}
    require(failed,message);
}
std::string write(const fs::path& dir,const std::string& name,const std::string& text) {
    const auto p=dir/name;std::ofstream out(p);out<<text;out.close();
    if(!out) throw std::runtime_error("cannot write TSV move test fixture");
    return p.string();
}
std::string technology() {
    std::ostringstream out;
    for(int layer=1;layer<=20;++layer) {
        const auto name=get_layer_name(layer);
        out<<"LAYER "<<name<<" TYPE ROUTING ; DIRECTION "<<(layer%2?"HORIZONTAL":"VERTICAL")
           <<" ; WIDTH 0.2 ; END "<<name<<'\n';
    }
    return out.str();
}
std::string design(bool fixed=false,size_t extra_pins=0) {
    std::ostringstream out;
    out<<"UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 600 400 ) ; "
       <<"GCELLGRID X 0 DO 7 STEP 100 ; GCELLGRID Y 0 DO 5 STEP 100 ; ";
    for(int layer=1;layer<=20;++layer)
        out<<"TRACKS X 5 DO 60 STEP 10 LAYER "<<get_layer_name(layer)<<" ; "
           <<"TRACKS Y 5 DO 40 STEP 10 LAYER "<<get_layer_name(layer)<<" ; ";
    out<<"COMPONENTS 0 ; END COMPONENTS PINS "<<6+extra_pins<<" ; ";
    for(int n=0;n<3;++n) for(int side=0;side<2;++side) {
        const int layer=n==2?3:((fixed&&n==0)?10+side:(side?14:3));
        out<<"- p"<<n<<(side?'t':'b')<<" + LAYER "<<get_layer_name(layer)
           <<" ( -1 -1 ) ( 1 1 ) + FIXED ( "<<(n==2?250:50)<<' '<<50+100*n<<" ) N ; ";
    }
    for(size_t i=0;i<extra_pins;++i)
        out<<"- extra"<<i<<" + LAYER metal3 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 50 ) N ; ";
    out<<"END PINS NETS 3 ; ";
    for(int n=0;n<3;++n) {
        out<<"- n"<<n<<" ( PIN p"<<n<<"b ) ( PIN p"<<n<<"t ) ";
        if(n==0) for(size_t i=0;i<extra_pins;++i) out<<"( PIN extra"<<i<<" ) ";
        out<<"; ";
    }
    out<<"END NETS END DESIGN";return out.str();
}
std::string heat(bool uniform=false) {
    std::ostringstream out;out<<"{\"meta\":{\"ambientTemp\":25},\"nodes\":[";
    for(int row=0;row<4;++row) for(int col=0;col<6;++col) {
        if(row||col) out<<',';
        out<<'['<<5+10*col<<','<<5+10*row<<",0,"<<(uniform?30:(col>=3?45:25))<<']';
    }
    out<<"]}";return out.str();
}
int slot_at(const TsvPolicy& policy,const RoutingGraph& graph,int col,int row) {
    for(const auto& s:policy.slots()) if(graph.col(s.lower_node)==col&&graph.row(s.lower_node)==row) return s.id;
    throw std::runtime_error("missing test slot");
}
size_t net_index(const DefParser& def,const std::string& name) {
    for(size_t n=0;n<def.nets().size();++n) if(def.nets()[n].name==name) return n;
    throw std::runtime_error("missing test net");
}
double recompute(const RoutingGraph& graph,const TsvPolicy& policy,const ThermalParser& thermal,
                 const std::vector<int>& assignment) {
    const auto& grid=graph.grid();std::vector<double> h(size_t(grid.cols)*grid.rows);
    std::vector<int> counts(h.size());
    for(int row=0;row<grid.rows;++row) for(int col=0;col<grid.cols;++col)
        h[size_t(row)*grid.cols+col]=std::max(0.0,thermal.get_temperature(col,row)-thermal.ambient_temperature());
    for(int slot:assignment) if(slot>=0) {
        const auto p=policy.slots().at(size_t(slot)).point;
        ++counts[size_t(grid.get_row(p.y))*grid.cols+grid.get_col(p.x)];
    }
    return PearsonState(std::move(h),std::move(counts)).correlation();
}
void run(const fs::path& dir,const LefParser& lef,bool fixed=false) {
    DefParser def;require(def.parse(write(dir,fixed?"fixed.def":"move.def",design(fixed)),lef,DefGridMapping::OFFICIAL_FULL),"design parses");
    RoutingGraphOptions go;go.geometry=RoutingGeometryMode::GLOBAL_GUIDE;go.allow_nonpreferred=false;go.score_20261020=true;
    RoutingGraph graph(def,lef,go);TsvPolicyOptions po;po.resource_mode=TsvResourceMode::GLOBAL_GRID;
    TsvPolicy policy(def,lef,graph,po);ThermalParser thermal;
    require(thermal.parse_json(write(dir,"heat.json",heat()),graph.grid(),def.dbu_per_micron(),def.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),"heat parses");
    const size_t a=net_index(def,"n0"),b=net_index(def,"n1"),same=net_index(def,"n2");
    const int old_a=slot_at(policy,graph,0,0),old_b=slot_at(policy,graph,0,1);
    std::vector<int> assignment(def.nets().size(),-1);assignment[a]=old_a;assignment[b]=old_b;
    const auto initial=assignment;
    TsvMoveSearch search(def,graph,policy,thermal,assignment);
    close(search.current_correlation(),recompute(graph,policy,thermal,assignment),"initial Pearson matches full grid recomputation");
    if(fixed) {
        require(search.candidates(a).empty(),"fixed M10/M11 crossing has no movable proposal");
        const auto before=search.current_correlation();
        require(!search.commit_move(a,slot_at(policy,graph,1,0)),"fixed crossing commit rejected");
        close(search.current_correlation(),before,"fixed rejection keeps Pearson state");
        require(search.current_slot(a)==old_a,"fixed rejection keeps assignment");return;
    }
    require(search.current_slot(same)==-1&&search.candidates(same).empty(),"same die uses -1 and generates no TSV");
    require(!search.commit_move(same,slot_at(policy,graph,1,0)),"same die cannot acquire a TSV");
    TsvMoveSearchOptions tiny;tiny.max_candidates=1;tiny.thermal_gain=0;
    const auto one=search.candidates(a,tiny);
    require(one.size()==1&&one.front().slot!=old_a&&one.front().slot!=old_b,
            "one-candidate budget must propose a new free slot even when all anchors equal the current slot");
    tiny.thermal_gain=1000;
    require(std::tanh(tiny.thermal_gain*search.current_correlation())==-1,
            "saturated thermal retrieval regression starts with a zero local tanh derivative");
    const auto hot=search.candidates(a,tiny);
    require(hot.size()==1&&hot.front().thermal_score_delta>0&&
            graph.col(policy.slots()[size_t(hot.front().slot)].lower_node)>=3,
            "finite positive thermal moves remain discoverable when the local derivative saturates");
    TsvMoveSearchOptions options;options.max_candidates=24;options.thermal_gain=4;options.length_price=0;
    const auto candidates=search.candidates(a,options);
    require(candidates.size()==22,"all 22 free cells exposed within a sufficiently large candidate budget");
    std::set<int> unique;size_t warm=0;
    for(size_t k=0;k<candidates.size();++k) {
        const auto& c=candidates[k];require(unique.insert(c.slot).second,"proposal slots unique");
        require(c.slot!=old_a&&c.slot!=old_b&&policy.compatible(def.nets()[a],c.slot),"proposal respects occupancy and compatibility");
        auto moved=assignment;moved[a]=c.slot;
        const double expected=recompute(graph,policy,thermal,moved);
        close(c.correlation_after,expected,"predicted Pearson equals independent full recomputation");
        const double gain=12.5*(std::tanh(options.thermal_gain*expected)-std::tanh(options.thermal_gain*search.current_correlation()));
        close(c.thermal_score_delta,gain,"thermal increment uses weighted quality points exactly once");
        close(c.estimated_score_delta,gain,"zero length/risk price leaves exact thermal gain");
        warm+=graph.col(policy.slots()[size_t(c.slot)].lower_node)>=3;
        if(k) require(candidates[k-1].estimated_score_delta>=c.estimated_score_delta,"proposals sorted descending by estimate");
    }
    require(warm>=2,"multiple usable cells from the same hot thermal block remain candidates");
    const auto repeated=search.candidates(a,options);
    require(repeated.size()==candidates.size(),"deterministic shortlist size");
    for(size_t k=0;k<candidates.size();++k)
        require(repeated[k].slot==candidates[k].slot&&repeated[k].estimated_score_delta==candidates[k].estimated_score_delta,"deterministic shortlist order");
    require(assignment==initial,"query context never mutates external assignment");
    const auto best=candidates.front();require(best.thermal_score_delta>0,"hot proposal improves exact heat score");
    require(search.commit_move(a,best.slot),"commit accepts validated free destination");assignment[a]=best.slot;
    close(search.current_correlation(),best.correlation_after,"commit applies predicted Pearson increment");
    close(search.current_correlation(),recompute(graph,policy,thermal,assignment),"committed Pearson matches independent full recomputation");
    const auto before_reject=search.current_correlation();
    require(!search.commit_move(b,best.slot),"occupied destination rejected");
    require(!search.commit_move(a,best.slot),"no-op commit rejected");
    require(!search.commit_move(a,-1)&&!search.commit_move(def.nets().size(),old_a),"invalid target and net rejected");
    close(search.current_correlation(),before_reject,"rejected commits leave exact Pearson unchanged");
    require(search.current_slot(b)==old_b&&search.current_slot(a)==best.slot,"rejected commits preserve owners");
    const auto second=search.candidates(b,options);bool old_free=false;
    for(const auto& c:second) {require(c.slot!=best.slot,"next query never returns newly occupied slot");old_free|=c.slot==old_a;}
    require(old_free,"prior occupied slot restored to free candidate index");
    require(!second.empty()&&search.commit_move(b,second.front().slot),"second independent move commits");assignment[b]=second.front().slot;
    close(search.current_correlation(),recompute(graph,policy,thermal,assignment),"consecutive moves retain full recompute agreement");
    require(search.commit_move(a,old_a)&&search.commit_move(b,old_b),"both moves can be restored to original free positions");
    close(search.current_correlation(),recompute(graph,policy,thermal,initial),"roundtrip restores original exact Pearson");
    options.length_price=0.01;options.risk_price=0.3;
    const auto priced=search.candidates(a,options);
    for(const auto& c:priced) {
        const auto risk=[&](int s){const auto& p=policy.slots()[size_t(s)];return 0.5/std::max(1.0,graph.guide_capacity(p.lower_node))+0.5/std::max(1.0,graph.guide_capacity(p.upper_node));};
        close(c.estimated_score_delta,c.thermal_score_delta-options.length_price*c.proxy_length_delta_dbu-options.risk_price*(risk(c.slot)-risk(old_a)),"length and capacity proxy use declared score units");
    }
    TsvMoveSearch sequence(def,graph,policy,thermal,initial);
    auto evolving=initial;
    for(size_t step=0;step<96;++step) {
        const size_t moving=step%2?a:b,other=moving==a?b:a;
        const auto proposals=sequence.candidates(moving,options);
        require(!proposals.empty(),"long move sequence retains free candidates");
        const auto pick=proposals[(step*7)%proposals.size()];
        require(pick.slot!=evolving[other],"interleaved move never steals another net's slot");
        require(sequence.commit_move(moving,pick.slot),"interleaved move commits");
        evolving[moving]=pick.slot;
        close(sequence.current_correlation(),pick.correlation_after,"interleaved move agrees with prediction");
        close(sequence.current_correlation(),recompute(graph,policy,thermal,evolving),"interleaved move agrees with full Pearson rebuild");
        require(!sequence.commit_move(other,pick.slot),"stale occupied candidate cannot commit");
    }
    options.max_candidates=3;require(search.candidates(a,options).size()<=3,"output candidate budget enforced");
    options.max_candidates=0;require(search.candidates(a,options).empty(),"zero candidate budget disables proposals");
    options.max_candidates=24;options.max_pins=1;require(search.candidates(a,options).empty(),"high fanout work guard enforced");
    options.max_pins=128;options.length_price=std::numeric_limits<double>::quiet_NaN();
    rejects([&]{search.candidates(a,options);},"nonfinite ranking price rejected");
    auto invalid=initial;invalid[b]=old_a;
    rejects([&]{TsvMoveSearch bad(def,graph,policy,thermal,invalid);},"duplicate initial assignment rejected");
    invalid=initial;invalid[same]=slot_at(policy,graph,1,0);
    rejects([&]{TsvMoveSearch bad(def,graph,policy,thermal,invalid);},"same-die initial TSV rejected");
    invalid=initial;invalid[a]=-1;
    rejects([&]{TsvMoveSearch bad(def,graph,policy,thermal,invalid);},"missing cross-die assignment rejected");
    ThermalParser uniform;require(uniform.parse_json(write(dir,"uniform.json",heat(true)),graph.grid(),def.dbu_per_micron(),def.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),"uniform heat parses");
    TsvMoveSearch flat(def,graph,policy,uniform,initial);options={};options.max_candidates=24;options.length_price=0.01;
    close(flat.current_correlation(),0,"uniform heat Pearson defined as zero");
    for(const auto& c:flat.candidates(a,options)) {close(c.thermal_score_delta,0,"uniform heat never fabricates thermal improvement");close(c.correlation_after,0,"uniform heat candidate Pearson zero");}
}
void macro_geometry(const fs::path& dir) {
    const std::string macros=
        "MACRO LOW CLASS BLOCK ; ORIGIN 1 2 ; SIZE 10 BY 10 ; "
        "PIN B PORT LAYER metal3 ; RECT 2 3 4 5 ; END END B END LOW "
        "MACRO HIGH CLASS BLOCK ; ORIGIN 1 2 ; SIZE 10 BY 10 ; "
        "PIN T PORT LAYER metal14 ; RECT 2 3 4 5 ; END END T END HIGH ";
    LefParser lef;require(lef.parse(write(dir,"macro.lef",technology()+macros)),"macro technology parses");
    auto content=design();
    const std::string no_components="COMPONENTS 0 ; END COMPONENTS";
    content.replace(content.find(no_components),no_components.size(),
        "COMPONENTS 2 ; - lo LOW + PLACED ( 400 200 ) FN ; - hi HIGH + PLACED ( 100 100 ) S ; END COMPONENTS");
    const std::string io_net="- n0 ( PIN p0b ) ( PIN p0t ) ";
    content.replace(content.find(io_net),io_net.size(),"- n0 ( lo B ) ( hi T ) ");
    DefParser def;require(def.parse(write(dir,"macro.def",content),lef,DefGridMapping::OFFICIAL_FULL),"oriented macro design parses");
    RoutingGraphOptions go;go.geometry=RoutingGeometryMode::GLOBAL_GUIDE;go.allow_nonpreferred=false;go.score_20261020=true;
    RoutingGraph graph(def,lef,go);TsvPolicyOptions po;po.resource_mode=TsvResourceMode::GLOBAL_GRID;
    TsvPolicy policy(def,lef,graph,po);ThermalParser thermal;
    require(thermal.parse_json(write(dir,"macro_heat.json",heat()),graph.grid(),def.dbu_per_micron(),def.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),"macro heat parses");
    const size_t n=net_index(def,"n0");const auto& pins=def.nets()[n].pins;
    close(pins[0].coord.x,460,"FN macro pin uses transformed x geometry");
    close(pins[0].coord.y,260,"macro pin includes LEF origin in y geometry");
    close(pins[1].coord.x,160,"S macro pin uses transformed x geometry");
    close(pins[1].coord.y,140,"S macro pin uses transformed y geometry");
    std::vector<int> assignment(def.nets().size(),-1);
    assignment[n]=slot_at(policy,graph,0,0);assignment[net_index(def,"n1")]=slot_at(policy,graph,0,1);
    TsvMoveSearch search(def,graph,policy,thermal,assignment);
    TsvMoveSearchOptions options;options.max_candidates=24;options.thermal_gain=0;options.length_price=1;
    const Point old=policy.slots()[size_t(assignment[n])].point;
    bool distinguishes_checker=false;
    const auto candidates=search.candidates(n,options);
    require(candidates.size()==22,"macro fixture explores free slots");
    for(const auto& candidate:candidates) {
        const Point target=policy.slots()[size_t(candidate.slot)].point;
        double actual=0,checker=0;
        for(const auto& pin:pins) {
            actual+=pin.coord.distance_to(target)-pin.coord.distance_to(old);
            checker+=pin.evaluator_coord.distance_to(target)-pin.evaluator_coord.distance_to(old);
        }
        close(candidate.proxy_length_delta_dbu,actual,"MST proxy uses actual transformed macro pin geometry");
        distinguishes_checker|=std::abs(actual-checker)>1e-6;
    }
    require(distinguishes_checker,"macro fixture distinguishes geometry from checker coordinates");
}
void work_budget(const fs::path& dir,const LefParser& lef) {
    DefParser def;require(def.parse(write(dir,"fanout.def",design(false,2000)),lef,DefGridMapping::OFFICIAL_FULL),"high fanout design parses");
    RoutingGraphOptions go;go.geometry=RoutingGeometryMode::GLOBAL_GUIDE;go.allow_nonpreferred=false;go.score_20261020=true;
    RoutingGraph graph(def,lef,go);TsvPolicyOptions po;po.resource_mode=TsvResourceMode::GLOBAL_GRID;
    TsvPolicy policy(def,lef,graph,po);ThermalParser thermal;
    require(thermal.parse_json(write(dir,"fanout_heat.json",heat()),graph.grid(),def.dbu_per_micron(),def.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),"high fanout heat parses");
    std::vector<int> assignment(def.nets().size(),-1);
    const size_t n=net_index(def,"n0");assignment[n]=slot_at(policy,graph,0,0);assignment[net_index(def,"n1")]=slot_at(policy,graph,0,1);
    TsvMoveSearch search(def,graph,policy,thermal,assignment);
    TsvMoveSearchOptions options;options.max_candidates=4096;options.max_pins=4096;
    require(search.candidates(n,options).empty(),"quadratic work remains bounded when callers raise both public limits");
    require(search.current_slot(n)==assignment[n],"work-budget skip preserves assignment");
    close(search.current_correlation(),recompute(graph,policy,thermal,assignment),"work-budget skip preserves Pearson");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"usage: v7_tsv_tests ARTIFACT_DIRECTORY");const fs::path dir=argv[1];fs::create_directories(dir);
        LefParser lef;require(lef.parse(write(dir,"tech.lef",technology())),"technology parses");
        run(dir,lef);run(dir,lef,true);macro_geometry(dir);work_budget(dir,lef);std::cout<<"V7 TSV move checks: "<<checks<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
