#pragma once
#include "WorldCityCompletion.hpp"
#include <queue>
#include <map>
#include <set>

namespace procedural::world::rewrite {
// Keep frontage and the shortest existing connections between occupied places.
// This removes empty cycles as well as dead ends, without inventing new roads.
inline size_t pruneUnoccupiedCityStreets(CityCompletionPlan &city,const SettlementFabric &fabric) {
  if(city.historicalLots.empty() || city.routes.empty())return 0;
  const size_t n=city.routes.size();std::vector<bool> required(n,false),keep(n,false);
  const auto serve=[&](const std::vector<Position> &points) {
    double best=INFINITY;size_t chosen=n;
    for(size_t i=0;i<n;++i)for(size_t j=1;j<city.routes[i].path.size();++j) {
      const auto a=city.routes[i].path[j-1],b=city.routes[i].path[j];
      const double dx=b.x-a.x,dz=b.z-a.z,l2=dx*dx+dz*dz;
      for(auto p:points) {
        const double t=std::clamp(((p.x-a.x)*dx+(p.z-a.z)*dz)/std::max(1e-12,l2),0.,1.);
        const double d=std::hypot(p.x-a.x-t*dx,p.z-a.z-t*dz)-city.routes[i].halfWidth;
        if(d<best){best=d;chosen=i;}
      }
    }
    if(chosen<n)required[chosen]=true;
  };
  std::set<uint64_t> occupied;
  for(const auto &lot:city.lots)if(lot.developed)occupied.insert(lot.lot);
  for(const auto &r:city.skeleton.regions)if(occupied.count(r.id))serve(r.boundary);
  for(const auto &gate:city.gatehouses)if(gate.built)serve({gate.position});
  for(const auto &compound:fabric.compounds)for(const auto &wing:compound.wings)serve(wing.brief.shell);
  for(size_t i=0;i<n;++i)for(const auto &water:city.waterways)if(water.route==city.routes[i].id)required[i]=true;
  std::map<uint64_t,const CityRoadEdge*> edges;
  for(const auto &e:city.skeleton.roadEdges)edges[e.id]=&e;
  std::map<uint64_t,std::set<size_t>> nodes;
  std::map<std::pair<int64_t,int64_t>,std::set<size_t>> points;
  std::vector<double> length(n,0.);std::vector<std::set<size_t>> graph(n);
  for(size_t i=0;i<n;++i) {
    const auto &r=city.routes[i];
    for(auto id:r.edges)if(edges.count(id)){nodes[edges[id]->from].insert(i);nodes[edges[id]->to].insert(i);}
    for(size_t j=0;j<r.path.size();++j) {
      const auto p=r.path[j];points[{int64_t(std::llround(p.x*1000)),int64_t(std::llround(p.z*1000))}].insert(i);
      if(j)length[i]+=std::hypot(p.x-r.path[j-1].x,p.z-r.path[j-1].z);
    }
  }
  const auto connect=[&](const auto &groups){for(const auto &[key,list]:groups)for(auto a:list)for(auto b:list)if(a!=b)graph[a].insert(b);};
  connect(nodes);connect(points);
  std::vector<bool> visited(n,false);
  for(size_t root=0;root<n;++root)if(required[root] && !visited[root]) {
    std::vector<double> distance(n,INFINITY);std::vector<size_t> parent(n,n);
    using Entry=std::pair<double,size_t>;std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> queue;
    distance[root]=0;queue.push({0,root});
    while(!queue.empty()) {
      auto [d,a]=queue.top();queue.pop();if(d>distance[a])continue;visited[a]=true;
      for(auto b:graph[a]){const double next=d+(length[a]+length[b])*.5+.001;
        if(next<distance[b]){distance[b]=next;parent[b]=a;queue.push({next,b});}}
    }
    for(size_t i=0;i<n;++i)if(required[i] && std::isfinite(distance[i])) {
      for(size_t a=i;a<n && !keep[a];a=parent[a])keep[a]=true;
    }
  }
  std::vector<CitySmoothedRoute> routes;
  for(size_t i=0;i<n;++i)if(keep[i])routes.push_back(std::move(city.routes[i]));
  const auto removed=n-routes.size();city.routes=std::move(routes);return removed;
}
}
