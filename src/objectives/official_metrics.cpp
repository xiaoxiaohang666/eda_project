#include "objectives/official_metrics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <tuple>
#include <unordered_set>
#ifdef __linux__
#include <dlfcn.h>
#endif

namespace eda {
namespace {
using Key = std::tuple<int,double,double,double,double>;
using Overlap = std::tuple<double,double,double,double,double>;
void check_options(const OfficialMetricOptions& o) {
    for (double x : {o.tsv_cost_ratio,o.via_cost_ratio,o.thermal_gain})
        if (!std::isfinite(x) || x < 0) throw std::runtime_error("invalid official metric ratio/gain");
    if (o.tsv_congestion_ratio < 0 || !std::isfinite(o.gcell_width_dbu) ||
        !std::isfinite(o.gcell_height_dbu) || o.gcell_width_dbu <= 0 || o.gcell_height_dbu <= 0)
        throw std::runtime_error("official metrics require positive original GCELLGRID X/Y STEP and nonnegative integer TSV demand ratio");
}
bool positive_overlap(const GuideRect& a,const GuideRect& b) {
    return std::min(a.x2,b.x2)>std::max(a.x1,b.x1) && std::min(a.y2,b.y2)>std::max(a.y1,b.y1);
}
bool positive_overlap(const GuideRect& a,const Rect& b) {
    return std::min(a.x2,b.x2)>std::max(a.x1,b.x1) && std::min(a.y2,b.y2)>std::max(a.y1,b.y1);
}
std::vector<GuideRect> clean_guides(const std::vector<GuideRect>& raw,const Rect& die) {
    std::vector<GuideRect> unique;
    std::set<Key> seen;
    for (const auto& g:raw) {
        const int l=get_layer_index(g.layer);
        if (!l || !std::isfinite(g.x1)||!std::isfinite(g.y1)||!std::isfinite(g.x2)||!std::isfinite(g.y2)||
            g.x1>g.x2 || g.y1>g.y2)
            throw std::runtime_error("invalid rectangle for official metrics");
        // Official trim_rectangle uses int() after clipping (truncation to zero).
        GuideRect c{std::trunc(std::max(g.x1,die.x1)),
                    std::trunc(std::max(g.y1,die.y1)),
                    std::trunc(std::min(g.x2,die.x2)),
                    std::trunc(std::min(g.y2,die.y2)),g.layer};
        if (c.x1>=c.x2 || c.y1>=c.y2) continue;
        if(seen.emplace(l,c.x1,c.y1,c.x2,c.y2).second) unique.push_back(c);
    }
    std::vector<GuideRect> result;
    for(size_t i=0;i<unique.size();++i) {
        bool contained=false;
        for(size_t j=0;j<unique.size();++j) if(i!=j && unique[i].layer==unique[j].layer &&
            unique[j].x1<=unique[i].x1 && unique[j].y1<=unique[i].y1 &&
            unique[j].x2>=unique[i].x2 && unique[j].y2>=unique[i].y2) {contained=true;break;}
        if(!contained) result.push_back(unique[i]);
    }
    if(result.empty()) throw std::runtime_error("net has no positive-area official guide after clipping");
    return result;
}
std::pair<int,int> interval(double lo,double hi,const std::vector<double>& lines) {
    const bool point=std::abs(hi-lo)<=1e-9*std::max(std::abs(lo),std::abs(hi));
    if(point) {
        int n=int(std::upper_bound(lines.begin(),lines.end(),lo)-lines.begin())-1;
        n=std::clamp(n,0,int(lines.size())-2);return {n,n+1};
    }
    return {std::max(0,int(std::upper_bound(lines.begin(),lines.end(),lo)-lines.begin())-1),
            std::min(int(lines.size())-1,int(std::lower_bound(lines.begin(),lines.end(),hi)-lines.begin()))};
}
std::vector<int> cells(const GuideRect& g,const RoutingGraph& graph) {
    const auto& grid=graph.grid();const auto [cb,ce]=interval(g.x1,g.x2,grid.x_lines);
    const auto [rb,re]=interval(g.y1,g.y2,grid.y_lines);
    std::vector<int> nodes;nodes.reserve(size_t(std::max(0,ce-cb))*std::max(0,re-rb));
    for(int r=rb;r<re;++r)for(int c=cb;c<ce;++c)nodes.push_back(graph.node(c,r,get_layer_index(g.layer)));
    return nodes;
}
std::vector<Rect> crossing_ports(const Net& net,const std::vector<GuideRect>& guides) {
    std::vector<Rect> result;
    for(const auto& pin:net.pins) {
        bool found=false;
        for(const auto& shape:pin.port_rects) {
            for(const auto& g:guides) if(get_layer_index(g.layer)==shape.layer_idx && positive_overlap(g,shape.rect)) {
                if(shape.layer_idx==10 || shape.layer_idx==11)result.push_back(shape.rect);
                found=true;break;
            }
            if(found)break;
        }
        if(!found && pin.port_rects.empty()) {
            // Pins constructed by callers without PORT metadata retain a point
            // witness. Real DEF pins always supply positive-area port shapes.
            for(const auto& g:guides) if(g.layer==pin.layer &&
                pin.coord.x>=g.x1 && pin.coord.x<=g.x2 && pin.coord.y>=g.y1 && pin.coord.y<=g.y2) {
                if(pin.layer_idx==10 || pin.layer_idx==11)
                    result.emplace_back(pin.coord.x-1e-9,pin.coord.y-1e-9,pin.coord.x+1e-9,pin.coord.y+1e-9);
                break;
            }
        }
    }
    return result;
}
Point select_tsv(const Net& net,const std::vector<GuideRect>& guides) {
    std::set<Overlap> candidates;
    for(const auto& a:guides)if(get_layer_index(a.layer)==10)
        for(const auto& b:guides)if(get_layer_index(b.layer)==11) {
            const double x1=std::max(a.x1,b.x1),y1=std::max(a.y1,b.y1),x2=std::min(a.x2,b.x2),y2=std::min(a.y2,b.y2);
            if(x1<x2 && y1<y2)candidates.emplace((x2-x1)*(y2-y1),x1,y1,x2,y2);
        }
    if(candidates.empty() || candidates.size()>2)
        throw std::runtime_error("official TSV overlap candidate count is outside 1..2 for "+net.name);
    Overlap chosen=*candidates.begin();
    if(candidates.size()==2) {
        const auto ports=crossing_ports(net,guides);
        if(ports.empty())throw std::runtime_error("official two-TSV candidates have no cross-interface pin for "+net.name);
        std::vector<Overlap> without_all_pins;
        for(const auto& candidate:candidates) {
            auto [area,x1,y1,x2,y2]=candidate;(void)area;
            bool all=true;
            for(const auto& p:ports)
                if(!(std::min(x2,p.x2)>std::max(x1,p.x1) && std::min(y2,p.y2)>std::max(y1,p.y1))) {all=false;break;}
            if(!all)without_all_pins.push_back(candidate);
        }
        if(without_all_pins.size()==1)chosen=without_all_pins.front();
        else if(without_all_pins.empty())chosen=*candidates.rbegin();
        else throw std::runtime_error("official TSV candidates are ambiguous for "+net.name);
    }
    const auto [area,x1,y1,x2,y2]=chosen;(void)area;
    return {(x1+x2)*0.5,(y1+y2)*0.5};
}
int checked_add(int a,uint64_t b) {
    if(a<0 || b>uint64_t(std::numeric_limits<int>::max()-a))
        throw std::runtime_error("official metric integer demand/overflow exceeds supported range");
    return a+int(b);
}
std::mutex& flute_mutex(){static std::mutex m;return m;}
}

struct NativeFluteBaseline::Impl {
    std::string assets;
    int maximum{150};
#ifdef __linux__
    void* python{nullptr};void* flute{nullptr};void* extension{nullptr};
    using Ctor=void(*)(void*);
    // Exact Linux x86-64 C ABI of the untouched official libflute.
    struct Branch {int x,y,n;};
    struct Tree {int degree,length;Branch* branch;};
    static_assert(sizeof(int)==4 && sizeof(Branch)==12 && sizeof(Tree)==16);
    static_assert(offsetof(Tree,branch)==8);
    using Eval=Tree(*)(int,int*,int*,int);
    using Max= int(*)(void*);
    Eval evaluate{nullptr};alignas(16) std::array<unsigned char,16> object{};
    ~Impl(){if(extension)dlclose(extension);if(flute)dlclose(flute);if(python)dlclose(python);}
#endif
};
NativeFluteBaseline::NativeFluteBaseline(const std::string& directory) {
#ifdef __linux__
    namespace fs=std::filesystem;
    const auto assets=fs::weakly_canonical(directory).string();
    for(const auto& name:{"libflute.so","tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so","POWV9.dat","POST9.dat"})
        if(!fs::is_regular_file(fs::path(assets)/name))
            throw std::runtime_error("missing original official FLUTE runtime asset: "+(fs::path(assets)/name).string());
    std::lock_guard<std::mutex> lock(flute_mutex());
    // readLUT owns global native tables. Keep one strong process-wide instance
    // so simultaneous and sequential baselines never overwrite those tables.
    static std::shared_ptr<Impl> cached;
    if(cached) {
        if(cached->assets!=assets)
            throw std::runtime_error("official FLUTE cannot mix runtime asset directories in one process");
        impl_=cached;
        return;
    }
    impl_=std::make_shared<Impl>();
    impl_->assets=assets;
    impl_->python=dlopen("libpython3.10.so.1.0",RTLD_NOW|RTLD_GLOBAL);
    if(!impl_->python)throw std::runtime_error(std::string("official FLUTE needs Linux libpython3.10.so.1.0: ")+dlerror());
    impl_->flute=dlopen((fs::path(impl_->assets)/"libflute.so").c_str(),RTLD_NOW|RTLD_GLOBAL);
    if(!impl_->flute)throw std::runtime_error(std::string("cannot load official libflute: ")+dlerror());
    impl_->extension=dlopen((fs::path(impl_->assets)/"tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so").c_str(),RTLD_NOW|RTLD_LOCAL);
    if(!impl_->extension)throw std::runtime_error(std::string("cannot load official FLUTE native extension: ")+dlerror());
    auto ctor=reinterpret_cast<Impl::Ctor>(dlsym(impl_->extension,"_ZN3tse9framework12SteinerFluteC1Ev"));
    auto max=reinterpret_cast<Impl::Max>(dlsym(impl_->extension,"_ZN3tse9framework12SteinerFlute12GetMaxDegreeEv"));
    auto original_method=dlsym(impl_->extension,"_ZN3tse9framework12SteinerFlute16GetSteinerLengthEiRKSt6vectorIiSaIiEES6_b");
    impl_->evaluate=reinterpret_cast<Impl::Eval>(dlsym(impl_->flute,"flute"));
    if(!ctor || !max || !original_method || !impl_->evaluate)
        throw std::runtime_error("official FLUTE native ABI symbols unavailable");
    const auto before=fs::current_path();
    try {fs::current_path(impl_->assets);ctor(impl_->object.data());fs::current_path(before);}
    catch(...){fs::current_path(before);throw;}
    impl_->maximum=max(impl_->object.data());
    if(impl_->maximum!=150)throw std::runtime_error("unsupported official FLUTE native degree limit");
    cached=impl_;
#else
    (void)directory;throw std::runtime_error("official native FLUTE baseline requires Linux CPython 3.10 runtime");
#endif
}
NativeFluteBaseline::~NativeFluteBaseline()=default;
NativeFluteBaseline::NativeFluteBaseline(NativeFluteBaseline&&) noexcept=default;
NativeFluteBaseline& NativeFluteBaseline::operator=(NativeFluteBaseline&&) noexcept=default;
const std::string& NativeFluteBaseline::asset_directory() const {if(!impl_)throw std::runtime_error("moved FLUTE runtime");return impl_->assets;}
int NativeFluteBaseline::max_degree() const {if(!impl_)throw std::runtime_error("moved FLUTE runtime");return impl_->maximum;}
NativeFluteResult NativeFluteBaseline::evaluate_points(const std::vector<Point>& points) const {
    if(!impl_)throw std::runtime_error("moved FLUTE runtime");
    if(points.empty())throw std::runtime_error("official FLUTE needs at least one pin");
    if(points.size()>size_t(impl_->maximum))return {0,0,true};
#ifdef __linux__
    std::vector<int> x(impl_->maximum,0),y(impl_->maximum,0);
    for(size_t i=0;i<points.size();++i) {
        for(double v:{points[i].x,points[i].y})if(!std::isfinite(v) ||
            v<std::numeric_limits<int>::min() || v>std::numeric_limits<int>::max())
            throw std::runtime_error("official FLUTE pin DBU does not fit native int");
        x[i]=int(points[i].x);y[i]=int(points[i].y);
    }
    // The original wrapper crashes in flute for degree 1. Its edge loop would
    // be empty; handle that boundary here without entering the native library.
    const int degree=int(points.size());
    if(degree==1)return {0,0,false};
    const auto x_bounds=std::minmax_element(x.begin(),x.begin()+degree);
    const auto y_bounds=std::minmax_element(y.begin(),y.begin()+degree);
    const int64_t span=int64_t(*x_bounds.second)-*x_bounds.first+
                       int64_t(*y_bounds.second)-*y_bounds.first;
    if(span>std::numeric_limits<int>::max())
        throw std::runtime_error("official FLUTE coordinate span exceeds native int arithmetic");

    std::lock_guard<std::mutex> lock(flute_mutex());
    auto tree=impl_->evaluate(degree,x.data(),y.data(),3);
    // The official wrapper omits this free. The exported freeTree handles
    // a different rooted-node representation; flute's flat Branch array uses
    // malloc and must be released with its matching libc free.
    std::unique_ptr<Impl::Branch,decltype(&std::free)> owned(tree.branch,&std::free);
    if(tree.degree!=degree || !tree.branch)
        throw std::runtime_error("official FLUTE returned invalid native tree");
    const int branch_count=2*degree-2; // Original method uses input degree.
    for(int i=0;i<branch_count;++i)
        if(tree.branch[i].n<0 || tree.branch[i].n>=branch_count)
            throw std::runtime_error("official FLUTE returned invalid branch neighbor");

    std::vector<std::pair<const Impl::Branch*,const Impl::Branch*>> edges;
    edges.reserve(branch_count);
    float length=0.0f; // Match the original per-edge float rounding and order.
    for(int i=0;i<branch_count;++i) {
        const auto& a=tree.branch[i];
        const auto& b=tree.branch[a.n];
        edges.emplace_back(&a,&b); // getVias includes zero-length/self edges.
        if(a.x==b.x && a.y==b.y)continue;
        const int64_t edge_length=int64_t(std::max(a.x,b.x))-std::min(a.x,b.x)+
                                  int64_t(std::max(a.y,b.y))-std::min(a.y,b.y);
        if(edge_length>std::numeric_limits<int>::max())
            throw std::runtime_error("official FLUTE branch length exceeds native int arithmetic");
        length+=static_cast<float>(edge_length);
    }
    std::set<std::tuple<int,int,int>> via_nodes;
    const auto equal_xy=[](const Impl::Branch& a,const Impl::Branch& b){
        return a.x==b.x && a.y==b.y;
    };
    for(size_t i=0;i<edges.size();++i)for(size_t j=i+1;j<edges.size();++j) {
        const auto& a=*edges[i].first;const auto& b=*edges[i].second;
        const auto& c=*edges[j].first;const auto& d=*edges[j].second;
        // Exact original getVias: coordinate equality, first-match else-if,
        // but set identity includes n. Co-located branches may count twice.
        if(equal_xy(a,c) || equal_xy(a,d))via_nodes.emplace(a.x,a.y,a.n);
        else if(equal_xy(b,c) || equal_xy(b,d))via_nodes.emplace(b.x,b.y,b.n);
    }
    if(!std::isfinite(length) || length<0)
        throw std::runtime_error("official FLUTE returned invalid result");
    return {length,int(via_nodes.size()),false};
#else
    throw std::runtime_error("official FLUTE unsupported host");
#endif
}
double NativeFluteBaseline::measure_net(const Net& net,double pitch,const OfficialMetricOptions& o) const {
    if(!std::isfinite(pitch)||pitch<=0)throw std::runtime_error("invalid FLUTE GCell pitch");
    std::vector<Point> points;std::set<std::string> names;
    // Original compute_hpwl breaks after the first geometry of each unique Pin.
    // Average DBU endpoints after transform: transforming a micron-space center
    // can round just below an integer and then lose one DBU during truncation.
    for(const auto& pin:net.pins)if(names.insert(pin.name).second)
        points.push_back(pin.port_rects.empty()?pin.coord:pin.port_rects.front().rect.center());
    const auto f=evaluate_points(points);if(f.skipped_degree_limit)return 0;
    return f.wirelength_dbu+(f.via_count+(net.is_cross_die?10:0))*o.via_cost_ratio*pitch+
        (net.is_cross_die?o.tsv_cost_ratio*pitch:0.0);
}
double official_thermal_gain_for_def_file(const std::string& path) {
    const auto name=std::filesystem::path(path).filename().string();
    static const std::map<std::string,double> table{
        {"open_3d_case001.input.def",11.684},{"ispd19_test001.input.def",11.684},
        {"aes_sky130hs.input.def",1.719},{"open_3d_case002.input.def",1.719},
        {"open_3d_case003.input.def",4.224},{"ispd19_test004.input.def",4.224},
        {"ispd18_test005.input.def",12.350},{"open_3d_case004.input.def",12.350},
        {"open_3d_case005.input.def",8.323},{"ispd18_test007.input.def",8.323},
        {"ispd18_test010.input.def",18.996},{"open_3d_case006.input.def",18.996},
        {"toy.input.def",21.492}};
    const auto f=table.find(name);return f==table.end()?20.0:f->second;
}
NetGuideMetrics measure_official_net_guides(const Net& net,const std::vector<GuideRect>& raw,
    const RoutingGraph& graph,const LefParser& lef,const OfficialMetricOptions& o,double reference) {
    check_options(o);
    if(!std::isfinite(reference)||reference<0)throw std::runtime_error("invalid cached exact official baseline");
    const auto guides=clean_guides(raw,graph.die_area());
    NetGuideMetrics result;result.reference=reference;
    std::map<int,int> demand;
    std::unordered_set<int> covered;
    std::array<std::vector<const GuideRect*>,21> retained;
    for(const auto& g:guides) {
        const int l=get_layer_index(g.layer);const auto direction=lef.get_layer_direction(g.layer);
        if(direction==Direction::UNKNOWN)throw std::runtime_error("official metric guide layer is not ROUTING");
        if(!net.is_cross_die && !net.pins.empty() && ((net.pins.front().layer_idx<=10)!=(l<=10)))
            throw std::runtime_error("official intra-die net uses the other die");
        const auto nodes=cells(g,graph);
        for(int n:nodes)demand[n]=1;
        const bool duplicate_cells=std::all_of(nodes.begin(),nodes.end(),[&](int n){return covered.count(n)!=0;});
        if(duplicate_cells)continue;
        retained[l].push_back(&g);
        for(int n:nodes)if(covered.insert(n).second)
            result.wirelength+=direction==Direction::HORIZONTAL?o.gcell_width_dbu:o.gcell_height_dbu;
    }
    const double pitch=std::max(o.gcell_width_dbu,o.gcell_height_dbu);
    for(int l=1;l<20;++l)for(const auto* a:retained[l]) {
        for(const auto* b:retained[l+1])if(positive_overlap(*a,*b)) {
            result.wirelength+=(l==10?o.tsv_cost_ratio:o.via_cost_ratio)*pitch;break;
        }
    }
    if(net.is_cross_die) {
        const auto point=select_tsv(net,guides);const auto& grid=graph.grid();
        const int col=grid.get_col(point.x),row=grid.get_row(point.y);
        if(col<0||row<0)throw std::runtime_error("official selected TSV is outside GCell grid");
        result.tsv_points.push_back(point);result.tsv_cells.push_back(row*grid.cols+col);
        for(int l:{10,11}) {const int node=graph.node(col,row,l);demand[node]=checked_add(demand[node],o.tsv_congestion_ratio);}
    }
    result.demand.assign(demand.begin(),demand.end());
    return result;
}
OfficialGuideMetrics aggregate_official_metrics(const std::vector<const NetGuideMetrics*>& nets,
    const std::vector<int>& guide_demand,const RoutingGraph& graph,const ThermalParser& thermal,
    const OfficialMetricOptions& o,bool capture) {
    check_options(o);
    if(guide_demand.size()!=graph.size())throw std::runtime_error("official demand dimensions mismatch");
    OfficialGuideMetrics result;auto& out=result.guide;
    if(capture)out.demand.resize(graph.size());
    const auto& grid=graph.grid();std::vector<int> tsv_counts(size_t(grid.rows)*grid.cols,0);
    for(const auto* n:nets) {
        if(!n || !n->connected || !std::isfinite(n->wirelength)||n->wirelength<0 ||
            !std::isfinite(n->reference)||n->reference<0)throw std::runtime_error("invalid cached official net");
        out.wirelength+=n->wirelength;out.reference+=n->reference;
        for(int cell:n->tsv_cells) {
            if(cell<0||size_t(cell)>=tsv_counts.size())throw std::runtime_error("official TSV cell invalid");
            tsv_counts[cell]=checked_add(tsv_counts[cell],1);out.tsv_count=checked_add(out.tsv_count,1);
        }
    }
    long double sum=0;
    for(size_t node=0;node<guide_demand.size();++node) {
        if(guide_demand[node]<0)throw std::runtime_error("official cached demand negative");
        const int demand=checked_add(guide_demand[node],graph.official_obstacle_demand(int(node)));
        if(capture)out.demand[node]=demand;
        if(demand==0)continue;
        ++out.used_cells;
        const double capacity=graph.official_capacity(int(node));
        if(!std::isfinite(capacity) || capacity<0)throw std::runtime_error("official raw capacity invalid");
        const int excess=std::max(0,demand-int(capacity));
        out.overflow=checked_add(out.overflow,uint64_t(excess));out.max_overflow=std::max(out.max_overflow,excess);
        if(excess)++result.overflow_entries;
        if(capacity<=0) {++result.zero_capacity_cells;sum+=10000.0L;}
        else if(excess){const long double r=0.1L*excess/capacity;sum+=r*r;}
    }
    result.squared_normalized_overflow=double(sum);
    result.average_overflow=out.used_cells?double(out.overflow)/out.used_cells:0.0;
    out.normalized=out.reference>0?out.wirelength/out.reference:100.0;
    std::vector<double> heat(tsv_counts.size());
    for(int row=0;row<grid.rows;++row)for(int col=0;col<grid.cols;++col)
        heat[size_t(row)*grid.cols+col]=std::max(0.0,thermal.get_temperature(col,row)-thermal.ambient_temperature());
    if(!out.tsv_count)throw std::runtime_error("official thermal scoring requires at least one TSV");
    if(std::none_of(heat.begin(),heat.end(),[](double h){return h>0;}))
        throw std::runtime_error("official thermal scoring requires positive temperature rise");
    out.correlation=PearsonState(std::move(heat),std::move(tsv_counts)).correlation();
    const double length=std::max(out.normalized,1e-9);
    result.wirelength_score=100.0*std::min(length,1.0/length);
    result.congestion_score=100.0/std::sqrt(1.0+double(sum));
    result.thermal_score=100.0*(0.5+0.5*std::tanh(o.thermal_gain*out.correlation));
    out.quality=(0.45*result.wirelength_score+0.30*result.congestion_score+0.25*result.thermal_score)/100.0;
    return result;
}
} // namespace eda
