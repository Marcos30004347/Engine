#pragma once
#include "WorldCityLandforms.hpp"
#include "procedural/geom3/Prism.hpp"
#include <array>

namespace procedural::world::rewrite {
// Planning cells supply the topology. Heights are sampled only at shared cell
// corners and at a centre when it represents a significant crest or hollow.
struct CityFacetTerrain {
  std::vector<std::array<Position,3>> faces;
  std::function<double(double,double)> height;
};
inline std::function<double(double,double)> cityFacetHeight(
    const std::vector<std::array<Position,3>> &faces,const std::function<double(double,double)> &source) {
  struct Index {std::vector<std::array<Position,3>> faces;std::map<std::pair<int,int>,std::vector<size_t>> bins;};
  auto index=std::make_shared<Index>();index->faces=faces;
  for(size_t i=0;i<faces.size();++i) {
    const auto &f=faces[i];
    for(int x=int(std::floor(std::min({f[0].x,f[1].x,f[2].x})/64));x<=int(std::floor(std::max({f[0].x,f[1].x,f[2].x})/64));++x)
      for(int z=int(std::floor(std::min({f[0].z,f[1].z,f[2].z})/64));z<=int(std::floor(std::max({f[0].z,f[1].z,f[2].z})/64));++z)index->bins[{x,z}].push_back(i);
  }
  return [index,source](double x,double z) {
    const auto found=index->bins.find({int(std::floor(x/64)),int(std::floor(z/64))});
    if(found!=index->bins.end())for(auto i:found->second) {
      const auto &f=index->faces[i];const Position p{x,0,z};
      const double det=landform_detail::cross(f[0],f[1],f[2]);if(std::abs(det)<1e-10)continue;
      const double a=landform_detail::cross(f[1],f[2],p)/det,b=landform_detail::cross(f[2],f[0],p)/det,c=1-a-b;
      if(std::min({a,b,c})>=-1e-7)return a*f[0].y+b*f[1].y+c*f[2].y;
    }
    return source(x,z);
  };
}
inline CityFacetTerrain cityFacetTerrain(const CityLandformPlan &land,
    const std::function<double(double,double)> &source,double tolerance=.75,bool shapeHeights=true) {
  CityFacetTerrain out;
  if(!source)return out;
  std::map<std::pair<int64_t,int64_t>,double> heights;
  const auto lift=[&](Position p) {
    const auto key=std::make_pair(int64_t(std::llround(p.x*1000)),int64_t(std::llround(p.z*1000)));
    auto [it,inserted]=heights.emplace(key,0.);
    if(inserted)it->second=source(p.x,p.z);
    p.y=it->second;return p;
  };
  for(const auto &cell:land.cells) {
    if(cell.boundary.size()<3)continue;
    std::vector<Position> ring;for(auto p:cell.boundary)ring.push_back(lift(p));
    geom2::Polygon polygon;
    for(auto p:ring)polygon.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
    std::vector<geom2::Point> vertices;std::vector<std::array<uint32_t,3>> triangles;
    if(!geom3::triangulate(polygon,triangles,vertices,nullptr))continue;
    auto centre=lift(cell.centre);bool peak=false;double prominence=0;
    for(auto t:triangles) {
      std::array<Position,3> f;
      for(unsigned i=0;i<3;++i)f[i]=lift({geom2::toMetres(vertices[t[i]].x),0,geom2::toMetres(vertices[t[i]].y)});
      const double det=landform_detail::cross(f[0],f[1],f[2]);if(std::abs(det)<1e-10)continue;
      const double a=landform_detail::cross(f[1],f[2],centre)/det,b=landform_detail::cross(f[2],f[0],centre)/det,c=1-a-b;
      if(std::min({a,b,c})>=-1e-7){prominence=centre.y-a*f[0].y-b*f[1].y-c*f[2].y;peak=std::abs(prominence)>tolerance;}
    }
    if(peak && shapeHeights)centre.y+=prominence*(land.facetPeakBoost-1.);
    if(peak && land.facetCrownFraction>0) {
      // Author a crown/basin contour before triangulation. Its silhouette is
      // inherited from the Voronoi polygon, with a broad level centre and
      // deliberate sloping shoulders rather than a radial point peak.
      std::vector<Position> crown;
      geom2::Polygon cap;
      for(auto p:ring) {
        Position q{centre.x+(p.x-centre.x)*land.facetCrownFraction,centre.y,
                   centre.z+(p.z-centre.z)*land.facetCrownFraction};
        crown.push_back(q);cap.outer.push_back({geom2::toUnit(q.x),geom2::toUnit(q.z)});
      }
      for(size_t i=0;i<ring.size();++i) {
        const auto j=(i+1)%ring.size();
        out.faces.push_back({ring[i],ring[j],crown[j]});
        out.faces.push_back({ring[i],crown[j],crown[i]});
      }
      std::vector<geom2::Point> cv;std::vector<std::array<uint32_t,3>> ct;
      if(geom3::triangulate(cap,ct,cv,nullptr))for(auto t:ct) {
        std::array<Position,3> f;
        for(unsigned i=0;i<3;++i)f[i]={geom2::toMetres(cv[t[i]].x),centre.y,geom2::toMetres(cv[t[i]].y)};
        out.faces.push_back(f);
      }
    }
    else if(peak)for(size_t i=0;i<ring.size();++i)out.faces.push_back({ring[i],ring[(i+1)%ring.size()],centre});
    else for(auto t:triangles) {
      std::array<Position,3> f;for(unsigned i=0;i<3;++i)f[i]=lift({geom2::toMetres(vertices[t[i]].x),0,geom2::toMetres(vertices[t[i]].y)});
      out.faces.push_back(f);
    }
  }
  out.height=cityFacetHeight(out.faces,source);
  return out;
}
}
