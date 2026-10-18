#include "router/tsv_policy.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

using namespace eda;
namespace fs=std::filesystem;
namespace {
int checks=0;
void require(bool ok,const std::string& message) {
    ++checks;if (!ok) throw std::runtime_error(message);
}
std::string write(const fs::path& directory,const std::string& name,const std::string& text) {
    const auto path=directory/name;std::ofstream out(path);out<<text;out.close();
    if (!out) throw std::runtime_error("cannot write TSV polish fixture");
    return path.string();
}
std::string technology() {
    std::ostringstream out;
    for (int layer=1;layer<=20;++layer) {
        const auto name=get_layer_name(layer);
        out<<"LAYER "<<name<<" TYPE ROUTING ; DIRECTION "<<(layer%2?"HORIZONTAL":"VERTICAL")
           <<" ; WIDTH 0.2 ; END "<<name<<'\n';
    }
    out<<"MACRO RESOURCE_SLOT_top SIZE 7 BY 7 ; END RESOURCE_SLOT_top\n"
       <<"MACRO TSV_BOTTOM SIZE 7 BY 7 ; PIN TSV_PORT PORT LAYER metal10 ; RECT 1 1 6 6 ; "
       <<"END END TSV_PORT END TSV_BOTTOM\n"
       <<"MACRO TSV_TOP SIZE 7 BY 7 ; PIN TSV_PORT PORT LAYER metal11 ; RECT 1 1 6 6 ; "
       <<"END END TSV_PORT END TSV_TOP\n"
       <<"LAYER tsv TYPE CUT ; END tsv\n"
       <<"VIA tsv_v DEFAULT LAYER metal10 ; RECT -2.5 -2.5 2.5 2.5 ; LAYER tsv ; "
       <<"RECT -2.5 -2.5 2.5 2.5 ; LAYER metal11 ; RECT -2.5 -2.5 2.5 2.5 ; END tsv_v\n";
    return out.str();
}
std::string design(const std::vector<Point>& slots,const std::vector<Point>& terminals,int fixed=-1) {
    std::ostringstream out;
    out<<"UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 1000 1000 ) ; "
       <<"GCELLGRID X 0 DO 11 STEP 100 ; GCELLGRID Y 0 DO 11 STEP 100 ; ";
    for (int layer=1;layer<=20;++layer)
        out<<"TRACKS X 5 DO 100 STEP 10 LAYER "<<get_layer_name(layer)<<" ; "
           <<"TRACKS Y 5 DO 100 STEP 10 LAYER "<<get_layer_name(layer)<<" ; ";
    out<<"COMPONENTS "<<slots.size()<<" ; ";
    for (size_t i=0;i<slots.size();++i)
        out<<"- r"<<std::setw(3)<<std::setfill('0')<<i<<" RESOURCE_SLOT_top + PLACED ( "
           <<slots[i].x-35<<' '<<slots[i].y-35<<" ) N ; ";
    out<<"END COMPONENTS PINS "<<2*terminals.size()<<" ; ";
    for (size_t i=0;i<terminals.size();++i) for (int side=0;side<2;++side)
        out<<"- p"<<i<<(side?'t':'b')<<" + LAYER "
           <<get_layer_name(int(i)==fixed?10+side:(side?14:3))
           <<" ( -1 -1 ) ( 1 1 ) + FIXED ( "<<terminals[i].x<<' '<<terminals[i].y<<" ) N ; ";
    out<<"END PINS NETS "<<terminals.size()<<" ; ";
    for (size_t i=0;i<terminals.size();++i)
        out<<"- n"<<std::setw(3)<<std::setfill('0')<<i<<" ( PIN p"<<i<<"b ) ( PIN p"<<i<<"t ) ; ";
    out<<"END NETS END DESIGN";return out.str();
}
double terminal_distance(const DefParser& def,const TsvPolicy& policy,const std::vector<int>& chosen) {
    double sum=0;
    for (size_t n=0;n<chosen.size();++n) for (const auto& pin:def.nets()[n].pins)
        sum+=pin.coord.distance_to(policy.slots().at(chosen[n]).point);
    return sum;
}
void legal(const DefParser& def,const TsvPolicy& policy,const std::vector<int>& chosen) {
    require(chosen.size()==def.nets().size(),"one assignment per input net");
    const std::set<int> occupied(chosen.begin(),chosen.end());
    require(occupied.size()==chosen.size() && !occupied.count(-1),"distinct real reservations retain capacity one");
    for (size_t n=0;n<chosen.size();++n) {
        require(policy.compatible(def.nets()[n],chosen[n]),"every fixed interface remains compatible");
        const auto& slot=policy.slots()[chosen[n]];
        for (int conflict:slot.conflicts) require(!occupied.count(conflict),"no occupied reservation conflicts");
        const std::vector<GuideRect> witness{
            {slot.envelope.x1,slot.envelope.y1,slot.envelope.x2,slot.envelope.y2,"metal10"},
            {slot.envelope.x1,slot.envelope.y1,slot.envelope.x2,slot.envelope.y2,"metal11"}};
        std::string error;
        require(policy.validate_mapping(def.nets()[n],witness,chosen[n],error),"real VIA witness maps to the chosen reservation: "+error);
    }
}
void candidate_and_reverse_tests(const fs::path& dir,const LefParser& lef) {
    // Completion leaves a distance-3500 assignment. The nearest slot per net
    // cannot find all improving partners; reverse proposals and wider forward
    // proposals each expose a distinct strictly improving exchange.
    const std::vector<Point> slots{{735,635},{835,335},{635,335},{735,235},{135,135},{635,635},{835,535}};
    const std::vector<Point> pins{{195,595},{675,755},{195,355},{195,595},{515,515},{115,115},{195,195}};
    DefParser def;require(def.parse(write(dir,"candidate.def",design(slots,pins)),lef),"candidate fixture parses");
    RoutingGraphOptions go;go.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
    RoutingGraph graph(def,lef,go);ThermalParser thermal;
    TsvPolicyOptions options;options.nearest_candidates=options.max_candidates=1;options.assignment_polish=true;
    TsvPolicy narrow(def,lef,graph,options);const auto a=narrow.assign(def.nets(),thermal);
    require(terminal_distance(def,narrow,a)==6360,"legacy two-pass single-candidate distance is independent and exact");
    options.polish_candidates=4;TsvPolicy wide(def,lef,graph,options);const auto b=wide.assign(def.nets(),thermal);
    require(terminal_distance(def,wide,b)==5880,"wider polish candidates expose a better actual terminal assignment");
    require(wide.assignment_candidate_edges()==narrow.assignment_candidate_edges() &&
        wide.assignment_global_completions()==narrow.assignment_global_completions(),"polish width never changes initial matching budgets");
    options.polish_candidates=0;options.polish_bidirectional=true;
    TsvPolicy reverse(def,lef,graph,options);const auto c=reverse.assign(def.nets(),thermal);
    require(terminal_distance(def,reverse,c)==6280,"reverse anchor proposals improve the same width-one assignment");
    require(reverse.assignment_polish_correlation_before()==reverse.assignment_polish_correlation_after(),"pure exchanges preserve exact Pearson");
    require(reverse.assignment_polish_cost_reduction()>0 && wide.assignment_polish_cost_reduction()>0,"each accepted exchange strictly reduces proxy");
    for (const auto* policy:{&narrow,&wide,&reverse}) {
        const auto chosen=policy->assign(def.nets(),thermal);legal(def,*policy,chosen);
        require(chosen==policy->assign(def.nets(),thermal),"polish and counters are deterministic on replay");
    }
    auto explicit_legacy=options;explicit_legacy.polish_candidates=1;explicit_legacy.polish_bidirectional=false;
    TsvPolicy legacy(def,lef,graph,explicit_legacy);
    require(legacy.assign(def.nets(),thermal)==a,"zero polish candidates inherits initial width exactly");
    options.polish_thermal_pareto=true;TsvPolicy pareto(def,lef,graph,options);
    require(pareto.assign(def.nets(),thermal)==c,"Pareto accepts the same cost-decreasing correlation-preserving swaps");
    for (auto invalid:{size_t(0),size_t(65)}) {
        auto bad=options;bad.polish_passes=invalid;bool rejected=false;
        try {TsvPolicy::preflight(def,lef,bad);} catch(const std::runtime_error&) {rejected=true;}
        require(rejected,"invalid pass budget is rejected before graph allocation");
    }
    auto bad=options;bad.polish_candidates=65537;bool rejected=false;
    try {TsvPolicy::preflight(def,lef,bad);} catch(const std::runtime_error&) {rejected=true;}
    require(rejected,"oversized polish candidate budget is rejected");
}
void pass_and_fixed_tests(const fs::path& dir,const LefParser& lef) {
    const std::vector<Point> slots{{335,235},{835,835},{735,535},{335,735},{235,435},{535,435},{135,835},{635,535},{535,535}};
    const std::vector<Point> pins{{195,675},{595,275},{355,515},{275,355},{515,275},{435,355},{435,195},{675,355},{675,435}};
    DefParser def;require(def.parse(write(dir,"passes.def",design(slots,pins)),lef),"pass fixture parses");
    RoutingGraphOptions go;go.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
    RoutingGraph graph(def,lef,go);ThermalParser thermal;
    TsvPolicyOptions options;options.nearest_candidates=options.max_candidates=1;
    options.assignment_polish=true;options.polish_candidates=2;
    TsvPolicy short_run(def,lef,graph,options);const auto a=short_run.assign(def.nets(),thermal);
    require(terminal_distance(def,short_run,a)==5360 && short_run.assignment_polish_passes()==2,"two passes exhaust their budget before convergence");
    options.polish_passes=8;TsvPolicy converged(def,lef,graph,options);const auto b=converged.assign(def.nets(),thermal);
    require(terminal_distance(def,converged,b)==5040,"a third pass enables a further strict geometric improvement");
    require(converged.assignment_polish_passes()==4,"polish stops after the first pass without an accepted move");
    legal(def,converged,b);
    auto fixed_pins=pins;fixed_pins[1]=slots[1];DefParser fixed;
    require(fixed.parse(write(dir,"fixed.def",design(slots,fixed_pins,1)),lef),"fixed-interface fixture parses");
    RoutingGraph fixed_graph(fixed,lef,go);options.polish_candidates=9;options.polish_bidirectional=true;
    for (bool pareto:{false,true}) {
        options.polish_thermal_pareto=pareto;
        TsvPolicy policy(fixed,lef,fixed_graph,options);const auto chosen=policy.assign(fixed.nets(),thermal);
        require(policy.slots()[chosen[1]].point.distance_to(slots[1])==0,"wide and reverse candidates never relocate fixed M10/M11 pins");
        legal(fixed,policy,chosen);
        require(chosen==policy.assign(fixed.nets(),thermal),"fixed-interface replay is deterministic");
        require(policy.assignment_polish_correlation_after()>=policy.assignment_polish_correlation_before(),"uniform-field Pareto remains valid");
    }
}
}
int main(int argc,char** argv) {
    try {
        if (argc!=2) throw std::runtime_error("usage: tsv_polish_tests artifact-directory");
        const fs::path dir=argv[1];fs::create_directories(dir);LefParser lef;
        require(lef.parse(write(dir,"polish.lef",technology())),"fixture technology parses");
        candidate_and_reverse_tests(dir,lef);pass_and_fixed_tests(dir,lef);
        std::cout<<"TSV polish checks: "<<checks<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
