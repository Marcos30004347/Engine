#pragma once
#include "WorldCityCompletion.hpp"
#include "WorldCityRelief.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace procedural::world::rewrite {
// A first engineered reach: flat water, masonry banks, retained public quays.
// Only nearly straight, broad, gently graded routes qualify. No hydrology is
// invented between disconnected reaches; the ends are contained by stone sills.
inline bool planCityWaterway(CityCompletionPlan &city, CityReliefPlan &relief,
                            uint64_t requestedRoute=0)
{
  if(!city.waterways.empty() || !relief.openGround)return false;
  const auto original=relief.accessGround?relief.accessGround:relief.openGround;
  std::vector<CitySmoothedRoute> candidates=city.routes;
  // A long street may contain a safe level reach even if its complete route
  // climbs a hill. Keep original ownership while testing contiguous subpaths.
  for(auto route:city.routes) {
    if(requestedRoute || route.halfWidth<2.6)continue;
    std::vector<Position> samples;
    for(size_t k=1;k<route.path.size();++k){const auto a=route.path[k-1],b=route.path[k];const int n=std::max(1,int(std::ceil(std::hypot(b.x-a.x,b.z-a.z)/20.)));
      for(int i=0;i<n;++i)samples.push_back({a.x+(b.x-a.x)*i/n,0,a.z+(b.z-a.z)*i/n});}
    if(!route.path.empty())samples.push_back(route.path.back());route.path=std::move(samples);
    for(size_t first=0;first+1<route.path.size();++first)for(double limit:{35.,60.,100.}) {
      auto part=route;part.path={route.path[first]};double run=0;
      for(size_t k=first+1;k<route.path.size();++k){auto a=route.path[k-1],b=route.path[k];double len=std::hypot(b.x-a.x,b.z-a.z);if(len<1e-6)continue;
        if(run+len>=limit){const double t=(limit-run)/len;part.path.push_back({a.x+(b.x-a.x)*t,0,a.z+(b.z-a.z)*t});break;}
        part.path.push_back(b);run+=len;
      }
      if(part.path.size()>1)candidates.push_back(std::move(part));
    }
  }
  std::stable_sort(candidates.begin(),candidates.end(),[](const auto &a,const auto &b){
    const auto length=[](const auto &r){return r.path.size()<2?0.:std::hypot(r.path.back().x-r.path.front().x,r.path.back().z-r.path.front().z);};
    return length(a)>length(b);
  });
  for(const auto &route:candidates) {
    if(requestedRoute && route.id!=requestedRoute)continue;
    if(route.path.size()<2 || !std::isfinite(route.halfWidth) || route.halfWidth<2.6 || route.kind==CityRoadKind::FieldTrack)continue;
    CityWaterway w;w.route=route.id;w.edges=route.edges;w.a=route.path.front();w.b=route.path.back();
    const double dx=w.b.x-w.a.x,dz=w.b.z-w.a.z,L=std::hypot(dx,dz);
    if(L<30 || !std::isfinite(L))continue;
    w.halfWidth=route.halfWidth-1.6;w.bankWidth=.4;
    const auto coordinates=[&](Position p){return std::pair<double,double>{((p.x-w.a.x)*dx+(p.z-w.a.z)*dz)/L,((p.z-w.a.z)*dx-(p.x-w.a.x)*dz)/L};};
    bool clear=true;
    for(auto p:route.path)if(std::abs(coordinates(p).second)>.05)clear=false;
    double low=std::numeric_limits<double>::max(),high=-low;
    for(double t=0;t<=L;t+=.5){const double h=original(w.a.x+dx*t/L,w.a.z+dz*t/L);if(!std::isfinite(h)){clear=false;break;}low=std::min(low,h);high=std::max(high,h);}
    if(!clear || high-low>.6)continue;
    w.waterLevel=low-1.1;w.bedLevel=w.waterLevel-1.2;
    // Do not remove lots or tunnel through independent raised blocks/stairs.
    for(const auto &r:city.skeleton.regions)if(r.kind==CityRegionKind::Lot)
      for(size_t k=0;k<r.boundary.size();++k){auto a=r.boundary[k],b=r.boundary[(k+1)%r.boundary.size()];const int n=std::max(1,int(std::ceil(std::hypot(b.x-a.x,b.z-a.z))));for(int i=0;i<=n;++i){auto q=coordinates({a.x+(b.x-a.x)*i/n,0,a.z+(b.z-a.z)*i/n});if(q.first>=0 && q.first<=L && std::abs(q.second)<route.halfWidth)clear=false;}}
    for(const auto &r:city.skeleton.regions)if(r.kind==CityRegionKind::Lot && r.boundary.size()>2) {
      const Position mid{(w.a.x+w.b.x)*.5,0,(w.a.z+w.b.z)*.5};bool inside=false;
      for(size_t i=0,j=r.boundary.size()-1;i<r.boundary.size();j=i++) {
        const auto a=r.boundary[i],b=r.boundary[j];
        if((a.z>mid.z)!=(b.z>mid.z) && mid.x<(b.x-a.x)*(mid.z-a.z)/(b.z-a.z)+a.x)inside=!inside;
      }
      if(inside)clear=false;
    }
    for(const auto &p:relief.platforms){
      double minT=1e100,maxT=-1e100,minC=1e100,maxC=-1e100;
      for(auto v:p.boundary){auto q=coordinates(v);minT=std::min(minT,q.first);maxT=std::max(maxT,q.first);minC=std::min(minC,q.second);maxC=std::max(maxC,q.second);}
      if(maxT>-10 && minT<L+10 && maxC>-route.halfWidth-10 && minC<route.halfWidth+10)clear=false;
    }
    for(const auto &s:relief.stairs){const int n=std::max(1,int(std::ceil(std::hypot(s.b.x-s.a.x,s.b.z-s.a.z))));for(int i=0;i<=n;++i){auto q=coordinates({s.a.x+(s.b.x-s.a.x)*i/n,0,s.a.z+(s.b.z-s.a.z)*i/n});if(q.first>-s.halfWidth && q.first<L+s.halfWidth && std::abs(q.second)<route.halfWidth+s.halfWidth)clear=false;}}
    for(const auto &t:city.towers){auto q=coordinates(t.position);if(q.first>-t.radius && q.first<L+t.radius && std::abs(q.second)<route.halfWidth+t.radius)clear=false;}
    for(const auto &g:city.gatehouses){auto q=coordinates(g.position);if(q.first>-g.depth && q.first<L+g.depth && std::abs(q.second)<route.halfWidth+g.openingWidth)clear=false;}
    if(!clear)continue;
    // Intersections are bridges, not submerged roads. Shallow-angle overlaps
    // cannot fit a compact crossing and leave the candidate unchanged.
    for(const auto &other:city.routes)if(other.id!=route.id)for(size_t k=1;k<other.path.size();++k){
      auto a=coordinates(other.path[k-1]),b=coordinates(other.path[k]);
      if(a.second*b.second>0 || std::abs(a.second-b.second)<1e-6)continue;
      const double t=a.second/(a.second-b.second),station=a.first+t*(b.first-a.first);
      if(station<0 || station>L)continue;
      const double len=std::hypot(b.first-a.first,b.second-a.second),sine=std::abs(b.second-a.second)/len;
      if(sine<.65){clear=false;continue;}
      const double cosine=std::abs(b.first-a.first)/len;
      const double half=(other.halfWidth+.35+(w.halfWidth+w.bankWidth)*cosine)/sine;
      w.bridges.push_back({std::max(0.,station-half),std::min(L,station+half)});
    }
    if(!clear)continue;
    w.bridges.push_back({0,1.5});w.bridges.push_back({L-1.5,L});
    std::sort(w.bridges.begin(),w.bridges.end());
    std::vector<std::pair<double,double>> merged;
    for(auto b:w.bridges){if(!merged.empty() && b.first<=merged.back().second+.5)merged.back().second=std::max(merged.back().second,b.second);else merged.push_back(b);}
    w.bridges=std::move(merged);
    for(const auto &wall:city.wallSegments){auto a=coordinates(wall.a),b=coordinates(wall.b);
      if(a.second*b.second>0 || std::abs(a.second-b.second)<1e-6)continue;
      double t=a.second/(a.second-b.second),station=a.first+t*(b.first-a.first);
      if(station<=0 || station>=L)continue;
      const double sine=std::abs(b.second-a.second)/std::hypot(b.first-a.first,b.second-a.second);
      if(sine<.65 || wall.height<4){clear=false;break;}
      const double half=(route.halfWidth+.2)/sine; // Include both public quays.
      const double run=std::hypot(wall.b.x-wall.a.x,wall.b.z-wall.a.z);
      if(t*run<half+1 || (1-t)*run<half+1){clear=false;break;}
      w.portals.push_back({wall.id,t*run-half,t*run+half});
    }
    if(!clear)continue;
    // Geometry owns the vertical faces. Runtime ground is recessed beneath
    // the bed; the city surface mesh is cut exactly at the same footprint.
    relief.accessGround=[original,w,dx,dz,L](double x,double z){
      double t=((x-w.a.x)*dx+(z-w.a.z)*dz)/L,c=((z-w.a.z)*dx-(x-w.a.x)*dz)/L;
      return t>=0 && t<=L && std::abs(c)<=w.halfWidth+w.bankWidth ? w.bedLevel-.25:original(x,z);
    };
    w.bankGround=original;
    city.waterways.push_back(std::move(w));return true;
  }
  return false;
}
} // namespace procedural::world::rewrite
