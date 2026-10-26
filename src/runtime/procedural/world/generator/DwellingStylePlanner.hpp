#pragma once
#include "CellDwellingPlanner.hpp"
#include <algorithm>
#include <cmath>

namespace procedural::world::generator {
inline rewrite::DwellingArchitecture historicalDwellingStyle(uint32_t age,uint64_t identity) {
  // Age is measured from the historical construction record, never the city ID.
  if(age>=180)return identity%3?rewrite::DwellingArchitecture::Roman:rewrite::DwellingArchitecture::HighFantasy;
  if(age>=40 && identity%5==0)return rewrite::DwellingArchitecture::Roman;
  return rewrite::DwellingArchitecture::Vernacular;
}
// Reserve an enclosed rectangular atrium; irregular lots safely fall back.
inline bool carveRomanCourt(const CellGrid &grid,const Board &area,double share,Board &built,Board &court) {
  const auto box=geom2::bounds(area);
  const int width=box.i1-box.i0+1,depth=box.j1-box.j0+1;
  for(int side=int(std::sqrt(area.count()*share));side>=3;--side){
    if(side>width-4 || side>depth-4)continue;
    Board candidate;const int x=box.i0+(width-side)/2,z=box.j0+(depth-side)/2;
    bool fits=true;
    for(int i=x-2;i<x+side+2;++i)for(int j=z-2;j<z+side+2;++j){
      auto c=grid.cell(i,j);if(c==geom2::kNoCell || !grid.contains(area,c))fits=false;
      if(i>=x&&i<x+side&&j>=z&&j<z+side&&c!=geom2::kNoCell)grid.add(candidate,c);
    }
    if(!fits)continue;auto rest=area & ~candidate;
    if(!grid.connected(rest) || geom2::narrow(grid,rest).count()>geom2::narrow(grid,area).count())continue;
    court=candidate;built=rest;return true;
  }
  return false;
}
inline void tuneDwellingStyle(const CellDwellingBrief &brief,CellDwellingSettings &s) {
  if(brief.architecture==rewrite::DwellingArchitecture::HighFantasy){
    s.grid.storeyMetres=std::max(s.grid.storeyMetres,3.6);return;
  }
  if(brief.architecture!=rewrite::DwellingArchitecture::Roman)return;
  // Reserve the court before partitioning rooms and solving doors/stairs.
  // Small plots retain their whole usable programme instead of a tiny well.
  s.courts=s.grid.maximumCells*s.grid.cellMetres*s.grid.cellMetres>=170?1:0;
  const bool temple=std::find(brief.tags.begin(),brief.tags.end(),"roman-temple")!=brief.tags.end();
  if(temple){s.courts=0;s.averageRoomCells=4096;s.coarseRoomCells=4096;s.formalFront=true;s.grid.storeyMetres=4.8;}
  s.courtShare=.25;s.yards=s.pens=s.sheds=0;
  s.averageRoomCells=std::max(4.,s.averageRoomCells*.78);
  s.coarseRoomCells=std::max(s.averageRoomCells,s.coarseRoomCells*.8);
  s.cornerTowers=s.stairTowerAbove=0;
}
inline void applyDwellingStyleFacade(rewrite::DwellingPlan &p) {
  using namespace rewrite;
  if(p.architecture==DwellingArchitecture::Vernacular)return;
  p.facade.timberFacade=p.facade.boardFacade=p.facade.timberFrameMembers=false;
  p.facade.masonryVariation=true;p.facade.variationSeed=p.seed+uint64_t(p.weathering*1000);
  p.facade.circularWindows=false;p.facade.shutters=false;
  p.facade.style=p.architecture==DwellingArchitecture::Roman?FacadeStyle::Romanesque:FacadeStyle::Gothic;
  p.facade.roofCrests=p.architecture==DwellingArchitecture::HighFantasy && p.weathering<.7;
  p.facade.windowWidth=1.0;p.facade.windowHeight=p.architecture==DwellingArchitecture::Roman?1.25:1.65;
}
inline void romanColumn(rewrite::DwellingPlan &p,rewrite::Position c,double base,double top,uint32_t age) {
  using namespace rewrite;
  ++p.colonnadeColumns;
  const auto course=[&](double low,double high,double radius,unsigned sides,bool fluted) {
    DwellingPillar post;post.base=low;post.top=high;post.masonry=true;post.capped=true;
    for(unsigned i=0;i<sides;++i){const double a=-6.283185307179586*i/sides+(sides==4?.78539816339:0.);
      const double flute=fluted&&i%2?.88:1.;
      const double wear=fluted?1.:1.-p.weathering*.09*((i*7+age)%5)/4.;
      const double r=radius*flute*wear;post.footprint.push_back({c.x+std::cos(a)*r,0,c.z+std::sin(a)*r});}
    p.pillars.push_back(std::move(post));
  };
  course(base,base+.14,.43,4,false);          // square plinth
  course(base+.14,base+.25,.32,16,false);    // lower torus
  course(base+.25,base+.32,.27,16,false);    // collar
  course(base+.32,top-.36,.245,24,true);     // fluted shaft
  course(top-.36,top-.25,.27,16,false);     // neck
  course(top-.25,top-.12,.34,16,false);     // echinus
  course(top-.12,top,.46,4,false);           // abacus
}
inline void romanPortico(const CellHouse &house,rewrite::DwellingPlan &p) {
  using namespace rewrite;
  if(p.storeys.empty()||house.architecturalLot.outer.empty())return;
  const bool temple=std::find(house.tags.begin(),house.tags.end(),"roman-temple")!=house.tags.end();
  const auto inside=[&](Position q){bool in=false;const auto &r=house.architecturalLot.outer;
    for(size_t i=0,j=r.size()-1;i<r.size();j=i++){const double ax=geom2::toMetres(r[i].x),az=geom2::toMetres(r[i].y),bx=geom2::toMetres(r[j].x),bz=geom2::toMetres(r[j].y);
      if((az>q.z)!=(bz>q.z)&&q.x<(bx-ax)*(q.z-az)/(bz-az)+ax)in=!in;}return in;};
  for(auto &door:p.openings)if(door.exterior&&door.kind==OpeningKind::Door&&std::abs(door.a.y)<.01){
    Position centre{(door.a.x+door.b.x)*.5,0,(door.a.z+door.b.z)*.5};
    const double len=std::hypot(door.b.x-door.a.x,door.b.z-door.a.z);if(len<.01)continue;
    const double ux=(door.b.x-door.a.x)/len,uz=(door.b.z-door.a.z)/len;
    double nx=-uz,nz=ux;Position mass{};for(auto q:p.storeys.front().outer){mass.x+=q.x/p.storeys.front().outer.size();mass.z+=q.z/p.storeys.front().outer.size();}
    if((mass.x-centre.x)*nx+(mass.z-centre.z)*nz>0){nx=-nx;nz=-nz;}
    const double half=temple?4.5:2.5,depth=2.2,top=std::max(p.storeys.front().top-.3,door.top+.65);
    if(top<2.3)continue;
    const auto at=[&](double x,double z,double y){return Position{centre.x+ux*x+nx*z,y,centre.z+uz*x+nz*z};};
    bool fits=true;for(double x:{-half-.5,half+.5})for(double z:{.1,depth+2.65})fits&=inside(at(x,z,0));
    for(int i=0;i<=8;++i)for(int j=0;j<=8;++j)
      fits &= inside(at(-half-.5+(2*half+1)*i/8.,.02+(depth+2.63)*j/8.,0));
    if(!fits)continue; // never project a portico into a street or neighbouring lot
    // Reserve the complete entrance before emitting any decoration. The broad
    // landing carries every plinth; a separate full-width flight clears the columns.
    p.entranceLanding={at(-half-.5,.02,0),at(half+.5,.02,0),at(half+.5,depth+.5,0),at(-half-.5,depth+.5,0)};
    p.entranceFlight={at(-half-.5,depth+.5,0),at(half+.5,depth+.5,0),at(half+.5,depth+2.6,0),at(-half-.5,depth+2.6,0)};
    const double rise=std::max(.18,p.siteAccess.rise);
    p.siteAccess.slabs.erase(std::remove_if(p.siteAccess.slabs.begin(),p.siteAccess.slabs.end(),[](const auto &s){return s.landing||s.tread;}),p.siteAccess.slabs.end());
    p.siteAccess.slabs.push_back({p.entranceLanding,-rise-.2,0,true,false});
    const int steps=std::max(1,int(std::ceil(rise/.18)));
    for(int i=0;i<steps;++i){const double back=depth+.5+2.1*i/steps,front=depth+.5+2.1*(i+1)/steps;
      p.siteAccess.slabs.push_back({{at(-half-.5,back,0),at(half+.5,back,0),at(half+.5,front,0),at(-half-.5,front,0)},-rise-.2,-rise*double(i+1)/steps,true,true});}
    if(temple){
      door.head=door.top;
      for(auto &leaf:p.doors)if(leaf.wall==door.wall){leaf.privateAccess=true;leaf.leaves=2;}
      // A temple's paired leaves belong to a real wide opening in the blueprint.
      const double width=std::max(len,2.2);
      auto wall=std::find_if(p.walls.begin(),p.walls.end(),[&](const auto &w){return w.id==door.wall;});
      if(wall!=p.walls.end() && std::hypot(wall->b.x-wall->a.x,wall->b.z-wall->a.z)>width+.4){
        door.a.x=centre.x-ux*width*.5;door.a.z=centre.z-uz*width*.5;
        door.b.x=centre.x+ux*width*.5;door.b.z=centre.z+uz*width*.5;
        door.head=door.top;
        for(auto &leaf:p.doors)if(leaf.wall==door.wall){leaf.privateAccess=true;leaf.width=width;}
      }
    }
    for(double x:{-half,half})romanColumn(p,at(x,depth,0),0,top,house.architecturalAgeYears);
    if(temple)for(double x:{-half/3,half/3})romanColumn(p,at(x,depth,0),0,top,house.architecturalAgeYears);
    DwellingPillar lintel;lintel.base=top;lintel.top=top+.22;lintel.masonry=true;lintel.capped=true;
    lintel.footprint={at(-half-.45,depth-.34,0),at(-half-.45,depth+.34,0),at(half+.45,depth+.34,0),at(half+.45,depth-.34,0)};
    double winding=0;for(size_t i=0;i<4;++i){auto a=lintel.footprint[i],b=lintel.footprint[(i+1)%4];winding+=a.x*b.z-b.x*a.z;}
    if(winding>0)std::reverse(lintel.footprint.begin(),lintel.footprint.end());p.pillars.push_back(std::move(lintel));
    const auto a=at(-half-.45,-.12,top+.22),b=at(half+.45,-.12,top+.22),c=at(half+.45,depth+.45,top+.22),d=at(-half-.45,depth+.45,top+.22);
    auto front=at(0,depth+.45,top+.22+(temple?1.7:.8)),back=at(0,-.12,front.y);
    p.roofs.push_back({{a,d,front,back},0,false,false});p.roofs.push_back({{back,front,c,b},0,false,false});
    p.roofs.push_back({{d,c,front},0,true,false});p.roofs.push_back({{b,a,back},0,true,false});p.roofs.push_back({{a,b,c,d},0,false,true});
    return;
  }
}
inline void finishDwellingStyle(const CellHouse &house,rewrite::DwellingPlan &p) {
  using namespace rewrite;
  p.architecture=house.architecture;p.architecturalAgeYears=house.architecturalAgeYears;
  p.weathering=std::clamp((double(house.architecturalAgeYears)-60.)/500.,0.,.85);
  applyDwellingStyleFacade(p);
  if(p.architecture==DwellingArchitecture::HighFantasy && !p.storeys.empty())
    for(auto &door:p.openings)if(door.exterior && door.kind==OpeningKind::Door && door.floor==0) {
      const double top=std::min(3.4,p.storeys.front().top-.35);
      if(top>=2.7){door.top=top;door.head=top;for(auto &leaf:p.doors)if(leaf.wall==door.wall){leaf.privateAccess=true;if(leaf.width>1.15)leaf.leaves=2;}}
    }
  if(p.architecture!=DwellingArchitecture::Roman)return;
  // A peristyle is placed entirely inside a generous court. Its outer wall
  // carries one roof edge; the column line carries the other.
  for(const auto &storey:p.storeys)if(storey.base>=-.001) {
    for(auto ring:storey.holes) {
      bool changed=true;while(changed&&ring.size()>4){changed=false;for(size_t i=0;i<ring.size();++i){auto a=ring[(i+ring.size()-1)%ring.size()],b=ring[i],c=ring[(i+1)%ring.size()];if(std::abs((b.x-a.x)*(c.z-b.z)-(b.z-a.z)*(c.x-b.x))<1e-6){ring.erase(ring.begin()+i);changed=true;break;}}}
      if(ring.size()!=4)continue;
      double span=1e30;Position centre{};
      for(size_t i=0;i<4;++i){centre.x+=ring[i].x/4;centre.z+=ring[i].z/4;span=std::min(span,std::hypot(ring[i].x-ring[(i+1)%4].x,ring[i].z-ring[(i+1)%4].z));}
      if(span<5.6-1e-6)continue;
      const double scale=1-3.2/span,base=storey.base,top=storey.top-.35;
      if(top-base<2.3)continue;
      std::vector<Position> inner;
      for(auto q:ring)inner.push_back({centre.x+(q.x-centre.x)*scale,top,centre.z+(q.z-centre.z)*scale});
      const auto column=[&](Position c){romanColumn(p,c,base,top,house.architecturalAgeYears);};
      for(size_t i=0;i<4;++i){const size_t j=(i+1)%4;const auto a=inner[i],b=inner[j];const int bays=std::max(1,int(std::ceil(std::hypot(b.x-a.x,b.z-a.z)/2.7)));
        for(int k=0;k<bays;++k)column({a.x+(b.x-a.x)*k/bays,base,a.z+(b.z-a.z)*k/bays});
        auto wa=ring[i],wb=ring[j];wa.y=wb.y=storey.top-.12;
        p.roofs.push_back({{wa,wb,b,a},0,false,false});
        // Closed soffit and fascia, not a floating one-sided roof plane.
        auto ua=wa,ub=wb,ia=a,ib=b;ua.y-=.12;ub.y-=.12;ia.y-=.12;ib.y-=.12;
        p.roofs.push_back({{ia,ib,ub,ua},0,false,true});
        p.roofs.push_back({{a,b,ib,ia},0,false,true});
      }
    }
    break;
  }
  romanPortico(house,p);
  // Coffered ceilings use the existing solid timber path and keep clear of
  // stair rooms and open courts. Short beams are checked against the room.
  for(const auto &room:p.rooms){
    if(room.openToRoof || room.kind==RoomKind::Courtyard || room.kind==RoomKind::Yard || room.kind==RoomKind::Staircase || room.boundary.size()<4 || room.usableArea<12)continue;
    const auto inside=[&](Position q){bool in=false;for(size_t i=0,j=room.boundary.size()-1;i<room.boundary.size();j=i++) {auto a=room.boundary[i],b=room.boundary[j];if((a.z>q.z)!=(b.z>q.z) && q.x<(b.x-a.x)*(q.z-a.z)/(b.z-a.z)+a.x)in=!in;}return in;};
    double x0=1e30,z0=1e30,x1=-1e30,z1=-1e30;
    for(auto q:room.boundary){x0=std::min(x0,q.x);x1=std::max(x1,q.x);z0=std::min(z0,q.z);z1=std::max(z1,q.z);}
    const double y=room.ceilingHeight>0?room.ceilingHeight-.18:room.boundary.front().y+house.gridClass.storeyMetres-.18;
    for(double x=x0+1.;x<x1-.5;x+=1.8)for(double z=z0+1.;z<z1-.5;z+=1.8)for(int axis=0;axis<2;++axis){Position a{x,y,z},b=a;if(axis)b.z+=1.8;else b.x+=1.8;if(!inside(a)||!inside(b))continue;
      bool clear=true;for(int n=0;n<=8;++n){Position q{a.x+(b.x-a.x)*n/8.,y,a.z+(b.z-a.z)*n/8.};if(!inside(q))clear=false;for(const auto &stair:p.stairs)if(!stair.opening.empty()) {
        double sx0=1e30,sz0=1e30,sx1=-1e30,sz1=-1e30;
        for(auto v:stair.opening){sx0=std::min(sx0,v.x);sx1=std::max(sx1,v.x);sz0=std::min(sz0,v.z);sz1=std::max(sz1,v.z);}
        if(q.x>=sx0-.25&&q.x<=sx1+.25&&q.z>=sz0-.25&&q.z<=sz1+.25)clear=false;
      }}
      if(clear){p.timbers.push_back({room.floor,TimberKind::Beam,a,b,.09,.14});++p.cofferBeams;}
    }
  }
}
} // namespace procedural::world::generator
