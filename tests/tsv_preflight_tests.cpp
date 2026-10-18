#include "router/tsv_policy.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <set>
#include <limits>

using namespace eda;
namespace fs=std::filesystem;
namespace {
int checks=0;
void require(bool ok,const std::string& reason) {
    ++checks;if (!ok) throw std::runtime_error(reason);
}
std::string write(const fs::path& dir,const std::string& name,const std::string& content) {
    const auto path=dir/name;
    std::ofstream out(path);out<<content;out.close();
    if (!out) throw std::runtime_error("cannot write TSV fixture");
    return path.string();
}
std::string technology() {
    std::string text;
    for (int l=1;l<=20;++l) {
        const auto name=get_layer_name(l);
        text+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+
              (l%2?"HORIZONTAL":"VERTICAL")+" ; WIDTH 0.2 ; END "+name+"\n";
    }
    for (const std::string die:{"bottom","top"}) {
        const auto name="RESOURCE_SLOT_"+die;
        text+="MACRO "+name+" SIZE 7 BY 7 ; END "+name+"\n";
    }
    text+="LAYER tsv TYPE CUT ; END tsv\nVIA tsv_v DEFAULT LAYER metal10 ; RECT -2.5 -2.5 2.5 2.5 ; "
          "LAYER tsv ; RECT -2.5 -2.5 2.5 2.5 ; LAYER metal11 ; RECT -2.5 -2.5 2.5 2.5 ; END tsv_v\n";
    for (int side=0;side<2;++side) {
        const std::string name=side?"TSV_TOP":"TSV_BOTTOM",layer=get_layer_name(10+side);
        text+="MACRO "+name+" SIZE 7 BY 7 ; PIN TSV_PORT PORT LAYER "+layer+
            " ; RECT 1 1 6 6 ; END END TSV_PORT END "+name+"\n";
    }
    return text;
}
std::string design(size_t bottom,size_t top,size_t cross,const std::vector<Point>& anchors={}) {
    const size_t net_count=std::max<size_t>(1,cross);
    const std::string top_layer=cross ? "metal11" : "metal1";
    std::ostringstream text;
    text<<"UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 1000 1000 ) ; "
        <<"GCELLGRID X 0 DO 11 STEP 100 ; GCELLGRID Y 0 DO 11 STEP 100 ; ";
    for (int layer=1;layer<=20;++layer)
        text<<"TRACKS X 5 DO 100 STEP 10 LAYER "<<get_layer_name(layer)<<" ; "
            <<"TRACKS Y 5 DO 100 STEP 10 LAYER "<<get_layer_name(layer)<<" ; ";
    text<<"COMPONENTS "<<bottom+top<<" ; ";
    for (size_t i=0;i<bottom;++i)
        text<<"- b"<<i<<" RESOURCE_SLOT_bottom + PLACED ( "<<100+i*80<<" 100 ) N ; ";
    for (size_t i=0;i<top;++i)
        text<<"- t"<<i<<" RESOURCE_SLOT_top + PLACED ( "<<100+i*80<<" 100 ) N ; ";
    text<<"END COMPONENTS PINS "<<2*net_count<<" ; ";
    for (size_t i=0;i<net_count;++i) {
        const Point bottom_pin=anchors.empty()?Point(50,50):anchors.at(i);
        const Point top_pin=anchors.empty()?Point(950,950):anchors.at(i);
        text<<"- p"<<i<<"b + LAYER metal1 ( -1 -1 ) ( 1 1 ) + FIXED ( "
            <<bottom_pin.x<<' '<<bottom_pin.y<<" ) N ; "
            <<"- p"<<i<<"t + LAYER "<<top_layer<<" ( -1 -1 ) ( 1 1 ) + FIXED ( "
            <<top_pin.x<<' '<<top_pin.y<<" ) N ; ";
    }
    text<<"END PINS NETS "<<net_count<<" ; ";
    for (size_t i=0;i<net_count;++i)
        text<<"- n"<<i<<" ( PIN p"<<i<<"b ) ( PIN p"<<i<<"t ) ; ";
    text<<"END NETS END DESIGN";
    return text.str();
}
void test_preflight(const fs::path& dir) {
    LefParser lef;
    require(lef.parse(write(dir,"preflight.lef",technology())),"parse fixture LEF");
    auto check=[&](size_t bottom,size_t top,size_t cross,bool accepted) {
        DefParser def;
        require(def.parse(write(dir,"preflight.def",design(bottom,top,cross)),lef),"parse fixture DEF");
        std::string error;
        TsvPolicyOptions options;options.resource_mode=TsvResourceMode::PAIRED;
        try {TsvPolicy::preflight(def,lef,options);} catch(const std::runtime_error& e) {error=e.what();}
        require(error.empty()==accepted,"preflight capacity bound");
        if (!accepted) {
            require(error.find("TSV_POLICY_UNRESOLVED")!=std::string::npos,"stable unresolved-policy diagnostic");
            require(error.find("cross_die_nets="+std::to_string(cross))!=std::string::npos,"cross-net count diagnostic");
            require(error.find("bottom_slots="+std::to_string(bottom))!=std::string::npos,"bottom count diagnostic");
            require(error.find("top_slots="+std::to_string(top))!=std::string::npos,"top count diagnostic");
            require(error.find("not a contest legality rule")!=std::string::npos,"does not misclassify input legality");
        }
    };
    check(0,0,0,true);
    check(0,3,2,false); // Public001 representation: only top reservations.
    check(1,3,2,false); // Public003/005 representation: insufficient bottom count.
    check(3,1,2,false);
    check(2,2,2,true);  // A necessary bound, not proof of geometric legality.
    DefParser single;
    require(single.parse(write(dir,"single.def",design(0,3,2)),lef),"single-side DEF");
    TsvPolicy::preflight(single,lef);
    require(true,"auto accepts real single-side resource projections");
    std::ostringstream heat;heat<<"{\"meta\":{\"ambientTemp\":25},\"nodes\":[";
    bool first=true;
    for (int row=0;row<10;++row) for (int col=0;col<10;++col) {
        if (!first) heat<<',';
        first=false;
        heat<<"["<<5+10*col<<","<<5+10*row<<",0,30]";
    }
    heat<<"]}";
    ThermalParser thermal;
    require(thermal.parse_json(write(dir,"heat.json",heat.str()),single.gcell_grid(),
        single.dbu_per_micron(),single.die_area()),"fixture heat");
    RoutingGraph graph(single,lef);
    TsvPolicyOptions options;options.nearest_candidates=options.max_candidates=1;
    TsvPolicy policy(single,lef,graph,options);
    require(policy.slots().size()==3 && policy.enabled_slots()==3,"projected real resources remain distinct");
    require(std::string(policy.pairing_mode())=="resource-projection-via-geometry","auto selects projected mode");
    const auto allocation=policy.assign(single.nets(),thermal);
    require(allocation.size()==2 && allocation[0]!=allocation[1],"global sparse completion allocates distinct actual resources");
    require(policy.assignment_global_completions()==1,"limited local graph is completed with one free resource");
    for (size_t n=0;n<allocation.size();++n) {
        const auto& slot=policy.slots().at(allocation[n]);
        require(slot.projected && slot.bottom_name.empty() && !slot.top_name.empty(),"no opposite-side input instance is fabricated");
        require(slot.via_name=="tsv_v","actual LEF TSV VIA retained");
        std::vector<GuideRect> witness{
            {slot.envelope.x1,slot.envelope.y1,slot.envelope.x2,slot.envelope.y2,"metal10"},
            {slot.envelope.x1,slot.envelope.y1,slot.envelope.x2,slot.envelope.y2,"metal11"}};
        std::string error;
        require(policy.validate_mapping(single.nets()[n],witness,slot.id,error),"projected VIA geometry witness remains validated: "+error);
    }
    options.global_completion=false;
    TsvPolicy limited(single,lef,graph,options);
    bool incomplete=false;
    try {limited.assign(single.nets(),thermal);} catch(const std::runtime_error& error) {
        incomplete=std::string(error.what()).find("heuristic exhausted")!=std::string::npos;
    }
    require(incomplete,"strict local pool reports heuristic exhaustion instead of fake feasibility");
    // Greedy chooses slots 0 and 1, leaving the third net unmatched. A layered
    // augmenting path must displace earlier owners into the third resource.
    DefParser augment;
    require(augment.parse(write(dir,"augment.def",design(0,3,3,{{155,135},{235,135},{155,135}})),lef),
        "augmenting fixture DEF");
    options.nearest_candidates=options.max_candidates=2;
    TsvPolicy augmentation(augment,lef,graph,options);
    const auto completed=augmentation.assign(augment.nets(),thermal);
    std::set<int> unique(completed.begin(),completed.end());
    require(unique.size()==3 && !unique.count(-1),"sparse layered matching displaces earlier owners");
    require(augmentation.assignment_global_completions()==0,"augmenting path completes strictly local candidates");

    // Projected mode must retain fixed aligned M10/M11 interfaces too. Free
    // resources are not a reason to move an already fixed crossing.
    std::string fixed_text=design(0,3,2,{{135,135},{135,135}});
    size_t next=0;
    for (int i=0;i<2;++i) {
        const auto begin=fixed_text.find("- p"+std::to_string(i)+"b + LAYER metal1",next);
        require(begin!=std::string::npos,"locate forced bottom interface fixture");
        const auto layer=begin+fixed_text.substr(begin).find("metal1");
        fixed_text.replace(layer,6,"metal10");next=layer+7;
    }
    DefParser fixed;
    require(fixed.parse(write(dir,"fixed-conflict.def",fixed_text),lef),"fixed projected fixture DEF");
    TsvPolicy fixed_policy(fixed,lef,graph);
    bool fixed_conflict=false;
    try {fixed_policy.assign(fixed.nets(),thermal);} catch(const std::runtime_error& error) {
        fixed_conflict=std::string(error.what()).find("fixed-interface capacity conflict")!=std::string::npos ||
            std::string(error.what()).find("heuristic exhausted")!=std::string::npos;
    }
    require(fixed_conflict,"projected free-resource completion never relocates fixed interface pins");

    // Adjacent reservation boundaries may touch in projected mode; positive
    // area overlap consumes the conservative independent supply instead.
    std::string touching_text=design(0,3,3);
    const auto touching_site=touching_text.find("PLACED ( 180 100 )");
    require(touching_site!=std::string::npos,"locate touching reservation fixture");
    touching_text.replace(touching_site,18,"PLACED ( 170 100 )");
    DefParser touching;
    require(touching.parse(write(dir,"touching.def",touching_text),lef),"touching fixture DEF");
    TsvPolicy touching_policy(touching,lef,graph);
    require(touching_policy.enabled_slots()==3,"projected reservation edge touch is not a positive-area conflict");
    std::string overlap_text=touching_text;
    overlap_text.replace(touching_site,18,"PLACED ( 169 100 )");
    DefParser overlap;
    require(overlap.parse(write(dir,"overlap.def",overlap_text),lef),"overlap fixture DEF");
    TsvPolicy overlap_policy(overlap,lef,graph);
    require(overlap_policy.enabled_slots()==2,"positive reservation overlap disables a conflicting projected candidate");
    bool insufficient=false;
    try {overlap_policy.assign(overlap.nets(),thermal);} catch(const std::runtime_error& error) {
        insufficient=std::string(error.what()).find("geometry supply insufficient")!=std::string::npos;
    }
    require(insufficient,"overlapping resources cannot fabricate capacity");
}
void test_assignment_polish(const fs::path& dir) {
    LefParser lef;DefParser def;ThermalParser thermal;
    require(lef.parse(write(dir,"polish.lef",technology())),"polish LEF parsed");
    auto design=[](bool fixed) {
        std::string text="UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 400 200 ) ; GCELLGRID X 0 DO 41 STEP 10 ; GCELLGRID Y 0 DO 21 STEP 10 ; ";
        for (int l=1;l<=20;++l) text+="TRACKS X 1 DO 200 STEP 2 LAYER "+get_layer_name(l)+" ; TRACKS Y 1 DO 100 STEP 2 LAYER "+get_layer_name(l)+" ; ";
        text+="COMPONENTS 4 ; ";
        for (int i=0;i<4;++i) text+="- r"+std::to_string(i)+" RESOURCE_SLOT_top + PLACED ( "+std::to_string(i*100)+" 65 ) N ; ";
        text+="END COMPONENTS PINS 6 ; ";
        for (int i=0;i<3;++i) {
            const int x=i==0?125:(fixed&&i==1?135:135+i*5);
            const int bottom=fixed&&i==1?10:3,top=fixed&&i==1?11:14;
            for(int side=0;side<2;++side) text+="- p"+std::to_string(2*i+side)+" + LAYER "+get_layer_name(side?top:bottom)+" ( -1 -1 ) ( 1 1 ) + FIXED ( "+std::to_string(x)+" 100 ) N ; ";
        }
        text+="END PINS NETS 3 ; ";
        for(int i=0;i<3;++i) text+="- "+std::string(1,char('a'+i))+" ( PIN p"+std::to_string(2*i)+" ) ( PIN p"+std::to_string(2*i+1)+" ) ; ";
        return text+"END NETS END DESIGN";
    };
    for(bool fixed:{false,true}) {
        require(def.parse(write(dir,fixed?"polish-fixed.def":"polish.def",design(fixed)),lef),"polish DEF parsed");
        RoutingGraphOptions graph_options;graph_options.geometry=RoutingGeometryMode::GLOBAL_GUIDE;
        RoutingGraph graph(def,lef,graph_options);TsvPolicyOptions options;options.nearest_candidates=1;options.max_candidates=2;
        TsvPolicy baseline(def,lef,graph,options);const auto before=baseline.assign(def.nets(),thermal);
        options.assignment_polish=true;TsvPolicy improved(def,lef,graph,options);const auto after=improved.assign(def.nets(),thermal);
        std::set<int> occupied;
        auto distance=[&](const std::vector<int>& assignment,const TsvPolicy& policy) {
            double result=0;for(size_t i=0;i<assignment.size();++i) for(const auto& pin:def.nets()[i].pins) result+=pin.coord.distance_to(policy.slots()[assignment[i]].point);return result;
        };
        for(size_t i=0;i<after.size();++i) {
            require(occupied.insert(after[i]).second,"polish preserves one net per reservation");
            require(improved.compatible(def.nets()[i],after[i]),"polish retains fixed-pin compatibility");
        }
        for(int slot:occupied) for(int conflict:improved.slots()[slot].conflicts) require(!occupied.count(conflict),"polish retains conflict-free reservation geometry");
        require(distance(after,improved)<=distance(before,baseline),"assignment spatial cost never increases in uniform fixture");
        if(!fixed) {
            require(improved.assignment_polish_swaps()>0 && improved.assignment_polish_cost_reduction()>0,"bad cardinality allocation improves through a real swap");
            require(distance(after,improved)<distance(before,baseline),"swap strictly reduces independent terminal distance");
        } else require(improved.slots()[after[1]].point.distance_to({135,100})==0,"fixed interface remains at its original coordinate");
        require(after==improved.assign(def.nets(),thermal),"polished assignment is deterministic on replay");
        auto pareto_options=options;pareto_options.polish_thermal_pareto=true;
        TsvPolicy pareto(def,lef,graph,pareto_options);
        const auto pareto_after=pareto.assign(def.nets(),thermal);
        require(distance(pareto_after,pareto)<=distance(before,baseline),"Pareto rule does not worsen uniform-fixture geometry");
        require(pareto.assignment_polish_correlation_after()>=pareto.assignment_polish_correlation_before(),"Pareto rule preserves exact full-grid Pearson");
        for(size_t i=0;i<pareto_after.size();++i) require(pareto.compatible(def.nets()[i],pareto_after[i]),"Pareto preserves fixed interface compatibility");
        require(pareto_after==pareto.assign(def.nets(),thermal),"Pareto assignment replays deterministically");
    }
    auto hot_text=design(false);
    hot_text=hot_text.substr(0,hot_text.find("PINS 6"))+"PINS 2 ; - hb + LAYER metal3 ( -1 -1 ) ( 1 1 ) + FIXED ( 125 100 ) N ; - ht + LAYER metal14 ( -1 -1 ) ( 1 1 ) + FIXED ( 125 100 ) N ; END PINS NETS 1 ; - hot ( PIN hb ) ( PIN ht ) ; END NETS END DESIGN";
    require(def.parse(write(dir,"polish-heat.def",hot_text),lef),"heat tradeoff fixture parsed");
    std::ostringstream field;field<<"{\"meta\":{\"ambientTemp\":25},\"nodes\":[";
    bool first_sample=true;
    for(int row=0;row<20;++row) for(int col=0;col<40;++col) {
        if(!first_sample) field<<',';
        first_sample=false;
        field<<'['<<col+0.5<<','<<row+0.5<<",0,"<<(col<=3?100:25)<<']';
    }
    field<<"]}";
    require(thermal.parse_json(write(dir,"polish-heat.json",field.str()),def.gcell_grid(),def.dbu_per_micron(),def.die_area()),"complete uniform heat tradeoff field parsed");
    RoutingGraphOptions go;go.geometry=RoutingGeometryMode::GLOBAL_GUIDE;RoutingGraph graph(def,lef,go);
    TsvPolicyOptions weak;weak.assignment_polish=true;weak.nearest_candidates=1;weak.max_candidates=2;
    TsvPolicy a(def,lef,graph,weak);const auto cold=a.assign(def.nets(),thermal);
    auto strong=weak;strong.polish_heat_weight=32;TsvPolicy b(def,lef,graph,strong);const auto hot=b.assign(def.nets(),thermal);
    const auto p=a.slots()[cold[0]].point,q=b.slots()[hot[0]].point;
    require(thermal.get_temperature(graph.grid().get_col(q.x),graph.grid().get_row(q.y))>thermal.get_temperature(graph.grid().get_col(p.x),graph.grid().get_row(p.y)),"higher heat weight selects an actually warmer unused resource");
    require(q.distance_to(def.nets()[0].pins[0].coord)>p.distance_to(def.nets()[0].pins[0].coord),"heat knob explicitly permits a geometric tradeoff, not a universal score guarantee");
    require(b.compatible(def.nets()[0],hot[0]) && b.assignment_polish_moves()>0,"warm move retains compatibility and uses the dynamic free index");
    // Independent closed form: 10% of 800 thermal cells are equally hot;
    // one TSV on a hot cell gives 3/sqrt(799), a cold cell -1/(3*sqrt(799)).
    const double cold_rho=-1.0/(3.0*std::sqrt(799.0)),hot_rho=3.0/std::sqrt(799.0);
    require(std::abs(b.assignment_polish_correlation_before()-cold_rho)<1e-12,"policy initial counts agree with independent binary-field Pearson");
    require(std::abs(b.assignment_polish_correlation_after()-hot_rho)<1e-12,"accepted free move updates complete-grid Pearson exactly");

    auto conservative=strong;conservative.polish_thermal_pareto=true;
    TsvPolicy pareto(def,lef,graph,conservative);
    const auto unchanged=pareto.assign(def.nets(),thermal);
    require(pareto.slots()[unchanged[0]].point.distance_to(p)==0,"Pareto rejects hotter free resource when geometry worsens");
    require(pareto.assignment_polish_correlation_after()>=pareto.assignment_polish_correlation_before(),"Pareto heat coefficient never decreases");

    for(double invalid:{-1.0,std::numeric_limits<double>::infinity()}) {
        auto bad=weak;bad.polish_heat_weight=invalid;bool rejected=false;
        try {TsvPolicy::preflight(def,lef,bad);} catch(const std::runtime_error&) {rejected=true;}
        require(rejected,"invalid heat weights rejected before allocation");
    }

}

}
int main(int argc,char** argv) {
    try {
        if (argc!=3) throw std::runtime_error("usage: tsv_preflight_tests source_root output_directory");
        const fs::path work=argv[2];fs::create_directories(work);
        test_preflight(work);
        test_assignment_polish(work);
        LefParser lef;DefParser def;
        const fs::path sample=fs::path(argv[1])/"benchmarks/sample001";
        require(lef.parse((sample/"sample001.input.lef").string()),"parse unchanged sample LEF");
        require(def.parse((sample/"sample001.input.def").string(),lef),"parse unchanged sample DEF");
        TsvPolicy::preflight(def,lef);
        require(true,"old paired sample remains eligible");
        std::cout<<"TSV preflight tests passed: "<<checks<<" checks\n";return 0;
    } catch(const std::exception& e) {
        std::cerr<<"TSV preflight test failed: "<<e.what()<<"\n";return 1;
    }
}
