#pragma once
#include "WorldCityFacetTerrain.hpp"
#include "procedural/ConstrainedDelaunay.hpp"
#include "procedural/geom2/Offset.hpp"

namespace procedural::world::rewrite {
struct CityContourTerrain : CityFacetTerrain {
  std::vector<std::vector<Position>> contours;
  std::string error;
};
// Two corner-cutting passes round broad landform outlines without height noise.
// Rebuild the polygon cover afterward so narrow necks and holes remain valid.
inline bool smoothCityContourPolygons(std::vector<geom2::Polygon> &polygons,std::string *error=nullptr) {
  const auto smooth=[](geom2::Ring &ring) {
    for(unsigned pass=0;pass<2;++pass) {
      geom2::Ring next;next.reserve(ring.size()*2);
      for(size_t i=0;i<ring.size();++i) {
        const auto a=ring[i],b=ring[(i+1)%ring.size()];
        next.push_back({a.x+(b.x-a.x)/4,a.y+(b.y-a.y)/4});
        next.push_back({b.x+(a.x-b.x)/4,b.y+(a.y-b.y)/4});
      }
      ring=std::move(next);
    }
  };
  std::vector<geom2::Operand> operands;
  for(auto p:polygons) {
    geom2::simplifyPolygon(p,.2,.5);
    smooth(p.outer);for(auto &hole:p.holes)smooth(hole);
    geom2::simplifyPolygon(p,.025,.1);operands.push_back(geom2::toOperand(p));
  }
  if(operands.empty())return true;
  std::vector<geom2::Polygon> result;
  if(!geom2::unionOf(operands,result,error))return false;
  polygons=std::move(result);return true;
}
// Build nested constant-height polygons from Voronoi cell elevation classes.
// No terrain triangles or sampled mesh edges exist until these loops are final.
inline CityContourTerrain cityContourTerrain(const CityLandformPlan &land,
    const std::function<double(double,double)> &source,double interval=2.) {
  CityContourTerrain out;if(!source || land.cells.empty() || interval<=0)return out;
  std::vector<geom2::Polygon> domain;
  std::vector<geom2::Operand> cells;std::vector<double> heights;
  double low=INFINITY,high=-INFINITY;
  for(const auto &cell:land.cells) {
    geom2::Polygon p;for(auto v:cell.boundary)p.outer.push_back({geom2::toUnit(v.x),geom2::toUnit(v.z)});
    cells.push_back(geom2::toOperand(p));double h=source(cell.centre.x,cell.centre.z);
    heights.push_back(h);low=std::min(low,h);high=std::max(high,h);
  }
  if(!geom2::unionOf(cells,domain,&out.error))return out;
  for(auto &p:domain)geom2::simplifyPolygon(p,.25,1.);
  if(!smoothCityContourPolygons(domain,&out.error))return out;
  out.contours=land.elevationContours;
  if(out.contours.empty()) {
    const auto add=[&](const std::vector<geom2::Polygon> &polygons,double level) {
      const auto ring=[&](const geom2::Ring &r){std::vector<Position> loop;for(auto p:r)loop.push_back({geom2::toMetres(p.x),level,geom2::toMetres(p.y)});if(loop.size()>2)out.contours.push_back(std::move(loop));};
      for(const auto &p:polygons){ring(p.outer);for(const auto &h:p.holes)ring(h);}
    };
    const double base=std::floor(low/interval)*interval;
    add(domain,base);auto previous=domain;
    geom2::OffsetOptions options;options.join=geom2::JoinStyle::Round;options.arcToleranceMetres=.2;
    for(double level=base+interval;level<=high && level<=base+interval*128;level+=interval) {
      std::vector<geom2::Operand> selected;
      for(size_t i=0;i<cells.size();++i)if(heights[i]>=level)selected.push_back(cells[i]);
      if(selected.empty())break;
      std::vector<geom2::Polygon> regions,expanded,rounded,inner,next;
      if(!geom2::unionOf(selected,regions,&out.error)){out.error="contour region union: "+out.error;return out;}
      for(auto &p:regions)geom2::simplifyPolygon(p,.5,2.);
      if(!smoothCityContourPolygons(regions,&out.error))return out;
      for(auto &p:previous)geom2::simplifyPolygon(p,.05,.2);
      if(!geom2::offsetPolygons(regions,8.,options,expanded,&out.error) ||
         !geom2::offsetPolygons(expanded,-8.,options,rounded,&out.error) ||
         !geom2::offsetPolygons(previous,-std::max(3.,interval*3.),options,inner,&out.error)){out.error="contour rounding at "+std::to_string(level)+": "+out.error;return out;}
      geom2::Operand a,b;
      for(auto p:rounded){geom2::simplifyPolygon(p,.05,.2);auto o=geom2::toOperand(p);a.rings.insert(a.rings.end(),o.rings.begin(),o.rings.end());}
      for(const auto &p:inner){auto o=geom2::toOperand(p);b.rings.insert(b.rings.end(),o.rings.begin(),o.rings.end());}
      if(a.rings.empty() || b.rings.empty())break;
      if(!geom2::intersectionOf({a,b},next,&out.error))return out;
      if(next.empty())break;
      add(next,level);previous=std::move(next);
    }
  }
  std::vector<procedural::Point2> points;std::vector<double> y;std::vector<procedural::Constraint> edges;
  std::map<std::pair<int64_t,int64_t>,int64_t> ids;
  for(const auto &loop:out.contours) {
    std::vector<int64_t> ring;
    for(auto p:loop) {
      auto key=std::make_pair(geom2::toUnit(p.x),geom2::toUnit(p.z));
      auto [it,added]=ids.emplace(key,int64_t(points.size()));
      if(added){points.push_back({geom2::toMetres(key.first),geom2::toMetres(key.second)});y.push_back(p.y);}
      else if(std::abs(y[it->second]-p.y)>1e-7){out.error="contours at different elevations touch";return out;}
      ring.push_back(it->second);
    }
    for(size_t i=0;i<ring.size();++i)if(ring[i]!=ring[(i+1)%ring.size()])edges.push_back({ring[i],ring[(i+1)%ring.size()]});
  }
  procedural::ConstrainedDelaunayTriangulator mesh;
  if(!mesh.triangulate(points,edges,{})){out.error="elevation contour CDT failed";return out;}
  const auto insideRing=[](const geom2::Ring &ring,double x,double z){bool in=false;for(size_t i=0,j=ring.size()-1;i<ring.size();j=i++) {
    const double ax=geom2::toMetres(ring[i].x),az=geom2::toMetres(ring[i].y),bx=geom2::toMetres(ring[j].x),bz=geom2::toMetres(ring[j].y);
    if((az>z)!=(bz>z) && x<(bx-ax)*(z-az)/(bz-az)+ax)in=!in;
  }return in;};
  for(uint64_t i=0;i<mesh.triangleCount();++i)if(mesh.triangleExists(i)) {
    std::array<int64_t,3> t{mesh.vertexOfTriangle(i,0),mesh.vertexOfTriangle(i,1),mesh.vertexOfTriangle(i,2)};
    if(*std::min_element(t.begin(),t.end())<0 || size_t(*std::max_element(t.begin(),t.end()))>=points.size())continue;
    const double x=(points[t[0]].x+points[t[1]].x+points[t[2]].x)/3.,z=(points[t[0]].y+points[t[1]].y+points[t[2]].y)/3.;
    bool inside=false;for(const auto &p:domain)if(insideRing(p.outer,x,z)){bool hole=false;for(const auto &h:p.holes)hole|=insideRing(h,x,z);inside|=!hole;}
    if(!inside)continue;
    std::array<Position,3> face;for(unsigned k=0;k<3;++k)face[k]={points[t[k]].x,y[t[k]],points[t[k]].y};out.faces.push_back(face);
  }
  out.height=cityFacetHeight(out.faces,source);return out;
}
}
