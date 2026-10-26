#pragma once
#include "WorldCityTerrainRoutes.hpp"
#include "procedural/ConstrainedDelaunay.hpp"
namespace procedural::world::rewrite {
struct CityTerrainRouteMesh {
  std::vector<Position> vertices;
  std::vector<std::array<uint32_t,3>> triangles;
};
// One terrain layer, CDT constrained by the edited PSLG. Refine locally with
// shared edge stations: repeated global CDT can lose a collinear inserted point.
inline bool meshCityTerrainRoutes(const CityTerrainRoutePlan &plan,double minX,double minZ,double maxX,double maxZ,
    CityTerrainRouteMesh &result,std::string *error=nullptr,double tolerance=.05,size_t maximumPoints=12000) {
  using namespace city_terrain_detail;
  const auto fail=[&](const char *s){if(error)*error=s;return false;};
  if(!plan.ground||!std::isfinite(minX+minZ+maxX+maxZ+tolerance)||maxX<=minX||maxZ<=minZ||tolerance<=0||maximumPoints<4)
    return fail("invalid construction terrain mesh input");
  auto lines=plan.constraints;
  std::vector<Position> boundary={{minX,0,minZ},{maxX,0,minZ},{maxX,0,maxZ},{minX,0,maxZ}};
  for(size_t i=0;i<4;++i)lines.push_back({boundary[i],boundary[(i+1)%4]});
  for(const auto &line:lines)for(auto p:line)if(p.x<minX-1e-7||p.x>maxX+1e-7||p.z<minZ-1e-7||p.z>maxZ+1e-7)
    return fail("construction constraints outside mesh domain");
  std::vector<std::array<Position,2>> noded;
  if(!nodeConstraints(lines,plan.ground,noded))return fail("construction mesh constraint noding failed");
  std::vector<procedural::Point2> points;std::map<std::pair<int64_t,int64_t>,uint32_t> ids;
  auto insert=[&](Position p) {
    auto key=std::make_pair(llround(p.x*1e6),llround(p.z*1e6));
    auto [it,added]=ids.emplace(key,uint32_t(points.size()));
    if(added)points.push_back({p.x,p.z});return it->second;
  };
  using Edge=std::pair<uint32_t,uint32_t>;
  const auto edge=[](uint32_t a,uint32_t b)->Edge{return a<b?Edge{a,b}:Edge{b,a};};
  std::set<Edge> segments;for(const auto &line:noded){auto a=insert(line[0]),b=insert(line[1]);if(a!=b)segments.insert(edge(a,b));}
  if(points.size()>maximumPoints)return fail("construction terrain mesh point budget exceeded");
  std::vector<procedural::Constraint> constraints;for(auto [a,b]:segments)constraints.push_back({a,b});
  procedural::ConstrainedDelaunayTriangulator cdt;
  if(!cdt.triangulate(points,constraints)) {
    // Documented flip/constraint oscillation: same PSLG, no dropped constraints.
    procedural::ConstrainedDelaunayTriangulator::Settings stable;stable.ensureDelaunay=false;
    if(!cdt.triangulate(points,constraints,stable))return fail("construction terrain CDT failed; constraints retained");
  }
  CityTerrainRouteMesh mesh;
  for(auto p:points){double y=plan.ground(p.x,p.y);if(!std::isfinite(y))return fail("nonfinite construction terrain");mesh.vertices.push_back({p.x,y,p.y});}
  std::set<Edge> emitted;
  for(size_t i=0;i<cdt.triangleCount();++i)if(cdt.triangleExists(i)) {
    auto a=cdt.vertexOfTriangle(i,0),b=cdt.vertexOfTriangle(i,1),c=cdt.vertexOfTriangle(i,2);
    if(a<0||b<0||c<0||size_t(std::max({a,b,c}))>=points.size())continue;
    double area=cross(mesh.vertices[a],mesh.vertices[b],mesh.vertices[c]);
    if(std::abs(area)<1e-10)continue;if(area<0)std::swap(b,c);
    mesh.triangles.push_back({uint32_t(a),uint32_t(b),uint32_t(c)});
    emitted.insert(edge(a,b));emitted.insert(edge(b,c));emitted.insert(edge(c,a));
  }
  for(auto e:segments)if(!emitted.count(e))return fail("construction CDT omitted a required edge");
  for(unsigned pass=0;pass<14;++pass) {
    std::set<Edge> split;
    for(auto t:mesh.triangles) {
      auto a=mesh.vertices[t[0]],b=mesh.vertices[t[1]],c=mesh.vertices[t[2]];
      Position centre{(a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3,(a.z+b.z+c.z)/3};
      bool refine=std::abs(plan.ground(centre.x,centre.z)-centre.y)>tolerance;
      for(unsigned i=0;i<3;++i) {
        auto mid=lerp(mesh.vertices[t[i]],mesh.vertices[t[(i+1)%3]],.5);
        refine|=std::abs(plan.ground(mid.x,mid.z)-mid.y)>tolerance;
      }
      // Refine the whole failing face. Repeatedly bisecting just its failing
      // edge creates skinny alternating diagonals that do not converge.
      if(refine)for(unsigned i=0;i<3;++i)split.insert(edge(t[i],t[(i+1)%3]));
    }
    if(split.empty()){result=std::move(mesh);if(error)error->clear();return true;}
    std::map<Edge,uint32_t> midpoints;
    auto lift=[&](Position p){p.y=plan.ground(p.x,p.z);uint32_t id=uint32_t(mesh.vertices.size());mesh.vertices.push_back(p);return id;};
    for(auto e:split)midpoints[e]=lift(lerp(mesh.vertices[e.first],mesh.vertices[e.second],.5));
    std::vector<std::array<uint32_t,3>> triangles;
    for(auto t:mesh.triangles) {
      std::array<uint32_t,3> mid{};std::array<bool,3> cut{};unsigned count=0;
      for(unsigned i=0;i<3;++i){auto m=midpoints.find(edge(t[i],t[(i+1)%3]));if(m!=midpoints.end()){mid[i]=m->second;cut[i]=true;++count;}}
      if(!count){triangles.push_back(t);continue;}
      if(count==3) {
        triangles.push_back({t[0],mid[0],mid[2]});triangles.push_back({mid[0],t[1],mid[1]});
        triangles.push_back({mid[2],mid[1],t[2]});triangles.push_back({mid[0],mid[1],mid[2]});
      } else if(count==1) {
        unsigned i=0;while(!cut[i])++i;
        auto a=t[i],b=t[(i+1)%3],c=t[(i+2)%3],m=mid[i];
        triangles.push_back({a,m,c});triangles.push_back({m,b,c});
      } else {
        unsigned i=0;while(!(cut[i]&&cut[(i+1)%3]))++i;
        auto a=t[i],b=t[(i+1)%3],c=t[(i+2)%3],u=mid[i],v=mid[(i+1)%3];
        triangles.push_back({u,b,v});triangles.push_back({a,u,c});triangles.push_back({u,v,c});
      }
    }
    if(mesh.vertices.size()>maximumPoints)return fail("construction terrain mesh point budget exceeded");
    for(auto p:mesh.vertices)if(!std::isfinite(p.y))return fail("nonfinite construction terrain refinement");
    mesh.triangles=std::move(triangles);
  }
  return fail("construction terrain refinement budget exhausted");
}
}
