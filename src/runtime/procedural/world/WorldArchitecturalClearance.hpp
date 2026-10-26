#pragma once
#include "WorldDwellingPlan.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace procedural::world::rewrite {
inline double architecturalSegmentDistance(Position p,Position a,Position b) {
  const double dx=b.x-a.x,dz=b.z-a.z,l=dx*dx+dz*dz;
  const double t=l>1e-12?std::clamp(((p.x-a.x)*dx+(p.z-a.z)*dz)/l,0.,1.):0.;
  return std::hypot(p.x-a.x-t*dx,p.z-a.z-t*dz);
}
inline double gallerySeparation(Position a,Position b,Position c,Position d) {
  const auto cross=[](Position p,Position q,Position r){return (q.x-p.x)*(r.z-p.z)-(q.z-p.z)*(r.x-p.x);};
  if(cross(a,b,c)*cross(a,b,d)<0 && cross(c,d,a)*cross(c,d,b)<0)return 0.;
  return std::min({architecturalSegmentDistance(a,c,d),architecturalSegmentDistance(b,c,d),
                   architecturalSegmentDistance(c,a,b),architecturalSegmentDistance(d,a,b)});
}
// A buttress carries only the uninterrupted wall stack immediately behind it.
// Test the whole metre-wide attachment and every roof above its projecting foot.
inline double wallButtressHeight(const DwellingPlan &plan,Position p,double ux,double uz) {
  double top=1e30;
  for(double offset:{-.48,0.,.48}) {
    Position q{p.x+ux*offset,0,p.z+uz*offset};
    std::vector<std::pair<double,double>> spans;
    for(const auto &wall:plan.walls)if(wall.exterior && wall.kind==WallKind::Solid &&
        architecturalSegmentDistance(q,wall.a,wall.b)<.08)
      spans.push_back({wall.a.y,wall.a.y+wall.height});
    std::sort(spans.begin(),spans.end());double height=0;
    for(auto [lo,hi]:spans){if(lo>height+.08)break;height=std::max(height,hi);}
    top=std::min(top,height);
  }
  if(top<5.)return 0.; // A low wing does not need monumental buttresses.
  double height=std::min(top*.8,top-.35);
  for(const auto &opening:plan.openings)if(opening.exterior &&
      architecturalSegmentDistance(p,opening.a,opening.b)<.8)
    height=std::min(height,opening.a.y+opening.base-.2);
  for(double across:{-.5,0.,.5})for(double depth:{0.,.3,.9,1.8}) {
    const double x=p.x+ux*across+uz*depth,z=p.z+uz*across-ux*depth;
    for(const auto &roof:plan.roofs) {
      const auto &ring=roof.polygon;if(ring.size()<3)continue;
      bool in=false;double edge=1e30;
      for(size_t i=0,j=ring.size()-1;i<ring.size();j=i++) {
        auto a=ring[j],b=ring[i];edge=std::min(edge,architecturalSegmentDistance({x,0,z},a,b));
        if((a.z>z)!=(b.z>z) && x<(b.x-a.x)*(z-a.z)/(b.z-a.z)+a.x)in=!in;
      }
      if(!in && edge>.001)continue;
      auto a=ring[0],b=ring[1],c=ring[2];
      const double nx=(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y);
      const double ny=(b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z);
      const double nz=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
      if(std::abs(ny)<1e-9)continue;
      const double roofY=a.y-(nx*(x-a.x)+nz*(z-a.z))/ny;
      if(roofY>0)height=std::min(height,roofY-.25);
    }
  }
  return height>=3.5?height:0.;
}
}
