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
    std::cout<<"3D IC Global Router v5\nUsage: "<<p<<" --def FILE --lef FILE --thermal-report FILE --output FILE\n"
        <<"Input aliases: --input-def, --lefs (repeatable), --thermal-json, -o\n"
        <<"--router negotiated|baseline  (default negotiated)\n"
        <<"--rrr-rounds N  --tsv-rounds N  (defaults 30, 10)\n"
        <<"--rrr-score-aware 0|1 (default 0; official movable-demand congestion priority)\n"
        <<"--rrr-net-limit N (default 128; 1..8192, requires rrr-score-aware 1)\n"
        <<"--large-recovery 0|1 (default 0; bounded fixed-TSV Guide wirelength recovery, official only)\n"
        <<"--large-recovery-nets N (1..4096, default 1024) --large-recovery-rounds N (1..2, default 1)\n"
        <<"--large-recovery-seconds N (0..3600 exclusive of zero, default 300; additional phase budget)\n"
        <<"--large-recovery-branches N (0..8, default 8; zero disables shared branch candidates)\n"
        <<"--search astar|dijkstra  --search-limit N (per search, default 250000)\n"
        <<"--time-limit SECONDS (default 120; scales with large net count)  --local-repair 0|1 (default 1)\n"
        <<"--wirelength-rounds N (default 3)  --pin-local 0|1 (default 1)\n"
        <<"--tsv-local 0|1 (default 1)  Fixed-TSV terminal path recovery\n"
        <<"--tsv-resource-mode auto|paired|projected (default auto) --tsv-candidates N --tsv-max-candidates N\n"
        <<"--cell-pin-access 0|1 --fast-initial 0|1 (defaults 1)\n"
        <<"--fast-quality 0|1 (default 1; bounded official fast tree/Z improvements)\n"
        <<"--fast-layer-pruning 0|1 (default 1; equivalent pruning of dominated L candidates)\n"
        <<"--model physical|official (default physical; official matches new score)\n"
        <<"--official-score-pricing 0|1 (default 0; derivative-scaled W/C congestion prices)\n"
        <<"--official-runtime DIR --nonpreferred 0|1 (official Guide model)\n"
        <<"--tsv-assignment-polish 0|1 (default 0; projected resource move/swap proxy optimization)\n"
        <<"--tsv-polish-thermal weighted|pareto (default weighted; pareto requires official model)\n"
        <<"--tsv-polish-heat-weight NUMBER (default 2; experimental proxy, requires polish 1)\n"
        <<"--tsv-polish-candidates N (default 0: inherit tsv-candidates; max 65536)\n"
        <<"--tsv-polish-passes N (default 2; range 1..64) --tsv-polish-bidirectional 0|1 (default 0)\n"
        <<"--tsv-polish-thermal-candidates 0|1 (default 0; thermal-aware free-slot KD candidates)\n"
        <<"--tsv-block-refine 0|1 (default 0; occupied-slot cost-improving permutations, official only)\n"
        <<"--tsv-block-size N (3..64, default 16) --tsv-block-passes N (1..16, default 2)\n"
        <<"--tsv-block-max-groups N (1..65536) --tsv-block-max-evals N (1..100000000)\n"
        <<"--tsv-block-seconds N (>0..600, default 60)\n"
        <<"--v5-pin-rooted 0|1 (default 0; fixed-TSV pin-rooted shared tree candidates, official only)\n"
        <<"--v5-topology-max-pins N (3..128, default 32)\n"
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
        std::vector<std::string> lefs;eda::RouterOptions options;bool explicit_time=false,explicit_polish_heat=false,explicit_polish_thermal=false,explicit_polish_budget=false,explicit_rrr_limit=false,explicit_recovery_budget=false;
        bool explicit_block_budget=false,explicit_topology_budget=false;
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];if(arg=="--help"||arg=="-h") {usage(argv[0]);return 0;}
            const std::set<std::string> recognized={"--def","--input-def","--lef","--lefs","--thermal-report","--thermal-json",
                "--output","-o","--stats","--router","--rrr-rounds","--rrr-score-aware","--rrr-net-limit","--tsv-rounds","--search","--time-limit","--search-limit","--local-repair","--wirelength-rounds","--pin-local","--tsv-local","--tsv-resource-mode","--tsv-candidates","--tsv-max-candidates","--cell-pin-access","--fast-initial","--fast-quality","--fast-layer-pruning","--model","--official-runtime","--official-score-pricing","--nonpreferred","--tsv-assignment-polish","--tsv-polish-heat-weight","--tsv-polish-thermal","--tsv-polish-candidates","--tsv-polish-passes","--tsv-polish-bidirectional","--tsv-polish-thermal-candidates"};
            const std::set<std::string> recovery_options={"--large-recovery","--large-recovery-nets","--large-recovery-rounds","--large-recovery-seconds","--large-recovery-branches"};
            const std::set<std::string> v5_options={"--tsv-block-refine","--tsv-block-size","--tsv-block-passes","--tsv-block-max-groups","--tsv-block-max-evals","--tsv-block-seconds","--v5-pin-rooted","--v5-topology-max-pins"};
            if(!recognized.count(arg)&&!recovery_options.count(arg)&&!v5_options.count(arg)) throw std::runtime_error("unknown option: "+arg);
            if(i+1>=argc) throw std::runtime_error("missing option value: "+arg);
            const std::string value=argv[++i];if(value.empty()||value[0]=='-') throw std::runtime_error("invalid option value: "+arg);
            if(arg=="--def"||arg=="--input-def") def_file=value;
            else if(arg=="--lef"||arg=="--lefs") lefs.push_back(value);
            else if(arg=="--thermal-report"||arg=="--thermal-json") thermal_file=value;
            else if(arg=="--output"||arg=="-o") output=value;
            else if(arg=="--stats") stats=value;
            else if(arg=="--router") {if(value!="baseline"&&value!="negotiated") throw std::runtime_error("invalid router");kind=value;}
            else if(arg=="--rrr-rounds") options.rrr_rounds=integer(value);
            else if(v5_options.count(arg)) {
                if(arg=="--tsv-block-seconds") {
                    size_t end=0;const double v=std::stod(value,&end);
                    if(end!=value.size()||!std::isfinite(v)||v<=0||v>600)
                        throw std::runtime_error("tsv-block-seconds must be >0 and <=600");
                    options.tsv_policy.block_max_seconds=v;explicit_block_budget=true;
                } else {
                    const int v=integer(value);
                    if(arg=="--tsv-block-refine"||arg=="--v5-pin-rooted") {
                        if(v>1) throw std::runtime_error("v5 boolean option must be 0 or 1");
                        if(arg=="--tsv-block-refine") options.tsv_policy.assignment_block_refine=v;
                        else options.v5_pin_rooted=v;
                    } else if(arg=="--v5-topology-max-pins") {
                        if(v<3||v>128) throw std::runtime_error("v5-topology-max-pins must be 3..128");
                        options.v5_topology_max_pins=size_t(v);explicit_topology_budget=true;
                    } else {
                        explicit_block_budget=true;
                        if(arg=="--tsv-block-size") {
                            if(v<3||v>64) throw std::runtime_error("tsv-block-size must be 3..64");
                            options.tsv_policy.block_size=size_t(v);
                        } else if(arg=="--tsv-block-passes") {
                            if(v<1||v>16) throw std::runtime_error("tsv-block-passes must be 1..16");
                            options.tsv_policy.block_passes=size_t(v);
                        } else if(arg=="--tsv-block-max-groups") {
                            if(v<1||v>65536) throw std::runtime_error("tsv-block-max-groups must be 1..65536");
                            options.tsv_policy.block_max_groups=size_t(v);
                        } else {
                            if(v<1) throw std::runtime_error("tsv-block-max-evals must be positive");
                            options.tsv_policy.block_max_cost_evaluations=size_t(v);
                        }
                    }
                }
            }
            else if(recovery_options.count(arg)) {
                if(arg=="--large-recovery-seconds") {
                    size_t end=0;const double v=std::stod(value,&end);
                    if(end!=value.size()||!std::isfinite(v)||v<=0||v>3600) throw std::runtime_error("large-recovery-seconds must be >0 and <=3600");
                    options.large_recovery_seconds=v;explicit_recovery_budget=true;
                } else {
                    const int v=integer(value);
                    if(arg=="--large-recovery") {if(v>1) throw std::runtime_error("large-recovery must be 0 or 1");options.large_recovery=v;}
                    else {
                        explicit_recovery_budget=true;
                        if(arg=="--large-recovery-nets") {if(v<1||v>4096) throw std::runtime_error("large-recovery-nets must be 1..4096");options.large_recovery_nets=size_t(v);}
                        else if(arg=="--large-recovery-rounds") {if(v<1||v>2) throw std::runtime_error("large-recovery-rounds must be 1..2");options.large_recovery_rounds=v;}
                        else {if(v>8) throw std::runtime_error("large-recovery-branches must be 0..8");options.large_recovery_branches=v;}
                    }
                }
            }
            else if(arg=="--rrr-score-aware") {
                const int v=integer(value);if(v>1) throw std::runtime_error("rrr-score-aware must be 0 or 1");
                options.rrr_score_aware=v;
            }
            else if(arg=="--rrr-net-limit") {
                explicit_rrr_limit=true;const int v=integer(value,true);
                if(v>8192) throw std::runtime_error("rrr-net-limit must be 1..8192");
                options.rrr_net_limit=size_t(v);
            }
            else if(arg=="--tsv-rounds") options.tsv_rounds=integer(value);
            else if(arg=="--wirelength-rounds") options.wirelength_rounds=integer(value);
            else if(arg=="--pin-local") {int v=integer(value);if(v>1) throw std::runtime_error("pin-local must be 0 or 1");options.pin_local=v;}
            else if(arg=="--tsv-local") {int v=integer(value);if(v>1) throw std::runtime_error("tsv-local must be 0 or 1");options.tsv_local=v;}
            else if(arg=="--tsv-resource-mode") {
                if(value=="auto") options.tsv_policy.resource_mode=eda::TsvResourceMode::AUTO;
                else if(value=="paired") options.tsv_policy.resource_mode=eda::TsvResourceMode::PAIRED;
                else if(value=="projected") options.tsv_policy.resource_mode=eda::TsvResourceMode::PROJECTED;
                else throw std::runtime_error("invalid TSV resource mode");
            }
            else if(arg=="--tsv-polish-thermal") {
                explicit_polish_thermal=true;
                if(value!="weighted" && value!="pareto") throw std::runtime_error("invalid TSV polish thermal rule");
                options.tsv_policy.polish_thermal_pareto=value=="pareto";
            }
            else if(arg=="--tsv-polish-heat-weight") {explicit_polish_heat=true;size_t end=0;double v=std::stod(value,&end);if(end!=value.size()||!std::isfinite(v)||v<0||v>1000000) throw std::runtime_error("invalid TSV polish heat weight");options.tsv_policy.polish_heat_weight=v;}
            else if(arg=="--tsv-assignment-polish") {int v=integer(value);if(v>1) throw std::runtime_error("tsv-assignment-polish must be 0 or 1");options.tsv_policy.assignment_polish=v;}
            else if(arg=="--tsv-polish-candidates") {
                explicit_polish_budget=true;int v=integer(value);
                if(v>65536) throw std::runtime_error("tsv-polish-candidates must be 0..65536");
                options.tsv_policy.polish_candidates=size_t(v);
            }
            else if(arg=="--tsv-polish-passes") {
                explicit_polish_budget=true;int v=integer(value,true);
                if(v>64) throw std::runtime_error("tsv-polish-passes must be 1..64");
                options.tsv_policy.polish_passes=v;
            }
            else if(arg=="--tsv-polish-bidirectional") {
                explicit_polish_budget=true;int v=integer(value);
                if(v>1) throw std::runtime_error("tsv-polish-bidirectional must be 0 or 1");
                options.tsv_policy.polish_bidirectional=v;
            }
            else if(arg=="--tsv-polish-thermal-candidates") {
                explicit_polish_budget=true;const int v=integer(value);
                if(v>1) throw std::runtime_error("tsv-polish-thermal-candidates must be 0 or 1");
                options.tsv_policy.polish_thermal_candidates=v;
            }
            else if(arg=="--tsv-candidates") options.tsv_policy.nearest_candidates=size_t(integer(value,true));
            else if(arg=="--tsv-max-candidates") options.tsv_policy.max_candidates=size_t(integer(value,true));
            else if(arg=="--cell-pin-access"||arg=="--fast-initial"||arg=="--fast-quality"||arg=="--fast-layer-pruning") {
                int v=integer(value);if(v>1) throw std::runtime_error("boolean option must be 0 or 1");
                if(arg=="--cell-pin-access") options.cell_pin_access=v;
                else if(arg=="--fast-initial") options.fast_initial=v;
                else if(arg=="--fast-quality") options.fast_quality=v;else options.fast_layer_pruning=v;
            }
            else if(arg=="--model") {
                if(value!="physical" && value!="official") throw std::runtime_error("invalid model");
                options.official_model=value=="official";
            }
            else if(arg=="--official-runtime") options.official_runtime=value;
            else if(arg=="--official-score-pricing") {
                const int v=integer(value);if(v>1) throw std::runtime_error("official-score-pricing must be 0 or 1");
                options.official_score_pricing=v;
            }
            else if(arg=="--nonpreferred") {int v=integer(value);if(v>1) throw std::runtime_error("nonpreferred must be 0 or 1");options.graph_options.allow_nonpreferred=v;}
            else if(arg=="--search-limit") options.search_limit=size_t(integer(value,true));
            else if(arg=="--local-repair") {int v=integer(value);if(v>1) throw std::runtime_error("local-repair must be 0 or 1");options.local_repair=v;}
            else if(arg=="--search") {if(value!="astar"&&value!="dijkstra") throw std::runtime_error("invalid search");options.dijkstra=value=="dijkstra";}
            else if(arg=="--time-limit") {explicit_time=true;size_t end=0;options.time_limit=std::stod(value,&end);
                if(end!=value.size()||!std::isfinite(options.time_limit)||options.time_limit<=0) throw std::runtime_error("invalid time limit");}
        }
        if(explicit_polish_heat&&!options.tsv_policy.assignment_polish)
            throw std::runtime_error("--tsv-polish-heat-weight requires --tsv-assignment-polish 1");
        if(explicit_polish_thermal&&!options.tsv_policy.assignment_polish)
            throw std::runtime_error("--tsv-polish-thermal requires --tsv-assignment-polish 1");
        if(explicit_polish_budget&&!options.tsv_policy.assignment_polish)
            throw std::runtime_error("TSV polish budgets require --tsv-assignment-polish 1");
        if(options.tsv_policy.polish_thermal_pareto&&!options.official_model)
            throw std::runtime_error("Pareto thermal rule requires --model official");
        if(explicit_polish_heat&&options.tsv_policy.polish_thermal_pareto)
            throw std::runtime_error("--tsv-polish-heat-weight requires weighted thermal rule");
        if(options.tsv_policy.polish_thermal_candidates && options.tsv_policy.polish_thermal_pareto)
            throw std::runtime_error("thermal candidates require weighted thermal rule");
        if(options.rrr_score_aware && !options.official_model)
            throw std::runtime_error("score-aware RRR requires --model official");
        if(options.official_score_pricing && !options.official_model)
            throw std::runtime_error("score pricing requires --model official");
        if(explicit_rrr_limit && !options.rrr_score_aware)
            throw std::runtime_error("--rrr-net-limit requires --rrr-score-aware 1");
        if(options.large_recovery && !options.official_model)
            throw std::runtime_error("large recovery requires --model official");
        if(explicit_recovery_budget && !options.large_recovery)
            throw std::runtime_error("large recovery budgets require --large-recovery 1");
        if((options.tsv_policy.assignment_block_refine||options.v5_pin_rooted)&&!options.official_model)
            throw std::runtime_error("v5 assignment/topology optimization requires --model official");
        if(explicit_block_budget&&!options.tsv_policy.assignment_block_refine)
            throw std::runtime_error("TSV block budgets require --tsv-block-refine 1");
        if(explicit_topology_budget&&!options.v5_pin_rooted)
            throw std::runtime_error("v5 topology budget requires --v5-pin-rooted 1");
        if(def_file.empty()||lefs.empty()||thermal_file.empty()) throw std::runtime_error("required: --def --lef --thermal-report");
        std::vector<std::string> inputs=lefs;inputs.push_back(def_file);inputs.push_back(thermal_file);
        for(const auto& input:inputs) if(same_file(input,output)||(!stats.empty()&&same_file(input,stats)))
            throw std::runtime_error("output/stats cannot overwrite input: "+input);
        if(!stats.empty()&&same_file(output,stats)) throw std::runtime_error("stats and guide output must differ");
        if(kind=="baseline"&&!stats.empty()) throw std::runtime_error("--stats requires negotiated router");
        eda::LefParser lef;for(const auto& p:lefs) if(!lef.parse(p)) return 1;
        eda::DefParser def;if(!def.parse(def_file,lef,options.official_model?eda::DefGridMapping::OFFICIAL_FULL:eda::DefGridMapping::CLIPPED_PHYSICAL)) return 1;
        if(!explicit_time && def.nets().size()>1000) options.time_limit=std::clamp(def.nets().size()*0.03,120.0,7200.0);
        eda::ThermalParser thermal;if(!thermal.parse_json(thermal_file,def.gcell_grid(),def.dbu_per_micron(),def.die_area(),options.official_model?eda::ThermalMapping::OFFICIAL_PREVIOUS:eda::ThermalMapping::LEGACY_BILINEAR)) return 1;
        if(thermal.interpolated_cells())
            std::cerr<<"[WARN] Missing GCell temperatures were locally interpolated; the fixed evaluator does not implement this mapping.\n";
        if(options.official_model) {
            if(kind!="negotiated") throw std::runtime_error("official model requires negotiated router");
            options.graph_options.geometry=eda::RoutingGeometryMode::GLOBAL_GUIDE;
            options.official_metrics.thermal_gain=eda::official_thermal_gain_for_def_file(def_file);
            options.official_metrics.gcell_width_dbu=def.gcell_pitch_x();
            options.official_metrics.gcell_height_dbu=def.gcell_pitch_y();
            if(options.official_runtime.empty()) options.official_runtime="output/runtime/official-flute";
        }
        if(kind=="baseline") {
            eda::BaselineRouter router(def,lef,thermal);if(!router.route_all()||!router.export_guide(output)) return 1;
        } else {
            // Resolve the TSV policy before allocating a potentially huge graph.
            eda::TsvPolicy::preflight(def,lef,options.tsv_policy);
            eda::NegotiatedRouter router(def,lef,thermal,options);
            if(!router.route_all()||!router.export_guide(output)||(!stats.empty()&&!router.export_stats(stats))) return 1;
        }
        std::cout<<"[SUCCESS] Guide exported: "<<output<<"\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"[ERROR] "<<e.what()<<"\n";return 1;}
}
