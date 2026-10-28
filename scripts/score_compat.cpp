#include "objectives/official_metrics.hpp"
#include "scoring/compat_resources.hpp"
#include "scoring/compat_validation.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>

// Independent evaluator of serialized Guides. No router state or engineering
// TSV assignment restrictions are used. October 20 behavior is document-based;
// unresolved choices are explicit in every report.
namespace fs = std::filesystem;
using namespace eda;
namespace {
struct Options {
    std::string guide, def, thermal, output;
    std::vector<std::string> lefs;
    std::string runtime{"output/runtime/official-flute"}, profile{"20261020"};
    double zero_penalty{10000}, tsv_demand{1};
    OfficialMetricOptions metrics;
    compat::ResourceOptions resources;
    bool explicit_gain{false}, csv{true};
    std::optional<double> router_seconds, router_memory;
    bool legacy() const { return profile=="legacy-20261011"; }
};
std::string quote(const std::string& s) {
    std::ostringstream out;out<<'"';
    for(unsigned char c:s) {
        if(c=='"'||c=='\\')out<<'\\'<<c;
        else if(c<32)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c)<<std::dec;
        else out<<c;
    }
    out<<'"';return out.str();
}
std::string array(const std::vector<std::string>& values) {
    std::string out="[";bool first=true;
    for(const auto& v:values){if(!first)out+=",";first=false;out+=quote(v);}
    return out+"]";
}
double number(const std::string& s) {
    size_t used=0;const double v=std::stod(s,&used);
    if(used!=s.size()||!std::isfinite(v)||v<0)throw std::runtime_error("expected finite nonnegative number: "+s);
    return v;
}
Options parse(int argc,char** argv) {
    Options o;std::set<std::string> seen;
    for(int i=1;i<argc;++i) {
        const std::string key=argv[i];
        if(key=="--help") {
            std::cout<<"eda_score_compat GUIDE --lefs LEF [--lefs LEF] --input-def DEF\n"
                "  --thermal-json JSON --output-dir NEW_DIR [--official-runtime DIR]\n"
                "  [--profile 20261020|legacy-20261011] [--no-csv]\n"
                "  [--resource-profile 20261020|platform-20261024]\n"
                "  [--zero-capacity-penalty 10000] [--pin-layer-policy first|shape]\n"
                "  [--deduplicate-tracks 0|1] [--thermal-gain G]\n"
                "  [--tsv-cost-ratio 6] [--via-cost-ratio 3] [--tsv-congestion-ratio 1]\n"
                "  [--router-runtime-seconds S --router-peak-memory-bytes B]\n"
                "Document-based compatible evaluator; not the official program.\n"
                "Exit 0: local PASS, 1: S1/S4 FAIL, 2: input/runtime/evaluation ERROR.\n";
            std::exit(0);
        }
        if(!key.starts_with("--")) {
            if(!o.guide.empty())throw std::runtime_error("only one positional GUIDE is accepted");
            o.guide=key;continue;
        }
        if(key!="--lefs"&&!seen.insert(key).second)throw std::runtime_error("duplicate option: "+key);
        if(key=="--no-csv"){o.csv=false;continue;}
        if(i+1==argc)throw std::runtime_error("missing value for "+key);
        const std::string value=argv[++i];
        if(key=="--lefs")o.lefs.push_back(value);
        else if(key=="--input-def")o.def=value;
        else if(key=="--thermal-json")o.thermal=value;
        else if(key=="--output-dir")o.output=value;
        else if(key=="--official-runtime")o.runtime=value;
        else if(key=="--profile")o.profile=value;
        else if(key=="--resource-profile") {
            if(value!="20261020"&&value!="platform-20261024")
                throw std::runtime_error("resource-profile must be 20261020 or platform-20261024");
            o.resources.platform_20261024=value=="platform-20261024";
        }
        else if(key=="--zero-capacity-penalty")o.zero_penalty=number(value);
        else if(key=="--tsv-cost-ratio")o.metrics.tsv_cost_ratio=number(value);
        else if(key=="--via-cost-ratio")o.metrics.via_cost_ratio=number(value);
        else if(key=="--tsv-congestion-ratio")o.tsv_demand=number(value);
        else if(key=="--thermal-gain"){o.metrics.thermal_gain=number(value);o.explicit_gain=true;}
        else if(key=="--router-runtime-seconds")o.router_seconds=number(value);
        else if(key=="--router-peak-memory-bytes")o.router_memory=number(value);
        else if(key=="--pin-layer-policy") {
            if(value!="first"&&value!="shape")throw std::runtime_error("pin-layer-policy must be first or shape");
            o.resources.pin_first_layer=value=="first";
        } else if(key=="--deduplicate-tracks") {
            if(value!="0"&&value!="1")throw std::runtime_error("deduplicate-tracks must be 0 or 1");
            o.resources.deduplicate_tracks=value=="1";
        } else throw std::runtime_error("unknown option: "+key);
    }
    if(o.profile!="20261020"&&!o.legacy())throw std::runtime_error("unknown scoring profile");
    if(o.zero_penalty<=0)throw std::runtime_error("zero-capacity-penalty must be positive");
    if(o.router_seconds.has_value()!=o.router_memory.has_value())
        throw std::runtime_error("provide both router runtime and peak memory, or neither");
    if(o.legacy()&&(!o.resources.pin_first_layer||o.resources.deduplicate_tracks||o.resources.platform_20261024))
        throw std::runtime_error("resource policy overrides apply only to the 20261020 profile");
    if(o.resources.platform_20261024&&(!o.resources.pin_first_layer||o.resources.deduplicate_tracks))
        throw std::runtime_error("platform resources require first-layer macro PIN projection and repeated tracks");
    if(o.guide.empty()||o.lefs.empty()||o.def.empty()||o.thermal.empty()||o.output.empty())
        throw std::runtime_error("required: GUIDE --lefs --input-def --thermal-json --output-dir");
    if(!o.explicit_gain)o.metrics.thermal_gain=official_thermal_gain_for_def_file(o.def);
    return o;
}
void prepare_output(const Options& o) {
    std::vector<std::string> inputs=o.lefs;
    inputs.insert(inputs.end(),{o.guide,o.def,o.thermal});
    for(const auto& path:inputs)if(!fs::is_regular_file(path))
        throw std::runtime_error("missing input file: "+path);
    const fs::path output=fs::weakly_canonical(o.output);
    // All writes go into a new or empty directory. Never reuse old scores.
    if(fs::exists(output)&&(!fs::is_directory(output)||!fs::is_empty(output)))
        throw std::runtime_error("output-dir must be new or empty");
    fs::create_directories(output);
}
struct Tsv {std::string net;Point point;int col,row;};
struct Result {
    compat::ValidationResult validation;
    std::string status{"ERROR"}, message, s4{"NOT_EVALUATED"};
    bool s1_known{false}, s1_pass{false};
    double length{},reference{},ratio{},w{},c{},t{},quality{},correlation{};
    double overflow{},max_overflow{},q{},q_fixed{};
    size_t used{},overflow_entries{},zero_cells{},tsvs{},skipped_baselines{};
    size_t exempt_guide{},exempt_tsv{};
    int rows{},cols{};double dbu{},ambient{};size_t mapped{};
    std::vector<std::string> warnings;
};
void finite(double x,const char* what) {
    if(!std::isfinite(x))throw std::runtime_error(std::string("numeric overflow: ")+what);
}
std::ofstream file(const fs::path& path) {
    std::ofstream out(path);if(!out)throw std::runtime_error("cannot write "+path.string());
    out<<std::setprecision(17);return out;
}
void finished(std::ofstream& out) {
    out.flush();if(!out)throw std::runtime_error("failed writing evaluation artifact");
}
std::string csv_quote(const std::string& value) {
    std::string s="\"";for(char c:value){s+=c;if(c=='"')s+='"';}return s+'"';
}
void write_report(const Options& o,const Result& r) {
    auto out=file(fs::path(o.output)/"evaluation_report.json");
    out<<"{\n\"status\":"<<quote(r.status)<<",\n\"evaluator\":\"eda-score-compat\",\n"
       <<"\"compatibility\":{\"profile\":"<<quote(o.profile)
       <<",\"official_equivalence\":false,\"basis\":"
       <<quote(o.legacy()?"reproduction-of-archived-official-20261011":"20261020-documents-and-archived-official-unchanged-components")
       <<",\"zero_capacity_penalty\":"<<o.zero_penalty
       <<",\"resource_profile\":"<<quote(o.legacy()?"legacy-20261011":
            o.resources.platform_20261024?"platform-20261024":"20261020")
       <<",\"pin_layer_policy\":"<<quote(o.resources.pin_first_layer?"first":"shape")
       <<",\"deduplicate_tracks\":"<<(o.resources.deduplicate_tracks?"true":"false")
       <<",\"assumptions\":["
       <<quote("New official executable is unavailable; exact parity with the October 20 implementation is not established.")<<","
       <<quote("Zero-capacity occupied cells contribute a configurable fixed squared penalty; the official constant and demand dependence are unspecified.")<<","
       <<quote(o.resources.platform_20261024
           ?"Observed platform resources use half-open fixed-shape track intervals; capacity retains a closed final cell boundary. This inferred resource policy does not certify official legality."
           :"October 20 tracks use coordinate-sorted patterns, cell half-open intervals (last cell closed) and closed shape endpoints; duplicate patterns remain separate unless requested.")<<","
       <<quote(o.resources.platform_20261024
           ?"Macro PIN fixed demand projects onto its first geometry layer; own-net credit retains only original first-layer shapes. IO resources and credit retain original layers; multilayer IO platform behavior remains unconfirmed."
           :"Pin layer policy applies to fixed demand and own-net pin masks; multi-PORT/layer registration in the new official implementation is unconfirmed.")<<","
       <<quote("Direction size checks use original DEF first GCELLGRID STEP and raw rectangles; measurement clips to DIEAREA using archived official rules.")<<","
       <<quote("Native FLUTE, via accounting, TSV selection, thermal mapping and basename gain table follow the archived official implementation.")<<","
       <<quote("The documented 128 GB S4 limit is interpreted as 128000000000 bytes; the official decimal/binary unit convention is unconfirmed.")<<"]},\n"
       <<"\"inputs\":{\"guide\":"<<quote(fs::absolute(o.guide).string())
       <<",\"lefs\":"<<array(o.lefs)<<",\"defs\":["<<quote(fs::absolute(o.def).string())
       <<"],\"thermal_json\":"<<quote(fs::absolute(o.thermal).string())<<"},\n"
       <<"\"s1\":{\"status\":"<<quote(r.s1_known?(r.s1_pass?"PASS":"FAIL"):"NOT_EVALUATED")
       <<",\"value\":"<<(r.s1_known?(r.s1_pass?"1":"0"):"null")<<"},\n"
       <<"\"s4\":{\"status\":"<<quote(r.s4)<<",\"evidence\":\""
       <<(o.router_seconds?"caller-reported-router-resources":"not-derivable-from-guide")
       <<"\",\"runtime_limit_seconds\":43200,\"memory_limit_bytes\":128000000000"
       <<",\"router_runtime_seconds\":"<<(o.router_seconds?std::to_string(*o.router_seconds):"null")
       <<",\"router_peak_memory_bytes\":"<<(o.router_memory?std::to_string(*o.router_memory):"null")<<"},\n"
       <<"\"grid\":{\"dbu_per_micron\":"<<r.dbu<<",\"rows\":"<<r.rows<<",\"cols\":"<<r.cols
       <<",\"mapped_thermal_xy_nodes\":"<<r.mapped<<"},\n"
       <<"\"validation\":{\"checked_nets\":"<<r.validation.checked_nets
       <<",\"checked_pins\":"<<r.validation.checked_pins<<",\"checked_rectangles\":"<<r.validation.checked_rectangles
       <<",\"error_count\":"<<r.validation.error_count<<",\"errors\":"<<array(r.validation.errors)<<"},\n";
    if(r.status=="PASS") {
        out<<"\"wirelength\":{\"total_dbu\":"<<r.length<<",\"total_um\":"<<r.length/r.dbu
           <<",\"hpwl_reference_dbu\":"<<r.reference<<",\"normalized\":"<<r.ratio
           <<",\"tsv_count\":"<<r.tsvs<<",\"skipped_degree_limit_nets\":"<<r.skipped_baselines
           <<",\"via_cost_ratio\":"<<o.metrics.via_cost_ratio<<",\"tsv_cost_ratio\":"<<o.metrics.tsv_cost_ratio<<"},\n"
           <<"\"congestion\":{\"max_overflow\":"<<r.max_overflow<<",\"average_overflow\":"<<(r.used?r.overflow/r.used:0)
           <<",\"total_overflow\":"<<r.overflow<<",\"used_layer_gcells\":"<<r.used
           <<",\"overflow_entries\":"<<r.overflow_entries<<",\"zero_capacity_cells\":"<<r.zero_cells
           <<",\"squared_normalized_overflow\":"<<r.q<<",\"fixed_only_squared_normalized_overflow\":"<<r.q_fixed
           <<",\"own_pin_exempt_guide_entries\":"<<r.exempt_guide<<",\"own_pin_exempt_tsv_entries\":"<<r.exempt_tsv
           <<",\"tsv_congestion_ratio\":"<<o.tsv_demand<<"},\n"
           <<"\"thermal_tsv\":{\"synthetic\":false,\"ambient_temperature\":"<<r.ambient
           <<",\"tsv_points\":"<<r.tsvs<<",\"correlation_above_ambient\":"<<r.correlation
           <<",\"saturation_gain\":"<<o.metrics.thermal_gain<<",\"saturation_gain_source\":"
           <<quote(o.explicit_gain?"explicit-override":"archived-def-basename-table-or-default-20")<<"},\n"
           <<"\"score\":{\"wirelength_score\":"<<r.w<<",\"congestion_score\":"<<r.c<<",\"thermal_score\":"<<r.t
           <<",\"wirelength_quality\":"<<r.w/100<<",\"congestion_quality\":"<<r.c/100<<",\"thermal_quality\":"<<r.t/100
           <<",\"quality\":"<<r.quality<<",\"weights\":{\"wirelength\":0.45,\"congestion\":0.30,\"thermal_tsv\":0.25}},\n";
    } else out<<"\"score\":"<<(r.status=="FAIL"?"0":"null")<<",\n";
    out<<"\"message\":"<<quote(r.message)<<",\"warnings\":"<<array(r.warnings)<<"\n}\n";
    finished(out);
}
void run(const Options& o,Result& r) {
    LefParser lef;for(const auto& p:o.lefs)if(!lef.parse(p))throw std::runtime_error("LEF parse failed: "+p);
    DefParser def;if(!def.parse(o.def,lef,DefGridMapping::OFFICIAL_FULL))throw std::runtime_error("DEF parse failed");
    const auto& grid=def.gcell_grid();r.rows=grid.rows;r.cols=grid.cols;r.dbu=def.dbu_per_micron();
    GuideMap guides;
    try {guides=compat::read_guides(o.guide);}
    catch(const std::runtime_error& e) {
        r.s1_known=true;r.status="FAIL";r.message=e.what();r.validation.error_count=1;
        r.validation.errors.push_back(r.message);return;
    }
    r.validation=compat::validate(def,lef,guides,o.legacy());
    if(r.validation.error_count){r.s1_known=true;r.status="FAIL";r.message="Guide violates selected profile legality";return;}
    // TSV selection below is part of S1. Do not announce PASS if an earlier
    // input/runtime failure prevents that check from completing.
    if(o.router_seconds) {
        r.s4=(*o.router_seconds>43200||*o.router_memory>128000000000.0)?"FAIL":"PASS";
        if(r.s4=="FAIL"){r.status="FAIL";r.message="caller-reported router resources exceed S4 limits";return;}
    } else r.warnings.push_back("Router runtime and peak memory were not supplied; S4 has not been evaluated. PASS only covers local S1 and quality evaluation.");
    ThermalParser thermal;
    if(!thermal.parse_json(o.thermal,grid,r.dbu,def.die_area(),ThermalMapping::OFFICIAL_PREVIOUS,o.csv))
        throw std::runtime_error("thermal JSON parsing/mapping failed");
    r.ambient=thermal.ambient_temperature();r.mapped=thermal.mapped_nodes();
    std::optional<RoutingGraph> legacy_graph;
    std::optional<compat::Resources> resources;
    if(o.legacy())legacy_graph.emplace(def,lef,RoutingGraphOptions{RoutingGeometryMode::GLOBAL_GUIDE});
    else resources.emplace(def,lef,o.resources);
    NativeFluteBaseline flute(o.runtime);
    OfficialMetricOptions metrics=o.metrics;
    metrics.gcell_width_dbu=def.gcell_pitch_x();metrics.gcell_height_dbu=def.gcell_pitch_y();
    metrics.tsv_congestion_ratio=0; // Add potentially fractional TSV demand after per-net pin filtering.
    const size_t plane=size_t(grid.rows)*grid.cols;
    std::vector<double> demand(resources?resources->size():legacy_graph->size(),0);
    std::vector<int> tsv_counts(size_t(grid.rows)*grid.cols,0);
    std::vector<Tsv> tsvs;
    long double length=0,reference=0;
    for(const auto& net:def.nets()) {
        std::set<std::string> unique_pins;for(const auto& pin:net.pins)unique_pins.insert(pin.name);
        if(unique_pins.size()>size_t(flute.max_degree()))++r.skipped_baselines;
        const double base=flute.measure_net(net,std::max(metrics.gcell_width_dbu,metrics.gcell_height_dbu),metrics);
        finite(base,"FLUTE reference");
        NetGuideMetrics measured;
        try {measured=measure_official_net_guides(net,guides.at(net.name),grid,def.die_area(),lef,metrics,base);}
        catch(const std::runtime_error& e) {
            const std::string reason=e.what();
            const bool guide_error=reason.starts_with("official TSV")||
                reason.starts_with("official two-TSV")||reason.starts_with("official selected TSV")||
                reason.starts_with("net has no positive-area official guide");
            if(!guide_error)throw;
            r.s1_known=true;r.s1_pass=false;r.status="FAIL";r.message=net.name+": "+reason;
            ++r.validation.error_count;if(r.validation.errors.size()<50)r.validation.errors.push_back(r.message);
            return;
        }
        length+=measured.wirelength;reference+=base;
        const auto pins=resources?resources->pin_nodes(net):std::vector<int>{};
        const auto exempt=[&](int node){return std::binary_search(pins.begin(),pins.end(),node);};
        for(const auto& [node,amount]:measured.demand) {
            if(exempt(node)){++r.exempt_guide;continue;}
            demand[node]+=amount;
        }
        for(size_t i=0;i<measured.tsv_cells.size();++i) {
            const int cell=measured.tsv_cells[i],row=cell/grid.cols,col=cell%grid.cols;
            if(tsv_counts[cell]==std::numeric_limits<int>::max())throw std::runtime_error("TSV count overflow");
            ++tsv_counts[cell];++r.tsvs;tsvs.push_back({net.name,measured.tsv_points[i],col,row});
            for(int layer:{10,11}) {
                const int node=int(size_t(layer-1)*plane+size_t(row)*grid.cols+col);
                if(exempt(node)){++r.exempt_tsv;continue;}
                demand[node]+=o.tsv_demand;finite(demand[node],"TSV demand");
            }
        }
    }
    r.s1_known=true;r.s1_pass=true;
    r.length=double(length);r.reference=double(reference);finite(r.length,"wirelength");finite(r.reference,"reference");
    finite(r.length/r.dbu,"wirelength in microns");
    if(r.skipped_baselines)r.warnings.push_back("FLUTE baseline skipped "+std::to_string(r.skipped_baselines)+" nets with more than 150 unique pins, matching archived behavior.");
    if(r.reference<=0) {
        if(!o.legacy())throw std::runtime_error("zero total FLUTE reference: updated official convention is unspecified; refusing to fabricate a score");
        r.ratio=100;r.warnings.push_back("Zero reference uses archived fallback normalized=100.");
    } else r.ratio=r.length/r.reference;
    finite(r.ratio,"normalized wirelength");
    std::optional<std::ofstream> overflow_csv;
    if(o.csv) {
        overflow_csv.emplace(file(fs::path(o.output)/"overflow_gcells.csv"));
        *overflow_csv<<"gcell_col,gcell_row,layer,direction,demand,capacity,overflow\n";
    }
    const auto penalty=[&](double use,double cap)->long double {
        if(use<=0)return 0;
        if(cap<=0)return o.zero_penalty;
        const long double v=0.1L*std::max(0.0,use-cap)/cap;return v*v;
    };
    long double q=0,q_fixed=0,total_over=0;
    for(size_t n=0;n<demand.size();++n) {
        const int node=int(n);
        const double cap=resources?resources->capacity(node):legacy_graph->official_capacity(node);
        const double fixed=resources?resources->fixed_demand(node):legacy_graph->official_obstacle_demand(node);
        demand[n]+=fixed;finite(demand[n],"total demand");
        q_fixed+=penalty(fixed,cap);q+=penalty(demand[n],cap);
        if(demand[n]<=0)continue;
        ++r.used;if(cap<=0)++r.zero_cells;
        const double over=std::max(0.0,demand[n]-cap);total_over+=over;r.max_overflow=std::max(r.max_overflow,over);
        if(over<=0)continue;
        ++r.overflow_entries;
        if(overflow_csv) {
            const auto name=get_layer_name(int(n/plane)+1);
            *overflow_csv<<n%plane%grid.cols<<','<<n%plane/grid.cols<<','<<name<<','
                <<(lef.get_layer_direction(name)==Direction::HORIZONTAL?"HORIZONTAL":"VERTICAL")<<','
                <<demand[n]<<','<<cap<<','<<over<<'\n';
        }
    }
    if(overflow_csv)finished(*overflow_csv);
    r.q=double(q);r.q_fixed=double(q_fixed);r.overflow=double(total_over);
    finite(r.q,"congestion");finite(r.q_fixed,"fixed congestion");finite(r.overflow,"overflow");
    if(r.zero_cells)r.warnings.push_back("Zero-capacity penalty affects "+std::to_string(r.zero_cells)+" cells; its exact updated official definition is unconfirmed.");
    std::vector<double> heat(tsv_counts.size());bool positive_heat=false;
    for(int row=0;row<grid.rows;++row)for(int col=0;col<grid.cols;++col) {
        const double h=std::max(0.0,thermal.get_temperature(col,row)-r.ambient);
        heat[size_t(row)*grid.cols+col]=h;positive_heat|=h>0;
    }
    if(!r.tsvs)throw std::runtime_error("thermal TSV distribution is undefined without TSVs (archived official rejects it)");
    if(!positive_heat)throw std::runtime_error("thermal distribution requires positive temperature rise (archived official rejects it)");
    r.correlation=PearsonState(heat,tsv_counts).correlation();
    const double ratio=std::max(r.ratio,1e-9);
    r.w=100*(o.legacy()?std::min(ratio,1/ratio):std::min(1.0,1/ratio));
    r.c=100/std::sqrt(1+r.q);r.t=100*(0.5+0.5*std::tanh(metrics.thermal_gain*r.correlation));
    r.quality=.45*r.w+.30*r.c+.25*r.t;finite(r.quality,"quality");
    if(o.csv) {
        auto heat_csv=file(fs::path(o.output)/"heat_gcell.csv");
        auto tsv_csv=file(fs::path(o.output)/"tsv_gcell.csv");
        auto count_csv=file(fs::path(o.output)/"thermal_node_count_per_gcell.csv");
        for(int row=0;row<grid.rows;++row) {
            for(int col=0;col<grid.cols;++col) {
                if(col){heat_csv<<',';tsv_csv<<',';count_csv<<',';}
                heat_csv<<thermal.get_temperature(col,row);tsv_csv<<tsv_counts[size_t(row)*grid.cols+col];
                count_csv<<thermal.node_counts().at(row).at(col);
            }
            heat_csv<<'\n';tsv_csv<<'\n';count_csv<<'\n';
        }
        finished(heat_csv);finished(tsv_csv);finished(count_csv);
        auto points=file(fs::path(o.output)/"tsv_points.csv");
        points<<"net,x_dbu,y_dbu,x_um_local,y_um_local,gcell_col,gcell_row,bottom_layer,top_layer,source\n";
        for(const auto& t:tsvs)points<<csv_quote(t.net)<<','<<t.point.x<<','<<t.point.y<<','
            <<(t.point.x-def.die_area().x1)/r.dbu<<','<<(t.point.y-def.die_area().y1)/r.dbu<<','
            <<t.col<<','<<t.row<<",metal10,metal11,selected-positive-overlap\n";
        finished(points);
    }
    r.status="PASS";r.message="Local compatible evaluation completed; see compatibility assumptions and S4 status.";
}
}
int main(int argc,char** argv) {
    std::optional<Options> options;bool prepared=false;Result result;
    try {
        options=parse(argc,argv);prepare_output(*options);prepared=true;
        run(*options,result);write_report(*options,result);
    } catch(const std::exception& error) {
        result.status="ERROR";result.message=error.what();
        std::cerr<<"[ERROR] "<<error.what()<<'\n';
        if(prepared)try{write_report(*options,result);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';}
    }
    std::cout<<result.status;
    if(result.status=="PASS")std::cout<<" quality="<<std::setprecision(12)<<result.quality;
    std::cout<<'\n';
    return result.status=="PASS"?0:result.status=="FAIL"?1:2;
}
