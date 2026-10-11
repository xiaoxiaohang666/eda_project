#include "parser/lef_parser.hpp"
#include "parser/def_parser.hpp"
#include "parser/thermal_parser.hpp"
#include "router/baseline_router.hpp"
#include "router/negotiated_router.hpp"
#include <filesystem>
#include <iostream>
#include <memory>

namespace {
void usage(const char* p) {
    std::cout<<"3D IC Global Router v3\nUsage: "<<p<<" --def FILE --lef FILE --thermal-report FILE --output FILE\n"
        <<"Input aliases: --input-def, --lefs (repeatable), --thermal-json, -o\n"
        <<"--router negotiated|baseline  (default negotiated)\n"
        <<"--rrr-rounds N  --tsv-rounds N  (defaults 30, 10)\n"
        <<"--search astar|dijkstra  --search-limit N (per search, default 250000)\n"
        <<"--time-limit SECONDS (default 120)  --local-repair 0|1 (default 1)\n"
        <<"--wirelength-rounds N (default 3)  --pin-local 0|1 (default 1)\n"
        <<"--tsv-local 0|1 (default 1)  Fixed-TSV terminal path recovery\n"
        <<"--stats FILE  Write metrics, TSV policy, assignment and search counters\n";
}
int integer(const std::string& value,bool positive=false) {
    size_t end=0;long n=std::stol(value,&end);
    if(end!=value.size()||n<(positive?1:0)||n>100000000) throw std::runtime_error("invalid integer option: "+value);
    return int(n);
}
bool same_file(const std::string& a,const std::string& b) {
    return std::filesystem::weakly_canonical(a)==std::filesystem::weakly_canonical(b)||
        (std::filesystem::exists(a)&&std::filesystem::exists(b)&&std::filesystem::equivalent(a,b));
}
}
int main(int argc,char** argv) {
    try {
        if(argc<2) {usage(argv[0]);return 1;}
        std::string def_file,thermal_file,output="output/result.guide",stats,kind="negotiated";
        std::vector<std::string> lefs;eda::RouterOptions options;
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];if(arg=="--help"||arg=="-h") {usage(argv[0]);return 0;}
            const std::set<std::string> recognized={"--def","--input-def","--lef","--lefs","--thermal-report","--thermal-json",
                "--output","-o","--stats","--router","--rrr-rounds","--tsv-rounds","--search","--time-limit","--search-limit","--local-repair","--wirelength-rounds","--pin-local","--tsv-local"};
            if(!recognized.count(arg)) throw std::runtime_error("unknown option: "+arg);
            if(i+1>=argc) throw std::runtime_error("missing option value: "+arg);
            const std::string value=argv[++i];if(value.empty()||value[0]=='-') throw std::runtime_error("invalid option value: "+arg);
            if(arg=="--def"||arg=="--input-def") def_file=value;
            else if(arg=="--lef"||arg=="--lefs") lefs.push_back(value);
            else if(arg=="--thermal-report"||arg=="--thermal-json") thermal_file=value;
            else if(arg=="--output"||arg=="-o") output=value;
            else if(arg=="--stats") stats=value;
            else if(arg=="--router") {if(value!="baseline"&&value!="negotiated") throw std::runtime_error("invalid router");kind=value;}
            else if(arg=="--rrr-rounds") options.rrr_rounds=integer(value);
            else if(arg=="--tsv-rounds") options.tsv_rounds=integer(value);
            else if(arg=="--wirelength-rounds") options.wirelength_rounds=integer(value);
            else if(arg=="--pin-local") {int v=integer(value);if(v>1) throw std::runtime_error("pin-local must be 0 or 1");options.pin_local=v;}
            else if(arg=="--tsv-local") {int v=integer(value);if(v>1) throw std::runtime_error("tsv-local must be 0 or 1");options.tsv_local=v;}
            else if(arg=="--search-limit") options.search_limit=size_t(integer(value,true));
            else if(arg=="--local-repair") {int v=integer(value);if(v>1) throw std::runtime_error("local-repair must be 0 or 1");options.local_repair=v;}
            else if(arg=="--search") {if(value!="astar"&&value!="dijkstra") throw std::runtime_error("invalid search");options.dijkstra=value=="dijkstra";}
            else if(arg=="--time-limit") {size_t end=0;options.time_limit=std::stod(value,&end);
                if(end!=value.size()||!std::isfinite(options.time_limit)||options.time_limit<=0) throw std::runtime_error("invalid time limit");}
        }
        if(def_file.empty()||lefs.empty()||thermal_file.empty()) throw std::runtime_error("required: --def --lef --thermal-report");
        std::vector<std::string> inputs=lefs;inputs.push_back(def_file);inputs.push_back(thermal_file);
        for(const auto& input:inputs) if(same_file(input,output)||(!stats.empty()&&same_file(input,stats)))
            throw std::runtime_error("output/stats cannot overwrite input: "+input);
        if(!stats.empty()&&same_file(output,stats)) throw std::runtime_error("stats and guide output must differ");
        if(kind=="baseline"&&!stats.empty()) throw std::runtime_error("--stats requires negotiated router");
        eda::LefParser lef;for(const auto& p:lefs) if(!lef.parse(p)) return 1;
        eda::DefParser def;if(!def.parse(def_file,lef)) return 1;
        eda::ThermalParser thermal;if(!thermal.parse_json(thermal_file,def.gcell_grid(),def.dbu_per_micron(),def.die_area())) return 1;
        if(kind=="baseline") {
            eda::BaselineRouter router(def,lef,thermal);if(!router.route_all()||!router.export_guide(output)) return 1;
        } else {
            eda::NegotiatedRouter router(def,lef,thermal,options);
            if(!router.route_all()||!router.export_guide(output)||(!stats.empty()&&!router.export_stats(stats))) return 1;
        }
        std::cout<<"[SUCCESS] Guide exported: "<<output<<"\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"[ERROR] "<<e.what()<<"\n";return 1;}
}
