#pragma once
#include "WorldGenerationContracts.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <set>

namespace procedural::world::rewrite {
// Structural relief from the actual world planning cells. No independent noise
// seeds, raster stripes, or synthetic replacement Voronoi diagram.
struct CityLandformSettings {
  double reliefMetres = 0; // Opt in; zero preserves the world's original surface.
  unsigned shelves = 4;
  double summitCoreFraction = .45; // Keep the anchor inside the inhabited core.
  double summitRadiusMetres = 60; // Reserve a broad crown, not a one-cell peak.
  double shoulderFraction = .55; // Outer fraction of each cell joining its neighbours.
  double facetPeakBoost = 1.6; // Strengthen hill/basin silhouettes without moving shared edges.
  double facetCrownFraction = .28; // Zero makes angular peaks; up to .6 creates broad polygon crowns.
};
struct CityLandformCell {
  uint64_t id = 0;
  std::vector<Position> boundary;
  Position centre;
  std::vector<size_t> vertices, neighbours;
  uint64_t uphill = 0; // Topological connection; NOT a certified walkable route.
  unsigned shelf = 0;
  double uplift = 0, minX = 0, maxX = 0, minZ = 0, maxZ = 0;
};
struct CityLandformPlan {
  std::vector<std::vector<Position>> elevationContours; // Each closed ring owns one absolute height.
  std::vector<CityLandformCell> cells;
  std::vector<Position> vertices; // y = shared uplift, not absolute altitude.
  uint64_t summit = 0, entrance = 0;
  double reliefMetres = 0, shoulderFraction = .55, facetCrownFraction = .28, facetPeakBoost = 1.6;
};
namespace landform_detail {
inline double cross(Position a, Position b, Position p) { return (b.x-a.x)*(p.z-a.z)-(b.z-a.z)*(p.x-a.x); }
inline double distance(Position a, Position b) { return std::hypot(a.x-b.x,a.z-b.z); }
inline std::vector<double> distances(const CityLandformPlan &p, size_t root, std::vector<size_t> *parents=nullptr) {
  std::vector<double> d(p.cells.size(),INFINITY); d[root]=0;
  if(parents)parents->assign(p.cells.size(),SIZE_MAX);
  using Entry=std::pair<double,size_t>; std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> q; q.push({0,root});
  while(!q.empty()) { auto [cost,i]=q.top();q.pop();if(cost>d[i])continue;
    for(auto j:p.cells[i].neighbours) { const double next=cost+distance(p.cells[i].centre,p.cells[j].centre);
      if(next+1e-8<d[j]) {d[j]=next;if(parents)(*parents)[j]=i;q.push({next,j});}
    }
  }return d;
}
}
// Exact corridor/convex-cell overlap, including a river crossing between all
// sampled corners and an entire lake boundary contained inside a large cell.
inline bool cityLandformTouchesWater(const std::vector<Position> &ring,Position a,Position b,double halfWidth) {
  using namespace landform_detail;
  if(ring.size()<3)return false;
  auto pointSegment=[](Position p,Position u,Position v) {
    const double dx=v.x-u.x,dz=v.z-u.z,len2=dx*dx+dz*dz;
    const double t=len2>0?std::clamp(((p.x-u.x)*dx+(p.z-u.z)*dz)/len2,0.,1.):0.;
    return std::hypot(p.x-u.x-dx*t,p.z-u.z-dz*t);
  };
  bool positive=false,negative=false;
  for(size_t i=0;i<ring.size();++i) {const double d=cross(ring[i],ring[(i+1)%ring.size()],a);positive|=d>1e-8;negative|=d< -1e-8;}
  if(!(positive&&negative))return true;
  for(size_t i=0;i<ring.size();++i) {
    const auto u=ring[i],v=ring[(i+1)%ring.size()];
    if(std::max(a.x,b.x)+halfWidth<std::min(u.x,v.x) || std::min(a.x,b.x)-halfWidth>std::max(u.x,v.x) ||
       std::max(a.z,b.z)+halfWidth<std::min(u.z,v.z) || std::min(a.z,b.z)-halfWidth>std::max(u.z,v.z))continue;
    if(cross(a,b,u)*cross(a,b,v)<0 && cross(u,v,a)*cross(u,v,b)<0)return true;
    if(std::min({pointSegment(a,u,v),pointSegment(b,u,v),pointSegment(u,a,b),pointSegment(v,a,b)})<=halfWidth+1e-7)return true;
  }return false;
}
// Requires convex, non-overlapping cells (the world Voronoi contract). T junctions
// are split before assigning shared heights, so cell traversal order cannot open seams.
inline bool planCityLandforms(std::vector<CityLandformCell> input,
    const std::function<double(double,double)> &ground, Position focus,
    const CityLandformSettings &s, CityLandformPlan &out, std::string *error=nullptr) {
  using namespace landform_detail;
  auto fail=[&](const char *why){if(error)*error=why;return false;};
  if(!ground || !std::isfinite(focus.x) || !std::isfinite(focus.z) || input.empty() || input.size()>2048 || !std::isfinite(s.reliefMetres) || s.reliefMetres<0 || s.reliefMetres>120 ||
      s.shelves<2 || s.shelves>12 || !std::isfinite(s.summitCoreFraction) || s.summitCoreFraction<.15 || s.summitCoreFraction>.75 || !std::isfinite(s.summitRadiusMetres) || s.summitRadiusMetres<0 || s.summitRadiusMetres>500 || !std::isfinite(s.shoulderFraction) || s.shoulderFraction<.2 || s.shoulderFraction>.9 || !std::isfinite(s.facetCrownFraction) || s.facetCrownFraction<0 || s.facetCrownFraction>.6 || !std::isfinite(s.facetPeakBoost) || s.facetPeakBoost<1 || s.facetPeakBoost>3)
    return fail("invalid landform cells or settings");
  std::sort(input.begin(),input.end(),[](auto &a,auto &b){return a.id<b.id;});
  CityLandformPlan p;p.reliefMetres=s.reliefMetres;p.shoulderFraction=s.shoulderFraction;p.facetCrownFraction=s.facetCrownFraction;p.facetPeakBoost=s.facetPeakBoost;
  std::map<std::pair<int64_t,int64_t>,size_t> vertexIndex;
  for(size_t i=0;i<input.size();++i) {
    auto &c=input[i];if(!c.id || (i && input[i-1].id==c.id) || c.boundary.size()<3)return fail("invalid landform cell identity");
    c.vertices.clear();c.neighbours.clear();c.centre={};c.uplift=0;c.shelf=0;c.uphill=0;
    c.minX=c.minZ=INFINITY;c.maxX=c.maxZ=-INFINITY;
    double area=0;
    for(size_t j=0;j<c.boundary.size();++j) {
      auto v=c.boundary[j],w=c.boundary[(j+1)%c.boundary.size()];
      if(!std::isfinite(v.x)||!std::isfinite(v.z)||std::abs(v.x)>1e8||std::abs(v.z)>1e8)return fail("invalid landform vertex");
      area+=v.x*w.z-v.z*w.x;c.centre.x+=v.x;c.centre.z+=v.z;
      c.minX=std::min(c.minX,v.x);c.maxX=std::max(c.maxX,v.x);c.minZ=std::min(c.minZ,v.z);c.maxZ=std::max(c.maxZ,v.z);
      auto key=std::make_pair(int64_t(std::llround(v.x*100000)),int64_t(std::llround(v.z*100000)));
      if(!vertexIndex.count(key)) {vertexIndex[key]=p.vertices.size();p.vertices.push_back({v.x,0,v.z});}
    }
    if(std::abs(area)<1e-6)return fail("degenerate landform cell");
    if(area<0)std::reverse(c.boundary.begin(),c.boundary.end());
    for(size_t j=0;j<c.boundary.size();++j)if(cross(c.boundary[j],c.boundary[(j+1)%c.boundary.size()],c.boundary[(j+2)%c.boundary.size()]) < -1e-5)
      return fail("landform cells must be convex");
    c.centre.x/=c.boundary.size();c.centre.z/=c.boundary.size();
    if(!std::isfinite(ground(c.centre.x,c.centre.z)))return fail("non-finite landform ground");
  }
  // Snap every participant to the same canonical position, not just its ID.
  for(auto &c:input) {
    c.centre={};c.minX=c.minZ=INFINITY;c.maxX=c.maxZ=-INFINITY;
    for(auto &v:c.boundary) {
      const auto key=std::make_pair(int64_t(std::llround(v.x*100000)),int64_t(std::llround(v.z*100000)));
      v=p.vertices[vertexIndex.at(key)];c.centre.x+=v.x;c.centre.z+=v.z;
      c.minX=std::min(c.minX,v.x);c.maxX=std::max(c.maxX,v.x);c.minZ=std::min(c.minZ,v.z);c.maxZ=std::max(c.maxZ,v.z);
    }
    c.centre.x/=c.boundary.size();c.centre.z/=c.boundary.size();
  }
  // Conform shared edges, including a large cell adjoining several small cells.
  for(auto &c:input) {
    for(size_t j=0;j<c.boundary.size();++j) {
      auto a=c.boundary[j],b=c.boundary[(j+1)%c.boundary.size()];const double len=distance(a,b);
      if(len<1e-6)return fail("zero length landform edge");
      std::vector<std::pair<double,size_t>> on;
      for(size_t k=0;k<p.vertices.size();++k) {auto v=p.vertices[k];const double t=((v.x-a.x)*(b.x-a.x)+(v.z-a.z)*(b.z-a.z))/(len*len);
        if(t>=-1e-8 && t<1-1e-8 && std::abs(cross(a,b,v))<=1e-6*len)on.push_back({t,k});
      }
      std::sort(on.begin(),on.end());for(auto [t,k]:on)c.vertices.push_back(k);
    }
    c.boundary.clear();for(auto v:c.vertices)c.boundary.push_back(p.vertices[v]);
  }
  p.cells=std::move(input);
  using Edge=std::pair<size_t,size_t>;std::map<Edge,std::vector<size_t>> edges;
  std::vector<std::set<size_t>> incident(p.vertices.size());std::vector<bool> boundary(p.vertices.size(),false),rim(p.cells.size(),false);
  for(size_t i=0;i<p.cells.size();++i) {auto &c=p.cells[i];for(size_t j=0;j<c.vertices.size();++j) {
    size_t a=c.vertices[j],b=c.vertices[(j+1)%c.vertices.size()];edges[std::minmax(a,b)].push_back(i);incident[a].insert(i);
  }}
  for(auto &[edge,owners]:edges) {
    if(owners.size()>2)return fail("non-manifold landform edge");
    if(owners.size()==1) {boundary[edge.first]=boundary[edge.second]=true;rim[owners[0]]=true;}
    else {p.cells[owners[0]].neighbours.push_back(owners[1]);p.cells[owners[1]].neighbours.push_back(owners[0]);}
  }
  size_t root=0;for(size_t i=1;i<p.cells.size();++i)if(distance(p.cells[i].centre,focus)<distance(p.cells[root].centre,focus))root=i;
  auto reach=distances(p,root);size_t entrance=root;double lowest=INFINITY;
  for(size_t i=0;i<p.cells.size();++i)if(std::isfinite(reach[i])&&rim[i]) {
    const auto c=p.cells[i].centre;const double h=ground(c.x,c.z);if(h<lowest){lowest=h;entrance=i;}
  }
  auto fromEntry=distances(p,entrance);double longest=1,minH=INFINITY,maxH=-INFINITY;
  for(size_t i=0;i<p.cells.size();++i)if(std::isfinite(fromEntry[i])) {
    longest=std::max(longest,fromEntry[i]);auto c=p.cells[i].centre;double h=ground(c.x,c.z);minH=std::min(minH,h);maxH=std::max(maxH,h);
  }
  double urbanRadius=0;
  for(size_t i=0;i<p.cells.size();++i)if(std::isfinite(fromEntry[i]))urbanRadius=std::max(urbanRadius,distance(p.cells[i].centre,focus));
  size_t summit=root;double best=-INFINITY;
  for(size_t i=0;i<p.cells.size();++i)if(std::isfinite(fromEntry[i])) {
    auto c=p.cells[i].centre;
    if(i!=root && distance(c,focus)>urbanRadius*s.summitCoreFraction)continue;
    const double score=.65*fromEntry[i]/longest+.35*(ground(c.x,c.z)-minH)/std::max(1.,maxH-minH)-(rim[i]?.6:0.);
    if(score>best){best=score;summit=i;}
  }
  std::vector<size_t> parents;auto fromSummit=distances(p,summit,&parents);double extent=1;
  for(double d:fromSummit)if(std::isfinite(d))extent=std::max(extent,d);
  const double crown=std::min(s.summitRadiusMetres,extent*.3);
  for(size_t i=0;i<p.cells.size();++i)if(std::isfinite(fromSummit[i])) {
    const double descent=std::max(0.,fromSummit[i]-crown)/std::max(1.,extent-crown);
    auto &c=p.cells[i];c.shelf=unsigned(std::floor(std::clamp(1-descent,0.,1.)*(s.shelves-1)+1e-8));
    c.uplift=s.reliefMetres*c.shelf/(s.shelves-1);
    if(parents[i]!=SIZE_MAX)c.uphill=p.cells[parents[i]].id;
  }
  for(size_t i=0;i<p.vertices.size();++i)if(!boundary[i]) {
    double sum=0;for(auto cell:incident[i])sum+=p.cells[cell].uplift;
    p.vertices[i].y=sum/std::max(size_t(1),incident[i].size());
  }
  p.summit=p.cells[summit].id;p.entrance=p.cells[entrance].id;out=std::move(p);if(error)error->clear();return true;
}
// Preference only; downstream engineering must still certify width and grade.
inline double cityLandformRouteCost(Position a, Position b,
    const std::function<double(double,double)> &ground,double maximumGrade) {
  const double length=landform_detail::distance(a,b);
  if(length<1e-6)return 0;
  const unsigned samples=std::max(2u,unsigned(std::ceil(length/5.)));
  double previous=ground(a.x,a.z),excess=0;
  for(unsigned k=1;k<=samples;++k) {
    const double t=double(k)/samples;
    const double height=ground(a.x+(b.x-a.x)*t,a.z+(b.z-a.z)*t);
    if(!std::isfinite(height)||!std::isfinite(previous))return INFINITY;
    const double grade=std::abs(height-previous)/(length/samples);
    excess+=std::pow(std::max(0.,grade-maximumGrade),2);previous=height;
  }
  return length*(1+40*excess/samples);
}
inline std::function<double(double,double)> cityLandformSurface(
    std::function<double(double,double)> ground, const CityLandformPlan &plan) {
  auto p=std::make_shared<const CityLandformPlan>(plan);
  return [ground=std::move(ground),p](double x,double z) {
    const double base=ground(x,z);Position v{x,0,z};
    for(const auto &c:p->cells) {
      if(x<c.minX-1e-7||x>c.maxX+1e-7||z<c.minZ-1e-7||z>c.maxZ+1e-7)continue;
      for(size_t j=0;j<c.vertices.size();++j) {
        auto a=p->vertices[c.vertices[j]],b=p->vertices[c.vertices[(j+1)%c.vertices.size()]];
        const double area=landform_detail::cross(c.centre,a,b);if(area<1e-9)continue;
        const double u=landform_detail::cross(c.centre,v,b)/area,w=landform_detail::cross(c.centre,a,v)/area;
        if(u< -1e-8 || w< -1e-8 || u+w>1+1e-8)continue;
        const double radial=std::max(0.,u+w),edge=radial>1e-9?(u*a.y+w*b.y)/radial:c.uplift;
        double blend=std::clamp((radial-(1-p->shoulderFraction))/p->shoulderFraction,0.,1.);blend*=blend*(3-2*blend);
        return base+c.uplift+(edge-c.uplift)*blend;
      }
    }return base;
  };
}
} // namespace procedural::world::rewrite
