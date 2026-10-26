#pragma once
#include "procedural/geom2/Boolean.hpp"
#include "procedural/geom2/Offset.hpp"
#include <map>
#include <cmath>
#include <functional>

namespace procedural::world::rewrite {
// The same simplified centreline must define the road and its sidewalk.
template<class Path> inline std::vector<geom2::Point> cityRoadPath(const Path &input) {
  std::vector<geom2::Point> path;
  for(auto p:input){geom2::Point q{geom2::toUnit(p.x),geom2::toUnit(p.z)};if(path.empty()||!(path.back()==q))path.push_back(q);}
  if(path.size()<3)return path;
  std::vector<bool> keep(path.size(),false);keep.front()=keep.back()=true;
  std::function<void(size_t,size_t)> simplify=[&](size_t first,size_t last){
    const double ax=geom2::toMetres(path[first].x),az=geom2::toMetres(path[first].y),dx=geom2::toMetres(path[last].x-path[first].x),dz=geom2::toMetres(path[last].y-path[first].y);
    double far=.05;size_t at=last;
    for(size_t k=first+1;k<last;++k){double x=geom2::toMetres(path[k].x)-ax,z=geom2::toMetres(path[k].y)-az,t=std::clamp((x*dx+z*dz)/std::max(1e-12,dx*dx+dz*dz),0.,1.);double d=std::hypot(x-t*dx,z-t*dz);if(d>far){far=d;at=k;}}
    if(at!=last){keep[at]=true;simplify(first,at);simplify(at,last);}
  };simplify(0,path.size()-1);
  std::vector<geom2::Point> result;for(size_t k=0;k<path.size();++k)if(keep[k])result.push_back(path[k]);return result;
}
// Road ownership and excavation must not introduce new curb edges. Intersect
// every owned face with the same planned corridor union (nonzero winding).
inline bool splitCityCarriageway(const geom2::Polygon &face,
    const std::vector<geom2::Polygon> &corridors,std::vector<geom2::Polygon> &road,
    std::vector<geom2::Polygon> &walk,std::string *error) {
  geom2::Operand interior;const auto bounds=geom2::bounds(face);
  for(const auto &p:corridors)if(bounds.overlaps(geom2::bounds(p))) {
    auto operand=geom2::toOperand(p);
    interior.rings.insert(interior.rings.end(),operand.rings.begin(),operand.rings.end());
  }
  road.clear();walk.clear();
  if(interior.rings.empty()){walk.push_back(face);return true;}
  return geom2::intersectionOf({geom2::toOperand(face),interior},road,error) &&
         geom2::differenceOf(geom2::toOperand(face),{interior},walk,error);
}
// Erosion is local within its radius. Include a halo wider than that radius,
// offset the connected local road union, then keep only the tile interior.
// Adjacent tiles therefore agree without offsetting an entire city's network.
inline bool insetCityRoadTiles(const std::vector<geom2::Polygon> &roads,double width,
                              std::vector<geom2::Polygon> &out,std::string *error) {
  constexpr double tile=64.;const double halo=std::max(2.,width*4.);
  std::map<std::pair<int,int>,std::vector<size_t>> cells;
  for(size_t i=0;i<roads.size();++i){const auto b=geom2::bounds(roads[i]);
    for(int x=int(std::floor((geom2::toMetres(b.minX)-halo)/tile));x<=int(std::floor((geom2::toMetres(b.maxX)+halo)/tile));++x)
      for(int z=int(std::floor((geom2::toMetres(b.minY)-halo)/tile));z<=int(std::floor((geom2::toMetres(b.maxY)+halo)/tile));++z)cells[{x,z}].push_back(i);
  }
  const auto rectangle=[](double x,double z,double a,double b){geom2::Polygon p;
    p.outer={{geom2::toUnit(x),geom2::toUnit(z)},{geom2::toUnit(a),geom2::toUnit(z)},
             {geom2::toUnit(a),geom2::toUnit(b)},{geom2::toUnit(x),geom2::toUnit(b)}};return geom2::toOperand(p);};
  out.clear();
  for(const auto &[cell,indices]:cells){const double x=cell.first*tile,z=cell.second*tile;
    const auto expanded=rectangle(x-halo,z-halo,x+tile+halo,z+tile+halo),core=rectangle(x,z,x+tile,z+tile);
    std::vector<geom2::Operand> clipped;
    for(auto i:indices){std::vector<geom2::Polygon> parts;
      if(!geom2::intersectionOf({geom2::toOperand(roads[i]),expanded},parts,error))return false;
      for(const auto &p:parts)clipped.push_back(geom2::toOperand(p));}
    if(clipped.empty())continue;
    std::vector<geom2::Polygon> united;
    if(!geom2::unionOf(clipped,united,error))return false;
    for(const auto &p:united){std::vector<geom2::Polygon> inset;
      geom2::OffsetOptions options;options.join=geom2::JoinStyle::Round;options.arcToleranceMetres=.06;
      if(!geom2::offsetPolygon(p,-width,options,inset,error))return false;
      for(const auto &q:inset){std::vector<geom2::Polygon> parts;
        if(!geom2::intersectionOf({geom2::toOperand(q),core},parts,error))return false;
        out.insert(out.end(),std::make_move_iterator(parts.begin()),std::make_move_iterator(parts.end()));}
    }
  }
  return true;
}
}
