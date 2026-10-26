#pragma once
#include "WorldGenerationContracts.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace procedural::world::rewrite {
inline bool cityYardNeedsBoundary(double lotArea, double houseArea)
{
  const double yard=lotArea-houseArea;
  return std::isfinite(yard) && yard>=24. && yard>=lotArea*.2;
}

// Preserve every blocked interval: even a narrow gap may be a reserved stair
// or doorway. Sampling never reconnects a fence across protected circulation.
inline std::vector<std::pair<double,double>> cityYardBoundaryRuns(
    double length,const std::vector<char> &free,double step,double gate=-1.,double gateWidth=1.8)
{
  std::vector<std::pair<double,double>> result;
  if(length<=0 || step<=0)return result;
  const auto add=[&](double a,double b){if(b-a>=.6)result.emplace_back(a,b);};
  const auto flush=[&](double a,double b){
    if(a<0)return;
    if(gate>=0 && gate<=length) {
      add(a,std::min(b,gate-gateWidth*.5));
      add(std::max(a,gate+gateWidth*.5),b);
    } else add(a,b);
  };
  double start=-1;
  for(size_t i=0;i<free.size();++i){
    const double t=std::min(length,i*step);
    if(free[i]){if(start<0)start=std::max(0.,t-step*.5);}
    else if(start>=0){flush(start,std::max(start,t-step*.5));start=-1;}
  }
  if(start>=0)flush(start,length);
  return result;
}

