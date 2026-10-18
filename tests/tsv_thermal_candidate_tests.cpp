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
void require(bool condition,const std::string& message) {
    ++checks;if (!condition) throw std::runtime_error(message);
}
std::string write(const fs::path& dir,const std::string& name,const std::string& value) {
    const auto path=dir/name;std::ofstream out(path);out<<value;out.close();
    if (!out) throw std::runtime_error("could not write thermal TSV fixture");
    return path.string();
}
std::string technology() {
    std::ostringstream out;
    for (int layer=1;layer<=20;++layer) {
        const auto name=get_layer_name(layer);
        out<<"LAYER "<<name<<" TYPE ROUTING ; DIRECTION "<<(layer%2?"HORIZONTAL":"VERTICAL")
           <<" ; WIDTH 0.2 ; END "<<name<<'\n';
    }
    out<<"MACRO RESOURCE_SLOT_top SIZE 7 BY 7 ; END RESOURCE_SLOT_top\n";
    for (int side=0;side<2;++side) {
        const std::string name=side?"TSV_TOP":"TSV_BOTTOM";
        out<<"MACRO "<<name<<" SIZE 7 BY 7 ; PIN TSV_PORT PORT LAYER "<<get_layer_name(10+side)
           <<" ; RECT 1 1 6 6 ; END END TSV_PORT END "<<name<<'\n';
    }
    out<<"LAYER tsv TYPE CUT ; END tsv\nVIA tsv_v DEFAULT LAYER metal10 ; RECT -2.5 -2.5 2.5 2.5 ; "
       <<"LAYER tsv ; RECT -2.5 -2.5 2.5 2.5 ; LAYER metal11 ; RECT -2.5 -2.5 2.5 2.5 ; END tsv_v\n";
    return out.str();
}
std::string design(const std::vector<int>& slot_x,const std::vector<int>& pin_x,bool fixed=false) {
    std::ostringstream out;
    out<<"UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 1000 1000 ) ; "
       <<"GCELLGRID X 0 DO 11 STEP 100 ; GCELLGRID Y 0 DO 11 STEP 100 ; ";
    for (int layer=1;layer<=20;++layer)
        out<<"TRACKS X 5 DO 100 STEP 10 LAYER "<<get_layer_name(layer)<<" ; "
           <<"TRACKS Y 5 DO 100 STEP 10 LAYER "<<get_layer_name(layer)<<" ; ";
    out<<"COMPONENTS "<<slot_x.size()<<" ; ";
    for (size_t i=0;i<slot_x.size();++i)
        out<<"- r"<<std::setw(3)<<std::setfill('0')<<i<<" RESOURCE_SLOT_top + FIXED ( "<<slot_x[i]-35<<" 100 ) N ; ";
    out<<"END COMPONENTS PINS "<<2*pin_x.size()<<" ; ";
    for (size_t i=0;i<pin_x.size();++i) for (int side=0;side<2;++side)
        out<<"- p"<<i<<(side?'t':'b')<<" + LAYER "<<get_layer_name(fixed?10+side:(side?14:3))
           <<" ( -1 -1 ) ( 1 1 ) + FIXED ( "<<pin_x[i]<<" 135 ) N ; ";
    out<<"END PINS NETS "<<pin_x.size()<<" ; ";
    for (size_t i=0;i<pin_x.size();++i)
        out<<"- n"<<std::setw(3)<<std::setfill('0')<<i<<" ( PIN p"<<i<<"b ) ( PIN p"<<i<<"t ) ; ";
    out<<"END NETS END DESIGN";return out.str();
}
std::string heat(bool uniform=false) {
    std::ostringstream out;out<<"{\"meta\":{\"ambientTemp\":25},\"nodes\":[";
    for (int row=0;row<10;++row) for (int col=0;col<10;++col) {
        if (row || col) out<<',';
        out<<'['<<5+10*col<<','<<5+10*row<<",0,"<<(uniform || col>=6?30:25)<<']';
    }
    out<<"]}";return out.str();
}
void legal(const DefParser& def,const TsvPolicy& policy,const std::vector<int>& chosen) {
    std::set<int> occupied(chosen.begin(),chosen.end());
    require(occupied.size()==chosen.size() && !occupied.count(-1),"thermal proposals preserve capacity one");
    for (size_t n=0;n<chosen.size();++n) {
        require(policy.compatible(def.nets()[n],chosen[n]),"thermal proposals preserve fixed-pin compatibility");
        const auto& slot=policy.slots().at(chosen[n]);
        for (int conflict:slot.conflicts) require(!occupied.count(conflict),"thermal proposals preserve reservation conflicts");
        std::vector<GuideRect> witness{
            {slot.envelope.x1,slot.envelope.y1,slot.envelope.x2,slot.envelope.y2,"metal10"},
            {slot.envelope.x1,slot.envelope.y1,slot.envelope.x2,slot.envelope.y2,"metal11"}};
        std::string error;require(policy.validate_mapping(def.nets()[n],witness,chosen[n],error),"thermal proposal retains a valid VIA witness: "+error);
    }
}
void tests(const fs::path& dir,const LefParser& lef) {
    DefParser def;require(def.parse(write(dir,"candidate.def",design({135,235,835},{135})),lef),"thermal candidate fixture parses");
    RoutingGraphOptions go;go.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
    RoutingGraph graph(def,lef,go);ThermalParser thermal;
    require(thermal.parse_json(write(dir,"heat.json",heat()),def.gcell_grid(),def.dbu_per_micron(),def.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),"thermal field parses");
    TsvPolicyOptions old;old.assignment_polish=true;old.nearest_candidates=old.max_candidates=1;old.polish_heat_weight=16;
    TsvPolicy spatial(def,lef,graph,old);const auto cold=spatial.assign(def.nets(),thermal);
    require(spatial.slots()[cold[0]].point.x==135,"spatial K=1 excludes the distant beneficial hot resource");
    require(spatial.assignment_polish_thermal_queries()==0 && spatial.assignment_polish_thermal_candidates()==0,"disabled mode performs no thermal queries");
    auto options=old;options.polish_thermal_candidates=true;
    TsvPolicy aware(def,lef,graph,options);const auto warm=aware.assign(def.nets(),thermal);
    require(aware.slots()[warm[0]].point.x==835,"thermal query exposes the hot slot beyond the spatial K shortlist");
    require(aware.assignment_polish_cost_reduction()>spatial.assignment_polish_cost_reduction(),"new candidate strictly improves the unchanged true proxy");
    require(aware.assignment_polish_moves()==1 && aware.assignment_polish_thermal_candidates()==1,"thermal shortlist adds exactly the previously missed accepted candidate");
    require(aware.assignment_polish_thermal_queries()==2,"thermal query runs once per visited net and pass");
    legal(def,aware,warm);
    require(aware.assign(def.nets(),thermal)==warm && aware.assignment_polish_thermal_candidates()==1,"thermal candidate replay and counters are deterministic");
    options.polish_thermal_candidates=false;TsvPolicy disabled(def,lef,graph,options);
    require(disabled.assign(def.nets(),thermal)==cold,"explicitly disabled mode preserves the original assignment exactly");
    options.polish_thermal_candidates=true;options.polish_heat_weight=10;
    TsvPolicy proposal_only(def,lef,graph,options);
    require(proposal_only.assign(def.nets(),thermal)==cold && proposal_only.assignment_polish_thermal_candidates()>0 &&
        proposal_only.assignment_polish_moves()==0,"a good thermal proposal is rejected when its true two-sided MST proxy gets worse");
    options.polish_thermal_candidates=false;
    options.polish_heat_weight=0;TsvPolicy zero_old(def,lef,graph,options);const auto zero=zero_old.assign(def.nets(),thermal);
    options.polish_thermal_candidates=true;TsvPolicy zero_new(def,lef,graph,options);
    require(zero_new.assign(def.nets(),thermal)==zero && zero_new.assignment_polish_thermal_candidates()==0,"lambda zero returns the same ordered spatial candidates and assignment");
    options.polish_heat_weight=16;ThermalParser uniform;
    require(uniform.parse_json(write(dir,"uniform.json",heat(true)),def.gcell_grid(),def.dbu_per_micron(),def.die_area(),ThermalMapping::OFFICIAL_PREVIOUS),"uniform field parses");
    TsvPolicy flat(def,lef,graph,options);
    require(flat.assign(def.nets(),uniform)==cold && flat.assignment_polish_thermal_candidates()==0,"uniform heat keeps spatial ranking and original assignment");
    auto invalid=options;invalid.polish_thermal_pareto=true;bool rejected=false;
    try {TsvPolicy::preflight(def,lef,invalid);} catch(const std::runtime_error&) {rejected=true;}
    require(rejected,"thermal shortlist explicitly rejects the Pareto rule");
    invalid=options;invalid.assignment_polish=false;rejected=false;
    try {TsvPolicy::preflight(def,lef,invalid);} catch(const std::runtime_error&) {rejected=true;}
    require(rejected,"thermal shortlist requires enabled assignment polish");
    DefParser fixed;require(fixed.parse(write(dir,"fixed.def",design({135,235,835},{135},true)),lef),"fixed-interface fixture parses");
    RoutingGraph fixed_graph(fixed,lef,go);TsvPolicy fixed_policy(fixed,lef,fixed_graph,options);
    const auto forced=fixed_policy.assign(fixed.nets(),thermal);legal(fixed,fixed_policy,forced);
    require(fixed_policy.slots()[forced[0]].point.x==135 && fixed_policy.assignment_polish_moves()==0,"thermal preference never relocates a fixed interface");
    // The subtree heat maximum remains high when its hot point is deleted.
    // Queries must still choose another active point, then terminate once all
    // hot resources are occupied; a static loose bound is not a live candidate.
    DefParser multiple;
    require(multiple.parse(write(dir,"inactive.def",design({135,235,335,635,835},{135,235})),lef),"dynamic free-resource fixture parses");
    RoutingGraph multiple_graph(multiple,lef,go);TsvPolicy reused(multiple,lef,multiple_graph,options);
    const auto moved=reused.assign(multiple.nets(),thermal);legal(multiple,reused,moved);
    require(reused.slots()[moved[0]].point.x==635 && reused.slots()[moved[1]].point.x==835,"inactive high-heat bounds never return an occupied resource");
    require(reused.assignment_polish_moves()==2,"each hot resource is consumed exactly once");
    require(reused.assignment_polish_thermal_candidates()<=reused.assignment_polish_thermal_queries(),"K=1 adds at most one thermal candidate per query");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"usage: tsv_thermal_candidate_tests artifact-directory");
        const fs::path dir=argv[1];fs::create_directories(dir);LefParser lef;
        require(lef.parse(write(dir,"thermal.lef",technology())),"fixture technology parses");
        tests(dir,lef);std::cout<<"TSV thermal candidate checks: "<<checks<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
