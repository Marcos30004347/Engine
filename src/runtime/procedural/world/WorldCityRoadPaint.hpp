#pragma once
#include "WorldCitySurfaces.hpp"
#include "procedural/geom2/Offset.hpp"
#include <map>

namespace procedural::world::rewrite {
// Close narrow cosmetic gaps between street, alley and frontage paint. Existing
// road cover is preserved; only newly filled gaps are clipped against lots.
inline bool mergeCityRoadPaintChunk(CitySurfaceStructure &surface,
    const std::vector<geom2::Polygon> &protectedLots,double maximumGap=1.5,std::string *error=nullptr) {
  std::map<uint32_t,std::vector<geom2::Operand>> channels;
  std::vector<geom2::Operand> all;
  std::vector<CitySurfaceStructure::Influence> background;
  for(const auto &p:surface.influences) {
    if(p.road || p.frontage || p.paving) {
      auto op=geom2::toOperand(p.polygon);channels[p.channel].push_back(op);all.push_back(std::move(op));
    } else background.push_back(p);
  }
  if(all.empty())return true;
  std::vector<geom2::Polygon> original,expanded,closed;
  if(!geom2::unionOf(all,original,error))return false;
  geom2::OffsetOptions options;options.join=geom2::JoinStyle::Round;options.arcToleranceMetres=.12;
  if(maximumGap>0) {
    if(!geom2::offsetPolygons(original,maximumGap*.5,options,expanded,error) ||
       !geom2::offsetPolygons(expanded,-maximumGap*.5,options,closed,error))return false;
    std::vector<geom2::Operand> exclusions=all;
    for(const auto &lot:protectedLots)exclusions.push_back(geom2::toOperand(lot));
    for(const auto &p:closed) {
      std::vector<geom2::Polygon> additions;
      if(!geom2::differenceOf(geom2::toOperand(p),exclusions,additions,error))return false;
      for(const auto &q:additions)channels[2].push_back(geom2::toOperand(q));
    }
  }
  // Circulation is painted last: later grass/yard paint must not re-open a
  // seam through a merged street. Stone wins only where materials overlap.
  for(const auto &[channel,ops]:channels) {
    std::vector<geom2::Polygon> merged;
    if(!geom2::unionOf(ops,merged,error))return false;
    for(auto &p:merged)background.push_back({std::move(p),channel,1,.5,true});
  }
  surface.influences=std::move(background);return true;
}
// Bounded, overlapping sections prevent a city-wide offset from multiplying
// every nearby contour intersection in one arrangement. Trim only after closing.
inline bool mergeCityRoadPaint(CitySurfaceStructure &surface,
    const std::vector<geom2::Polygon> &protectedLots,double maximumGap=1.5,std::string *error=nullptr,bool forceSections=false) {
  size_t vertices=0;for(const auto &p:surface.influences)if(p.road || p.frontage || p.paving)vertices+=p.polygon.outer.size();
  if(!forceSections && vertices<4000)return mergeCityRoadPaintChunk(surface,protectedLots,maximumGap,error);
  std::vector<CitySurfaceStructure::Influence> result,roads;
  double x0=INFINITY,z0=INFINITY,x1=-INFINITY,z1=-INFINITY;
  for(const auto &p:surface.influences) {
    if(!(p.road || p.frontage || p.paving)){result.push_back(p);continue;}
    roads.push_back(p);const auto b=geom2::bounds(p.polygon);
    x0=std::min(x0,geom2::toMetres(b.minX));x1=std::max(x1,geom2::toMetres(b.maxX));
    z0=std::min(z0,geom2::toMetres(b.minY));z1=std::max(z1,geom2::toMetres(b.maxY));
  }
  const auto rectangle=[](double x,double z,double w,double h){geom2::Polygon p;
    p.outer={{geom2::toUnit(x),geom2::toUnit(z)},{geom2::toUnit(x+w),geom2::toUnit(z)},
             {geom2::toUnit(x+w),geom2::toUnit(z+h)},{geom2::toUnit(x),geom2::toUnit(z+h)}};return p;};
  constexpr double size=64.;const double halo=std::max(2.,maximumGap*2);
  for(double x=std::floor(x0/size)*size;x<x1;x+=size)for(double z=std::floor(z0/size)*size;z<z1;z+=size) {
    const auto core=rectangle(x,z,size,size),pad=rectangle(x-halo,z-halo,size+2*halo,size+2*halo);
    const auto box=geom2::bounds(pad);CitySurfaceStructure local;
    for(const auto &road:roads) {
      if(!box.overlaps(geom2::bounds(road.polygon)))continue;
      std::vector<geom2::Polygon> clipped;
      if(!geom2::intersectionOf({geom2::toOperand(road.polygon),geom2::toOperand(pad)},clipped,error))return false;
      for(auto &p:clipped){auto q=road;q.polygon=std::move(p);local.influences.push_back(std::move(q));}
    }
    if(local.influences.empty())continue;
    std::vector<geom2::Polygon> reserved;
    for(const auto &p:protectedLots)if(box.overlaps(geom2::bounds(p)))reserved.push_back(p);
    if(!mergeCityRoadPaintChunk(local,reserved,maximumGap,error))return false;
    for(const auto &paint:local.influences) {
      std::vector<geom2::Polygon> clipped;
      if(!geom2::intersectionOf({geom2::toOperand(paint.polygon),geom2::toOperand(core)},clipped,error))return false;
      for(auto &p:clipped){auto q=paint;q.polygon=std::move(p);result.push_back(std::move(q));}
    }
  }
  surface.influences=std::move(result);return true;
}

}
