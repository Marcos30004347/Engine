#pragma once
#include "WorldCityStreetProfiles.hpp"
#include "WorldCityConstruction.hpp"
#include "WorldSurfaceArrangement.hpp"
#include <memory>

namespace procedural::world::rewrite {
struct CityTerrainBench {
  uint64_t id=0, road=0;
  CityConstructionId owner;
  std::vector<Position> boundary; // Convex CCW; horizontal building/court bench.
  double offset=0; // Above the closest solved road station; never sampled ground.
};
struct CityTerrainReservation {
  uint64_t id=0;
  std::vector<Position> boundary; // Convex protected water or occupied ground.
};
struct CityTerrainRouteCandidate {
  uint64_t id=0;
  CitySkeleton roads;
  std::vector<CityTerrainBench> benches;
};
struct CityTerrainRouteSettings {
  CityReliefSettings streets;
  double maximumCut=4, maximumFill=2, maximumEarthwork=100000;
  double transitionWidth=3, sampleSpacing=1, maximumBenchReach=20;
  size_t maximumCandidates=32, maximumSamples=250000;
};
struct CityTerrainEdit {
  uint64_t id=0;
  CityConstructionId owner;
  SurfacePlane plane;
  std::vector<Position> boundary;
};
struct CityTerrainRouteRejection { uint64_t candidate=0;std::string reason; };
struct CityTerrainRoutePlan {
  uint64_t revision=0,candidate=0;
  CitySkeleton roads;
  CityStreetProfilePlan profiles;
  std::vector<CityTerrainEdit> edits;
  std::vector<std::array<Position,2>> constraints;
  std::vector<CityTerrainRouteRejection> rejections;
  double maximumCut=0,maximumFill=0,earthwork=0,score=0;
  size_t samples=0;
  std::function<double(double,double)> source,ground;
};
namespace city_terrain_detail {
inline double cross(Position a,Position b,Position c) {return (b.x-a.x)*(c.z-a.z)-(b.z-a.z)*(c.x-a.x);}
inline double distance(Position p,Position a,Position b) {
  const double dx=b.x-a.x,dz=b.z-a.z,d=dx*dx+dz*dz;
  const double t=d>1e-12?std::clamp(((p.x-a.x)*dx+(p.z-a.z)*dz)/d,0.,1.):0;
  return std::hypot(p.x-a.x-t*dx,p.z-a.z-t*dz);
}
inline bool inside(const std::vector<Position> &poly,Position p) {
  for(size_t i=0;i<poly.size();++i)if(cross(poly[i],poly[(i+1)%poly.size()],p)<-1e-7)return false;
  return poly.size()>=3;
}
inline bool convex(const std::vector<Position> &p) {
  if(p.size()<3||p.size()>256)return false;
  double area=0;
  for(size_t i=0;i<p.size();++i) {
    auto a=p[i],b=p[(i+1)%p.size()],c=p[(i+2)%p.size()];
    if(!std::isfinite(a.x+a.y+a.z)||cross(a,b,c)<-1e-8)return false;
    area+=a.x*b.z-a.z*b.x;
  }
  return area>1e-8;
}
inline std::vector<Position> intersect(std::vector<Position> a,const std::vector<Position> &b) {
  for(size_t i=0;i<b.size()&&!a.empty();++i) {
    std::vector<Position> out;const auto u=b[i],v=b[(i+1)%b.size()];
    for(size_t j=0;j<a.size();++j) {
      auto p=a[j],q=a[(j+1)%a.size()];double dp=cross(u,v,p),dq=cross(u,v,q);
      if(dp>=-1e-8)out.push_back(p);
      if((dp<0)!=(dq<0)) {double t=dp/(dp-dq);out.push_back({p.x+(q.x-p.x)*t,0,p.z+(q.z-p.z)*t});}
    }
    a=std::move(out);
  }
  return a;
}
inline double separation(const std::vector<Position> &a,const std::vector<Position> &b) {
  if(!intersect(a,b).empty())return 0;
  double d=INFINITY;
  for(auto p:a)for(size_t j=0;j<b.size();++j)d=std::min(d,distance(p,b[j],b[(j+1)%b.size()]));
  for(auto p:b)for(size_t j=0;j<a.size();++j)d=std::min(d,distance(p,a[j],a[(j+1)%a.size()]));
  return d;
}
inline double boundaryDistance(const std::vector<Position> &poly,Position p) {
  double d=INFINITY;for(size_t i=0;i<poly.size();++i)d=std::min(d,distance(p,poly[i],poly[(i+1)%poly.size()]));return d;
}
inline Position lerp(Position a,Position b,double t){return {a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t};}
// Split at all intersections, including collinear overlaps. Every shared XY
// station is then lifted once from the edited surface, not from old contours.
inline bool nodeConstraints(std::vector<std::array<Position,2>> lines,
    const std::function<double(double,double)> &height,std::vector<std::array<Position,2>> &out) {
  if(lines.size()>4096)return false;
  std::vector<std::vector<double>> cuts(lines.size(),{0,1});
  for(size_t i=0;i<lines.size();++i)for(size_t j=i+1;j<lines.size();++j) {
    auto a=lines[i][0],b=lines[i][1],c=lines[j][0],d=lines[j][1];
    const double ux=b.x-a.x,uz=b.z-a.z,vx=d.x-c.x,vz=d.z-c.z,det=ux*vz-uz*vx;
    if(std::abs(det)>1e-10) {
      double t=((c.x-a.x)*vz-(c.z-a.z)*vx)/det,s=((c.x-a.x)*uz-(c.z-a.z)*ux)/det;
      if(t>=0&&t<=1&&s>=0&&s<=1){cuts[i].push_back(t);cuts[j].push_back(s);}
    } else if(std::abs(cross(a,b,c))<1e-8) {
      double l=ux*ux+uz*uz,m=vx*vx+vz*vz;
      if(l>1e-12)for(auto p:{c,d}){double t=((p.x-a.x)*ux+(p.z-a.z)*uz)/l;if(t>0&&t<1)cuts[i].push_back(t);}
      if(m>1e-12)for(auto p:{a,b}){double t=((p.x-c.x)*vx+(p.z-c.z)*vz)/m;if(t>0&&t<1)cuts[j].push_back(t);}
    }
  }
  std::set<std::array<int64_t,4>> seen;out.clear();
  for(size_t i=0;i<lines.size();++i) {
    auto &t=cuts[i];std::sort(t.begin(),t.end());
    for(size_t k=1;k<t.size();++k) {
      auto a=lerp(lines[i][0],lines[i][1],t[k-1]),b=lerp(lines[i][0],lines[i][1],t[k]);
      auto key=[](Position p){return std::pair<int64_t,int64_t>{llround(p.x*1e6),llround(p.z*1e6)};};
      auto u=key(a),v=key(b);if(v<u)std::swap(u,v);if(u==v)continue;
      if(!seen.insert({u.first,u.second,v.first,v.second}).second)continue;
      a={u.first/1e6,0,u.second/1e6};b={v.first/1e6,0,v.second/1e6};
      a.y=height(a.x,a.z);b.y=height(b.x,b.z);if(!std::isfinite(a.y+b.y))return false;
      out.push_back({a,b});
    }
  }
  return true;
}
}
// All alternatives represent the same required route identities and anchors.
// Caller composes layout alternatives BEFORE assigning occupied plots. Bounded
// search chooses the least-earthwork valid alternative, never drops demand.
inline bool planCityTerrainRoutes(const std::vector<CityTerrainRouteCandidate> &alternatives,
    const std::function<double(double,double)> &source,const CityTerrainRouteSettings &settings,
    const std::vector<CityTerrainReservation> &protectedLand,
    const std::vector<std::array<Position,2>> &naturalContours,
    CityTerrainRoutePlan &result,std::vector<CityTerrainRouteRejection> &rejections,std::string *error=nullptr) {
  using namespace city_terrain_detail;
  rejections.clear();const auto fail=[&](const char *why){if(error)*error=why;return false;};
  if(!std::isfinite(settings.maximumBenchReach)||settings.maximumBenchReach<=0)return fail("invalid bench reach");
  if(!source||alternatives.empty()||!settings.maximumCandidates||alternatives.size()>settings.maximumCandidates ||
     !(settings.maximumCut>=0)||!(settings.maximumFill>=0)||!(settings.maximumEarthwork>=0)||
     !(settings.transitionWidth>0)||!(settings.sampleSpacing>=.1)||settings.sampleSpacing>2 || !settings.maximumSamples ||
     !std::isfinite(settings.maximumCut+settings.maximumFill+settings.maximumEarthwork+settings.transitionWidth+settings.sampleSpacing))
    return fail("invalid terrain-route settings");
  for(const auto &r:protectedLand)if(!convex(r.boundary))return fail("invalid protected polygon");
  for(const auto &line:naturalContours)for(auto p:line)if(!std::isfinite(p.x+p.y+p.z)||std::abs(p.x)>1e8||std::abs(p.z)>1e8)return fail("invalid contour constraint");
  auto candidates=alternatives;std::sort(candidates.begin(),candidates.end(),[](const auto &a,const auto &b){return a.id<b.id;});
  std::set<uint64_t> candidateIds;bool found=false;CityTerrainRoutePlan best;
  std::map<uint64_t,std::pair<uint64_t,uint64_t>> required;
  for(const auto &e:candidates.front().roads.roadEdges)required[e.id]={e.from,e.to};
  std::map<uint64_t,Position> anchors;
  for(const auto &n:candidates.front().roads.roadNodes)anchors[n.id]=n.position;
  std::map<uint64_t,CityConstructionId> benchOwners;
  for(const auto &b:candidates.front().benches)benchOwners[b.id]=b.owner;
  for(const auto &candidate:candidates) {
    if(!candidate.id||!candidateIds.insert(candidate.id).second)return fail("duplicate candidate identity");
    CityTerrainRoutePlan p;p.candidate=candidate.id;p.revision=candidate.roads.revision;p.roads=candidate.roads;p.source=source;
    std::string why;
    std::map<uint64_t,std::pair<uint64_t,uint64_t>> actual;
    for(const auto &e:candidate.roads.roadEdges)actual[e.id]={e.from,e.to};
    if(!p.revision || actual!=required || actual.size()!=candidate.roads.roadEdges.size())why="candidate changed required route identities";
    std::map<uint64_t,Position> candidateAnchors;
    for(const auto &n:candidate.roads.roadNodes) {
      if(!n.id||!candidateAnchors.emplace(n.id,n.position).second||!anchors.count(n.id)||
         !std::isfinite(n.position.x+n.position.z)||
         std::hypot(n.position.x-anchors[n.id].x,n.position.z-anchors[n.id].z)>1e-7)why="candidate changed required anchors";
    }
    if(candidateAnchors.size()!=anchors.size())why="candidate removed required anchors";
    std::map<uint64_t,CityConstructionId> candidateBenches;
    for(const auto &b:candidate.benches)candidateBenches[b.id]=b.owner;
    if(candidateBenches!=benchOwners)why="candidate changed required benches";
    for(const auto &e:candidate.roads.roadEdges) {
      if(!e.id||!std::isfinite(e.halfWidth)||e.halfWidth<=0)why="invalid route width or identity";
      if(e.path.size()>1 && (std::hypot(e.path.front().x-anchors[e.from].x,e.path.front().z-anchors[e.from].z)>1e-7 ||
          std::hypot(e.path.back().x-anchors[e.to].x,e.path.back().z-anchors[e.to].z)>1e-7))why="route endpoints differ from shared anchors";
    }
    if(why.empty())planCityStreetProfiles(candidate.roads,source,settings.streets,p.profiles,&why);
    uint64_t editId=0;
    std::map<uint64_t,double> widths;for(const auto &e:p.roads.roadEdges)widths[e.id]=e.halfWidth>0?e.halfWidth:1.5;
    if(why.empty())for(const auto &street:p.profiles.streets) {
      // Mitered cross-sections share exact heights at bends. Triangles span
      // the resulting quad, so graded turns do not overlap independent ribbons.
      std::vector<Position> left,right;
      for(size_t i=0;i<street.path.size();++i) {
        auto centre=street.path[i];Position prev=i?street.path[i-1]:centre,next=i+1<street.path.size()?street.path[i+1]:centre;
        double ax=centre.x-prev.x,az=centre.z-prev.z,bx=next.x-centre.x,bz=next.z-centre.z;
        double al=std::hypot(ax,az),bl=std::hypot(bx,bz);
        if(al<1e-8){ax=bx;az=bz;al=bl;}if(bl<1e-8){bx=ax;bz=az;bl=al;}
        if(al<1e-8){why="degenerate road ribbon";break;}
        ax/=al;az/=al;bx/=bl;bz/=bl;
        double nx=-az-bz,nz=ax+bx,nl=std::hypot(nx,nz);
        if(nl<1e-8){why="reversing road ribbon";break;}
        nx/=nl;nz/=nl;double alignment=nx*(-az)+nz*ax;
        if(alignment<.25){why="road turn requires a larger landing";break;}
        double width=widths[street.edge]/alignment;
        left.push_back({centre.x+nx*width,centre.y,centre.z+nz*width});
        right.push_back({centre.x-nx*width,centre.y,centre.z-nz*width});
      }
      if(!why.empty())break;
      for(size_t i=1;i<left.size();++i)for(auto face:{std::array<Position,3>{right[i-1],right[i],left[i]},std::array<Position,3>{right[i-1],left[i],left[i-1]}}) {
        auto a=face[0],b=face[1],c=face[2];double det=cross(a,b,c);
        if(det<=1e-8){why="road ribbon folds over itself";break;}
        double sx=((b.y-a.y)*(c.z-a.z)-(c.y-a.y)*(b.z-a.z))/det;
        double sz=((b.x-a.x)*(c.y-a.y)-(c.x-a.x)*(b.y-a.y))/det;
        if(std::hypot(sx,sz)>settings.streets.maximumStreetGrade+1e-7){why="road turn exceeds grade budget";break;}
        p.edits.push_back({++editId,{CityConstructionKind::Route,street.edge},{sx,sz,a.y-sx*a.x-sz*a.z},{a,b,c}});
      }
      if(!why.empty())break;
    }
    std::set<uint64_t> benchIds;
    if(why.empty())for(const auto &bench:candidate.benches) {
      if(!bench.id||!bench.owner.source||!benchIds.insert(bench.id).second||!convex(bench.boundary)||!std::isfinite(bench.offset)) {why="invalid building bench";break;}
      Position centre{};for(auto v:bench.boundary){centre.x+=v.x/bench.boundary.size();centre.z+=v.z/bench.boundary.size();}
      double nearest=INFINITY,level=0;
      for(const auto &road:p.profiles.streets)if(road.edge==bench.road)for(size_t k=1;k<road.path.size();++k) {
        auto a=road.path[k-1],b=road.path[k];double dx=b.x-a.x,dz=b.z-a.z,l=dx*dx+dz*dz;
        double t=l>0?std::clamp(((centre.x-a.x)*dx+(centre.z-a.z)*dz)/l,0.,1.):0;
        auto q=lerp(a,b,t);double d=std::hypot(centre.x-q.x,centre.z-q.z);
        if(d<nearest){nearest=d;level=q.y+bench.offset;}
      }
      if(!std::isfinite(nearest)||nearest>settings.maximumBenchReach){why="bench has no nearby serving route";break;}
      p.edits.push_back({++editId,bench.owner,{0,0,level},bench.boundary});
    }
    if(why.empty())for(size_t i=0;i<p.edits.size();++i) {
      const auto &a=p.edits[i];
      for(const auto &r:protectedLand)if(separation(a.boundary,r.boundary)<settings.transitionWidth-1e-7){why="terrain edit invades protected land; explicit crossing or different route required";break;}
      for(size_t j=0;j<i&&why.empty();++j)for(auto v:intersect(a.boundary,p.edits[j].boundary))
        if(std::abs(a.plane.height(v.x,v.z)-p.edits[j].plane.height(v.x,v.z))>1e-6){why="incompatible overlapping terrain edit heights";break;}
      if(!why.empty())break;
    }
    if(!why.empty()){rejections.push_back({candidate.id,why});continue;}
    auto edits=std::make_shared<const std::vector<CityTerrainEdit>>(p.edits);const double band=settings.transitionWidth;
    p.ground=[source,edits,band](double x,double z) {
      Position q{x,0,z};double base=source(x,z),weighted=base,total=1;
      for(const auto &e:*edits) {
        const double h=e.plane.height(x,z);
        if(inside(e.boundary,q))return h;
        double d=boundaryDistance(e.boundary,q);
        if(d<band) {
          // Weight tends to infinity at the core: other banks cannot create
          // a height jump at the edge of a solved road/bench.
          double w=(band-d)/std::max(d,1e-12);weighted+=w*h;total+=w;
        }
      }
      return weighted/total;
    };
    for(const auto &edit:p.edits)for(auto v:edit.boundary) {
      double delta=source(v.x,v.z)-p.ground(v.x,v.z);
      if(!std::isfinite(delta))why="nonfinite edit boundary sample";
      p.maximumCut=std::max(p.maximumCut,delta);p.maximumFill=std::max(p.maximumFill,-delta);
    }
    // Audit core and banks on a shared lattice, deduplicated between edits.
    // These are sampled earthwork bounds; source fields must be finite and
    // piecewise smooth at the configured resolution.
    std::set<std::pair<int64_t,int64_t>> samples;
    for(const auto &e:p.edits) {
      double x0=INFINITY,z0=INFINITY,x1=-INFINITY,z1=-INFINITY;
      for(auto v:e.boundary){x0=std::min(x0,v.x-band);x1=std::max(x1,v.x+band);z0=std::min(z0,v.z-band);z1=std::max(z1,v.z+band);}
      const double pitch=settings.sampleSpacing;
      if(!std::isfinite(x0+x1+z0+z1)||std::max({std::abs(x0),std::abs(x1),std::abs(z0),std::abs(z1)})>1e8 ||
         (x1-x0)/pitch*(z1-z0)/pitch>settings.maximumSamples){why="terrain sample budget exceeded";break;}
      for(int64_t x=int64_t(std::floor(x0/pitch));x<=int64_t(std::ceil(x1/pitch))&&why.empty();++x)
        for(int64_t z=int64_t(std::floor(z0/pitch));z<=int64_t(std::ceil(z1/pitch));++z) {
          Position q{x*pitch,0,z*pitch};if(!inside(e.boundary,q)&&boundaryDistance(e.boundary,q)>band)continue;
          if(!samples.insert({x,z}).second)continue;
          if(samples.size()>settings.maximumSamples){why="terrain sample budget exceeded";break;}
          double delta=source(q.x,q.z)-p.ground(q.x,q.z);
          if(!std::isfinite(delta)){why="nonfinite terrain edit sample";break;}
          p.maximumCut=std::max(p.maximumCut,delta);p.maximumFill=std::max(p.maximumFill,-delta);
          p.earthwork+=std::abs(delta)*pitch*pitch;
        }
    }
    p.samples=samples.size();
    if(why.empty()&&(p.maximumCut>settings.maximumCut+1e-7||p.maximumFill>settings.maximumFill+1e-7||p.earthwork>settings.maximumEarthwork))why="cut/fill budget exceeded";
    if(!why.empty()){rejections.push_back({candidate.id,why});continue;}
    std::vector<std::array<Position,2>> lines;
    for(const auto &e:p.edits)for(size_t i=0;i<e.boundary.size();++i)lines.push_back({e.boundary[i],e.boundary[(i+1)%e.boundary.size()]});
    // Keep natural contours only outside edit cores. Their intervening sections
    // are replaced by the edit boundary and canonical height, never buried.
    for(auto line:naturalContours) {
      std::vector<double> cuts{0,1};auto a=line[0],b=line[1];
      for(const auto &e:p.edits)for(size_t i=0;i<e.boundary.size();++i) {
        auto c=e.boundary[i],d=e.boundary[(i+1)%e.boundary.size()];
        double ux=b.x-a.x,uz=b.z-a.z,vx=d.x-c.x,vz=d.z-c.z,det=ux*vz-uz*vx;
        if(std::abs(det)<1e-10)continue;
        double t=((c.x-a.x)*vz-(c.z-a.z)*vx)/det,u=((c.x-a.x)*uz-(c.z-a.z)*ux)/det;
        if(t>0&&t<1&&u>=0&&u<=1)cuts.push_back(t);
      }
      std::sort(cuts.begin(),cuts.end());
      for(size_t i=1;i<cuts.size();++i) {
        auto mid=lerp(a,b,(cuts[i-1]+cuts[i])*.5);bool replaced=false;
        for(const auto &e:p.edits)replaced|=inside(e.boundary,mid);
        if(!replaced)lines.push_back({lerp(a,b,cuts[i-1]),lerp(a,b,cuts[i])});
      }
    }
    if(!nodeConstraints(lines,p.ground,p.constraints)){rejections.push_back({candidate.id,"constraint noding budget or height failure"});continue;}
    double length=0;for(const auto &r:p.profiles.streets)for(size_t i=1;i<r.path.size();++i)length+=std::hypot(r.path[i].x-r.path[i-1].x,r.path[i].z-r.path[i-1].z);
    p.score=p.earthwork+length;
    if(!found||p.score<best.score-1e-8){found=true;best=std::move(p);}
  }
  if(!found)return fail("no terrain-route candidate satisfies constraints");
  best.rejections=rejections;result=std::move(best);if(error)error->clear();return true;
}
// Small bounded grammar for a single troublesome route. Endpoints and request
// IDs stay fixed; protected-land and earthwork checks decide which side fits.
inline std::vector<CityTerrainRouteCandidate> cityRouteDetours(
    CityTerrainRouteCandidate base,uint64_t edge,const std::vector<double> &offsets) {
  std::vector<CityTerrainRouteCandidate> out{base};uint64_t next=base.id;
  for(double offset:offsets)for(double side:{-1.,1.}) {
    if(!std::isfinite(offset)||offset<=0)continue;
    auto candidate=base;candidate.id=++next;
    auto route=std::find_if(candidate.roads.roadEdges.begin(),candidate.roads.roadEdges.end(),[&](const auto &e){return e.id==edge;});
    if(route==candidate.roads.roadEdges.end()||route->path.size()<2)continue;
    auto a=route->path.front(),b=route->path.back();double dx=b.x-a.x,dz=b.z-a.z,len=std::hypot(dx,dz);
    if(len<1e-8)continue;
    auto p=city_terrain_detail::lerp(a,b,.25),q=city_terrain_detail::lerp(a,b,.75);
    p.x-=dz/len*side*offset;p.z+=dx/len*side*offset;
    q.x-=dz/len*side*offset;q.z+=dx/len*side*offset;
    route->path={a,p,q,b};out.push_back(std::move(candidate));
  }
  return out;
}

}
