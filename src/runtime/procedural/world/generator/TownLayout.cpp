#include "TownLayout.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "LotSplitter.hpp"
#include "RingLotPlanner.hpp"
#include "WardBuildings.hpp"
#include "procedural/geom2/Boolean.hpp"
#include "graphrewrite/runtime/WorkerPool.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <tuple>

namespace procedural::world::generator {

namespace {

using rewrite::CityLayout;
using rewrite::CityStreet;
using rewrite::CityStreetBlock;
using rewrite::CityWallTower;
using rewrite::DistrictUse;
using rewrite::SurfaceContour;
using rewrite::SurfaceRole;

double distance(Position a, Position b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
}

Position mix(Position a, Position b, double t)
{
  return {a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t};
}

ConvexRing ribbon(Position a, Position b, double halfWidth)
{
  const double l = distance(a, b), x = -(b.z - a.z) / l * halfWidth, z = (b.x - a.x) / l * halfWidth;
  return {{a.x - x, 0, a.z - z}, {b.x - x, 0, b.z - z}, {b.x + x, 0, b.z + z}, {a.x + x, 0, a.z + z}};
}

ConvexRing cutRing(const ConvexRing &ring, double nx, double nz, double offset)
{
  ConvexRing out;
  for (size_t i=0;i<ring.size();++i)
  {
    const auto a=ring[i],b=ring[(i+1)%ring.size()];
    const double da=nx*a.x+nz*a.z-offset,db=nx*b.x+nz*b.z-offset;
    if (da<=0) out.push_back(a);
    if ((da<0 && db>0)||(da>0 && db<0)) out.push_back(mix(a,b,da/(da-db)));
  }
  return out;
}

// Area alone admits unusable slivers. Test a street-aligned square, with
// space for the normal dwelling setback on each side.
bool fitsDwelling(const ConvexRing &ring)
{
  for (size_t i=0;i<ring.size();++i)
  {
    const auto a=ring[i],b=ring[(i+1)%ring.size()];
    const double len=distance(a,b);
    if (len<4.8) continue;
    const double ux=(b.x-a.x)/len,uz=(b.z-a.z)/len;
    auto centres=ring;
    for (size_t j=0;j<ring.size() && !centres.empty();++j)
    {
      const auto p=ring[j],q=ring[(j+1)%ring.size()];
      const double nx=q.z-p.z,nz=p.x-q.x;
      const double support=2.4*(std::abs(nx*ux+nz*uz)+std::abs(-nx*uz+nz*ux));
      centres=cutRing(centres,nx,nz,nx*p.x+nz*p.z-support);
    }
    if (centres.size()>=3) return true;
  }
  return false;
}

ConvexRing lotUnion(const ConvexRing &a,const ConvexRing &b)
{
  auto points=a; points.insert(points.end(),b.begin(),b.end());
  std::sort(points.begin(),points.end(),[](auto p,auto q) { return std::tie(p.x,p.z)<std::tie(q.x,q.z); });
  points.erase(std::unique(points.begin(),points.end(),[](auto p,auto q){return distance(p,q)<1e-6;}),points.end());
  if (points.size()<3) return {};
  const auto turn=[](auto p,auto q,auto r){return (q.x-p.x)*(r.z-p.z)-(q.z-p.z)*(r.x-p.x);};
  ConvexRing hull;
  for (auto p:points) { while(hull.size()>1 && turn(hull[hull.size()-2],hull.back(),p)<=0) hull.pop_back(); hull.push_back(p); }
  const size_t lower=hull.size();
  for (size_t i=points.size()-1;i-->0;) { auto p=points[i]; while(hull.size()>lower && turn(hull[hull.size()-2],hull.back(),p)<=0) hull.pop_back(); hull.push_back(p); }
  hull.pop_back();
  // Never fill an alley, courtyard or concavity with a convex envelope.
  if (std::abs(signedArea(hull)-signedArea(a)-signedArea(b))>1e-4)
  {
    // Preserve a concave union instead of filling its missing corner. Lots
    // sharing a complete opposing edge can be joined by deleting that seam.
    for (size_t i=0;i<a.size();++i) for (size_t j=0;j<b.size();++j)
      if (distance(a[i],b[(j+1)%b.size()])<1e-6 && distance(a[(i+1)%a.size()],b[j])<1e-6)
      {
        ConvexRing joined;
        for (size_t k=0;k<a.size();++k) joined.push_back(a[(i+1+k)%a.size()]);
        for (size_t k=1;k+1<b.size();++k) joined.push_back(b[(j+1+k)%b.size()]);
        bool simple=true;
        for (size_t u=0;u<joined.size();++u) for (size_t v=u+1;v<joined.size();++v)
        {
          const auto p=joined[u],q=joined[(u+1)%joined.size()],r=joined[v],s=joined[(v+1)%joined.size()];
          if (distance(p,r)<.002) simple=false;
          if (v==u+1 || (u==0 && v+1==joined.size())) continue;
          if (segmentDistance(p,r,s)<.002 || segmentDistance(q,r,s)<.002 ||
              segmentDistance(r,p,q)<.002 || segmentDistance(s,p,q)<.002 ||
              (turn(p,q,r)*turn(p,q,s)<0 && turn(r,s,p)*turn(r,s,q)<0)) simple=false;
        }
        if (simple && std::abs(signedArea(joined)-signedArea(a)-signedArea(b))<1e-4) return joined;
      }
    return {};
  }
  return hull;
}

// Watabou's ward class for a district use, scaled from his square units.
struct WardClass
{
  double minSq, gridChaos, sizeChaos, emptyProb;
};

WardClass wardClass(uint8_t use, bool outskirts, double unit, double r1, double r2, double r3)
{
  if (outskirts)
  {
    return {(10 + 50 * r1 * r2) * unit, .5 + .3 * r3, .7, .04}; // GateWard
  }

  switch (DistrictUse(use))
  {
  case DistrictUse::Suburb:
  case DistrictUse::WorkerHousing:
    return {(10 + 30 * r1 * r2) * unit, .6 + .4 * r3, .8, .03}; // Slum
  case DistrictUse::Merchant:
  case DistrictUse::Guild:
    return {(50 + 60 * r1 * r2) * unit, .5 + .3 * r3, .7, .15}; // MerchantWard
  case DistrictUse::WealthyResidential:
  case DistrictUse::Manor:
    return {(80 + 30 * r1 * r2) * unit, .5 + .3 * r3, .8, .2}; // PatriciateWard
  case DistrictUse::Civic:
    return {(80 + 30 * r1 * r2) * unit, .1 + .3 * r3, .3, .04}; // AdministrationWard
  default:
    return {(10 + 80 * r1 * r2) * unit, .5 + .2 * r3, .6, .04}; // CraftsmenWard
  }
}

} // namespace

TownLayout::TownLayout(uint64_t domain) : Generator("town.layout", domain, "town.layout-input", "city.layout", GenerationScope::Patch)
{
}

uint32_t TownLayout::packFrontageLots(std::vector<ConvexRing> &lots, const std::vector<CityStreet> &streets)
{
  const auto frontage = [&](Position a, Position b) {
    const double len=distance(a,b);
    if(len<3.) return false;
    const Position m{(a.x+b.x)*.5,0,(a.z+b.z)*.5};
    for(const auto &road:streets) {
      auto path=road.path;
      if(path.size()<2)path={road.a,road.b};
      for(size_t k=1;k<path.size();++k) {
        const auto u=path[k-1],v=path[k];const double run=distance(u,v);
        if(run>1e-6 && std::abs((b.x-a.x)*(v.z-u.z)-(b.z-a.z)*(v.x-u.x))/(len*run)<.3 &&
           segmentDistance(m,u,v)<=road.halfWidth+1.5) return true;
      }
    }
    return false;
  };
  const auto usable = [&](const ConvexRing &ring) {
    geom2::Polygon polygon;
    for(auto p:ring)polygon.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
    if(!geom2::counterClockwise(polygon.outer))geom2::reverse(polygon.outer);
    double best=0;
    for(size_t i=0;i<ring.size();++i) {
      auto a=ring[i],b=ring[(i+1)%ring.size()];
      if(!frontage(a,b))continue;
      const double len=distance(a,b);double u0,v0,u1,v1;
      if(geom2::largestInscribedRectangle(polygon,(b.x-a.x)/len,(b.z-a.z)/len,.6,u0,v0,u1,v1) &&
         std::min(u1-u0,v1-v0)>=6.6-1e-6)
        best=std::max(best,std::floor((u1-u0)/1.2+1e-6)*std::floor((v1-v0)/1.2+1e-6)*1.44);
    }
    return best;
  };
  const auto original=lots;
  std::vector<double> capacity;for(const auto &lot:original)capacity.push_back(usable(lot));
  std::vector<bool> consumed(original.size(),false);
  std::vector<ConvexRing> packed;
  uint32_t changes=0;
  for(size_t i=0;i<original.size();++i) {
    if(consumed[i])continue;
    ConvexRing region=original[i];size_t partner=original.size();
    const double area=std::abs(signedArea(region));
    // Repair skinny scraps with a touching neighbour. Exact union cannot
    // consume the public lane or courtyard between disconnected parcels.
    if(capacity[i]<area*.6) {
      double best=0;
      for(size_t j=i+1;j<original.size();++j)if(!consumed[j]) {
        auto joined=lotUnion(region,original[j]);
        if(joined.empty() || std::abs(signedArea(joined))>360.)continue;
        const double gain=usable(joined)-capacity[i]-capacity[j];
        if(gain>best+1.){best=gain;partner=j;}
      }
      if(partner<original.size())region=lotUnion(region,original[partner]);
    }
    const double combined=std::abs(signedArea(region));
    if(combined<150. && partner==original.size()){packed.push_back(region);continue;}
    LotSplitInput input;input.polygon=region;input.targetArea=100.;input.spread=.2;
    input.minimumWidth=6.6;input.minimumCorner=.6;input.frontageWeight=4.;
    input.chordsAreRoads=false;input.convexLots=true;input.key=i+1;
    input.random=[](uint64_t key,uint64_t lane){return double((key*1664525ull+lane*1013904223ull)%1000003)/1000003.;};
    for(size_t k=0;k<region.size();++k)
      input.edgeKinds.push_back(frontage(region[k],region[(k+1)%region.size()])?LotEdgeKind::Road:LotEdgeKind::None);
    LotSplitPlan split;Ledger ledger;std::string error;
    const double oldCapacity=capacity[i]+(partner<original.size()?capacity[partner]:0);
    const size_t oldCount=1+(partner<original.size());
    bool accept=LotSplitter::plan(input,split,ledger,&error) && !split.lots.empty();
    double newCapacity=0,newArea=0;
    for(const auto &lot:split.lots) {
      const double fitted=usable(lot.ring);
      accept &= fitted>=43.56-1e-6;
      newCapacity+=fitted;newArea+=std::abs(signedArea(lot.ring));
    }
    accept &= std::abs(newArea-combined)<.01 &&
      ((split.lots.size()>oldCount && newCapacity>=oldCapacity*.85) || newCapacity>oldCapacity*1.1+1.);
    if(accept) {
      for(auto &lot:split.lots)packed.push_back(std::move(lot.ring));
      if(partner<original.size())consumed[partner]=true;
      ++changes;
    } else packed.push_back(original[i]);
  }
  lots=std::move(packed);
  return changes;
}

bool TownLayout::plan(const TownLayoutInput &s, CityLayout &out, Ledger &ledger, std::string *error)
{
  auto fail = [&](const std::string &message) {
    ledger.add(LedgerKind::Rejected, 0, "town layout: " + message);

    if (error != nullptr)
    {
      *error = message;
    }

    return false;
  };

  if (s.patches.empty() || s.border.size() < 3 || s.gates.empty() || !s.random)
  {
    return fail("a town needs patches, a border, gates and a random source");
  }

  CityLayout r;
  r.revision = s.revision;
  r.wallSetback = s.walled ? s.wallSetback : 0;
  r.envelope.outer = s.border;
  r.districts = s.districts;
  r.reservations = s.compounds;

  // The central routing anchor is not an unbuildable hexagonal reservation.
  // Its commercial blocks are allocated below; explicit civic compounds keep
  // their own reservations and remain obstacles to ordinary buildings.

  // Gates are border vertices where at least two core patches meet (a street
  // can leave them inward); every gate snaps to the nearest such vertex.
  std::vector<TownPatch> patches = s.patches;
  std::vector<Position> gates;

  for (auto gate : s.gates)
  {
    Position best = gate;
    double nearest = 1e30;

    for (auto v : s.border)
    {
      unsigned core = 0;

      for (const auto &patch : patches)
      {
        if (!patch.withinWalls)
        {
          continue;
        }

        for (auto q : patch.ring)
        {
          if (distance(q, v) < 1e-3)
          {
            ++core;
            break;
          }
        }
      }

      const double d = distance(v, gate);

      if (core >= 2 && d < nearest)
      {
        nearest = d;
        best = v;
      }
    }

    gates.push_back(best);
  }

  // CurtainWall.buildGates: when only one outer patch touches a gate there is
  // no edge for a road to leave along, so that patch is split from the gate
  // to its farthest vertex in the outward direction.
  uint32_t splits = 0;

  for (auto gate : gates)
  {
    std::vector<size_t> touching;

    for (size_t i = 0; i < patches.size(); ++i)
    {
      if (patches[i].withinWalls)
      {
        continue;
      }

      for (auto q : patches[i].ring)
      {
        if (distance(q, gate) < 1e-3)
        {
          touching.push_back(i);
          break;
        }
      }
    }

    if (touching.size() != 1 || patches[touching[0]].ring.size() <= 3)
    {
      continue;
    }

    TownPatch outer = patches[touching[0]];
    const size_t n = outer.ring.size();
    size_t gateIndex = 0;

    for (size_t k = 0; k < n; ++k)
    {
      if (distance(outer.ring[k], gate) < 1e-3)
      {
        gateIndex = k;
      }
    }

    // Outward: away from the town centre through the gate.
    const double ox = gate.x - s.centre.x, oz = gate.z - s.centre.z, ol = std::max(1e-9, std::hypot(ox, oz));
    size_t farthest = n;
    double score = -2;

    for (size_t k = 0; k < n; ++k)
    {
      if (k == gateIndex || (k + 1) % n == gateIndex || (gateIndex + 1) % n == k || onRing(s.border, outer.ring[k], 1e-3))
      {
        continue;
      }

      const double dx = outer.ring[k].x - gate.x, dz = outer.ring[k].z - gate.z, dl = std::max(1e-9, std::hypot(dx, dz));
      const double along = (dx * ox + dz * oz) / (dl * ol);

      if (along > score)
      {
        score = along;
        farthest = k;
      }
    }

    if (farthest == n)
    {
      continue;
    }

    const size_t i1 = std::min(gateIndex, farthest), i2 = std::max(gateIndex, farthest);
    TownPatch first = outer, second = outer;
    first.ring.assign(outer.ring.begin() + i1, outer.ring.begin() + i2 + 1);
    second.ring.assign(outer.ring.begin() + i2, outer.ring.end());
    second.ring.insert(second.ring.end(), outer.ring.begin(), outer.ring.begin() + i1 + 1);
    first.id = outer.id * 16 + 1;
    second.id = outer.id * 16 + 2;

    if (first.ring.size() >= 3 && second.ring.size() >= 3 && signedArea(first.ring) > 1 && signedArea(second.ring) > 1)
    {
      patches[touching[0]] = first;
      patches.push_back(second);
      ++splits;
    }
  }

  if (splits)
  {
    ledger.add(LedgerKind::Repaired, 0, "town layout: " + std::to_string(splits) + " outer patches split at their gates for a road");
  }

  // Expose interior route choices BEFORE A*. These are planning patches,
  // not lots; child patches retain their original district ownership.
  std::map<uint64_t,uint64_t> patchSources;
  uint64_t nextPatch=1;
  for (const auto &p:patches) nextPatch=std::max(nextPatch,p.id+1);
  if (!patches.empty())
  {
    std::vector<TownPatch> refined;
    // Irregular, locally relaxed Voronoi cells provide the routing graph.
    // Repeated longest-edge bisection inherited two axes at every depth and
    // turned otherwise organic world cells into rectangular street grids.
    for (const auto &patch:patches)
    {
      Position centre{};double minX=1e30,minZ=1e30,maxX=-1e30,maxZ=-1e30;
      for(auto p:patch.ring){centre.x+=p.x/patch.ring.size();centre.z+=p.z/patch.ring.size();
        minX=std::min(minX,p.x);minZ=std::min(minZ,p.z);maxX=std::max(maxX,p.x);maxZ=std::max(maxZ,p.z);}
      bool reserved=patch.plaza;
      for(const auto &compound:s.compounds)reserved|=strictlyInsideConvex(compound,centre);
      const double target=patch.withinWalls ? 1300. : patch.outskirts ? 2400. : 4800.;
      const unsigned count=reserved ? 1 : std::clamp(unsigned(std::ceil(signedArea(patch.ring)/target)),1u,48u);
      if(count==1){patchSources[patch.id]=patch.id;refined.push_back(patch);continue;}
      std::vector<Position> sites;
      const double separation=std::sqrt(std::abs(signedArea(patch.ring))/count)*.38;
      for(unsigned attempt=0;sites.size()<count && attempt<2048;++attempt) {
        Position p{minX+(maxX-minX)*s.random(patch.id,810+attempt*2),0,
                   minZ+(maxZ-minZ)*s.random(patch.id,811+attempt*2)};
        if(!strictlyInsideConvex(patch.ring,p))continue;
        bool separated=true;for(auto q:sites)separated &= distance(p,q)>separation;
        if(separated)sites.push_back(p);
      }
      if(sites.size()<2){patchSources[patch.id]=patch.id;refined.push_back(patch);continue;}
      const auto cells=[&]() {
        std::vector<ConvexRing> result;
        for(size_t i=0;i<sites.size();++i) {
          auto ring=patch.ring;
          for(size_t j=0;j<sites.size() && ring.size()>=3;++j)if(i!=j) {
            const double length=distance(sites[i],sites[j]);
            const double nx=(sites[j].x-sites[i].x)/length,nz=(sites[j].z-sites[i].z)/length;
            ring=cutRing(ring,nx,nz,nx*(sites[i].x+sites[j].x)*.5+nz*(sites[i].z+sites[j].z)*.5);
          }
          result.push_back(std::move(ring));
        }
        return result;
      };
      // One partial Lloyd step reduces tiny wards without erasing the seed's
      // irregular junctions. Original world boundaries and gates stay fixed.
      auto initial=cells();
      for(size_t i=0;i<initial.size();++i)if(initial[i].size()>=3) {
        Position centroid{};for(auto p:initial[i]){centroid.x+=p.x/initial[i].size();centroid.z+=p.z/initial[i].size();}
        sites[i]=mix(sites[i],centroid,.65);
      }
      auto final=cells();bool valid=true;
      for(const auto &ring:final)valid &= ring.size()>=3 && signedArea(ring)>100;
      if(!valid){patchSources[patch.id]=patch.id;refined.push_back(patch);continue;}
      for(size_t i=0;i<final.size();++i) {
        auto child=patch;child.id=i ? nextPatch++ : patch.id;child.ring=std::move(final[i]);
        patchSources[child.id]=patch.id;refined.push_back(std::move(child));
      }
    }
    patches=std::move(refined);

    // Weld the refined lattice.
    //
    // A cell cut into children does not tell its neighbour, so the neighbour
    // carries one edge where the children carry two and the shared vertex
    // between them lies in the middle of the neighbour's edge. The street
    // relaxation pins exactly those vertices (it cannot move a vertex one of
    // its cells has never heard of), which on a refined lattice is most of
    // them. Splitting the neighbour's edge at the vertex is what the tiling
    // helper already does everywhere else.
    std::vector<ConvexRing> rings;

    for (const auto &patch : patches)
    {
      rings.push_back(patch.ring);
    }

    weldTiling(rings);

    for (size_t i = 0; i < patches.size() && i < rings.size(); ++i)
    {
      if (rings[i].size() >= 3)
      {
        patches[i].ring = rings[i];
      }
    }
  }

  // 1. Streets.
  TownStreetsInput streetInput;
  streetInput.patches = patches;
  streetInput.border = s.border;
  streetInput.gates = gates;
  streetInput.exits = s.exits;
  streetInput.centre = s.centre;
  streetInput.corridorHalfWidth = s.arteryHalfWidth;
  streetInput.terrain = s.terrain;
  streetInput.obstacles = s.compounds;
  // What may NOT move under the street relaxation: the wall ring and the
  // gates (the streets generator pins those itself) and the corners of a
  // reserved compound, whose building is already standing on them.
  //
  // Not every planning-cell boundary. Pinning those froze the whole lattice -
  // two vertices moved in a town of two hundred and sixty patches - and the
  // cell borders are exactly the lines the streets run along, so the streets
  // came out as angular as the Voronoi diagram underneath them. Watabou moves
  // every vertex a street passes through and lets the patches follow, which
  // is what makes his streets curve.
  for(const auto &compound:s.compounds)for(auto p:compound)streetInput.fixedVertices.push_back(p);

  // Where the other streets start.
  //
  // Watabou runs a street from every gate to the plaza and has eight to twelve
  // gates, so his core is criss-crossed; a settlement here has one gate per
  // rural road - two - and its core held two streets, which is what made the
  // interior read as one undivided blob of Voronoi cells. The anchors are the
  // wall vertices between two core patches (a postern's line, without opening
  // the wall) spaced round the ring, and the corner of every reserved
  // compound, which is a building people walk to.
  {
    double perimeter = 0;

    for (size_t i = 0; i < s.border.size(); ++i)
    {
      perimeter += distance(s.border[i], s.border[(i + 1) % s.border.size()]);
    }

    const double spacing = std::max(90., perimeter / 12.);
    double since = spacing;

    for (size_t i = 0; i < s.border.size(); ++i)
    {
      since += distance(s.border[(i + s.border.size() - 1) % s.border.size()], s.border[i]);

      bool isGate = false;

      for (auto g : gates)
      {
        isGate = isGate || distance(g, s.border[i]) < spacing * .5;
      }

      unsigned core = 0;

      for (const auto &patch : patches)
      {
        if (!patch.withinWalls)
        {
          continue;
        }

        for (auto q : patch.ring)
        {
          if (distance(q, s.border[i]) < 1e-3)
          {
            ++core;

            break;
          }
        }
      }

      if (isGate || core < 2 || since < spacing)
      {
        continue;
      }

      since = 0;
      streetInput.anchors.push_back(s.border[i]);
    }

    for (const auto &compound : s.compounds)
    {
      if (compound.size() < 3)
      {
        continue;
      }

      Position nearest = compound.front();

      for (auto p : compound)
      {
        if (distance(p, s.centre) < distance(nearest, s.centre))
        {
          nearest = p;
        }
      }

      streetInput.anchors.push_back(nearest);
    }
  }
  for(const auto &patch:patches)if(patch.plaza)for(auto p:patch.ring)streetInput.fixedVertices.push_back(p);
  TownStreetsPlan streets;

  const auto townTick=[]{return std::chrono::steady_clock::now();};
  const auto townSince=[](const std::chrono::steady_clock::time_point &from){
    return std::chrono::duration<double>(std::chrono::steady_clock::now()-from).count();};
  const bool townTiming=std::getenv("CITY_DEBUG")!=nullptr;
  auto townPhase=townTick();
  double streetsS=0,blocksS=0,lotsS=0,reserveS=0;
  double splitterS=0,ringS=0,wardS=0,mergeS=0;
  uint32_t splitterCalls=0,ringCalls=0,wardCalls=0;

  if (!TownStreets::plan(streetInput, streets, ledger, error))
  {
    return false;
  }

  // The smoothing moves shared vertices: patches, the border (the wall
  // shares its vertices with the patches, as Watabou's does) and the gates.
  for (auto &patch : patches)
  {
    TownStreets::remap(streets, patch.ring);
  }

  ConvexRing border = s.border;
  TownStreets::remap(streets, border);
  gates = streets.gates;
  TownStreets::remap(streets, gates);
  r.envelope.outer = border;

  uint64_t streetId = 1000;

  // Author artery bends now, before blocks exist. Keep every original graph
  // vertex as an anchor; tangent samples change the street envelope and the
  // neighbouring block cuts, not merely a renderer's line style.
  std::vector<std::vector<Position>> curvedArteries;
  for (const auto &artery:streets.arteries)
  {
    if (artery.size()<2) continue;
    // Terrain-aware routes have already been relaxed with grade checks.
    // Unchecked Bezier excursions can cross a cliff or reserved compound.
    if (s.terrain || !s.compounds.empty()) { curvedArteries.push_back(artery); continue; }
    std::vector<Position> curve{artery.front()};
    for (size_t i=1;i+1<artery.size();++i)
    {
      const auto a=artery[i-1],b=artery[i],c=artery[i+1];
      const double incoming=distance(a,b),outgoing=distance(b,c);
      const double reach=std::min({incoming*.3,outgoing*.3,s.arteryHalfWidth*3});
      if (reach<.1) { curve.push_back(b); continue; }
      // Two quadratic spans pass through the original junction, with one
      // shared tangent, preserving connections to side streets at that node.
      const Position p=mix(b,a,reach/incoming),q=mix(b,c,reach/outgoing);
      const double len=distance(a,c);
      if (len<1e-6) { curve.push_back(b); continue; }
      const Position left{b.x-(c.x-a.x)/len*reach*.45,0,b.z-(c.z-a.z)/len*reach*.45};
      const Position right{b.x+(c.x-a.x)/len*reach*.45,0,b.z+(c.z-a.z)/len*reach*.45};

      // A turn is as many facets as its size deserves.
      //
      // Twelve segments per half-bend is a curve drawn for a plotter, not for
      // a street: on a three metre bend that is a facet every 25 cm, and the
      // road ribbon, the block cuts and the lot reservations all carry that
      // detail downstream. One facet per `bendFacetMetres` of arc, between two
      // and twelve, keeps the shape of a long sweeping turn and lets a short
      // corner be a corner.
      const double arc=distance(p,b)+distance(b,q);
      const unsigned facets=std::clamp(unsigned(std::llround(arc/std::max(.5,s.bendFacetMetres))),2u,12u);

      for (unsigned k=0;k<=facets;++k) { double t=double(k)/facets; curve.push_back(mix(mix(p,left,t),mix(left,b,t),t)); }
      for (unsigned k=1;k<=facets;++k) { double t=double(k)/facets; curve.push_back(mix(mix(b,right,t),mix(right,q,t),t)); }
    }
    curve.push_back(artery.back());
    curvedArteries.push_back(std::move(curve));
  }

  for (const auto &artery : curvedArteries)
  {
    for (size_t i = 1; i < artery.size(); ++i)
    {
      if (distance(artery[i - 1], artery[i]) < 1e-6)
      {
        continue;
      }

      CityStreet street;
      street.id = streetId++;
      street.a = artery[i - 1];
      street.b = artery[i];
      street.halfWidth = s.arteryHalfWidth;
      street.arterial = true;
      street.path = {street.a, street.b};
      r.streets.push_back(std::move(street));
    }
  }

  // 2. Wall: the border's own edges are the sections (Watabou's wall is the
  // patch circumference), opened at the gates; towers stand on the border
  // vertices, thinned to about a section length apart, never on a gate.
  if (s.walled)
  {
    const auto &ring = border;
    const size_t n = ring.size();
    const double opening = s.arteryHalfWidth + 1.5 + s.wallTowerRadius;

    auto isGate = [&](Position v) {
      for (auto g : gates)
      {
        if (distance(g, v) < 1e-6)
        {
          return true;
        }
      }

      return false;
    };

    for (size_t i = 0; i < n; ++i)
    {
      Position a = ring[i], b = ring[(i + 1) % n];
      const double l = distance(a, b);

      if (l < 1e-6)
      {
        continue;
      }

      // A gate at either end opens the section by the road's width.
      if (isGate(ring[i]))
      {
        a = mix(a, b, std::min(.49, opening / l));
      }

      if (isGate(ring[(i + 1) % n]))
      {
        b = mix(ring[i], ring[(i + 1) % n], 1 - std::min(.49, opening / l));
      }

      if (distance(a, b) > 1e-6)
      {
        r.walls.push_back({streetId++, a, b, .7, false});
      }
    }

    double sinceTower = s.wallSectionMetres;

    for (size_t i = 0; i < n; ++i)
    {
      const Position v = ring[i];
      sinceTower += distance(ring[(i + n - 1) % n], v);

      if (isGate(v))
      {
        sinceTower = s.wallSectionMetres * .5; // The gate's flanks stand close.
        continue;
      }

      if (sinceTower < s.wallSectionMetres * .6)
      {
        continue;
      }

      sinceTower = 0;
      CityWallTower tower;
      tower.id = (uint64_t(1) << 63) | streetId++;
      tower.center = v;
      tower.height = s.wallTowerHeight;

      for (unsigned k = 0; k < 8; ++k)
      {
        const double angle = k * 3.14159265358979323846 / 4;
        tower.footprint.push_back({v.x + s.wallTowerRadius * std::cos(angle), 0, v.z + s.wallTowerRadius * std::sin(angle)});
      }

      r.towers.push_back(std::move(tower));
    }

    // Gate flanks: a tower on each side of every opening.
    for (const auto &wall : r.walls)
    {
      for (auto end : {wall.a, wall.b})
      {
        bool flank = false;

        for (auto g : gates)
        {
          flank |= distance(g, end) < opening + 1e-3 && distance(g, end) > 1e-6;
        }

        if (!flank)
        {
          continue;
        }

        bool present = false;

        for (const auto &tower : r.towers)
        {
          present |= distance(tower.center, end) < s.wallTowerRadius;
        }

        if (present)
        {
          continue;
        }

        CityWallTower tower;
        tower.id = (uint64_t(1) << 63) | streetId++;
        tower.center = end;
        tower.height = s.wallTowerHeight;

        for (unsigned k = 0; k < 8; ++k)
        {
          const double angle = k * 3.14159265358979323846 / 4;
          tower.footprint.push_back({end.x + s.wallTowerRadius * std::cos(angle), 0, end.z + s.wallTowerRadius * std::sin(angle)});
        }

        r.towers.push_back(std::move(tower));
      }
    }
  }

  // 3. Blocks.
  TownBlocksInput blockInput;
  blockInput.patches = patches;
  for (auto &patch:blockInput.patches)
    if (patch.plaza) patch.use=uint8_t(DistrictUse::Merchant);
  blockInput.arteries = streets.arteries;
  blockInput.curvedArteries = curvedArteries;
  blockInput.wallRing = s.walled ? border : ConvexRing{};
  blockInput.gates = gates;
  blockInput.compounds = s.compounds;
  blockInput.arteryHalfWidth = s.arteryHalfWidth;
  blockInput.mainStreet = s.mainStreet;
  blockInput.regularStreet = s.regularStreet;
  blockInput.alley = s.alley;
  blockInput.maximumInteriorBlockArea = s.maximumInteriorBlockArea;
  blockInput.maximumInteriorBlockSpan = s.maximumInteriorBlockSpan;
  blockInput.seed = s.seed;
  blockInput.random = s.random;
  TownBlocksPlan blocks;

  streetsS+=townSince(townPhase);
  townPhase=townTick();

  if (!TownBlocks::plan(blockInput, blocks, ledger, error))
  {
    return false;
  }

  // 4. Buildings per block: Watabou's ward classes, court wards rung.
  std::set<std::tuple<int64_t,int64_t,int64_t,int64_t>> laneKeys;
  const auto addLane = [&](Position a, Position b, double halfWidth, bool arterial=false) {
    auto ka=std::make_pair(int64_t(std::llround(a.x*1000)),int64_t(std::llround(a.z*1000)));
    auto kb=std::make_pair(int64_t(std::llround(b.x*1000)),int64_t(std::llround(b.z*1000)));
    if (kb<ka) std::swap(ka,kb);
    if (ka==kb || !laneKeys.emplace(ka.first,ka.second,kb.first,kb.second).second) return;
    CityStreet lane;
    lane.id=streetId++; lane.a=a; lane.b=b; lane.path={a,b}; lane.halfWidth=halfWidth; lane.arterial=arterial;
    r.streets.push_back(std::move(lane));
  };
  for (const auto &lane:blocks.lanes) addLane(lane.a,lane.b,lane.halfWidth,lane.arterial);
  uint64_t blockId = 1;
  uint32_t buildings = 0, courts = 0, thinned = 0, mergedLots = 0, constrainedLots = 0;

  blocksS+=townSince(townPhase);
  townPhase=townTick();

  // Every block's use, then every block's lots, before the loop reads either.
  //
  // Cutting a block into lots is the whole of the layout's time - 86 s of 92
  // over 306 blocks, measured - and it depends on nothing but that block's
  // own ring and a key fixed by its index. So the uses are settled first
  // (they decide the target areas), then the cuts run on a pool, and the loop
  // below consumes them in order.
  std::vector<DistrictUse> blockUse(blocks.blocks.size());
  std::vector<uint8_t> blockPoor(blocks.blocks.size(),0);

  for (size_t i=0;i<blocks.blocks.size();++i)
  {
    const auto &block=blocks.blocks[i];
    DistrictUse use=DistrictUse(block.use);

    for (const auto &lane:blocks.lanes) if (lane.arterial)
      for (auto p:block.ring)
        if (segmentDistance(p,lane.a,lane.b)<lane.halfWidth+1 &&
            (use==DistrictUse::Residential || use==DistrictUse::Craftsmen)) use=DistrictUse::Merchant;

    blockUse[i]=use;
    blockPoor[i]=use==DistrictUse::Suburb || use==DistrictUse::WorkerHousing;
  }

  std::vector<LotSplitPlan> blockSplits(blocks.blocks.size());
  std::vector<uint8_t> blockSplitOk(blocks.blocks.size(),0);

  {
    graphrewrite::runtime::WorkerPool splitPool(graphrewrite::runtime::WorkerPool::hardwareWorkers());
    const uint32_t count=uint32_t(blocks.blocks.size());
    splitPool.parallelFor(count,splitPool.chunkCountFor(count),[&](const uint32_t,const uint32_t begin,const uint32_t end) {
      for (uint32_t i=begin;i<end;++i)
      {
        const auto &block=blocks.blocks[i];
        if (!block.dissolved) continue;

        const DistrictUse use=blockUse[i];
        const bool poor=blockPoor[i]!=0;
        LotSplitInput split;
        split.polygon=block.ring;
        split.targetArea=poor ? 48. : use==DistrictUse::Merchant ? 100. : use==DistrictUse::WealthyResidential || use==DistrictUse::Manor ? 200. : 72.;
        split.spread=.5;
        split.minimumWidth=poor ? 5.5 : 7.;
        split.minimumCorner=.5;
        split.chordsAreRoads=false;
        split.convexLots=true;
        split.key=uint64_t(i)+1;
        split.random=s.random;

        for (size_t k=0;k<block.ring.size();++k)
        {
          const Position a=block.ring[k],b=block.ring[(k+1)%block.ring.size()];
          const Position m{(a.x+b.x)*.5,0,(a.z+b.z)*.5};
          bool onStreet=false;

          for (const auto &lane:blocks.lanes)
            onStreet=onStreet || segmentDistance(m,lane.a,lane.b)<lane.halfWidth+1.5;

          for (const auto &artery:curvedArteries)
            for (size_t j=1;j<artery.size() && !onStreet;++j)
              onStreet=segmentDistance(m,artery[j-1],artery[j])<s.arteryHalfWidth+1.5;

          split.edgeKinds.push_back(onStreet ? LotEdgeKind::Road : LotEdgeKind::None);
        }

        Ledger splitLedger;
        std::string splitError;
        blockSplitOk[i]=LotSplitter::plan(split,blockSplits[i],splitLedger,&splitError) ? 1u : 0u;
      }
    });
  }

  for (size_t blockIndex=0;blockIndex<blocks.blocks.size();++blockIndex)
  {
    const auto &block = blocks.blocks[blockIndex];
    CityStreetBlock city;
    city.id = blockId++;
    city.boundary.outer = block.ring;
    city.use = blockUse[blockIndex];
    city.district = patchSources.count(block.patch) ? patchSources.at(block.patch) : block.patch;
    const double r1 = s.random(city.id, 101), r2 = s.random(city.id, 102), r3 = s.random(city.id, 103);
    const WardClass ward = wardClass(block.use, block.outskirts, s.unit, r1, r2, r3);
    const bool poorWard=city.use==DistrictUse::Suburb || city.use==DistrictUse::WorkerHousing;
    const bool courtWard = poorWard || (!block.outskirts && (city.use==DistrictUse::Merchant || city.use==DistrictUse::Residential || city.use==DistrictUse::Craftsmen));
    bool rung = false;

    if (s.villageFarmsteads && !block.withinWalls)
    {
      // Farm.createGeometry: a 4 x 4 unit house square (10 m) between a
      // random vertex and the centroid, turned at random, cut into a small
      // cluster (createOrthoBuilding, minBlockSq 8 units, fill .5).
      const ConvexRing &ring = block.ring;
      Position centroid{0, 0, 0};

      for (auto v : ring)
      {
        centroid.x += v.x / ring.size();
        centroid.z += v.z / ring.size();
      }

      const Position corner = ring[size_t(s.random(city.id, 121) * ring.size()) % ring.size()];
      const double t = .3 + .4 * s.random(city.id, 122);
      const Position at{corner.x + (centroid.x - corner.x) * t, 0, corner.z + (centroid.z - corner.z) * t};
      const double angle = s.random(city.id, 123) * 3.14159265358979, side = 5 * std::sqrt(s.unit / 6.25) * 2;
      ConvexRing house;

      for (auto [dx, dz] : {std::pair{-.5, -.5}, std::pair{.5, -.5}, std::pair{.5, .5}, std::pair{-.5, .5}})
      {
        const double x = dx * side, z = dz * side;
        house.push_back({at.x + x * std::cos(angle) - z * std::sin(angle), 0, at.z + x * std::sin(angle) + z * std::cos(angle)});
      }

      // The house square is kept inside the block.
      for (size_t k = 0; k < ring.size() && !house.empty(); ++k)
      {
        const Position a = ring[k], b = ring[(k + 1) % ring.size()];
        const double l = distance(a, b);

        if (l < 1e-9)
        {
          continue;
        }

        const double nx = (b.z - a.z) / l, nz = (a.x - b.x) / l, c = nx * a.x + nz * a.z;
        ConvexRing clipped;

        for (size_t i = 0; i < house.size(); ++i)
        {
          const Position u = house[i], v = house[(i + 1) % house.size()];
          const double du = nx * u.x + nz * u.z - c, dv = nx * v.x + nz * v.z - c;

          if (du <= 0)
          {
            clipped.push_back(u);
          }

          if ((du < 0) != (dv < 0))
          {
            clipped.push_back(mix(u, v, du / (du - dv)));
          }
        }

        house = clipped;
      }

      if (house.size() >= 3 && std::abs(signedArea(house)) > 20)
      {
        city.lots = WardBuildings::orthoBuilding(house, 8 * s.unit, .5, city.id, s.random);
      }

      buildings += uint32_t(city.lots.size());
      r.blocks.push_back(std::move(city));
      continue;
    }

    // A dissolved block is the land between streets, not a cell: it can be
    // concave, and every cutter above this line clips half-planes, which on a
    // concave ring takes the far side of the block off with the cut. The
    // chord splitter is the one that cuts a simple polygon, so it takes them.
    if (block.dissolved)
    {
      LotSplitPlan &lots=blockSplits[blockIndex];
      const bool splitOk=blockSplitOk[blockIndex]!=0;
      ++splitterCalls;

      if (splitOk && !lots.lots.empty())
      {
        uint32_t escaped = 0;

        for (const auto &lot : lots.lots)
        {
          // A chord across a concave block can leave the block; the splitter
          // refuses most of those and the rest are refused here, because a
          // lot outside its block is a house in the street. Measured on the
          // finished ring, not trusted from the cut.
          bool inside = lot.ring.size() >= 3 && std::abs(signedArea(lot.ring)) >= 12;
          const bool sized = inside;

          for (auto v : lot.ring)
          {
            inside = inside && (strictlyInsideConvex(block.ring, v) || onRing(block.ring, v, .01) || insideEvenOdd(block.ring, v));
          }

          // And a ring the mesher can actually triangulate: simple, convex,
          // no spike. A chord across a concave block can leave a ring that
          // crosses itself, and that surfaces four stages later as a failed
          // triangulation with no way back to the cut that made it.
          for (size_t u = 0; u + 1 < lot.ring.size() && inside; ++u)
          {
            for (size_t w = u + 2; w < lot.ring.size(); ++w)
            {
              if (u == 0 && w + 1 == lot.ring.size())
              {
                continue;
              }

              const Position p = lot.ring[u], q = lot.ring[u + 1], t = lot.ring[w], z = lot.ring[(w + 1) % lot.ring.size()];
              const double a = cross(p, q, t), b = cross(p, q, z), c = cross(t, z, p), d = cross(t, z, q);
              inside = inside && !(a * b < 0 && c * d < 0) && segmentDistance(p, t, z) > .002;
            }
          }

          // Concave is allowed: the mesher triangulates a concave outline and
          // the dwelling planner frames one. What it cannot do is either of
          // the two above - a ring that crosses itself, or a sliver with no
          // room for a house - and measuring those separately is what showed
          // that most of the refusals were slivers, not bad cuts.
          (void)sized;

          if (!inside)
          {
            ++escaped;
            city.openLots.push_back(lot.ring);

            continue;
          }

          city.lots.push_back(lot.ring);
        }

        // A piece too small to build on is not waste: it belongs to whichever
        // neighbour it touches. Merged by an exact boolean union - never a
        // convex hull, which would swallow the alley between them.
        uint32_t absorbed = 0;

        for (size_t k = 0; k < city.openLots.size();)
        {
          const auto &spare = city.openLots[k];

          if (spare.size() < 3 || std::abs(signedArea(spare)) < 1.)
          {
            ++k;

            continue;
          }

          geom2::Polygon sliver;

          for (auto v : spare)
          {
            sliver.outer.push_back({geom2::toUnit(v.x), geom2::toUnit(v.z)});
          }

          if (!geom2::counterClockwise(sliver.outer))
          {
            geom2::reverse(sliver.outer);
          }

          bool merged = false;

          for (size_t j = 0; j < city.lots.size() && !merged; ++j)
          {
            geom2::Polygon neighbour;

            for (auto v : city.lots[j])
            {
              neighbour.outer.push_back({geom2::toUnit(v.x), geom2::toUnit(v.z)});
            }

            if (!geom2::counterClockwise(neighbour.outer))
            {
              geom2::reverse(neighbour.outer);
            }

            const double both = std::abs(geom2::areaMetres(sliver)) + std::abs(geom2::areaMetres(neighbour));
            std::vector<geom2::Polygon> united;
            std::string unionError;

            if (!geom2::unionOf({geom2::toOperand(sliver), geom2::toOperand(neighbour)}, united, &unionError) || united.size() != 1 || !united.front().holes.empty())
            {
              continue;
            }

            if (std::abs(std::abs(geom2::areaMetres(united.front())) - both) > .01)
            {
              continue;
            }

            ConvexRing ring;

            for (auto v : united.front().outer)
            {
              ring.push_back({geom2::toMetres(v.x), 0, geom2::toMetres(v.y)});
            }

            if (ring.size() < 3)
            {
              continue;
            }

            city.lots[j] = std::move(ring);
            merged = true;
            ++absorbed;
          }

          if (merged)
          {
            city.openLots.erase(city.openLots.begin() + k);
          }
          else
          {
            ++k;
          }
        }

        if (escaped != 0 || absorbed != 0)
        {
          ledger.add(LedgerKind::Omitted,
                     0,
                     "town layout: block " + std::to_string(city.id) + " refused " + std::to_string(escaped) + " pieces, " + std::to_string(absorbed) + " of them absorbed by a neighbour");
        }

        // A lot too small for a house goes to its smallest neighbour.
        //
        // The splitter's minimum keeps a lot wide enough for a frontage, not
        // large enough for a house: a fifth of the city's houses stood on
        // under sixteen square metres, most of them on lots this size, and
        // read as huts between the houses. Merged into the SMALLEST lot that
        // shares an edge - the union must be one polygon, no hole, the two
        // areas exactly - and repeated until nothing under the minimum is
        // left, since the merged lot may still be small.
        uint32_t merged = 0;

        // The floor a lot has to clear before it is merged away. A poor ward
        // gets a HIGHER one, not a lower one: its lots start smallest, so
        // without this the merge never fires there and the sheds survive.
        const double floorMetres2 = poorWard ? std::max(s.minimumLotMetres2, 62.) : s.minimumLotMetres2;

        for (bool again = true; again;)
        {
          again = false;

          for (size_t i = 0; i < city.lots.size() && !again; ++i)
          {
            if (std::abs(signedArea(city.lots[i])) >= floorMetres2)
            {
              continue;
            }

            geom2::Polygon small;
            for (auto v : city.lots[i]) small.outer.push_back({geom2::toUnit(v.x), geom2::toUnit(v.z)});
            if (!geom2::counterClockwise(small.outer)) geom2::reverse(small.outer);

            size_t best = SIZE_MAX;
            double bestArea = 1e300;
            std::vector<geom2::Polygon> bestUnion;

            for (size_t j = 0; j < city.lots.size(); ++j)
            {
              if (j == i) continue;
              const double area = std::abs(signedArea(city.lots[j]));
              if (area >= bestArea) continue;

              geom2::Polygon neighbour;
              for (auto v : city.lots[j]) neighbour.outer.push_back({geom2::toUnit(v.x), geom2::toUnit(v.z)});
              if (!geom2::counterClockwise(neighbour.outer)) geom2::reverse(neighbour.outer);

              std::vector<geom2::Polygon> united;
              std::string unionError;
              if (!geom2::unionOf({geom2::toOperand(small), geom2::toOperand(neighbour)}, united, &unionError) ||
                  united.size() != 1 || !united.front().holes.empty())
                continue;
              const double both = std::abs(geom2::areaMetres(small)) + std::abs(geom2::areaMetres(neighbour));
              if (std::abs(std::abs(geom2::areaMetres(united.front())) - both) > .01) continue;
              // Sharing an edge, not a corner: the union's outline loses at
              // least two vertices to the join.
              if (united.front().outer.size() > small.outer.size() + neighbour.outer.size() - 2) continue;

              best = j;
              bestArea = area;
              bestUnion = std::move(united);
            }

            if (best == SIZE_MAX)
            {
              continue;
            }

            ConvexRing ring;
            for (auto v : bestUnion.front().outer) ring.push_back({geom2::toMetres(v.x), 0, geom2::toMetres(v.y)});
            if (ring.size() < 3) continue;

            city.lots[best] = std::move(ring);
            city.lots.erase(city.lots.begin() + long(i));
            ++merged;
            again = true;
          }
        }

        if (merged != 0)
        {
          ledger.add(LedgerKind::Omitted, 0,
                     "town layout: block " + std::to_string(city.id) + " merged " + std::to_string(merged) + " lots under " +
                     std::to_string(int(floorMetres2)) + " m2 into their smallest neighbour");
        }

        rung = !city.lots.empty();
      }
      else
      {
        ledger.add(LedgerKind::Omitted, 0, "town layout: dissolved block " + std::to_string(city.id) + " has no lots");
      }
    }

    // A dissolved block is concave and the planners below clip half-planes:
    // handing one to them takes the far side of the block off with every cut,
    // which is how a lot ended up eight metres outside its own block. If the
    // chord splitter could not cut it, it stays open ground.
    if (block.dissolved)
    {
      buildings += uint32_t(city.lots.size());
      r.blocks.push_back(std::move(city));

      continue;
    }

    if (!rung && courtWard)
    {
      // A poor ward is not a ward of sheds.
      //
      // Thirty six square metres on a six metre depth is a plot that fits one
      // room and a door, which is what the timber houses in the suburbs were:
      // the smallest target in the town, so they came out smaller than
      // anything else and read as outbuildings between the houses. A poor
      // household is still a household - it wants a room to sleep in, a room
      // to work in and somewhere to put the yard.
      const double plot = poorWard ? 62. : city.use == DistrictUse::Merchant ? 72. : 55.;
      RingLotInput ringInput;
      ringInput.block = block.ring;
      ringInput.depth = poorWard ? 9. : city.use == DistrictUse::Merchant ? 14. : 12.;
      ringInput.solidFallback=true;
      ringInput.targetArea = plot;
      ringInput.minimumFrontage = 5.5;
      ringInput.divideCourtAbove = poorWard ? 180 : DistrictUse(block.use) == DistrictUse::Merchant ? 2500 : 700;
      // Keep small shared courts. Filling them creates backland plots without
      // street frontage; large blocks already receive access streets upstream.
      ringInput.fillCourtBelow = 0;
      ringInput.alleyWidth = s.alley * 1.5;
      ringInput.key = city.id;
      ringInput.random = s.random;
      double longest = -1;

      for (size_t k = 0; k < block.ring.size(); ++k)
      {
        const Position a = block.ring[k], b = block.ring[(k + 1) % block.ring.size()];

        if (distance(a, b) > longest)
        {
          longest = distance(a, b);
          ringInput.axisRadians = std::atan2(b.z - a.z, b.x - a.x);
        }
      }

      RingLotPlan ringPlan;
      Ledger ringLedger;
      std::string ringError;

      const auto ringStart=townTick();++ringCalls;
      const bool ringOk=RingLotPlanner::plan(ringInput, ringPlan, ringLedger, &ringError);
      ringS+=townSince(ringStart);
      if (ringOk && !ringPlan.lots.empty())
      {
        city.lots = ringPlan.lots;
        city.openLots = ringPlan.open;
        for (const auto &alley:ringPlan.alleys)
          if (alley.size()>=2) addLane(alley.front(),alley.back(),ringInput.alleyWidth*.5);

        for (auto &court : ringPlan.courts)
        {
          city.courtyards.push_back({court, {}});
        }

        courts += uint32_t(ringPlan.courts.size());
        rung = true;
      }
    }

    if (!rung)
    {
      WardBuildingsInput wardInput;
      wardInput.patch = block.ring;
      wardInput.minSq = ward.minSq;
      if (city.use==DistrictUse::WealthyResidential || city.use==DistrictUse::Manor)
        wardInput.minSq=std::min(ward.minSq,city.use==DistrictUse::Manor ? 220. : 140.);
      if (!block.outskirts && (city.use==DistrictUse::Residential || city.use==DistrictUse::Craftsmen ||
          city.use==DistrictUse::WorkerHousing || city.use==DistrictUse::Merchant))
        wardInput.minSq=std::min(ward.minSq,city.use==DistrictUse::Merchant ? 110. : 75.);
      wardInput.gridChaos = ward.gridChaos;
      wardInput.sizeChaos = ward.sizeChaos;
      wardInput.emptyProb = ward.emptyProb;
      if(city.use==DistrictUse::Suburb || city.use==DistrictUse::WorkerHousing) {
        // Fallback irregular plots need room for the 4.8 m admission square;
        // cutting to 32 m² before alleys makes slivers that are merged away.
        wardInput.minSq=55.;
        wardInput.emptyProb=.015;
      }
      if (!block.outskirts && (city.use==DistrictUse::Residential || city.use==DistrictUse::Craftsmen || city.use==DistrictUse::WorkerHousing))
        wardInput.emptyProb=.01;
      wardInput.alley = s.alley;
      wardInput.outskirts = block.outskirts;
      wardInput.populated = block.populated;
      wardInput.vertexDensity = block.vertexDensity;
      wardInput.key = city.id;
      wardInput.random = s.random;
      WardBuildingsPlan wardPlan;
      Ledger wardLedger;
      std::string wardError;

      const auto wardStart=townTick();++wardCalls;
      const bool wardOk=WardBuildings::plan(wardInput, wardPlan, wardLedger, &wardError);
      wardS+=townSince(wardStart);
      if (wardOk)
      {
        city.lots = wardPlan.buildings;
        city.openLots = wardPlan.open;
        for (const auto &alley:wardPlan.alleys)
          if (alley.size()>=2) addLane(alley.front(),alley.back(),s.alley*.5);
        thinned += wardPlan.filtered;
      }
      else
      {
        ledger.add(LedgerKind::Omitted, 0, "town layout: block " + std::to_string(city.id) + " has no buildings: " + wardError);
      }
    }

    // Consolidate sub-minimum plots before assigning dwelling programmes.
    // Merge only an exact union: never bridge a road or courtyard.
    const auto mergeStart=townTick();
    for (size_t i=0;i<city.lots.size();)
    {
      if (fitsDwelling(city.lots[i])) {++i;continue;}
      size_t best=city.lots.size(); double bestArea=1e100; ConvexRing merged;
      for (size_t j=0;j<city.lots.size();++j) if (j!=i)
      {
        auto candidate=lotUnion(city.lots[i],city.lots[j]);
        if (!candidate.empty() && signedArea(candidate)<bestArea)
        {best=j;bestArea=signedArea(candidate);merged=std::move(candidate);}
      }
      if (best==city.lots.size()) {++i;continue;}
      city.lots[best]=std::move(merged);city.lots.erase(city.lots.begin()+i);
      ++mergedLots;
      i=0;
    }
    mergeS+=townSince(mergeStart);
    for (const auto &lot:city.lots) if (!fitsDwelling(lot)) ++constrainedLots;
    buildings += uint32_t(city.lots.size());
    r.blocks.push_back(std::move(city));
  }

  // Alleys terminate on inset block boundaries, not on the parent street's
  // centreline. Bridge that reserved margin before publishing road surfaces.
  // Never extend through a lot merely to make the graph connected.
  const auto overlaps = [](const ConvexRing &a, const ConvexRing &b)
  {
    const auto separated = [](const ConvexRing &p, const ConvexRing &q)
    {
      for (size_t i = 0; i < p.size(); ++i)
      {
        const auto u = p[i], v = p[(i + 1) % p.size()];
        const double nx = -(v.z-u.z), nz = v.x-u.x;
        double pmin = 1e30, pmax = -1e30, qmin = 1e30, qmax = -1e30;
        for (auto x : p) { const double d = x.x*nx+x.z*nz; pmin=std::min(pmin,d); pmax=std::max(pmax,d); }
        for (auto x : q) { const double d = x.x*nx+x.z*nz; qmin=std::min(qmin,d); qmax=std::max(qmax,d); }
        if (pmax <= qmin + .001 || qmax <= pmin + .001) return true;
      }
      return false;
    };
    return !a.empty() && !b.empty() && !separated(a,b) && !separated(b,a);
  };
  const size_t originalStreets = r.streets.size();
  for (size_t i = 0; i < originalStreets; ++i)
  {
    const auto street = r.streets[i];
    if (street.arterial) continue;
    for (int end = 0; end < 2; ++end)
    {
      const Position p = end ? street.b : street.a, other = end ? street.a : street.b;
      bool joined = false;
      Position best;
      double bestDistance = 1e30;
      for (size_t j = 0; j < r.streets.size(); ++j)
      {
        if (j == i) continue;
        const auto &target = r.streets[j];
        const double dx = target.b.x-target.a.x, dz = target.b.z-target.a.z, dd = dx*dx+dz*dz;
        if (dd < 1e-10) continue;
        const double t = std::clamp(((p.x-target.a.x)*dx+(p.z-target.a.z)*dz)/dd,0.0,1.0);
        const Position q = mix(target.a,target.b,t);
        const double gap = distance(p,q);
        if (gap < .001) { joined = true; break; }
        if (gap >= bestDistance || gap > target.halfWidth + street.halfWidth + 1 || gap > 8) continue;
        if ((q.x-p.x)*(p.x-other.x)+(q.z-p.z)*(p.z-other.z) < .25*gap*distance(p,other)) continue;
        // The road surface has round caps. A ribbon alone misses a lot just
        // beyond either endpoint, particularly at subdivided T-junctions.
        const auto corridor = ribbon(p,q,street.halfWidth);
        const auto touchesRoad = [&](const ConvexRing &ring) {
          if (overlaps(corridor,ring) || strictlyInsideConvex(ring,p) || strictlyInsideConvex(ring,q)) return true;
          for (size_t k=0;k<ring.size();++k) {
            const auto a=ring[k],b=ring[(k+1)%ring.size()];
            if (segmentDistance(p,a,b)<street.halfWidth || segmentDistance(q,a,b)<street.halfWidth ||
                segmentDistance(a,p,q)<street.halfWidth) return true;
          }
          return false;
        };
        bool blocked = false;
        for (const auto &block : r.blocks)
          for (const auto &lot : block.lots)
            if (touchesRoad(lot)) { blocked = true; break; }
        for (const auto &reservation : r.reservations)
          if (touchesRoad(reservation)) blocked = true;
        for (const auto &wall : r.walls)
          if (distance(wall.a,wall.b) > .001 && touchesRoad(ribbon(wall.a,wall.b,wall.halfWidth))) blocked = true;
        for (const auto &tower : r.towers)
          if (touchesRoad(tower.footprint)) blocked = true;
        if (!blocked) { best = q; bestDistance = gap; }
      }
      if (!joined && bestDistance < 1e30) addLane(p,best,street.halfWidth);
    }
  }

  // Alley planners own their straight strips, but acute junctions also need
  // their round end clearance. Reserve the complete final network in lots,
  // including concave unions, before any dwelling request leaves the planner.
  struct RoadReservation { geom2::Operand polygon; double x0,z0,x1,z1; };
  std::vector<RoadReservation> roadReservations;
  for(const auto &street:r.streets) {
    const double w=street.halfWidth+.0001;
    const double x0=std::min(street.a.x,street.b.x)-w*2,x1=std::max(street.a.x,street.b.x)+w*2;
    const double z0=std::min(street.a.z,street.b.z)-w*2,z1=std::max(street.a.z,street.b.z)+w*2;
    ConvexRing cap{{x0,0,z0},{x1,0,z0},{x1,0,z1},{x0,0,z1}};
    const double direction=std::atan2(street.b.z-street.a.z,street.b.x-street.a.x)+1.5707963267948966;
    for(unsigned k=0;k<16;++k) {
      const double angle=direction+k*6.283185307179586/16,nx=std::cos(angle),nz=std::sin(angle);
      cap=cutRing(cap,nx,nz,std::max(nx*street.a.x+nz*street.a.z,nx*street.b.x+nz*street.b.z)+w);
    }
    geom2::Polygon polygon;for(auto p:cap)polygon.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
    roadReservations.push_back({geom2::toOperand(polygon),x0,z0,x1,z1});
  }
  lotsS+=townSince(townPhase);
  townPhase=townTick();

  // The network is merged per tile before any lot asks about it.
  //
  // A smoothed artery is sampled every metre, so the street list is thousands
  // of one-metre capsules and a lot beside one was differenced against fifty
  // of them: three and a half thousand lots times fifty operands is what took
  // the layout from seconds to minutes. Unioned once per 48 m tile, a lot sees
  // three or four merged polygons instead.
  struct RoadTile { std::vector<geom2::Operand> merged; };
  const double roadTile=48.;
  double roadMinX=1e30,roadMinZ=1e30,roadMaxX=-1e30,roadMaxZ=-1e30;
  for(const auto &road:roadReservations) {
    roadMinX=std::min(roadMinX,road.x0);roadMinZ=std::min(roadMinZ,road.z0);
    roadMaxX=std::max(roadMaxX,road.x1);roadMaxZ=std::max(roadMaxZ,road.z1);
  }
  std::map<std::pair<int64_t,int64_t>,RoadTile> roadTiles;
  if(!roadReservations.empty()) {
    std::map<std::pair<int64_t,int64_t>,std::vector<geom2::Operand>> buckets;
    for(const auto &road:roadReservations) {
      const int64_t cx0=int64_t(std::floor((road.x0-roadMinX)/roadTile)),cx1=int64_t(std::floor((road.x1-roadMinX)/roadTile));
      const int64_t cz0=int64_t(std::floor((road.z0-roadMinZ)/roadTile)),cz1=int64_t(std::floor((road.z1-roadMinZ)/roadTile));
      for(int64_t cx=cx0;cx<=cx1;++cx)for(int64_t cz=cz0;cz<=cz1;++cz)buckets[{cx,cz}].push_back(road.polygon);
    }
    for(auto &[key,capsules]:buckets) {
      RoadTile tile;
      std::vector<geom2::Polygon> united;
      if(capsules.size()>1 && geom2::unionOf(capsules,united,nullptr))
        for(const auto &piece:united)tile.merged.push_back(geom2::toOperand(piece));
      else tile.merged=capsules;
      roadTiles.emplace(key,std::move(tile));
    }
  }

  buildings=0;
  for(auto &block:r.blocks) {
    std::vector<ConvexRing> legal;
    for(const auto &lot:block.lots) {
      double x0=1e30,z0=1e30,x1=-1e30,z1=-1e30;
      geom2::Polygon polygon;
      for(auto p:lot){x0=std::min(x0,p.x);z0=std::min(z0,p.z);x1=std::max(x1,p.x);z1=std::max(z1,p.z);polygon.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});}
      std::vector<geom2::Operand> obstacles;
      if(!roadReservations.empty()) {
        const int64_t cx0=int64_t(std::floor((x0-roadMinX)/roadTile)),cx1=int64_t(std::floor((x1-roadMinX)/roadTile));
        const int64_t cz0=int64_t(std::floor((z0-roadMinZ)/roadTile)),cz1=int64_t(std::floor((z1-roadMinZ)/roadTile));
        for(int64_t cx=cx0;cx<=cx1;++cx)for(int64_t cz=cz0;cz<=cz1;++cz) {
          const auto found=roadTiles.find({cx,cz});
          if(found==roadTiles.end())continue;
          for(const auto &piece:found->second.merged)obstacles.push_back(piece);
        }
      }
      if(obstacles.empty()){legal.push_back(lot);continue;}
      std::vector<geom2::Polygon> remaining;std::string why;
      if(!geom2::differenceOf(geom2::toOperand(polygon),obstacles,remaining,&why))return fail("lot street reservation: "+why);
      for(const auto &piece:remaining) {
        ConvexRing ring;for(auto p:piece.outer)ring.push_back({geom2::toMetres(p.x),0,geom2::toMetres(p.y)});
        // Boolean intersections are quantised to micrometres. On very short
        // bevel edges, keep near-boundary points decisively inside their owner
        // instead of in the predicate's numerical boundary band. The 0.1 mm
        // road reservation margin above still exceeds this adjustment.
        Position centre{};for(auto p:block.boundary.outer){centre.x+=p.x/block.boundary.outer.size();centre.z+=p.z/block.boundary.outer.size();}
        for(auto &p:ring)if(!strictlyInsideConvex(block.boundary.outer,p) && !onRing(block.boundary.outer,p,1e-6)) {
          double nearest=1e30;
          for(size_t i=0;i<block.boundary.outer.size();++i)nearest=std::min(nearest,segmentDistance(p,block.boundary.outer[i],block.boundary.outer[(i+1)%block.boundary.outer.size()]));
          if(nearest<.0001 && distance(p,centre)>.0001)p=mix(p,centre,.00005/distance(p,centre));
        }
        if(piece.holes.empty() && ring.size()>=4 && signedArea(ring)>=12)legal.push_back(std::move(ring));
        else if(ring.size()>=3)block.openLots.push_back(std::move(ring));
      }
    }
    block.lots=std::move(legal);buildings+=uint32_t(block.lots.size());
  }

  uint32_t packedFrontages=0;
  for(auto &block:r.blocks)
    if(block.use==DistrictUse::Residential || block.use==DistrictUse::Merchant ||
       block.use==DistrictUse::Craftsmen || block.use==DistrictUse::WorkerHousing ||
       block.use==DistrictUse::WealthyResidential || (!s.villageFarmsteads && block.use==DistrictUse::Suburb))
    {
      const auto count=packFrontageLots(block.lots,r.streets);
      block.frontagePacked=count>0;
      packedFrontages+=count;
    }
  if(packedFrontages)ledger.add(LedgerKind::Repaired,0,"repacked "+std::to_string(packedFrontages)+" adjoining street parcels");
  std::fprintf(stderr,"town frontage packing: %u parcel groups across %zu blocks\n",packedFrontages,r.blocks.size());
  buildings=0;for(const auto &block:r.blocks)buildings+=uint32_t(block.lots.size());

  reserveS+=townSince(townPhase);

  if (townTiming)
    std::fprintf(stderr,"town layout: streets %.0f s | blocks %.0f s | lots %.0f s | street reservation %.0f s (%zu blocks)\n"
      "  splitter %.0f s (%u calls) | ring %.0f s (%u) | ward %.0f s (%u) | sliver merge %.0f s\n",
      streetsS,blocksS,lotsS,reserveS,blocks.blocks.size(),
      splitterS,splitterCalls,ringS,ringCalls,wardS,wardCalls,mergeS);

  // Wards, suburbs and the ground and road contours the world builder reads.
  uint64_t contourId = 1;

  // The market square: open, paved ground at the heart of the town, published as a
  // paved surface, with no block over it, so nothing builds on it.
  if (blocks.market.size() >= 3)
  {
    SurfaceContour square;
    square.id = contourId++;
    square.material = 1;
    square.role = SurfaceRole::Road; // Paved public ground, like the streets it opens off.
    square.terrainSurface = true;
    square.plane.constant = s.terrain ? s.terrain(blocks.market.front().x, blocks.market.front().z) : 0;
    square.loop = blocks.market;
    r.contours.push_back(std::move(square));
    r.market = blocks.market;
  }


  for (const auto &patch : patches)
  {
    if (!(patch.withinWalls || patch.outskirts))
    {
      continue;
    }

    r.wards.push_back({patch.id, {patch.ring}});

    if (patch.outskirts)
    {
      r.suburbs.push_back(patch.ring);
    }

    SurfaceContour c;
    c.id = contourId++;
    c.material = patch.withinWalls ? 11 : 16;
    c.loop = patch.ring;
    r.contours.push_back(std::move(c));
  }

  for (const auto &street : r.streets)
  {
    SurfaceContour c;
    c.id = contourId++;
    c.material = 1;
    c.role = SurfaceRole::Road;
    c.terrainSurface = true;
    c.plane.constant = s.terrain ? s.terrain(street.a.x, street.a.z) : 0;
    c.loop = ribbon(street.a, street.b, street.halfWidth);
    r.contours.push_back(std::move(c));
  }

  r.subdivisionCells = uint32_t(patches.size());
  ledger.add(LedgerKind::Repaired,0,"town lot admission: "+std::to_string(mergedLots)+
      " neighbour merges, "+std::to_string(constrainedLots)+" constrained plots without a safe merge");
  ledger.add(
      LedgerKind::Repaired,
      0,
      "town layout: " + std::to_string(r.blocks.size()) + " blocks, " + std::to_string(buildings) + " buildings, " + std::to_string(courts) + " courts, " + std::to_string(thinned) + " thinned outside, " +
          std::to_string(r.streets.size()) + " street segments, " + std::to_string(r.walls.size()) + " wall sections");
  out = std::move(r);
  return true;
}

bool TownLayout::run(const TownLayoutInput &input, CityLayout &output, Ledger &ledger, std::string *error)
{
  return plan(input, output, ledger, error);
}

} // namespace procedural::world::generator
