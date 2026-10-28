#include "router/tsv_policy.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>

using namespace eda;
namespace fs=std::filesystem;
namespace {
int checks=0;
void require(bool ok,const std::string& message) {
    ++checks;if (!ok) throw std::runtime_error(message);
}
std::string write(const fs::path& dir,const std::string& name,const std::string& text) {
    const auto path=dir/name;std::ofstream out(path);out<<text;out.close();
    if (!out) throw std::runtime_error("cannot write block TSV fixture");
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
       <<"MACRO TSV_BOTTOM SIZE 7 BY 7 ; PIN TSV_PORT PORT LAYER metal10 ; RECT 1 1 6 6 ; END END TSV_PORT END TSV_BOTTOM\n"
       <<"MACRO TSV_TOP SIZE 7 BY 7 ; PIN TSV_PORT PORT LAYER metal11 ; RECT 1 1 6 6 ; END END TSV_PORT END TSV_TOP\n"
       <<"LAYER tsv TYPE CUT ; END tsv\n"
       <<"VIA tsv_v DEFAULT LAYER metal10 ; RECT -2.5 -2.5 2.5 2.5 ; LAYER tsv ; "
       <<"RECT -2.5 -2.5 2.5 2.5 ; LAYER metal11 ; RECT -2.5 -2.5 2.5 2.5 ; END tsv_v\n";
    return out.str();
}
std::string design(const std::vector<Point>& slots,const std::vector<Point>& pins,int fixed=-1) {
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
    out<<"END COMPONENTS PINS "<<2*pins.size()<<" ; ";
    for (size_t i=0;i<pins.size();++i) for (int side=0;side<2;++side)
        out<<"- p"<<i<<(side?'t':'b')<<" + LAYER "
           <<get_layer_name(int(i)==fixed?10+side:(side?14:3))
           <<" ( -1 -1 ) ( 1 1 ) + FIXED ( "<<pins[i].x<<' '<<pins[i].y<<" ) N ; ";
    out<<"END PINS NETS "<<pins.size()<<" ; ";
    for (size_t i=0;i<pins.size();++i)
        out<<"- n"<<std::setw(3)<<std::setfill('0')<<i<<" ( PIN p"<<i<<"b ) ( PIN p"<<i<<"t ) ; ";
    out<<"END NETS END DESIGN";return out.str();
}
double distance(const std::vector<Net>& nets,const TsvPolicy& policy,const std::vector<int>& chosen) {
    double sum=0;
    for (size_t n=0;n<nets.size();++n) for (const auto& pin:nets[n].pins)
        sum+=pin.coord.distance_to(policy.slots().at(chosen[n]).point);
    return sum;
}
void legal(const std::vector<Net>& nets,const TsvPolicy& policy,const std::vector<int>& before,
           const std::vector<int>& after) {
    const std::set<int> occupied(after.begin(),after.end());
    require(occupied==std::set<int>(before.begin(),before.end()),"exact occupied slot set is invariant, including every thermal cell");
    require(occupied.size()==after.size() && !occupied.count(-1),"capacity one and complete assignment preserved");
    for (size_t n=0;n<nets.size();++n) {
        require(policy.compatible(nets[n],after[n]),"fixed and geometric compatibility preserved");
        const auto& slot=policy.slots()[after[n]];
        for (int other:slot.conflicts) require(!occupied.count(other),"enabled reservation set remains conflict free");
        const std::vector<GuideRect> witness{
            {slot.envelope.x1,slot.envelope.y1,slot.envelope.x2,slot.envelope.y2,"metal10"},
            {slot.envelope.x1,slot.envelope.y1,slot.envelope.x2,slot.envelope.y2,"metal11"}};
        std::string error;
        require(policy.validate_mapping(nets[n],witness,after[n],error),"real VIA geometry remains a valid witness: "+error);
    }
}
TsvPolicyOptions base_options() {
    TsvPolicyOptions options;options.nearest_candidates=options.max_candidates=1;
    options.assignment_polish=true;options.polish_candidates=16;options.polish_passes=8;
    options.block_size=3;return options;
}
void cycle_and_budget_tests(const fs::path& dir,const LefParser& lef) {
    // Geometric Manhattan costs (one die) are
    // [480,640,620], [680,360,1260], [920,120,1020].
    // Identity costs 1860; every transposition is equal or worse. The cyclic
    // permutation [2,0,1] costs 1420. This is not a synthetic arbitrary matrix.
    const std::vector<Point> slots{{535,935},{135,335},{935,235}};
    const std::vector<Point> pins{{595,515},{115,675},{115,435}};
    DefParser def;require(def.parse(write(dir,"cycle.def",design(slots,pins)),lef),"cycle design parses");
    RoutingGraphOptions go;go.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
    RoutingGraph graph(def,lef,go);ThermalParser thermal;
    auto options=base_options();TsvPolicy old(def,lef,graph,options);const auto before=old.assign(def.nets(),thermal);
    require(distance(def.nets(),old,before)==3720,"pair-only reference reaches the known strict-cycle local optimum");
    for (size_t i=0;i<before.size();++i) for (size_t j=i+1;j<before.size();++j) {
        auto trial=before;std::swap(trial[i],trial[j]);
        require(distance(def.nets(),old,trial)>=3720,"no two-net exchange improves this geometric fixture");
    }
    require(old.assignment_block_groups()==0 && old.assignment_block_seconds()==0,"default-off performs no block work");
    options.assignment_block_refine=true;TsvPolicy improved(def,lef,graph,options);
    const auto after=improved.assign(def.nets(),thermal);
    require(distance(def.nets(),improved,after)==2840,"three-cycle reduces the exact geometric cost beyond pair swaps");
    require(improved.assignment_block_improvements()==1 && improved.assignment_block_moved_nets()==3,
            "one atomic three-net permutation was accepted");
    require(improved.assignment_block_cost_reduction()>0 && improved.assignment_block_cost_evaluations()==18,
            "two complete bounded 3x3 matrices were evaluated");
    require(improved.assignment_block_mst_work()==144,"MST work counts both two-terminal sides for every edge");
    require(improved.assignment_polish_correlation_before()==improved.assignment_polish_correlation_after(),
            "existing Pearson diagnostics remain valid after occupancy-preserving refinement");
    legal(def.nets(),improved,before,after);
    require(improved.assign(def.nets(),thermal)==after && improved.assignment_block_moved_nets()==3,
            "replay is deterministic and statistics reset");
    options.block_max_cost_evaluations=8;TsvPolicy partial(def,lef,graph,options);
    require(partial.assign(def.nets(),thermal)==before,"incomplete matrix never commits a partial permutation");
    require(partial.assignment_block_cost_evaluations()==8 && partial.assignment_block_improvements()==0 &&
            partial.assignment_block_budget_exhausted(),"edge budget is exact and reports exhaustion");
    options.block_max_cost_evaluations=2000000;options.block_max_groups=1;TsvPolicy one(def,lef,graph,options);
    require(one.assign(def.nets(),thermal)==after && one.assignment_block_groups()==1 && one.assignment_block_budget_exhausted(),
            "group budget retains the last complete improvement and stops before another group");
    options.block_max_groups=8192;options.block_passes=1;TsvPolicy single_pass(def,lef,graph,options);
    require(single_pass.assign(def.nets(),thermal)==after && single_pass.assignment_block_groups()==1 &&
            !single_pass.assignment_block_budget_exhausted(),"configured single pass is a normal bounded completion");
    options.block_max_seconds=1e-12;TsvPolicy expired(def,lef,graph,options);
    require(expired.assign(def.nets(),thermal)==before && expired.assignment_block_budget_exhausted() &&
            expired.assignment_block_cost_evaluations()==0,"monotonic cap preserves a complete known-feasible assignment");
    auto no_polish=base_options();no_polish.assignment_polish=false;
    TsvPolicy feasible(def,lef,graph,no_polish);const auto feasible_assignment=feasible.assign(def.nets(),thermal);
    options=base_options();options.assignment_block_refine=true;TsvPolicy global_expired(def,lef,graph,options);
    require(global_expired.assign(def.nets(),thermal,std::chrono::steady_clock::now()-std::chrono::seconds(1))==feasible_assignment &&
            global_expired.assignment_polish_passes()==0 && global_expired.assignment_block_cost_evaluations()==0,
            "expired shared deadline preserves feasibility and skips optional polish and block work");
    auto many_pins=def.nets();while (many_pins[0].pins.size()<=64) many_pins[0].pins.push_back(many_pins[0].pins.front());
    options=base_options();TsvPolicy many_old(def,lef,graph,options);const auto many_before=many_old.assign(many_pins,thermal);
    options.assignment_block_refine=true;TsvPolicy many_new(def,lef,graph,options);const auto many_after=many_new.assign(many_pins,thermal);
    require(many_new.assignment_block_skipped_high_fanout()==1 && many_before[0]==many_after[0],
            "high-fanout net is pinned to its existing reservation instead of consuming quadratic work");
    legal(many_pins,many_new,many_before,many_after);
}
void fixed_and_optimal_tests(const fs::path& dir,const LefParser& lef) {
    const std::vector<Point> slots{{535,935},{135,335},{935,235},{835,835}};
    const std::vector<Point> pins{{595,515},{115,675},{115,435},{835,835}};
    DefParser fixed;require(fixed.parse(write(dir,"fixed.def",design(slots,pins,3)),lef),"fixed-interface design parses");
    RoutingGraphOptions go;go.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
    RoutingGraph graph(fixed,lef,go);ThermalParser thermal;auto options=base_options();
    TsvPolicy old(fixed,lef,graph,options);const auto before=old.assign(fixed.nets(),thermal);
    options.assignment_block_refine=true;TsvPolicy improved(fixed,lef,graph,options);
    const auto after=improved.assign(fixed.nets(),thermal);
    require(before[3]==after[3] && !improved.compatible(fixed.nets()[3],after[0]),"incompatible alternatives never move the fixed interface");
    require(distance(fixed.nets(),improved,after)==2840 && improved.assignment_block_moved_nets()==3,
            "free three-cycle can improve while an incompatible fixed net remains untouched");
    legal(fixed.nets(),improved,before,after);
    DefParser optimal;require(optimal.parse(write(dir,"optimal.def",design(slots,slots)),lef),"optimal fixture parses");
    RoutingGraph optimal_graph(optimal,lef,go);TsvPolicy unchanged(optimal,lef,optimal_graph,options);
    const auto mapping=unchanged.assign(optimal.nets(),thermal);
    require(distance(optimal.nets(),unchanged,mapping)==0 && unchanged.assignment_block_improvements()==0,
            "already-optimal mapping is kept without neutral churn");
    legal(optimal.nets(),unchanged,mapping,mapping);
    for (int field=0;field<12;++field) {
        auto bad=options;
        switch (field) {
        case 0:bad.block_size=2;break;case 1:bad.block_size=65;break;
        case 2:bad.block_passes=0;break;case 3:bad.block_passes=17;break;
        case 4:bad.block_max_groups=0;break;case 5:bad.block_max_groups=65537;break;
        case 6:bad.block_max_cost_evaluations=0;break;case 7:bad.block_max_cost_evaluations=100000001;break;
        case 8:bad.block_max_seconds=0;break;case 9:bad.block_max_seconds=601;break;
        case 10:bad.block_max_seconds=std::numeric_limits<double>::infinity();break;
        case 11:bad.block_max_seconds=std::numeric_limits<double>::quiet_NaN();break;
        }
        bool rejected=false;try {TsvPolicy::preflight(fixed,lef,bad);}catch(const std::runtime_error&) {rejected=true;}
        require(rejected,"invalid block work bound is rejected before routing allocation");
    }
}
}
int main(int argc,char** argv) {
    try {
        if (argc!=2) throw std::runtime_error("usage: tsv_assignment_v5_tests artifact-directory");
        const fs::path dir=argv[1];fs::create_directories(dir);LefParser lef;
        require(lef.parse(write(dir,"block.lef",technology())),"fixture technology parses");
        cycle_and_budget_tests(dir,lef);fixed_and_optimal_tests(dir,lef);
        std::cout<<"TSV V5 assignment checks: "<<checks<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
