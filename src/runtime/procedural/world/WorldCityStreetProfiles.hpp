#pragma once
#include "WorldCitySkeleton.hpp"
#include "WorldCityReliefNoise.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <queue>
#include <limits>

namespace procedural::world::rewrite {
// A circulation blueprint, independent of mesh generation. Junctions share
// levels; interior stations let a road climb and descend between junctions.
struct CityStreetProfilePlan {
  struct Street {
    uint64_t edge=0;
    std::vector<Position> path;
    double maximumGrade=0, maximumCut=0;
  };
  std::vector<Street> streets;
  std::map<uint64_t,double> junctions;
  size_t stationCount=0;
  double maximumCut=0;
};
inline bool planCityStreetProfiles(const CitySkeleton &skeleton,
    const std::function<double(double,double)> &terrain,const CityReliefSettings &settings,
    CityStreetProfilePlan &result,std::string *error=nullptr) {
  const auto fail=[&](const char *why){if(error)*error=why;return false;};
  if(!terrain || !std::isfinite(settings.maximumStreetGrade) || settings.maximumStreetGrade<=0 ||
     !std::isfinite(settings.streetExcavationMetres) || settings.streetExcavationMetres<0)
    return fail("invalid street profile input");
  struct Node {double ceiling=INFINITY;std::vector<std::pair<size_t,double>> links;};
  struct Chain {uint64_t id;std::vector<Position> points;std::vector<size_t> nodes;};
  std::vector<Node> nodes;std::map<uint64_t,size_t> junction;
  std::map<uint64_t,Position> positions;
  for(const auto &n:skeleton.roadNodes)positions[n.id]=n.position;
  for(const auto &[id,p]:positions){junction[id]=nodes.size();nodes.push_back({});}
  auto edges=skeleton.roadEdges;
  std::sort(edges.begin(),edges.end(),[](const auto &a,const auto &b){return a.id<b.id;});
  std::vector<Chain> chains;
  for(const auto &edge:edges) {
    if(!junction.count(edge.from) || !junction.count(edge.to))return fail("street has missing junction");
    auto path=edge.path;if(path.size()<2)path={positions[edge.from],positions[edge.to]};
    const double width=edge.halfWidth>0?edge.halfWidth:1.5;
    if(!std::isfinite(width) || width>100)return fail("invalid street profile width");
    Chain chain;chain.id=edge.id;
    for(size_t k=1;k<path.size();++k) {
      const auto a=path[k-1],b=path[k];const double dx=b.x-a.x,dz=b.z-a.z,len=std::hypot(dx,dz);
      if(!std::isfinite(len) || len>100000)return fail("invalid street profile segment");
      const unsigned steps=std::max(1u,unsigned(std::ceil(len)));
      const unsigned cross=std::max(2u,unsigned(std::ceil(width*2)));
      for(unsigned j=0;j<=steps;++j) {
        const double t=double(j)/steps;Position p{a.x+dx*t,0,a.z+dz*t};
        size_t node;
        if(j==0 && !chain.nodes.empty())node=chain.nodes.back();
        else if(k==1 && j==0)node=junction[edge.from];
        else if(k+1==path.size() && j==steps)node=junction[edge.to];
        else {if(nodes.size()>=250000)return fail("street station budget exceeded");node=nodes.size();nodes.push_back({});}
        for(unsigned c=0;c<=cross;++c) {
          const double lateral=-width+2*width*c/cross;
          const double h=terrain(p.x-(len>1e-9?dz/len:0)*lateral,p.z+(len>1e-9?dx/len:1)*lateral);
          if(!std::isfinite(h))return fail("non-finite street terrain sample");
          nodes[node].ceiling=std::min(nodes[node].ceiling,h-settings.streetExcavationMetres-.035);
        }
        if(j==0 && !chain.nodes.empty())continue;
        if(!chain.nodes.empty()) {
          const auto previous=chain.nodes.back();const auto q=chain.points.back();
          const double reach=settings.maximumStreetGrade*std::hypot(p.x-q.x,p.z-q.z);
          nodes[previous].links.push_back({node,reach});nodes[node].links.push_back({previous,reach});
        }
        chain.nodes.push_back(node);chain.points.push_back(p);
      }
    }
    if(chain.nodes.size()>1)chains.push_back(std::move(chain));
  }
  // Greatest grade-constrained surface below every excavation ceiling. Unlike
  // dropping an entire edge, a local valley affects only the necessary ramps.
  using Entry=std::pair<double,size_t>;
  std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> queue;
  std::vector<double> heights;heights.reserve(nodes.size());
  for(size_t i=0;i<nodes.size();++i){heights.push_back(nodes[i].ceiling);if(std::isfinite(heights.back()))queue.push({heights.back(),i});}
  while(!queue.empty()) {
    const auto [h,i]=queue.top();queue.pop();if(h>heights[i]+1e-10)continue;
    for(const auto &[j,reach]:nodes[i].links)if(h+reach<heights[j]-1e-10){heights[j]=h+reach;queue.push({heights[j],j});}
  }
  CityStreetProfilePlan out;out.stationCount=nodes.size();
  for(const auto &[id,node]:junction)if(std::isfinite(heights[node]))out.junctions[id]=heights[node];
  for(auto &chain:chains) {
    CityStreetProfilePlan::Street street;street.edge=chain.id;
    for(size_t i=0;i<chain.points.size();++i) {
      auto p=chain.points[i];p.y=heights[chain.nodes[i]];
      if(!std::isfinite(p.y))return fail("unresolved street station");
      street.maximumCut=std::max(street.maximumCut,terrain(p.x,p.z)-p.y-.035);
      if(i) {
        const auto q=chain.points[i-1];const double d=std::hypot(p.x-q.x,p.z-q.z);
        if(d>1e-8)street.maximumGrade=std::max(street.maximumGrade,std::abs(p.y-heights[chain.nodes[i-1]])/d);
      }
      // Keep the solved shape, removing only exactly collinear stations. Broad
      // flats and ramps need few faces; stations are a planning resolution.
      while(street.path.size()>1) {
        const auto a=street.path[street.path.size()-2],b=street.path.back();
        const double length=std::hypot(p.x-a.x,p.z-a.z),d=std::hypot(b.x-a.x,b.z-a.z);
        if(length<1e-9 || std::abs((b.x-a.x)*(p.z-a.z)-(b.z-a.z)*(p.x-a.x))>1e-7 ||
           d>length || std::abs(b.y-(a.y+(p.y-a.y)*d/length))>1e-8)break;
        street.path.pop_back();
      }
      street.path.push_back(p);
    }
    out.maximumCut=std::max(out.maximumCut,street.maximumCut);out.streets.push_back(std::move(street));
  }
  result=std::move(out);if(error)error->clear();return true;
}
}
