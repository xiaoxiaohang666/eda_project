// Read-only saved-Guide analysis; this does not route or call any scorer/FLUTE.
// Build with C++20/O2, -Isrc, build/libeda_core.a and -ldl; no CMake changes.
#include "objectives/official_metrics.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

using namespace eda;
namespace fs=std::filesystem;
namespace {
struct Options {std::vector<fs::path> lefs;fs::path def,guide,csv,output;};
std::string quote(const std::string& s) {
    std::ostringstream out;out<<'"';
    for(unsigned char c:s) {
        if(c=='"'||c=='\\') out<<'\\'<<c;
        else if(c<32) out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<int(c)<<std::dec;
        else out<<c;
    }
    out<<'"';return out.str();
}
std::string csv_quote(const std::string& s) {
    std::string out="\"";for(char c:s){if(c=='"')out+='"';out+=c;}return out+'"';
}
long double risk(int demand,double capacity) {
    if(demand<0 || capacity<0) throw std::runtime_error("invalid demand/capacity");
    if(capacity==0) {
        if(demand) throw std::runtime_error("positive demand at zero capacity is outside verified official domain");
        return 0;
    }
    const long double ratio=.1L*std::max(0,demand-int(capacity))/capacity;
    return ratio*ratio;
}
double score(long double squared) {return 100/std::sqrt(1+std::max(0.L,squared));}
void self_test() {
    auto near=[](long double a,long double b){if(std::abs(a-b)>1e-15L)throw std::runtime_error("risk self-test failed");};
    near(risk(20,10),.01L);near(risk(8,10),0);
    near(risk(14,10)-risk(12,10),.0012L);
    near(risk(14,10)-risk(13,10),.0007L);
    bool rejected=false;try{(void)risk(1,0);}catch(const std::exception&){rejected=true;}
    if(!rejected)throw std::runtime_error("zero-capacity self-test failed");
    std::cout<<"PASS risk decomposition, exact joint deletion, marginal deletion, zero-capacity boundary\n";
}
Options options(int argc,char** argv) {
    Options o;
    for(int i=1;i<argc;++i) {
        const std::string key=argv[i];
        if(key=="--help") {
            std::cout<<"--lef FILE (repeatable) --def FILE --guide FILE --official-overflow CSV --output-dir NEW\n"
                <<"or --self-test. No thermal/FLUTE needed: only congestion is measured.\n";std::exit(0);
        }
        if(i+1>=argc)throw std::runtime_error("missing value: "+key);
        const auto value=fs::weakly_canonical(argv[++i]);
        if(key=="--lef")o.lefs.push_back(value);else if(key=="--def")o.def=value;
        else if(key=="--guide")o.guide=value;else if(key=="--official-overflow")o.csv=value;
        else if(key=="--output-dir")o.output=value;else throw std::runtime_error("unknown option: "+key);
    }
    if(o.lefs.empty()||o.def.empty()||o.guide.empty()||o.csv.empty()||o.output.empty())
        throw std::runtime_error("required: --lef --def --guide --official-overflow --output-dir");
    auto inputs=o.lefs;inputs.insert(inputs.end(),{o.def,o.guide,o.csv});
    for(const auto& path:inputs) {
        if(!fs::is_regular_file(path))throw std::runtime_error("missing input: "+path.string());
        const auto relative=o.output.lexically_relative(path.parent_path());
        if(!relative.empty() && *relative.begin()!="..")throw std::runtime_error("output must be outside input directories");
    }
    if(fs::exists(o.output))throw std::runtime_error("output directory must be new");
    return o;
}
struct Footprint {
    std::vector<std::pair<int,int>> demand;
    std::vector<int> pin_locked,rrr_locked;
    size_t rectangles{0};double wirelength{0},raw_priority{0};
    long double full_relief{0},rrr_relief{0};
    size_t overflow_cells{0},pin_overflow_cells{0},rrr_removable_overflow_cells{0};
};
std::vector<int> unique_pin_cells(const Net& net,const RoutingGraph& graph,size_t& uncertain) {
    std::vector<int> locked;
    for(const auto& pin:net.pins) {
        std::set<int> possible;
        for(const auto& shape:pin.port_rects) {
            if(shape.layer_idx<1||shape.layer_idx>20)continue;
            const auto& d=graph.die_area();const auto& lines=graph.grid();
            const double x1=std::max(shape.rect.x1,d.x1),x2=std::min(shape.rect.x2,d.x2);
            const double y1=std::max(shape.rect.y1,d.y1),y2=std::min(shape.rect.y2,d.y2);
            if(x1>=x2||y1>=y2)continue;
            const int cb=std::max(0,int(std::upper_bound(lines.x_lines.begin(),lines.x_lines.end(),x1)-lines.x_lines.begin())-1);
            const int ce=std::min(lines.cols,int(std::lower_bound(lines.x_lines.begin(),lines.x_lines.end(),x2)-lines.x_lines.begin()));
            const int rb=std::max(0,int(std::upper_bound(lines.y_lines.begin(),lines.y_lines.end(),y1)-lines.y_lines.begin())-1);
            const int re=std::min(lines.rows,int(std::lower_bound(lines.y_lines.begin(),lines.y_lines.end(),y2)-lines.y_lines.begin()));
            for(int r=rb;r<re;++r)for(int c=cb;c<ce;++c)possible.insert(graph.node(c,r,shape.layer_idx));
        }
        if(possible.size()==1)locked.push_back(*possible.begin());else ++uncertain;
    }
    std::sort(locked.begin(),locked.end());locked.erase(std::unique(locked.begin(),locked.end()),locked.end());return locked;
}
struct Layer {
    uint64_t used{0},over_cells{0},fixed{0},guides{0},tsvs{0},pins{0},rrr_locked{0},overflow{0};
    long double total{0},fixed_only{0},no_tsv{0},pin_floor{0},rrr_floor{0};
};
void add(Layer& a,const Layer& b) {
    a.used+=b.used;a.over_cells+=b.over_cells;a.fixed+=b.fixed;a.guides+=b.guides;a.tsvs+=b.tsvs;
    a.pins+=b.pins;a.rrr_locked+=b.rrr_locked;a.overflow+=b.overflow;
    a.total+=b.total;a.fixed_only+=b.fixed_only;a.no_tsv+=b.no_tsv;a.pin_floor+=b.pin_floor;a.rrr_floor+=b.rrr_floor;
}
std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> result;std::istringstream in(line);std::string field;
    while(std::getline(in,field,','))result.push_back(field);
    return result;
}
size_t verify_csv(const fs::path& path,const RoutingGraph& graph,const std::vector<int>& total,size_t expected) {
    std::ifstream in(path);std::string line;std::getline(in,line);
    if(line.find("gcell_col,gcell_row,layer,direction,demand,capacity,overflow")!=0)
        throw std::runtime_error("unrecognized official overflow CSV header");
    std::vector<unsigned char> seen(total.size());size_t rows=0;
    while(std::getline(in,line)) {
        if(line.empty())continue;
        const auto f=split(line);if(f.size()!=7)throw std::runtime_error("malformed official CSV row");
        const int node=graph.node(std::stoi(f[0]),std::stoi(f[1]),get_layer_index(f[2]));
        if(node<0||size_t(node)>=total.size()||seen[node]++)throw std::runtime_error("invalid/duplicate CSV cell");
        const double cap=graph.official_capacity(node);const int over=std::max(0,total[node]-int(cap));
        if(std::stoi(f[4])!=total[node]||std::stod(f[5])!=cap||std::stoi(f[6])!=over||over<=0)
            throw std::runtime_error("CSV demand/capacity/overflow mismatch at node "+std::to_string(node));
        ++rows;
    }
    if(rows!=expected)throw std::runtime_error("CSV row count differs from all reconstructed overflow cells");
    return rows;
}
int removable(const Footprint& net,int node,int demand,bool rrr) {
    return rrr && std::binary_search(net.rrr_locked.begin(),net.rrr_locked.end(),node)?0:demand;
}
void analyze(const Options& o) {
    const auto start=std::chrono::steady_clock::now();
    LefParser lef;for(const auto& path:o.lefs)if(!lef.parse(path.string()))throw std::runtime_error("LEF parse failed");
    DefParser def;if(!def.parse(o.def.string(),lef,DefGridMapping::OFFICIAL_FULL))throw std::runtime_error("DEF parse failed");
    RoutingGraphOptions go;go.geometry=RoutingGeometryMode::GLOBAL_GUIDE;RoutingGraph graph(def,lef,go);
    OfficialMetricOptions mo;mo.gcell_width_dbu=def.gcell_pitch_x();mo.gcell_height_dbu=def.gcell_pitch_y();
    std::unordered_map<std::string,size_t> index;
    for(size_t i=0;i<def.nets().size();++i)index.emplace(def.nets()[i].name,i);
    std::vector<Footprint> nets(def.nets().size());std::vector<unsigned char> seen(nets.size());
    std::vector<int> guides(graph.size()),tsvs(graph.size()),pins(graph.size()),locked(graph.size()),total(graph.size());
    size_t uncertain_pins=0,tsv_count=0,rectangles=0,pin_floor_mismatch=0;
    std::ifstream in(o.guide);std::string name,token;
    while(in>>name) {
        const auto found=index.find(name);
        if(found==index.end()||seen[found->second]++)throw std::runtime_error("unknown/duplicate Guide net: "+name);
        if(!(in>>token)||token!="(")throw std::runtime_error("missing Guide '('");
        std::vector<GuideRect> raw;bool ended=false;
        while(in>>token) {
            if(token==")"){ended=true;break;}
            size_t used=0;GuideRect r;r.x1=std::stod(token,&used);
            if(used!=token.size()||!(in>>r.y1>>r.x2>>r.y2>>r.layer))throw std::runtime_error("malformed Guide rectangle");
            raw.push_back(r);
        }
        if(!ended||raw.empty())throw std::runtime_error("unterminated/empty Guide net");
        const size_t ni=found->second;const auto& source=def.nets()[ni];auto& n=nets[ni];
        auto measured=measure_official_net_guides(source,raw,graph,lef,mo,0);
        n.rectangles=raw.size();rectangles+=raw.size();n.wirelength=measured.wirelength;
        n.demand=std::move(measured.demand);n.pin_locked=unique_pin_cells(source,graph,uncertain_pins);
        n.rrr_locked=n.pin_locked;
        for(int cell:measured.tsv_cells) {
            ++tsv_count;
            for(int layer:{10,11}) {
                const int node=graph.node(cell%graph.grid().cols,cell/graph.grid().cols,layer);
                ++tsvs[node];n.rrr_locked.push_back(node);
            }
        }
        std::sort(n.rrr_locked.begin(),n.rrr_locked.end());
        n.rrr_locked.erase(std::unique(n.rrr_locked.begin(),n.rrr_locked.end()),n.rrr_locked.end());
        for(int node:n.pin_locked) {
            const auto it=std::lower_bound(n.demand.begin(),n.demand.end(),std::pair(node,0));
            if(it==n.demand.end()||it->first!=node)++pin_floor_mismatch;
            ++pins[node];
        }
        for(int node:n.rrr_locked)++locked[node];
        for(auto [node,count]:n.demand)guides[node]+=count;
    }
    if(!in.eof()||std::find(seen.begin(),seen.end(),0)!=seen.end())throw std::runtime_error("Guide net set does not equal DEF");
    if(pin_floor_mismatch)throw std::runtime_error("single-cell pin requirement disagrees with actual Guide; lower-bound proof failed");
    std::array<Layer,21> layers;Layer overall;
    std::vector<int> hot;
    std::map<std::tuple<int,int,int>,Layer> regions;
    for(size_t node=0;node<graph.size();++node) {
        guides[node]-=tsvs[node];const int fixed=graph.official_obstacle_demand(int(node));
        if(guides[node]<0||locked[node]>guides[node])throw std::runtime_error("Guide/TSV/required demand decomposition failed");
        total[node]=fixed+guides[node]+tsvs[node];const double cap=graph.official_capacity(int(node));
        Layer s;s.fixed=fixed;s.guides=guides[node];s.tsvs=tsvs[node];s.pins=pins[node];s.rrr_locked=locked[node];
        s.used=total[node]>0;s.overflow=std::max(0,total[node]-int(cap));s.over_cells=s.overflow>0;
        s.total=risk(total[node],cap);s.fixed_only=risk(fixed,cap);s.no_tsv=risk(fixed+guides[node],cap);
        s.pin_floor=risk(fixed+pins[node],cap);s.rrr_floor=risk(fixed+locked[node]+tsvs[node],cap);
        add(layers[graph.layer(int(node))],s);add(overall,s);
        if(s.over_cells) {hot.push_back(int(node));add(regions[{graph.layer(int(node)),graph.col(int(node))/16,graph.row(int(node))/16}],s);}
    }
    const size_t csv_rows=verify_csv(o.csv,graph,total,overall.over_cells);
    for(size_t ni=0;ni<nets.size();++ni) {
        auto& n=nets[ni];
        for(auto [node,count]:n.demand) {
            const double cap=graph.official_capacity(node);const int over=std::max(0,total[node]-int(cap));
            if(!over)continue;
            ++n.overflow_cells;n.raw_priority+=double(over)*count;
            const auto base=risk(total[node],cap);n.full_relief+=base-risk(total[node]-count,cap);
            if(std::binary_search(n.pin_locked.begin(),n.pin_locked.end(),node))++n.pin_overflow_cells;
            const int movable=removable(n,node,count,true);
            if(movable)++n.rrr_removable_overflow_cells;
            n.rrr_relief+=base-risk(total[node]-movable,cap);
        }
    }
    fs::create_directories(o.output);
    auto file=[&](const char* name){std::ofstream f(o.output/name);f.exceptions(std::ios::badbit|std::ios::failbit);f<<std::setprecision(17);return f;};
    auto layer_out=file("layers.csv");
    layer_out<<"layer,used_cells,overflow_cells,overflow,fixed_demand,guide_demand,tsv_extra_demand,pin_required_guide_demand,rrr_required_guide_demand,total_squared,fixed_only_squared,no_tsv_squared,pin_floor_squared,fixed_tsv_rrr_floor_squared,total_share,guide_increment,tsv_increment\n";
    for(int l=1;l<=20;++l) {const auto& s=layers[l];layer_out<<l<<','<<s.used<<','<<s.over_cells<<','<<s.overflow<<','<<s.fixed<<','<<s.guides<<','<<s.tsvs<<','<<s.pins<<','<<s.rrr_locked<<','<<double(s.total)<<','<<double(s.fixed_only)<<','<<double(s.no_tsv)<<','<<double(s.pin_floor)<<','<<double(s.rrr_floor)<<','<<double(s.total/overall.total)<<','<<double(s.no_tsv-s.fixed_only)<<','<<double(s.total-s.no_tsv)<<'\n';}
    const auto hot_key=[&](int n){return risk(total[n],graph.official_capacity(n));};
    std::partial_sort(hot.begin(),hot.begin()+std::min<size_t>(200,hot.size()),hot.end(),[&](int a,int b){return hot_key(a)>hot_key(b);});
    if(hot.size()>200)hot.resize(200);
    std::unordered_set<int> hot_set(hot.begin(),hot.end());std::unordered_map<int,std::vector<size_t>> owners;
    for(size_t ni=0;ni<nets.size();++ni)for(auto [node,count]:nets[ni].demand){(void)count;if(hot_set.count(node))owners[node].push_back(ni);}
    auto hotspots=file("hotspots.csv");hotspots<<"layer,col,row,x_dbu,y_dbu,capacity,fixed,guide,tsv_extra,pin_required,rrr_required,overflow,squared,fixed_only_squared,fixed_tsv_rrr_floor_squared,owner_count,first_12_owners\n";
    for(int n:hot) {
        std::string names;for(size_t k=0;k<std::min<size_t>(12,owners[n].size());++k){if(k)names+=';';names+=def.nets()[owners[n][k]].name;}
        const auto p=graph.point(n);const auto f=graph.official_obstacle_demand(n);const auto c=graph.official_capacity(n);
        hotspots<<graph.layer(n)<<','<<graph.col(n)<<','<<graph.row(n)<<','<<p.x<<','<<p.y<<','<<c<<','<<f<<','<<guides[n]<<','<<tsvs[n]<<','<<pins[n]<<','<<locked[n]<<','<<std::max(0,total[n]-int(c))<<','<<double(risk(total[n],c))<<','<<double(risk(f,c))<<','<<double(risk(f+locked[n]+tsvs[n],c))<<','<<owners[n].size()<<','<<csv_quote(names)<<'\n';
    }
    auto region_out=file("regions.csv");region_out<<"layer,tile_col16,tile_row16,overflow_cells,overflow,total_squared,fixed_only_squared,fixed_tsv_rrr_floor_squared\n";
    for(const auto& [k,s]:regions) {auto [l,c,r]=k;region_out<<l<<','<<c<<','<<r<<','<<s.over_cells<<','<<s.overflow<<','<<double(s.total)<<','<<double(s.fixed_only)<<','<<double(s.rrr_floor)<<'\n';}
    std::array<std::vector<size_t>,3> rankings;
    for(auto& rank:rankings){rank.resize(nets.size());std::iota(rank.begin(),rank.end(),0);}
    auto order=[&](const auto& value){return [&,value](size_t a,size_t b){if(value(a)!=value(b))return value(a)>value(b);if(def.nets()[a].pins.size()!=def.nets()[b].pins.size())return def.nets()[a].pins.size()<def.nets()[b].pins.size();return def.nets()[a].name<def.nets()[b].name;};};
    std::stable_sort(rankings[0].begin(),rankings[0].end(),order([&](size_t n){return nets[n].raw_priority;}));
    std::stable_sort(rankings[1].begin(),rankings[1].end(),order([&](size_t n){return nets[n].full_relief;}));
    std::stable_sort(rankings[2].begin(),rankings[2].end(),order([&](size_t n){return nets[n].rrr_relief;}));
    const std::array<const char*,3> names{"raw_guide_overflow_proxy","exact_full_net_marginal","exact_rrr_removable_marginal"};
    auto net_out=file("top_nets.csv");net_out<<"ranking,rank,net,pins,cross_die,guide_rectangles,demand_cells,wirelength_dbu,raw_guide_priority,full_delete_relief,rrr_removable_delete_relief,overflow_cells,pin_required_overflow_cells,rrr_removable_overflow_cells\n";
    for(size_t k=0;k<rankings.size();++k)for(size_t i=0;i<std::min<size_t>(1024,nets.size());++i){const size_t ni=rankings[k][i];const auto& n=nets[ni];net_out<<names[k]<<','<<i+1<<','<<csv_quote(def.nets()[ni].name)<<','<<def.nets()[ni].pins.size()<<','<<def.nets()[ni].is_cross_die<<','<<n.rectangles<<','<<n.demand.size()<<','<<n.wirelength<<','<<n.raw_priority<<','<<double(n.full_relief)<<','<<double(n.rrr_relief)<<','<<n.overflow_cells<<','<<n.pin_overflow_cells<<','<<n.rrr_removable_overflow_cells<<'\n';}
    auto budgets=file("budget_effects.csv");budgets<<"ranking,deletion_model,nets,exact_joint_squared_relief,fraction_total_squared,fraction_above_fixed_tsv_rrr_floor,remaining_congestion_score,congestion_score_increase,weighted_score_gain_if_W_T_unchanged\n";
    for(size_t k=0;k<rankings.size();++k)for(bool rrr:{false,true}) {
        std::vector<int> removed(graph.size());long double relief=0;size_t cursor=0;
        for(size_t target:{size_t(32),size_t(128),size_t(256),size_t(384),size_t(512),size_t(1024),size_t(4096)}) {
            const size_t limit=std::min(target,nets.size());
            for(;cursor<limit;++cursor) {const auto& n=nets[rankings[k][cursor]];
                for(auto [node,count]:n.demand){const int delta=removable(n,node,count,rrr);if(!delta)continue;const auto cap=graph.official_capacity(node);relief+=risk(total[node]-removed[node],cap)-risk(total[node]-removed[node]-delta,cap);removed[node]+=delta;}}
            const auto new_score=score(overall.total-relief),gain=new_score-score(overall.total);
            budgets<<names[k]<<','<<(rrr?"preserve_single_cell_pins_and_current_TSV_landing":"delete_entire_net_even_pins_TSV")<<','<<limit<<','<<double(relief)<<','<<double(relief/overall.total)<<','<<double(relief/std::max(1e-30L,overall.total-overall.rrr_floor))<<','<<new_score<<','<<gain<<','<<.30*gain<<'\n';
        }
    }
    auto summary=file("summary.json");
    summary<<"{\n\"scope\":\"read-only own official congestion reconstruction, checked against saved original CSV\",\n"
        <<"\"warning\":\"Deletion effects are optimistic upper bounds, not feasible reroutes. Pin lower bound covers only single-cell PORT choices; TSV lower bound assumes current assigned landing is fixed. Raw Guide priority omits physical edge-usage priority unavailable in serialized Guide.\",\n"
        <<"\"def\":"<<quote(o.def.string())<<",\n\"guide\":"<<quote(o.guide.string())<<",\n\"official_overflow_csv\":"<<quote(o.csv.string())<<",\n"
        <<"\"nets\":"<<nets.size()<<",\n\"rectangles\":"<<rectangles<<",\n\"tsvs\":"<<tsv_count<<",\n\"graph_nodes\":"<<graph.size()<<",\n\"grid_cols\":"<<graph.grid().cols<<",\n\"grid_rows\":"<<graph.grid().rows<<",\n"
        <<"\"official_csv_rows_checked\":"<<csv_rows<<",\n\"official_csv_mismatches\":0,\n\"uncertain_multicell_or_missing_PORT_pins\":"<<uncertain_pins<<",\n"
        <<"\"total_squared_overflow\":"<<double(overall.total)<<",\n\"total_overflow\":"<<overall.overflow<<",\n\"congestion_score\":"<<score(overall.total)<<",\n"
        <<"\"fixed_OBS_PIN_only_squared\":"<<double(overall.fixed_only)<<",\n\"fixed_OBS_PIN_only_C_ceiling\":"<<score(overall.fixed_only)<<",\n"
        <<"\"ordinary_guide_increment_after_fixed\":"<<double(overall.no_tsv-overall.fixed_only)<<",\n\"TSV_extra_increment_after_guides\":"<<double(overall.total-overall.no_tsv)<<",\n"
        <<"\"single_cell_pin_lower_bound_squared\":"<<double(overall.pin_floor)<<",\n\"single_cell_pin_lower_bound_C_ceiling\":"<<score(overall.pin_floor)<<",\n"
        <<"\"fixed_TSV_RRR_lower_bound_squared\":"<<double(overall.rrr_floor)<<",\n\"fixed_TSV_RRR_C_ceiling\":"<<score(overall.rrr_floor)<<",\n"
        <<"\"seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<"\n}\n";
    std::cout<<"PASS saved original CSV rows="<<csv_rows<<" nets="<<nets.size()<<" C="<<score(overall.total)<<" fixed-only C ceiling="<<score(overall.fixed_only)<<" pin/current-TSV C ceiling="<<score(overall.rrr_floor)<<'\n';
}
}
int main(int argc,char** argv) {
    try {if(argc==2&&std::string(argv[1])=="--self-test")self_test();else analyze(options(argc,argv));return 0;}
    catch(const std::exception& e){std::cerr<<"analyze_official_congestion: "<<e.what()<<'\n';return 1;}
}
