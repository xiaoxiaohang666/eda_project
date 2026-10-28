#include "router/negotiated_router.hpp"
#include <optional>
#include <numeric>

namespace eda {

std::vector<NetRoute> NegotiatedRouter::pin_local_candidates(size_t index) {
    const auto& net=def_.nets()[index];
    std::vector<NetRoute> result;
    if(net.is_cross_die || net.pins.size()!=2) return result;
    const auto& first=net.pins[0];const auto& last=net.pins[1];
    const int low=first.layer_idx<=10?1:11,high=low+9;
    if(last.layer_idx<low || last.layer_idx>high) return result;
    for(auto& route:terminal_pair_candidates(first,last,low,high)) {
        try {encode(index,route);++counters_.pin_local_candidates;result.push_back(std::move(route));}
        catch(const std::runtime_error&) { /* Keep the searched route available. */ }
    }
    return result;
}

// Geometry-only building block shared by same-die and future TSV-side routing.
// Encoding a whole net belongs to the caller, after all terminals are included.
std::vector<NetRoute> NegotiatedRouter::terminal_pair_candidates(
        const Pin& first,const Pin& last,int low,int high) {
    std::vector<NetRoute> result;
    const auto& grid=graph_.grid();
    // The submission's global-grid model emits occupied GCells, not physical
    // wire-width corridors. Keep physical geometry only for the older models.
    const bool grid_geometry=options_.official_model &&
        options_.tsv_policy.resource_mode==TsvResourceMode::GLOBAL_GRID;
    auto terminal_point=[&](const Pin& pin) {
        if(!grid_geometry) return pin.coord;
        return graph_.point(graph_.node(grid.get_col(pin.coord.x),grid.get_row(pin.coord.y),pin.layer_idx));
    };
    const Point first_point=terminal_point(first),last_point=terminal_point(last);
    auto build=[&](int horizontal,int vertical,int kind)->std::optional<NetRoute> {
        NetRoute route;route.explicit_geometry=!grid_geometry;
        PinAccess geometry;
        auto append_guide=[&](const Rect& r,int layer) {
            const auto& die=def_.die_area();
            geometry.guides.emplace_back(std::max(die.x1,std::floor(r.x1)),std::max(die.y1,std::floor(r.y1)),
                std::min(die.x2,std::ceil(r.x2)),std::min(die.y2,std::ceil(r.y2)),get_layer_name(layer));
        };
        auto node_at=[&](Point p,int layer) {return graph_.node(grid.get_col(p.x),grid.get_row(p.y),layer);};
        auto reserve=[&](int a,int b) {
            if(!graph_.legal_edge(a,b)) return false;
            route.edges.insert(route_edge(a,b));route.nodes.insert(a);route.nodes.insert(b);return true;
        };
        auto segment=[&](Point a,Point b,int layer) {
            if(!graph_.legal_segment(a,b,layer)) return false;
            int n=node_at(a,layer),target=node_at(b,layer);
            if(!graph_.legal_node(n)||!graph_.legal_node(target)) return false;
            route.nodes.insert(n);
            while(n!=target) {
                int c=graph_.col(n),r=graph_.row(n);
                if(c!=graph_.col(target)) c+=graph_.col(target)>c?1:-1;
                else r+=graph_.row(target)>r?1:-1;
                int next=graph_.node(c,r,layer);
                if(!reserve(n,next)) return false;
                n=next;
            }
            if(!grid_geometry) {
                append_guide(graph_.segment_rect(a,b,layer),layer);
                geometry.length+=a.distance_to(b);
            }
            return true;
        };
        auto stack=[&](Point p,int from,int to) {
            for(int l=std::min(from,to);l<std::max(from,to);++l) {
                if(l==10||!graph_.legal_via(p,l)||!reserve(node_at(p,l),node_at(p,l+1))) return false;
                if(!grid_geometry) for(const auto& shape:graph_.via_rects(p,l)) {
                    int metal=get_layer_index(shape.layer);
                    if(metal) append_guide(shape.rect,metal);
                }
                if(!grid_geometry) ++geometry.vias;
            }
            return true;
        };
        Point cursor=first_point;int layer=first.layer_idx;
        if(!segment(cursor,cursor,layer)) return std::nullopt;
        auto leg=[&](Point to,int next_layer) {
            // A degenerate L/Z leg carries no wire. Switching layers here and
            // immediately switching back would create a useless via excursion.
            if(cursor.distance_to(to)<1e-8) return true;
            if(!stack(cursor,layer,next_layer)||!segment(cursor,to,next_layer)) return false;
            cursor=to;layer=next_layer;return true;
        };
        const Point a=first_point,b=last_point;
        bool ok=false;
        if(kind==0) ok=leg({b.x,a.y},horizontal)&&leg(b,vertical);
        if(kind==1) ok=leg({a.x,b.y},vertical)&&leg(b,horizontal);
        if(kind==2) {
            const double mid=grid_geometry
                ? graph_.point(graph_.node((grid.get_col(a.x)+grid.get_col(b.x))/2,grid.get_row(a.y),horizontal)).x
                : std::floor((a.x+b.x)*0.5);
            ok=leg({mid,a.y},horizontal)&&leg({mid,b.y},vertical)&&leg(b,horizontal);
        }
        if(kind==3) {
            const double mid=grid_geometry
                ? graph_.point(graph_.node(grid.get_col(a.x),(grid.get_row(a.y)+grid.get_row(b.y))/2,vertical)).y
                : std::floor((a.y+b.y)*0.5);
            ok=leg({a.x,mid},vertical)&&leg({b.x,mid},horizontal)&&leg(b,vertical);
        }
        if(!ok||!stack(b,layer,last.layer_idx)||!segment(b,b,last.layer_idx)) return std::nullopt;
        if(!grid_geometry) {
            geometry.node=node_at(first.coord,first.layer_idx);
            route.accesses.push_back(std::move(geometry));
        }
        return route;
    };
    std::vector<std::tuple<int,int,int>> pairs;
    for(int h=low;h<=high;++h) if(lef_.get_layer_direction(get_layer_name(h))==Direction::HORIZONTAL)
        for(int v=low;v<=high;++v) if(lef_.get_layer_direction(get_layer_name(v))==Direction::VERTICAL) {
            const int hv=std::abs(first.layer_idx-h)+std::abs(h-v)+std::abs(v-last.layer_idx);
            const int vh=std::abs(first.layer_idx-v)+std::abs(v-h)+std::abs(h-last.layer_idx);
            pairs.emplace_back(std::min(hv,vh),h,v);
        }
    std::sort(pairs.begin(),pairs.end());
    if(pairs.size()>6) pairs.resize(6);
    for(auto [rank,h,v]:pairs) for(int kind=0;kind<4;++kind) {
        (void)rank;
        if(timed_out()) return result;
        try {if(auto candidate=build(h,v,kind)) result.push_back(std::move(*candidate));}
        catch(const std::runtime_error&) { /* An invalid local template leaves the searched path available. */ }
    }
    return result;
}

void NegotiatedRouter::recover_wirelength() {
    // Congestion negotiation intentionally uses large penalties. Once legal,
    // release those prices in a separate pass and accept only complete,
    // independently remeasured improvements. No denominator-only improvements.
    const double saved_present=present_,saved_history=history_scale_;
    for(int round=0;round<options_.wirelength_rounds&&!timed_out();++round) {
        present_=round==0?0.25:(round==1?0.05:0.0);history_scale_=0;
        std::vector<size_t> order(routes_.size());std::iota(order.begin(),order.end(),0);
        std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b) {
            const double x=routes_[a].measured.wirelength-routes_[a].measured.reference;
            const double y=routes_[b].measured.wirelength-routes_[b].measured.reference;
            return x==y?def_.nets()[a].name<def_.nets()[b].name:x>y;
        });
        size_t accepted=0;
        for(size_t i:order) {
            if(timed_out()) break;
            ++counters_.wirelength_trials;
            const auto before=best_metrics_;const auto old=routes_[i];
            external_wirelength_=before.guide.wirelength-old.measured.wirelength;
            external_reference_=before.guide.reference-old.measured.reference;
            account(i,old,-1);routes_[i]=NetRoute{};
            try {
                routes_[i]=build_route(i,old.slot);account(i,routes_[i],1);
                const auto after=measure();
                if((options_.official_model || (after.guide.overflow<=before.guide.overflow && after.physical_overflow<=before.physical_overflow &&
                   after.guide.wirelength<=before.guide.wirelength+1e-6 &&
                   after.physical_length<=before.physical_length+1e-6 &&
                   (after.guide.overflow<before.guide.overflow || after.physical_overflow<before.physical_overflow ||
                    after.guide.wirelength<before.guide.wirelength-1e-6 ||
                    after.physical_length<before.physical_length-1e-6 || after.vias<before.vias))) && better(after,before)) {
                    validate_complete();best_metrics_=after;++accepted;++counters_.wirelength_accepted;continue;
                }
            } catch(const std::exception& e) {++rejection_reasons_[e.what()];}
            account(i,routes_[i],-1);routes_[i]=old;account(i,old,1);++counters_.rollbacks;
        }
        std::cout<<"[WIRELENGTH] round="<<round+1<<" accepted="<<accepted
                 <<" normalized="<<best_metrics_.guide.normalized<<" physical_length="<<best_metrics_.physical_length<<"\n";
        // The first two rounds deliberately change the search objective, even
        // if a prior round found no improvement; later identical sweeps stop.
        if(!accepted&&round>=2) break;
    }
    present_=saved_present;history_scale_=saved_history;
    external_wirelength_=0;external_reference_=0;
}

} // namespace eda
