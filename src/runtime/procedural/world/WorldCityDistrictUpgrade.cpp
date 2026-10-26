#include "WorldCityDistrictUpgrade.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <memory>
#include <limits>

namespace procedural::world::rewrite {
namespace {
using Ring=std::vector<Position>;
bool inside(const Ring &r,Position p) {
  bool yes=false;for(size_t i=0,j=r.size()-1;i<r.size();j=i++) {auto a=r[j],b=r[i];
    if((a.z>p.z)!=(b.z>p.z) && p.x<(b.x-a.x)*(p.z-a.z)/(b.z-a.z)+a.x)yes=!yes;
  }return yes;
}
double boundaryDistance(const Ring &ring,double x,double z) {
  double nearest=std::numeric_limits<double>::max();
  for(size_t i=0;i<ring.size();++i) {
    auto a=ring[i],b=ring[(i+1)%ring.size()];double dx=b.x-a.x,dz=b.z-a.z;
    double den=dx*dx+dz*dz,t=den>1e-12?std::clamp(((x-a.x)*dx+(z-a.z)*dz)/den,0.,1.):0.;
    nearest=std::min(nearest,std::hypot(x-a.x-t*dx,z-a.z-t*dz));
  }
  return nearest;
}
double hillHeight(const CityReliefPlan::LandBlock &block,double x,double z,double original) {
  double t=std::clamp(boundaryDistance(block.boundary,x,z)/block.blendWidth,0.,1.);
  t=t*t*(3-2*t);
  double plane=block.centre.y+block.slopeX*(x-block.centre.x)+block.slopeZ*(z-block.centre.z);
  return original+(plane-original)*t;
}

}
void planCityHillBlocks(const CityCompletionPlan &city,CityReliefPlan &relief,const CityReliefSettings &settings) {
  if(!relief.openGround || !relief.landBlocks.empty())return;
  auto source=relief.openGround;
  for(const auto &region:city.skeleton.regions) {
    if(region.kind!=CityRegionKind::Block || region.boundary.size()<3)continue;
    CityReliefPlan::LandBlock block;block.block=region.id;block.boundary=region.boundary;
    block.minX=block.minZ=1e30;block.maxX=block.maxZ=-1e30;
    for(auto p:block.boundary) {
      block.minX=std::min(block.minX,p.x);block.maxX=std::max(block.maxX,p.x);
      block.minZ=std::min(block.minZ,p.z);block.maxZ=std::max(block.maxZ,p.z);
    }
    for(const auto &lot:city.skeleton.regions)if(lot.kind==CityRegionKind::Lot && lot.parent==region.id)block.lots.push_back(lot.id);
    std::vector<Position> samples=block.boundary;
    double spacing=std::max(2.,std::max(block.maxX-block.minX,block.maxZ-block.minZ)/32.);
    for(double x=block.minX+spacing*.5;x<block.maxX;x+=spacing)
      for(double z=block.minZ+spacing*.5;z<block.maxZ;z+=spacing)
        if(inside(block.boundary,{x,0,z}))samples.push_back({x,0,z});
    bool finite=true;block.minimum=1e30;block.maximum=-1e30;
    for(auto &p:samples) {
      p.y=source(p.x,p.z);finite &= std::isfinite(p.y);
      block.centre.x+=p.x;block.centre.y+=p.y;block.centre.z+=p.z;
      block.minimum=std::min(block.minimum,p.y);block.maximum=std::max(block.maximum,p.y);
    }
    if(!finite)continue;
    block.centre.x/=samples.size();block.centre.y/=samples.size();block.centre.z/=samples.size();
    double xx=0,zz=0,xz=0,xh=0,zh=0;
    for(auto p:samples) {
      double x=p.x-block.centre.x,z=p.z-block.centre.z,h=p.y-block.centre.y;
      xx+=x*x;zz+=z*z;xz+=x*z;xh+=x*h;zh+=z*h;
    }
    double determinant=xx*zz-xz*xz;
    if(determinant>1e-9) {
      block.slopeX=(xh*zz-zh*xz)/determinant;block.slopeZ=(zh*xx-xh*xz)/determinant;
    }
    double residual=0;
    for(auto p:samples)residual=std::max(residual,std::abs(p.y-block.centre.y-block.slopeX*(p.x-block.centre.x)-block.slopeZ*(p.z-block.centre.z)));
    // Enough shoulder width to join the fitted incline without a curb/cliff.
    block.blendWidth=std::max(4.,residual*3/std::max(.01,settings.maximumWalkGrade));
    auto proposed=[&](double x,double z) {
      double h=source(x,z);return inside(block.boundary,{x,0,z})?hillHeight(block,x,z,h):h;
    };
    for(auto p:samples) {
      double gx=(proposed(p.x+.25,p.z)-proposed(p.x-.25,p.z))/.5;
      double gz=(proposed(p.x,p.z+.25)-proposed(p.x,p.z-.25))/.5;
      block.maximumGrade=std::max(block.maximumGrade,std::hypot(gx,gz));
    }
    if(block.maximum-block.minimum>.15 && block.maximumGrade<=settings.maximumWalkGrade && determinant>1e-9)
      block.kind=CityReliefPlan::LandBlock::Kind::Hillside;
    relief.landBlocks.push_back(std::move(block));
  }
  auto hills=std::make_shared<std::vector<CityReliefPlan::LandBlock>>();
  for(const auto &block:relief.landBlocks)if(block.kind==CityReliefPlan::LandBlock::Kind::Hillside)hills->push_back(block);
  if(hills->empty())return;
  relief.openGround=[source,hills](double x,double z) {
    double original=source(x,z);
    for(const auto &block:*hills)if(x>block.minX && x<block.maxX && z>block.minZ && z<block.maxZ && inside(block.boundary,{x,0,z}))
      return hillHeight(block,x,z,original);
    return original;
  };
  // No access structures exist yet in the full-city planning pipeline.
  // Preserve previously planned excavations if this is called on a preview.
  if(!relief.accessGround)relief.accessGround=relief.openGround;
}
void upgradeCityDistricts(CityCompletionPlan &city,CityReliefPlan &relief,const CityReliefSettings &settings,unsigned maximumBlocks,const std::set<uint64_t> *occupiedBlocks) {
  if(!relief.openGround)return;
  planCityHillBlocks(city,relief,settings);
  unsigned accepted=0;
  for(size_t index=0;index<city.skeleton.regions.size() && accepted<maximumBlocks;++index) {
    const auto original=city.skeleton.regions[index];
    if(original.kind!=CityRegionKind::Block || original.boundary.size()<3)continue;
    if(occupiedBlocks && !occupiedBlocks->count(original.id))continue;
    auto land=std::find_if(relief.landBlocks.begin(),relief.landBlocks.end(),[&](const auto &b){return b.block==original.id;});
    if(land!=relief.landBlocks.end() && land->kind==CityReliefPlan::LandBlock::Kind::Hillside)continue;
    Ring ring=original.boundary;double area=0;
    for(size_t i=0;i<ring.size();++i){auto a=ring[i],b=ring[(i+1)%ring.size()];area+=a.x*b.z-b.x*a.z;}
    if(std::abs(area)<700)continue;
    if(area<0)std::reverse(ring.begin(),ring.end());
    bool done=false;
    for(size_t edge=0;edge<ring.size() && !done;++edge) {
      auto a=ring[edge],b=ring[(edge+1)%ring.size()];double length=std::hypot(b.x-a.x,b.z-a.z);
      if(length<16)continue;
      double tx=(b.x-a.x)/length,tz=(b.z-a.z)/length,nx=-tz,nz=tx;
      Position mouth{(a.x+b.x)*.5,0,(a.z+b.z)*.5};
      const double depth=14,half=1.4;
      const auto point=[&](double side,double along){return Position{mouth.x+tx*side+nx*along,0,mouth.z+tz*side+nz*along};};
      bool fits=true;for(double d:{1.,7.,14.,16.})for(double side:{-half,0.,half})fits &= inside(ring,point(side,d));
      if(!fits)continue;
      // Start within the existing public frontage, not across the street.
      auto start=point(0,-.5);double nearRoad=1e30;
      for(const auto &route:city.routes)for(size_t k=1;k<route.path.size();++k) {
        auto p=route.path[k-1],q=route.path[k];double dx=q.x-p.x,dz=q.z-p.z,den=dx*dx+dz*dz;
        if(den<1e-8)continue;double t=std::clamp(((start.x-p.x)*dx+(start.z-p.z)*dz)/den,0.,1.);
        nearRoad=std::min(nearRoad,std::hypot(start.x-p.x-t*dx,start.z-p.z-t*dz)-route.halfWidth);
      }
      if(nearRoad>3)continue;
      CityReliefPlan::RaisedBlock deck;deck.block=original.id;deck.rockCut=true;deck.stairHalfWidth=half;
      for(size_t k=0;k<ring.size();++k){deck.boundary.push_back(ring[k]);if(k==edge) {
        deck.boundary.push_back(point(-half,0));deck.boundary.push_back(point(-half,depth));
        deck.boundary.push_back(point(half,depth));deck.boundary.push_back(point(half,0));
      }}
      start.y=relief.openGround(start.x,start.z);deck.accessFrom=start;
      double high=start.y,low=start.y;
      if(land!=relief.landBlocks.end()){high=std::max(high,land->maximum);low=std::min(low,land->minimum);}
      for(auto p:ring){double h=relief.openGround(p.x,p.z);high=std::max(high,h);low=std::min(low,h);}
      // Preserve gentle hills and their graded streets. A retaining platform
      // is justified only by actual heightmap relief, never by a random lift.
      if(high-low<1.2)continue;
      deck.level=high+.15;
      if(deck.level-start.y<1.2 || deck.level-start.y>4.4)continue;
      deck.accessTo=point(0,depth);deck.accessTo.y=deck.level;
      std::set<uint64_t> remove;unsigned kept=0;
      for(const auto &lot:city.skeleton.regions)if(lot.kind==CityRegionKind::Lot && lot.parent==original.id) {
        // Conservative oriented bounds reserve the whole stair court and its upper exit.
        double s0=1e30,s1=-1e30,d0=1e30,d1=-1e30;
        for(auto p:lot.boundary){double s=(p.x-mouth.x)*tx+(p.z-mouth.z)*tz,d=(p.x-mouth.x)*nx+(p.z-mouth.z)*nz;
          s0=std::min(s0,s);s1=std::max(s1,s);d0=std::min(d0,d);d1=std::max(d1,d);}
        if(s0<half+1 && s1>-half-1 && d0<depth+3 && d1>-.8)remove.insert(lot.id);else ++kept;
      }
      if(kept<2)continue;
      auto skeleton=city.skeleton;
      skeleton.regions.erase(std::remove_if(skeleton.regions.begin(),skeleton.regions.end(),[&](const auto &r){return remove.count(r.id);}),skeleton.regions.end());
      auto candidate=relief;std::string why;
      if(!planRaisedCityBlocks(skeleton,{deck},settings,candidate,&why))continue;
      // Commit the lot reservation and its validated structure together.
      city.skeleton=std::move(skeleton);
      city.lots.erase(std::remove_if(city.lots.begin(),city.lots.end(),[&](const auto &l){return remove.count(l.lot);}),city.lots.end());
      city.skeleton.regionContacts.erase(std::remove_if(city.skeleton.regionContacts.begin(),city.skeleton.regionContacts.end(),[&](const auto &c){return remove.count(c.a)||remove.count(c.b);}),city.skeleton.regionContacts.end());
      for(auto id:remove){candidate.regionLevel.erase(id);candidate.regionPlinth.erase(id);candidate.regionSkirt.erase(id);}
      relief=std::move(candidate);++accepted;done=true;
      // Erasure can move the current block; locate it before continuing iteration.
      for(size_t k=0;k<city.skeleton.regions.size();++k)if(city.skeleton.regions[k].id==original.id){index=k;break;}
    }
  }
}
}