// An acute plot corner need not become a fence corner. If the house and its
// approach fit in an inset, house-aligned rectangle, that rectangle is a more
// useful enclosure than the legal lot's spike. The legal parcel is unchanged.
inline std::optional<std::vector<Position>> cityYardInsetFence(
    const std::vector<Position> &lot,const std::vector<Position> &footprint,
    const std::vector<Position> &access={})
{
  if(lot.size()<3 || footprint.size()<3)return std::nullopt;
  const auto distance=[](Position p,Position a,Position b){
    const double dx=b.x-a.x,dz=b.z-a.z,l2=dx*dx+dz*dz;
    const double t=l2>1e-12?std::clamp(((p.x-a.x)*dx+(p.z-a.z)*dz)/l2,0.,1.):0.;
    return std::hypot(p.x-a.x-t*dx,p.z-a.z-t*dz);
  };
  const auto inside=[&](Position p){
    bool yes=false;
    for(size_t i=0,j=lot.size()-1;i<lot.size();j=i++)
      if((lot[i].z>p.z)!=(lot[j].z>p.z) &&
         p.x<(lot[j].x-lot[i].x)*(p.z-lot[i].z)/(lot[j].z-lot[i].z)+lot[i].x)yes=!yes;
    return yes;
  };
  double signedArea=0;for(size_t i=0;i<lot.size();++i){auto a=lot[i],b=lot[(i+1)%lot.size()];signedArea+=a.x*b.z-b.x*a.z;}
  bool acute=false;
  for(size_t i=0;i<lot.size();++i){
    auto a=lot[(i+lot.size()-1)%lot.size()],b=lot[i],c=lot[(i+1)%lot.size()];
    const double ux=a.x-b.x,uz=a.z-b.z,vx=c.x-b.x,vz=c.z-b.z;
    const double den=std::hypot(ux,uz)*std::hypot(vx,vz);
    const double turn=(b.x-a.x)*(c.z-b.z)-(b.z-a.z)*(c.x-b.x);
    if(den>1e-8 && turn*signedArea>0 && (ux*vx+uz*vz)/den>.42){acute=true;break;}
  }
  if(!acute)return std::nullopt;
  size_t longest=0;double run=0;
  for(size_t i=0;i<footprint.size();++i){const auto a=footprint[i],b=footprint[(i+1)%footprint.size()];
    const double d=std::hypot(b.x-a.x,b.z-a.z);if(d>run){run=d;longest=i;}}
  if(run<2.)return std::nullopt;
  const auto a=footprint[longest],b=footprint[(longest+1)%footprint.size()];
  const double ux=(b.x-a.x)/run,uz=(b.z-a.z)/run,vx=-uz,vz=ux;
  Position origin{};for(const auto &p:footprint){origin.x+=p.x/footprint.size();origin.z+=p.z/footprint.size();}
  double x0=1e100,x1=-1e100,z0=1e100,z1=-1e100;
  for(const auto &p:footprint){const double x=(p.x-origin.x)*ux+(p.z-origin.z)*uz,z=(p.x-origin.x)*vx+(p.z-origin.z)*vz;
    x0=std::min(x0,x);x1=std::max(x1,x);z0=std::min(z0,z);z1=std::max(z1,z);}
  // Include every stair and landing so the new fence cannot cut the approach.
  for(const auto &p:access){const double x=(p.x-origin.x)*ux+(p.z-origin.z)*uz,z=(p.x-origin.x)*vx+(p.z-origin.z)*vz;
    x0=std::min(x0,x);x1=std::max(x1,x);z0=std::min(z0,z);z1=std::max(z1,z);}
  const auto point=[&](double x,double z){return Position{origin.x+ux*x+vx*z,0,origin.z+uz*x+vz*z};};
  for(double margin:{2.4,1.8,1.2,.8}){
    if((x1-x0+2*margin)*(z1-z0+2*margin)>std::abs(signedArea)*.4)continue;
    std::vector<Position> ring={point(x0-margin,z0-margin),point(x1+margin,z0-margin),
                                point(x1+margin,z1+margin),point(x0-margin,z1+margin)};
    bool safe=true;
    for(size_t i=0;i<4 && safe;++i){const auto p=ring[i],q=ring[(i+1)%4];
      const int samples=std::max(1,int(std::ceil(std::hypot(q.x-p.x,q.z-p.z)/.5)));
      for(int k=0;k<=samples && safe;++k){Position t{p.x+(q.x-p.x)*k/samples,0,p.z+(q.z-p.z)*k/samples};
        if(!inside(t)){safe=false;break;}
        for(size_t j=0;j<lot.size();++j)if(distance(t,lot[j],lot[(j+1)%lot.size()])<.2){safe=false;break;}
      }
    }
    if(safe)return ring;
  }
  // Where the house sits too close to a legal side for a rectangular yard,
  // bevel only the pointed corners. Keep each cut wholly inside the parcel
  // and refuse it if it would squeeze the house or its entrance steps.
  std::vector<Position> softened;
  std::vector<std::pair<Position,Position>> cuts;
  for(size_t i=0;i<lot.size();++i){
    const auto prev=lot[(i+lot.size()-1)%lot.size()],p=lot[i],next=lot[(i+1)%lot.size()];
    const double ax=prev.x-p.x,az=prev.z-p.z,bx=next.x-p.x,bz=next.z-p.z;
    const double la=std::hypot(ax,az),lb=std::hypot(bx,bz);
    const double turn=(p.x-prev.x)*(next.z-p.z)-(p.z-prev.z)*(next.x-p.x);
    const bool pointed=la>2. && lb>2. && turn*signedArea>0 &&
        (ax*bx+az*bz)/(la*lb)>.42;
    if(!pointed){softened.push_back(p);continue;}
    // A shallow trim leaves the same needle-shaped silhouette. Cut well back
    // along both sides; the containment/clearance checks below reject a cut
    // whenever the building or its approach needs that corner.
    const double cut=std::min({6.,la*.65,lb*.65});
    const Position start{p.x+ax*cut/la,0,p.z+az*cut/la};
    const Position end{p.x+bx*cut/lb,0,p.z+bz*cut/lb};
    softened.push_back(start);softened.push_back(end);
    cuts.emplace_back(start,end);
  }
  if(softened.size()==lot.size())return std::nullopt;
  const auto within=[&](Position p){
    bool yes=false;
    for(size_t i=0,j=softened.size()-1;i<softened.size();j=i++)
      if((softened[i].z>p.z)!=(softened[j].z>p.z) &&
         p.x<(softened[j].x-softened[i].x)*(p.z-softened[i].z)/
             (softened[j].z-softened[i].z)+softened[i].x)yes=!yes;
    if(!yes){
      double edgeDistance=1e100;
      for(size_t i=0;i<softened.size();++i)
        edgeDistance=std::min(edgeDistance,distance(p,softened[i],softened[(i+1)%softened.size()]));
      if(edgeDistance>.001)return false;
    }
    // Original lot sides are allowed to be close to a stair: their panels
    // are clipped by the existing clearance sampler. Only a new diagonal
    // must stand clear of the approach before we accept this bevel.
    for(const auto &[a,b]:cuts)if(distance(p,a,b)<.55)return false;
    return true;
  };
  for(const auto &p:footprint)if(!within(p))return std::nullopt;
  for(const auto &p:access)if(inside(p) && !within(p))return std::nullopt;
  for(const auto &p:access)for(const auto &[a,b]:cuts)
    if(distance(p,a,b)<.55)return std::nullopt;
  for(size_t i=0;i<softened.size();++i){
    const auto p=softened[i],q=softened[(i+1)%softened.size()];
    const int samples=std::max(1,int(std::ceil(std::hypot(q.x-p.x,q.z-p.z)/.5)));
    for(int k=1;k<samples;++k){
      Position t{p.x+(q.x-p.x)*k/samples,0,p.z+(q.z-p.z)*k/samples};
      if(inside(t))continue;
      double edgeDistance=1e100;
      for(size_t j=0;j<lot.size();++j)
        edgeDistance=std::min(edgeDistance,distance(t,lot[j],lot[(j+1)%lot.size()]));
      if(edgeDistance>.001)return std::nullopt;
    }
  }
  return softened;
}
}
