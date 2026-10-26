#pragma once
#include "WorldCityFacetTerrain.hpp"

namespace procedural::world::rewrite {
struct CityElevationContours {
  std::vector<std::vector<Position>> bands;
  std::vector<std::array<Position,2>> lines;
};
// Slice the planning planes at shared absolute elevations, before final CDT.
// Every neighbour computes the same intersection on a shared slope edge.
inline CityElevationContours planCityElevationContours(
    const std::vector<std::array<Position,3>> &planes,double interval=2.) {
  CityElevationContours out;
  if(!std::isfinite(interval) || interval<=0)return out;
  std::set<std::array<int64_t,6>> seen;
  const auto clip=[](const std::vector<Position> &ring,double level,bool above) {
    std::vector<Position> result;
    const auto inside=[&](Position p){return above?p.y>=level:p.y<=level;};
    for(size_t i=0;i<ring.size();++i) {
      auto a=ring[i],b=ring[(i+1)%ring.size()];bool ia=inside(a),ib=inside(b);
      if(ia)result.push_back(a);
      if(ia!=ib) {
        // Canonical endpoint ordering avoids last-bit differences in reverse edges.
        if(std::tie(a.x,a.z,a.y)>std::tie(b.x,b.z,b.y))std::swap(a,b);
        const double t=(level-a.y)/(b.y-a.y);
        result.push_back({a.x+(b.x-a.x)*t,level,a.z+(b.z-a.z)*t});
      }
    }
    std::vector<Position> clean;
    for(auto p:result)if(clean.empty() || std::hypot(p.x-clean.back().x,p.z-clean.back().z)>1e-8)clean.push_back(p);
    if(clean.size()>1 && std::hypot(clean.front().x-clean.back().x,clean.front().z-clean.back().z)<1e-8)clean.pop_back();
    return clean;
  };
  for(const auto &plane:planes) {
    const double lo=std::min({plane[0].y,plane[1].y,plane[2].y}),hi=std::max({plane[0].y,plane[1].y,plane[2].y});
    if(hi-lo<1e-8){out.bands.push_back({plane.begin(),plane.end()});continue;}
    const auto first=int64_t(std::floor(lo/interval)),last=int64_t(std::floor(hi/interval));
    if(last-first>4096)continue; // Bounded malformed-input guard.
    for(auto k=first;k<=last;++k) {
      auto band=clip({plane.begin(),plane.end()},k*interval,true);
      band=clip(band,(k+1)*interval,false);
      if(band.size()<3)continue;
      double area=0;for(size_t j=0;j<band.size();++j)area+=band[j].x*band[(j+1)%band.size()].z-band[j].z*band[(j+1)%band.size()].x;
      if(std::abs(area)<1e-9)continue;
      for(size_t j=0;j<band.size();++j) {
        auto a=band[j],b=band[(j+1)%band.size()];
        if(std::abs(a.y-b.y)>1e-8 || std::abs(a.y/interval-std::round(a.y/interval))>1e-8)continue;
        if(std::tie(a.x,a.z)>std::tie(b.x,b.z))std::swap(a,b);
        std::array<int64_t,6> key{int64_t(std::llround(a.x*1e6)),int64_t(std::llround(a.y*1e6)),int64_t(std::llround(a.z*1e6)),
                                  int64_t(std::llround(b.x*1e6)),int64_t(std::llround(b.y*1e6)),int64_t(std::llround(b.z*1e6))};
        if(seen.insert(key).second)out.lines.push_back({a,b});
      }
      out.bands.push_back(std::move(band));
    }
  }
  return out;
}
}
