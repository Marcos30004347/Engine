#include "CellDwellingSite.hpp"
#include "procedural/geom2/Boolean.hpp"
#include <algorithm>
#include <cmath>
#include <queue>

namespace procedural::world::generator {
void planCellDwellingSite(const CellDwellingBrief &brief,const CellDwellingSettings &s,CellHouse &house)
{
  house.siteAccess={};
  if(house.floors.empty() || house.entrance==geom2::kNoEdge) return;
  const auto &grid=house.grid;
  const auto cellPoly=[&](uint32_t c){Board b;b.set(grid.cellRow(c),grid.cellColumn(c));geom2::Polygon p;geom2::polygonOf(grid,house.frame,b,p);return p;};
  const auto ring=[](const geom2::Polygon &p){std::vector<rewrite::Position> out;for(auto v:p.outer)out.push_back({geom2::toMetres(v.x),0,geom2::toMetres(v.y)});return out;};
  geom2::Polygon footprint;
  if(!geom2::polygonOf(grid,house.frame,house.floors.front().area,footprint) || !footprint.holes.empty()) return;
  // Even ground-level houses carry an explicit support pad polygon.
  house.siteAccess.slabs.push_back({ring(footprint),-.05,0,false,false});
  if(!s.entranceFlightCells || !std::isfinite(s.entrancePlatformRise) || s.entrancePlatformRise<=0) return;
  const auto inside=grid.edgeCell(house.entrance);
  const auto outward=geom2::ccw(grid.decode(house.entrance).dir);
  const auto landing=grid.neighbour(inside,outward);
  std::vector<int8_t> usable(grid.width()*grid.height(),-1);
  const auto free=[&](uint32_t c){
    if(c==geom2::kNoCell || grid.contains(house.floors.front().area,c) || grid.contains(house.site.obstacle,c)) return false;
    if(usable[c]>=0) return usable[c]!=0;
    auto p=cellPoly(c);std::vector<geom2::Polygon> outside;
    if(!geom2::differenceOf(geom2::toOperand(p),{geom2::toOperand(brief.lot)},outside)) return false;
    double area=0;for(const auto &q:outside)area+=std::abs(geom2::areaMetres(q));
    usable[c]=area<1e-7;return usable[c]!=0;
  };
  if(!free(landing)) return;
  const auto frontage=[&](uint32_t c){
    double x,z;geom2::cellPoint(grid,house.frame,c,x,z);
    for(auto edge:brief.frontage) {
      const auto a=brief.lot.outer[edge%brief.lot.outer.size()],b=brief.lot.outer[(edge+1)%brief.lot.outer.size()];
      const double ax=geom2::toMetres(a.x),az=geom2::toMetres(a.y),dx=geom2::toMetres(b.x-a.x),dz=geom2::toMetres(b.y-a.y);
      const double t=std::clamp(((x-ax)*dx+(z-az)*dz)/std::max(1e-9,dx*dx+dz*dz),0.,1.);
      if(std::hypot(x-ax-t*dx,z-az-t*dz)<=house.frame.cellMetres*1.5) return true;
    }return false;
  };
  std::vector<geom2::Dir> directions=s.sidewaysEntrance ? std::vector<geom2::Dir>{geom2::cw(outward),geom2::ccw(outward),outward} : std::vector<geom2::Dir>{outward,geom2::cw(outward),geom2::ccw(outward)};
  for(auto direction:directions) for(uint32_t length=std::min(3u,s.entranceFlightCells);length>0;--length)
  {
    std::vector<uint32_t> flight;uint32_t c=landing;bool valid=true;
    for(uint32_t n=0;n<length;++n){c=grid.neighbour(c,direction);if(!free(c)){valid=false;break;}flight.push_back(c);}
    if(!valid) continue;
    // Ground approach is also on the lot grid. It may bend, but cannot pass
    // through the house, the raised landing, or another tread.
    std::vector<uint32_t> parent(usable.size(),geom2::kNoCell);std::queue<uint32_t> queue;
    const auto foot=grid.neighbour(flight.back(),direction);
    if(!free(foot)) continue;
    parent[foot]=foot;queue.push(foot);uint32_t end=geom2::kNoCell;
    while(!queue.empty()) {
      c=queue.front();queue.pop();if(frontage(c)){end=c;break;}
      for(auto d:geom2::kCardinal) {auto next=grid.neighbour(c,d);
        if(next==geom2::kNoCell || parent[next]!=geom2::kNoCell || next==landing || std::find(flight.begin(),flight.end(),next)!=flight.end() || !free(next))continue;
        parent[next]=c;queue.push(next);
      }
    }
    if(end==geom2::kNoCell) continue;
    auto &access=house.siteAccess;
    const uint32_t steps=std::max(1u,uint32_t(std::floor(length*house.frame.cellMetres/.28)));
    // Limit height by the available tread run, not an arbitrary low plinth cap.
    access.rise=std::min(s.entrancePlatformRise,steps*.18);
    access.layout=direction==outward ? rewrite::EntranceLayout::Inset : rewrite::EntranceLayout::Sideways;
    access.slabs.front().bottom=-access.rise;
    access.reservedCells=flight;access.reservedCells.push_back(landing);
    access.slabs.push_back({ring(cellPoly(landing)),-access.rise,0,true,false});
    while(true){access.reservedCells.push_back(end);access.slabs.push_back({ring(cellPoly(end)),-access.rise-.03,-access.rise,false,false});if(end==foot)break;end=parent[end];}
    // Widen the level arrival pad without taking space from the flight,
    // ground approach, neighboring buildings, or the lot boundary.
    for(auto side:{geom2::cw(direction),geom2::ccw(direction)}) {
      const auto extra=grid.neighbour(landing,side);
      if(free(extra) && std::find(access.reservedCells.begin(),access.reservedCells.end(),extra)==access.reservedCells.end()) {
        access.reservedCells.push_back(extra);
        access.slabs.push_back({ring(cellPoly(extra)),-access.rise,0,true,false});
        break;
      }
    }
    // Uniform treads follow the selected grid strip from the ground foot to
    // the landing. No sloped joiner or mesh-time stair placement.
    double lx,lz,fx,fz;geom2::cellPoint(grid,house.frame,landing,lx,lz);geom2::cellPoint(grid,house.frame,flight.back(),fx,fz);
    const double distance=std::hypot(fx-lx,fz-lz),ux=(fx-lx)/distance,uz=(fz-lz)/distance,w=house.frame.cellMetres*.5;
    const double run=length*house.frame.cellMetres;
    for(uint32_t k=0;k<steps;++k){const double near=w+run*k/steps,far=w+run*(k+1)/steps;
      std::vector<rewrite::Position> p={{lx+ux*near-uz*w,0,lz+uz*near+ux*w},{lx+ux*far-uz*w,0,lz+uz*far+ux*w},{lx+ux*far+uz*w,0,lz+uz*far-ux*w},{lx+ux*near+uz*w,0,lz+uz*near-ux*w}};
      access.slabs.push_back({p,-access.rise,-access.rise*double(k)/steps,false,true});
    }
    return;
  }
}
}
