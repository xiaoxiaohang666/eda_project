#include "router/cost_diagnostics.hpp"
#include <numeric>
#include <iomanip>
namespace eda {
void write_cost_diagnostics(std::ostream& out,const std::vector<NetRoute>& routes,const std::vector<Net>& nets,const std::vector<double>& before_tsv) {
    std::vector<size_t> order(routes.size());std::iota(order.begin(),order.end(),0);
    std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return routes[a].measured.wirelength>routes[b].measured.wirelength;});
    double planar=0,vias=0,tsvs=0,total=0,high_fanout=0,migration_excess=0;
    for(size_t i=0;i<routes.size();++i) {
        const auto& m=routes[i].measured;
        planar+=m.planar_wirelength;vias+=m.via_wirelength;tsvs+=m.tsv_wirelength;total+=m.wirelength;
        if(nets[i].pins.size()>16) high_fanout+=m.wirelength;
        if(before_tsv.size()==routes.size()) migration_excess+=std::max(0.0,m.wirelength-before_tsv[i]);
    }
    auto top=[&](double fraction) {double sum=0;for(size_t k=0;k<std::min(order.size(),size_t(std::ceil(order.size()*fraction)));++k)sum+=routes[order[k]].measured.wirelength;return sum;};
    out<<"{\"planar_dbu\":"<<planar<<",\"ordinary_via_dbu\":"<<vias<<",\"tsv_dbu\":"<<tsvs
       <<",\"total_dbu\":"<<total<<",\"high_fanout_dbu\":"<<high_fanout<<",\"top_1_percent_dbu\":"<<top(.01)
       <<",\"top_5_percent_dbu\":"<<top(.05)<<",\"positive_migration_excess_dbu\":"<<migration_excess<<",\"nets\":[";
    for(size_t i=0;i<routes.size();++i) {
        if(i)out<<',';
        const auto& m=routes[i].measured;
        // std::quoted escapes quotes/backslashes; DEF identifiers cannot contain control whitespace.
        out<<"{\"net\":"<<std::quoted(nets[i].name)<<",\"pins\":"<<nets[i].pins.size()<<",\"planar_dbu\":"<<m.planar_wirelength
           <<",\"ordinary_via_dbu\":"<<m.via_wirelength<<",\"tsv_dbu\":"<<m.tsv_wirelength<<",\"ordinary_connections\":"<<m.ordinary_connections
           <<",\"total_dbu\":"<<m.wirelength<<",\"reference_dbu\":"<<m.reference;
        if(before_tsv.size()==routes.size())out<<",\"migration_delta_dbu\":"<<m.wirelength-before_tsv[i];
        out<<'}';
    }
    out<<"]}";
}
}
