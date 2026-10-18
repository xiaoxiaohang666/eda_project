#include "validation/guide_validator.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

using namespace eda;
namespace fs=std::filesystem;
namespace {
size_t checks=0;
void require(bool value,const std::string& reason) {
    ++checks;if (!value) throw std::runtime_error(reason);
}
std::string write(const fs::path& dir,const std::string& name,const std::string& text) {
    const auto path=dir/name;std::ofstream out(path);out<<text;out.close();
    require(bool(out),"could not write single-net validator fixture");return path.string();
}
std::string technology() {
    std::string text;
    for (int layer=1;layer<=20;++layer) {
        const auto name=get_layer_name(layer);
        text+="LAYER "+name+" TYPE ROUTING ; DIRECTION "+(layer%2?"HORIZONTAL":"VERTICAL")+
            " ; WIDTH 0.2 ; END "+name+"\n";
    }
    return text+"MACRO CELL CLASS CORE ; SIZE 10 BY 10 ; PIN P PORT LAYER metal1 ; "
        "RECT 1 1 3 3 ; END END P END CELL\n";
}
std::string design() {
    return "UNITS DISTANCE MICRONS 10 ; DIEAREA ( 0 0 ) ( 400 300 ) ; "
        "GCELLGRID X 0 DO 5 STEP 100 ; GCELLGRID Y 0 DO 4 STEP 100 ; "
        "COMPONENTS 1 ; - u CELL + FIXED ( 20 20 ) FN ; END COMPONENTS "
        "PINS 3 ; - local_out + LAYER metal3 ( -1 -1 ) ( 1 1 ) + FIXED ( 250 40 ) N ; "
        "- cross_bottom + LAYER metal10 ( -1 -1 ) ( 1 1 ) + FIXED ( 50 150 ) N ; "
        "- cross_top + LAYER metal12 ( -1 -1 ) ( 1 1 ) + FIXED ( 250 150 ) N ; END PINS "
        "NETS 2 ; - local ( u P ) ( PIN local_out ) ; "
        "- cross ( PIN cross_bottom ) ( PIN cross_top ) ; END NETS END DESIGN";
}
GuideMap valid_guides() {
    return {{"local",{{95,35,175,45,"metal1"},{170,35,180,45,"metal2"},{175,35,255,45,"metal3"}}},
            {"cross",{{45,145,155,155,"metal10"},{150,145,160,155,"metal11"},{155,145,255,155,"metal12"}}}};
}
const Net& named(const DefParser& def,const std::string& name) {
    for (const auto& net:def.nets()) if (net.name==name) return net;
    throw std::runtime_error("missing fixture net");
}
// Each local mutation is checked through the direct API and the original
// whole-map entry point. Expected diagnostics pin the pre-extraction order.
void equivalent(const DefParser& def,const LefParser& lef,const std::string& name,
                const std::vector<GuideRect>& candidate,bool expected,const std::string& diagnostic) {
    auto complete=valid_guides();complete.at(name)=candidate;
    std::string local_error="stale local error",whole_error="stale whole error";
    const bool local=validate_net_guides(named(def,name),def.die_area(),lef,candidate,local_error);
    const bool whole=validate_guides(def,lef,complete,whole_error);
    require(local==expected && whole==expected,"single-net and whole-map status agree for "+name+": "+diagnostic);
    require(local_error==diagnostic && whole_error==diagnostic,"single-net and whole-map preserve diagnostic: "+diagnostic);
}
void tests(const DefParser& def,const LefParser& lef) {
    const auto guides=valid_guides();const auto& local=named(def,"local");const auto& cross=named(def,"cross");
    require(local.pins.front().coord.x==100 && local.pins.front().coord.y==40,"fixture uses the real FN-transformed macro terminal");
    require(!local.is_cross_die && cross.is_cross_die,"fixture parser identifies both die domains");
    for (const auto& net:def.nets()) equivalent(def,lef,net.name,guides.at(net.name),true,"");
    auto boundary=guides.at("local");boundary.front().y1=40;
    equivalent(def,lef,"local",boundary,true,"");
    auto ordinary_touch=guides.at("local");ordinary_touch[1].x1=175;ordinary_touch[1].x2=185;ordinary_touch[2].x1=185;
    equivalent(def,lef,"local",ordinary_touch,false,"local: disconnected rectangles");
    auto ordinary_corner=guides.at("local");ordinary_corner[1]={175,45,185,55,"metal2"};
    equivalent(def,lef,"local",ordinary_corner,false,"local: disconnected rectangles");
    std::vector<GuideRect> same_layer_edge={{95,35,170,45,"metal1"},{170,35,180,45,"metal1"},
        {175,40,185,50,"metal2"},{180,35,255,45,"metal3"}};
    equivalent(def,lef,"local",same_layer_edge,true,"");
    auto same_layer_corner=same_layer_edge;same_layer_corner[1].y1=45;same_layer_corner[1].y2=55;
    equivalent(def,lef,"local",same_layer_corner,false,"local: disconnected rectangles");
    auto duplicate=guides.at("cross");duplicate.push_back(duplicate.front());
    equivalent(def,lef,"cross",duplicate,true,"");
    equivalent(def,lef,"local",{},false,"missing guide for local");
    equivalent(def,lef,"cross",{},false,"missing guide for cross");
    auto wrong=guides.at("local");wrong.front().layer="metal99";
    equivalent(def,lef,"local",wrong,false,"local: unknown routing layer");
    wrong=guides.at("local");wrong.front().x1=95.5;
    equivalent(def,lef,"local",wrong,false,"local: non-integer/non-finite guide coordinate");
    for (const double invalid:{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN(),9007199254740992.0}) {
        wrong=guides.at("local");wrong.front().x1=invalid;
        equivalent(def,lef,"local",wrong,false,"local: non-integer/non-finite guide coordinate");
    }
    wrong=guides.at("local");wrong.front().x2=wrong.front().x1;
    equivalent(def,lef,"local",wrong,false,"local: invalid/out-of-die rectangle");
    wrong=guides.at("local");wrong.front().x1=180;
    equivalent(def,lef,"local",wrong,false,"local: invalid/out-of-die rectangle");
    wrong=guides.at("local");wrong.front().x1=-1;
    equivalent(def,lef,"local",wrong,false,"local: invalid/out-of-die rectangle");
    wrong=guides.at("local");wrong.front().layer="metal11";
    equivalent(def,lef,"local",wrong,false,"local: intra-die net uses the other die");
    wrong=guides.at("local");wrong.front().x1=101;
    equivalent(def,lef,"local",wrong,false,"local: uncovered pin u.P");
    wrong=guides.at("local");wrong.front().layer="metal2";
    equivalent(def,lef,"local",wrong,false,"local: uncovered pin u.P");
    wrong=guides.at("local");wrong.erase(wrong.begin()+1);
    equivalent(def,lef,"local",wrong,false,"local: disconnected rectangles");
    wrong=guides.at("local");wrong.push_back({300,200,310,210,"metal1"});
    equivalent(def,lef,"local",wrong,false,"local: disconnected rectangles");
    wrong=guides.at("cross");wrong.front().x2=150;
    equivalent(def,lef,"cross",wrong,false,"cross: disconnected rectangles");
    wrong=guides.at("cross");wrong.front().x2=150;wrong.front().y2=150;wrong[1].y1=150;wrong[1].y2=160;
    equivalent(def,lef,"cross",wrong,false,"cross: disconnected rectangles");
    std::string error="stale";auto missing=guides;missing.erase("cross");
    require(!validate_guides(def,lef,missing,error) && error=="guide/DEF Net set mismatch","whole-map size check keeps its precedence over missing lookup");
    missing=guides;auto entry=missing.extract("cross");entry.key()="unknown_net";missing.insert(std::move(entry));
    require(!validate_guides(def,lef,missing,error) && error=="missing guide for cross","equal-size mismatched set retains its missing-net diagnostic");
    auto extra=guides;extra.emplace("unknown_net",guides.at("local"));
    require(!validate_guides(def,lef,extra,error) && error=="guide/DEF Net set mismatch","unexpected extra net is rejected before per-net validation");
    DefParser empty;
    require(!validate_guides(empty,lef,{},error) && error=="invalid/empty design","whole-map design validation stays outside the local helper");
    // A copied Net can explicitly require an interface even when its remaining
    // rectangles all lie on one die. Preserve the original final TSV guard.
    auto required=local;required.is_cross_die=true;
    require(!validate_net_guides(required,def.die_area(),lef,guides.at("local"),error) &&
        error=="local: no positive-area M10/M11 connection","the final positive-area TSV requirement remains explicit");
    auto top=local;top.pins={cross.pins.back()};top.is_cross_die=false;
    require(!validate_net_guides(top,def.die_area(),lef,guides.at("local"),error) &&
        error=="local: intra-die net uses the other die","top-only nets retain their die restriction");
    require(validate_guides(def,lef,guides,error) && error.empty(),"successful whole validation clears a prior error");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"usage: net_validator_tests artifact-directory");
        const fs::path dir=argv[1];fs::create_directories(dir);LefParser lef;DefParser def;
        require(lef.parse(write(dir,"validator.lef",technology())) &&
            def.parse(write(dir,"validator.def",design()),lef),"single-net fixture parses");
        tests(def,lef);std::cout<<"Single-net validator checks: "<<checks<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
