#include "WorldCityPaving.hpp"
#include "WorldCityPlotContracts.hpp"
#include "WorldCityStructures.hpp"
#include "WorldCityComposition.hpp"
#include "WorldCityTerrainRoutesMesh.hpp"
#include "WorldCityConstructionAdapter.hpp"
#include "WorldCityModel.hpp"
#include "WorldCityYardBoundaries.hpp"
#include "WorldCityFacetTerrain.hpp"
#include "WorldCityRoadPaint.hpp"
#include "WorldCityContourTerrain.hpp"
#include "SpatialBins.hpp"

#include <chrono>
#include <unordered_map>

#include "procedural/ConstrainedDelaunay.hpp"

#include "graphrewrite/runtime/WorkerPool.hpp"
#include <cstdio>
#include <cstdlib>

#include "WorldDwellingPlan.hpp"
#include "WorldArchitecturalClearance.hpp"
#include "procedural/geom3/Prism.hpp"
#include "procedural/geom2/Offset.hpp"
#include "generator/CellDwellingClasses.hpp"
#include "generator/PlazaPlanner.hpp"
#include "generator/CellDwellingPlanner.hpp"
#include "generator/CellDwellingRealise.hpp"
#include "generator/DwellingStylePlanner.hpp"
#include "generator/FortressJunction.hpp"
#include "hierarchy/CellSkeleton.hpp"
#include "hierarchy/DwellingVoxelAssembly.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <set>
#include <tuple>
#include <functional>

namespace procedural::world::rewrite {

namespace {
constexpr double kCityDwellingModule=1.4;
// Where the ground is.
//
// The fabric publishes the settlement's own surface. When the town stands on a
// hill the relief plan publishes that same surface with the streets graded
// down it and the lot pads cut horizontally into it, and that is the one the
// mesh must be built on - otherwise the houses stand on the raw hillside while
// the plan put them on terraces. Nothing in this file reads `fabric.terrain`
// directly; it asks this.
struct Ground
{
  const std::function<double(double, double)> *field = nullptr;
  double datum = 0;

  double operator()(const double x, const double z) const
  {
    return field != nullptr && *field ? (*field)(x, z) : datum;
  }

  bool following() const
  {
    return field != nullptr && bool(*field);
  }
};

inline Ground groundOf(const SettlementFabric &fabric, const CityReliefPlan *relief)
{
  Ground g;
  g.datum = fabric.terraceLevel;
  // The terrain is not carved.
  //
  // It used to be: every lot was a flat pad cut into the hill, and the ground
  // field answered with the terrace under any point inside one. That put a
  // step at every lot boundary, and a step in a surface that stores one
  // height per vertex is a slit you can see through - the cracks - and it
  // needed a face built along every edge to close, and the rim of each lot
  // took its neighbour's level wherever the two outlines crossed. The
  // platform a house stands on is BUILT on the hill instead, so the hill
  // itself is just the hill, with its streets graded down it.
  g.field = relief != nullptr && relief->accessGround ? &relief->accessGround : relief != nullptr && relief->openGround ? &relief->openGround : (fabric.terrain ? &fabric.terrain : nullptr);

  if (relief != nullptr && relief->ground)
  {
    g.datum = relief->minimumLevel;
  }

  return g;
}
} // namespace
namespace {
using Key = std::tuple<int64_t, int64_t, int64_t>;

// Welding is a hash lookup, not a tree walk.
//
// Every triangle of the city mesh asks this three times - thirty million
// lookups for ten million triangles - and a red-black tree pays a pointer
// chase per level plus a three-way tuple compare at each. The vertex ids it
// hands out are assigned on insertion and nothing iterates it, so the mesh is
// identical whichever container holds it.
struct VertexKeyHash
{
  size_t operator()(const Key &key) const noexcept
  {
    const auto mix = [](uint64_t v) {
      v ^= v >> 33;
      v *= 0xFF51AFD7ED558CCDull;
      v ^= v >> 33;
      v *= 0xC4CEB9FE1A85EC53ull;
      return v ^ (v >> 33);
    };

    return size_t(mix(uint64_t(std::get<0>(key))) ^ (mix(uint64_t(std::get<1>(key))) << 1) ^ (mix(uint64_t(std::get<2>(key))) << 2));
  }
};

using VertexIndex = std::unordered_map<Key, uint32_t, VertexKeyHash>;

void triangle(CityModelPlan &out, VertexIndex &index, std::array<Position, 3> p, uint32_t material)
{
  std::array<uint32_t, 3> ids{};
  for (size_t k = 0; k < 3; ++k)
  {
    const Key key{int64_t(std::llround(p[k].x * 10000)), int64_t(std::llround(p[k].y * 10000)), int64_t(std::llround(p[k].z * 10000))};
    auto it = index.find(key);
    if (it == index.end())
    {
      it = index.emplace(key, uint32_t(out.mesh.vertices.size())).first;
      out.mesh.vertices.push_back(p[k]);
    }
    ids[k] = it->second;
  }
  if (ids[0]==ids[1] || ids[1]==ids[2] || ids[2]==ids[0]) return;
  // Validate the welded triangle, not its pre-weld coordinates. Fine joinery
  // can contain points closer than the shared mesh quantisation; using the old
  // coordinates produced a non-zero normal for a triangle whose indices had
  // already collapsed to one edge.
  const Position a = out.mesh.vertices[ids[0]], b = out.mesh.vertices[ids[1]], c = out.mesh.vertices[ids[2]];
  Position n{(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y), (b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z), (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)};
  const double l = std::hypot(n.x, std::hypot(n.y, n.z));
  if (l <= 1e-10) return;

  out.mesh.triangles.push_back(ids);
  out.mesh.expectedNormals.push_back({n.x/l, n.y/l, n.z/l});
  out.materials.push_back(material);
  out.triangleUVs.push_back({{{p[0].x,p[0].z},{p[1].x,p[1].z},{p[2].x,p[2].z}}});
  const bool masonry=(material>=31 && material<=38 && material!=34) || material==54;
  out.surfaceMaterials.push_back(masonry?1:material==34 || material==39?2:material==55?3:0);
  // A steep face is textured against the wall it looks like, not the plan it
  // is drawn on. Masonry already did this; ground did not, and ground on a
  // hillside is how the bank below a cut terrace gets built - projected in
  // plan it smears one row of texels into vertical streaks the whole height
  // of the bank. Which axis to use is the face's own normal, so nothing has
  // to know whether a triangle was meant to be a floor or a wall.
  if(std::abs(n.y/l)<=.7)for(size_t k=0;k<3;++k)
    out.triangleUVs.back()[k]={std::abs(n.x)>std::abs(n.z)?p[k].z:p[k].x,p[k].y};
  out.groundWeights.push_back({});
  out.roadOwners.push_back(0);
  out.roadSegments.push_back(0);
}

// Shared edge criterion: flat ground and straight ramps need no extra faces.
// Both owners of an edge (including its kerb) make the same decision.
bool terrainEdgeNeedsSplit(Position a,Position b,const Ground &ground,double spacing)
{
  if(std::hypot(b.x-a.x,b.z-a.z)<=spacing)return false;
  const double ha=ground(a.x,a.z),hb=ground(b.x,b.z);
  for(double t:{.25,.5,.75})
    if(std::abs(ground(a.x+(b.x-a.x)*t,a.z+(b.z-a.z)*t)-(ha+(hb-ha)*t))>.02)return true;
  return false;
}

// One surface, triangulated and refined but not yet welded.
//
// The refinement is pure - it reads the piece, the terrain and the ground
// field and touches nothing shared - so it can run on a worker. Welding is
// not: it assigns vertex ids in order. Keeping them apart is what lets the
// city mesh be built by ten threads and still come out identical, because the
// welding still happens once, in piece order, on one thread.
struct RefinedSurface
{
  std::vector<std::array<Position,3>> triangles;
  std::vector<std::array<std::array<double,3>,3>> weights;
  uint32_t material=51;
  uint32_t bridges=0,eared=0;
  bool road=false;
  uint64_t owner=0;
  uint32_t segment=0;
  bool ok=true;
  std::string error;
};

RefinedSurface refineSurface(const CitySurfacePiece &piece,const Ground &ground,double level,bool levelled,double spacing,
                             const CityGroundWeightIndex &weights)
{
  RefinedSurface out;
  // A retaining band is not ground seen flat, it is a wall seen from the
  out.material=piece.kind==CitySurfaceKind::Road ? 50 : piece.kind==CitySurfaceKind::Lot ? 52 : 51;
  out.road=piece.kind==CitySurfaceKind::Road || piece.kind==CitySurfaceKind::Sidewalk;
  out.owner=piece.owner;
  out.segment=piece.segment;

  // Delaunay, not ear clipping.
  //
  // Ear clipping fans a polygon from whichever vertex it can clip, so a
  // sixty-four metre tile of ground with holes in it comes out as a spray of
  // splinters - two hundred and eighty thousand of them across the city,
  // metres long and centimetres wide, which is what draws as those pale
  // cracks fanning across the grass. A constrained Delaunay maximises the
  // smallest angle instead, so the same ground comes out as triangles.
  std::vector<geom2::Point> vertices;std::vector<std::array<uint32_t,3>> triangles;
  std::string why;
  {
    std::vector<Point2> points;
    std::vector<procedural::Constraint> constraints;

    // One index per distinct point: a hole that touches its outer ring shares
    // a vertex, and a triangulator handed the same point twice has no edge
    // between them to constrain.
    std::map<std::pair<int64_t,int64_t>,int64_t> seen;
    const auto addRing=[&](const geom2::Ring &ring) {
      if(ring.size()<3)return;
      std::vector<int64_t> indices;
      indices.reserve(ring.size());

      for(const auto &q:ring) {
        const std::pair<int64_t,int64_t> key{q.x,q.y};
        const auto found=seen.find(key);

        if(found!=seen.end()){indices.push_back(found->second);continue;}

        const int64_t at=int64_t(points.size());
        seen.emplace(key,at);
        points.push_back({geom2::toMetres(q.x),geom2::toMetres(q.y)});
        vertices.push_back(q);
        indices.push_back(at);
      }

      for(size_t k=0;k<indices.size();++k) {
        const int64_t a=indices[k],b=indices[(k+1)%indices.size()];
        if(a!=b)constraints.push_back({a,b});
      }
    };

    addRing(piece.polygon.outer);
    for(const auto &hole:piece.polygon.holes)addRing(hole);

    procedural::ConstrainedDelaunayTriangulator mesh;
    procedural::ConstrainedDelaunayTriangulator::Settings settings;
    settings.ensureDelaunay=true;

    // Ear clipping is the fallback, not the method.
    //
    // A triangulator that refuses is worse than one that makes splinters, and
    // the Delaunay does refuse on some remainder polygons the booleans leave.
    // Those pieces keep the old tessellation; everything else gets triangles.
    if(!mesh.triangulate(points,constraints,settings)) {
      vertices.clear();

      if(!geom3::triangulate(piece.polygon,triangles,vertices,&why)) {
        out.ok=false;
        out.error="city surface "+std::to_string(int(piece.kind))+" owner "+std::to_string(piece.owner)+": "+why+" ("+geom2::polygonFault(piece.polygon)+")";
        return out;
      }

      ++out.eared;
    }
    else
    {
    // The triangulation fills the convex hull, so the pieces outside the
    // outline and inside its holes are dropped by testing each centroid.
    const auto inside=[&](const double x,const double y) {
      const auto crosses=[&](const geom2::Ring &ring) {
        bool in=false;
        for(size_t i=0,j=ring.size()-1;i<ring.size();j=i++) {
          const double ix=geom2::toMetres(ring[i].x),iy=geom2::toMetres(ring[i].y);
          const double jx=geom2::toMetres(ring[j].x),jy=geom2::toMetres(ring[j].y);
          if((iy>y)!=(jy>y) && x<(jx-ix)*(y-iy)/(jy-iy)+ix)in=!in;
        }
        return in;
      };

      if(!crosses(piece.polygon.outer))return false;
      for(const auto &hole:piece.polygon.holes)if(crosses(hole))return false;
      return true;
    };

    const uint64_t slots=mesh.triangleCount();
    const size_t inputs=points.size();

    for(uint64_t k=0;k<slots;++k) {
      if(!mesh.triangleExists(k))continue;
      const int64_t a=mesh.vertexOfTriangle(k,0),b=mesh.vertexOfTriangle(k,1),c=mesh.vertexOfTriangle(k,2);
      if(a<0||b<0||c<0)continue;
      if(size_t(a)>=inputs||size_t(b)>=inputs||size_t(c)>=inputs)continue; // super-triangle
      const double cx=(points[a].x+points[b].x+points[c].x)/3.,cy=(points[a].y+points[b].y+points[c].y)/3.;
      if(!inside(cx,cy))continue;
      triangles.push_back({uint32_t(a),uint32_t(b),uint32_t(c)});
    }
    }
  }

  // A terrace is drawn at its own level, not at whatever the field reads
  // under each of its vertices.
  //
  // Two lots that share an edge each have rim vertices a hair inside the
  // other's outline, and a pad is a hard override wherever it contains a
  // point - so one lot's rim took its NEIGHBOUR's level, and the triangle
  // from that rim to its own flat interior carried the whole step between
  // the two terraces. Measured, those triangles spanned one to two metres
  // each, and they are what came up through the floors of the houses
  // standing on them. The level is a number the plan decided for this piece;
  // sampling the field at the piece's own edge cannot answer for it.
  const auto height=[&](const double x,const double z) {
    return (levelled ? level : piece.terrainFollowing ? ground(x,z) : ground.datum)+piece.height;
  };
  const auto point=[&](uint32_t i){const double x=geom2::toMetres(vertices[i].x),z=geom2::toMetres(vertices[i].y);
    return Position{x,height(x,z),z};};
  const auto groundPoint=[&](Position p) { p.y=height(p.x,p.z);return p; };

  // The ground field, once per POINT of this piece.
  //
  // The serial build cached it by welded vertex id, so a vertex shared by six
  // triangles was evaluated once; asking per triangle corner instead
  // multiplied the field queries sixfold and doubled the CPU of the whole
  // model. The same dedup, keyed by the same quantisation the weld uses.
  std::unordered_map<uint64_t,std::array<double,3>> local;
  const auto weightAt=[&](const Position &p)->std::array<double,3> {
    const uint64_t key=uint64_t(int64_t(std::llround(p.x*10000)))*1469598103934665603ull
                      ^uint64_t(int64_t(std::llround(p.z*10000)));
    const auto found=local.find(key);
    if(found!=local.end())return found->second;
    const auto value=cityGroundWeights(weights,p.x,p.z);
    local.emplace(key,value);
    return value;
  };

  // Whether an edge splits may depend on that edge and nothing else.
  //
  // The rule used to be "split edges longer than the spacing, in triangles
  // larger than a quarter cell". Area belongs to the triangle, not the edge,
  // so of the two triangles either side of a long edge the fat one split it
  // and the thin one did not. That is a T-junction: the split side steps down
  // the ground, the unsplit side crosses it as a straight chord, and the gap
  // between them is the crack. It is why the cracks disappear when the camera
  // looks straight down - nothing is missing from above, the two surfaces
  // just sit at different heights in between. Testing the edge alone makes
  // both sides reach the same answer, and a conforming refinement has no
  // T-junctions to crack along.
  std::function<void(std::array<Position,3>,unsigned)> emit=[&](std::array<Position,3> p,unsigned depth) {
    std::array<bool,3> split{};std::array<Position,3> midpoint{};unsigned count=0;
    for(size_t k=0;k<3;++k) {
      const auto a=p[k],b=p[(k+1)%3];
      if(!levelled && piece.terrainFollowing && terrainEdgeNeedsSplit(a,b,ground,spacing) && depth<16) {
        midpoint[k]=groundPoint({(a.x+b.x)*.5,0,(a.z+b.z)*.5});split[k]=true;++count;
      }
    }
    if(count==3) {
      emit({p[0],midpoint[0],midpoint[2]},depth+1);emit({midpoint[0],p[1],midpoint[1]},depth+1);
      emit({midpoint[2],midpoint[1],p[2]},depth+1);emit({midpoint[0],midpoint[1],midpoint[2]},depth+1);return;
    }
    if(count==1) {
      size_t k=0;while(!split[k])++k;
      emit({p[k],midpoint[k],p[(k+2)%3]},depth+1);emit({midpoint[k],p[(k+1)%3],p[(k+2)%3]},depth+1);return;
    }
    if(count==2) {
      size_t k=0;while(split[k])++k;const size_t b=(k+1)%3,c=(k+2)%3;
      emit({p[c],midpoint[c],midpoint[b]},depth+1);emit({p[k],p[b],midpoint[c]},depth+1);emit({p[b],midpoint[b],midpoint[c]},depth+1);
      return;
    }
    // Detect an interior crest/valley even when all boundary edges are flat.
    const Position centre=groundPoint({(p[0].x+p[1].x+p[2].x)/3.,0,(p[0].z+p[1].z+p[2].z)/3.});
    if(depth<16 && std::abs(centre.y-(p[0].y+p[1].y+p[2].y)/3.)>.02 &&
       std::max({std::hypot(p[0].x-p[1].x,p[0].z-p[1].z),std::hypot(p[1].x-p[2].x,p[1].z-p[2].z),std::hypot(p[2].x-p[0].x,p[2].z-p[0].z)})>spacing) {
      emit({p[0],p[1],centre},depth+1);emit({p[1],p[2],centre},depth+1);emit({p[2],p[0],centre},depth+1);return;
    }
    out.triangles.push_back(p);
    std::array<std::array<double,3>,3> weight{};
    for(size_t k=0;k<3;++k)weight[k]=weightAt(p[k]);
    out.weights.push_back(weight);
  };

  // Boolean ownership can legitimately leave very thin triangles along a
  // road boundary. Dropping them by aspect ratio opens a visible ground crack.
  // Only zero-area faces are disposable; preserve the complete owned cover.
  for(auto t:triangles) {
    const auto a=point(t[0]),b=point(t[2]),c=point(t[1]);
    const double ab=std::hypot(b.x-a.x,b.z-a.z),bc=std::hypot(c.x-b.x,c.z-b.z),ca=std::hypot(a.x-c.x,a.z-c.z);
    const double longest=std::max(ab,std::max(bc,ca));
    const double area=std::abs((b.x-a.x)*(c.z-a.z)-(c.x-a.x)*(b.z-a.z))*.5;
    if(longest<=1e-12 || area<=1e-14){++out.bridges;continue;}
    emit({a,b,c},0);
  }

  return out;
}

// The welding half: piece order, one thread, identical to the serial run.
// The face that closes the step from the carved street up to the ground.
//
// It is built with the same "split an edge longer than the spacing" rule the
// ground uses, so the top of the kerb lands on exactly the points the ground
// beside it lands on. Built as a plain chord it would be the one place in the
// city that still cracked.
void kerb(CityModelPlan &out,VertexIndex &index,const Ground &ground,double spacing,
          double bottom,double top,Position a,Position b,uint32_t material,
          const CityGroundWeightIndex &weights)
{
  // The kerb is ground seen edge on, so it is textured as ground.
  //
  // Left to the default it took the flat palette colour of the street
  // material and drew as a pale dashed line along every kerb - the very thing
  // the cracks looked like. It takes the ground's own textures instead, with
  // the upright UVs a wall gets rather than the plan UVs a floor gets, which
  // would collapse to a single column of texels on a vertical face.
  const auto face=[&](const std::array<Position,3> &p,const bool alongZ) {
    const size_t before=out.mesh.triangles.size();
    triangle(out,index,p,material);
    if(out.mesh.triangles.size()==before)return;
    out.surfaceMaterials.back()=11;
    for(size_t k=0;k<3;++k) {
      out.triangleUVs.back()[k]={alongZ?p[k].z:p[k].x,p[k].y};
      out.groundWeights.back()[k]=cityGroundWeights(weights,p[k].x,p[k].z);
    }
  };

  const bool alongZ=std::abs(b.z-a.z)>std::abs(b.x-a.x);
  std::function<void(Position,Position,unsigned)> run=[&](Position p,Position q,unsigned depth) {
    if(terrainEdgeNeedsSplit(p,q,ground,spacing) && depth<16) {
      const Position middle{(p.x+q.x)*.5,0,(p.z+q.z)*.5};
      run(p,middle,depth+1);run(middle,q,depth+1);return;
    }
    const double py=ground(p.x,p.z),qy=ground(q.x,q.z);
    Position low{p.x,py+bottom,p.z},next{q.x,qy+bottom,q.z},high=next,back=low;
    high.y=qy+top;back.y=py+top;
    face({low,next,high},alongZ);
    face({low,high,back},alongZ);
  };
  run(a,b,0);
}

bool weldSurface(CityModelPlan &out,VertexIndex &index,const CitySurfacePiece &piece,const RefinedSurface &refined,
                 const Ground &ground,double spacing,std::string *error)
{
  if(!refined.ok) { if(error)*error=refined.error; return false; }

  for(size_t t=0;t<refined.triangles.size();++t) {
    const size_t before=out.mesh.triangles.size();
    triangle(out,index,refined.triangles[t],refined.material);
    if(out.mesh.triangles.size()>before) {
      const bool flags=piece.kind==CitySurfaceKind::Sidewalk || piece.kind==CitySurfaceKind::StairTread;
      const bool court=piece.kind==CitySurfaceKind::Court;
      const bool patternedRoad=piece.kind==CitySurfaceKind::Road && piece.pavingMaterial==13 && piece.owner%3==1;
      out.surfaceMaterials.back()=flags?(piece.owner%2?33:18):court?19:patternedRoad?32:piece.pavingMaterial?piece.pavingMaterial:11;
      // Keep each route in one paving family; continuous world UVs avoid
      // restarting the bond at triangulation and ownership boundaries.
      out.groundWeights.back()=(flags || court || piece.pavingMaterial) ? CityModelPlan::GroundWeights{} : refined.weights[t];
      if(refined.road){++out.roadTriangles;out.roadOwners.back()=refined.owner;out.roadSegments.back()=refined.segment;}
    }
  }

  const bool raisedPublic=piece.kind==CitySurfaceKind::Court || piece.kind==CitySurfaceKind::StairTread;
  if((!piece.terrainFollowing || raisedPublic) && piece.height>piece.bottom) {
    std::function<void(Position,Position,unsigned)> edge=[&](Position p,Position q,unsigned depth) {
      if(terrainEdgeNeedsSplit(p,q,ground,spacing) && depth<16) {
        Position middle{(p.x+q.x)*.5,0,(p.z+q.z)*.5};
        edge(p,middle,depth+1);edge(middle,q,depth+1);return;
      }
      p.y=(piece.terrainFollowing?ground(p.x,p.z):ground.datum)+piece.bottom;
      q.y=(piece.terrainFollowing?ground(q.x,q.z):ground.datum)+piece.bottom;
      Position r=q,s=p;r.y+=piece.height-piece.bottom;s.y+=piece.height-piece.bottom;
      triangle(out,index,{p,q,r},54);triangle(out,index,{p,r,s},54);
    };
    const auto skirt=[&](const geom2::Ring &ring) {
      for(size_t i=0;i<ring.size();++i) {
        auto a=ring[i],b=ring[(i+1)%ring.size()];
        edge({geom2::toMetres(a.x),0,geom2::toMetres(a.y)},{geom2::toMetres(b.x),0,geom2::toMetres(b.y)},0);
      }
    };
    skirt(piece.polygon.outer);
    for(const auto &hole:piece.polygon.holes)skirt(hole);
  }

  return true;
}

void prism(CityModelPlan &out, VertexIndex &index, const std::vector<Position> &ring, double bottom, double top, uint32_t wallMaterial, uint32_t topMaterial)
{
  if (ring.size() < 3) return;
  auto oriented=ring;double area=0;for(size_t i=0;i<ring.size();++i){const auto a=ring[i],b=ring[(i+1)%ring.size()];area+=a.x*b.z-b.x*a.z;}
  if(area<0)std::reverse(oriented.begin(),oriented.end());
  for (size_t i = 0; i < ring.size(); ++i)
  {
    Position a = oriented[i], b = oriented[(i+1)%ring.size()], c = b, d = a;
    a.y = b.y = bottom; c.y = d.y = top;
    triangle(out, index, {a,c,b}, wallMaterial);
    triangle(out, index, {a,d,c}, wallMaterial);
  }
  // Platforms and stair courts have re-entrant corners. A fan bridges
  // those notches and leaves a false slab over otherwise valid stairs.
  std::vector<geom3::Vertex> vertices;
  for(auto p:oriented)vertices.push_back({p.x,top,p.z});
  std::vector<std::array<uint32_t,3>> faces;
  if(!geom3::triangulateFace(vertices,faces,nullptr))return;
  for(auto f:faces) {
    Position a=oriented[f[0]],b=oriented[f[1]],c=oriented[f[2]];
    a.y=b.y=c.y=top;
    if((b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z)<0)std::swap(b,c);
    triangle(out,index,{a,b,c},topMaterial);
  }
}

std::vector<Position> interiorViews(const DwellingPlan &plan,double base)
{
  std::vector<Position> views;
  for(const auto &room:plan.rooms) {
    if(room.boundary.size()<3)continue;
    std::vector<geom3::Vertex> vertices;for(auto p:room.boundary)vertices.push_back({p.x,p.y,p.z});
    std::vector<std::array<uint32_t,3>> triangles;
    if(!geom3::triangulateFace(vertices,triangles,nullptr))continue;
    double largest=0;Position eye{};
    for(auto t:triangles){const auto a=vertices[t[0]],b=vertices[t[1]],c=vertices[t[2]];
      const double area=std::abs((b.x-a.x)*(c.z-a.z)-(b.z-a.z)*(c.x-a.x));
      if(area>largest){largest=area;eye={(a.x+b.x+c.x)/3,base+a.y+1.55,(a.z+b.z+c.z)/3};}}
    if(largest>1)views.push_back(eye);
    if(views.size()>=24)break;
  }
  return views;
}

Position mean(const std::vector<Position> &p)
{
  Position c{};
  for (auto q : p) { c.x += q.x; c.z += q.z; }
  if (!p.empty()) { c.x /= p.size(); c.z /= p.size(); }
  return c;
}

std::vector<Position> inset(const std::vector<Position> &p, double amount)
{
  const Position c = mean(p);
  double radius = 0;
  for (auto q : p) radius += std::hypot(q.x-c.x, q.z-c.z);
  radius = p.empty() ? 0 : radius / p.size();
  const double scale = radius > amount + .2 ? (radius-amount)/radius : .65;
  std::vector<Position> out;
  for (auto q : p) out.push_back({c.x+(q.x-c.x)*scale, q.y, c.z+(q.z-c.z)*scale});
  return out;
}

void roof(CityModelPlan &out, VertexIndex &index, const std::vector<Position> &ring, double eave, double rise, uint32_t material)
{
  Position peak = mean(ring); peak.y = eave + rise;
  for (size_t i = 0; i < ring.size(); ++i)
  {
    Position a = ring[i], b = ring[(i+1)%ring.size()]; a.y = b.y = eave;
    triangle(out, index, {a,b,peak}, material);
  }
}

// Faces a solid could not triangulate, over the whole city. One that folded
// is dropped, counted and reported rather than failing the lot - and through
// the lot, the whole city, which is what one folded board did. The count is
// in the debug summary so it cannot creep up unnoticed.
static uint32_t gDroppedFaces=0;

// The map that takes a dwelling out of the rectilinear frame it was planned
// in and onto the ground it actually stands on. Null is the identity.
using Warp = std::function<Position(Position)>;

#include "WorldCityWaterwayMesh.inc"

bool solid(CityModelPlan &out, VertexIndex &index, const hierarchy::Solid &solid, double y, uint32_t baseMaterial, std::string *error, const Warp *warp = nullptr, bool instances = false)
{
  bool valid=true;
  auto faces=[&](const std::vector<hierarchy::Solid::Face> &group,uint32_t material) {
    for (size_t fi=0;fi<group.size();++fi) if (group[fi].size()>=3)
    {
      if (!valid) return;
      auto face=group[fi];
      // Atlas quads arrive as adjacent triangle pairs. Rejoin only identical
      // shared corners, including material and UV seams.
      const auto same=[](const auto &a,const auto &b){return a.x==b.x && a.y==b.y && a.z==b.z && a.u==b.u && a.v==b.v && a.material==b.material;};
      bool paired=false;
      if(instances && face.size()==3 && fi+1<group.size() && group[fi+1].size()==3 &&
         same(face[0],group[fi+1][0]) && same(face[2],group[fi+1][1])) {
        face.push_back(group[fi+1][2]);paired=true;
      }
      if(instances && face.size()==4) {
        std::array<Position,4> p;
        for(size_t k=0;k<4;++k){p[k]={face[k].x,face[k].y+y,face[k].z};if(warp && *warp)p[k]=(*warp)(p[k]);}
        const auto sub=[](Position a,Position b){return Position{a.x-b.x,a.y-b.y,a.z-b.z};};
        const auto cross=[](Position a,Position b){return Position{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
        const auto dot=[](Position a,Position b){return a.x*b.x+a.y*b.y+a.z*b.z;};
        const auto n=cross(sub(p[1],p[0]),sub(p[2],p[0]));
        const double norm=std::sqrt(dot(n,n));
        bool convex=norm>1e-8 && std::abs(dot(n,sub(p[3],p[0])))<norm*1e-7;
        for(size_t k=0;k<4;++k)convex &= dot(n,cross(sub(p[(k+1)%4],p[k]),sub(p[(k+2)%4],p[(k+1)%4])))>1e-12 && face[k].material==face[0].material;
        if(convex) {
          CityModelPlan::QuadInstance q{};
          for(size_t k=0;k<4;++k){q[k*3]=p[k].x;q[k*3+1]=p[k].y;q[k*3+2]=p[k].z;q[12+k*2]=face[k].u;q[13+k*2]=face[k].v;}
          q[20]=material;q[21]=uint32_t(face[0].material);out.quadInstances.push_back(q);
          if(paired)++fi;
          continue;
        }
      }
      if(paired)face.pop_back(); // Unsafe pair: retain both original triangles.
      std::vector<geom3::Vertex> vertices;
      for (auto p:face) vertices.push_back({p.x,p.y+y,p.z});
      std::vector<std::array<uint32_t,3>> triangles;
      if (!geom3::triangulateFace(vertices,triangles,error)) {
        ++gDroppedFaces;
        if(std::getenv("CITY_DEBUG") && gDroppedFaces<=8) {
          std::string why="folded face dropped: material "+std::to_string(material)+": "+(error?*error:std::string());
          for (auto p:vertices) why+=" ["+std::to_string(p.x)+","+std::to_string(p.y)+","+std::to_string(p.z)+"]";
          std::fprintf(stderr,"%s\n",why.c_str());
        }
        if(error)error->clear();
        continue;
      }
      for (auto t:triangles) {
        // Warped after triangulation, never before: a face is planar in the
        // frame it was planned in, and the triangulator needs it that way.
        // Once it is triangles, bending them is safe - a triangle is planar
        // wherever its corners are put.
        const auto vertex=[&](size_t k){ const auto p=vertices[k]; const Position q{p.x,p.y,p.z}; return warp!=nullptr && *warp ? (*warp)(q) : q; };
        const auto before=out.mesh.triangles.size();
        triangle(out,index,{vertex(t[0]),vertex(t[1]),vertex(t[2])},material);
        if(out.mesh.triangles.size()!=before) {
          for(size_t k=0;k<3;++k)out.triangleUVs.back()[k]={face[t[k]].u,face[t[k]].v};
          out.surfaceMaterials.back()=uint32_t(face[t[0]].material);
        }
      }
    }
  };
  faces(solid.walls,baseMaterial); faces(solid.innerWalls,baseMaterial+1); faces(solid.slopes,baseMaterial+2); faces(solid.gables,baseMaterial);
  faces(solid.soffits,baseMaterial+3); faces(solid.pillars,baseMaterial); faces(solid.joinery,baseMaterial+4); faces(solid.reveals,baseMaterial); // Exterior jambs belong to the visible shell.
  faces(solid.innerReveals,baseMaterial+1); faces(solid.decking,baseMaterial+3); faces(solid.stairs,baseMaterial+3); faces(solid.timbers,baseMaterial+5);
  faces(solid.furniture,baseMaterial+5); faces(solid.dormerWalls,baseMaterial); faces(solid.dormerWindows,baseMaterial+4); faces(solid.dormerRoofs,baseMaterial+2);
  faces(solid.dormerCeilings,baseMaterial+3); faces(solid.yard,baseMaterial+6);
  faces(solid.foundations,53); // Dark stone entrance platforms, distinct from block paving.
  return valid;
}

std::vector<Position> drum(Position c, double radius, uint32_t sides)
{
  std::vector<Position> ring;
  for (uint32_t i = 0; i < sides; ++i)
  {
    const double a = 6.283185307179586 * i / sides;
    ring.push_back({c.x + radius*std::cos(a), c.y, c.z + radius*std::sin(a)});
  }
  return ring;
}

bool pointInside(Position p, const std::vector<Position> &poly)
{
  bool in = false;
  for (size_t i=0, j=poly.size()-1; i<poly.size(); j=i++)
    if ((poly[i].z>p.z)!=(poly[j].z>p.z) && p.x < (poly[j].x-poly[i].x)*(p.z-poly[i].z)/(poly[j].z-poly[i].z)+poly[i].x) in=!in;
  return in;
}

double segmentDistance(Position p, Position a, Position b)
{
  const double dx=b.x-a.x,dz=b.z-a.z,l2=dx*dx+dz*dz;
  const double t=l2>1e-12?std::clamp(((p.x-a.x)*dx+(p.z-a.z)*dz)/l2,0.,1.):0;
  return std::hypot(p.x-(a.x+dx*t),p.z-(a.z+dz*t));
}

CityModelBuildingKind districtKind(DistrictUse use)
{
  if (use == DistrictUse::Civic || use == DistrictUse::Guild) return CityModelBuildingKind::Civic;
  if (use == DistrictUse::Temple || use == DistrictUse::Church) return CityModelBuildingKind::Religious;
  if (use == DistrictUse::Craftsmen) return CityModelBuildingKind::Workshop;
  return CityModelBuildingKind::House;
}

bool housingDistrict(DistrictUse use)
{
  return use==DistrictUse::Residential || use==DistrictUse::Suburb || use==DistrictUse::Craftsmen || use==DistrictUse::Merchant ||
         use==DistrictUse::WealthyResidential || use==DistrictUse::WorkerHousing;
}

generator::BuildingClass buildingClass(DistrictUse use, double area)
{
  if (area < 48) return generator::BuildingClass::Hut;
  if (area < 78) return generator::BuildingClass::PeasantCottage;
  if (use==DistrictUse::Craftsmen || use==DistrictUse::Merchant) return generator::BuildingClass::ShopHouse;
  if (use==DistrictUse::WealthyResidential) return generator::BuildingClass::Mansion;
  if (use==DistrictUse::WorkerHousing) return generator::BuildingClass::Dormitory;
  if (use==DistrictUse::Suburb) return generator::BuildingClass::PeasantCottage;
  return generator::BuildingClass::Dwelling;
}
} // namespace

bool buildCastleDungeonCutaway(const generator::CompoundPlan &castle,uint32_t level,CityModelPlan &result,
                               std::string *error,const CityModelSettings *voxelSettings)
{
  CityModelPlan out;VertexIndex index;std::set<std::array<int64_t,4>> emitted;
  bool found=false;
  const auto wall=[&](Position a,Position b,double bottom,double top) {
    const double len=std::hypot(b.x-a.x,b.z-a.z);if(len<.01)return;
    const double nx=-(b.z-a.z)/len*.18,nz=(b.x-a.x)/len*.18;
    prism(out,index,{{a.x-nx,0,a.z-nz},{b.x-nx,0,b.z-nz},{b.x+nx,0,b.z+nz},{a.x+nx,0,a.z+nz}},bottom,top,36,37);
  };
  for(const auto &room:castle.dungeonRooms)if(room.level==level) {
    found=true;const auto &ring=room.footprint;
    const size_t first=out.mesh.triangles.size();
    prism(out,index,ring,room.floor-.2,room.floor,36,37);
    for(size_t i=0;i<ring.size();++i) {
      const auto a=ring[i],b=ring[(i+1)%ring.size()];
      std::array<int64_t,2> ka{std::llround(a.x*1000),std::llround(a.z*1000)},kb{std::llround(b.x*1000),std::llround(b.z*1000)};
      if(kb<ka)std::swap(ka,kb);
      if(!emitted.insert({ka[0],ka[1],kb[0],kb[1]}).second)continue;
      const auto door=std::find_if(castle.dungeonConnections.begin(),castle.dungeonConnections.end(),[&](const auto &link){
        return !link.stairs && (link.from==room.id || link.to==room.id) && segmentDistance(link.position,a,b)<.01;
      });
      if(door==castle.dungeonConnections.end())wall(a,b,room.floor,room.floor+1.35);
      else {
        const double len=std::hypot(b.x-a.x,b.z-a.z),ux=(b.x-a.x)/len,uz=(b.z-a.z)/len;
        wall(a,{door->position.x-ux*.9,0,door->position.z-uz*.9},room.floor,room.floor+1.35);
        wall({door->position.x+ux*.9,0,door->position.z+uz*.9},b,room.floor,room.floor+1.35);
      }
    }
    if(room.role=="stair-hall") {
      const auto centre=mean(ring);const auto a=ring[0],b=ring[1];
      const double length=std::hypot(b.x-a.x,b.z-a.z),ux=(b.x-a.x)/length,uz=(b.z-a.z)/length;
      for(unsigned step=0;step<24;++step) {
        const double t=-3.6+step*.3;
        const auto point=[&](double x,double z){return Position{centre.x+ux*x-uz*z,0,centre.z+uz*x+ux*z};};
        prism(out,index,{point(-.8,t),point(.8,t),point(.8,t+.3),point(-.8,t+.3)},room.floor,room.floor+(step+1)/6.,36,37);
      }
    }
    if(voxelSettings && voxelSettings->voxelUniqueMeshSink) {
      CityVoxelUniqueMeshPart part{room.id,CityModelBuildingKind::CastleRoom,"dungeon-room",
                                   first,out.mesh.triangles.size()-first};
      if(!voxelSettings->voxelUniqueMeshSink(part,out,error))return false;
    }
    out.buildings.push_back({room.id,castle.id,CityModelBuildingKind::CastleRoom,ring,room.floor,4,0,true,false});
  }
  if(!found){if(error)*error="dungeon level is not present";return false;}
  result=std::move(out);if(error)error->clear();return true;
}

bool buildCityModel(const CityCompletionPlan &city, const SettlementFabric &fabric, const CityModelSettings &settings, CityModelPlan &result, std::string *error, const CityReliefPlan *relief)
{
  auto fail = [&](const std::string &why) { if (error) *error = why; return false; };
  if (!city.revision || !std::isfinite(settings.lotInset) || settings.lotInset < 0 || settings.towerSides < 6 ||
      !std::isfinite(settings.contourIntervalMetres) || settings.contourIntervalMetres<.25 ||
      !std::isfinite(settings.groundVertexSpacing) || settings.groundVertexSpacing<.25) return fail("invalid city model input");

  if(city.plotContracts && (!relief || !relief->constructionTerrain ||
      city.plotContracts->revision!=city.revision || city.plotContracts->terrain!=relief->constructionTerrain))
    return fail("plot contracts require their accepted terrain revision");
  if(city.structureContracts && (city.structureContracts->revision!=city.revision ||
      city.structureContracts->plots!=city.plotContracts || !city.wallSegments.empty() ||
      !city.towers.empty() || !city.gatehouses.empty()))
    return fail("structural contracts require their accepted plot snapshot and exclusive boundaries");
  if(city.composition && (city.composition->revision!=city.revision || city.composition->plots!=city.plotContracts || city.composition->structures!=city.structureContracts))
    return fail("composition requires its accepted plot and structure snapshots");
  CityModelPlan out;
  out.revision = city.revision;
  out.construction = cityConstructionCandidate(city,relief);
  const auto emitUniqueBuildingPart = [&](uint64_t id, CityModelBuildingKind kind,
                                          const char *role, size_t first, size_t end = SIZE_MAX) {
    if (end == SIZE_MAX) end = out.mesh.triangles.size();
    if (end < first || end > out.mesh.triangles.size()) return fail("invalid unique building mesh span");
    if (!settings.voxelUniqueMeshSink || first == end) return true;
    CityVoxelUniqueMeshPart part{id,kind,role,first,end-first};
    std::string why;
    if (!settings.voxelUniqueMeshSink(part,out,&why))
      return fail("building "+std::to_string(id)+" unique voxel part: "+why);
    return true;
  };
  // Structures that are not buildings (walls, plazas, trees, fences, bridges)
  // go to the same sink under an id of their own: role and ordinal, hashed
  // apart from building ids.
  const auto civilId=[](const char *role,uint64_t ordinal) {
    uint64_t h=0xcbf29ce484222325ull;
    for(const char *c=role;*c;++c)h=(h^uint64_t(uint8_t(*c)))*0x100000001b3ull;
    h=(h^ordinal)*0x100000001b3ull;
    return h|(1ull<<63);
  };
  const auto emitCivilPart=[&](const char *role,uint64_t ordinal,size_t first,size_t end=SIZE_MAX) {
    return emitUniqueBuildingPart(civilId(role,ordinal),CityModelBuildingKind::Civic,role,first,end);
  };
  VertexIndex index;
  index.reserve(65536); // Grow with actual geometry instead of preallocating four million buckets.
  // The ground the whole city is built on: the relief plan's graded surface
  // where the town stands on a hill, the fabric's own terrace where it does
  // not. `datum` is the single level the pieces that still want one read.
  Ground ground = groundOf(fabric, relief);
  if(!city.waterways.empty())ground.field=&city.waterways.front().bankGround;
  // Contours describe the natural land. Grading is applied afterward and must
  // not be reconstructed from (and flattened back onto) those contour heights.
  const auto *natural=fabric.terrain ? &fabric.terrain : ground.field;
  CityContourTerrain facetTerrain;
  if(relief && relief->constructionTerrain) {
    const auto &construction=*relief->constructionTerrain;
    double x0=INFINITY,z0=INFINITY,x1=-INFINITY,z1=-INFINITY;
    const auto extent=[&](Position p){x0=std::min(x0,p.x);x1=std::max(x1,p.x);z0=std::min(z0,p.z);z1=std::max(z1,p.z);};
    for(const auto &r:city.skeleton.regions)for(auto p:r.boundary)extent(p);
    for(const auto &line:construction.constraints)for(auto p:line)extent(p);
    CityTerrainRouteMesh mesh;
    if(!meshCityTerrainRoutes(construction,x0-1,z0-1,x1+1,z1+1,mesh,error))return false;
    for(auto t:mesh.triangles)facetTerrain.faces.push_back({mesh.vertices[t[0]],mesh.vertices[t[1]],mesh.vertices[t[2]]});
    facetTerrain.height=construction.ground;
    out.elevationLines=construction.constraints;
  } else if(settings.polygonTerrain && natural)facetTerrain=cityContourTerrain(fabric.landforms,*natural,settings.contourIntervalMetres);
  const bool polygonTerrain=!facetTerrain.faces.empty();
  if(!facetTerrain.error.empty())return fail(facetTerrain.error);
  for(const auto &loop:facetTerrain.contours)for(size_t k=0;k<loop.size();++k)out.elevationLines.push_back({loop[k],loop[(k+1)%loop.size()]});
  const bool gradedStreets=relief && !relief->edges.empty() && bool(relief->openGround);
  if(polygonTerrain && !gradedStreets && city.waterways.empty() && !(relief && relief->accessGround))ground.field=&facetTerrain.height;
  // Only non-planar cut/fill banks refine; flat ground and road planes stay sparse.
  const double groundSpacing=gradedStreets ? std::min(2.,settings.groundVertexSpacing) : polygonTerrain ? std::numeric_limits<double>::infinity() : settings.groundVertexSpacing;
  const double datum = ground.datum;
  auto compounds=fabric.compounds;
  for(auto &compound:compounds)if(!compound.fantasyGothic && !compound.wings.empty()) {
    double top=-1e30;
    for(const auto &wing:compound.wings) {
      const auto &ring=wing.brief.shell;
      for(size_t i=0;i<ring.size();++i) {
        const auto a=ring[i],b=ring[(i+1)%ring.size()];
        const unsigned steps=std::max(1u,unsigned(std::ceil(std::hypot(b.x-a.x,b.z-a.z)*2)));
        for(unsigned j=0;j<=steps;++j)top=std::max(top,ground(a.x+(b.x-a.x)*j/steps,a.z+(b.z-a.z)*j/steps));
      }
    }
    if(top>-1e29)for(auto &wing:compound.wings)wing.brief.foundation=std::max(wing.brief.foundation,top+.25);
  }
  // Ground ownership skins sit 35 mm above terrain. Keep the finished floor
  // above that skin; otherwise grass/paving is visible through every flush
  // ground-storey room even though the floor geometry is present.
  constexpr double finishedFloorOffset=.05;
  const double lotFinishedFloorOffset=finishedFloorOffset+std::max(0.,settings.streetCarveMetres);
  std::set<uint64_t> castleWalls;
  std::map<uint64_t,std::pair<size_t,size_t>> wallKinds;
  for (const auto &segment : city.wallSegments) { ++wallKinds[segment.wall].second; wallKinds[segment.wall].first += segment.sharedWithCastle; }
  for (const auto &[wall,count] : wallKinds) if (count.first*2 >= count.second) castleWalls.insert(wall);

  if(!buildCitySurfaces(city,out.surfaces,error)) return fail("city surface ownership: "+(error?*error:std::string{}));

  if(relief!=nullptr && relief->ground) {
    // Every surface follows the ground once the ground is the relief plan's.
    //
    // The pads are already horizontal in that field and the streets already
    // graded down it, so a piece does not carry its own level: it carries the
    // offset it sits at above whatever is under it - the street's carve, the
    // 35 mm ownership skin - exactly as it did over the flat terrace.
    for(auto &piece:out.surfaces.pieces) piece.terrainFollowing=true;

    // A lane whose ground was too steep for a street is paved in stone.
    //
    // It used to be a stair. Without treads a stair is just a slope nobody can
    // climb, so the grade solve now brings every way inside the walking limit
    // and what is left of "this one was steep" is what a hill town actually
    // does with such a lane: it cobbles it, because dirt washes off a slope.
    if(!relief->steepRoads.empty()) {
      std::set<uint64_t> stone;
      for(const auto &route:city.routes)
        for(const uint64_t edge:route.edges)
          if(relief->steepRoads.count(edge)) { stone.insert(route.id); break; }
      uint32_t paved=0;
      for(const auto &piece:out.surfaces.pieces)
        if(piece.kind==CitySurfaceKind::Road && stone.count(piece.owner) && piece.polygon.outer.size()>=3) {
          out.surfaces.influences.push_back({piece.polygon,2,1,.4,true});
          ++paved;
        }
      if(paved) std::fprintf(stderr,"city relief: %u road surfaces paved in stone on the steep lanes\n",paved);
    }

    // A lot is one surface again.
    //
    // It was cut into a flat pad and a half metre retaining band, so that the
    // step from the terrace down to the ground had somewhere to stand. That
    // band was a wall - three metres of drop across half a metre of plan - and
    // it cost more than it was worth: the boolean that cut it out of every lot
    // left slivers, which are the cracks; the ground texture is laid in plan,
    // so on a face that steep it smeared into vertical streaks; and nothing
    // needed the wall in the first place. The region is simply flattened and
    // the cut grades back into the hill around it.
  }
  {
    // Complete the city terrain beneath unallocated verges and civic gaps.
    // Subtract every allocated surface so this never becomes a second skin.
    double minX=1e30,minZ=1e30,maxX=-1e30,maxZ=-1e30;
    for(const auto &region:city.skeleton.regions)
      if(region.kind==CityRegionKind::Block || region.kind==CityRegionKind::Lot)
        for(auto p:region.boundary){minX=std::min(minX,p.x);minZ=std::min(minZ,p.z);maxX=std::max(maxX,p.x);maxZ=std::max(maxZ,p.z);}
    if(minX<maxX && minZ<maxZ) {
      // Tile by tile, never the whole town at once.
      //
      // Subtracting every allocated surface from one rectangle leaves ONE
      // polygon with a hole per lot, and hole bridging inside the triangulator
      // is quadratic in holes: at three and a half thousand lots that single
      // call ran for over ten minutes and looked exactly like a hang. A tile
      // holds a few dozen holes, and the tiles are independent.
      const double tile=64.;
      const size_t before=out.surfaces.pieces.size();
      std::vector<std::pair<geom2::Operand,std::array<double,4>>> allocated;
      allocated.reserve(before);
      for(size_t k=0;k<before;++k) {
        const auto box=geom2::bounds(out.surfaces.pieces[k].polygon);
        allocated.push_back({geom2::toOperand(out.surfaces.pieces[k].polygon),
          {geom2::toMetres(box.minX),geom2::toMetres(box.minY),geom2::toMetres(box.maxX),geom2::toMetres(box.maxY)}});
      }
      for(double x=minX-12;x<maxX+12;x+=tile)
        for(double z=minZ-12;z<maxZ+12;z+=tile) {
          const double x1=std::min(x+tile,maxX+12),z1=std::min(z+tile,maxZ+12);
          if(x1<=x||z1<=z)continue;
          geom2::Polygon extent;
          for(auto p:std::array<Position,4>{{{x,0,z},{x1,0,z},{x1,0,z1},{x,0,z1}}})
            extent.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
          std::vector<geom2::Operand> nearby;
          for(const auto &[operand,box]:allocated)
            if(box[2]>=x && box[0]<=x1 && box[3]>=z && box[1]<=z1)nearby.push_back(operand);
          std::vector<geom2::Polygon> remainder;
          if(nearby.empty())remainder.push_back(extent);
          else if(!geom2::differenceOf(geom2::toOperand(extent),nearby,remainder,error))
            return fail("city terrain remainder at "+std::to_string(x)+","+std::to_string(z)+": "+(error?*error:std::string{}));
          for(auto &p:remainder)
            if(std::abs(geom2::areaMetres(p))>.05)
              out.surfaces.pieces.push_back({CitySurfaceKind::Block,0,0,std::move(p),.035,0,true});
        }
      if(std::getenv("CITY_DEBUG"))
        std::fprintf(stderr,"city ground: %zu allocated surfaces became %zu remainder pieces\n",before,out.surfaces.pieces.size()-before);
    }
  }

  // Remove the exact masonry channel footprint from ground ownership skins.
  // The bed and bank faces below are structural meshes, not heightmap cliffs.
  if(!city.waterways.empty()) {
    std::vector<geom2::Operand> cuts;
    for(const auto &w:city.waterways){geom2::Polygon p;for(auto v:waterwayBox(w,0,std::hypot(w.b.x-w.a.x,w.b.z-w.a.z),-w.halfWidth-w.bankWidth,w.halfWidth+w.bankWidth))p.outer.push_back({geom2::toUnit(v.x),geom2::toUnit(v.z)});cuts.push_back(geom2::toOperand(p));}
    std::vector<CitySurfacePiece> pieces;
    for(const auto &piece:out.surfaces.pieces){std::vector<geom2::Polygon> remainder;if(!geom2::differenceOf(geom2::toOperand(piece.polygon),cuts,remainder,error))return false;for(auto &p:remainder){auto q=piece;q.polygon=std::move(p);pieces.push_back(std::move(q));}}
    out.surfaces.pieces=std::move(pieces);
    for(size_t wi=0;wi<city.waterways.size();++wi) {
      const size_t before=out.mesh.triangles.size();
      waterwayMesh(out,index,city.waterways[wi]);
      if(!emitCivilPart("waterway",wi,before))return false;
    }
  }

  // Urban paving is a slab above the terrain. The streets remain flush;
  // adjacent plots and sidewalks rise together without internal curb cuts.
  // Keep the existing setting name for saved configurations.
  // Architectural ownership must survive terrain tessellation intact.
  const auto architecturalSurfaces=out.surfaces.pieces;
  // Retain semantic lots for building allocation before removing drawable ground.
  if(relief) {
    std::vector<geom2::Operand> excavations;
    for(const auto &deck:relief->platforms) {
      geom2::Polygon polygon;
      for(auto p:deck.boundary)polygon.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
      if(!geom2::counterClockwise(polygon.outer))geom2::reverse(polygon.outer);
      excavations.push_back(geom2::toOperand(polygon));
    }
    if(!excavations.empty()) {
      std::vector<CitySurfacePiece> remaining;
      for(const auto &piece:out.surfaces.pieces) {
        std::vector<geom2::Polygon> cut;
        if(!geom2::differenceOf(geom2::toOperand(piece.polygon),excavations,cut,error))return false;
        for(auto &polygon:cut){auto skin=piece;skin.polygon=std::move(polygon);remaining.push_back(std::move(skin));}
      }
      out.surfaces.pieces=std::move(remaining);
    }
  }

  const bool textureStreets=settings.textureStreets && (settings.streetPavementMetres<=0 || settings.streetCarveMetres<=0) && polygonTerrain && (!relief || relief->platforms.empty());
  if(textureStreets && gradedStreets) {
    // Keep circulation boundaries as CDT constraints even when their material
    // is painted. Deleting them makes triangles span both kerbs and the hill.
    for(auto &p:out.surfaces.pieces) {
      p.kind=CitySurfaceKind::Block;p.height=.035;p.terrainFollowing=true;
    }
  } else if(textureStreets) {
    std::vector<geom2::Operand> cover;
    for(const auto &p:out.surfaces.pieces)cover.push_back(geom2::toOperand(p.polygon));
    std::vector<geom2::Polygon> united;
    if(!geom2::unionOf(cover,united,error))return fail("texture terrain cover: "+(error?*error:std::string{}));
    out.surfaces.pieces.clear();
    for(auto &p:united)out.surfaces.pieces.push_back({CitySurfaceKind::Block,0,0,std::move(p),.035,0,true});
  }
  const double carve=textureStreets ? 0. : std::max(0.,settings.streetCarveMetres);
  const double pavement=std::max(0.,settings.streetPavementMetres);
  // The material mask describes natural ground, not the exact road boundary.
  // Once a road has a mesh and kerbs, paint its entire owned surface.
  for(auto &piece:out.surfaces.pieces)if(piece.kind==CitySurfaceKind::Road) {
    piece.pavingMaterial=13;
    for(const auto &route:city.routes)if(route.id==piece.owner) {
      if(route.groundCover==CityGroundCover::Dirt || route.kind==CityRoadKind::FieldTrack)
        piece.pavingMaterial=12;
      break;
    }
  }
  uint32_t paved=0,whole=0;
  std::vector<uint8_t> carriage(out.surfaces.pieces.size(),0);

  std::set<uint64_t> occupiedLots,occupiedBlocks;
  for(const auto &lot:city.lots)if(lot.developed)occupiedLots.insert(lot.lot);
  for(const auto &r:city.skeleton.regions)if(r.kind==CityRegionKind::Lot && occupiedLots.count(r.id))occupiedBlocks.insert(r.parent);
  const auto raisedPaving=[&](const CitySurfacePiece &p) {
    if(p.kind==CitySurfaceKind::Sidewalk || p.kind==CitySurfaceKind::Court)return true;
    if(p.kind==CitySurfaceKind::Lot)return city.historicalLots.empty() || occupiedLots.count(p.owner)>0;
    if(p.kind==CitySurfaceKind::Block)return city.historicalLots.empty() || occupiedBlocks.count(p.owner)>0;
    return false;
  };
  if(carve>0) {
    const auto source=out.surfaces.pieces;
    std::vector<geom2::Polygon> roadPolygons;
    for(const auto &p:architecturalSurfaces)if(p.kind==CitySurfaceKind::Road)roadPolygons.push_back(p.polygon);
    std::vector<geom2::Polygon> carriageways;
    if(!insetCityRoadTiles(roadPolygons,pavement,carriageways,error))return false;
    std::vector<CitySurfacePiece> pieces;std::vector<uint8_t> lowered;
    for(size_t k=0;k<source.size();++k) {
      const auto &piece=source[k];bool quay=false;
      for(const auto &w:city.waterways)if(piece.owner==w.route)quay=true;
      if(piece.kind!=CitySurfaceKind::Road || quay){pieces.push_back(piece);lowered.push_back(0);continue;}
      std::vector<geom2::Polygon> road,walkway;
      if(!splitCityCarriageway(piece.polygon,carriageways,road,walkway,error))return false;
      for(auto &p:road){auto q=piece;q.polygon=std::move(p);pieces.push_back(std::move(q));lowered.push_back(1);}
      for(auto &p:walkway){auto q=piece;q.kind=CitySurfaceKind::Sidewalk;q.polygon=std::move(p);pieces.push_back(std::move(q));lowered.push_back(0);}
      ++paved;
    }
    out.surfaces.pieces=std::move(pieces);carriage=std::move(lowered);
    for(auto &p:out.surfaces.pieces)
      if(raisedPaving(p))p.height+=carve;
  }

  // Intersect ownership polygons with the coarse land planes before CDT.
  // Roads and lots therefore constrain the same planar cover, and no sampled
  // recursive subdivision can reintroduce heightmap tessellation afterward.
  if(polygonTerrain) {
    std::vector<geom2::Polygon> facets;
    for(const auto &face:facetTerrain.faces) {
      geom2::Polygon p;for(auto v:face)p.outer.push_back({geom2::toUnit(v.x),geom2::toUnit(v.z)});
      facets.push_back(std::move(p));
    }
    std::vector<CitySurfacePiece> pieces;
    for(const auto &piece:out.surfaces.pieces) {
      const auto box=geom2::bounds(piece.polygon);
      std::vector<geom2::Operand> cuts;
      for(const auto &facet:facets) {
        const auto b=geom2::bounds(facet);
        if(b.maxX<box.minX || b.minX>box.maxX || b.maxY<box.minY || b.minY>box.maxY)continue;
        std::vector<geom2::Polygon> clipped;
        auto operand=geom2::toOperand(facet);
        if(!geom2::intersectionOf({geom2::toOperand(piece.polygon),operand},clipped,error))return false;
        cuts.push_back(std::move(operand));
        for(auto &p:clipped){auto q=piece;q.polygon=std::move(p);pieces.push_back(std::move(q));}
      }
      std::vector<geom2::Polygon> remainder;
      if(cuts.empty())remainder.push_back(piece.polygon);
      else if(!geom2::differenceOf(geom2::toOperand(piece.polygon),cuts,remainder,error))return false;
      for(auto &p:remainder){auto q=piece;q.polygon=std::move(p);pieces.push_back(std::move(q));}
    }
    out.surfaces.pieces=std::move(pieces);
  }
  nodeCitySurfaceBoundaries(out.surfaces);
  {
    // Shared-edge noding can leave a touching ring on sampled terrain too,
    // not only at polygon-terrain facet junctions.
    // Regularise the complete cover instead of omitting its thin pieces.
    std::vector<CitySurfacePiece> regular;
    for(const auto &piece:out.surfaces.pieces) {
      if(geom2::polygonFault(piece.polygon).empty()){regular.push_back(piece);continue;}
      std::vector<geom2::Polygon> repaired;
      if(!geom2::unionOf({geom2::toOperand(piece.polygon)},repaired,error))return false;
      for(auto &p:repaired){auto q=piece;q.polygon=std::move(p);regular.push_back(std::move(q));}
    }
    out.surfaces.pieces=std::move(regular);
  }


  // Close only the outside boundary of raised urban paving. Shared lot,
  // block and sidewalk edges cancel, so no internal slab seams are exposed.
  std::map<std::pair<geom2::Point,geom2::Point>,uint32_t> kerbs;
  if(carve>0) {
    const auto count=[&](const geom2::Ring &ring) {
      for(size_t i=0;i<ring.size();++i) {
        const auto a=ring[i],b=ring[(i+1)%ring.size()];
        if(a==b)continue;
        kerbs[a<b ? std::make_pair(a,b) : std::make_pair(b,a)]+=1;
      }
    };
    for(size_t k=0;k<out.surfaces.pieces.size();++k) {
      if(!raisedPaving(out.surfaces.pieces[k]))continue;
      count(out.surfaces.pieces[k].polygon.outer);
      for(const auto &hole:out.surfaces.pieces[k].polygon.holes)count(hole);
    }
  }
  if(std::getenv("CITY_DEBUG"))
    std::fprintf(stderr,"street pavements: %u streets given a walkway either side, %u carved whole\n",paved,whole);

  if(textureStreets) {
    std::vector<geom2::Polygon> protectedLots;
    for(const auto &p:architecturalSurfaces)if(p.kind==CitySurfaceKind::Lot)protectedLots.push_back(p.polygon);
    if(!mergeCityRoadPaint(out.surfaces,protectedLots,1.5,error))return fail("merged street paint: "+(error?*error:std::string{}));
  }

  // Taken once for the whole city: the ground field is asked once per mesh
  // vertex, and there are millions of them.
  const CityGroundWeightIndex weights=buildCityGroundWeightIndex(out.surfaces);
  // Ground is emitted after successful architecture reserves its caps.
  // Failed house plans therefore never leave holes in the terrain.
  std::vector<geom2::Polygon> groundReplacements;
  const auto emitGround=[&]() -> bool {
    if(!groundReplacements.empty()) {
      std::vector<CitySurfacePiece> remaining;
      for(const auto &piece:out.surfaces.pieces) {
        std::vector<geom2::Operand> cuts;const auto box=geom2::bounds(piece.polygon);
        for(const auto &pad:groundReplacements)if(box.overlaps(geom2::bounds(pad)))cuts.push_back(geom2::toOperand(pad));
        if(cuts.empty()){remaining.push_back(piece);continue;}
        std::vector<geom2::Polygon> pieces{piece.polygon};
        // Bound Boolean arrangements just like mesh refinement: a broad
        // terrain remainder can span hundreds of independently built caps.
        for(size_t first=0;first<cuts.size();first+=16) {
          std::vector<geom2::Operand> batch(cuts.begin()+first,cuts.begin()+std::min(cuts.size(),first+16));
          std::vector<geom2::Polygon> next;
          for(const auto &p:pieces){std::vector<geom2::Polygon> rest;
            if(!geom2::differenceOf(geom2::toOperand(p),batch,rest,error))return fail("ground cap ownership: "+(error?*error:std::string{}));
            next.insert(next.end(),std::make_move_iterator(rest.begin()),std::make_move_iterator(rest.end()));}
          pieces=std::move(next);
        }
        for(auto &p:pieces){auto q=piece;q.polygon=std::move(p);remaining.push_back(std::move(q));}
      }
      out.surfaces.pieces=std::move(remaining);
    }
  const auto surfacesStart=std::chrono::steady_clock::now();
  uint32_t droppedBridges=0;
  {
    // Refined by a pool, welded by this thread, in batches.
    //
    // Eleven thousand pieces of ground, each independent until the moment it
    // is welded: the refinement is where the time goes and it parallelises
    // exactly. A batch bounds the memory - the whole city refined at once is
    // ten million triangles held twice - and welding a batch in piece order
    // keeps the mesh byte-identical to the serial build.
    graphrewrite::runtime::WorkerPool pool(std::max(1u,std::min(settings.maximumWorkers,graphrewrite::runtime::WorkerPool::hardwareWorkers())));
    const size_t batch=32;
    std::vector<RefinedSurface> refined(batch);

    for(size_t first=0;first<out.surfaces.pieces.size();first+=batch) {
      const size_t last=std::min(first+batch,out.surfaces.pieces.size());
      const uint32_t count=uint32_t(last-first);
      pool.parallelFor(count,pool.chunkCountFor(count),[&](const uint32_t,const uint32_t begin,const uint32_t end) {
        for(uint32_t k=begin;k<end;++k)
          refined[k]=refineSurface(out.surfaces.pieces[first+k],ground,0,false,groundSpacing,weights);
      });

      for(uint32_t k=0;k<count;++k) {
        droppedBridges+=refined[k].bridges;
        const auto &piece=out.surfaces.pieces[first+k];
        if(!weldSurface(out,index,piece,refined[k],ground,groundSpacing,error))return fail("ground surface: "+(error?*error:std::string{}));

      }

      for(uint32_t k=0;k<count;++k)refined[k]=RefinedSurface{};
    }
  }
  if(carve>0) {
    uint32_t faces=0;
    for(const auto &piece:out.surfaces.pieces) {
      if(!raisedPaving(piece))continue;
      const auto walk=[&](const geom2::Ring &ring) {
        for(size_t i=0;i<ring.size();++i) {
          const auto a=ring[i],b=ring[(i+1)%ring.size()];
          if(a==b)continue;
          const auto found=kerbs.find(a<b ? std::make_pair(a,b) : std::make_pair(b,a));
          if(found==kerbs.end() || found->second!=1)continue;
          const size_t firstKerb=out.mesh.triangles.size();
          kerb(out,index,ground,groundSpacing,piece.height-carve,piece.height,
               {geom2::toMetres(a.x),0,geom2::toMetres(a.y)},{geom2::toMetres(b.x),0,geom2::toMetres(b.y)},50,weights);
          for(size_t t=firstKerb;t<out.mesh.triangles.size();++t) {
            out.roadOwners[t]=piece.owner;out.roadSegments[t]=piece.segment;
          }
          ++faces;
        }
      };
      walk(piece.polygon.outer);
      for(const auto &hole:piece.polygon.holes)walk(hole);
    }
    if(std::getenv("CITY_DEBUG"))
      std::fprintf(stderr,"city streets: %u raised paving edges %.2f m high\n",faces,carve);
  }
  if(std::getenv("CITY_DEBUG"))
    std::fprintf(stderr,"city surfaces: %zu pieces in %.0f s, %u hole bridges dropped\n",out.surfaces.pieces.size(),
      std::chrono::duration<double>(std::chrono::steady_clock::now()-surfacesStart).count(),droppedBridges);
    return true;
  };
  // Raised blocks are independent solids: horizontal deck, vertical masonry
  // sides and a buried footing. Never feed their discontinuity to terrain meshing.
  uint64_t rockOrdinal=0;
  if(relief)for(const auto &rock:relief->rocks) {
    const size_t rockFirst=out.mesh.triangles.size();
    const auto emitRock=[&]{return emitCivilPart("rock",rockOrdinal++,rockFirst);};
    constexpr int sides=9;
    std::array<Position,sides> previous;
    for(int ring=0;ring<4;++ring) {
      std::array<Position,sides> current;
      const double scale=ring==0?1.:ring==1?.94:ring==2?.66:.28;
      for(int k=0;k<sides;++k) {
        const double angle=6.283185307179586*k/sides;
        const double rough=1.+.14*std::sin(k*2.37+double(rock.seed%997));
        current[k]={rock.centre.x+rock.radius*scale*rough*std::cos(angle),
          rock.centre.y+rock.height*(ring/3.)+(ring?rock.height*.045*std::sin(k*1.7+rock.seed):0.),
          rock.centre.z+rock.radius*scale*rough*std::sin(angle)};
      }
      const size_t first=out.mesh.triangles.size();
      if(ring)for(int k=0;k<sides;++k){const int n=(k+1)%sides;
        triangle(out,index,{previous[k],current[k],current[n]},54);
        triangle(out,index,{previous[k],current[n],previous[n]},54);
      }
      if(ring==3)for(int k=1;k+1<sides;++k)triangle(out,index,{current[0],current[k+1],current[k]},54);
      for(size_t t=first;t<out.surfaceMaterials.size();++t)out.surfaceMaterials[t]=27;
      previous=current;
    }
    if(!emitRock())return false;
  }
  const auto blocksStairAccess=[&](Position a,Position b,double low,double high) {
    if(!relief)return false;
    const double length=std::hypot(b.x-a.x,b.z-a.z);
    const unsigned samples=std::max(1u,unsigned(std::ceil(length/.2)));
    for(unsigned k=0;k<=samples;++k) {
      Position p{a.x+(b.x-a.x)*k/samples,0,a.z+(b.z-a.z)*k/samples};
      for(const auto &stair:relief->stairs) {
        const double dx=stair.b.x-stair.a.x,dz=stair.b.z-stair.a.z,len=std::hypot(dx,dz);
        const double along=((p.x-stair.a.x)*dx+(p.z-stair.a.z)*dz)/len;
        const double across=std::abs((p.x-stair.a.x)*dz-(p.z-stair.a.z)*dx)/len;
        if(along<-.1 || along>len+.1 || across>stair.halfWidth+.15)continue;
        double floor=stair.b.y+.04;for(auto band:stair.bands)if(along<=band.first){floor=band.second;break;}
        if(high>floor-.05 && low<floor+2.2)return true;
      }
      for(const auto &landing:relief->stairLandings) {
        if(high<landing.level-.05 || low>landing.level+2.2)continue;
        bool inside=false;double edge=1e30;const auto &ring=landing.boundary;
        for(size_t i=0,j=ring.size()-1;i<ring.size();j=i++) {
          const auto u=ring[i],v=ring[j];edge=std::min(edge,segmentDistance(p,u,v));
          if((u.z>p.z)!=(v.z>p.z) && p.x<(v.x-u.x)*(p.z-u.z)/(v.z-u.z)+u.x)inside=!inside;
        }
        if(inside && edge>.04)return true;
      }
    }
    return false;
  };
  if(relief)for(const auto &deck:relief->platforms) {
    const size_t firstDeck=out.mesh.triangles.size();
    prism(out,index,deck.boundary,deck.foundation,deck.level+.04,54,37);
    if(deck.rockCut)for(size_t t=firstDeck;t<out.surfaceMaterials.size();++t)
      out.surfaceMaterials[t]=out.materials[t]==54?27:19;
    if(deck.rockCut) {
      const auto polygonOf=[](const std::vector<Position> &ring) {
        geom2::Polygon p;for(auto v:ring)p.outer.push_back({geom2::toUnit(v.x),geom2::toUnit(v.z)});
        if(!geom2::counterClockwise(p.outer))geom2::reverse(p.outer);return p;
      };
      // Terrace paths live on the deck, not in the terrain below it. Union
      // junctions before meshing and clip to the shelf so notches remain open.
      std::vector<geom2::Operand> ribbons;
      for(const auto &route:city.routes)for(size_t k=1;k<route.path.size();++k) {
        const auto a=route.path[k-1],b=route.path[k];
        if(std::abs(a.y-deck.level)>.01 || std::abs(b.y-deck.level)>.01)continue;
        const double length=std::hypot(b.x-a.x,b.z-a.z);if(length<.01)continue;
        const double nx=-(b.z-a.z)/length*route.halfWidth,nz=(b.x-a.x)/length*route.halfWidth;
        ribbons.push_back(geom2::toOperand(polygonOf({{a.x+nx,0,a.z+nz},{a.x-nx,0,a.z-nz},
          {b.x-nx,0,b.z-nz},{b.x+nx,0,b.z+nz}})));
      }
      std::vector<geom2::Polygon> united;
      if(!ribbons.empty() && !geom2::unionOf(ribbons,united,error))
        return fail("deck "+std::to_string(deck.block)+" path union: "+(error?*error:std::string{}));
      for(const auto &path:united) {
        std::vector<geom2::Polygon> clipped;
        if(!geom2::intersectionOf({geom2::toOperand(path),geom2::toOperand(polygonOf(deck.boundary))},clipped,error))
          return fail("deck "+std::to_string(deck.block)+" path clip: "+(error?*error:std::string{}));
        for(const auto &piece:clipped) {
          std::vector<std::array<uint32_t,3>> triangles;std::vector<geom2::Point> vertices;
          if(!geom3::triangulate(piece,triangles,vertices,error))return fail("deck paving: "+(error?*error:std::string{}));
          for(auto t:triangles) {
            std::array<Position,3> points;
            for(int k=0;k<3;++k)points[2-k]={geom2::toMetres(vertices[t[k]].x),deck.level+.06,geom2::toMetres(vertices[t[k]].y)};
            const auto first=out.surfaceMaterials.size();triangle(out,index,points,50);
            // Cobbled circulation contrasts with the surrounding courtyard
            // slabs without introducing a second, overlapping ownership skin.
            if(out.surfaceMaterials.size()>first)out.surfaceMaterials.back()=13;
          }
        }
      }
    }
    for(const auto &bridge:deck.bridges) {
      const size_t bridgeFirst=out.mesh.triangles.size();
      const double length=std::hypot(bridge.b.x-bridge.a.x,bridge.b.z-bridge.a.z);
      const double ux=(bridge.b.x-bridge.a.x)/length,uz=(bridge.b.z-bridge.a.z)/length;
      const auto point=[&](double along,double side,double y){return Position{bridge.a.x+ux*along-uz*side,y,bridge.a.z+uz*along+ux*side};};
      const double top=deck.level+.04;
      for(int segment=0;segment<24;++segment) {
        const double t0=segment/24.,t1=(segment+1)/24.;
        const auto underside=[&](double t){return top-bridge.thickness-bridge.archRise+bridge.archRise*std::sqrt(std::max(0.,1.-std::pow(2*t-1,2)));};
        const double y0=underside(t0),y1=underside(t1),w=bridge.halfWidth;
        std::array<Position,8> v{point(t0*length,-w,y0),point(t1*length,-w,y1),point(t1*length,w,y1),point(t0*length,w,y0),
          point(t0*length,-w,top),point(t1*length,-w,top),point(t1*length,w,top),point(t0*length,w,top)};
        const auto face=[&](int a,int b,int c,int d,uint32_t m){triangle(out,index,{v[a],v[b],v[c]},m);triangle(out,index,{v[a],v[c],v[d]},m);};
        face(4,5,6,7,37);face(0,3,2,1,54);face(0,1,5,4,54);face(3,7,6,2,54);
        if(segment==0)face(0,4,7,3,54);if(segment==23)face(1,2,6,5,54);
      }
      for(double side:{-bridge.halfWidth,bridge.halfWidth})
        prism(out,index,{point(0,side-.12,0),point(length,side-.12,0),point(length,side+.12,0),point(0,side+.12,0)},top,top+.9,54,37);
      if(!emitCivilPart("bridge",bridgeFirst,bridgeFirst))return false;
    }

    for(size_t i=0;i<deck.boundary.size();++i) {
      const auto a=deck.boundary[i],b=deck.boundary[(i+1)%deck.boundary.size()];
      const double length=std::hypot(b.x-a.x,b.z-a.z),ux=(b.x-a.x)/length,uz=(b.z-a.z)/length;
      const unsigned bays=std::max(1u,unsigned(std::ceil(length/1.5)));
      for(unsigned k=0;k<bays;++k) {
        const double lo=length*k/bays,hi=length*(k+1)/bays;
        Position start{a.x+ux*lo,0,a.z+uz*lo},end{a.x+ux*hi,0,a.z+uz*hi};
        bool entrance=blocksStairAccess(start,end,deck.level+.04,deck.level+.9);
        for(const auto &stair:relief->stairs)for(auto endpoint:{stair.a,stair.b})
          if(std::abs(endpoint.y-deck.level)<.1 && segmentDistance(endpoint,start,end)<stair.halfWidth+.25)entrance=true;
        for(const auto &other:relief->platforms)for(auto endpoint:{other.accessFrom,other.accessTo})
          if(std::abs(endpoint.y-deck.level)<.1 && segmentDistance(endpoint,start,end)<other.stairHalfWidth+.25)entrance=true;
        for(const auto &owner:relief->platforms)for(const auto &bridge:owner.bridges)for(auto endpoint:{bridge.a,bridge.b})
          if(std::abs(endpoint.y-deck.level)<.1 && segmentDistance(endpoint,start,end)<bridge.halfWidth+.3)entrance=true;
        if(entrance)continue;
        auto ring=std::vector<Position>{{start.x-uz*.12,0,start.z+ux*.12},{end.x-uz*.12,0,end.z+ux*.12},
          {end.x+uz*.12,0,end.z-ux*.12},{start.x+uz*.12,0,start.z-ux*.12}};
        prism(out,index,ring,deck.level+.04,deck.level+.9,54,37);
      }
    }
  }
  if(relief)for(const auto &landing:relief->stairLandings) {
    prism(out,index,landing.boundary,landing.foundation,landing.level,54,37);
    // Guard the landing perimeter, leaving the flight mouths and deck exit clear.
    for(size_t i=0;i<landing.boundary.size();++i) {
      const auto a=landing.boundary[i],b=landing.boundary[(i+1)%landing.boundary.size()];
      const double length=std::hypot(b.x-a.x,b.z-a.z),ux=(b.x-a.x)/length,uz=(b.z-a.z)/length;
      const unsigned bays=std::max(1u,unsigned(std::ceil(length/.5)));
      for(unsigned k=0;k<bays;++k) {
        const double lo=length*k/bays,hi=length*(k+1)/bays;
        Position a0{a.x+ux*lo,0,a.z+uz*lo},b0{a.x+ux*hi,0,a.z+uz*hi};bool opening=blocksStairAccess(a0,b0,landing.level,landing.level+.9);
        for(const auto &stair:relief->stairs)for(auto p:{stair.a,stair.b})
          if(std::abs(p.y+.04-landing.level)<.1 && segmentDistance(p,a0,b0)<stair.halfWidth+.3)opening=true;
        for(const auto &deck:relief->platforms)
          if(std::abs(deck.level+.04-landing.level)<.1 && segmentDistance(deck.accessTo,a0,b0)<deck.stairHalfWidth+.3)opening=true;
        if(opening)continue;
        prism(out,index,{{a0.x-uz*.08,0,a0.z+ux*.08},{b0.x-uz*.08,0,b0.z+ux*.08},
          {b0.x+uz*.08,0,b0.z-ux*.08},{a0.x+uz*.08,0,a0.z-ux*.08}},landing.level,landing.level+.9,54,37);
      }
    }
  }
  // Public stairs are infrastructure, emitted before lots and dwellings.
  if(relief)for(const auto &stair:relief->stairs) {
    const double dx=stair.b.x-stair.a.x,dz=stair.b.z-stair.a.z,len=std::hypot(dx,dz),ux=dx/len,uz=dz/len;
    const auto at=[&](double along,double side){return Position{stair.a.x+ux*along-uz*side,0,stair.a.z+uz*along+ux*side};};
    double previous=0;
    for(const auto &band:stair.bands) {
      prism(out,index,{at(previous,-stair.halfWidth),at(band.first,-stair.halfWidth),at(band.first,stair.halfWidth),at(previous,stair.halfWidth)},stair.supported?stair.foundation:stair.a.y-.28,band.second,36,37);
      previous=band.first;
    }
    // A side of an end landing can be the exit into a court/deck. Only
    // guard exposed drops: a continuous rail here fences off that connection.
    const auto floorAt=[&](double along) {
      for(auto band:stair.bands)if(along<=band.first)return band.second;
      return stair.b.y+.04;
    };
    const auto contains=[](const std::vector<Position> &ring,Position p) {
      bool inside=false;
      for(size_t i=0,j=ring.size()-1;i<ring.size();j=i++) {
        auto a=ring[j],b=ring[i];
        if((a.z>p.z)!=(b.z>p.z) && p.x<(b.x-a.x)*(p.z-a.z)/(b.z-a.z)+a.x)inside=!inside;
      }
      return inside;
    };
    for(double side:{-stair.halfWidth,stair.halfWidth}) {
      const auto connected=[&](double along) {
        const auto p=at(along,side+(side<0?-.16:.16));
        const double floor=floorAt(along);
        for(const auto &deck:relief->platforms)
          if(deck.foundation<=floor && deck.level+.04>=floor-.08 && contains(deck.boundary,p))return true;
        for(const auto &landing:relief->stairLandings)
          if(std::abs(landing.level-floor)<.08 && contains(landing.boundary,p))return true;
        return false;
      };
      // Split at tread boundaries and polygon intersections so openings are
      // exact, independent of the overall flight length or railing post spacing.
      std::vector<double> cuts{0,len};
      for(auto band:stair.bands)cuts.push_back(band.first);
      const auto probeOrigin=at(0,side+(side<0?-.16:.16));
      const auto cutRing=[&](const std::vector<Position> &ring) {
        for(size_t i=0;i<ring.size();++i) {
          auto a=ring[i],b=ring[(i+1)%ring.size()];
          double ex=b.x-a.x,ez=b.z-a.z,det=ux*ez-uz*ex;
          if(std::abs(det)<1e-9)continue;
          double px=a.x-probeOrigin.x,pz=a.z-probeOrigin.z;
          double t=(px*ez-pz*ex)/det,u=(px*uz-pz*ux)/det;
          if(t>0 && t<len && u>=0 && u<=1)cuts.push_back(t);
        }
      };
      for(const auto &deck:relief->platforms)cutRing(deck.boundary);
      for(const auto &landing:relief->stairLandings)cutRing(landing.boundary);
      std::sort(cuts.begin(),cuts.end());
      cuts.erase(std::unique(cuts.begin(),cuts.end(),[](double a,double b){return std::abs(a-b)<1e-7;}),cuts.end());
      std::vector<std::pair<double,double>> exposed;
      for(size_t i=1;i<cuts.size();++i)if(!connected((cuts[i-1]+cuts[i])*.5)) {
        if(!exposed.empty() && std::abs(exposed.back().second-cuts[i-1])<1e-7)exposed.back().second=cuts[i];
        else exposed.push_back({cuts[i-1],cuts[i]});
      }
      for(auto span:exposed) {
        // Keep the thickness of posts outside the neighboring opening too.
        const double lo=span.first+.06,hi=span.second-.06;
        if(hi<=lo)continue;
        const unsigned posts=std::max(1u,unsigned(std::ceil((hi-lo)/3.)));
        for(unsigned post=0;post<=posts;++post) {
          const double along=lo+(hi-lo)*post/posts;
          const double rail=stair.a.y+(stair.b.y-stair.a.y)*along/len+1.15;
          prism(out,index,{at(along-.06,side-.06),at(along+.06,side-.06),at(along+.06,side+.06),at(along-.06,side+.06)},floorAt(along),rail,55,55);
        }
        std::array<Position,8> v;
        for(int end=0;end<2;++end)for(int corner=0;corner<4;++corner){double along=end?hi:lo;auto p=at(along,side+(corner==0 || corner==3?-.055:.055));p.y=stair.a.y+(stair.b.y-stair.a.y)*along/len+1.15+(corner<2?-.055:.055);v[end*4+corner]=p;}
        const auto quad=[&](int a,int b,int c,int d){triangle(out,index,{v[a],v[b],v[c]},55);triangle(out,index,{v[a],v[c],v[d]},55);};
        quad(0,3,2,1);quad(4,5,6,7);quad(0,1,5,4);quad(3,7,6,2);quad(0,4,7,3);quad(1,2,6,5);
      }
    }
  }

  if(settings.groundOnly) {
    if(!emitGround())return false;
    result=std::move(out);
    if(error)error->clear();
    return true;
  }

  const Ground voxelTerrainGround = ground;
  // Buildings and their yards stand on decks, while the terrain already emitted
  // above stays at its original elevation. Keep this field alive until meshing ends.
  std::function<double(double,double)> deckSupport;
  if(relief && !relief->platforms.empty()) {
    const Ground terrain=ground;
    deckSupport=[terrain,relief](double x,double z) {
      for(const auto &deck:relief->platforms) {
        bool inside=false;
        for(size_t i=0,j=deck.boundary.size()-1;i<deck.boundary.size();j=i++) {
          const auto a=deck.boundary[j],b=deck.boundary[i];
          if(segmentDistance({x,0,z},a,b)<1e-6)return deck.level;
          if((a.z>z)!=(b.z>z) && x<(b.x-a.x)*(z-a.z)/(b.z-a.z)+a.x)inside=!inside;
        }
        if(inside)return deck.level;
      }
      return terrain(x,z);
    };
    ground.field=&deckSupport;
  }

  // Curtain faces are triangulated verbatim from the structural phase. This is
  // intentionally boring: future crenellations replace this code without
  // changing wall identity, adjacency or gate contacts.
  for (const auto &segment : city.wallSegments)
  {
    if (!segment.built) continue;
    const size_t before = out.mesh.triangles.size();

    // A curtain wall on a hill climbs it.
    //
    // The first attempt held the walk level over each section and stretched
    // the masonry underneath, which is how a town wall is built across gentle
    // ground and is wrong on a slope: it turns a rampart into a flight of
    // level terraces with a cliff of masonry at every joint. The Great Wall is
    // the model - the rampart follows the ridge, parallel to the ground under
    // it, and where the ridge is too steep to walk the WALK becomes a stair
    // while the wall itself keeps climbing.
    //
    // So each corner is placed against its own position along the section: the
    // walk on the straight ramp between the two ends, the footing on the
    // ground actually under that corner, buried, because the ground between
    // two ends of a chord does not lie on the chord.
    const double footA = ground(segment.a.x, segment.a.z), footB = ground(segment.b.x, segment.b.z);
    const double dx = segment.b.x - segment.a.x, dz = segment.b.z - segment.a.z;
    const double run = std::hypot(dx, dz);
    const double length2 = dx * dx + dz * dz;
    const auto along = [&](const Position &p) {
      return length2 > 1e-12 ? std::clamp(((p.x - segment.a.x) * dx + (p.z - segment.a.z) * dz) / length2, 0., 1.) : 0.;
    };

    // How far a tower or gatehouse standing on this end reaches back along
    // the section.
    const auto clearance = [&](const Position &node) {
      double radius = 0;
      for (const auto &tower : city.towers)
      {
        if (!tower.built) continue;
        if (std::hypot(tower.position.x - node.x, tower.position.z - node.z) <= tower.radius + 1.5)
          radius = std::max(radius, tower.radius);
      }
      for (const auto &gate : city.gatehouses)
      {
        if (!gate.built) continue;
        if (std::hypot(gate.position.x - node.x, gate.position.z - node.z) <= gate.openingWidth + 3.)
          radius = std::max(radius, gate.openingWidth * .5 + 1.6);
      }
      return radius;
    };

    const CityWaterway::Portal *portal=nullptr;
    for(const auto &w:city.waterways)for(const auto &p:w.portals)if(p.wall==segment.id){portal=&p;}
    if(portal) {
      const auto box=[&](double from,double to,double bottom,double top){
        CityWaterway axis;axis.a=segment.a;axis.b=segment.b;
        auto ring=waterwayBox(axis,from,to,-segment.width*.5,segment.width*.5);
        auto lower=ring,upper=ring;
        for(size_t i=0;i<ring.size();++i){const double base=footA+(footB-footA)*along(ring[i]);lower[i].y=base+bottom;upper[i].y=base+top;}
        for(size_t i=0;i<4;++i){const size_t j=(i+1)%4;triangle(out,index,{lower[i],upper[j],lower[j]},31);triangle(out,index,{lower[i],upper[i],upper[j]},31);}
        triangle(out,index,{upper[0],upper[2],upper[1]},32);triangle(out,index,{upper[0],upper[3],upper[2]},32);
        triangle(out,index,{lower[0],lower[1],lower[2]},31);triangle(out,index,{lower[0],lower[2],lower[3]},31);
      };
      box(std::max(0.,clearance(segment.a)-.25),portal->from,-.4,segment.height);
      box(portal->to,run-std::max(0.,clearance(segment.b)-.25),-.4,segment.height);
      box(portal->from,portal->to,2.3,segment.height);
      out.wallTriangles+=uint32_t(out.mesh.triangles.size()-before);
      continue;
    }

    // The section stops at the drum.
    //
    // A section runs node to node and a tower stands ON the node, so the last
    // two or three metres of every section are inside a tower. On flat ground
    // both are solid masonry at the same level and nobody sees it; on a slope
    // the section arrives ramped and its end pushes out through the drum's
    // side, above or below the floor the tower has there. So the section is
    // shortened to meet the drum and the drum closes the joint, which is what
    // a tower is for. A quarter of a metre of overlap is kept so the two
    // always intersect rather than leaving a crack.
    const double headA = std::max(0., clearance(segment.a) - .25);
    const double headB = std::max(0., clearance(segment.b) - .25);
    const double shortened = run - headA - headB;
    const double keep = run > 1e-6 && shortened > 1. ? shortened / run : 1.;
    const double shift = run > 1e-6 && shortened > 1. ? headA / run : 0.;

    for (const auto &face : segment.faces)
    {
      auto c = face.corners;
      for (auto &p : c)
      {
        // Only the along-axis component moves; the cross-section is untouched.
        const double t = along(p);
        const double t2 = shift + t * keep;
        p.x += dx * (t2 - t);
        p.z += dz * (t2 - t);
        const double rampBase = footA + (footB - footA) * t2;
        p.y = p.y <= 1e-6 ? std::min(rampBase, ground(p.x, p.z)) - .4 : rampBase + p.y;
      }
      const uint32_t material = segment.palisade ? 34 : castleWalls.count(segment.wall) ? (face.kind == CityWallFaceKind::Up ? 37 : 36) : (face.kind == CityWallFaceKind::Up ? 32 : 31);
      triangle(out, index, {c[0],c[1],c[2]}, material);
      triangle(out, index, {c[0],c[2],c[3]}, material);
    }

    out.wallTriangles += uint32_t(out.mesh.triangles.size()-before);
    if (!emitCivilPart("wall",segment.id,before)) return false;
  }

  for (const auto &tower : city.towers)
  {
    if (!tower.built) continue;
    const size_t before = out.mesh.triangles.size();
    const double towerBase = ground(tower.position.x, tower.position.z);
    Position p = tower.position; p.y = towerBase;
    generator::FortressJunctionBrief brief;
    brief.centre=p; brief.base=towerBase; brief.height=tower.height;
    brief.towerRadius=tower.radius; brief.towerSides=settings.towerSides;
    brief.towerWallDepth=.35; brief.stairWidth=.8;
    brief.roofHeight=std::min(3.,tower.radius*.9);
    // A storey of the spiral is about three metres, so the turns come often
    // enough for a wall-walk door to meet one within a step.
    brief.floors=std::max(2u,uint32_t(std::lround(tower.height/3.2)));

    // A door onto the wall walk in each direction the wall leaves the tower,
    // at the wall's top. The stair planner puts it on the turn landing
    // nearest that height, so the tower is entered from the walk on every
    // level the walk reaches, not only from the ground.
    for (const auto &segment:city.wallSegments)
    {
      if (!segment.built || segment.wall!=tower.wall) continue;
      for (const auto &[near,far]:{std::make_pair(segment.a,segment.b),std::make_pair(segment.b,segment.a)})
      {
        if (std::hypot(near.x-p.x,near.z-p.z)>tower.radius+1.5) continue;
        const double run=std::hypot(far.x-near.x,far.z-near.z);
        if (run<1e-6 || segment.height<3.) continue;
        brief.elevatedDoors.push_back({(far.x-near.x)/run,towerBase+segment.height,(far.z-near.z)/run});
        // A spiral meets the far door's landing up to half a storey above the
        // walk, and that door's head must clear the lookout: a tower on a
        // wall stands at least that much taller than the walk.
        brief.height=std::max(brief.height,segment.height+brief.height/double(brief.floors)*.5+2.7);
      }
    }

    // Which way the town is, so the door is cut on the inside of the circuit:
    // the centre of the wall this tower belongs to, which is the town it
    // defends. A tower entered from the field is a gift to a besieger.
    {
      Position centre{}; uint32_t counted=0;
      for (const auto &segment:city.wallSegments)
        if (segment.wall==tower.wall) { centre.x+=segment.a.x+segment.b.x; centre.z+=segment.a.z+segment.b.z; counted+=2; }
      if (counted!=0) {
        centre.x/=double(counted); centre.z/=double(counted);
        const double reach=std::hypot(centre.x-p.x,centre.z-p.z);
        if (reach>1e-6) brief.inward={(centre.x-p.x)/reach,0,(centre.z-p.z)/reach};
      }
      // The tower is sized to its door, not the door to the tower.
      //
      // A ten-sided tower of two and a half metres has faces a metre and a
      // half across, and a doorway with jambs either side does not fit one -
      // which is why the towers came out sealed. The radius that carries a
      // real door is arithmetic, so the tower grows the little it needs to.
      brief.doorWidth=1.1;
      {
        const double pi=3.14159265358979323846;
        const double sides=double(std::max(3u,brief.towerSides));
        const double need=brief.doorWidth+2.*.22;
        const double wanted=need/(2.*std::sin(pi/sides))+brief.towerWallDepth/std::cos(pi/sides);

        // ...but never into the next tower along.
        //
        // Growing a tower to fit its door is right; growing it until it meets
        // its neighbour is not, and the audit caught exactly that - two
        // defence towers sharing ground on a closely spaced stretch of wall.
        // Half the gap to the nearest tower, less a hand's breadth, is the
        // most any of them may take.
        double nearest=1e30;
        for (const auto &other:city.towers)
          if (other.built && other.id!=tower.id)
            nearest=std::min(nearest,std::hypot(other.position.x-p.x,other.position.z-p.z));
        const double room=nearest<1e29 ? std::max(brief.towerRadius,nearest*.5-.25) : wanted;
        brief.towerRadius=std::max(brief.towerRadius,std::min(wanted,room));
      }
      if(std::getenv("CITY_DEBUG"))
        std::fprintf(stderr,"tower %llu wall %llu segments %u inward (%.2f,%.2f) radius %.1f height %.1f\n",
          (unsigned long long)tower.id,(unsigned long long)tower.wall,counted,brief.inward.x,brief.inward.z,tower.radius,tower.height);
    }
    generator::FortressJunctionBlueprint blueprint;
    std::string why;
    if (!generator::planFortressJunction(brief,blueprint,&why))
      return fail("tower "+std::to_string(tower.id)+": "+why);
    // Steps on the wall top up to each walk door whose sill the spiral
    // could only put above the walk: a short flight along the wall, as wide
    // as the walk allows, ending at the tower's face.
    for (const auto &opening:blueprint.openings)
    {
      if (opening.bottom<=towerBase+.1) continue;
      const auto a=blueprint.tower[opening.face],b=blueprint.tower[(opening.face+1)%blueprint.tower.size()];
      const double fx=(b.z-a.z),fz=-(b.x-a.x),fl=std::hypot(fx,fz); // Outward normal of the face.
      const Position *walk=nullptr;double facing=-2;
      for (const auto &door:brief.elevatedDoors) {
        const double d=(door.x*fx+door.z*fz)/fl;
        if (d>facing) { facing=d; walk=&door; }
      }
      if (walk==nullptr) continue;
      const double rise=opening.bottom-walk->y;
      if (rise<=.2) continue;
      const unsigned risers=std::max(2u,unsigned(std::ceil(rise/.19)));
      const double going=.28,width=1.2;
      const double ux=walk->x,uz=walk->z,nx=-uz,nz=ux;
      const Position foot{a.x+(b.x-a.x)*opening.along,0,a.z+(b.z-a.z)*opening.along};
      for (unsigned k=0;k<risers;++k) {
        const double near=brief.towerWallDepth*.5+k*going,far=near+going;
        const double top=opening.bottom-k*(rise/double(risers));
        prism(out,index,{{foot.x+ux*near+nx*width*.5,0,foot.z+uz*near+nz*width*.5},{foot.x+ux*far+nx*width*.5,0,foot.z+uz*far+nz*width*.5},
          {foot.x+ux*far-nx*width*.5,0,foot.z+uz*far-nz*width*.5},{foot.x+ux*near-nx*width*.5,0,foot.z+uz*near-nz*width*.5}},walk->y,top,31,32);
      }
    }
    auto structure=generator::buildFortressJunctionStructure(blueprint);
    hierarchy::Solid mesh;
    generator::meshFortressJunction(structure,mesh);
    // The fortress groups both skins of its shell as innerWalls. Both belong
    // in the exterior view too; do not hide the entire tower with house skins.
    mesh.walls.insert(mesh.walls.end(),mesh.innerWalls.begin(),mesh.innerWalls.end());
    mesh.innerWalls.clear();
    // Cylindrical metric UVs share stone-course heights around each tower.
    // Unwrap each face locally across atan2's seam instead of stretching it.
    for(auto &face:mesh.walls) {
      if(face.empty())continue;
      double low=face.front().y,high=low;
      for(const auto &v:face){low=std::min(low,v.y);high=std::max(high,v.y);}
      const bool horizontal=high-low<1e-6;
      const double reference=std::atan2(face.front().z-p.z,face.front().x-p.x);
      for(auto &vertex:face) {
        double angle=std::atan2(vertex.z-p.z,vertex.x-p.x);
        while(angle-reference>3.141592653589793)angle-=6.283185307179586;
        while(angle-reference< -3.141592653589793)angle+=6.283185307179586;
        vertex.u=horizontal ? vertex.x-p.x : angle*brief.towerRadius;
        vertex.v=horizontal ? vertex.z-p.z : vertex.y-towerBase;
        vertex.material=hierarchy::Solid::SurfaceMaterial::TowerMasonry;vertex.authoredUV=true;
      }
    }
    if (!solid(out,index,mesh,0,40,&why)) return fail("tower triangulation: "+why);
    out.towerTriangles += uint32_t(out.mesh.triangles.size()-before);
    CityModelBuilding building{tower.id,tower.wall,CityModelBuildingKind::DefenceTower,blueprint.tower,towerBase,brief.height+brief.belfryHeight,brief.roofHeight,false,true};
    building.pieces=uint32_t(structure.pieces.size()); building.contacts=uint32_t(structure.contacts.size());
    out.structuralPieces+=building.pieces; out.structuralContacts+=building.contacts;
    if (!emitUniqueBuildingPart(tower.id,CityModelBuildingKind::DefenceTower,"tower",before)) return false;
    out.buildings.push_back(std::move(building));
  }

  // A gatehouse is two masonry shoulders with a covered room above the clear
  // passage. Its orientation comes from the wall circuit, never world axes.
  for (const auto &gate : city.gatehouses)
  {
    if (!gate.built) continue;
    Position tangent{1,0,0};
    double best = 1e100;
    for (const auto &s : city.wallSegments) if (s.wall == gate.wall)
    {
      const double d = std::min(std::hypot(s.a.x-gate.position.x,s.a.z-gate.position.z),std::hypot(s.b.x-gate.position.x,s.b.z-gate.position.z));
      if (d < best) { best=d; const double l=std::hypot(s.b.x-s.a.x,s.b.z-s.a.z); if (l>1e-6) tangent={(s.b.x-s.a.x)/l,0,(s.b.z-s.a.z)/l}; }
    }
    const Position normal{-tangent.z,0,tangent.x};
    const double half=gate.openingWidth*.5, shoulder=1.6, depth=gate.depth*.5;
    // The gate stands on the road that goes through it, which on a hill is
    // not the level the rest of the circuit is on.
    const double gateBase = ground(gate.position.x, gate.position.z);
    auto box = [&](double lo,double hi) {
      std::vector<Position> r;
      for (auto uv : std::array<std::pair<double,double>,4>{{{lo,-depth},{hi,-depth},{hi,depth},{lo,depth}}})
        r.push_back({gate.position.x+tangent.x*uv.first+normal.x*uv.second,gateBase,gate.position.z+tangent.z*uv.first+normal.z*uv.second});
      return r;
    };
    const size_t before = out.mesh.triangles.size();
    const bool castle=castleWalls.count(gate.wall);
    prism(out,index,box(-half-shoulder,-half),gateBase,gateBase+gate.height,castle?38:35,castle?37:32);
    prism(out,index,box(half,half+shoulder),gateBase,gateBase+gate.height,castle?38:35,castle?37:32);
    auto upper=box(-half,half);
    prism(out,index,upper,gateBase+3.2,gateBase+gate.height,castle?38:35,castle?37:32);
    roof(out,index,box(-half-shoulder,half+shoulder),gateBase+gate.height,1.8,castle?39:34);
    out.gatehouseTriangles += uint32_t(out.mesh.triangles.size()-before);
    if (!emitUniqueBuildingPart(gate.id,CityModelBuildingKind::Gatehouse,"gatehouse",before)) return false;
    out.buildings.push_back({gate.id,gate.wall,CityModelBuildingKind::Gatehouse,box(-half-shoulder,half+shoulder),gateBase,gate.height,1.8,false,true});
  }

  if(city.structureContracts) {
    const auto &assembly=city.structureContracts->mesh;
    for(auto t:assembly.triangles) {
      if(t[0]>=assembly.vertices.size()||t[1]>=assembly.vertices.size()||t[2]>=assembly.vertices.size())
        return fail("invalid accepted structural mesh");
      triangle(out,index,{assembly.vertices[t[0]],assembly.vertices[t[1]],assembly.vertices[t[2]]},35);
    }
    out.structuralPieces+=uint32_t(city.structureContracts->solids.size());
  }

  if(settings.infrastructureOnly) {
    if(!emitGround())return false;
    result=std::move(out);if(error)error->clear();return true;
  }
  if(city.composition)for(const auto &prop:city.composition->props){
    for(const auto &part:prop.parts){
      const auto first=out.mesh.triangles.size();prism(out,index,city_structure_detail::footprint(part.box),part.box.y0,part.box.y1,17,17);
      for(size_t t=first;t<out.surfaceMaterials.size();++t)out.surfaceMaterials[t]=part.material;
    }
    out.functionalProps.push_back(prop.id);
  }
  uint32_t basedHouses=0,warpedHouses=0,squareInSkewedLot=0,unreachablePlots=0,dampedWarps=0;
  double worstWarpScale=1.;
  std::vector<double> warpScales;
  std::vector<double> platformHeights;
  uint32_t pathsBuilt=0,pathTreads=0,steepWays=0,pathLandings=0;
  double deepestBase=0;
  std::set<uint64_t> developed;
  std::map<uint64_t,const CityLotDevelopment *> allocations;
  for (const auto &lot : city.lots) if (lot.developed) { developed.insert(lot.lot); allocations[lot.lot]=&lot; }
  Position urbanCentre{}; size_t urbanCount=0;
  for (const auto &r : city.skeleton.regions) if (r.kind==CityRegionKind::Lot && developed.count(r.id)) { auto c=mean(r.boundary); urbanCentre.x+=c.x; urbanCentre.z+=c.z; ++urbanCount; }
  if (urbanCount) { urbanCentre.x/=urbanCount; urbanCentre.z/=urbanCount; }
  // Reservations are inputs to the dwelling BLUEPRINT. Never crop or discard
  // intersecting mesh triangles after the building has already been designed.
  struct Occupied { geom2::Polygon polygon; geom2::Box bounds; uint64_t lot=0; };
  std::vector<Occupied> occupied;
  const auto reserve = [&](const std::vector<Position> &ring, double clearance, uint64_t lot=0) {
    geom2::Polygon polygon;
    for (auto p:ring) polygon.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
    if (!geom2::counterClockwise(polygon.outer)) geom2::reverse(polygon.outer);
    std::vector<geom2::Polygon> expanded;
    if (!geom2::offsetPolygon(polygon,clearance,{},expanded,nullptr)) return false;
    for (auto &p:expanded) { const auto box=geom2::bounds(p); occupied.push_back({std::move(p),box,lot}); }
    return true;
  };
  for (const auto &compound:compounds)
    for (const auto &wing:compound.wings)
      if (!reserve(wing.brief.shell,1.)) return fail("invalid compound footprint reservation");
  for (const auto &building:out.buildings)
    if (!reserve(building.footprint,.7)) return fail("invalid fortification footprint reservation");
  using SharedEdge=std::array<int64_t,4>;
  const auto edgeKey=[](Position a,Position b) {
    std::pair<int64_t,int64_t> x{std::llround(a.x*1000),std::llround(a.z*1000)},y{std::llround(b.x*1000),std::llround(b.z*1000)};
    if (y<x) std::swap(x,y);
    return SharedEdge{x.first,x.second,y.first,y.second};
  };
  std::map<SharedEdge,std::vector<uint64_t>> rowEdges;
  for (const auto &r:city.skeleton.regions)
    if (allocations.count(r.id) && allocations.at(r.id)->rowHouse)
      for (size_t i=0;i<r.boundary.size();++i) rowEdges[edgeKey(r.boundary[i],r.boundary[(i+1)%r.boundary.size()])].push_back(r.id);
  std::vector<CityVoxelPartyLine> voxelPartyLines;
  std::vector<CityVoxelPartyFacet> voxelPartyFacets;
  std::map<uint64_t,std::vector<CityVoxelPartyLine>> voxelPartyByBuilding;
  if (settings.voxelBoundarySink) for (const auto &[edge, users]:rowEdges) {
    // Two boundary vertices within a millimetre key to a zero-length edge.
    // It carries no wall area, so it is not a party line.
    if (edge[0]==edge[2] && edge[1]==edge[3]) continue;
    if (users.size()>2) return fail("nonmanifold voxel party line");
    if (users.size()==2 && users[0]!=users[1]) {
      const CityVoxelPartyLine line{edge,users[0],users[1]};
      voxelPartyLines.push_back(line);
      voxelPartyByBuilding[users[0]].push_back(line);
      voxelPartyByBuilding[users[1]].push_back(line);
    }
  }
  uint32_t attempts=0,sharedGridRetries=0,setbackRetries=0,edgeRetries=0;
  double planMs=0,realiseMs=0,skeletonMs=0,meshMs=0,solidMs=0,obstacleMs=0;
  double envelopeMs=0,viewsMs=0,reserveMs=0,bodyMs=0;
  const auto tick=[]{return std::chrono::steady_clock::now();};
  const auto since=[](const std::chrono::steady_clock::time_point &from){
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-from).count();};

  // Covered passages, by the id of the lot that carries one: filled by the
  // gateway pass below, read by the planner as it builds each brief.
  std::map<uint64_t,generator::CoveredPassageRequest> passages;

  // ...and the lattice each was snapped to. A gateway house must be planned
  // on the same grid its passage was measured in, or the strip falls between
  // cell centres and the reservation refuses it as too narrow.
  std::map<uint64_t,geom2::CellFrame> passageGrids;

  // The planning half of a lot, on its own.
  //
  // Everything up to and including the blueprint reads only the skeleton, the
  // allocation and the obstacles it is handed, and writes only its own result -
  // which is what lets a pool run it. Realising, meshing and welding stay with
  // the caller, in lot order, because those touch the shared mesh.
  struct LotPlan
  {
    generator::CellDwellingBrief brief;
    generator::CellDwellingSettings settings;
    generator::CellHouse house;
    geom2::Polygon legalLot;
    std::vector<Position> lotBoundary;
    const CityLotDevelopment *allocation=nullptr;
    std::string why,failure;
    bool planned=false,skip=false,poorHome=false,fatal=false;
    uint32_t attempts=0,sharedGridRetries=0,setbackRetries=0,edgeRetries=0;
    uint32_t blueprints=0,structures=0,meshes=0;
    double planMs=0,obstacleMs=0,realiseMs=0,skeletonMs=0,meshMs=0,envelopeMs=0;
    DwellingPlan plan;
    hierarchy::Skeleton skeleton;
    hierarchy::Solid mesh;
  };

  const auto planLot=[&](const CitySkeletonRegion &region,const std::vector<Occupied> &neighbours)
  {
    LotPlan result;
    if(city.plotContracts) {
      auto contract=std::find_if(city.plotContracts->plots.begin(),city.plotContracts->plots.end(),
        [&](const auto &c){return c.building==region.id;});
      result.allocation=allocations.at(region.id);result.planned=true;
      if(contract==city.plotContracts->plots.end()){result.fatal=true;result.failure="missing accepted plot contract";return result;}
      result.brief=contract->brief;result.settings=contract->settings;result.house=contract->house;
      result.plan=contract->dwelling;result.legalLot=contract->brief.lot;result.lotBoundary=contract->lot;
      auto accessSlabs=std::move(result.plan.siteAccess.slabs);
      result.skeleton=hierarchy::buildSkeleton(result.plan);hierarchy::meshSkeleton(result.skeleton,result.mesh);
      result.plan.siteAccess.slabs=std::move(accessSlabs);
      if(result.mesh.empty()){result.fatal=true;result.failure="contract dwelling emitted no geometry";}
      result.blueprints=result.structures=result.meshes=1;return result;
    }
    // Fit buildings to the legal allocation after street/block/lot clipping,
    // not the original overlapping proposal. Disconnected remnants keep their
    // ground cover; the largest component supplies one contiguous building lot.
    const CitySurfacePiece *allocated=nullptr;double allocatedArea=0;
    for(const auto &surface:architecturalSurfaces)if(surface.kind==CitySurfaceKind::Lot && surface.owner==region.id) {
      const double area=geom2::areaMetres(surface.polygon);if(area>allocatedArea){allocated=&surface;allocatedArea=area;}
    }
    // A gateway lot is its merged boundary, not its ground surface.
    //
    // The surfaces are cut per lot and the lane between them is a road
    // reservation subtracted from both, so looking the lot up by id here
    // returned ONE of the two halves and lost the lane and the other half
    // with it - the footprint then had no buildable ground either side of
    // the passage, and every gateway house was refused. The merged region
    // IS the legal lot; the pass that made it already checked it is one
    // hole-free polygon.
    const bool gateway=passages.count(region.id)!=0;
    if(!allocated && !gateway){result.skip=true;return result;}
    geom2::Polygon &legalLot=result.legalLot;
    if(gateway) {
      for(auto p:region.boundary)legalLot.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
      if(!geom2::counterClockwise(legalLot.outer))geom2::reverse(legalLot.outer);
    } else legalLot=allocated->polygon;
    geom2::Polygon proposedLot;
    for(auto p:region.boundary)proposedLot.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
    if(!geom2::counterClockwise(proposedLot.outer))geom2::reverse(proposedLot.outer);
    // Preserve the original starting edge when clipping did not change the
    // proposal. Party/frontage edge IDs depend on that stable ring ordering.
    const bool unchanged=gateway || (legalLot.holes.empty() && std::abs(geom2::areaMetres(proposedLot)-allocatedArea)<1e-6);
    if(unchanged)legalLot=proposedLot;
    std::vector<Position> &lotBoundary=result.lotBoundary;for(auto p:legalLot.outer)lotBoundary.push_back({geom2::toMetres(p.x),0,geom2::toMetres(p.y)});
    if(unchanged)lotBoundary=region.boundary;

    auto &brief=result.brief;
    brief.id=region.id;
    brief.seed=region.id ^ (city.revision*0x9E3779B97F4A7C15ull);
    const auto &allocation=*allocations.at(region.id);
    result.allocation=&allocation;
    brief.architecturalAgeYears=allocation.architecturalAgeYears;
    brief.architecture=generator::historicalDwellingStyle(allocation.architecturalAgeYears,region.id);
    brief.building=allocation.merchantHouse ? generator::BuildingClass::ShopHouse : generator::BuildingClass::Dwelling;
    if (allocation.residence==CityResidenceKind::Mansion) brief.building=generator::BuildingClass::Mansion;
    if (allocation.residence==CityResidenceKind::Manor) brief.building=generator::BuildingClass::ManorHouse;
    if (allocation.residence==CityResidenceKind::Dormitory) brief.building=generator::BuildingClass::Dormitory;
    if (allocation.residence==CityResidenceKind::Inn || allocation.residence==CityResidenceKind::Pub)
      brief.building=generator::BuildingClass::Inn;
    brief.capacity=generator::CellDwellingBrief::Capacity{allocation.preferredFootprintMetres2,allocation.desiredRooms,allocation.preferredRoomMetres2,true,allocation.rowHouse};
    brief.capacity->minimumSpanCells=allocation.rowHouse ? 2 : 3;
    brief.capacity->maximumAspectRatio=4.;
    if(allocation.rowHouse)brief.tags.push_back("stairwell");
    // The trade is a tag the room programme reads: it puts the forge, the
    // oven room or the byre on the ground floor before any bedroom.
    if(!allocation.trade.empty())brief.tags.push_back(allocation.trade);
    if(allocation.trade=="farmstead")brief.setbackMetres=std::max(brief.setbackMetres,.3);
    brief.capacity->minimumFloors=allocation.minimumFloors;
    brief.capacity->preferredFootprintMetres2=std::max(16.,brief.capacity->preferredFootprintMetres2);
    brief.setbackMetres=allocation.rowHouse ? 0 : .15;
    const bool plannedPavement=std::any_of(city.skeleton.publicSpaces.begin(),city.skeleton.publicSpaces.end(),
        [&](const CityPublicSpace &p){return p.owner==region.parent && p.kind==CityPublicSpace::Kind::Sidewalk;});
    brief.frontageSetbackMetres=allocation.rowHouse && !city.routes.empty() && !plannedPavement ? .95+.25*(region.id%3) : 0;
    if(allocation.trade=="church")brief.building=generator::BuildingClass::Church;
    if(allocation.trade=="temple" || allocation.trade=="roman-temple")brief.building=generator::BuildingClass::Temple;
    if(allocation.trade=="roman-temple") {brief.architecture=DwellingArchitecture::Roman;brief.setbackMetres=3.3;brief.frontageSetbackMetres=5.2;}
    else if(brief.architecture==DwellingArchitecture::Roman && !allocation.rowHouse && allocation.preferredFootprintMetres2>200)brief.frontageSetbackMetres=5.2;
    brief.standing=generator::PlotStanding::Modest;
    brief.lot=legalLot;
    if (const auto passage=passages.find(region.id); passage!=passages.end()) {
      brief.coveredPassage=passage->second;
      // On the lattice the passage was measured in, and hard against the
      // lane: a setback here would hold the houses off the very edge the
      // passage needs them to stand on.
      if (const auto grid=passageGrids.find(region.id); grid!=passageGrids.end()) brief.sharedGrid=grid->second;
      brief.setbackMetres=0;
      brief.frontageSetbackMetres=region.publicFrontage.size()==2?1.4:0.;
    }
    if (!geom2::counterClockwise(brief.lot.outer)) geom2::reverse(brief.lot.outer);
    std::set<uint64_t> rowNeighbours;
    // rowEdges is shared by every lot planned in parallel: look up, never
    // operator[], whose insert on a miss raced the other planners and broke
    // the tree (SIGSEGV in __tree_balance_after_insert, or a lookup that
    // walked a cycle for hours).
    if (allocation.rowHouse)
      for (size_t i=0;i<lotBoundary.size();++i)
        if (const auto row=rowEdges.find(edgeKey(lotBoundary[i],lotBoundary[(i+1)%lotBoundary.size()]));row!=rowEdges.end())
          for (auto neighbour:row->second)
            if (neighbour!=region.id) { brief.party.push_back(uint32_t(i)); rowNeighbours.insert(neighbour); }
    const auto lotBounds=geom2::bounds(brief.lot);
    // legalLot already excludes the road reservation. Do not also classify
    // the public street as an entrance obstacle: that rejected front doors
    // while leaving side/rear doors as the only candidates.
    {
      const auto gathering=tick();
      for (const auto &obstacle:neighbours)
        if (!rowNeighbours.count(obstacle.lot) && obstacle.bounds.maxX>=lotBounds.minX && obstacle.bounds.minX<=lotBounds.maxX &&
            obstacle.bounds.maxY>=lotBounds.minY && obstacle.bounds.minY<=lotBounds.maxY)
          brief.obstacles.push_back(obstacle.polygon);
      result.obstacleMs+=since(gathering);
    }

    // Frontage is a structural input: orient the cell grid to the nearest
    // completed smooth road, rather than to an arbitrary lot edge.
    size_t frontage=0; double frontageDistance=1e100,frontageCost=1e100;
    double lotHigh=-1e30;
    for(auto p:lotBoundary)lotHigh=std::max(lotHigh,ground(p.x,p.z));
    Position streetA{},streetB{};
    for (size_t i=0;i<lotBoundary.size();++i)
    {
      Position m{(lotBoundary[i].x+lotBoundary[(i+1)%lotBoundary.size()].x)*.5,0,(lotBoundary[i].z+lotBoundary[(i+1)%lotBoundary.size()].z)*.5};
      double d=1e100; Position a{},b{};
      for (const auto &route:city.routes) for (size_t k=0;k+1<route.path.size();++k) {
        auto ra=route.path[k],rb=route.path[k+1];
        for(const auto &w:city.waterways)if(w.route==route.id){
          const double wx=w.b.x-w.a.x,wz=w.b.z-w.a.z,wl=wx*wx+wz*wz;
          const double at=((m.x-w.a.x)*wx+(m.z-w.a.z)*wz)/wl;
          if(at<0 || at>1)continue;
          const double dx=rb.x-ra.x,dz=rb.z-ra.z,len=std::hypot(dx,dz);
          if(len>1e-6){const double side=((m.z-ra.z)*dx-(m.x-ra.x)*dz)>=0?1.:-1.;const double offset=side*(w.halfWidth+w.bankWidth+.6);ra.x-=dz/len*offset;ra.z+=dx/len*offset;rb.x-=dz/len*offset;rb.z+=dx/len*offset;}
        }
        const double gap=segmentDistance(m,ra,rb);
        if (gap<d) { d=gap; a=ra; b=rb; }
      }
      // Prefer an uphill frontage when it has a similarly close public way.
      // The former distance-only choice lifted the house above its uphill end,
      // then asked a narrow downhill setback to contain the entire climb.
      const double cost=d+std::max(0.,lotHigh-ground(m.x,m.z))*2.;
      if (cost<frontageCost) { frontageCost=cost;frontageDistance=d; frontage=i; streetA=a; streetB=b; }
    }
    // A planned inward court is the public frontage of its ring of houses.
    // Its edge, rather than the nearest outside road, supplies the door bay
    // and shared building grid. Only an edge actually adjoining the paving
    // qualifies; a house behind another row keeps its street-facing entry.
    bool courtFrontage=false;
    if(region.courtFacing) {
      double best=3.5;
      for(const auto &space:city.skeleton.publicSpaces)if(space.owner==region.parent &&
          space.kind==CityPublicSpace::Kind::Court && space.boundary.size()>=3)
        for(size_t i=0;i<lotBoundary.size();++i) {
          const auto a=lotBoundary[i],b=lotBoundary[(i+1)%lotBoundary.size()];
          const double ex=b.x-a.x,ez=b.z-a.z,edgeLength=std::hypot(ex,ez);
          if(edgeLength<3.)continue;
          const Position mid{(a.x+b.x)*.5,0,(a.z+b.z)*.5};
          for(size_t j=0;j<space.boundary.size();++j) {
            const auto c=space.boundary[j],d=space.boundary[(j+1)%space.boundary.size()];
            const double dx=d.x-c.x,dz=d.z-c.z,len=std::hypot(dx,dz);
            if(len<1. || std::abs((ex*dx+ez*dz)/(edgeLength*len))<.75)continue;
            const double t=std::clamp(((mid.x-c.x)*dx+(mid.z-c.z)*dz)/(len*len),0.,1.);
            const double gap=std::hypot(mid.x-c.x-dx*t,mid.z-c.z-dz*t);
            if(gap<best) {best=gap;frontage=i;streetA=a;streetB=b;courtFrontage=true;frontageDistance=gap;}
          }
        }
    }
    if(region.publicFrontage.size()==2) {
      const auto a=region.publicFrontage[0],b=region.publicFrontage[1];
      double best=1e100;
      for(size_t i=0;i<lotBoundary.size();++i) {
        const auto p=lotBoundary[i],q=lotBoundary[(i+1)%lotBoundary.size()];
        const double cost=segmentDistance(p,a,b)+segmentDistance(q,a,b);
        if(cost<best) {best=cost;frontage=i;}
      }
      if(best<.05) {streetA=a;streetB=b;courtFrontage=true;frontageDistance=0;}
    }
    if(courtFrontage) {
      brief.frontageSetbackMetres=0;
      // The court is the open space. Its edge lots should form a continuous
      // built frontage rather than repeat a detached house's private yard.
      brief.capacity->preferredFootprintMetres2=std::max(
          brief.capacity->preferredFootprintMetres2,region.area*.88);
      brief.capacity->compactnessPenalty=0;
    }
    brief.frontage={uint32_t(frontage)};
    if(!gateway && (!courtFrontage || region.publicFrontage.size()==2) && ground.following() && frontageDistance<12.) {
      const auto a=lotBoundary[frontage],b=lotBoundary[(frontage+1)%lotBoundary.size()];
      const double x=(a.x+b.x)*.5,z=(a.z+b.z)*.5;
      const double climb=std::max(0.,lotHigh-ground(x,z)+finishedFloorOffset);
      if(climb>.35) {
        const double run=std::hypot(b.x-a.x,b.z-a.z);
        double depth=0;
        if(run>.01)for(auto p:lotBoundary)depth=std::max(depth,std::abs((p.x-a.x)*(b.z-a.z)-(p.z-a.z)*(b.x-a.x))/run);
        // Keep useful floor area while reserving a climbable frontage. This
        // reservation reaches the grid planner, rather than discarding a fully
        // built dwelling when its doorstep flight cannot fit afterward.
        const double approach=std::min(6.5,std::max(0.,depth-5.));
        brief.frontageSetbackMetres=std::max(brief.frontageSetbackMetres,std::min(approach,.9+(climb+.5)*.27/.17));
      }
    }

    // The house is square to its STREET, not to the lot edge that happens to
    // face it.
    //
    // A lot cut by a chord has edges at whatever angle the cut left, and the
    // grid took its axis from one of them - so two houses on one frontage
    // stood at different angles and the row read as a heap. The street's own
    // direction, shared by every lot along it, is what a row of houses is
    // square to, and sharing the lattice origin as well puts their party
    // walls on the same lines.
    {
      const double run=std::hypot(streetB.x-streetA.x,streetB.z-streetA.z);
      // ...and only where the lot's own frontage runs along that street.
      //
      // A lot cut by a chord can face the street across a skew edge, and
      // forcing the street's axis on it simply fails - measured, six hundred
      // lots planned twice for nothing. Twenty degrees is the tolerance: past
      // it the lot is not part of that row and keeps its own square.
      bool along=false;
      if (run>.5 && !lotBoundary.empty())
      {
        const auto a=lotBoundary[frontage],b=lotBoundary[(frontage+1)%lotBoundary.size()];
        const double edge=std::hypot(b.x-a.x,b.z-a.z);
        if (edge>.5)
        {
          const double cross=std::abs((b.x-a.x)/edge*(streetB.z-streetA.z)/run-(b.z-a.z)/edge*(streetB.x-streetA.x)/run);
          along=cross<.35;
        }
      }
      if (!gateway && run>.5 && along && frontageDistance<24.)
      {
        geom2::CellFrame lattice;
        lattice.cellMetres=(brief.building==generator::BuildingClass::Church || brief.building==generator::BuildingClass::Temple)?2.8:kCityDwellingModule; // Sacred halls reserve wider structural bays.
        lattice.axisX=(streetB.x-streetA.x)/run;
        lattice.axisZ=(streetB.z-streetA.z)/run;
        lattice.originX=streetA.x;
        lattice.originZ=streetA.z;
        brief.sharedGrid=lattice;
      }
    }
    if(allocation.rowHouse && allocation.maximumFloors>=2 && region.id%3!=0) {
      // Street-facing only; the realised envelope is checked against the legal lot below.
      brief.jettyMetres.assign(allocation.maximumFloors,0.);
      brief.jettyMetres[1]=region.id%2 ? .8 : .95;
      if(allocation.maximumFloors>=3 && region.id%4==0)brief.jettyMetres[2]=.45;
    }

    auto &dwellingSettings=result.settings;
    dwellingSettings.footprintAttempts=24;
    dwellingSettings.capacityCellMetres=(brief.building==generator::BuildingClass::Church || brief.building==generator::BuildingClass::Temple)?2.8:kCityDwellingModule;
    dwellingSettings.doorWidthMetres=1.3;
    dwellingSettings.minimumCorridorMetres=2.4;
    if(gateway && brief.sharedGrid) {
      dwellingSettings.capacityCellMetres=brief.sharedGrid->cellMetres;
      dwellingSettings.minimumCorridorMetres=2.1;
    }
    dwellingSettings.jettyPerFloorMetres=.95;
    dwellingSettings.jettyTotalMetres=1.4;
    dwellingSettings.maximumFloors=allocation.residence==CityResidenceKind::Dormitory ? 0 : allocation.maximumFloors;
    dwellingSettings.eavesOverhangMetres=allocation.rowHouse ? .5 : .45;
    dwellingSettings.preferStraightStairs=allocation.residence==CityResidenceKind::House && region.id%3!=0;
    if(dwellingSettings.preferStraightStairs) {
      dwellingSettings.stairLandingDepthMetres=.8;
      dwellingSettings.minimumTreadMetres=.22;
    }
    const bool hospitality=allocation.residence==CityResidenceKind::Inn || allocation.residence==CityResidenceKind::Pub;
    if(allocation.residence==CityResidenceKind::House && !gateway) {
      auto grid=generator::profileFor(brief.building).grid;
      grid.storeyMetres=2.7;brief.grid=grid;
    }
    dwellingSettings.roofPitch=.85+.16*(region.id%4);
    dwellingSettings.roofRidgeMetres=hospitality?5.8:4.0+.5*(region.id%3);
    if(hospitality || allocation.merchantHouse) {
      dwellingSettings.dormerChance=.8;
      dwellingSettings.maximumDormers=3;
      dwellingSettings.dormerWidthMetres=1.3+.2*(region.id%3);
      dwellingSettings.balconyDepthMetres=1.2;
    }
    const bool poorHome=result.poorHome=(region.districtUse==DistrictUse::Suburb || region.districtUse==DistrictUse::WorkerHousing) &&
      allocation.residence==CityResidenceKind::House && !allocation.merchantHouse;
    if(poorHome) {
      brief.standing=generator::PlotStanding::Slum;
      // A hut is for a plot that can hold nothing else.
      //
      // The test was on the footprint REQUEST, which for a poor home is
      // always modest, so nearly every house in a poor ward was built as a
      // Hut with `fillLot` off - it stood in the middle of its plot at
      // whatever size the class implies and ignored the ground around it.
      // Widening the lots changed nothing, because a hut does not fill its
      // lot by construction. The plot decides now: one too small to hold a
      // house is still a hut, and the rest are houses that use what they
      // have.
      if(!allocation.rowHouse && region.area<42. && allocation.maximumFloors==1) {
        brief.building=generator::BuildingClass::Hut;
        brief.capacity->minimumSpanCells=2;
        brief.capacity->fillLot=false;
      }
      dwellingSettings.maximumWindows=1;
      dwellingSettings.dormerChance=0;
      dwellingSettings.maximumDormers=0;
    }
    else if(region.districtUse==DistrictUse::WealthyResidential || allocation.residence==CityResidenceKind::Manor || allocation.residence==CityResidenceKind::Mansion)
    {
      brief.standing=generator::PlotStanding::Wealthy;
      dwellingSettings.roofPitch=1.35;dwellingSettings.roofRidgeMetres=6.5;
      dwellingSettings.dormerChance=.85;dwellingSettings.maximumDormers=4;
    }

    if(allocation.trade=="guardhouse") {
      brief.standing=generator::PlotStanding::Grand;
      brief.tags.push_back("stairwell");
      dwellingSettings.roofPitch=1.15;
      dwellingSettings.roofRidgeMetres=4.2;
      dwellingSettings.maximumDormers=0;
      dwellingSettings.windowDensity=.5;
    }

    // A gallery over the yard, on the houses that can afford one.
    //
    // The realiser has built balconies all along - posts, decking and a rail,
    // on the yard side where a house of this age puts one - and the city
    // never asked for one, because a depth of nought is no balcony and that
    // is the default. A merchant's house, a manor and a mansion get one.
    if((brief.standing==generator::PlotStanding::Wealthy || allocation.merchantHouse) &&
       allocation.maximumFloors>=2 && region.area>=60)
      dwellingSettings.balconyDepthMetres=1.1+.2*double(region.id%3);
    // Every house stands on something; how much of a stoop it gets is the
    // difference between them.
    //
    // This used to ask for a raised entrance only from a detached house on a
    // lot of ninety square metres or more, which is one city house in ten:
    // the other nine sat on a five centimetre pad, doors flush with the mud,
    // and the plinth and steps the planner already knows how to build were
    // never asked for. A poor house or a house in a row gets a low plinth and
    // one or two steps - the door is off the street, and that is all - and a
    // house with room in front of it gets the full stoop.
    const bool grandEntrance=!courtFrontage && !poorHome && !allocation.rowHouse && region.area>=90;
    dwellingSettings.entranceFlightCells=grandEntrance ? 1+uint32_t(region.id%3) : 1;
    dwellingSettings.entrancePlatformRise=grandEntrance ? .72+.18*(region.id%3) : .28+.08*(region.id%3);
    dwellingSettings.sidewaysEntrance=grandEntrance && (region.id%2)==0;
    dwellingSettings.entranceWidthMetres=1.3;
    if(grandEntrance)brief.setbackMetres=1.4;
    if(courtFrontage)brief.setbackMetres=0;
    auto &house=result.house;
    generator::Ledger ledger;
    auto &why=result.why;
    auto phase=tick();
    ++result.attempts;
    auto &planned=result.planned;
    planned=generator::planCellDwelling(brief,dwellingSettings,house,ledger,&why);
    result.planMs+=since(phase);
    if (!planned && brief.sharedGrid)
    {
      // The shared lattice is a preference too: a lot the street's own axis
      // cannot be fitted into keeps its house, square to itself.
      brief.sharedGrid.reset();
      ++result.attempts;++result.sharedGridRetries;
      planned=generator::planCellDwelling(brief,dwellingSettings,house,ledger,&why);
    }
    if (!planned)
    {
      // The setback is a preference, not a reason to abandon a narrow lot.
      // Street clearance is already reserved outside the allocated polygon.
      brief.setbackMetres=.15;
      dwellingSettings.eavesOverhangMetres=.35;
      ++result.attempts;++result.setbackRetries;
      planned=generator::planCellDwelling(brief,dwellingSettings,house,ledger,&why);
    }
    if (!planned)
    {
      // Land first: retry other legal grid orientations before abandoning a
      // valid lot. Keep every obstacle; only optional programme demand shrinks.
      // Preserve mixed-use semantics and the district height limit on retry.
      if (brief.building!=generator::BuildingClass::ShopHouse && brief.building!=generator::BuildingClass::Inn)
        brief.building=generator::BuildingClass::Dwelling;
      brief.capacity->desiredRooms=1;
      brief.capacity->attachedRow=false;
      // The lot-admission square is not a mandatory building silhouette:
      // existing structural reservations may leave a smaller usable base.
      brief.capacity->minimumSpanCells=2;
      brief.setbackMetres=0;
      // The longest edges first, and only a few of them.
      //
      // Sweeping every edge of every stubborn lot was fifteen hundred
      // planning attempts - a quarter of all the work - to rescue a handful
      // of lots. A building stands on a long edge or it does not stand.
      std::vector<uint32_t> order;
      for (uint32_t edge=0;edge<uint32_t(region.boundary.size());++edge) order.push_back(edge);
      std::sort(order.begin(),order.end(),[&](const uint32_t a,const uint32_t b) {
        const auto &ring=region.boundary;
        const auto ax=ring[(a+1)%ring.size()],ay=ring[a],bx=ring[(b+1)%ring.size()],by=ring[b];
        return std::hypot(ax.x-ay.x,ax.z-ay.z)>std::hypot(bx.x-by.x,bx.z-by.z);
      });
      if (order.size()>3) order.resize(3);

      for (const uint32_t edge:order)
      {
        if (planned) break;
        brief.frontage={edge};
        ++result.attempts;++result.edgeRetries;
        planned=generator::planCellDwelling(brief,dwellingSettings,house,ledger,&why);
      }
    }
    // Shape preference must not turn an otherwise viable household into an
    // empty plot. Retry maximum capacity, keeping the same module pitch and
    // aspect bound, before the final whole-module admission check.
    if(brief.capacity && brief.capacity->maximumAspectRatio>0 && brief.capacity->compactnessPenalty>0) {
      const double cellArea=dwellingSettings.capacityCellMetres*dwellingSettings.capacityCellMetres;
      const uint32_t minimumCells=uint32_t(std::floor(24./cellArea+1e-9));
      if(!planned || house.floors.empty() || house.floors.front().area.count()<minimumCells) {
        brief.capacity->compactnessPenalty=0;
        ++result.attempts;
        planned=generator::planCellDwelling(brief,dwellingSettings,house,ledger,&why);
      }
    }
    // Filling a frontage is optional; the household programme is mandatory.
    // If the larger shell cannot support its rooms/access, retry the original
    // historical footprint rather than losing that household to urban infill.
    const double retryCellArea=dwellingSettings.capacityCellMetres*dwellingSettings.capacityCellMetres;
    const uint32_t retryMinimumCells=uint32_t(std::floor(24./retryCellArea+1e-9));
    if (brief.capacity && (!planned || house.floors.empty() ||
        house.floors.front().area.count()<retryMinimumCells)) {
      const auto binding=std::find_if(city.historicalLots.begin(),city.historicalLots.end(),
          [&](const auto &b){return b.lot==region.id;});
      if(binding!=city.historicalLots.end() && binding->requiredFootprintArea>=4 &&
         binding->requiredFootprintArea<brief.capacity->preferredFootprintMetres2) {
        brief.capacity->preferredFootprintMetres2=binding->requiredFootprintArea;
        ++result.attempts;
        planned=generator::planCellDwelling(brief,dwellingSettings,house,ledger,&why);
      }
    }
    // A technically fitted handful of cells is not a viable town house.
    // Keep its allocated ground available instead of realising a tiny hut
    // after the fallback fitter reduced the room programme.
    if(planned && !house.floors.empty()) {
      geom2::Polygon occupied;
      geom2::polygonOf(house.grid,house.frame,house.floors.front().area,occupied);
      // Admission is measured in complete modules too. A 4 x 4 footprint
      // on the 1.2 m lattice is a useful 23.04 m2 house, not an empty lot
      // simply because an older metre-grid threshold demanded 24 m2.
      const double cellArea=house.frame.cellMetres*house.frame.cellMetres;
      const double minimumFootprint=std::floor(24./cellArea+1e-9)*cellArea;
      if(std::abs(geom2::areaMetres(occupied))+1e-6<minimumFootprint) {
        result.failure="reserved ground: fitted dwelling footprint is below the minimum whole-module area";
        return result;
      }
    }
    ++result.blueprints;
    DwellingPlan &plan=result.plan;
    phase=tick();
    const bool realised=generator::realiseCellDwelling(house,dwellingSettings,plan,&why);
    result.realiseMs+=since(phase);
    if (!realised)
    {
      result.failure="structure: "+why;

      return result;
    }
    // Reserve the entire upper envelope, not just the base. Staying inside
    // disjoint legal lots also prevents opposing and neighbouring jetties
    // from colliding. Retry only the optional projection, never clip meshes.
    const auto legalEnvelope=[&]() {
      for(const auto &storey:plan.storeys) {
        geom2::Polygon p;for(auto v:storey.outer)p.outer.push_back({geom2::toUnit(v.x),geom2::toUnit(v.z)});
        if(!geom2::counterClockwise(p.outer))geom2::reverse(p.outer);
        std::vector<geom2::Polygon> outside;
        if(!geom2::differenceOf(geom2::toOperand(p),{geom2::toOperand(legalLot)},outside,&why))return false;
        for(const auto &q:outside)if(geom2::areaMetres(q)>.0001)return false;
        for(const auto &obstacle:brief.obstacles) {
          std::vector<geom2::Polygon> overlap;
          if(!geom2::intersectionOf({geom2::toOperand(p),geom2::toOperand(obstacle)},overlap,&why))return false;
          for(const auto &q:overlap)if(geom2::areaMetres(q)>.0001)return false;
        }
      }
      return true;
    };
    const auto envelopeStart=tick();
    if(!house.jettyMetres.empty()) {
      for(unsigned retry=0;retry<3 && !legalEnvelope();++retry) {
        for(auto &projection:house.jettyMetres)projection=retry==2 ? 0 : projection*.5;
        if(!generator::realiseCellDwelling(house,dwellingSettings,plan,&why)){result.failure="jetty retry: "+why;result.fatal=true;return result;}
      }
    }
    if(!legalEnvelope() && !house.partyGaps.empty()) {
      // A diagonal party boundary cannot accept the furthest gap along an
      // entire orthogonal wall run. Back off that optional extension, keeping
      // the wall closed and the legal house instead of abandoning the lot.
      for(unsigned retry=0;retry<3 && !legalEnvelope();++retry) {
        for(auto &gap:house.partyGaps)gap=retry==2?0:gap*.5;
        if(!generator::realiseCellDwelling(house,dwellingSettings,plan,&why)) {
          result.failure="party-wall retry: "+why;return result;
        }
      }
    }
    if(!legalEnvelope()){result.failure="envelope";return result;}
    // A projected eave can invalidate a clipped dormer face on an articulated
    // row house. Negotiate the optional jetty before accepting the structure;
    // never drop a roof face or leave a partly emitted building in the mesh.
    const auto validRoof=[&]() {
      for(const auto &roof:plan.roofs) {
        std::vector<geom3::Vertex> face;for(auto p:roof.polygon)face.push_back({p.x,p.y,p.z});
        std::vector<std::array<uint32_t,3>> triangles;
        if(!geom3::triangulateFace(face,triangles,nullptr))return false;
      }
      return true;
    };
    if(!house.jettyMetres.empty() && !validRoof()) {
      for(auto &p:house.jettyMetres)p=0;
      if(!generator::realiseCellDwelling(house,dwellingSettings,plan,&why)){result.failure="flush roof retry: "+why;result.fatal=true;return result;}
    }
    result.envelopeMs+=since(envelopeStart);
    // Pick coherent whole-storey materials and aperture proportions.
    if(!poorHome && !plan.storeys.empty() && region.id%3!=1)
      plan.facade.stoneBaseHeight=plan.storeys.front().top;
    plan.facade.windowWidth=1.1+.1*(region.id%3);
    plan.facade.windowHeight=1.15+.15*((region.id/3)%3);
    plan.facade.roofCrests=!poorHome && (courtFrontage || hospitality || region.id%3==0);
    plan.facade.shutters=false; // Closed boards hid the aperture on most city homes.
    plan.facade.glazed=true;
    if(hospitality){plan.facade.timberFrameMembers=true;plan.facade.timberBayMetres=2.4;}
    if(courtFrontage && !poorHome) {
      // A coherent civic frontage, with tall glazed masonry bays between the
      // party walls. Aperture clearance is still negotiated by FacadeAtlas.
      plan.facade.masonryVariation=true;
      plan.facade.variationSeed=brief.seed;
      if(region.id%3==0) {
        plan.facade.timberFacade=false;plan.facade.boardFacade=false;
        plan.facade.windowHeight=1.65;plan.facade.windowWidth=1.1;
      }
    }

    if(!plan.facade.timberFacade && region.id%5==0)plan.facade.circularWindows=true;
    if(!plan.facade.timberFacade && !plan.facade.boardFacade) {
      plan.facade.masonryVariation=true;plan.facade.variationSeed=brief.seed;
      plan.facade.style=region.id%3==0?FacadeStyle::Romanesque:FacadeStyle::Gothic;
    }
    if(allocation.residence==CityResidenceKind::Manor || allocation.residence==CityResidenceKind::Mansion) {
      plan.facade.roofCrests=true;
      plan.facade.style=FacadeStyle::Gothic;plan.facade.timberFacade=false;plan.facade.boardFacade=false;
      plan.facade.masonryVariation=true;plan.facade.shutters=false;plan.facade.glazed=true;
      plan.facade.windowHeight=1.65;plan.facade.windowWidth=.95;
    }
    generator::applyDwellingStyleFacade(plan);
    // The city terrain-access pass owns the exterior stair. Keep the support
    // and chosen floor rise, but do not mesh a second dwelling-local flight.
    if(plan.entranceLanding.empty() && plan.siteAccess.slabs.size()>1)plan.siteAccess.slabs.resize(1);
    plan.siteAccess.reservedCells.clear();
    ++result.structures;
    plan.id=brief.id; plan.seed=brief.seed;
    phase=tick();
    // City terrain owns the foundation; keep the access metadata but exclude
    // its dwelling-local support faces from this mesh.
    auto siteSlabs=std::move(plan.siteAccess.slabs);
    hierarchy::Skeleton &skeleton=result.skeleton;skeleton=hierarchy::buildSkeleton(plan);
    plan.siteAccess.slabs=std::move(siteSlabs);
    result.skeletonMs+=since(phase);
    if(std::getenv("CITY_OPENINGS")) {
      std::map<int,unsigned> planned,built;
      for(const auto &o:plan.openings)if(o.kind==OpeningKind::Window)++planned[int(o.floor)];
      for(const auto &f:skeleton.structure.faces)if(f.object==0 && f.kind==hierarchy::FaceKind::WindowedWall)++built[int(std::round(f.outline[0].y*10))];
      std::fprintf(stderr,"openings %llu planned",(unsigned long long)region.id);for(auto [level,n]:planned)std::fprintf(stderr," %d:%u",level,n);
      std::fprintf(stderr," built");for(auto [level,n]:built)std::fprintf(stderr," %d:%u",level,n);std::fprintf(stderr,"\n");
    }

    hierarchy::Solid &dwellingMesh=result.mesh;
    phase=tick();
    hierarchy::meshSkeleton(skeleton,dwellingMesh);
    result.meshMs+=since(phase);
    if (dwellingMesh.empty()) { result.failure="mesh: structural skeleton emitted no faces"; return result; }
    ++result.meshes;

    return result;
  };

  // The lots this pass will build, in skeleton order.
  std::vector<const CitySkeletonRegion *> lots;
  for (const auto &region : city.skeleton.regions)
    if (region.kind == CityRegionKind::Lot && developed.count(region.id) && region.boundary.size()>=3 && housingDistrict(region.districtUse))
      lots.push_back(&region);

  // ---- gateway houses over the alleys ---------------------------------------
  //
  // An alley cut through a block leaves a lot on each side of it. Some of
  // those pairs become one house: the two lots and the alley between them
  // are one lot, the alley's strip is a covered passage on the ground floor,
  // and the upper storey runs across it - the archway house of every old
  // town, with the lane going through under the rooms. The planner has known
  // how to reserve a passage all along; nobody asked. Tried first, on its
  // own, so a pair the planner refuses goes back to being two houses.
  std::deque<CitySkeletonRegion> gatewayRegions;
  std::deque<CityLotDevelopment> gatewayAllocations;
  uint32_t gatewaysTried=0,gatewaysBuilt=0;
  {
    std::set<uint64_t> taken;
    const auto segDist=[](const Position &p,const Position &a,const Position &b){return segmentDistance(p,a,b);};

    auto gatewayWays=city.routes;
    // Block approaches are polygons, not road graph edges. Derive their
    // centreline for the existing covered-passage planner without duplicating
    // their public ground or changing street ownership.
    for(const auto &alley:city.skeleton.alleys) {
      if(alley.size()!=4)continue;
      size_t shortEdge=0;double shortest=1e30,longest=0;
      for(size_t i=0;i<4;++i) {
        const double length=std::hypot(alley[(i+1)%4].x-alley[i].x,alley[(i+1)%4].z-alley[i].z);
        if(length<shortest){shortest=length;shortEdge=i;}longest=std::max(longest,length);
      }
      if(shortest<1.2 || shortest>2.6 || longest<4.)continue;
      CitySmoothedRoute way;way.halfWidth=shortest*.5;
      for(size_t i:{shortEdge,(shortEdge+2)%4})
        way.path.push_back({(alley[i].x+alley[(i+1)%4].x)*.5,0,(alley[i].z+alley[(i+1)%4].z)*.5});
      gatewayWays.push_back(std::move(way));
    }
    if(city.plotContracts)gatewayWays.clear();
    for (const auto &route:gatewayWays)
    {
      // An alley: a lane cut through a block, 2.25 m wide, straight or as
      // near as the smoothing left it.
      if (route.path.size()<2 || route.halfWidth>1.3) continue;
      const Position a=route.path.front(),b=route.path.back();
      {
        double along=0;for (size_t k=0;k+1<route.path.size();++k) along+=std::hypot(route.path[k+1].x-route.path[k].x,route.path[k+1].z-route.path[k].z);
        if (along>1e-9 && std::hypot(b.x-a.x,b.z-a.z)<along*.97) continue;
      }
      const double len=std::hypot(b.x-a.x,b.z-a.z); if (len<4) continue;
      const double dx=(b.x-a.x)/len,dz=(b.z-a.z)/len,nx=-dz,nz=dx;

      // The best lot on each side: the one with the longest edge along the alley.
      struct Side { const CitySkeletonRegion *lot=nullptr; double length=0,t0=0,t1=0; };
      Side sides[2];
      for (const CitySkeletonRegion *lot:lots)
      {
        if (taken.count(lot->id) || lot->boundary.size()<4 || lot->boundary.size()>6 || lot->area>140) continue;
        const auto &allocation=*allocations.at(lot->id);
        if (allocation.residence!=CityResidenceKind::House) continue;
        for (size_t i=0;i<lot->boundary.size();++i)
        {
          const Position p=lot->boundary[i],q=lot->boundary[(i+1)%lot->boundary.size()];
          const double el=std::hypot(q.x-p.x,q.z-p.z); if (el<3) continue;
          const double parallel=std::abs(((q.x-p.x)*dx+(q.z-p.z)*dz)/el);
          if (parallel<.985) continue;
          const Position m{(p.x+q.x)*.5,0,(p.z+q.z)*.5};
          if (segDist(m,a,b)>route.halfWidth+.4) continue;
          const int side=((m.x-a.x)*nx+(m.z-a.z)*nz)>0 ? 1 : 0;
          const double t0=std::clamp(std::min((p.x-a.x)*dx+(p.z-a.z)*dz,(q.x-a.x)*dx+(q.z-a.z)*dz),0.,len);
          const double t1=std::clamp(std::max((p.x-a.x)*dx+(p.z-a.z)*dz,(q.x-a.x)*dx+(q.z-a.z)*dz),0.,len);
          if (t1-t0>sides[side].length) sides[side]={lot,t1-t0,t0,t1};
        }
      }
      if (!sides[0].lot || !sides[1].lot || sides[0].lot==sides[1].lot) continue;
      const double t0=std::max(sides[0].t0,sides[1].t0),t1=std::min(sides[0].t1,sides[1].t1);
      if (t1-t0<4) continue;

      // The two lots and the strip between them, as one polygon.
      const auto polygonOf=[](const std::vector<Position> &ring){
        geom2::Polygon p;for (auto v:ring) p.outer.push_back({geom2::toUnit(v.x),geom2::toUnit(v.z)});
        if (!geom2::counterClockwise(p.outer)) geom2::reverse(p.outer);return p;};
      const double w=route.halfWidth+.06;
      const std::vector<Position> strip={{a.x+dx*t0-nx*w,0,a.z+dz*t0-nz*w},{a.x+dx*t1-nx*w,0,a.z+dz*t1-nz*w},
                                         {a.x+dx*t1+nx*w,0,a.z+dz*t1+nz*w},{a.x+dx*t0+nx*w,0,a.z+dz*t0+nz*w}};
      std::vector<geom2::Polygon> united;
      if (!geom2::unionOf({geom2::toOperand(polygonOf(sides[0].lot->boundary)),geom2::toOperand(polygonOf(strip)),
                           geom2::toOperand(polygonOf(sides[1].lot->boundary))},united,nullptr) || united.size()!=1 || !united.front().holes.empty())
        continue;

      // The passage, cell-aligned to the lattice the house will be planned on:
      // the street's, whose axis is across the alley and whose origin is the
      // street's own vertex. Whole cells along the street, the strip's full
      // length down the alley, never narrower than two cells.
      Position streetA{},streetB{};double nearest=1e100;
      for (const auto &other:city.routes) { if (route.id!=0 && other.id==route.id) continue; for (size_t k=0;k+1<other.path.size();++k) {
        const double d0=segDist(a,other.path[k],other.path[k+1]),d1=segDist(b,other.path[k],other.path[k+1]);
        if (std::min(d0,d1)<nearest) { nearest=std::min(d0,d1); streetA=other.path[k]; streetB=other.path[k+1]; } } }
      const double run=std::hypot(streetB.x-streetA.x,streetB.z-streetA.z);
      if (nearest>24 || run<.5) continue;
      const double ax=(streetB.x-streetA.x)/run,az=(streetB.z-streetA.z)/run;
      if (std::abs(ax*dx+az*dz)>.15) continue; // The alley must run across the street, not along it.
      const double sA=(a.x-streetA.x)*ax+(a.z-streetA.z)*az;
      // Three cells wide, so two whole columns of cell centres fall inside it
      // whatever fraction of a cell the planner's frame shifts by.
      const double module=generator::CellDwellingSettings{}.capacityCellMetres; // Compact passage wings retain their authored lattice.
      double c0=(std::floor((sA-w)/module)-.5)*module,c1=(std::ceil((sA+w)/module)+.5)*module;
      if (c1-c0<3*module) c1=c0+3*module;
      const double px=-az,pz=ax; // Down the alley, in the lattice's own sense.
      const double u0=std::min((a.x+dx*t0-streetA.x)*px+(a.z+dz*t0-streetA.z)*pz,(a.x+dx*t1-streetA.x)*px+(a.z+dz*t1-streetA.z)*pz);
      const double u1=std::max((a.x+dx*t0-streetA.x)*px+(a.z+dz*t0-streetA.z)*pz,(a.x+dx*t1-streetA.x)*px+(a.z+dz*t1-streetA.z)*pz);
      const double v0=std::floor(u0/module)*module,v1=std::ceil(u1/module)*module;
      generator::CoveredPassageRequest request;
      request.id=sides[0].lot->id;
      request.clearance=2.4;
      geom2::CellFrame lattice;
      lattice.cellMetres=kCityDwellingModule; // City door bays allow a 1.3 m opening.
      lattice.axisX=ax;
      lattice.axisZ=az;
      lattice.originX=streetA.x;
      lattice.originZ=streetA.z;
      for (auto [c,v]:{std::pair{c0,v0},std::pair{c1,v0},std::pair{c1,v1},std::pair{c0,v1}})
        request.polygon.outer.push_back({geom2::toUnit(streetA.x+ax*c+px*v),geom2::toUnit(streetA.z+az*c+pz*v)});
      if (!geom2::counterClockwise(request.polygon.outer)) geom2::reverse(request.polygon.outer);

      // The merged lot, with an allocation that wants two storeys.
      CitySkeletonRegion merged=*sides[0].lot;
      merged.boundary.clear();
      for (auto v:united.front().outer) merged.boundary.push_back({geom2::toMetres(v.x),0,geom2::toMetres(v.y)});
      merged.area=std::abs(geom2::areaMetres(united.front()));
      CityLotDevelopment allocation=*allocations.at(sides[0].lot->id);
      allocation.rowHouse=false;
      allocation.maximumFloors=std::max(2u,allocation.maximumFloors);
      allocation.minimumFloors=2;
      allocation.preferredFootprintMetres2=std::min(merged.area*.8,180.);
      allocation.desiredRooms=std::max(allocation.desiredRooms,4u);
      gatewayRegions.push_back(std::move(merged));
      gatewayAllocations.push_back(allocation);
      const CitySkeletonRegion &region=gatewayRegions.back();
      const auto previous=allocations[region.id];
      allocations[region.id]=&gatewayAllocations.back();
      passages[region.id]=request;
      passageGrids[region.id]=lattice;
      ++gatewaysTried;

      // Tried on its own; kept only if the planner built the passage.
      LotPlan trial=planLot(region,occupied);
      if (!trial.planned || !trial.failure.empty() || trial.house.coveredPassage.empty())
      {
        if (std::getenv("CITY_DEBUG") && gatewaysTried-gatewaysBuilt<=6)
          std::fprintf(stderr,"gateway refused at lot %llu: %s\n",(unsigned long long)region.id,(trial.why.empty()?trial.failure:trial.why).c_str());
        allocations[region.id]=previous;
        passages.erase(region.id);
        passageGrids.erase(region.id);
        gatewayRegions.pop_back();
        gatewayAllocations.pop_back();
        continue;
      }

      ++gatewaysBuilt;
      if (std::getenv("CITY_DEBUG"))
        std::fprintf(stderr,"gateway built at lot %llu near (%.1f, %.1f)\n",(unsigned long long)region.id,(a.x+b.x)*.5,(a.z+b.z)*.5);
      taken.insert(sides[0].lot->id);taken.insert(sides[1].lot->id);
      for (auto &lot:lots) if (lot==sides[0].lot) lot=&region;
      lots.erase(std::remove(lots.begin(),lots.end(),sides[1].lot),lots.end());
    }
    if (std::getenv("CITY_DEBUG"))
      std::fprintf(stderr,"gateway houses: %u alleys tried, %u built\n",gatewaysTried,gatewaysBuilt);
  }

  // Waves of lots that cannot reach each other.
  //
  // Lots are NOT independent: each accepted house is reserved into `occupied`
  // and the next lot's obstacles come from there. Batching by index and
  // reusing a pre-planned answer where no neighbour reaches was tried and is a
  // loss - at 98 % occupancy with a 0.15 m setback a neighbour's 0.7 m
  // reservation really does cross into nearly every adjacent lot. What stays
  // true is that only ADJACENT lots interact, so colour the lots such that no
  // two in a wave can reach each other: a whole wave then plans and builds at
  // once, and every lot in it sees exactly the houses of the earlier waves,
  // which is a legal serial order for it.
  const double lotReach=1.6; // The reservation buffer, doubled, with room to spare.
  std::vector<std::array<double,4>> lotBoxes(lots.size());
  for (size_t k=0;k<lots.size();++k) {
    double x0=1e30,z0=1e30,x1=-1e30,z1=-1e30;
    for (auto p:lots[k]->boundary){x0=std::min(x0,p.x);z0=std::min(z0,p.z);x1=std::max(x1,p.x);z1=std::max(z1,p.z);}
    lotBoxes[k]={x0-lotReach,z0-lotReach,x1+lotReach,z1+lotReach};
  }

  std::vector<uint32_t> wave(lots.size(),0);
  {
    const double cellSize=32.;
    double gridX=1e30,gridZ=1e30;
    for (const auto &box:lotBoxes){gridX=std::min(gridX,box[0]);gridZ=std::min(gridZ,box[1]);}
    std::map<std::pair<int64_t,int64_t>,std::vector<uint32_t>> buckets;
    for (size_t k=0;k<lots.size();++k) {
      const auto &box=lotBoxes[k];
      const int64_t cx0=int64_t(std::floor((box[0]-gridX)/cellSize)),cx1=int64_t(std::floor((box[2]-gridX)/cellSize));
      const int64_t cz0=int64_t(std::floor((box[1]-gridZ)/cellSize)),cz1=int64_t(std::floor((box[3]-gridZ)/cellSize));
      std::vector<uint32_t> used;
      for (int64_t cx=cx0;cx<=cx1;++cx)for (int64_t cz=cz0;cz<=cz1;++cz) {
        const auto found=buckets.find({cx,cz});
        if (found==buckets.end())continue;
        for (const uint32_t other:found->second) {
          const auto &b=lotBoxes[other];
          if (box[2]<b[0] || box[0]>b[2] || box[3]<b[1] || box[1]>b[3])continue;
          used.push_back(wave[other]);
        }
      }
      std::sort(used.begin(),used.end());
      uint32_t colour=0;
      for (const uint32_t taken:used){ if(taken==colour)++colour; else if(taken>colour)break; }
      wave[k]=colour;
      for (int64_t cx=cx0;cx<=cx1;++cx)for (int64_t cz=cz0;cz<=cz1;++cz)buckets[{cx,cz}].push_back(uint32_t(k));
    }
  }

  // `CITY_SERIAL_LOTS=1` puts every lot in its own wave, which IS the serial
  // order - the control for attributing anything the waves are blamed for.
  if (std::getenv("CITY_SERIAL_LOTS")!=nullptr)
    for (size_t k=0;k<lots.size();++k) wave[k]=uint32_t(k);

  uint32_t waves=0;
  for (const uint32_t colour:wave) waves=std::max(waves,colour+1);
  std::vector<std::vector<uint32_t>> waveMembers(waves);
  for (size_t k=0;k<lots.size();++k) waveMembers[wave[k]].push_back(uint32_t(k));

  // Preserve wave order, but release temporary dwelling meshes after each
  // bounded batch instead of retaining an entire neighbourhood in memory.
  std::vector<std::vector<uint32_t>> boundedWaves;
  const size_t lotBatch=std::max(1u,settings.maximumResidentLots);
  for(const auto &members:waveMembers)
    for(size_t first=0;first<members.size();first+=lotBatch)
      boundedWaves.emplace_back(members.begin()+first,members.begin()+std::min(members.size(),first+lotBatch));
  waveMembers=std::move(boundedWaves);waves=uint32_t(waveMembers.size());

  graphrewrite::runtime::WorkerPool lotPool(std::max(1u,std::min(settings.maximumWorkers,graphrewrite::runtime::WorkerPool::hardwareWorkers())));

  if (std::getenv("CITY_DEBUG"))
    std::fprintf(stderr,"lot waves: %u waves over %zu lots, largest %zu\n",waves,lots.size(),
      waves ? std::max_element(waveMembers.begin(),waveMembers.end(),[](const auto &a,const auto &b){return a.size()<b.size();})->size() : size_t(0));

  for (uint32_t w=0;w<waves;++w)
  {
    const auto &members=waveMembers[w];
    std::vector<LotPlan> wavePlans(members.size());
    const uint32_t count=uint32_t(members.size());
    lotPool.parallelFor(count,lotPool.chunkCountFor(count),[&](const uint32_t,const uint32_t begin,const uint32_t end) {
      for (uint32_t k=begin;k<end;++k) wavePlans[k]=planLot(*lots[members[k]],occupied);
    });

    for (size_t m=0;m<members.size();++m)
    {
      const auto &region=*lots[members[m]];

      const auto bodyStart=tick();
      auto phase=tick();
      LotPlan lot=std::move(wavePlans[m]);
    if(lot.skip)continue;
    attempts+=lot.attempts;sharedGridRetries+=lot.sharedGridRetries;
    setbackRetries+=lot.setbackRetries;edgeRetries+=lot.edgeRetries;
    planMs+=lot.planMs;obstacleMs+=lot.obstacleMs;
    auto &brief=lot.brief;auto &dwellingSettings=lot.settings;auto &house=lot.house;
    auto &legalLot=lot.legalLot;auto &lotBoundary=lot.lotBoundary;
    const auto &allocation=*lot.allocation;const bool poorHome=lot.poorHome;
    bool planned=lot.planned;std::string why=lot.why;
    generator::Ledger ledger;
    (void)allocation;(void)poorHome;(void)legalLot;(void)lotBoundary;
    if (!planned)
    {
      ++out.lotRejected; if (out.rejections.size()<16) out.rejections.push_back("lot "+std::to_string(region.id)+" blueprint: "+why); continue;
    }
    if (!lot.failure.empty())
    {
      if (lot.fatal) return fail("lot "+std::to_string(region.id)+" "+lot.failure);
      ++out.lotRejected;
      if (out.rejections.size()<16) out.rejections.push_back("lot "+std::to_string(region.id)+" "+lot.failure);
      continue;
    }
    ++out.lotBlueprints;++out.lotStructures;++out.lotMeshes;
    auto &plan=lot.plan;
    auto &skeleton=lot.skeleton;
    auto &dwellingMesh=lot.mesh;
    realiseMs+=lot.realiseMs;skeletonMs+=lot.skeletonMs;meshMs+=lot.meshMs;envelopeMs+=lot.envelopeMs;
    const size_t before=out.mesh.triangles.size();
    // The house stands at its lot's own level, asked for rather than sampled.
    //
    // It used to sample the ground field at the boundary's centroid, and the
    // centroid of a concave lot is not inside it: an L-shaped plot, or one a
    // road has bitten into, puts that point out on the verge, where the field
    // is the blend and not the terrace. The floor was then set below the lot's
    // own pad and the pad cut up through it - measured, eleven hundred houses
    // with ground through the floor and the offending surface was the lot's
    // own platform in nine hundred and ninety nine of every thousand. The
    // level is a number the plan already decided; there is no reason to go
    // looking for it in the field.
    // The platform stands ON the hill, so it has to clear it.
    //
    // The level the plan picked is the one the street can reach, and that is
    // what the way to the door is built against. But the hill is no longer
    // cut away under the house, so a platform at that level is buried
    // wherever the ground beside it is higher. It is lifted to clear the
    // highest ground under its own footprint; what that costs is a longer
    // climb from the street, which is the path's business and not the
    // platform's.
    const auto lotCentre=mean(region.boundary);
    double stand=ground(lotCentre.x,lotCentre.z);
    if(relief!=nullptr) {
      auto level=relief->regionLevel.find(region.id);
      if(level!=relief->regionLevel.end()) stand=level->second;
    }

    const CityPlotContract *contract=nullptr;
    if(city.plotContracts)for(const auto &c:city.plotContracts->plots)if(c.building==region.id){contract=&c;break;}
    const auto &sole=plan.storeys.empty()?region.boundary:plan.storeys.front().outer;
    double under=1e30,over=-1e30;
    for(size_t k=0;k<sole.size();++k) {
      const auto &a=sole[k],&b=sole[(k+1)%sole.size()];
      for(const auto q:{a,Position{(a.x+b.x)*.5,0,(a.z+b.z)*.5}}) {
        const double h=ground(q.x,q.z);
        under=std::min(under,h);over=std::max(over,h);
      }
    }
    {
      const auto c=mean(sole);
      const double h=ground(c.x,c.z);
      under=std::min(under,h);over=std::max(over,h);
    }
    if(!(under<1e29)) { under=stand; over=stand; }

    // As low as it can be and still stand on the hill.
    //
    // Taking the street's level when that was higher built ten metre podiums,
    // which is the tall base problem over again. A platform only has to clear
    // the ground under its own footprint; whatever is left between it and the
    // street is a climb, and a climb is what the way to the door is for.
    stand=over+.04;
    platformHeights.push_back(over-under);
    double base=stand+lotFinishedFloorOffset+plan.siteAccess.rise;
    phase=tick();
    const auto withinLot=[&](const double x,const double z){
      bool in=false;
      for(size_t i=0,j=lotBoundary.size()-1;i<lotBoundary.size();j=i++)
        if((lotBoundary[i].z>z)!=(lotBoundary[j].z>z) &&
           x<(lotBoundary[j].x-lotBoundary[i].x)*(z-lotBoundary[i].z)/(lotBoundary[j].z-lotBoundary[i].z+1e-300)+lotBoundary[i].x)
          in=!in;
      return in;
    };

    // A house is planned square and set down on the lot it has.
    //
    // The planner works on a rectilinear cell grid, which is what makes it a
    // planner and not a puzzle: rooms, stairs and openings all assume right
    // angles. But a block cut by bisectors gives lots that are quadrilaterals
    // and not rectangles, so a square house in a skewed lot either left a
    // wedge of ground along one edge or overhung it - which is why houses on
    // the same frontage did not meet. The plan stays square; what changes is
    // where its corners land. The lot's own bounding rectangle is mapped onto
    // the lot's four corners, and the same map is applied to the house - so a
    // house inset from its lot edges stays inset by the same share, and two
    // houses either side of a party line are skewed identically and touch
    // along it.
    // A house is planned square and set down on the lot it has.
    //
    // The planner works on a rectilinear cell grid, which is what makes it a
    // planner and not a puzzle: rooms, stairs and openings all assume right
    // angles and are dimensioned in metres. But a block cut by bisectors
    // gives lots that are quadrilaterals and not rectangles, so a square
    // house in a skewed lot either left a wedge of ground along one edge or
    // overhung it - which is why houses on the same frontage did not meet.
    // The plan stays square; what changes is where its corners land.
    //
    // The rectangle the map starts from is the lot's OWN, found by turning it
    // to each of its edges and keeping the tightest. The axis-aligned box was
    // the first attempt and it is badly wrong for a lot lying at an angle: a
    // plot at forty five degrees has a box twice its size, so the map halved
    // everything inside it. Measured, a tenth of warped houses came out at
    // 0.43 of plan or worse - doors two thirds of a door wide, corridors
    // nobody could pass in. Against the lot's own rectangle a rotated
    // rectangle maps to itself exactly, and a skewed quad only shears.
    Warp warp;
    if(!contract && relief!=nullptr && settings.warpToLot && lotBoundary.size()>=4) {
      // The four corners of the lot: the vertex nearest each corner of its
      // axis-aligned box. A lot clipped by a road has more than four vertices
      // without being any less of a quadrilateral.
      double bx0=lotBoundary[0].x,bx1=bx0,bz0=lotBoundary[0].z,bz1=bz0;
      for(const auto &q:lotBoundary){bx0=std::min(bx0,q.x);bx1=std::max(bx1,q.x);bz0=std::min(bz0,q.z);bz1=std::max(bz1,q.z);}

      std::array<Position,4> corner{};
      std::set<size_t> used;
      const std::array<std::pair<double,double>,4> box{{{bx0,bz0},{bx1,bz0},{bx1,bz1},{bx0,bz1}}};
      for(size_t c=0;c<4;++c) {
        double best=1e30; size_t pick=0; bool found=false;
        for(size_t k=0;k<lotBoundary.size();++k) {
          if(used.count(k))continue;
          const double d=std::hypot(lotBoundary[k].x-box[c].first,lotBoundary[k].z-box[c].second);
          if(d<best){best=d;pick=k;found=true;}
        }
        if(!found)break;
        used.insert(pick); corner[c]=lotBoundary[pick];
      }

      double quadArea=0,ringArea=0;
      for(size_t k=0;k<4;++k){const auto &a=corner[k],&b=corner[(k+1)%4];quadArea+=a.x*b.z-b.x*a.z;}
      for(size_t k=0;k<lotBoundary.size();++k){const auto &a=lotBoundary[k],&b=lotBoundary[(k+1)%lotBoundary.size()];ringArea+=a.x*b.z-b.x*a.z;}
      const double turn=ringArea>0?1.:-1.;
      bool convexLot=true;
      for(size_t k=0;k<lotBoundary.size() && convexLot;++k) {
        const auto &a=lotBoundary[k],&b=lotBoundary[(k+1)%lotBoundary.size()],&c=lotBoundary[(k+2)%lotBoundary.size()];
        const double cross=(b.x-a.x)*(c.z-b.z)-(b.z-a.z)*(c.x-b.x);
        const double scale=std::hypot(b.x-a.x,b.z-a.z)*std::hypot(c.x-b.x,c.z-b.z);
        if(scale>1e-6 && cross*turn<-.08*scale) convexLot=false;
      }
      quadArea=std::abs(quadArea)*.5; ringArea=std::abs(ringArea)*.5;
      const bool closeEnough=quadArea>=ringArea*.82 && quadArea<=ringArea*1.18;
      const bool quadIsTheLot=used.size()==4 && ringArea>1. &&
        ((convexLot && quadArea>=ringArea*.6 && quadArea<=ringArea*1.4) || closeEnough);

      if(quadIsTheLot) {
        // The lot's own rectangle: turned to each edge in turn, tightest kept.
        double ex=1,ez=0,originX=0,originZ=0,spanU=0,spanV=0,smallest=1e30;
        for(size_t k=0;k<4;++k) {
          const double dx=corner[(k+1)%4].x-corner[k].x,dz=corner[(k+1)%4].z-corner[k].z;
          const double len=std::hypot(dx,dz);
          if(len<1e-6)continue;
          const double ax=dx/len,az=dz/len;
          double u0=1e30,u1=-1e30,v0=1e30,v1=-1e30;
          for(const auto &q:corner) {
            const double u=q.x*ax+q.z*az, v=-q.x*az+q.z*ax;
            u0=std::min(u0,u);u1=std::max(u1,u);v0=std::min(v0,v);v1=std::max(v1,v);
          }
          const double area=(u1-u0)*(v1-v0);
          if(area<smallest && area>1e-6) {
            smallest=area; ex=ax; ez=az;
            originX=u0; originZ=v0; spanU=u1-u0; spanV=v1-v0;
          }
        }

        if(spanU>.5 && spanV>.5) {
          // The quad's corners, in that frame, matched to its four corners.
          std::array<Position,4> shaped{};
          {
            const std::array<std::pair<double,double>,4> uv{{{originX,originZ},{originX+spanU,originZ},
                                                            {originX+spanU,originZ+spanV},{originX,originZ+spanV}}};
            std::set<size_t> taken;
            for(size_t c=0;c<4;++c) {
              double best=1e30; size_t pick=0;
              for(size_t k=0;k<4;++k) {
                if(taken.count(k))continue;
                const double u=corner[k].x*ex+corner[k].z*ez, v=-corner[k].x*ez+corner[k].z*ex;
                const double d=std::hypot(u-uv[c].first,v-uv[c].second);
                if(d<best){best=d;pick=k;}
              }
              taken.insert(pick); shaped[c]=corner[pick];
            }
          }

          const double widthScale=std::min(std::hypot(shaped[1].x-shaped[0].x,shaped[1].z-shaped[0].z),
                                           std::hypot(shaped[2].x-shaped[3].x,shaped[2].z-shaped[3].z))/spanU;
          const double depthScale=std::min(std::hypot(shaped[3].x-shaped[0].x,shaped[3].z-shaped[0].z),
                                           std::hypot(shaped[2].x-shaped[1].x,shaped[2].z-shaped[1].z))/spanV;
          const double squeeze=std::min(widthScale,depthScale);

          // Still opened out where the quad is narrower than its rectangle,
          // so nothing the plan dimensioned in metres comes out smaller.
          double open=1.;
          if(squeeze>1e-6 && squeeze<1.) open=std::min(1./squeeze,settings.maximumWarpOpening);
          {
            Position middle{};
            for(const auto &q:shaped){middle.x+=q.x*.25;middle.z+=q.z*.25;}
            for(auto &q:shaped){q.x=middle.x+(q.x-middle.x)*open;q.z=middle.z+(q.z-middle.z)*open;}
          }

          const double kept=squeeze*open;
          worstWarpScale=std::min(worstWarpScale,kept);
          warpScales.push_back(kept);

          if(kept>=settings.minimumWarpScale) {
            warp=[shaped,ex,ez,originX,originZ,spanU,spanV](Position p){
              const double u=std::clamp((p.x*ex+p.z*ez-originX)/spanU,-.35,1.35);
              const double v=std::clamp((-p.x*ez+p.z*ex-originZ)/spanV,-.35,1.35);
              const double x=(1-u)*(1-v)*shaped[0].x+u*(1-v)*shaped[1].x+u*v*shaped[2].x+(1-u)*v*shaped[3].x;
              const double z=(1-u)*(1-v)*shaped[0].z+u*(1-v)*shaped[1].z+u*v*shaped[2].z+(1-u)*v*shaped[3].z;
              return Position{x,p.y,z};
            };
          }
        }
      }

      if(warp) ++warpedHouses; else ++squareInSkewedLot;
    }

    // Resolve the finished lot after the final footprint transform. Sampling
    // only original corners misses both warped uphill corners and interior crests.
    std::vector<Position> supportFootprint;
    for(const auto &p:sole)supportFootprint.push_back(warp?warp(p):p);
    // A rising rear garden must not lift the front door. Only a full-lot
    // podium needs to stand above the entire plot; a house needs its footprint.
    std::vector<Position> supportedLot=settings.raisedLotAprons ? lotBoundary : supportFootprint;
    under=1e30;over=-1e30;
    double sx0=1e30,sz0=1e30,sx1=-1e30,sz1=-1e30;
    const auto sampleSupport=[&](Position p) {
      const double h=ground(p.x,p.z);under=std::min(under,h);over=std::max(over,h);
    };
    for(auto p:supportedLot){sampleSupport(p);sx0=std::min(sx0,p.x);sx1=std::max(sx1,p.x);sz0=std::min(sz0,p.z);sz1=std::max(sz1,p.z);}
    for(double x=sx0;x<=sx1;x+=.5)for(double z=sz0;z<=sz1;z+=.5)
      if(settings.raisedLotAprons ? withinLot(x,z) : pointInside({x,0,z},supportFootprint))sampleSupport({x,0,z});
    stand=over+.04;
    base=stand+lotFinishedFloorOffset+plan.siteAccess.rise;

    if(contract) {
      if(over+.035>contract->floor+1e-6)return fail("accepted plot terrain penetrates floor");
      base=contract->floor;stand=base-lotFinishedFloorOffset;
    }

    // Can anybody get in, and can the way be built?
    //
    // Asked before the house is, and answered by building the way rather than
    // by estimating it. Guessing from the climb alone left three hundred and
    // fifty flights that were emitted and then turned out to want more treads
    // than their run could carry - houses with a stair up to them that nobody
    // could climb, which is worse than an empty plot. The way is laid out
    // here, in full, and only if it comes out climbable does anything get
    // emitted at all.
    struct Step { std::vector<Position> ring; double bottom, top; };
    std::vector<Step> wayToDoor;
    // A raised block owns the descent to the street. Local dwelling stoops
    // already meet its paved deck; a second nearest-road flight would pierce
    // the retaining wall and ignore the reserved public stair court.
    bool terraceAccess=false;
    if(relief)for(const auto &deck:relief->platforms)
      if(deck.block==region.parent){terraceAccess=true;break;}
    if(contract && !terraceAccess)for(const auto &step:contract->steps)wayToDoor.push_back({step.boundary,step.bottom,step.top});
    if(!contract && !terraceAccess && plan.entranceLanding.empty()) {
      // Prefer the block's own public circulation. A street is only a
      // fallback for lots without a planned sidewalk or court.
      const auto nearestWay=[&](const Position &p,Position &at){
        double best=1e30;
        bool blockAccess=false;
        for(const auto &space:city.skeleton.publicSpaces) {
          if(space.owner!=region.parent || (space.kind!=CityPublicSpace::Kind::Sidewalk && space.kind!=CityPublicSpace::Kind::Court))continue;
          blockAccess=true;
          for(size_t k=0;k<space.boundary.size();++k) {
            const auto u=space.boundary[k],w=space.boundary[(k+1)%space.boundary.size()];
            const double dx=w.x-u.x,dz=w.z-u.z,len2=dx*dx+dz*dz;
            if(len2<1e-8)continue;
            const double t=std::clamp(((p.x-u.x)*dx+(p.z-u.z)*dz)/len2,0.,1.);
            const Position q{u.x+dx*t,0,u.z+dz*t};
            const double d=std::hypot(q.x-p.x,q.z-p.z);
            if(d<best){best=d;at=q;}
          }
        }
        if(blockAccess && best<4.)return best;
        best=1e30;
        for(const auto &route:city.routes)
          for(size_t k=0;k+1<route.path.size();++k) {
            const auto &u=route.path[k];const auto &w=route.path[k+1];
            const double dx=w.x-u.x,dz=w.z-u.z,len2=dx*dx+dz*dz;
            const double t=len2>1e-12?std::clamp(((p.x-u.x)*dx+(p.z-u.z)*dz)/len2,0.,1.):0.;
            const double qx=u.x+dx*t,qz=u.z+dz*t;
            const double d=std::hypot(p.x-qx,p.z-qz);
            if(d<best){best=d;const double inset=std::min(d,route.halfWidth+.35);at={qx+(p.x-qx)*inset/std::max(d,1e-9),0,qz+(p.z-qz)*inset/std::max(d,1e-9)};}
          }
        return best;
      };

      // Reserve neighboring plots too, including houses in later batches.
      // Looking only at meshes already emitted makes access depend on order.
      std::vector<const CitySkeletonRegion*> accessNeighbors;
      for(const auto &r:city.skeleton.regions)if(r.kind==CityRegionKind::Lot && r.id!=region.id) {
        double x0=1e30,z0=1e30,x1=-1e30,z1=-1e30;
        for(auto p:r.boundary){x0=std::min(x0,p.x);z0=std::min(z0,p.z);x1=std::max(x1,p.x);z1=std::max(z1,p.z);}
        if(x1>=sx0-6 && x0<=sx1+6 && z1>=sz0-6 && z0<=sz1+6)accessNeighbors.push_back(&r);
      }
      // Every exterior doorway, in the world, with the street it faces.
      struct Way { Position door, toward; double climb, gap; };
      std::vector<Way> doors;
      for(const auto &o:plan.openings) {
        if(!o.exterior || o.kind!=OpeningKind::Door || std::abs(o.a.y+o.base)>.1) continue;
        Position at{(o.a.x+o.b.x)*.5,base+o.a.y+o.base,(o.a.z+o.b.z)*.5};
        if(warp) { const auto q=warp(at); at.x=q.x; at.z=q.z; }
        // A stair can finish on the block's ground or its public sidewalk;
        // it does not have to consume the entire distance to a road.
        Position a=o.a,b=o.b;if(warp){a=warp(a);b=warp(b);}
        const double wallLength=std::hypot(b.x-a.x,b.z-a.z);
        if(wallLength>1e-6) {
          double nx=-(b.z-a.z)/wallLength,nz=(b.x-a.x)/wallLength;
          const auto centre=mean(supportFootprint);
          if(nx*(at.x-centre.x)+nz*(at.z-centre.z)<0){nx=-nx;nz=-nz;}
          for(double distance:{.65,1.,1.4,1.9,2.5,3.2,4.,5.})for(double angle:{0.,-.45,.45,-.8,.8}) {
            const double ux=nx*std::cos(angle)-nz*std::sin(angle),uz=nx*std::sin(angle)+nz*std::cos(angle);
            const Position q{at.x+ux*distance,0,at.z+uz*distance};
            bool owned=withinLot(q.x,q.z);
            for(const auto &r:city.skeleton.regions)if(r.id==region.parent && pointInside(q,r.boundary))owned=true;
            for(const auto &space:city.skeleton.publicSpaces)if(space.owner==region.parent &&
              (space.kind==CityPublicSpace::Kind::Sidewalk || space.kind==CityPublicSpace::Kind::Court) && pointInside(q,space.boundary))owned=true;
            if(!owned)continue;
            const double level=settings.raisedLotAprons?stand+lotFinishedFloorOffset:ground(q.x,q.z)+std::max(0.,settings.streetCarveMetres);
            const double climb=std::abs(base-level);
            // Try a compact straight flight before falling back to a longer
            // route or a switchback; a short target must not force a zigzag.
            if(climb>=.12 && distance<std::ceil(climb/.17)*.27)continue;
            doors.push_back({at,q,climb,distance});
          }
        }
        Position on{};
        const double gap=nearestWay(at,on);
        if(gap>40. || gap<.8) continue;
        doors.push_back({at,on,std::abs(base-(settings.raisedLotAprons ? stand+lotFinishedFloorOffset : ground(on.x,on.z))),gap});
      }

      // The front is the door with the least to climb.
      std::stable_sort(doors.begin(),doors.end(),[](const Way &a,const Way &b){return a.gap+a.climb*2<b.gap+b.climb*2;});

      bool served=doors.empty(); // A house with no exterior door is not this pass's problem.

      for(const auto &candidate:doors) {
        if(candidate.climb>settings.maximumEntranceClimbMetres) continue;

        const Position door=candidate.door;
        Position landing=candidate.toward;
        if(std::hypot(landing.x-door.x,landing.z-door.z)<.3)continue;

        // The landing lies inside the lot apron, whose surface is raised.
        landing.y=settings.raisedLotAprons ? stand+lotFinishedFloorOffset :
          ground(landing.x,landing.z)+std::max(0.,settings.streetCarveMetres);
        const double climb=base-landing.y;
        const double run=std::hypot(door.x-landing.x,door.z-landing.z);
        if(std::abs(climb)<.12) { served=true; break; } // Flush: no works needed.
        if(run<=.3) continue;

        const double ux=(door.x-landing.x)/run,uz=(door.z-landing.z)/run;
        const double half=.8;
        const double need=std::abs(climb)*1.6;

        // A switchback, where a straight flight will not fit: out across the
        // plot, back across it, and on, with a landing at every turn.
        std::vector<Position> way{landing};

        if(need>run+.5) {
          double side=0,band=0;
          for(const double trial:{1.,-1.}) {
            for(const double width:{5.5,4.,2.8,1.8}) {
              const Position probe{landing.x-uz*width*trial,0,landing.z+ux*width*trial};
              if(withinLot(probe.x,probe.z)){side=trial;band=width;break;}
            }
            if(side!=0)break;
          }

          if(side!=0) {
            const uint32_t zigs=uint32_t(std::clamp(std::ceil((need-run)/band),1.,4.));
            for(uint32_t z=0;z<zigs;++z) {
              const double along=run*double(z+1)/double(zigs+1);
              const double across=(z%2==0)?band:0.;
              way.push_back({landing.x+ux*along-uz*across*side,0,landing.z+uz*along+ux*across*side});
            }
          }
        }

        way.push_back({door.x,0,door.z});

        double total=0;
        for(size_t k=1;k<way.size();++k) total+=std::hypot(way[k].x-way[k-1].x,way[k].z-way[k-1].z);
        if(total<.5) continue;

        const double byRise=std::ceil(std::abs(climb)/.17);
        const double byRun=std::floor(total/.27);
        if(byRun<byRise) continue; // Not climbable on this door's line; try the next.

        const uint32_t treads=uint32_t(std::clamp(byRise,1.,512.));
        const double perMetre=climb/total;
        double walked=0;

        for(size_t leg=1;leg<way.size();++leg) {
          const double lx=way[leg].x-way[leg-1].x,lz=way[leg].z-way[leg-1].z;
          const double len=std::hypot(lx,lz);
          if(len<1e-6) continue;
          const double lux=lx/len,luz=lz/len;
          const double nx=-luz*half,nz=lux*half;
          const uint32_t legTreads=std::max(1u,uint32_t(std::lround(double(treads)*len/total)));
          const double footY=landing.y+perMetre*walked;

          for(uint32_t k=0;k<legTreads;++k) {
            const double s0=len*double(k)/double(legTreads),s1=len*double(k+1)/double(legTreads);
            std::vector<Position> tread{
              {way[leg-1].x+lux*s0-nx,0,way[leg-1].z+luz*s0-nz},{way[leg-1].x+lux*s1-nx,0,way[leg-1].z+luz*s1-nz},
              {way[leg-1].x+lux*s1+nx,0,way[leg-1].z+luz*s1+nz},{way[leg-1].x+lux*s0+nx,0,way[leg-1].z+luz*s0+nz}};
            double beneath=1e30;
            for(const auto &q:tread) beneath=std::min(beneath,ground(q.x,q.z));
            const double top=footY+perMetre*len*double(k+1)/double(legTreads);
            const double below=footY+perMetre*len*double(k)/double(legTreads);
            wayToDoor.push_back({std::move(tread),std::min(beneath-.2,std::min(below,top)-.06),top});
          }

          walked+=len;

          if(leg+1<way.size()) {
            const double turnY=landing.y+perMetre*walked;
            const Position c=way[leg];
            std::vector<Position> pad{
              {c.x-lux*half-nx,0,c.z-luz*half-nz},{c.x+lux*half-nx,0,c.z+luz*half-nz},
              {c.x+lux*half+nx,0,c.z+luz*half+nz},{c.x-lux*half+nx,0,c.z-luz*half+nz}};
            double beneath=1e30;
            for(const auto &q:pad) beneath=std::min(beneath,ground(q.x,q.z));
            wayToDoor.push_back({std::move(pad),std::min(beneath-.2,turnY-.25),turnY});
            ++pathLandings;
          }
        }

        // Validate the whole tread width against every road segment: on a
        // bend, a clear centreline alone does not protect the carriageway.
        bool streetClear=true;
        for(const auto &step:wayToDoor)for(size_t e=0;e<step.ring.size();++e)
          for(int sample=0;sample<=4;++sample) {
            const auto a=step.ring[e],b=step.ring[(e+1)%step.ring.size()];
            Position p{a.x+(b.x-a.x)*sample/4.,0,a.z+(b.z-a.z)*sample/4.};
            // A private entrance cannot cross another house or a terrace
            // retaining edge, even when the nearest road is otherwise clear.
            for(const auto &other:out.buildings)
              if(pointInside(p,other.footprint))streetClear=false;
            for(const auto *other:accessNeighbors)if(pointInside(p,other->boundary))streetClear=false;
            if(relief)for(const auto &deck:relief->platforms)
              if(deck.block!=region.parent && pointInside(p,deck.boundary))streetClear=false;
            for(const auto &route:city.routes)for(size_t k=1;k<route.path.size();++k)
              if(segmentDistance(p,route.path[k-1],route.path[k])<std::max(.9,route.halfWidth-settings.streetPavementMetres)-.01)streetClear=false;
          }
        if(!streetClear){wayToDoor.clear();continue;}
        served=true;
        ++pathsBuilt;
        pathTreads+=uint32_t(wayToDoor.size());
        break;
      }

      if(!served) { ++unreachablePlots; continue; }
    }

    if(plan.entranceLanding.size()==4 && plan.entranceFlight.size()==4) {
      const auto worldRing=[&](const std::vector<Position> &ring){auto r=ring;for(auto &p:r){p.y+=base;if(warp)p=warp(p);}return r;};
      auto landing=worldRing(plan.entranceLanding),flight=worldRing(plan.entranceFlight);
      double bottom=base-.2;for(auto p:landing)bottom=std::min(bottom,ground(p.x,p.z)-.2);
      double foot=std::max(ground(flight[2].x,flight[2].z),ground(flight[3].x,flight[3].z));
      foot=std::min(base,foot);bottom=std::min(bottom,foot-.2);
      wayToDoor.clear();wayToDoor.push_back({landing,bottom,base});
      const int count=std::max(1,int(std::ceil((base-foot)/.18)));
      const double run=std::hypot(flight[3].x-flight[0].x,flight[3].z-flight[0].z);
      if(run/count<.24){++unreachablePlots;out.rejections.push_back("planned portico flight too short for terrain rise");continue;}

      const auto lerp=[](Position a,Position b,double t){return Position{a.x+(b.x-a.x)*t,0,a.z+(b.z-a.z)*t};};
      for(int i=0;i<count;++i){const double a=double(i)/count,b=double(i+1)/count;
        std::vector<Position> tread{lerp(flight[0],flight[3],a),lerp(flight[1],flight[2],a),lerp(flight[1],flight[2],b),lerp(flight[0],flight[3],b)};
        double under=bottom;for(auto p:tread)under=std::min(under,ground(p.x,p.z)-.2);
        wayToDoor.push_back({tread,under,base-(base-foot)*b});
      }
    }

    const size_t instanceBefore=out.quadInstances.size();
    const size_t dwellingMeshStart=out.mesh.triangles.size();
    const bool emitted=solid(out,index,dwellingMesh,base,12,&why,&warp,settings.instanceDwellingPanels);
    const size_t dwellingMeshEnd=out.mesh.triangles.size();
    solidMs+=since(phase);
    if (!emitted) return fail("lot "+std::to_string(region.id)+" triangulation: "+why);
    // One material policy for ordinary mesh triangles and reusable panels.
    const auto style=city.composition?std::find_if(city.composition->buildings.begin(),city.composition->buildings.end(),[&](const auto &s){return s.building==region.id;}):decltype(city.composition->buildings.begin()){};
    if(city.composition && style==city.composition->buildings.end())return fail("missing accepted building style");
    const auto materialFor=[&](uint32_t group,uint32_t material) {
      if(material==2)material=region.id%5==0?25:region.id%5==1?20:2;
      if(plan.architecture!=DwellingArchitecture::Vernacular && !city.composition) {
        if(group==14)material=plan.architecture==DwellingArchitecture::Roman?2:20;
        else if(material==1 || material==4 || material==16)
          material=plan.architecture==DwellingArchitecture::Roman?(plan.weathering>.3?1:9):15;
      }
      if(!city.composition && region.districtUse==DistrictUse::Merchant &&
         (material==4 || material==16) && region.id%4)material=28+uint32_t(region.id%4);
      if(city.composition) {
        if(group==14)material=style->roof;
        else if(material==4 || material==16)material=style->wall;
      }
      return material;
    };
    for(size_t t=before;t<out.surfaceMaterials.size();++t)out.surfaceMaterials[t]=materialFor(out.materials[t],out.surfaceMaterials[t]);
    for(size_t i=instanceBefore;i<out.quadInstances.size();++i){auto &q=out.quadInstances[i];q[21]=materialFor(uint32_t(q[20]),uint32_t(q[21]));}
    out.lotTriangles += uint32_t(out.mesh.triangles.size()-before+2*(out.quadInstances.size()-instanceBefore));
    std::vector<Position> footprint=plan.storeys.empty()?region.boundary:plan.storeys.front().outer;
    for (auto &p:footprint) { p.y+=base; if(warp) { const auto q=warp(p); p.x=q.x; p.z=q.z; } }

    // The base the house stands on.
    //
    // A flattened lot is flat where the lot is, and a footprint does not stop
    // exactly at its lot's edge: part of it reaches into the bank outside, or
    // onto a neighbour's ground, or onto a verge the blend left a little low.
    // With no base the floor hangs over that and the ground shows under the
    // wall - measured, half the houses in the town stood over a gap. So the
    // footprint is walled down to the lowest ground under it. This is not the
    // old plinth: it is as deep as this house's own ground needs and no
    // deeper, and where the ground already meets the floor there is none.
    // The dwelling owns a horizontal stone lot, not a sloping paved patch.
    // Emit it only after the entrance and native dwelling have both succeeded.
    const bool sharedSupport=city.structureContracts && city.structureContracts->supportedBuildings.count(region.id);
    if(!sharedSupport && settings.raisedLotAprons && lotBoundary.size()>=3 && stand+lotFinishedFloorOffset-under>.06) {
      geom2::Polygon pad;for(auto p:lotBoundary)pad.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
      std::vector<geom2::Operand> stairCuts;
      for(const auto &step:wayToDoor) {
        geom2::Polygon cut;for(auto p:step.ring)cut.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
        stairCuts.push_back(geom2::toOperand(cut));
      }
      std::vector<geom2::Polygon> pads;
      if(stairCuts.empty())pads.push_back(pad);
      else if(!geom2::differenceOf(geom2::toOperand(pad),stairCuts,pads,&why))return fail("lot entrance reservation: "+why);
      for(const auto &p:pads) {
        groundReplacements.push_back(p);
        std::vector<geom2::Point> vertices;std::vector<std::array<uint32_t,3>> faces;
        if(!geom3::triangulate(p,faces,vertices,&why))return fail("lot platform cap: "+why);
        const double top=stand+lotFinishedFloorOffset-.03;
        for(auto f:faces) {
          std::array<Position,3> points;
          for(size_t k=0;k<3;++k)points[k]={geom2::toMetres(vertices[f[k]].x),top,geom2::toMetres(vertices[f[k]].y)};
          if((points[1].z-points[0].z)*(points[2].x-points[0].x)-(points[1].x-points[0].x)*(points[2].z-points[0].z)<0)std::swap(points[1],points[2]);
          triangle(out,index,points,37);
        }
        const auto sides=[&](const geom2::Ring &ring) {
          for(size_t i=0;i<ring.size();++i) {
            const auto p=ring[i],q=ring[(i+1)%ring.size()];
            Position a{geom2::toMetres(p.x),under-.3,geom2::toMetres(p.y)},b{geom2::toMetres(q.x),under-.3,geom2::toMetres(q.y)},c=b,d=a;
            c.y=d.y=top;triangle(out,index,{a,c,b},54);triangle(out,index,{a,d,c},54);
          }
        };
        sides(p.outer);for(const auto &hole:p.holes)sides(hole);
      }
    }
    for(const auto &step:wayToDoor) {
      prism(out,index,step.ring,step.bottom,step.top,54,37);
      geom2::Polygon replacement;for(auto q:step.ring)replacement.outer.push_back({geom2::toUnit(q.x),geom2::toUnit(q.z)});
      groundReplacements.push_back(std::move(replacement));
    }
    if(!sharedSupport && base-under>.06) {
      // The city owns this support; stop under the finished floor so its
      // cap and wall sides cannot coincide with the dwelling skin.
      prism(out,index,footprint,under-.3,base-.02,54,54);
      ++basedHouses;
      deepestBase=std::max(deepestBase,base-under);
    }
    const double wallHeight=plan.storeys.empty()?0:plan.storeys.back().top;
    double roofHeight=0;
    for (const auto &r:plan.roofs) for (auto p:r.polygon) roofHeight=std::max(roofHeight,p.y-wallHeight);
    const bool central=std::hypot(mean(footprint).x-urbanCentre.x,mean(footprint).z-urbanCentre.z)<100;
    out.structuralPieces+=uint32_t(skeleton.pieces.size()); out.structuralContacts+=uint32_t(skeleton.contacts.size());
    out.structuralRooms+=uint32_t(skeleton.rooms.size()); out.structuralRoomConnections+=uint32_t(skeleton.roomConnections.size());
    CityModelBuilding building{region.id,region.parent,districtKind(region.districtUse),footprint,base,wallHeight,roofHeight,central,false};
    if (allocation.residence==CityResidenceKind::Mansion) building.kind=CityModelBuildingKind::Mansion;
    if (allocation.residence==CityResidenceKind::Manor) building.kind=CityModelBuildingKind::Manor;
    if (allocation.residence==CityResidenceKind::Inn) building.kind=CityModelBuildingKind::Inn;
    if (allocation.residence==CityResidenceKind::Pub) building.kind=CityModelBuildingKind::Pub;
    building.pieces=uint32_t(skeleton.pieces.size()); building.contacts=uint32_t(skeleton.contacts.size()); building.rooms=uint32_t(skeleton.rooms.size()); building.roomConnections=uint32_t(skeleton.roomConnections.size());
    building.windows=uint32_t(std::count_if(plan.openings.begin(),plan.openings.end(),[](const auto &o){return o.exterior && o.kind==OpeningKind::Window;}));
    building.entranceLayout=plan.siteAccess.layout;building.entranceRise=plan.siteAccess.rise;building.entranceCells=plan.siteAccess.reservedCells;
    building.siteAccess=plan.siteAccess;
    if(!plan.entranceLanding.empty())building.siteAccess.slabs.erase(std::remove_if(building.siteAccess.slabs.begin(),building.siteAccess.slabs.end(),[](const auto &s){return s.landing||s.tread;}),building.siteAccess.slabs.end());
    building.jettyMetres=house.jettyMetres;
    building.trade=allocation.trade;
    if(lot.allocation->trade=="roman-temple" || lot.allocation->trade=="church" || lot.allocation->trade=="temple")building.kind=CityModelBuildingKind::Religious;
    building.architecture=lot.allocation->trade=="roman-temple"?"Roman temple":dwellingArchitectureName(lot.plan.architecture);
    building.architecturalAgeYears=lot.plan.architecturalAgeYears;
    building.weathering=lot.plan.weathering;
    building.colonnadeColumns=plan.colonnadeColumns;building.cofferBeams=plan.cofferBeams;
    for(const auto &room:plan.rooms) {
      const char *use="living";
      switch(room.use) {
        case RoomUse::Sleeping: use="sleeping";break; case RoomUse::Shop: use="shop";break; case RoomUse::Store: use="store";break;
        case RoomUse::Common: use="common";break; case RoomUse::Service: use="service";break; case RoomUse::Hall: use="hall";break;
        case RoomUse::Corridor: use="corridor";break; case RoomUse::Entrance: use="entrance";break; case RoomUse::Tower: use="tower";break;
        default: break;
      }
      building.roomList.push_back({room.id,room.floor,use,roomKindName(room.kind),room.boundary,room.usableArea});
      for(auto &p:building.roomList.back().boundary)p.y+=base;
    }
    if(passages.count(region.id))++out.gatewayHouses;
    { const auto viewStart=tick(); building.interiorViews=interiorViews(plan,base); viewsMs+=since(viewStart); }

    // Everything the plan produced moves with the house, not just its walls.
    //
    // The warp was applied to the mesh, the footprint and the doors, and
    // these two were left in the frame the plan was made in: the entrance
    // slabs that the clutter and the fencing read to know where the steps
    // are, and the interior viewpoints the viewer stands a camera on. A
    // house that has been set onto its lot with its landings still back on
    // the drawing board is a house whose props sit beside the step instead
    // of on it, and whose interior views are in the wall.
    if(warp) {
      for(auto &slab:building.siteAccess.slabs)
        for(auto &q:slab.polygon){const auto w=warp(q);q.x=w.x;q.z=w.z;}
      for(auto &q:building.interiorViews){const auto w=warp(q);q.x=w.x;q.z=w.z;}
      for(auto &room:building.roomList)for(auto &q:room.boundary){const auto w=warp(q);q.x=w.x;q.z=w.z;}
    }
    // These are already in world coordinates; reserve the same steps we mesh.
    for(const auto &step:wayToDoor)building.siteAccess.slabs.push_back({step.ring,step.bottom,step.top,false,true});
    for(const auto &o:plan.openings)if(o.exterior && o.kind==OpeningKind::Door)
    {
      Position at{(o.a.x+o.b.x)*.5,base+o.a.y+o.base,(o.a.z+o.b.z)*.5};
      if(warp) { const auto q=warp(at); at.x=q.x; at.z=q.z; }
      building.exteriorDoors.push_back(at);
    }

    // The way to the door.
    //
    // A platform standing on the hill is a platform you cannot get onto, and
    // a door that opens onto a two metre drop is worse than no door. So the
    // way is BUILT, from the doorstep out to the nearest street, as a flight
    // where there is a rise to take and as a plain path where there is not.
    // It is built rather than carved for the same reason the platform is: a
    // carved one is a step in the terrain, and a step in the terrain is a
    // slit you can see through and a boolean that leaves slivers.
    //
    // Its shape is fixed by its run and its rise together. A rise that wants
    // more treads than the run can carry is a bank nobody can climb on that
    // line; it is recorded and the flight takes what fits, because a ladder
    // to the door is not an improvement on a drop.
    { const auto reserveStart=tick();
      const bool reserved=reserve(footprint,.7,region.id);
      reserveMs+=since(reserveStart);
      if (!reserved) return fail("invalid dwelling footprint reservation"); }
    if (!emitUniqueBuildingPart(region.id,building.kind,"lot-support",before,dwellingMeshStart) ||
        !emitUniqueBuildingPart(region.id,building.kind,"entrance-works",dwellingMeshEnd)) return false;
    if (settings.voxelFoundationSink) {
      std::vector<CityVoxelFoundationSample> samples;
      std::string joinError;
      const std::function<double(double,double)> supportHeight =
        [&](double x,double z) { return ground(x,z); };
      if (!sampleCityVoxelFoundationJoin(footprint,base-.03,supportHeight,samples,&joinError) ||
          !settings.voxelFoundationSink(region.id,samples,&joinError))
        return fail("lot "+std::to_string(region.id)+" voxel foundation join: "+joinError);
    }
    if (settings.voxelAssemblySink || settings.voxelBoundarySink) {
      if (warp) return fail("voxel assembly cannot represent a warped dwelling lot");
      hierarchy::DwellingVoxelAssembly assembly;
      std::string assemblyError;
      if (!hierarchy::buildDwellingVoxelAssembly(house,plan,skeleton,region.id,brief.seed,
                                                 assembly,&assemblyError))
        return fail("lot "+std::to_string(region.id)+" voxel assembly: "+assemblyError);
      assembly.grid.baseMetres=base;
      assembly.legacyTriangleFirst=dwellingMeshStart;
      assembly.legacyTriangleCount=dwellingMeshEnd-dwellingMeshStart;
      assembly.legacyPanelFirst=instanceBefore;
      assembly.legacyPanelCount=out.quadInstances.size()-instanceBefore;
      if (settings.voxelBoundarySink) {
        std::vector<CityVoxelPartyFacet> facets;
        const auto found=voxelPartyByBuilding.find(region.id);
        static const std::vector<CityVoxelPartyLine> noPartyLines;
        if (!collectCityVoxelPartyFacets(assembly,skeleton,
                                        found==voxelPartyByBuilding.end()?noPartyLines:found->second,
                                        facets,&assemblyError))
          return fail("lot "+std::to_string(region.id)+" voxel party facets: "+assemblyError);
        voxelPartyFacets.insert(voxelPartyFacets.end(),facets.begin(),facets.end());
      }
      if (settings.voxelAssemblySink && !settings.voxelAssemblySink(assembly,skeleton,&assemblyError))
        return fail("lot "+std::to_string(region.id)+" voxel sink: "+assemblyError);
    }
    out.buildings.push_back(std::move(building));
    bodyMs+=since(bodyStart);
    }
  }

  if(std::getenv("CITY_DEBUG"))
    std::fprintf(stderr,"lot planning: %u attempts for %u blueprints (%u shared-grid retries, %u setback, %u per-edge)\n"
      "  plan %.0f s | realise %.0f s | skeleton %.0f s | mesh %.0f s | weld %.0f s | obstacles %.0f s\n"
      "  envelope %.0f s | views %.0f s | reserve %.0f s | whole body %.0f s\n",
      attempts,out.lotBlueprints,sharedGridRetries,setbackRetries,edgeRetries,
      planMs/1000,realiseMs/1000,skeletonMs/1000,meshMs/1000,solidMs/1000,obstacleMs/1000,
      envelopeMs/1000,viewsMs/1000,reserveMs/1000,bodyMs/1000);

  std::vector<std::pair<Position,double>> openSpaceReservations;
  for(const auto &plaza:city.skeleton.plazas) {
    openSpaceReservations.push_back({plaza.centre,plaza.monumentRadius+1.});
    for(const auto &p:plaza.pavilions)openSpaceReservations.push_back({p.centre,4.2});
    for(const auto &p:plaza.gardens)openSpaceReservations.push_back({p,2.4});
  }
  // Vacant plots and planned common ground need a deliberate use. Keep the
  // street network open, but turn suitable leftovers into planted pockets or
  // small civic places. This pass runs after all houses and access slabs exist
  // so a tree or monument cannot occupy an entry that the planner accepted.
  const size_t openSpaceFirst=out.mesh.triangles.size();
  if(settings.streetDetails) {
    struct OpenSpace { uint64_t id; std::vector<Position> boundary; double rise; bool court; bool residual=false; };
    std::vector<OpenSpace> spaces;
    for(const auto &space:city.skeleton.publicSpaces)
      if((space.kind==CityPublicSpace::Kind::Court || space.kind==CityPublicSpace::Kind::Garden ||
          space.kind==CityPublicSpace::Kind::Yard || space.kind==CityPublicSpace::Kind::Sidewalk) &&
          space.boundary.size()>=3)
        spaces.push_back({space.owner*1315423911ull+spaces.size()+1,space.boundary,space.rise,
                          space.kind==CityPublicSpace::Kind::Court});
    for(const auto &region:city.skeleton.regions)
      if(region.kind==CityRegionKind::Lot && !developed.count(region.id) && region.boundary.size()>=3)
        spaces.push_back({region.id,region.boundary,0.,false});
    const auto inside=[](const std::vector<Position> &ring,double x,double z){
      bool yes=false;for(size_t i=0,j=ring.size()-1;i<ring.size();j=i++)
        if((ring[i].z>z)!=(ring[j].z>z) &&
           x<(ring[j].x-ring[i].x)*(z-ring[i].z)/(ring[j].z-ring[i].z)+ring[i].x)yes=!yes;
      return yes;
    };
    const auto distanceTo=[&](const std::vector<Position> &ring,Position p){
      double d=1e100;for(size_t i=0;i<ring.size();++i)
        d=std::min(d,segmentDistance(p,ring[i],ring[(i+1)%ring.size()]));return d;
    };
    const auto circle=[](Position p,double radius,int sides=10){
      std::vector<Position> ring;ring.reserve(sides);
      for(int i=0;i<sides;++i){const double a=6.283185307179586*i/sides;
        ring.push_back({p.x+radius*std::cos(a),0,p.z+radius*std::sin(a)});}
      return ring;
    };
    const auto square=[](Position p,double half){return std::vector<Position>{
      {p.x-half,0,p.z-half},{p.x+half,0,p.z-half},
      {p.x+half,0,p.z+half},{p.x-half,0,p.z+half}};};
    // Subdivision can leave a broad urban wedge between otherwise developed
    // blocks. It has no Lot or PublicSpace record, so identify small pockets
    // inside the district itself. Keep them away from block boundaries and
    // require nearby houses; this does not plant the undeveloped outskirts.
    std::vector<const CitySkeletonRegion*> blocks;
    for(const auto &r:city.skeleton.regions)
      if(r.kind==CityRegionKind::Block && r.boundary.size()>=3)blocks.push_back(&r);
    size_t residualSpaces=0;
    for(const auto &district:city.skeleton.regions)if(district.kind==CityRegionKind::District &&
        district.boundary.size()>=3 && residualSpaces<180){
      size_t districtPockets=0;
      double minX=1e100,maxX=-1e100,minZ=1e100,maxZ=-1e100;
      for(const auto &p:district.boundary){minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);
        minZ=std::min(minZ,p.z);maxZ=std::max(maxZ,p.z);}
      for(double z=minZ+3.1;z<maxZ-3.1 && residualSpaces<180 && districtPockets<8;z+=5.5)
        for(double x=minX+3.1;x<maxX-3.1 && residualSpaces<180 && districtPockets<8;x+=5.5){
          const Position p{x,0,z};
          if(!inside(district.boundary,x,z) || distanceTo(district.boundary,p)<3.1)continue;
          bool blockGround=false;
          for(const auto *block:blocks)if(inside(block->boundary,x,z) ||
              distanceTo(block->boundary,p)<3.1){blockGround=true;break;}
          if(blockGround)continue;
          unsigned neighbours=0;
          for(const auto &building:out.buildings)if(building.footprint.size()>=3){
            const auto c=mean(building.footprint);
            if(std::hypot(c.x-x,c.z-z)<24. && ++neighbours>=2)break;
          }
          if(neighbours<2)continue;
          spaces.push_back({district.id+uint64_t(residualSpaces)*1099511628211ull,
                            square(p,3.),0.,false,true});
          ++residualSpaces;++districtPockets;
        }
    }
    const auto decoratedPrism=[&](const std::vector<Position> &ring,double bottom,double top,uint32_t surface){
      const auto first=out.mesh.triangles.size();
      prism(out,index,ring,bottom,top,17,17);
      for(size_t i=first;i<out.surfaceMaterials.size();++i)out.surfaceMaterials[i]=surface;
    };
    const auto decoratedTriangle=[&](std::array<Position,3> points,uint32_t surface){
      const auto first=out.mesh.triangles.size();
      triangle(out,index,points,17);
      for(size_t i=first;i<out.surfaceMaterials.size();++i)out.surfaceMaterials[i]=surface;
    };
    struct Feature {Position at;double radius;};
    std::vector<Feature> placed;
    for(const auto &[at,radius]:openSpaceReservations)placed.push_back({at,radius});
    uint32_t trees=0,gardens=0,wells=0,monuments=0;
    for(const auto &space:spaces){
      double twiceArea=0,minX=1e100,maxX=-1e100,minZ=1e100,maxZ=-1e100;
      Position centre{};
      for(size_t i=0;i<space.boundary.size();++i){
        const auto a=space.boundary[i],b=space.boundary[(i+1)%space.boundary.size()];
        twiceArea+=a.x*b.z-b.x*a.z;centre.x+=a.x;centre.z+=a.z;
        minX=std::min(minX,a.x);maxX=std::max(maxX,a.x);
        minZ=std::min(minZ,a.z);maxZ=std::max(maxZ,a.z);
      }
      const double area=std::abs(twiceArea)*.5;
      if(area<(space.residual?30.:space.court?65.:42.) || maxX-minX<4.5 || maxZ-minZ<4.5)continue;
      centre.x/=space.boundary.size();centre.z/=space.boundary.size();
      const double centralDistance=std::hypot(centre.x-urbanCentre.x,centre.z-urbanCentre.z);
      const bool civic=(space.court && area>125. && centralDistance<190.) ||
          (!space.court && area>180. && centralDistance<145. && space.id%11==0);
      const uint32_t quota=civic && area>360.?2:area>220.?2:1;
      for(uint32_t n=0;n<quota;++n){
        // The large court gets one useful focal point. Vacant lots remain
        // mostly green, with a little variety among garden beds and trees.
        const unsigned kind=civic && n==0 ? ((wells+monuments)%2==0?3:2) :
                            (space.id+n*7)%4==0 ? 1 : 0; // tree, garden, well, monument
        const double radius=kind==0?1.8:kind==1?1.55:kind==2?2.25:2.0;
        const auto clear=[&](Position p){
          if(!inside(space.boundary,p.x,p.z) || distanceTo(space.boundary,p)<radius+.55)return false;
          for(const auto &other:placed)
            if(std::hypot(p.x-other.at.x,p.z-other.at.z)<radius+other.radius+1.5)return false;
          for(const auto &b:out.buildings){
            if(b.footprint.size()>=3 && (inside(b.footprint,p.x,p.z) ||
                distanceTo(b.footprint,p)<radius+1.2))return false;
            for(const auto &slab:b.siteAccess.slabs)if(slab.polygon.size()>=3 &&
                (inside(slab.polygon,p.x,p.z) || distanceTo(slab.polygon,p)<radius+.7))return false;
            for(const auto &door:b.exteriorDoors)
              if(std::hypot(p.x-door.x,p.z-door.z)<radius+2.2)return false;
          }
          for(const auto &route:city.routes)for(size_t k=1;k<route.path.size();++k)
            if(segmentDistance(p,route.path[k-1],route.path[k])<route.halfWidth+radius+.65)return false;
          for(const auto &alley:city.skeleton.alleys)if(alley.size()>=3 &&
              (inside(alley,p.x,p.z) || distanceTo(alley,p)<radius+.45))return false;
          // Carry the approach through the court, not only to its boundary.
          // Otherwise a perfectly clear gateway terminates at a monument.
          if(space.court)for(const auto &alley:city.skeleton.alleys)
            for(const auto &entry:alley)if(distanceTo(space.boundary,entry)<.2)
              if(segmentDistance(p,entry,centre)<radius+1.2)return false;
          for(const auto &taken:occupied)if(p.x>=taken.bounds.minX-radius && p.x<=taken.bounds.maxX+radius &&
              p.z>=taken.bounds.minY-radius && p.z<=taken.bounds.maxY+radius){
            if(geom2::contains(taken.polygon,{geom2::toUnit(p.x),geom2::toUnit(p.z)}))return false;
            const auto &ring=taken.polygon.outer;
            for(size_t i=0;i<ring.size();++i){
              const auto a=ring[i],b=ring[(i+1)%ring.size()];
              if(segmentDistance(p,{geom2::toMetres(a.x),0,geom2::toMetres(a.y)},
                    {geom2::toMetres(b.x),0,geom2::toMetres(b.y)})<radius+.2)return false;
            }
          }
          return true;
        };
        Position at{};bool found=false;
        // Centre first, then a deterministic two-metre lattice. A candidate
        // must retain generous circulation rather than fill every bare patch.
        if(clear(centre)){at=centre;found=true;}
        for(double z=minZ+radius+.6;z<maxZ-radius-.6 && !found;z+=2.)
          for(double x=minX+radius+.6;x<maxX-radius-.6 && !found;x+=2.)
            if(clear({x,0,z})){at={x,0,z};found=true;}
        if(!found)continue;
        const double base=ground(at.x,at.z)+std::max(0.,space.rise)+.12;
        if(kind==0){
          decoratedPrism(circle(at,1.42),base-.025,base+.035,11);
          decoratedPrism(circle(at,.22),base,base+2.1,3);
          for(int tier=0;tier<2;++tier){
            const double low=base+1.35+tier*.7,mid=low+.55,high=mid+.65;
            const double r=1.75-tier*.25;
            auto lower=circle(at,r,10),upper=circle(at,r*.72,10);
            for(int k=0;k<10;++k){const int j=(k+1)%10;
              auto a=lower[k],b=lower[j],c=upper[k],d=upper[j];
              a.y=b.y=low;c.y=d.y=mid;
              decoratedTriangle({a,c,b},24);decoratedTriangle({b,c,d},24);
              auto tip=at;tip.y=high;
              decoratedTriangle({c,tip,d},24);
            }
          }
          ++trees;
        }else if(kind==1){
          decoratedPrism(square(at,1.3),base,base+.28,9);
          decoratedPrism(square(at,1.12),base+.285,base+.37,11);
          for(double x:{-.65,.65})for(double z:{-.65,.65})
            decoratedPrism(circle({at.x+x,0,at.z+z},.42,8),base+.35,base+.8,24);
          ++gardens;
        }else if(kind==2){
          decoratedPrism(circle(at,1.7,12),base,base+.24,9);
          decoratedPrism(circle(at,1.2,12),base+.25,base+.48,23);
          for(int i=0;i<12;++i){const double a=6.283185307179586*i/12;
            decoratedPrism(circle({at.x+1.47*std::cos(a),0,at.z+1.47*std::sin(a)},.23,6),
                  base+.23,base+.62,9);}
          ++wells;
        }else{
          decoratedPrism(square(at,1.25),base,base+.36,9);
          decoratedPrism(square(at,.75),base+.36,base+2.75,1);
          decoratedPrism(square(at,.85),base+2.69,base+2.88,9);
          ++monuments;
        }
        placed.push_back({at,radius});
        openSpaceReservations.push_back({at,radius});
        // Later lot-boundary sampling must also respect these new objects.
        if(!reserve(square(at,radius*.65),.2))return fail("invalid open-space reservation");
      }
    }
    if(std::getenv("CITY_DEBUG"))std::fprintf(stderr,
      "open spaces: %zu candidates (%zu residual pockets), %u trees, %u garden beds, %u wells, %u monuments\n",
      spaces.size(),residualSpaces,trees,gardens,wells,monuments);
  }
  if(!emitCivilPart("open-space",0,openSpaceFirst))return false;

  // ---- the lot boundaries, and what stands in the yard ----------------------
  //
  // A lot edge that is not a party wall gets a boundary: a wall or a fence
  // along the street with a gate where the path reaches it, a fence between
  // neighbours where the houses do not meet, a fence across the back. Then
  // the yard gets what a yard of this age has in it - a woodpile against the
  // house, a washing line, a trough, a pen - on ground the house, the steps
  // and the boundary leave free. None of it is a building: it is built here,
  // from prisms, after every house has been placed, so it can see them all.
  const size_t yardFirst=out.mesh.triangles.size();
  if(!city.plotContracts && (settings.lotBoundaries || settings.streetDetails)) {
    const auto started=tick();
    const auto groundAt=[&](const double x,const double z){return ground(x,z)+.035;};
    const auto insideRing=[](const std::vector<Position> &ring,const double x,const double z){
      bool in=false;
      for(size_t i=0,j=ring.size()-1;i<ring.size();j=i++)
        if((ring[i].z>z)!=(ring[j].z>z) && x<(ring[j].x-ring[i].x)*(z-ring[i].z)/(ring[j].z-ring[i].z)+ring[i].x)in=!in;
      return in;
    };
    const auto ringDistance=[&](const std::vector<Position> &ring,const double x,const double z){
      double d=1e100;for(size_t i=0;i<ring.size();++i)d=std::min(d,segmentDistance({x,0,z},ring[i],ring[(i+1)%ring.size()]));return d;
    };
    // A box standing on the ground: `along` and `across` are its axes.
    const auto box=[&](const Position &centre,const double ax,const double az,const double length,const double width,
                       const double bottom,const double height,const uint32_t material){
      const double px=-az,pz=ax;
      std::vector<Position> ring={{centre.x-ax*length*.5-px*width*.5,0,centre.z-az*length*.5-pz*width*.5},
                                  {centre.x+ax*length*.5-px*width*.5,0,centre.z+az*length*.5-pz*width*.5},
                                  {centre.x+ax*length*.5+px*width*.5,0,centre.z+az*length*.5+pz*width*.5},
                                  {centre.x-ax*length*.5+px*width*.5,0,centre.z-az*length*.5+pz*width*.5}};
      prism(out,index,ring,bottom,bottom+height,material,material);
    };
    const auto trough=[&](Position c,double ax,double az,double y) {
      box(c,ax,az,1.35,.65,y,.12,37);
      for(int side:{-1,1}) {
        box({c.x-az*.27*side,0,c.z+ax*.27*side},ax,az,1.35,.11,y+.12,.38,37);
        box({c.x+ax*.62*side,0,c.z+az*.62*side},ax,az,.11,.43,y+.12,.38,37);
      }
      box(c,ax,az,1.11,.41,y+.30,.015,28);
    };
    enum class Boundary:uint8_t{StoneWall,Picket,Boards,Rails};
    struct GatePlan{Position hinge,latch,inward;Boundary kind;};
    std::vector<GatePlan> gatePlan;
    std::vector<std::pair<Position,Position>> boundaryReservations;
    std::vector<std::pair<Position,double>> propReservations;

    // Spatial indices. Every clearance test in this stage used to scan the
    // whole city - every house, step, road segment, alley, prop and fence -
    // for each sample along each lot edge: 61 s of a 3,800-house bake. Each
    // item's box carries its own margin; a query is the point plus the
    // caller's radius, and the exact test still decides.
    constexpr double kBin=8.;
    const auto ringBounds=[](const std::vector<Position> &ring,double margin){
      std::array<double,4> box{1e300,1e300,-1e300,-1e300};
      for(const auto &v:ring){box[0]=std::min(box[0],v.x);box[1]=std::min(box[1],v.z);box[2]=std::max(box[2],v.x);box[3]=std::max(box[3],v.z);}
      return std::array<double,4>{box[0]-margin,box[1]-margin,box[2]+margin,box[3]+margin};
    };
    struct RouteSegment{Position a,b;double halfWidth;};
    std::vector<RouteSegment> routeSegments;
    SpatialBins routeBins(kBin);
    for(const auto &route:city.routes)for(size_t k=1;k<route.path.size();++k){
      const auto &a=route.path[k-1],&b=route.path[k];const double w=route.halfWidth;
      routeBins.insert(std::min(a.x,b.x)-w,std::min(a.z,b.z)-w,std::max(a.x,b.x)+w,std::max(a.z,b.z)+w);
      routeSegments.push_back({a,b,w});
    }
    // Any road within its half width plus `extra` of the point.
    const auto nearRoute=[&](const Position &p,double extra){
      return routeBins.any(p.x,p.z,extra,[&](uint32_t i){
        const auto &g=routeSegments[i];return segmentDistance(p,g.a,g.b)<g.halfWidth+extra;});
    };
    SpatialBins buildingBins(kBin);   // Footprint and doors, by out.buildings index.
    for(const auto &b:out.buildings){
      std::vector<Position> extent=b.footprint;
      for(const auto &door:b.exteriorDoors)extent.push_back(door);
      if(extent.empty())extent.push_back({0,0,0});
      const auto box=ringBounds(extent,1.2);
      buildingBins.insert(box[0],box[1],box[2],box[3]);
    }
    struct StepSlab{const std::vector<Position> *polygon;};
    std::vector<StepSlab> stepSlabs;
    SpatialBins slabBins(kBin);
    for(const auto &b:out.buildings)for(const auto &slab:b.siteAccess.slabs)
      if((slab.landing || slab.tread) && !slab.polygon.empty()){
        const auto box=ringBounds(slab.polygon,0.);
        slabBins.insert(box[0],box[1],box[2],box[3]);
        stepSlabs.push_back({&slab.polygon});
      }
    SpatialBins alleyBins(kBin);
    for(const auto &alley:city.skeleton.alleys){
      const auto box=ringBounds(alley.empty()?std::vector<Position>{{0,0,0}}:alley,0.);
      alleyBins.insert(box[0],box[1],box[2],box[3]);
    }
    SpatialBins openSpaceBins(kBin);
    for(const auto &[p,r]:openSpaceReservations)openSpaceBins.insert(p.x-r-.15,p.z-r-.15,p.x+r+.15,p.z+r+.15);
    SpatialBins occupiedBins(kBin);
    for(const auto &taken:occupied)
      occupiedBins.insert(taken.bounds.minX-.4,taken.bounds.minY-.4,taken.bounds.maxX+.4,taken.bounds.maxY+.4);
    // Filled as props and fences are placed; boxes carry the larger of the
    // margins the tests below use.
    SpatialBins propBins(kBin),boundaryBins(kBin);
    const auto reserveProp=[&](Position at,double radius){
      propBins.insert(at.x-radius-.25,at.z-radius-.25,at.x+radius+.25,at.z+radius+.25);
      propReservations.push_back({at,radius});
    };

    const auto exteriorClear=[&](Position at,double radius,uint64_t own,bool checkOwn) {
      if(propBins.any(at.x,at.z,radius,[&](uint32_t i){const auto &[p,r]=propReservations[i];
          return std::hypot(at.x-p.x,at.z-p.z)<radius+r+.15;}))return false;
      if(openSpaceBins.any(at.x,at.z,radius,[&](uint32_t i){const auto &[p,r]=openSpaceReservations[i];
          return std::hypot(at.x-p.x,at.z-p.z)<radius+r+.15;}))return false;
      if(boundaryBins.any(at.x,at.z,radius,[&](uint32_t i){const auto &[a,b]=boundaryReservations[i];
          return segmentDistance(at,a,b)<radius+.25;}))return false;
      if(buildingBins.any(at.x,at.z,radius,[&](uint32_t i){
          const auto &b=out.buildings[i];
          if((checkOwn || b.id!=own) && !b.footprint.empty() &&
             (insideRing(b.footprint,at.x,at.z) || ringDistance(b.footprint,at.x,at.z)<radius))return true;
          for(auto door:b.exteriorDoors)if(std::hypot(at.x-door.x,at.z-door.z)<radius+1.2)return true;
          return false;}))return false;
      if(slabBins.any(at.x,at.z,radius+.2,[&](uint32_t i){const auto &slab=*stepSlabs[i].polygon;
          return slab.size()>=3 && (insideRing(slab,at.x,at.z) || ringDistance(slab,at.x,at.z)<radius+.2);}))return false;
      if(nearRoute(at,radius))return false;
      if(alleyBins.any(at.x,at.z,radius,[&](uint32_t i){const auto &alley=city.skeleton.alleys[i];
          return alley.size()>=3 && (insideRing(alley,at.x,at.z) || ringDistance(alley,at.x,at.z)<radius);}))return false;
      return true;
    };
    struct BoundaryRun { Position a,b;Boundary kind; };
    std::vector<BoundaryRun> boundaryPlan;
    const auto boundary=[&](const Position &a,const Position &b,const Boundary kind){
      if(std::hypot(b.x-a.x,b.z-a.z)<.4)return;
      boundaryBins.insert(std::min(a.x,b.x)-.25,std::min(a.z,b.z)-.25,std::max(a.x,b.x)+.25,std::max(a.z,b.z)+.25);
      boundaryReservations.emplace_back(a,b);
      boundaryPlan.push_back({a,b,kind});
    };

    // Every lot edge, and how many lots use it: a shared edge is a party line.
    std::map<SharedEdge,std::vector<uint64_t>> lotEdges;
    std::map<uint64_t,const CitySkeletonRegion*> lotsById;
    for(const auto &r:city.skeleton.regions)if(r.kind==CityRegionKind::Lot && allocations.count(r.id)){
      lotsById[r.id]=&r;
      for(size_t i=0;i<r.boundary.size();++i)lotEdges[edgeKey(r.boundary[i],r.boundary[(i+1)%r.boundary.size()])].push_back(r.id);
    }
    std::map<uint64_t,const CityModelBuilding*> housesById;
    for(const auto &b:out.buildings)if(!b.footprint.empty())housesById[b.id]=&b;

    // The mean developed lot in each block: what "dense" means here.
    const double denseLotMetres2=58.;
    std::map<uint64_t,double> blockLotArea;
    {
      std::map<uint64_t,std::pair<double,uint32_t>> totals;
      for(const auto &[id,region]:lotsById) {
        if(!housesById.count(id))continue;
        totals[region->parent].first+=region->area;
        totals[region->parent].second+=1;
      }
      for(const auto &[block,sum]:totals)
        if(sum.second)blockLotArea[block]=sum.first/double(sum.second);
    }

    uint32_t fenced=0,walled=0,gated=0,props=0,openBlocks=0,landlocked=0;
    for(const auto &[block,mean]:blockLotArea)if(mean<denseLotMetres2)++openBlocks;
    for(const auto &[id,region]:lotsById){
      const auto house=housesById.find(id);if(house==housesById.end())continue;
      const auto &lot=region->boundary;if(lot.size()<3)continue;
      const auto &building=*house->second;
      double houseArea=0,lotArea=0;
      for(size_t k=0;k<building.footprint.size();++k){auto a=building.footprint[k],b=building.footprint[(k+1)%building.footprint.size()];houseArea+=a.x*b.z-b.x*a.z;}
      for(size_t k=0;k<lot.size();++k){auto a=lot[k],b=lot[(k+1)%lot.size()];lotArea+=a.x*b.z-b.x*a.z;}
      const bool encloseYard=cityYardNeedsBoundary(std::abs(lotArea)*.5,std::abs(houseArea)*.5);
      const bool wealthy=region->districtUse==DistrictUse::WealthyResidential || region->districtUse==DistrictUse::Merchant ||
                         allocations.at(id)->merchantHouse || allocations.at(id)->residence!=CityResidenceKind::House;
      const uint64_t draw=(id*0x9E3779B97F4A7C15ull)>>40;

      // A dense block is not fenced.
      //
      // Where the houses are packed, the gap between them IS the way through
      // - the lane to the back, the passage between two frontages - and a
      // fence across it seals the block. Only the street front is bounded
      // there; the sides and the back are left open, which is also what a
      // row of houses with no yard to speak of actually looks like.
      const bool dense=!encloseYard && blockLotArea.count(region->parent) && blockLotArea.at(region->parent)<denseLotMetres2;

      // A lot the street does not reach is not fenced at all.
      //
      // Its way in is across a neighbour - that is what a back lot IS - and
      // a boundary round it seals the house off entirely. Measured on the
      // block plan, a third of the lots have no street edge of their own, so
      // this is not an edge case; where it happens the yards run together
      // and the way through stays open.
      bool anyStreet=false;
      for(size_t i=0;i<lot.size() && !anyStreet;++i) {
        const Position m{(lot[i].x+lot[(i+1)%lot.size()].x)*.5,0,(lot[i].z+lot[(i+1)%lot.size()].z)*.5};
        anyStreet=nearRoute(m,1.4);
      }
      if(!anyStreet){++landlocked;continue;}
      // The style of the front: a wall for the well-off and a quarter of
      // everyone else, boards or pickets for the rest; the sides and the back
      // are plainer than the front.
      const Boundary front=wealthy || draw%4==0 ? Boundary::StoneWall : draw%4==3 ? Boundary::Boards : Boundary::Picket;
      const Boundary side=wealthy ? Boundary::StoneWall : draw%3==0 ? Boundary::Boards : Boundary::Rails;
      // The door, projected: where the gate goes.
      const Position door=building.exteriorDoors.empty() ? Position{} : building.exteriorDoors.front();
      // The ground the steps take.
      std::vector<const std::vector<Position>*> taken;
      for(const auto &slab:building.siteAccess.slabs)if(slab.landing || slab.tread)taken.push_back(&slab.polygon);
      std::vector<Position> approach;
      for(const auto *slab:taken)approach.insert(approach.end(),slab->begin(),slab->end());
      const auto insetFence=encloseYard ? cityYardInsetFence(lot,building.footprint,approach) : std::nullopt;
      const auto &fenceLot=insetFence ? *insetFence : lot;

      // How far the door is from the boundary it opens towards: the nearest
      // edge, so the lot always has exactly one way in even where the door
      // stands well back.
      double doorReach=4.5;
      if(!building.exteriorDoors.empty()) {
        double nearest=1e100;
        for(size_t i=0;i<fenceLot.size();++i) {
          const auto shared=lotEdges.find(edgeKey(fenceLot[i],fenceLot[(i+1)%fenceLot.size()]));
          if(!insetFence && shared!=lotEdges.end() && shared->second.size()>1)continue; // Not through the neighbour's wall.
          nearest=std::min(nearest,segmentDistance(door,fenceLot[i],fenceLot[(i+1)%fenceLot.size()]));
        }
        if(nearest<1e99)doorReach=std::max(4.5,nearest+.05);
      }

      if(settings.lotBoundaries && encloseYard)for(size_t i=0;i<fenceLot.size();++i){
        const Position a=fenceLot[i],b=fenceLot[(i+1)%fenceLot.size()];
        const double length=std::hypot(b.x-a.x,b.z-a.z);if(length<.6)continue;
        const auto shared=lotEdges.find(edgeKey(a,b));
        const bool party=!insetFence && shared!=lotEdges.end() && shared->second.size()>1;
        if(party && shared->second.front()!=id)continue; // The lower id builds the shared fence, once.
        const Position m{(a.x+b.x)*.5,0,(a.z+b.z)*.5};
        // A street edge is one a route runs along.
        const bool street=nearRoute(m,1.4);
        if(dense && !street)continue; // The way through a dense block stays open.
        const Boundary kind=street?front:side;
        // Walk the edge; keep only the stretches no house stands on and no
        // step crosses, and leave the gate where the door's path meets it.
        const double gateAt=building.exteriorDoors.empty()?-1.:
          ((door.x-a.x)*(b.x-a.x)+(door.z-a.z)*(b.z-a.z))/std::max(1e-9,length);

        // A gate wherever the door's own way out crosses the line.
        //
        // The gate used to be cut only where the boundary met a street, so a
        // house whose door faces an alley or the middle of the block was
        // fenced in: the fence ran unbroken across the only way to its door.
        // The edge the door leads to gets a gate whatever kind of edge it
        // is, and a lot whose door leads nowhere near its boundary gets one
        // on the edge nearest the door instead.
        const bool gateHere=gateAt>=0 && gateAt<=length && segmentDistance(door,a,b)<=doorReach+1e-6;
        // Sample all structural and public circulation clearances. Blocked
        // samples stay blocked, even when the gap is shorter than a panel.
        const double step=.25;
        const size_t samples=size_t(length/step)+2;
        std::vector<char> open(samples,0);

        for(size_t i=0;i<samples;++i) {
          const double t=std::min(double(i)*step,length);
          const double x=a.x+(b.x-a.x)*t/length,z=a.z+(b.z-a.z)*t/length;
          bool free=ringDistance(building.footprint,x,z)>.3 && !insideRing(building.footprint,x,z);
          if(party)for(auto other:shared->second)if(other!=id){const auto h=housesById.find(other);
            if(h!=housesById.end() && (ringDistance(h->second->footprint,x,z)<=.3 || insideRing(h->second->footprint,x,z)))free=false;}
          // ...nor through anything else already standing: a neighbour on
          // another lot, a compound range, a tower, the city wall. A fence
          // run through a wall is a fence sticking out of masonry.
          if(free && occupiedBins.any(x,z,0.,[&](uint32_t i){const auto &taken=occupied[i];
              return x>=taken.bounds.minX-.4 && x<=taken.bounds.maxX+.4 && z>=taken.bounds.minY-.4 && z<=taken.bounds.maxY+.4 &&
                     geom2::contains(taken.polygon,{geom2::toUnit(x),geom2::toUnit(z)});}))free=false;
          if(free && slabBins.any(x,z,.3,[&](uint32_t i){const auto &slab=*stepSlabs[i].polygon;
              return insideRing(slab,x,z) || ringDistance(slab,x,z)<.3;}))free=false;
          if(free && nearRoute({x,0,z},.3))free=false;
          if(free && alleyBins.any(x,z,.3,[&](uint32_t i){const auto &alley=city.skeleton.alleys[i];
              return alley.size()>=3 && (insideRing(alley,x,z) || ringDistance(alley,x,z)<.3);}))free=false;
          if(free && propBins.any(x,z,0.,[&](uint32_t i){const auto &[p,r]=propReservations[i];
              return std::hypot(x-p.x,z-p.z)<r+.25;}))free=false;
          // Adjacent lots can split the same party line at different vertices.
          // Reserve geometry, not just an exact pair of edge endpoints.
          if(free && boundaryBins.any(x,z,0.,[&](uint32_t i){const auto &[u,v]=boundaryReservations[i];
              const double dx=v.x-u.x,dz=v.z-u.z,run=std::hypot(dx,dz);
              const double along=((x-u.x)*dx+(z-u.z)*dz)/std::max(run,1e-9);
              const double parallel=std::abs((b.x-a.x)*dz-(b.z-a.z)*dx)/std::max(length*run,1e-9);
              return parallel<.02 && along>.1 && along<run-.1 && segmentDistance({x,0,z},u,v)<.25;}))free=false;
          open[i]=free?1:0;
        }

        for(const auto &[from,to]:cityYardBoundaryRuns(length,open,step,gateHere?gateAt:-1.)) {
          boundary({a.x+(b.x-a.x)*from/length,0,a.z+(b.z-a.z)*from/length},
                   {a.x+(b.x-a.x)*to/length,0,a.z+(b.z-a.z)*to/length},kind);
          if(kind==Boundary::StoneWall)++walled;else ++fenced;
        }
        if(gateHere) {
          ++gated;
          // The gate itself: a post either side of the gap and a leaf hung
          // from one of them, swung open into the yard.
          const double ux=(b.x-a.x)/length,uz=(b.z-a.z)/length;
          const Position hinge{a.x+ux*(gateAt-.9),0,a.z+uz*(gateAt-.9)},latch{a.x+ux*(gateAt+.9),0,a.z+uz*(gateAt+.9)};
          Position lotCentre{};for(const auto &v:fenceLot){lotCentre.x+=v.x/double(fenceLot.size());lotCentre.z+=v.z/double(fenceLot.size());}
          Position inward{-uz,0,ux};
          if((lotCentre.x-m.x)*inward.x+(lotCentre.z-m.z)*inward.z<0){inward.x=-inward.x;inward.z=-inward.z;}
          gatePlan.push_back({hinge,latch,inward,kind});
        }
      }

      if(!settings.streetDetails)continue;
      // ---- the street frontage ----------------------------------------------
      //
      // What stands against a wall on the public side: a bench by the door, a
      // barrel or two, a stack of crates, and over a door that trades, a sign
      // hung from a bracket. A medieval street is its frontage - the
      // references are all wall, kerb and the things leaning on the wall -
      // and ours was bare plaster down to the cobbles.
      for(size_t i=0;i<building.footprint.size();++i) {
        const Position p=building.footprint[i],q=building.footprint[(i+1)%building.footprint.size()];
        const double length=std::hypot(q.x-p.x,q.z-p.z);
        if(length<2.2)continue;
        const double ax=(q.x-p.x)/length,az=(q.z-p.z)/length;
        // Outward from the building: the side the lot's centre is not on.
        double cx=0,cz=0;
        for(const auto &v:building.footprint){cx+=v.x/double(building.footprint.size());cz+=v.z/double(building.footprint.size());}
        const Position m{(p.x+q.x)*.5,0,(p.z+q.z)*.5};
        double nx=-az,nz=ax;
        if((m.x+nx-cx)*(m.x-cx)+(m.z+nz-cz)*(m.z-cz)<(m.x-cx)*(m.x-cx)+(m.z-cz)*(m.z-cz)){nx=-nx;nz=-nz;}

        // Only a wall that faces a street, and only where the ground outside
        // it is open.
        const bool onStreet=nearRoute({m.x+nx*1.2,0,m.z+nz*1.2},1.6);
        if(!onStreet)continue;

        const uint64_t roll=(id*0x9E3779B97F4A7C15ull+uint64_t(i)*1099511628211ull)>>38;
        const double y=groundAt(m.x,m.z);

        // A sign over the door, where the door is on this wall.
        if(!building.exteriorDoors.empty() && segmentDistance(door,p,q)<1.2) {
          const double armOut=.95,head=door.y+2.95;
          // Doorways sit on their dwelling foundations, above the sloping
          // street. Leave 2.4 metres clear below a sign, measured from the door.
          box({door.x+nx*armOut*.5,0,door.z+nz*armOut*.5},nx,nz,armOut,.09,head,.09,55);
          box({door.x+nx*armOut,0,door.z+nz*armOut},ax,az,.62,.06,head-.55,.45,55);
          box({door.x+nx*armOut,0,door.z+nz*armOut},nx,nz,.1,.1,head-.1,.1,55);
          ++props;
        }

        // Cantilevered market shades leave the pavement clear below 2.6m.
        // Keep the canopy off the door and within a short wall-side band.
        const bool trading=building.kind==CityModelBuildingKind::Inn || building.kind==CityModelBuildingKind::Pub || region->districtUse==DistrictUse::Merchant || region->districtUse==DistrictUse::Craftsmen;
        if(trading && length>4.5) {
          const double candidate=length*.3;
          const Position at{p.x+ax*candidate,0,p.z+az*candidate};
          if(std::hypot(at.x-door.x,at.z-door.z)>2.1) {
            const auto point=[&](double along,double reach,double height){return Position{at.x+ax*along+nx*reach,y+height,at.z+az*along+nz*reach};};
            for(int stripe=0;stripe<8;++stripe) {
              const double left=-1.4+stripe*.35,right=left+.35;
              const auto a=point(left,.04,3.15),b=point(right,.04,3.15),c=point(right,1.05,2.65),d=point(left,1.05,2.65);
              for(auto face:std::vector<std::array<Position,3>>{{a,b,c},{a,c,d},{c,b,a},{d,c,a}}){triangle(out,index,face,55);out.surfaceMaterials.back()=stripe%2?26:4;}
            }
            for(double along:{-1.4,1.4})box(point(along,.5,2.62),nx,nz,1.05,.08,y+2.62,.08,55);
            ++props;
          }
        }

        // ...and the things that lean on the wall, along its length.
        for(double t=1.1;t<length-.9;t+=2.6) {
          const Position at{p.x+ax*t,0,p.z+az*t};
          if(segmentDistance(door,{at.x,0,at.z},{at.x,0,at.z})<1.3)continue;
          const double frontageOffset=.42;
          const Position stand{at.x+nx*frontageOffset,0,at.z+nz*frontageOffset};
          if(!insideRing(lot,stand.x,stand.z) && ringDistance(lot,stand.x,stand.z)>1.4)continue;
          bool obstructs=nearRoute(stand,.4);
          for(const auto *slab:taken)obstructs|=insideRing(*slab,stand.x,stand.z) || ringDistance(*slab,stand.x,stand.z)<.45;
          for(auto entrance:building.exteriorDoors)obstructs|=std::hypot(stand.x-entrance.x,stand.z-entrance.z)<1.4;
          if(obstructs || !exteriorClear(stand,.95,id,false) || !insideRing(lot,stand.x,stand.z) || ringDistance(lot,stand.x,stand.z)<.4)continue;
          const double base=groundAt(stand.x,stand.z);

          switch((roll+uint64_t(t*4.))%5) {
            case 0: // a bench along the wall
              box(stand,ax,az,1.7,.42,base+.43,.09,55);
              for(int e=-1;e<=1;e+=2)box({stand.x+ax*.7*e,0,stand.z+az*.7*e},ax,az,.1,.38,base,.44,55);
              break;
            case 1: { // barrels
              for(int b=0;b<2;++b) {
                const Position c{stand.x+ax*(.64*b-.32),0,stand.z+az*(.64*b-.32)};
                const auto ring=[&](double radius){std::vector<Position> r;for(int k=0;k<12;++k){const double a=k*6.28318530718/12;r.push_back({c.x+radius*std::cos(a),0,c.z+radius*std::sin(a)});}return r;};
                prism(out,index,ring(.24),base,base+.12,17,55);prism(out,index,ring(.29),base+.12,base+.70,17,55);prism(out,index,ring(.24),base+.70,base+.82,17,55);
                for(double h:{.16,.62})prism(out,index,ring(.305),base+h,base+h+.055,40,40);
              }
              break;
            }
            case 2: // a stack of crates
              box(stand,ax,az,.7,.62,base,.5,55);
              box({stand.x+ax*.12,0,stand.z+az*.12},ax,az,.56,.5,base+.5,.42,55);
              break;
            case 3: // a water trough
              trough(stand,ax,az,base);
              break;
            default: break; // a stretch of bare wall
          }

          if((roll+uint64_t(t*4.))%5!=4){++props;reserveProp(stand,.95);}
        }
      }

      // ---- the yard ---------------------------------------------------------
      //
      // Candidate ground: inside the lot, off the house, off the steps, and a
      // little in from the boundary. Sampled on a half-metre grid; a prop is
      // placed where a cluster of free samples is.
      double area=0;for(size_t i=0;i<lot.size();++i){const auto p=lot[i],q=lot[(i+1)%lot.size()];area+=p.x*q.z-q.x*p.z;}area=std::abs(area)*.5;
      if(area<28)continue;
      double minX=1e100,minZ=1e100,maxX=-1e100,maxZ=-1e100;
      for(const auto &p:lot){minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);minZ=std::min(minZ,p.z);maxZ=std::max(maxZ,p.z);}
      const auto freeAt=[&](const double x,const double z,const double clearance){
        if(!insideRing(lot,x,z) || ringDistance(lot,x,z)<clearance)return false;
        if(!exteriorClear({x,0,z},clearance,id,true))return false;
        if(insideRing(building.footprint,x,z) || ringDistance(building.footprint,x,z)<clearance)return false;
        for(const auto *slab:taken)if(insideRing(*slab,x,z) || ringDistance(*slab,x,z)<clearance)return false;
        for(const auto &slab:building.siteAccess.slabs)if(!slab.landing && !slab.tread && slab.polygon.size()>=3 &&
          ringDistance(slab.polygon,x,z)<clearance*.5 && !insideRing(building.footprint,x,z) && insideRing(slab.polygon,x,z))return false;
        return true;
      };
      // Which way the house's nearest wall runs, so a woodpile lies along it.
      const auto alongWall=[&](const double x,const double z,double &ax,double &az){
        double best=1e100;ax=1;az=0;
        for(size_t i=0;i<building.footprint.size();++i){const auto p=building.footprint[i],q=building.footprint[(i+1)%building.footprint.size()];
          const double d=segmentDistance({x,0,z},p,q);const double l=std::hypot(q.x-p.x,q.z-p.z);
          if(d<best && l>1e-6){best=d;ax=(q.x-p.x)/l;az=(q.z-p.z)/l;}}
      };
      // A prop wants this much clear ground round its centre.
      struct Prop{double clearance;uint8_t kind;};
      const Prop wanted[]={{.9,0},{1.7,1},{.7,2},{1.6,3}}; // woodpile, washing line, trough, pen
      uint32_t placed=0;
      const uint32_t quota=area<60?1:area<110?2:3;
      for(uint32_t attempt=0;attempt<4 && placed<quota;++attempt){
        const Prop prop=wanted[(draw/(attempt+1)+attempt)%4];
        if(prop.kind==3 && area<70)continue;
        // Scan the lot for a free spot, from a seeded offset so lots differ.
        bool done=false;
        const double ox=std::fmod(double(draw%97)*.37,.5),oz=std::fmod(double(draw%89)*.41,.5);
        for(double z=minZ+oz;z<=maxZ && !done;z+=.5)for(double x=minX+ox;x<=maxX && !done;x+=.5){
          if(!freeAt(x,z,prop.clearance))continue;
          double ax=1,az=0;alongWall(x,z,ax,az);
          const double y=groundAt(x,z);
          if(prop.kind==0) { // Individual split logs, with visible gaps.
            for(int row=0;row<3;++row)for(int log=0;log<3-row;++log) {
              const double across=(log-(2-row)*.5)*.21;
              box({x-az*across,0,z+ax*across},ax,az,1.3,.18,y+row*.19,.17,55);
            }
          }                       // the woodpile
          else if(prop.kind==1){                                                       // the washing line
            for(int e=-1;e<=1;e+=2)box({x+ax*1.6*e,0,z+az*1.6*e},ax,az,.09,.09,y,1.85,55);
            box({x,0,z},ax,az,3.2,.03,y+1.78,.03,55);
            for(double t=-1.1;t<=1.1;t+=.55)box({x+ax*t,0,z+az*t},ax,az,.42,.02,y+1.0,.76,12);
          }
          else if(prop.kind==2)trough({x,0,z},ax,az,y);                  // the trough
          else{                                                                        // the pen
            const double px=-az,pz=ax,h=2.2;
            const Position c{x,0,z};
            const Position corners[4]={{c.x-ax*h*.5-px*h*.5,0,c.z-az*h*.5-pz*h*.5},{c.x+ax*h*.5-px*h*.5,0,c.z+az*h*.5-pz*h*.5},
                                       {c.x+ax*h*.5+px*h*.5,0,c.z+az*h*.5+pz*h*.5},{c.x-ax*h*.5+px*h*.5,0,c.z-az*h*.5+pz*h*.5}};
            for(int k=0;k<4;++k)boundary(corners[k],corners[(k+1)%4],Boundary::Rails);
          }
          reserveProp({x,0,z},prop.clearance);
          ++placed;++props;done=true;
        }
      }
    }
    // Build the planned runs together. Union masonry before extrusion so
    // corners have one connected shell instead of overlapping capped boxes.
    const auto masonry=[&](double halfWidth,double bottom,double top,uint32_t material) {
      std::vector<geom2::Operand> ribbons;
      for(const auto &run:boundaryPlan)if(run.kind==Boundary::StoneWall) {
        const auto a=run.a,b=run.b;const double len=std::hypot(b.x-a.x,b.z-a.z);
        const double ux=(b.x-a.x)/len,uz=(b.z-a.z)/len,nx=-uz*halfWidth,nz=ux*halfWidth;
        geom2::Polygon p;
        for(auto q:std::vector<Position>{{a.x-ux*halfWidth-nx,0,a.z-uz*halfWidth-nz},
          {b.x+ux*halfWidth-nx,0,b.z+uz*halfWidth-nz},{b.x+ux*halfWidth+nx,0,b.z+uz*halfWidth+nz},
          {a.x-ux*halfWidth+nx,0,a.z-uz*halfWidth+nz}})p.outer.push_back({geom2::toUnit(q.x),geom2::toUnit(q.z)});
        ribbons.push_back(geom2::toOperand(p));
      }
      if(ribbons.empty())return true;
      std::vector<geom2::Polygon> joined;std::string why;
      if(!geom2::unionOf(ribbons,joined,&why))return false;
      for(const auto &p:joined) {
        const auto sides=[&](const geom2::Ring &ring){for(size_t k=0;k<ring.size();++k){
          const auto v=ring[k],w=ring[(k+1)%ring.size()];
          Position a{geom2::toMetres(v.x),0,geom2::toMetres(v.y)},b{geom2::toMetres(w.x),0,geom2::toMetres(w.y)};
          a.y=groundAt(a.x,a.z)+bottom;b.y=groundAt(b.x,b.z)+bottom;
          auto c=b,d=a;c.y+=top-bottom;d.y+=top-bottom;
          triangle(out,index,{a,c,b},material);triangle(out,index,{a,d,c},material);
        }};
        sides(p.outer);for(const auto &hole:p.holes)sides(hole);
        std::vector<geom2::Point> verts;std::vector<std::array<uint32_t,3>> tris;
        if(!geom3::triangulate(p,tris,verts,&why))return false;
        for(auto t:tris){std::array<Position,3> face;for(int k=0;k<3;++k){auto v=verts[t[k]];face[k]={geom2::toMetres(v.x),0,geom2::toMetres(v.y)};face[k].y=groundAt(face[k].x,face[k].z)+top;}
          if((face[1].z-face[0].z)*(face[2].x-face[0].x)-(face[1].x-face[0].x)*(face[2].z-face[0].z)<0)std::swap(face[1],face[2]);
          triangle(out,index,face,material);
        }
      }
      return true;
    };
    if(!masonry(.21,-.05,.2,36) || !masonry(.17,.18,.95,36) || !masonry(.22,.90,1.02,32))return fail("yard perimeter union failed");
    // A fence is joinery, not a set of thin panels: posts with caps, two
    // rails let into them, and pickets with pointed heads or boards edge to
    // edge on the rails. Every piece is a closed solid with its own faces,
    // so it reads from any side and at any distance.
    std::set<std::pair<int64_t,int64_t>> fencePosts;
    const auto oriented=[&](std::array<Position,3> t,Position outward,uint32_t material,uint32_t surface){
      const Position u{t[1].x-t[0].x,t[1].y-t[0].y,t[1].z-t[0].z},v{t[2].x-t[0].x,t[2].y-t[0].y,t[2].z-t[0].z};
      const Position nrm{u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
      if(nrm.x*outward.x+nrm.y*outward.y+nrm.z*outward.z<0)std::swap(t[1],t[2]);
      const size_t before=out.mesh.triangles.size();
      triangle(out,index,t,material);
      if(out.mesh.triangles.size()>before && surface)out.surfaceMaterials.back()=surface;
    };
    // A post with a pyramid cap.
    const auto post=[&](Position p,double ax,double az,double side,double base,double top,uint32_t material){
      box(p,ax,az,side,side,base,top-base,material);
      const double px=-az,pz=ax,h=side*.5+.015;
      const std::array<Position,4> foot{{{p.x-ax*h-px*h,top,p.z-az*h-pz*h},{p.x+ax*h-px*h,top,p.z+az*h-pz*h},{p.x+ax*h+px*h,top,p.z+az*h+pz*h},{p.x-ax*h+px*h,top,p.z-az*h+pz*h}}};
      const Position apex{p.x,top+side*.7,p.z};
      for(size_t k=0;k<4;++k){const Position mid{(foot[k].x+foot[(k+1)%4].x)*.5-p.x,.2,(foot[k].z+foot[(k+1)%4].z)*.5-p.z};oriented({foot[k],foot[(k+1)%4],apex},mid,material,0);}
      oriented({foot[0],foot[2],foot[1]},{0,-1,0},material,0);oriented({foot[0],foot[3],foot[2]},{0,-1,0},material,0);
    };
    // A thin board or picket: a planar profile in the fence's plane,
    // extruded a few centimetres across it. `profile` is (along, up) pairs
    // round the outline; `at` is the origin on the run.
    const auto plank=[&](Position at,double ax,double az,const std::vector<std::pair<double,double>> &profile,double thickness,uint32_t material,uint32_t surface){
      const double nx=-az,nz=ax;
      std::vector<Position> front,back;
      for(const auto &[t,y]:profile){front.push_back({at.x+ax*t-nx*thickness*.5,at.y+y,at.z+az*t-nz*thickness*.5});back.push_back({at.x+ax*t+nx*thickness*.5,at.y+y,at.z+az*t+nz*thickness*.5});}
      for(size_t k=1;k+1<front.size();++k){oriented({front[0],front[k],front[k+1]},{-nx,0,-nz},material,surface);oriented({back[0],back[k],back[k+1]},{nx,0,nz},material,surface);}
      double centreY=0;for(const auto &[t,y]:profile)centreY+=(at.y+y)/double(profile.size());
      for(size_t k=0;k<front.size();++k){const size_t q=(k+1)%front.size();
        const Position mid{(front[k].x+front[q].x)*.5-at.x,(front[k].y+front[q].y)*.5-centreY,(front[k].z+front[q].z)*.5-at.z};
        oriented({front[k],front[q],back[q]},mid,material,surface);oriented({front[k],back[q],back[k]},mid,material,surface);}
    };
    // One rail between two points, let into the posts: a box that follows
    // the ground from end to end.
    const auto rail=[&](Position a,Position b,double ax,double az,double low,double high,double width,uint32_t material){
      const double nx=-az*width*.5,nz=ax*width*.5;
      std::array<Position,4> q{{{a.x-nx,0,a.z-nz},{b.x-nx,0,b.z-nz},{b.x+nx,0,b.z+nz},{a.x+nx,0,a.z+nz}}};
      std::array<Position,4> r=q;for(size_t k=0;k<4;++k){q[k].y=groundAt(q[k].x,q[k].z)+low;r[k].y=groundAt(r[k].x,r[k].z)+high;}
      const Position c{(a.x+b.x)*.5,0,(a.z+b.z)*.5};
      for(size_t k=0;k<4;++k){const size_t j=(k+1)%4;const Position mid{(q[k].x+q[j].x)*.5-c.x,0,(q[k].z+q[j].z)*.5-c.z};
        oriented({q[k],q[j],r[j]},mid,material,0);oriented({q[k],r[j],r[k]},mid,material,0);}
      oriented({r[0],r[1],r[2]},{0,1,0},material,0);oriented({r[0],r[2],r[3]},{0,1,0},material,0);
      oriented({q[0],q[2],q[1]},{0,-1,0},material,0);oriented({q[0],q[3],q[2]},{0,-1,0},material,0);
    };
    const auto fenceRun=[&](Position a,Position b,Boundary kind,double ownHeight){
      const double len=std::hypot(b.x-a.x,b.z-a.z);if(len<.3)return;
      const double ax=(b.x-a.x)/len,az=(b.z-a.z)/len;
      const double height=ownHeight>0?ownHeight:kind==Boundary::Picket?1.05:kind==Boundary::Boards?1.4:1.1;
      const unsigned bays=std::max(1u,unsigned(std::ceil(len/2.2)));
      for(unsigned k=0;k<=bays;++k){const double t=double(k)/bays;Position p{a.x+(b.x-a.x)*t,0,a.z+(b.z-a.z)*t};
        if(fencePosts.emplace(std::llround(p.x*100),std::llround(p.z*100)).second)
          post(p,ax,az,.12,groundAt(p.x,p.z)-.05,groundAt(p.x,p.z)+height+.12,55);
      }
      if(kind==Boundary::Rails){for(double h:{.26,.6,height-.08})rail(a,b,ax,az,h-.045,h+.045,.07,55);return;}
      rail(a,b,ax,az,height-.3,height-.22,.06,55);rail(a,b,ax,az,.3,.38,.06,55);
      if(kind==Boundary::Boards) {
        // Boards edge to edge on the rails, heads alternately flat and pointed.
        unsigned n=0;
        for(double t=.09;t<len-.08;t+=.17,++n){const Position at{a.x+ax*t,groundAt(a.x+ax*t,a.z+az*t),a.z+az*t};
          const double top=height-.02;
          const std::vector<std::pair<double,double>> flat{{-.075,.06},{.075,.06},{.075,top},{-.075,top}};
          const std::vector<std::pair<double,double>> pointed{{-.075,.06},{.075,.06},{.075,top-.06},{0,top+.03},{-.075,top-.06}};
          plank(at,ax,az,n%2?pointed:flat,.03,55,10);}
      } else {
        // Pickets a hand apart with pointed heads.
        for(double t=.2;t<len-.14;t+=.3){const Position at{a.x+ax*t,groundAt(a.x+ax*t,a.z+az*t),a.z+az*t};
          const double top=height-.08;
          plank(at,ax,az,{{-.04,.1},{.04,.1},{.04,top-.06},{0,top+.02},{-.04,top-.06}},.025,55,0);}
      }
    };
    for(const auto &run:boundaryPlan)if(run.kind!=Boundary::StoneWall)fenceRun(run.a,run.b,run.kind,0);
    // The gates: taller posts with caps either side of the gap, and a leaf
    // of boards hung from the hinge post, open into the yard with a
    // diagonal brace across it.
    for(const auto &gate:gatePlan) {
      const double len=std::hypot(gate.latch.x-gate.hinge.x,gate.latch.z-gate.hinge.z);if(len<1.)continue;
      const double ax=(gate.latch.x-gate.hinge.x)/len,az=(gate.latch.z-gate.hinge.z)/len;
      const double height=gate.kind==Boundary::StoneWall?1.2:gate.kind==Boundary::Boards?1.4:1.1;
      for(const Position &p:{gate.hinge,gate.latch})
        if(fencePosts.emplace(std::llround(p.x*100),std::llround(p.z*100)).second)
          post(p,ax,az,gate.kind==Boundary::StoneWall?.3:.16,groundAt(p.x,p.z)-.05,groundAt(p.x,p.z)+height+.12,gate.kind==Boundary::StoneWall?36:55);
      // The leaf swings 70 degrees into the yard from the hinge.
      const double c=std::cos(1.22),sn=std::sin(1.22);
      double lx=ax*c+gate.inward.x*sn,lz=az*c+gate.inward.z*sn;
      const double ll=std::hypot(lx,lz);lx/=ll;lz/=ll;
      const Position hinge{gate.hinge.x+ax*.12,0,gate.hinge.z+az*.12};
      const Position far{hinge.x+lx*(len-.4),0,hinge.z+lz*(len-.4)};
      const double leafHeight=height-.1;
      rail(hinge,far,lx,lz,.28,.36,.06,55);rail(hinge,far,lx,lz,leafHeight-.3,leafHeight-.22,.06,55);
      for(double t=.1;t<len-.45;t+=.17){const Position at{hinge.x+lx*t,groundAt(hinge.x+lx*t,hinge.z+lz*t),hinge.z+lz*t};
        plank(at,lx,lz,{{-.075,.08},{.075,.08},{.075,leafHeight-.05},{0,leafHeight+.02},{-.075,leafHeight-.05}},.03,55,10);}
      // The brace: a board from the low hinge corner to the high far corner.
      {const double run=len-.5,rise=leafHeight-.7;const double bl=std::hypot(run,rise);
       const Position lo{hinge.x+lx*.1,groundAt(hinge.x,hinge.z)+.36,hinge.z+lz*.1},hi{hinge.x+lx*(len-.4),groundAt(hinge.x,hinge.z)+leafHeight-.3,hinge.z+lz*(len-.4)};
       const double dx=(hi.x-lo.x)/bl,dy=(hi.y-lo.y)/bl,dz=(hi.z-lo.z)/bl;const double nx=-lz*.035,nz=lx*.035;
       const double px=-dy*lx,py=dx*lx+dz*lz,pz=-dy*lz; // across the board, in its plane
       const double pl=std::hypot(px,std::hypot(py,pz));
       std::array<Position,4> f{{{lo.x-px/pl*.05,lo.y-py/pl*.05,lo.z-pz/pl*.05},{hi.x-px/pl*.05,hi.y-py/pl*.05,hi.z-pz/pl*.05},{hi.x+px/pl*.05,hi.y+py/pl*.05,hi.z+pz/pl*.05},{lo.x+px/pl*.05,lo.y+py/pl*.05,lo.z+pz/pl*.05}}};
       std::array<Position,4> g=f;for(auto &q:f){q.x-=nx;q.z-=nz;}for(auto &q:g){q.x+=nx;q.z+=nz;}
       oriented({f[0],f[1],f[2]},{-nx,0,-nz},55,10);oriented({f[0],f[2],f[3]},{-nx,0,-nz},55,10);
       oriented({g[0],g[1],g[2]},{nx,0,nz},55,10);oriented({g[0],g[2],g[3]},{nx,0,nz},55,10);
       for(size_t k=0;k<4;++k){const size_t j=(k+1)%4;const Position mid{(f[k].x+f[j].x)*.5-(lo.x+hi.x)*.5,(f[k].y+f[j].y)*.5-(lo.y+hi.y)*.5,(f[k].z+f[j].z)*.5-(lo.z+hi.z)*.5};
         oriented({f[k],f[j],g[j]},mid,55,10);oriented({f[k],g[j],g[k]},mid,55,10);}
      }
    }
    if(std::getenv("CITY_DEBUG")) {
      const auto &tally=generator::CellHouse::partyTally();
      std::fprintf(stderr,"party walls: %u briefs named one, %u houses found theirs, %u runs pushed to the lot line, %u floors folded and reverted\n",
        tally.briefs.load(),tally.houses.load(),tally.pushed.load(),tally.folded.load());
    }
    if(std::getenv("CITY_DEBUG"))
      std::fprintf(stderr,"lot boundaries: %u fence runs, %u wall runs, %u gates, %u yard props in %.0f s; "
        "%u dense blocks left open, %u back lots left unfenced, %u folded faces dropped\n",
        fenced,walled,gated,props,since(started)/1000,openBlocks,landlocked,gDroppedFaces);
  }
  if(!emitCivilPart("yard",0,yardFirst))return false;

  // Large Gothic compounds own a complete fortification shell, independent
  // of the city's outer circuit. Their towers retain the fortress stair and
  // doorway topology rather than being filled cylinders.
  for(const auto &compound:compounds)if(compound.fantasyGothic) {
    const double base=compound.wings.empty()?0:compound.wings.front().brief.foundation;
    for(const auto &piece:compound.siteSolids) {
      const size_t first=out.mesh.triangles.size();
      // Stair notches are concave. A fan cap would bridge the opening and
      // bury the entire flight beneath a false floor.
      auto ring=piece.footprint;
      double signedArea=0;for(size_t k=0;k<ring.size();++k){const auto a=ring[k],b=ring[(k+1)%ring.size()];signedArea+=a.x*b.z-b.x*a.z;}
      if(signedArea<0)std::reverse(ring.begin(),ring.end());
      double perimeter=0;for(size_t k=0;k<ring.size();++k)perimeter+=std::hypot(ring[k].x-ring[(k+1)%ring.size()].x,ring[k].z-ring[(k+1)%ring.size()].z);
      const bool rocky=!piece.guard && piece.top-piece.bottom>2.5 && std::abs(signedArea)/std::max(1.,perimeter)>3.;
      if(rocky) {
        // Sculpt the exposed foundation within its reserved footprint. The
        // upper masonry stays flush with the deck, including stair notches.
        std::vector<Position> contour;
        for(size_t k=0;k<ring.size();++k) {
          const auto a=ring[k],b=ring[(k+1)%ring.size()];
          const unsigned count=std::max(1u,unsigned(std::ceil(std::hypot(b.x-a.x,b.z-a.z)/4.)));
          for(unsigned j=0;j<count;++j){const double t=double(j)/count;contour.push_back({a.x+(b.x-a.x)*t,0,a.z+(b.z-a.z)*t});}
        }
        const auto inside=[&](Position p){bool in=false;for(size_t i=0,j=ring.size()-1;i<ring.size();j=i++)
          if((ring[i].z>p.z)!=(ring[j].z>p.z) && p.x<(ring[j].x-ring[i].x)*(p.z-ring[i].z)/(ring[j].z-ring[i].z)+ring[i].x)in=!in;return in;};
        std::vector<Position> middle;
        for(size_t k=0;k<contour.size();++k) {
          const auto previous=contour[(k+contour.size()-1)%contour.size()],next=contour[(k+1)%contour.size()];
          const double dx=next.x-previous.x,dz=next.z-previous.z,len=std::hypot(dx,dz);
          auto p=contour[k];const double inset=.18+.16*(.5+.5*std::sin(p.x*.47+p.z*.31));
          if(len>1e-6){auto q=p;q.x-=dz/len*inset;q.z+=dx/len*inset;if(inside(q))p=q;}
          p.y=piece.bottom+(piece.top-piece.bottom)*(.36+.08*std::sin(p.x*.21+p.z*.34));middle.push_back(p);
        }
        for(size_t k=0;k<contour.size();++k) {
          const size_t j=(k+1)%contour.size();
          auto a=contour[k],b=contour[j],c=a,d=b;a.y=b.y=piece.bottom;c.y=d.y=piece.top-.85;
          const size_t start=out.mesh.triangles.size();
          triangle(out,index,{a,middle[j],b},36);triangle(out,index,{a,middle[k],middle[j]},36);
          triangle(out,index,{middle[k],d,middle[j]},36);triangle(out,index,{middle[k],c,d},36);
          for(size_t t=start;t<out.surfaceMaterials.size();++t)out.surfaceMaterials[t]=27;
          auto e=c,f=d;e.y=f.y=piece.top;
          triangle(out,index,{c,f,d},36);triangle(out,index,{c,e,f},36);
        }
      } else for(size_t k=0;k<ring.size();++k) {
        auto a=ring[k],b=ring[(k+1)%ring.size()],c=b,d=a;
        a.y=b.y=piece.bottom;c.y=d.y=piece.top;
        triangle(out,index,{a,c,b},36);triangle(out,index,{a,d,c},36);
      }
      std::vector<geom3::Vertex> vertices;for(auto p:ring)vertices.push_back({p.x,piece.top,p.z});
      std::vector<std::array<uint32_t,3>> triangles;std::string why;
      if(!geom3::triangulateFace(vertices,triangles,&why))return fail("castle terrace cap: "+why);
      for(auto t:triangles) {
        Position a{vertices[t[0]].x,piece.top,vertices[t[0]].z},b{vertices[t[1]].x,piece.top,vertices[t[1]].z},c{vertices[t[2]].x,piece.top,vertices[t[2]].z};
        if((b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z)<0)std::swap(b,c);
        triangle(out,index,{a,b,c},37);
      }
      for(size_t t=first;t<out.surfaceMaterials.size();++t)if(out.surfaceMaterials[t]!=27)out.surfaceMaterials[t]=out.materials[t]==37?19:1;
      if(!emitCivilPart("fortification",first,first))return false;
    }
    // Convex subtraction keeps wall bodies out of tower interiors. Each
    // surviving piece is convex, so its cap cannot bridge a doorway or hole.
    const auto subtractTower=[](std::vector<Position> inside,const std::vector<Position> &tower) {
      std::vector<std::vector<Position>> outside;
      for(size_t e=0;e<tower.size() && inside.size()>=3;++e) {
        const auto a=tower[e],b=tower[(e+1)%tower.size()];
        const auto side=[&](Position p){return (b.x-a.x)*(p.z-a.z)-(b.z-a.z)*(p.x-a.x);};
        const auto clip=[&](bool keepInside) {
          std::vector<Position> result;
          for(size_t i=0;i<inside.size();++i) {
            const auto p=inside[i],q=inside[(i+1)%inside.size()];const double sp=side(p),sq=side(q);
            const bool ip=keepInside?sp>=-1e-8:sp<=1e-8,iq=keepInside?sq>=-1e-8:sq<=1e-8;
            if(ip)result.push_back(p);
            if(ip!=iq && std::abs(sp-sq)>1e-10){const double t=sp/(sp-sq);result.push_back({p.x+(q.x-p.x)*t,0,p.z+(q.z-p.z)*t});}
          }
          return result;
        };
        auto cut=clip(false);if(cut.size()>=3)outside.push_back(std::move(cut));inside=clip(true);
      }
      return outside;
    };
    const auto bar=[&](Position a,Position b,double width,double bottom,double top) {
      const double len=std::hypot(b.x-a.x,b.z-a.z);if(len<1e-6)return;
      const double nx=-(b.z-a.z)/len*width*.5,nz=(b.x-a.x)/len*width*.5;
      std::vector<std::vector<Position>> pieces{{{a.x-nx,0,a.z-nz},{b.x-nx,0,b.z-nz},{b.x+nx,0,b.z+nz},{a.x+nx,0,a.z+nz}}};
      for(const auto &tower:compound.towers) {
        std::vector<std::vector<Position>> next;
        for(auto &piece:pieces){auto parts=subtractTower(piece,tower.footprint);next.insert(next.end(),parts.begin(),parts.end());}
        pieces=std::move(next);
      }
      if(compound.wallAttachedKeep)for(const auto &wing:compound.wings)if(wing.role=="keep") {
        std::vector<std::vector<Position>> next;
        for(const auto &piece:pieces){auto parts=subtractTower(piece,wing.brief.shell);next.insert(next.end(),parts.begin(),parts.end());}
        pieces=std::move(next);
      }
      for(const auto &piece:pieces)prism(out,index,piece,bottom,top,36,37);
    };
    const size_t compoundWallFirst=out.mesh.triangles.size();
    for(const auto &wall:compound.walls) {
      const double base=(compound.wings.empty()?0:compound.wings.front().brief.foundation)+wall.a.y;
      const double len=std::hypot(wall.b.x-wall.a.x,wall.b.z-wall.a.z);
      if(compound.landmarkLayout!=generator::LandmarkLayout::Citadel) {
        if(!wall.gate)bar(wall.a,wall.b,wall.thickness,base,base+wall.height);
        continue;
      }
      bar(wall.a,wall.b,wall.thickness,base+(wall.gate?8.:0.),base+wall.height);
      if(wall.gate && len>1.) {
        const Position middle{(wall.a.x+wall.b.x)*.5,base+8.,(wall.a.z+wall.b.z)*.5};
        const double nx=-(wall.b.z-wall.a.z)/len*wall.thickness*.5,nz=(wall.b.x-wall.a.x)/len*wall.thickness*.5;
        for(const auto end:{wall.a,wall.b}) {
          std::array<Position,3> front{{{end.x-nx,base+4.5,end.z-nz},{end.x-nx,base+8.,end.z-nz},{middle.x-nx,middle.y,middle.z-nz}}},back=front;
          for(auto &p:back){p.x+=2*nx;p.z+=2*nz;}
          triangle(out,index,front,36);triangle(out,index,{back[2],back[1],back[0]},36);
          for(size_t i=0;i<3;++i){const size_t j=(i+1)%3;triangle(out,index,{front[i],back[i],back[j]},36);triangle(out,index,{front[i],back[j],front[j]},36);}
        }
      }
      // Clear 2.8m promenade between two half-metre parapets. Merlons
      // belong to the outside parapet, never across the walking surface.
      const double nx=-(wall.b.z-wall.a.z)/len,nz=(wall.b.x-wall.a.x)/len;
      for(double sign:{-1.,1.}) {
        const auto point=[&](double t){return Position{wall.a.x+(wall.b.x-wall.a.x)*t/len+nx*sign*(wall.thickness*.5-.25),0,
          wall.a.z+(wall.b.z-wall.a.z)*t/len+nz*sign*(wall.thickness*.5-.25)};};
        std::vector<std::pair<double,double>> gaps;
        if(sign>0)for(const auto &portal:compound.wallAccess)if(std::abs(portal.at.y-base-wall.height)<.01 && segmentDistance(portal.at,wall.a,wall.b)<.1) {
          const double along=((portal.at.x-wall.a.x)*(wall.b.x-wall.a.x)+(portal.at.z-wall.a.z)*(wall.b.z-wall.a.z))/len;
          gaps.push_back({std::max(0.,along-portal.width*.5),std::min(len,along+portal.width*.5)});
        }
        std::sort(gaps.begin(),gaps.end());double from=0;
        for(const auto gap:gaps){if(gap.first>from)bar(point(from),point(gap.first),.5,base+wall.height,base+wall.height+.85);from=std::max(from,gap.second);}
        if(from<len)bar(point(from),point(len),.5,base+wall.height,base+wall.height+.85);
        if(sign<0)for(double at=0;at+1.1<len;at+=2.3)
          bar(point(at),point(at+1.1),.5,base+wall.height+.85,base+wall.height+1.65);
      }
    }
    if(compound.landmarkLayout==generator::LandmarkLayout::Citadel)for(const auto &wall:compound.walls)if(!wall.gate) {
      const double base=(compound.wings.empty()?0:compound.wings.front().brief.foundation)+wall.a.y;
      const double length=std::hypot(wall.b.x-wall.a.x,wall.b.z-wall.a.z);
      const double ux=(wall.b.x-wall.a.x)/length,uz=(wall.b.z-wall.a.z)/length,nx=uz,nz=-ux;
      for(double at=7.;at<length-5.;at+=9.) {
        const Position centre{wall.a.x+ux*at,0,wall.a.z+uz*at};
        for(const auto tier:{std::array<double,3>{0.,.35,1.2},std::array<double,3>{.35,.65,.8}}) {
          const Position a{centre.x+nx*(wall.thickness*.5-.05),0,centre.z+nz*(wall.thickness*.5-.05)},b{a.x+nx*tier[2],0,a.z+nz*tier[2]};
          bar(a,b,1.1,base+wall.height*tier[0],base+wall.height*tier[1]);
        }
      }
    }
    if(!emitCivilPart("compound-wall",compound.id,compoundWallFirst))return false;
    uint64_t ti=0;
    for(const auto &tower:compound.towers) {
      const double base=(compound.wings.empty()?0:compound.wings.front().brief.foundation)+tower.footprint.front().y;
      const auto centre=mean(tower.footprint);
      generator::FortressJunctionBrief brief;brief.centre=centre;brief.base=base;
      brief.towerRadius=std::hypot(tower.footprint.front().x-centre.x,tower.footprint.front().z-centre.z);
      brief.towerRotation=std::atan2(tower.footprint.front().z-centre.z,tower.footprint.front().x-centre.x)+3.141592653589793*.5;
      brief.towerSides=uint32_t(tower.footprint.size());brief.height=tower.height;brief.floors=uint32_t(std::round(tower.height/4.2));
      brief.inhabitedFloors=true;
      for(const auto &wall:compound.walls) {
        const double dx=wall.b.x-wall.a.x,dz=wall.b.z-wall.a.z,len=std::hypot(dx,dz);
        const double along=((centre.x-wall.a.x)*dx+(centre.z-wall.a.z)*dz)/(len*len);
        const double distance=std::abs((centre.x-wall.a.x)*dz-(centre.z-wall.a.z)*dx)/len;
        if(distance<.1 && along>=-1e-6 && along<=1.000001) {
          if(along>.001)brief.elevatedDoors.push_back({-dx/len,base+wall.height,-dz/len});
          if(along<.999)brief.elevatedDoors.push_back({dx/len,base+wall.height,dz/len});
        }
      }
      brief.roofHeight=brief.towerRadius*(2.8+.2*(ti%3));brief.belfryHeight=3.2;brief.sweptSpire=true;
      const auto inner=mean(compound.enclosure);const double run=std::hypot(inner.x-centre.x,inner.z-centre.z);
      brief.inward={(inner.x-centre.x)/run,0,(inner.z-centre.z)/run};
      generator::FortressJunctionBlueprint bp;std::string why;
      if(!generator::planFortressJunction(brief,bp,&why))return fail("Gothic tower: "+why);
      // Narrow defensive lights alternate with wider upper chamber windows.
      for(uint32_t floor=1;floor<brief.floors;++floor)for(uint32_t face=0;face<12;face+=2) {
        const double bottom=base+floor*4.2+1.0,top=bottom+1.8;
        bool overlaps=false;for(const auto &door:bp.openings)
          overlaps|=door.face==face && top>door.bottom && bottom<door.top;
        if(!overlaps)bp.openings.push_back({face,bottom,top,floor+2>=brief.floors?1.0:.6});
      }
      auto structure=generator::buildFortressJunctionStructure(bp);hierarchy::Solid mesh;
      generator::meshFortressJunction(structure,mesh);
      mesh.walls.insert(mesh.walls.end(),mesh.innerWalls.begin(),mesh.innerWalls.end());mesh.innerWalls.clear();
      const auto first=out.mesh.triangles.size();
      if(!solid(out,index,mesh,0,40,&why))return fail(why);
      for(size_t t=first;t<out.surfaceMaterials.size();++t) {
        const bool roof=out.materials[t]==42;
        out.surfaceMaterials[t]=roof?20:1;
        const auto &tri=out.mesh.triangles[t];
        const bool horizontal=std::abs(out.mesh.vertices[tri[0]].y-out.mesh.vertices[tri[1]].y)<1e-6 &&
          std::abs(out.mesh.vertices[tri[0]].y-out.mesh.vertices[tri[2]].y)<1e-6;
        if(horizontal)out.surfaceMaterials[t]=19;
        double reference=0;
        for(size_t k=0;k<3;++k) {
          const auto p=out.mesh.vertices[out.mesh.triangles[t][k]];
          double angle=std::atan2(p.z-centre.z,p.x-centre.x);
          if(k==0)reference=angle;
          while(angle-reference>3.141592653589793)angle-=6.283185307179586;
          while(angle-reference< -3.141592653589793)angle+=6.283185307179586;
          out.triangleUVs[t][k]=(roof || horizontal)?std::array<double,2>{p.x,p.z}:std::array<double,2>{angle*brief.towerRadius,p.y-base};
        }
      }
      for(double level:{9.3,tower.height-.7}) {
        for(size_t k=0;k<bp.tower.size();++k) {
          const auto a=bp.tower[k],b=bp.tower[(k+1)%bp.tower.size()];
          const auto expand=[&](Position p,double extra){const double scale=(brief.towerRadius+extra)/brief.towerRadius;return Position{centre.x+(p.x-centre.x)*scale,0,centre.z+(p.z-centre.z)*scale};};
          prism(out,index,{expand(a,0),expand(b,0),expand(b,.35),expand(a,.35)},base+level,base+level+.3,36,37);
        }
      }
      const uint64_t towerId=compound.id*1000+(++ti);
      if (!emitUniqueBuildingPart(towerId,CityModelBuildingKind::DefenceTower,"compound-tower",first)) return false;
      out.buildings.push_back({towerId,compound.id,CityModelBuildingKind::DefenceTower,bp.tower,base,tower.height+brief.belfryHeight,brief.roofHeight,true,true});
      auto &record=out.buildings.back();record.rooms=brief.floors+1;
      for(const auto &surface:bp.towerAccess.surfaces)if(surface.kind!=generator::access::SurfaceKind::Landing)
        record.interiorViews.push_back({centre.x+2.,surface.height+1.65,centre.z});
      if(bp.doorway.size()==4)record.exteriorDoors.push_back({(bp.doorway[0].x+bp.doorway[1].x)*.5,base,(bp.doorway[0].z+bp.doorway[1].z)*.5});
    }
  }

  // Estate landscape is planned with the footprint allocations. It is a
  // native mesh layer, so exports and runtime consumers see the same courts.
  const size_t estateFirst=out.mesh.triangles.size();
  for(const auto &compound:compounds)if(compound.fantasyGothic) {
    const double base=compound.wings.empty()?0:compound.wings.front().brief.foundation;
    const auto dressed=[&](const std::vector<Position> &ring,double low,double high,uint32_t material) {
      const size_t first=out.mesh.triangles.size();prism(out,index,ring,low,high,36,37);
      for(size_t t=first;t<out.surfaceMaterials.size();++t)out.surfaceMaterials[t]=material;
    };
    const auto strip=[&](Position a,Position b,double width,double low,double high,uint32_t material) {
      const double len=std::hypot(b.x-a.x,b.z-a.z);if(len<1e-6)return;
      const double nx=-(b.z-a.z)/len*width*.5,nz=(b.x-a.x)/len*width*.5;
      dressed({{a.x-nx,0,a.z-nz},{b.x-nx,0,b.z-nz},{b.x+nx,0,b.z+nz},{a.x+nx,0,a.z+nz}},low,high,material);
    };
    for(const auto &yard:compound.yards) {
      const double base=(compound.wings.empty()?0:compound.wings.front().brief.foundation)+yard.footprint.front().y;
      dressed(yard.footprint,base+.025,base+.055,yard.role=="great-yard"?19:11);
      for(size_t k=0;k<yard.footprint.size();++k)strip(yard.footprint[k],yard.footprint[(k+1)%yard.footprint.size()],1.5,base+.055,base+.075,18);
    }
    for(const auto &path:compound.paths)strip(path.a,path.b,path.width,base+path.a.y+(compound.terraces.empty()?.04:.025),base+path.a.y+(compound.terraces.empty()?.10:.04),18);
    for(const auto &garden:compound.gardens) {
      const double base=(compound.wings.empty()?0:compound.wings.front().brief.foundation)+garden.footprint.front().y;
      dressed(garden.footprint,base+.06,base+.16,11);
      const auto centre=mean(garden.footprint);
      for(size_t k=0;k<garden.footprint.size();++k) {
        const auto a=garden.footprint[k],b=garden.footprint[(k+1)%garden.footprint.size()];
        strip(a,b,.42,base+.1,base+.3,1);
        const auto inward=[&](Position p){return Position{p.x+(centre.x-p.x)*.13,0,p.z+(centre.z-p.z)*.13};};
        strip(inward(a),inward(b),.65,base+.16,base+.85,24);
      }
      // Four planted islands keep the parterre readable at city scale.
      for(const auto corner:garden.footprint) {
        const Position p{centre.x+(corner.x-centre.x)*.45,0,centre.z+(corner.z-centre.z)*.45};
        std::vector<Position> ring;for(unsigned k=0;k<8;++k){const double a=k*6.28318530718/8;ring.push_back({p.x+std::cos(a)*1.1,0,p.z+std::sin(a)*1.1});}
        dressed(ring,base+.16,base+.62,24);
      }
    }
    for(const auto centre:compound.fountains) {
      const double base=(compound.wings.empty()?0:compound.wings.front().brief.foundation)+centre.y;
      const auto circle=[&](double radius){std::vector<Position> ring;for(unsigned k=0;k<24;++k){const double a=k*6.28318530718/24;ring.push_back({centre.x+std::cos(a)*radius,0,centre.z+std::sin(a)*radius});}return ring;};
      dressed(circle(3.3),base+.1,base+.28,1);
      const auto inner=circle(2.8),outer=circle(3.1);
      for(size_t k=0;k<24;++k){const size_t q=(k+1)%24;dressed({outer[k],outer[q],inner[q],inner[k]},base+.28,base+.78,1);}
      dressed(circle(2.8),base+.28,base+.62,23);
      dressed(circle(.52),base+.62,base+2.5,1);
      dressed(circle(1.25),base+1.6,base+1.82,1);
      dressed(circle(.78),base+2.45,base+2.64,1);
    }
  }

  // The market square's structures: the arcade and its gates, the dais and
  // the monument, planned by the skeleton and built here on the square's
  // ground. Every face carries its own surface, so nothing is recoloured.
  if(!emitCivilPart("estate",0,estateFirst))return false;
  for(const auto &plaza:city.skeleton.plazas) {
    const size_t plazaFirst=out.mesh.triangles.size();
    hierarchy::Solid mesh;
    generator::meshPlaza(plaza,mesh);
    std::string why;
    if(!solid(out,index,mesh,plaza.level+finishedFloorOffset,36,&why))return fail("plaza "+std::to_string(plaza.id)+": "+why);
    if(!emitCivilPart("plaza",plaza.id,plazaFirst))return false;
    for(const auto &note:plaza.notes)if(std::getenv("CITY_DEBUG"))std::fprintf(stderr,"plaza %llu: %s\n",(unsigned long long)plaza.id,note.c_str());
  }

  struct RealisedWing {
    const generator::CompoundPlan *compound;
    const generator::CompoundWing *wing;
    generator::CellHouse house;
    DwellingPlan plan;
  };
  std::vector<RealisedWing> realisedWings;
  // Preserve the precinct planner's wing allocations, but realise them using
  // the same cell blueprint -> skeleton -> mesh contract as ordinary houses.
  // Legacy wing.plan has floor slabs but not the cell wall/roof topology.
  for (const auto &compound : compounds)
    for (const auto &wing : compound.wings)
    {
      // Fortification shells already own these spaces. A second square wing
      // puts square floor plates through the polygonal tower's exterior.
      if (compound.kind==generator::CompoundKind::Castle && (wing.role=="tower" || wing.role=="gatehouse")) continue;
      generator::CellDwellingBrief brief;
      brief.id=wing.brief.id; brief.seed=wing.brief.seed;
      brief.building=compound.kind==generator::CompoundKind::Church ? generator::BuildingClass::Church : compound.kind==generator::CompoundKind::Manor ? generator::BuildingClass::ManorHouse : generator::BuildingClass::GuildHall;
      if (compound.kind==generator::CompoundKind::Monastery)
      {
        brief.building=wing.role=="church" ? generator::BuildingClass::Church : wing.role=="dormitory" ? generator::BuildingClass::Dormitory : generator::BuildingClass::Dwelling;
        brief.tags.push_back("monastery-"+wing.role);
      }
      if(compound.fantasyGothic)brief.building=wing.role=="chapel"?generator::BuildingClass::Church:generator::BuildingClass::Castle;
      brief.floors=wing.brief.floors;
      if(compound.fantasyGothic && compound.landmarkLayout==generator::LandmarkLayout::Citadel) {
        auto grid=generator::profileFor(brief.building).grid;grid.storeyMetres=4.2;brief.grid=grid;
      }
      for (auto p:wing.brief.shell) brief.lot.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
      if (!geom2::counterClockwise(brief.lot.outer)) geom2::reverse(brief.lot.outer);
      double area=0;
      for (size_t i=0;i<wing.brief.shell.size();++i) { const auto a=wing.brief.shell[i],b=wing.brief.shell[(i+1)%wing.brief.shell.size()]; area+=a.x*b.z-b.x*a.z; }
      area=std::abs(area)*.5;
      brief.capacity=generator::CellDwellingBrief::Capacity{std::clamp(area,4.,4096.),uint32_t(std::clamp(area/30.,1.,128.)),30.,true};
      // Compound reservations and road curves originate in different planning
      // stages. The final full-width roads are hard blueprint obstacles too;
      // ownership containment alone does not establish street clearance.
      for (const auto &route:city.routes)
        for (size_t k=1;k<route.path.size();++k)
        {
          const auto a=route.path[k-1],b=route.path[k];
          const double dx=b.x-a.x,dz=b.z-a.z,len=std::hypot(dx,dz);
          if (len<1e-6) continue;
          const double clearance=route.halfWidth+.5;
          const double ux=dx/len,uz=dz/len,nx=-uz*clearance,nz=ux*clearance;
          geom2::Polygon obstacle;
          for (const auto &p:std::vector<Position>{{a.x-ux*clearance+nx,0,a.z-uz*clearance+nz},
              {a.x-ux*clearance-nx,0,a.z-uz*clearance-nz},
              {b.x+ux*clearance-nx,0,b.z+uz*clearance-nz},
              {b.x+ux*clearance+nx,0,b.z+uz*clearance+nz}})
            obstacle.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
          if (!geom2::counterClockwise(obstacle.outer)) geom2::reverse(obstacle.outer);
          const auto bounds=geom2::bounds(obstacle),lot=geom2::bounds(brief.lot);
          if(bounds.maxX>=lot.minX && bounds.minX<=lot.maxX && bounds.maxY>=lot.minY && bounds.minY<=lot.maxY)
            brief.obstacles.push_back(std::move(obstacle));
        }
      brief.frontage={0};
      if (compound.kind==generator::CompoundKind::Monastery)
      {
        double nearest=1e100;
        for (size_t i=0;i<wing.brief.shell.size();++i)
        {
          const auto a=wing.brief.shell[i],b=wing.brief.shell[(i+1)%wing.brief.shell.size()];
          const double d=segmentDistance(wing.brief.entrance,a,b);
          if (d<nearest) {nearest=d;brief.frontage={uint32_t(i)};}
        }
      }
      generator::CellDwellingSettings dwellingSettings;
      if(compound.kind==generator::CompoundKind::Monastery && wing.role=="church") {
        dwellingSettings.tallRoomStoreys=2.5;
        dwellingSettings.roofPitch=.85;
        dwellingSettings.outerWallMetres=.65;
        dwellingSettings.dormerChance=0;
      }
      dwellingSettings.maximumFloors=(compound.kind==generator::CompoundKind::Castle || compound.kind==generator::CompoundKind::Manor || compound.kind==generator::CompoundKind::Monastery) ? 0u : std::max(4u,brief.floors);
      if(compound.fantasyGothic) {
        dwellingSettings.roofPitch=1.35;dwellingSettings.roofRidgeMetres=wing.role=="keep"?12.:8.;
        dwellingSettings.dormerChance=0;
        dwellingSettings.outerWallMetres=.8;
        dwellingSettings.capacityCellMetres=2.4;
        dwellingSettings.doorWidthMetres=1.6;
        dwellingSettings.minimumCorridorMetres=2.4+dwellingSettings.innerWallMetres;
      }
      generator::CellHouse house;
      generator::Ledger ledger;
      DwellingPlan plan;
      std::string why;
      if (!generator::planCellDwelling(brief,dwellingSettings,house,ledger,&why) ||
          !generator::realiseCellDwelling(house,dwellingSettings,plan,&why))
        return fail("compound "+std::to_string(compound.id)+" "+wing.role+": "+why);
      if(compound.fantasyGothic || compound.gothicFacades) {
        plan.facade.roofCrests=true;
        plan.facade.style=FacadeStyle::Gothic;plan.facade.timberFacade=false;
        plan.facade.masonryVariation=true;plan.facade.variationSeed=wing.brief.seed;
        plan.facade.stoneVaults=true;plan.facade.glazed=true;
        plan.facade.windowHeight=2.2;plan.facade.windowWidth=1.15;
      }
      realisedWings.push_back({&compound,&wing,std::move(house),std::move(plan)});
    }

  // Compound links add openings after cell planning. Keep their thresholds
  // and level approach clear of stair footprints on either connected floor.
  const auto stairClearThreshold=[](const DwellingPlan &plan,uint32_t floor,Position p) {
    for(const auto &flight:plan.stairs) {
      if(flight.external || (flight.lowerFloor!=floor && flight.lowerFloor+1!=floor))continue;
      for(const auto *ring:{&flight.opening,&flight.lowerLanding,&flight.upperLanding}) {
        if(ring->size()<3)continue;
        bool inside=false;
        for(size_t i=0,j=ring->size()-1;i<ring->size();j=i++) {
          const auto a=(*ring)[i],b=(*ring)[j];
          if(segmentDistance(p,a,b)<1.8)return false;
          if((a.z>p.z)!=(b.z>p.z) && p.x<(b.x-a.x)*(p.z-a.z)/(b.z-a.z)+a.x)inside=!inside;
        }
        if(inside)return false;
      }
    }
    return true;
  };

  // A requested wall-level route must land on a real occupied storey. The
  // third/fourth 4.2m floors align with the 12.6/16.8m curtain walks; only the
  // 5cm finished-floor allowance remains, spread over the bridge as a slope.
  for(auto &realised:realisedWings)for(const auto &request:realised.compound->wallAccess)if(request.wing==realised.wing->brief.id) {
    auto &plan=realised.plan;const double base=realised.wing->brief.foundation+finishedFloorOffset;
    size_t chosen=plan.walls.size();double best=1e100;Position target;
    std::vector<Position> accessPath;
    for(size_t i=0;i<plan.walls.size();++i) {
      const auto &wall=plan.walls[i];if(!wall.exterior || std::abs(base+wall.a.y-request.at.y)>.06)continue;
      const double dx=wall.b.x-wall.a.x,dz=wall.b.z-wall.a.z,len=std::hypot(dx,dz);if(len<1.8)continue;
      // The access request may meet a long merged facade away from its
      // midpoint. Project onto its usable door interval instead of rejecting
      // a valid perpendicular connection because the wall centre is distant.
      const double along=std::clamp(((request.at.x-wall.a.x)*dx+(request.at.z-wall.a.z)*dz)/len,.9,len-.9);
      const Position q{wall.a.x+dx*along/len,base+wall.a.y,wall.a.z+dz*along/len};
      if(!stairClearThreshold(plan,wall.floor,q))continue;
      const double distance=std::hypot(q.x-request.at.x,q.z-request.at.z);
      if(distance<.15 || distance>24 || distance>=best)continue;
      std::vector<Position> path{request.at};
      if(std::abs((q.x-request.at.x)*dx+(q.z-request.at.z)*dz)>len*distance*.4) {
        const double sign=((request.at.x-q.x)*(-dz)+(request.at.z-q.z)*dx)>=0?1.:-1.;
        const double approach=std::abs((request.at.x-q.x)*(-dz)+(request.at.z-q.z)*dx)/len;
        if(approach<1.2)continue;
        const double setback=std::min(2.4,approach*.5);
        Position landing{q.x-sign*dz/len*setback,q.y,q.z+sign*dx/len*setback};
        if(std::hypot(landing.x-request.at.x,landing.z-request.at.z)<1.8)continue;
        path.push_back(landing);
      }
      path.push_back(q);
      bool clear=true;
      for(const auto &other:plan.walls)if(other.exterior && other.floor==wall.floor && other.id!=wall.id)
        for(size_t segment=1;segment<path.size();++segment)
          if(gallerySeparation(path[segment-1],path[segment],other.a,other.b)<.8-1e-5)clear=false;
      // The bridge is a walk in the open air between this wing and the
      // curtain. It may not pass through another range, nor through a
      // tower: a route was accepted on the strength of this wing's own walls
      // alone, and the deck went through the roof of the range next door.
      const auto crosses=[&](Position a,Position b,const std::vector<Position> &ring,double margin) {
        if(ring.size()<3)return false;
        if(pointInside(a,ring) || pointInside(b,ring))return true;
        for(size_t i=0;i<ring.size();++i) {
          const auto c=ring[i],d=ring[(i+1)%ring.size()];
          const auto orient=[](Position p,Position q,Position r){return (q.x-p.x)*(r.z-p.z)-(q.z-p.z)*(r.x-p.x);};
          if(orient(a,b,c)*orient(a,b,d)<0 && orient(c,d,a)*orient(c,d,b)<0)return true;
          if(std::min({segmentDistance(a,c,d),segmentDistance(b,c,d),segmentDistance(c,a,b),segmentDistance(d,a,b)})<margin)return true;
        }
        return false;
      };
      for(size_t segment=1;segment<path.size() && clear;++segment) {
        for(const auto &other:realised.compound->wings)
          if(other.brief.id!=realised.wing->brief.id && crosses(path[segment-1],path[segment],other.brief.shell,.9))clear=false;
        for(const auto &tower:realised.compound->towers)
          if(crosses(path[segment-1],path[segment],tower.footprint,.6))clear=false;
      }
      if(!clear)continue;
      chosen=i;best=distance;target=q;accessPath=std::move(path);
    }
    if(chosen==plan.walls.size()) {
      double levelError=1e100,longest=0,nearest=1e100;
      for(const auto &wall:plan.walls)if(wall.exterior) {
        levelError=std::min(levelError,std::abs(base+wall.a.y-request.at.y));
        if(std::abs(base+wall.a.y-request.at.y)<=.06) {
          longest=std::max(longest,std::hypot(wall.b.x-wall.a.x,wall.b.z-wall.a.z));
          nearest=std::min(nearest,segmentDistance(request.at,wall.a,wall.b));
        }
      }
      return fail("no occupied storey meets castle wall access for wing "+std::to_string(request.wing)+
                  " (base="+std::to_string(base)+", requested="+std::to_string(request.at.y)+
                  ", floor error="+std::to_string(levelError)+", longest wall="+std::to_string(longest)+
                  ", nearest="+std::to_string(nearest)+")");
    }
    const auto wall=plan.walls[chosen];const double len=std::hypot(wall.b.x-wall.a.x,wall.b.z-wall.a.z),wx=(wall.b.x-wall.a.x)/len,wz=(wall.b.z-wall.a.z)/len;
    plan.openings.erase(std::remove_if(plan.openings.begin(),plan.openings.end(),[&](const auto &o){return o.wall==wall.id;}),plan.openings.end());
    plan.doors.erase(std::remove_if(plan.doors.begin(),plan.doors.end(),[&](const auto &d){return d.wall==wall.id;}),plan.doors.end());
    DwellingOpening opening;opening.wall=wall.id;opening.floor=wall.floor;opening.exterior=true;opening.kind=OpeningKind::Door;
    const auto approach=accessPath[accessPath.size()-2];
    const double approachLength=std::hypot(target.x-approach.x,target.z-approach.z);
    const double normalCosine=std::abs((target.x-approach.x)*wz-(target.z-approach.z)*wx)/approachLength;
    const double doorHalf=.8/normalCosine;
    opening.a={target.x-wx*doorHalf,wall.a.y,target.z-wz*doorHalf};opening.b={target.x+wx*doorHalf,wall.a.y,target.z+wz*doorHalf};
    opening.base=0;opening.top=2.6;opening.head=2.4;opening.thickness=wall.thickness;plan.openings.push_back(opening);
    out.wallAccessLinks.push_back({request.wing,request.at,target,1.6,wall.floor,accessPath});
    // A bent access has a continuous landing and side rails, with no rail
    // across either exit. The final segment meets the actual door normally.
    std::vector<Position> offsets(accessPath.size());
    for(size_t k=0;k<accessPath.size();++k) {
      const auto normal=[&](size_t segment) {
        const auto a=accessPath[segment],b=accessPath[segment+1];
        const double length=std::hypot(b.x-a.x,b.z-a.z);
        return Position{-(b.z-a.z)/length,0,(b.x-a.x)/length};
      };
      if(!k)offsets[k]=normal(0);
      else if(k+1==accessPath.size())offsets[k]=normal(k-1);
      else {
        const auto a=normal(k-1),b=normal(k);const double denominator=1+a.x*b.x+a.z*b.z;
        offsets[k]={(a.x+b.x)/denominator,0,(a.z+b.z)/denominator};
      }
    }
    // The bridge is a structure, not a surface: a deck slab a quarter of a
    // metre thick with a parapet of masonry down each side, standing on
    // piers from the court. It used to be one quad with two skirts and a
    // zero-thickness rail, which read as a sheet of paper laid across the
    // yard. The clear walk between the parapets is still 1.6 m.
    const double clearHalf=.8,parapet=.25,deckDepth=.25,parapetHeight=1.05;
    const auto side=[&](size_t k,double offset,double height) {
      const auto p=accessPath[k],n=offsets[k];return Position{p.x+n.x*offset,p.y+height,p.z+n.z*offset};
    };
    // A box over four plan corners carrying their own heights: top, bottom a
    // depth below, and four sides, each wound to look out of the box.
    const auto box=[&](std::array<Position,4> top,double depth,uint32_t wallMaterial,uint32_t topMaterial) {
      std::array<Position,4> bottom=top;for(auto &p:bottom)p.y-=depth;
      Position centre{};for(size_t k=0;k<4;++k){centre.x+=(top[k].x+bottom[k].x)*.125;centre.y+=(top[k].y+bottom[k].y)*.125;centre.z+=(top[k].z+bottom[k].z)*.125;}
      const auto face=[&](std::array<Position,4> f,uint32_t material) {
        const Position u{f[1].x-f[0].x,f[1].y-f[0].y,f[1].z-f[0].z},v{f[2].x-f[0].x,f[2].y-f[0].y,f[2].z-f[0].z};
        const Position n{u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
        Position mean{};for(auto p:f){mean.x+=p.x*.25;mean.y+=p.y*.25;mean.z+=p.z*.25;}
        if(n.x*(mean.x-centre.x)+n.y*(mean.y-centre.y)+n.z*(mean.z-centre.z)<0)std::swap(f[1],f[3]);
        triangle(out,index,{f[0],f[1],f[2]},material);triangle(out,index,{f[0],f[2],f[3]},material);
      };
      face(top,topMaterial);face(bottom,wallMaterial);
      for(size_t k=0;k<4;++k){const size_t q=(k+1)%4;face({top[k],top[q],bottom[q],bottom[k]},wallMaterial);}
    };
    double walked=0,nextPier=2.4;
    for(size_t k=1;k<accessPath.size();++k) {
      box({side(k-1,-clearHalf-parapet,0),side(k,-clearHalf-parapet,0),side(k,clearHalf+parapet,0),side(k-1,clearHalf+parapet,0)},deckDepth,36,37);
      for(double sign:{-1.,1.}) {
        // No parapet across the curtain walk's own gap: the first two metres
        // from the wall are open, so the walk is entered from either side.
        double from=0;
        const double length=std::hypot(accessPath[k].x-accessPath[k-1].x,accessPath[k].z-accessPath[k-1].z);
        if(k==1 && !(realised.compound->wallAttachedKeep && realised.wing->role=="keep"))from=std::min(1.,1.9/length);
        if(from>=1.)continue;
        const auto along=[&](double offset,double t){const auto a=side(k-1,offset,parapetHeight),b=side(k,offset,parapetHeight);
          return Position{a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t};};
        const double inner=sign*clearHalf,outer=sign*(clearHalf+parapet);
        box({along(inner,from),along(inner,1),along(outer,1),along(outer,from)},parapetHeight,36,37);
      }
      // Piers under the deck from the court, every two and a half metres,
      // never within a metre of either end, where the curtain and the wing's
      // wall carry it.
      const double length=std::hypot(accessPath[k].x-accessPath[k-1].x,accessPath[k].z-accessPath[k-1].z);
      const double ux=(accessPath[k].x-accessPath[k-1].x)/length,uz=(accessPath[k].z-accessPath[k-1].z)/length;
      const double total=[&]{double sum=0;for(size_t s=1;s<accessPath.size();++s)sum+=std::hypot(accessPath[s].x-accessPath[s-1].x,accessPath[s].z-accessPath[s-1].z);return sum;}();
      while(nextPier<walked+length && nextPier<total-1.2) {
        const double t=nextPier-walked;
        const Position at{accessPath[k-1].x+ux*t,accessPath[k-1].y+(accessPath[k].y-accessPath[k-1].y)*t/length,accessPath[k-1].z+uz*t};
        const double half=.4;
        prism(out,index,{{at.x-ux*half+uz*half,0,at.z-uz*half-ux*half},{at.x+ux*half+uz*half,0,at.z+uz*half-ux*half},
          {at.x+ux*half-uz*half,0,at.z+uz*half+ux*half},{at.x-ux*half-uz*half,0,at.z-uz*half+ux*half}},
          realised.wing->brief.foundation,at.y-deckDepth,36,37);
        nextPier+=2.5;
      }
      walked+=length;
    }
  }

  // Connect the realised footprints, not the allocation rectangles. Door
  // apertures are authored before the skeleton, so both ends are real cuts.
  for(const auto &compound:compounds)if(compound.fantasyGothic) {
    std::vector<size_t> members;for(size_t i=0;i<realisedWings.size();++i)if(realisedWings[i].compound==&compound)members.push_back(i);

    struct Candidate {size_t a,b,wa,wb;Position p,q;double distance;};
    std::vector<Candidate> candidates;
    const auto inside=[](Position p,const std::vector<Position> &ring) {
      bool in=false;for(size_t i=0,j=ring.size()-1;i<ring.size();j=i++) {
        const auto a=ring[i],b=ring[j];if((a.z>p.z)!=(b.z>p.z) && p.x<(b.x-a.x)*(p.z-a.z)/(b.z-a.z)+a.x)in=!in;
      }return in;
    };
    const auto groundRing=[](const DwellingPlan &p)->const std::vector<Position>& {
      for(const auto &storey:p.storeys)if(storey.base>=-.01 && storey.top>0)return storey.outer;
      return p.storeys.front().outer;
    };
    for(size_t i=0;i<members.size();++i)for(size_t j=i+1;j<members.size();++j) {
      if(std::abs(realisedWings[members[i]].wing->brief.foundation-realisedWings[members[j]].wing->brief.foundation)>.01)continue;
      auto &a=realisedWings[members[i]].plan,&b=realisedWings[members[j]].plan;
      Candidate best{members[i],members[j],0,0,{},{},1e100};
      for(size_t ai=0;ai<a.walls.size();++ai)if(a.walls[ai].exterior && std::abs(a.walls[ai].a.y)<.01) {
        const auto &aw=a.walls[ai];
        const double alen=std::hypot(aw.b.x-aw.a.x,aw.b.z-aw.a.z);if(alen<1.8)continue;
        // Slide the 1.6m doorway inside its wall bay when the centreline
        // clips a courtyard return. Preserve a 5cm jamb at either end.
        for(double shiftA:{0.,-.35,.35}) {
        const double ta=.5+std::clamp(shiftA,-(alen-1.7)*.5,(alen-1.7)*.5)/alen;
        const Position p{aw.a.x+(aw.b.x-aw.a.x)*ta,0,aw.a.z+(aw.b.z-aw.a.z)*ta};
        for(size_t bi=0;bi<b.walls.size();++bi)if(b.walls[bi].exterior && std::abs(b.walls[bi].a.y)<.01) {
          const auto &bw=b.walls[bi];const double blen=std::hypot(bw.b.x-bw.a.x,bw.b.z-bw.a.z);if(blen<1.8)continue;
          for(double shiftB:{0.,-.35,.35}) {
          const double tb=.5+std::clamp(shiftB,-(blen-1.7)*.5,(blen-1.7)*.5)/blen;
          const Position q{bw.a.x+(bw.b.x-bw.a.x)*tb,0,bw.a.z+(bw.b.z-bw.a.z)*tb};
          if(!stairClearThreshold(a,aw.floor,p) || !stairClearThreshold(b,bw.floor,q))continue;
          const double distance=std::hypot(q.x-p.x,q.z-p.z);
          if(distance>=best.distance || distance<.2 || std::hypot(bw.b.x-bw.a.x,bw.b.z-bw.a.z)<1.8)continue;
          // The gallery approaches both thresholds close to their normals.
          const double ax=aw.b.x-aw.a.x,az=aw.b.z-aw.a.z,bx=bw.b.x-bw.a.x,bz=bw.b.z-bw.a.z;
          if(std::abs((q.x-p.x)*ax+(q.z-p.z)*az)>distance*std::hypot(ax,az)*.15 ||
             std::abs((q.x-p.x)*bx+(q.z-p.z)*bz)>distance*std::hypot(bx,bz)*.15)continue;
          bool clear=true;
          for(double t=std::min(.45,.65/distance);t<1.-std::min(.45,.65/distance) && clear;t+=.03) {
            for(double side:{-1.45,0.,1.45}) {
              const Position probe{p.x+(q.x-p.x)*t-(q.z-p.z)/distance*side,0,p.z+(q.z-p.z)*t+(q.x-p.x)/distance*side};
              for(const auto m:members) {
                // The terminal canopy bonds into its own facade. Its side
                // overhang is allowed there, but not along the open gallery.
                if(side!=0 && ((m==members[i] && t*distance<1.6) || (m==members[j] && (1-t)*distance<1.6)))continue;
                if(inside(probe,groundRing(realisedWings[m].plan))){clear=false;break;}
              }
              for(const auto &tower:compound.towers)if(inside(probe,tower.footprint)){clear=false;break;}
              // A canopy may overhang planting; its walking aisle must stay clear.
              if(side==0)for(const auto &garden:compound.gardens)if(inside(probe,garden.footprint)){clear=false;break;}
            }
          }
          if(clear)best={members[i],members[j],ai,bi,p,q,distance};
          }
        }
        }
      }
      if(best.distance<80)candidates.push_back(best);
    }
    std::fprintf(stderr,"compound connections: %zu wings, %zu viable pairs\n",members.size(),candidates.size());
    std::sort(candidates.begin(),candidates.end(),[](const auto &a,const auto &b){return a.distance<b.distance;});
    std::vector<size_t> parent(realisedWings.size());for(size_t i=0;i<parent.size();++i)parent[i]=i;
    const auto root=[&](size_t n){while(parent[n]!=n)n=parent[n];return n;};
    const auto cut=[&](size_t member,size_t w,Position p) {
      auto &plan=realisedWings[member].plan;const auto &wall=plan.walls[w];
      const double len=std::hypot(wall.b.x-wall.a.x,wall.b.z-wall.a.z),ux=(wall.b.x-wall.a.x)/len,uz=(wall.b.z-wall.a.z)/len;
      plan.openings.erase(std::remove_if(plan.openings.begin(),plan.openings.end(),[&](const auto &opening){return opening.wall==wall.id;}),plan.openings.end());
      DwellingOpening opening;opening.wall=wall.id;opening.floor=wall.floor;opening.exterior=true;opening.kind=OpeningKind::Door;
      opening.a={p.x-ux*.8,0,p.z-uz*.8};opening.b={p.x+ux*.8,0,p.z+uz*.8};
      opening.base=0;opening.top=2.6;opening.head=2.4;opening.thickness=wall.thickness;plan.openings.push_back(opening);
      plan.doors.erase(std::remove_if(plan.doors.begin(),plan.doors.end(),[&](const auto &d){return d.wall==wall.id;}),plan.doors.end());
    };
    for(const auto &link:candidates)if(root(link.a)!=root(link.b)) {
      // A canopy is 2.8m wide. Reject crossing/parallel galleries whose
      // arcades would occupy the same space before cutting either doorway.
      bool overlaps=false;
      for(const auto &existing:out.buildingLinks)
        if(gallerySeparation(link.p,link.q,existing.a,existing.b)<2.9)overlaps=true;
      if(overlaps)continue;
      parent[root(link.a)]=root(link.b);cut(link.a,link.wa,link.p);cut(link.b,link.wb,link.q);
      const auto &a=realisedWings[link.a],&b=realisedWings[link.b];
      const double base=a.wing->brief.foundation+finishedFloorOffset;
      Position p=link.p,q=link.q;p.y=q.y=base;
      out.buildingLinks.push_back({a.wing->brief.id,b.wing->brief.id,p,q,1.6});
      const double ux=(q.x-p.x)/link.distance,uz=(q.z-p.z)/link.distance,nx=-uz,nz=ux;
      const auto rectangle=[&](double t0,double t1,double half) {return std::vector<Position>{
        {p.x+ux*t0-nx*half,0,p.z+uz*t0-nz*half},{p.x+ux*t1-nx*half,0,p.z+uz*t1-nz*half},
        {p.x+ux*t1+nx*half,0,p.z+uz*t1+nz*half},{p.x+ux*t0+nx*half,0,p.z+uz*t0+nz*half}};};
      prism(out,index,rectangle(-.05,link.distance+.05,1.2),base-.15,base,36,37);
      prism(out,index,rectangle(0,link.distance,1.4),base+3.75,base+4.05,36,37);
      // Side arcades leave the full central aisle open. Capitals and arch
      // rings stay over the side piers, never across either entrance.
      if(link.distance>3.4) {
        const double first=.9,last=link.distance-.9;
        const int bays=std::max(1,int(std::ceil((last-first)/3.8)));
        const double span=(last-first)/bays;
        const auto point=[&](double along,double side,double y){return Position{p.x+ux*along+nx*side,base+y,p.z+uz*along+nz*side};};
        const auto pier=[&](double along,double side,double half,double low,double high){
          const auto a=point(along-half,side-half,0),b=point(along+half,side-half,0),c=point(along+half,side+half,0),d=point(along-half,side+half,0);
          prism(out,index,{a,b,c,d},base+low,base+high,36,37);
        };
        const auto quad=[&](Position a,Position b,Position c,Position d){triangle(out,index,{a,b,c},36);triangle(out,index,{a,c,d},36);};
        const bool pointed=(a.wing->brief.seed+b.wing->brief.seed)%2==0;
        for(double side:{-1.05,1.05}) {
          for(int bay=0;bay<=bays;++bay) {
            const double at=first+bay*span;
            pier(at,side,.18,0,.22);pier(at,side,.115,.22,2.35);pier(at,side,.18,2.35,2.52);
          }
          for(int bay=0;bay<bays;++bay)for(int k=0;k<16;++k) {
            const auto rise=[&](double t){return pointed?(std::sqrt(std::max(0.,1.-std::pow(std::max(t,1.-t),2))))/(std::sqrt(3.)*.5):std::sqrt(std::max(0.,1.-std::pow(2*t-1,2)));};
            const double t0=k/16.,t1=(k+1)/16.,x0=first+bay*span+t0*span,x1=first+bay*span+t1*span;
            const double y0=2.48+1.11*rise(t0),y1=2.48+1.11*rise(t1);
            const Position v[]={point(x0,side-.12,y0),point(x1,side-.12,y1),point(x1,side-.12,y1+.16),point(x0,side-.12,y0+.16),point(x0,side+.12,y0),point(x1,side+.12,y1),point(x1,side+.12,y1+.16),point(x0,side+.12,y0+.16)};
            quad(v[0],v[3],v[2],v[1]);quad(v[4],v[5],v[6],v[7]);quad(v[0],v[1],v[5],v[4]);quad(v[3],v[7],v[6],v[2]);quad(v[0],v[4],v[7],v[3]);quad(v[1],v[2],v[6],v[5]);
          }
        }
      }
    }
  }

  for(auto &realised:realisedWings) {
      const auto &compound=*realised.compound;const auto &wing=*realised.wing;auto &plan=realised.plan;
      std::string why;
      hierarchy::Skeleton skeleton=hierarchy::buildSkeleton(plan);
      hierarchy::Solid mesh;
      hierarchy::meshSkeleton(skeleton,mesh);
      // A legacy floor-only plan is nonempty too. Never accept it as a
      // completed building: its exposed plates look like crossing city faces.
      if (mesh.walls.empty() || mesh.slopes.empty())
        return fail("compound "+std::to_string(compound.id)+" "+wing.role+": structural mesh missing walls or roof");
      const size_t before=out.mesh.triangles.size();
      const double base=wing.brief.foundation+finishedFloorOffset;
      if(!compound.fantasyGothic && !plan.storeys.empty()) {
        double bottom=base;
        for(auto p:plan.storeys.front().outer)bottom=std::min(bottom,ground(p.x,p.z)-.3);
        // Structural stone stops below the finished interior floor.
        prism(out,index,plan.storeys.front().outer,bottom,base-.02,54,37);
      }
      const size_t wingMeshStart=out.mesh.triangles.size();
      if (!solid(out,index,mesh,base,40,&why)) return fail("compound triangulation: "+why);
      const size_t wingMeshEnd=out.mesh.triangles.size();
      if(compound.fantasyGothic)for(size_t t=before;t<out.surfaceMaterials.size();++t) {
        if(out.materials[t]==40)out.surfaceMaterials[t]=1;
        if(out.materials[t]==42)out.surfaceMaterials[t]=20;
      }
      if(compound.fantasyGothic) {
        const auto &ring=plan.storeys.front().outer;
        for(size_t edge=0;edge<ring.size();++edge) {
          const auto a=ring[edge],b=ring[(edge+1)%ring.size()];
          const double length=std::hypot(b.x-a.x,b.z-a.z);if(length<5)continue;
          const double ux=(b.x-a.x)/length,uz=(b.z-a.z)/length,nx=uz,nz=-ux;
          for(double at=3.2;at<length-2;at+=6.4) {
            const auto p=Position{a.x+ux*at,0,a.z+uz*at};
            bool doorway=false;for(const auto &opening:plan.openings)if(opening.kind==OpeningKind::Door && opening.exterior && opening.base<.1)
              doorway|=segmentDistance(p,opening.a,opening.b)<1.0;
            if(doorway)continue;
            const auto foot=[&](double depth){return std::vector<Position>{{p.x-ux*.5,0,p.z-uz*.5},{p.x+nx*depth-ux*.5,0,p.z+nz*depth-uz*.5},{p.x+nx*depth+ux*.5,0,p.z+nz*depth+uz*.5},{p.x+ux*.5,0,p.z+uz*.5}};};
            const double height=wallButtressHeight(plan,p,ux,uz);
            if(height<=0)continue;
            prism(out,index,foot(1.8),base,base+height*.3,36,37);
            prism(out,index,foot(1.2),base+height*.3,base+height*.65,36,37);
            prism(out,index,foot(.65),base+height*.65,base+height,36,37);
          }
        }
      }
      out.compoundTriangles += uint32_t(out.mesh.triangles.size()-before);
      CityModelBuildingKind kind = (compound.kind == generator::CompoundKind::Church || compound.kind==generator::CompoundKind::Monastery) ? CityModelBuildingKind::Religious
                                   : compound.kind == generator::CompoundKind::Manor ? CityModelBuildingKind::Manor
                                   : compound.kind == generator::CompoundKind::Hall ? CityModelBuildingKind::Civic
                                   : wing.role == "keep" ? CityModelBuildingKind::Keep : CityModelBuildingKind::CastleRoom;
      // Castle compound semantics settle every range except the freestanding
      // keep on the curtain. Geometric proximity remains a validation aid,
      // but the relation is authored here so later mesh detail does not erase
      // the structural attachment when a wall lining or tolerance is added.
      bool attached=compound.kind == generator::CompoundKind::Castle && wing.role != "keep";
      if (compound.kind == generator::CompoundKind::Castle && wing.brief.shell.size()>=3)
        for (auto p : wing.brief.shell) for (const auto &w : compound.walls)
          if (!w.gate && segmentDistance(p,w.a,w.b)<.6) attached=true;
      out.structuralPieces+=uint32_t(skeleton.pieces.size()); out.structuralContacts+=uint32_t(skeleton.contacts.size());
      out.structuralRooms+=uint32_t(skeleton.rooms.size()); out.structuralRoomConnections+=uint32_t(skeleton.roomConnections.size());
      auto footprint=plan.storeys.front().outer;
      for (auto &p:footprint) p.y+=base;
      const double wallHeight=plan.storeys.back().top;
      double roofHeight=0;
      for (const auto &r:plan.roofs) for (auto p:r.polygon) roofHeight=std::max(roofHeight,p.y-wallHeight);
      CityModelBuilding building{wing.brief.id,compound.id,kind,footprint,base,wallHeight,roofHeight,true,attached};
      building.pieces=uint32_t(skeleton.pieces.size()); building.contacts=uint32_t(skeleton.contacts.size()); building.rooms=uint32_t(skeleton.rooms.size()); building.roomConnections=uint32_t(skeleton.roomConnections.size());
      building.windows=uint32_t(std::count_if(plan.openings.begin(),plan.openings.end(),[](const auto &o){return o.exterior && o.kind==OpeningKind::Window;}));
      building.interiorViews=interiorViews(plan,base);
      for(const auto &room:plan.rooms) {
        building.roomList.push_back({room.id,room.floor,"",roomKindName(room.kind),room.boundary,room.usableArea});
        for(auto &p:building.roomList.back().boundary)p.y+=base;
      }
      for(const auto &door:plan.openings)if(door.exterior && door.kind==OpeningKind::Door && std::abs(door.a.y)<.01)
        building.exteriorDoors.push_back({(door.a.x+door.b.x)*.5,base,(door.a.z+door.b.z)*.5});
      for(const auto &room:plan.rooms){building.chapels+=room.kind==RoomKind::SideChapel;building.chancels+=room.kind==RoomKind::Chancel;}
      if (settings.voxelAssemblySink || settings.voxelBoundarySink) {
        hierarchy::DwellingVoxelAssembly assembly;
        std::string assemblyError;
        if (!hierarchy::buildDwellingVoxelAssembly(realised.house,plan,skeleton,wing.brief.id,
                                                   wing.brief.seed,assembly,&assemblyError))
          return fail("compound wing "+std::to_string(wing.brief.id)+" voxel assembly: "+assemblyError);
        assembly.grid.baseMetres=base;
        assembly.legacyTriangleFirst=wingMeshStart;
        assembly.legacyTriangleCount=wingMeshEnd-wingMeshStart;
        if (settings.voxelAssemblySink && !settings.voxelAssemblySink(assembly,skeleton,&assemblyError))
          return fail("compound wing "+std::to_string(wing.brief.id)+" voxel sink: "+assemblyError);
      }
      if (settings.voxelFoundationSink) {
        std::vector<CityVoxelFoundationSample> samples;
        std::string joinError;
        const std::function<double(double,double)> supportHeight =
          [&](double x,double z) { return ground(x,z); };
        if (!sampleCityVoxelFoundationJoin(footprint,base-.03,supportHeight,samples,&joinError) ||
            !settings.voxelFoundationSink(wing.brief.id,samples,&joinError))
          return fail("compound wing "+std::to_string(wing.brief.id)+" voxel foundation join: "+joinError);
      }
      if (!emitUniqueBuildingPart(wing.brief.id,kind,"wing-foundation",before,wingMeshStart) ||
          !emitUniqueBuildingPart(wing.brief.id,kind,"wing-buttress",wingMeshEnd)) return false;
      out.buildings.push_back(std::move(building));
    }

  if (settings.voxelBoundarySink) {
    std::set<uint64_t> accepted;
    for (const auto &building : out.buildings) accepted.insert(building.id);
    CityVoxelBoundaryPlan boundary;
    std::string boundaryError;
    if (!buildCityVoxelBoundaryPlan(out.surfaces,voxelPartyLines,accepted,boundary,&boundaryError))
      return fail("city voxel boundary plan: "+boundaryError);
    if (!resolveCityVoxelPartyContacts(boundary,voxelPartyFacets,&boundaryError))
      return fail("city voxel party contacts: "+boundaryError);
    const auto addFeature = [&](CityVoxelFeatureKind kind,CityVoxelFallback fallback,
                                uint64_t owner,uint32_t source,const std::vector<Position> &ring) {
      CityVoxelFeature feature;
      if (!cityVoxelFeature(kind,fallback,owner,source,ring,feature)) return false;
      addCityVoxelBoundaryFeature(boundary,feature);
      return true;
    };
    for (uint32_t i=0;i<out.buildings.size();++i)
      if (out.buildings[i].footprint.size()>=3 &&
          !addFeature(CityVoxelFeatureKind::Foundation,CityVoxelFallback::SolidMesh,
                      out.buildings[i].id,i,out.buildings[i].footprint))
        return fail("invalid voxel foundation bounds");
    if (relief) for (uint32_t i=0;i<relief->platforms.size();++i)
      if (relief->platforms[i].boundary.size()>=3 &&
          !addFeature(CityVoxelFeatureKind::RetainingWall,CityVoxelFallback::SolidMesh,
                      relief->platforms[i].block,i,relief->platforms[i].boundary))
        return fail("invalid voxel retaining wall bounds");
    for (uint32_t i=0;i<city.waterways.size();++i) {
      const auto &water=city.waterways[i];
      const double length=std::hypot(water.b.x-water.a.x,water.b.z-water.a.z);
      if (length<1e-6) return fail("invalid voxel waterway axis");
      const double nx=-(water.b.z-water.a.z)/length,nz=(water.b.x-water.a.x)/length;
      const auto ribbon=[&](double halfWidth) {
        return std::vector<Position>{{water.a.x+nx*halfWidth,0,water.a.z+nz*halfWidth},
                                     {water.b.x+nx*halfWidth,0,water.b.z+nz*halfWidth},
                                     {water.b.x-nx*halfWidth,0,water.b.z-nz*halfWidth},
                                     {water.a.x-nx*halfWidth,0,water.a.z-nz*halfWidth}};
      };
      if (!addFeature(CityVoxelFeatureKind::Water,CityVoxelFallback::WaterPlane,
                      water.route,i,ribbon(water.halfWidth)) ||
          !addFeature(CityVoxelFeatureKind::WaterBank,CityVoxelFallback::SolidMesh,
                      water.route,i,ribbon(water.halfWidth+water.bankWidth)))
        return fail("invalid voxel water bank bounds");
    }
    if (!auditCityVoxelBoundaryPlan(boundary,&boundaryError))
      return fail("city voxel boundary audit: "+boundaryError);
    const std::function<double(double,double)> terrainHeight =
      [voxelTerrainGround](double x,double z) { return voxelTerrainGround(x,z); };
    if (!settings.voxelBoundarySink(boundary,out,city,relief,terrainHeight,&boundaryError))
      return fail("city voxel boundary sink: "+boundaryError);
  }
  if (out.mesh.triangles.size()!=out.materials.size() || out.mesh.triangles.size()!=out.mesh.expectedNormals.size()) return fail("city model mesh arrays diverged");
  if(basedHouses) {
    std::sort(platformHeights.begin(),platformHeights.end());
    const auto at=[&](const double f){return platformHeights.empty()?0.:platformHeights[size_t(f*double(platformHeights.size()-1))];};
    std::fprintf(stderr,"city relief: %u platforms built, height median %.2f p90 %.2f max %.2f m\n",basedHouses,at(.5),at(.9),at(1.));
  }
  if(warpedHouses||squareInSkewedLot) {
    std::sort(warpScales.begin(),warpScales.end());
    const double p10=warpScales.empty()?1.:warpScales[warpScales.size()/10];
    std::fprintf(stderr,"city relief: %u houses set onto the shape of their lot, %u left square; smallest dimension kept: worst %.2f, p10 %.2f of plan\n",
      warpedHouses,squareInSkewedLot,worstWarpScale,p10);
  }
  if(unreachablePlots) std::fprintf(stderr,"city relief: %u plots left empty, no doorway within reach of a street\n",unreachablePlots);
  if(pathsBuilt) std::fprintf(stderr,"city relief: %u ways to a door, %u treads, %u landings, %u too steep for their run\n",pathsBuilt,pathTreads,pathLandings,steepWays);
  if(city.plotContracts)for(const auto &c:city.plotContracts->plots) {
    auto built=std::find_if(out.buildings.begin(),out.buildings.end(),[&](const auto &b){return b.id==c.building;});
    if(built==out.buildings.end())return fail("accepted plot was not emitted");
  }
  if(!emitGround())return false;
  result=std::move(out);
  if (error) error->clear();
  return true;
}

void appendCityModelTile(const CityModelPlan &model, double minX, double minZ, double maxX, double maxZ,
                        SplineFaceMesh &mesh, std::vector<uint32_t> &materials,
                        std::vector<std::array<std::array<double,2>,3>> *triangleUVs,
                        std::vector<uint32_t> *surfaceMaterials,
                        std::vector<CityModelPlan::GroundWeights> *groundWeights)
{
  std::map<uint32_t,uint32_t> retained;
  for (size_t ti=0;ti<model.mesh.triangles.size();++ti)
  {
    const auto t=model.mesh.triangles[ti];
    const auto a=model.mesh.vertices[t[0]],b=model.mesh.vertices[t[1]],c=model.mesh.vertices[t[2]];
    const auto attributes=[&](const std::array<Position,3> &points) {
      if(surfaceMaterials)surfaceMaterials->push_back(ti<model.surfaceMaterials.size()?model.surfaceMaterials[ti]:0);
      if(!triangleUVs && !groundWeights)return;
      std::array<std::array<double,2>,3> mapped{};
      CityModelPlan::GroundWeights blended{};
      {
        const Position u{b.x-a.x,b.y-a.y,b.z-a.z},v{c.x-a.x,c.y-a.y,c.z-a.z};
        const double uu=u.x*u.x+u.y*u.y+u.z*u.z,uv=u.x*v.x+u.y*v.y+u.z*v.z,vv=v.x*v.x+v.y*v.y+v.z*v.z;
        const double determinant=uu*vv-uv*uv;
        for(size_t k=0;k<3;++k) {
          const Position q{points[k].x-a.x,points[k].y-a.y,points[k].z-a.z};
          const double qu=q.x*u.x+q.y*u.y+q.z*u.z,qv=q.x*v.x+q.y*v.y+q.z*v.z;
          const double s=determinant>1e-20?(qu*vv-qv*uv)/determinant:0;
          const double r=determinant>1e-20?(qv*uu-qu*uv)/determinant:0;
          if(ti<model.triangleUVs.size())for(size_t d=0;d<2;++d)mapped[k][d]=model.triangleUVs[ti][0][d]*(1-s-r)+model.triangleUVs[ti][1][d]*s+model.triangleUVs[ti][2][d]*r;
          if(ti<model.groundWeights.size())for(size_t d=0;d<3;++d)blended[k][d]=model.groundWeights[ti][0][d]*(1-s-r)+model.groundWeights[ti][1][d]*s+model.groundWeights[ti][2][d]*r;
        }
      }
      if(triangleUVs)triangleUVs->push_back(mapped);
      if(groundWeights)groundWeights->push_back(blended);
    };
    if (std::max({a.x,b.x,c.x})<minX || std::min({a.x,b.x,c.x})>maxX ||
        std::max({a.z,b.z,c.z})<minZ || std::min({a.z,b.z,c.z})>maxZ) continue;
    if (std::min({a.x,b.x,c.x})>=minX && std::max({a.x,b.x,c.x})<=maxX &&
        std::min({a.z,b.z,c.z})>=minZ && std::max({a.z,b.z,c.z})<=maxZ)
    {
      std::array<uint32_t,3> ids;
      for (size_t k=0;k<3;++k)
      {
        auto [it,added]=retained.emplace(t[k],uint32_t(mesh.vertices.size()));
        if (added) mesh.vertices.push_back(model.mesh.vertices[t[k]]);
        ids[k]=it->second;
      }
      mesh.triangles.push_back(ids);
      mesh.expectedNormals.push_back(model.mesh.expectedNormals[ti]);
      materials.push_back(model.materials[ti]);
      attributes({a,b,c});
      continue;
    }
    std::vector<Position> polygon{model.mesh.vertices[t[0]],model.mesh.vertices[t[1]],model.mesh.vertices[t[2]]};
    // Sutherland-Hodgman clips the original plane, including height. It works
    // for vertical walls as well as roofs and terrain-following road ribbons.
    for (unsigned side=0;side<4 && polygon.size()>=3;++side)
    {
      const bool xAxis=side<2;
      const bool lower=side==0 || side==2;
      const double bound=side==0 ? minX : side==1 ? maxX : side==2 ? minZ : maxZ;
      const auto coordinate=[&](Position p){return xAxis ? p.x : p.z;};
      std::vector<Position> clipped;
      for (size_t i=0;i<polygon.size();++i)
      {
        const auto a=polygon[i],b=polygon[(i+1)%polygon.size()];
        const double da=coordinate(a)-bound,db=coordinate(b)-bound;
        const bool inA=lower ? da>=0 : da<=0, inB=lower ? db>=0 : db<=0;
        if (inA) clipped.push_back(a);
        if (inA!=inB)
        {
          const double u=da/(da-db);
          Position p{a.x+(b.x-a.x)*u,a.y+(b.y-a.y)*u,a.z+(b.z-a.z)*u};
          if (xAxis) p.x=bound; else p.z=bound;
          clipped.push_back(p);
        }
      }
      polygon=std::move(clipped);
    }
    for (size_t k=1;k+1<polygon.size();++k)
    {
      const auto a=polygon[0],b=polygon[k],c=polygon[k+1];
      const Position n{(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y),
                       (b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z),
                       (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)};
      if (std::hypot(n.x,std::hypot(n.y,n.z))<=1e-10) continue;
      const auto offset=uint32_t(mesh.vertices.size());
      mesh.vertices.insert(mesh.vertices.end(),{a,b,c});
      mesh.triangles.push_back({offset,offset+1,offset+2});
      mesh.expectedNormals.push_back(model.mesh.expectedNormals[ti]);
      materials.push_back(model.materials[ti]);
      attributes({a,b,c});
    }
  }
}

const char *cityModelBuildingKindName(CityModelBuildingKind kind)
{
  static constexpr const char *names[]={"house","workshop","civic","religious","castle-room","keep","gatehouse","defence-tower","mansion","manor","inn","pub"};
  return names[size_t(kind)];
}
} // namespace procedural::world::rewrite
