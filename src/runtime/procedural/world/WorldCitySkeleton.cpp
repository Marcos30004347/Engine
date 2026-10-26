#include "WorldCitySkeleton.hpp"

#include "generator/CityBlockPlanner.hpp"
#include "generator/PlazaPlanner.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace procedural::world::rewrite {
namespace {
constexpr double epsilon = 1e-6;

uint64_t mix(uint64_t h, uint64_t v)
{
  h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  return h;
}

uint64_t stableId(uint64_t domain, uint64_t source, uint64_t index = 0)
{
  uint64_t h = 0xcbf29ce484222325ULL;
  h = mix(h, domain);
  h = mix(h, source);
  h = mix(h, index);
  return h ? h : 1;
}

int64_t quantize(double v)
{
  return std::llround(v * 1000.0);
}

uint64_t pointKey(const Position &p)
{
  uint64_t h = stableId(100, uint64_t(quantize(p.x)));
  h = mix(h, uint64_t(quantize(p.y)));
  return mix(h, uint64_t(quantize(p.z)));
}

double cross(const Position &a, const Position &b, const Position &c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

double polygonArea(const std::vector<Position> &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  return std::abs(a) * .5;
}

Position centroid(const std::vector<Position> &p)
{
  Position c;
  if (p.empty())
    return c;
  for (const auto &v : p)
  {
    c.x += v.x;
    c.y += v.y;
    c.z += v.z;
  }
  c.x /= p.size();
  c.y /= p.size();
  c.z /= p.size();
  return c;
}

double distance2(const Position &a, const Position &b)
{
  const double dx = a.x - b.x, dz = a.z - b.z;
  return dx * dx + dz * dz;
}

Position local(const Position &p, const std::vector<Position> &boundary)
{
  const Position c = centroid(boundary);
  return {p.x - c.x, p.y - c.y, p.z - c.z};
}

bool pointInPolygon(const Position &p, const std::vector<Position> &poly)
{
  bool inside = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
  {
    const auto &a = poly[i];
    const auto &b = poly[j];
    const bool crosses = (a.z > p.z) != (b.z > p.z);
    if (crosses && p.x < (b.x - a.x) * (p.z - a.z) / (b.z - a.z) + a.x)
      inside = !inside;
  }
  return inside;
}

bool collinearOverlap(const Position &a, const Position &b, const Position &c, const Position &d, Position &mid, double &length)
{
  const double ab = std::hypot(b.x - a.x, b.z - a.z);
  if (ab <= epsilon || std::abs(cross(a, b, c)) > epsilon * ab || std::abs(cross(a, b, d)) > epsilon * ab)
    return false;
  const double ux = (b.x - a.x) / ab, uz = (b.z - a.z) / ab;
  double c0 = (c.x - a.x) * ux + (c.z - a.z) * uz;
  double c1 = (d.x - a.x) * ux + (d.z - a.z) * uz;
  if (c0 > c1)
    std::swap(c0, c1);
  const double lo = std::max(0.0, c0), hi = std::min(ab, c1);
  if (hi - lo <= epsilon)
    return false;
  length = hi - lo;
  const double t = (lo + hi) * .5;
  mid = {a.x + ux * t, (a.y + b.y) * .5, a.z + uz * t};
  return true;
}

bool sharedBoundary(const std::vector<Position> &a, const std::vector<Position> &b, Position &point, double &length)
{
  length = 0;
  Position weighted;
  for (size_t i = 0; i < a.size(); ++i)
    for (size_t j = 0; j < b.size(); ++j)
    {
      Position mid;
      double segment = 0;
      if (!collinearOverlap(a[i], a[(i + 1) % a.size()], b[j], b[(j + 1) % b.size()], mid, segment))
        continue;
      weighted.x += mid.x * segment;
      weighted.y += mid.y * segment;
      weighted.z += mid.z * segment;
      length += segment;
    }
  if (length <= epsilon)
    return false;
  point = {weighted.x / length, weighted.y / length, weighted.z / length};
  return true;
}

uint64_t polygonKey(const std::vector<Position> &p)
{
  std::vector<uint64_t> keys;
  for (const auto &v : p)
    keys.push_back(pointKey(v));
  std::sort(keys.begin(), keys.end());
  uint64_t h = stableId(101, keys.size());
  for (auto key : keys)
    h = mix(h, key);
  return h;
}

void addRegion(
    CitySkeleton &out,
    std::set<uint64_t> &ids,
    uint64_t domain,
    uint64_t source,
    uint64_t index,
    uint64_t parent,
    CityRegionKind kind,
    CityLandUse use,
    DistrictUse districtUse,
    const std::vector<Position> &boundary,
    bool exterior)
{
  if (boundary.size() < 3 || polygonArea(boundary) <= epsilon)
    return;
  CitySkeletonRegion r;
  r.id = stableId(domain, source, index);
  while (!ids.insert(r.id).second)
    r.id = mix(r.id, 1);
  r.source = source;
  r.parent = parent;
  r.kind = kind;
  r.landUse = use;
  r.districtUse = districtUse;
  r.boundary = boundary;
  r.exterior = exterior;
  r.area = polygonArea(boundary);
  out.regions.push_back(std::move(r));
}

void addRoad(
    CitySkeleton &out,
    std::map<uint64_t, size_t> &nodes,
    uint64_t domain,
    uint64_t source,
    CityRoadKind kind,
    std::vector<Position> path,
    double halfWidth)
{
  if (path.size() < 2)
    return;
  const auto node = [&](const Position &p)
  {
    const uint64_t id = stableId(120, pointKey(p));
    auto it = nodes.find(id);
    if (it != nodes.end())
      return id;
    nodes[id] = out.roadNodes.size();
    out.roadNodes.push_back({id, CityRoadNodeKind::Endpoint, p, {}});
    return id;
  };
  CityRoadEdge edge;
  edge.id = stableId(domain, source, polygonKey(path));
  edge.source = source;
  edge.kind = kind;
  edge.from = node(path.front());
  edge.to = node(path.back());
  edge.path = std::move(path);
  edge.halfWidth = halfWidth;
  out.roadEdges.push_back(edge);
  out.roadNodes[nodes[edge.from]].edges.push_back(edge.id);
  out.roadNodes[nodes[edge.to]].edges.push_back(edge.id);
}

Position closestPoint(const Position &p, const Position &a, const Position &b)
{
  const double dx = b.x - a.x, dz = b.z - a.z;
  const double dd = dx * dx + dz * dz;
  const double t = dd <= epsilon ? 0 : std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / dd, 0.0, 1.0);
  return {a.x + dx * t, a.y + (b.y - a.y) * t, a.z + dz * t};
}

// Node the planar street arrangement, not just the input polyline endpoints.
// Otherwise T-junctions and crossings are disconnected in routing and meshing.
void nodeRoads(CitySkeleton &out, std::map<uint64_t, size_t> &nodes)
{
  struct Segment
  {
    CityRoadEdge road;
    Position a, b;
    std::vector<std::pair<double, Position>> cuts;
  };
  std::vector<Segment> segments;
  for (const auto &road : out.roadEdges)
    for (size_t i = 1; i < road.path.size(); ++i)
    {
      const auto a = road.path[i - 1], b = road.path[i];
      if (std::hypot(b.x - a.x, b.z - a.z) > epsilon)
        segments.push_back({road, a, b, {{0, a}, {1, b}}});
    }
  for (size_t i = 0; i < segments.size(); ++i)
    for (size_t j = i + 1; j < segments.size(); ++j)
    {
      auto &a = segments[i];
      auto &b = segments[j];
      if (std::max(a.a.x, a.b.x) + .001 < std::min(b.a.x, b.b.x) ||
          std::max(b.a.x, b.b.x) + .001 < std::min(a.a.x, a.b.x) ||
          std::max(a.a.z, a.b.z) + .001 < std::min(b.a.z, b.b.z) ||
          std::max(b.a.z, b.b.z) + .001 < std::min(a.a.z, a.b.z))
        continue;
      const double ax = a.b.x - a.a.x, az = a.b.z - a.a.z;
      const double bx = b.b.x - b.a.x, bz = b.b.z - b.a.z;
      const double dx = b.a.x - a.a.x, dz = b.a.z - a.a.z;
      const double det = ax * bz - az * bx;
      if (std::abs(det) > epsilon)
      {
        const double t = (dx * bz - dz * bx) / det;
        const double u = (dx * az - dz * ax) / det;
        if (t < -epsilon || t > 1 + epsilon || u < -epsilon || u > 1 + epsilon)
          continue;
        Position p{a.a.x + ax * t, a.a.y + (a.b.y - a.a.y) * t, a.a.z + az * t};
        if (std::abs(p.y - (b.a.y + (b.b.y - b.a.y) * u)) > .01)
          continue; // A bridge is not a junction with the road below it.
        a.cuts.push_back({std::clamp(t, 0.0, 1.0), p});
        b.cuts.push_back({std::clamp(u, 0.0, 1.0), p});
      }
      else
      {
        const auto cut = [](Segment &s, const Position &p)
        {
          const auto q = closestPoint(p, s.a, s.b);
          if (std::hypot(q.x - p.x, q.z - p.z) > .0001 || std::abs(q.y - p.y) > .01)
            return;
          const double x = s.b.x - s.a.x, z = s.b.z - s.a.z;
          s.cuts.push_back({((q.x - s.a.x) * x + (q.z - s.a.z) * z) / (x*x + z*z), p});
        };
        cut(a, b.a); cut(a, b.b); cut(b, a.a); cut(b, a.b);
      }
    }
  out.roadNodes.clear();
  out.roadEdges.clear();
  nodes.clear();
  std::set<std::pair<uint64_t, uint64_t>> emitted;
  for (auto &s : segments)
  {
    std::sort(s.cuts.begin(), s.cuts.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (size_t i = 1; i < s.cuts.size(); ++i)
    {
      const auto a = s.cuts[i - 1].second, b = s.cuts[i].second;
      const auto ka = pointKey(a), kb = pointKey(b);
      if (ka == kb || !emitted.insert(std::minmax(ka, kb)).second)
        continue;
      addRoad(out, nodes, 34, s.road.source, s.road.kind, {a, b}, s.road.halfWidth);
    }
  }
}
} // namespace

bool composeCitySkeleton(const CityTerritory &territory, const CityWalls *walls, const SettlementFabric &fabric, CitySkeleton &result, std::string *error)
{
  CitySkeleton out;
  out.revision = std::max(territory.revision, fabric.revision);
  std::set<uint64_t> ids;
  std::map<uint64_t, uint64_t> districtIds, patchIds, blockIds;

  for (const auto &d : territory.regions.districts)
    for (size_t i = 0; i < d.pieces.size(); ++i)
    {
      const CityLandUse use = d.use == DistrictUse::Harbor ? CityLandUse::Harbour : CityLandUse::UrbanDistrict;
      addRegion(out, ids, 1, d.id, i, 0, CityRegionKind::District, use, d.use, d.pieces[i], false);
      if (!out.regions.empty())
        districtIds.emplace(d.id, out.regions.back().id);
    }
  for (const auto &patch : territory.rural.patches)
  {
    addRegion(out, ids, 2, patch.id, 0, 0, CityRegionKind::RuralPatch, CityLandUse::Arable, DistrictUse::Residential, patch.boundary, true);
    if (!out.regions.empty())
      patchIds[patch.id] = out.regions.back().id;
  }
  for (size_t i = 0; i < territory.fields.size() && i < territory.rural.patches.size(); ++i)
    for (const auto &field : territory.fields[i].fields)
      addRegion(out, ids, 3, field.id, 0, patchIds[territory.rural.patches[i].id], CityRegionKind::Field, CityLandUse::Arable, DistrictUse::Residential, field.boundary, true);
  for (const auto &holding : territory.holdings)
  {
    const size_t beforeYard = out.regions.size();
    addRegion(out, ids, 4, holding.patch, 0, patchIds[holding.patch], CityRegionKind::Farmyard, CityLandUse::Farmstead, DistrictUse::Residential, holding.yard, true);
    if (out.regions.size() == beforeYard)
    {
      continue;
    }
    // The farmhouse is a dwelling, not a box: its site in the yard is a lot
    // with the farmstead programme, so the house gets its byre and barn
    // from the same room programme as a town house gets its forge, and a
    // farmer's household from the history can be bound to it.
    const uint64_t yard = out.regions.back().id;
    for (const auto &building : holding.buildings)
    {
      if (building.use != FarmBuildingUse::Farmhouse || building.footprint.size() < 3)
      {
        continue;
      }
      const Position centre = centroid(building.footprint);
      std::vector<Position> site;
      for (const auto &p : building.footprint)
      {
        const double dx = p.x - centre.x, dz = p.z - centre.z, run = std::hypot(dx, dz);
        site.push_back(run > 1e-9 ? Position{p.x + dx / run * 1.7, 0, p.z + dz / run * 1.7} : p);
      }
      const size_t beforeLot = out.regions.size();
      addRegion(out, ids, 4, holding.patch, building.id, yard, CityRegionKind::Lot, CityLandUse::Farmstead, DistrictUse::Residential, site, true);
      if (out.regions.size() > beforeLot)
      {
        out.regions.back().lotProgramme = "farmstead";
      }
    }
  }

  // A block's plots come from the block planner, not from the subdivision.
  //
  // The subdivision cuts a block into pieces by bisecting it, which is a way
  // of dividing AREA and not a way of laying out a street: the pieces face
  // wherever their cut happened to leave them, so the houses on them stood at
  // unrelated angles and the street read as a scatter rather than a frontage.
  // The block planner lays a row against each edge that faces a street, cuts
  // it into plots across that frontage, and leaves the middle as a garden -
  // so the houses along one street share an axis, share party lines, and meet.
  //
  // The old lots are kept as the fallback. A block the planner refuses - too
  // small, no street on any edge - still gets built the way it always was,
  // because an unplanned block is better than a missing one.
  for (const auto &block : fabric.layout.blocks)
  {
    const uint64_t parent = districtIds.count(block.district) ? districtIds[block.district] : 0;
    const size_t beforeBlock=out.regions.size();
    addRegion(out, ids, 5, block.id, 0, parent, CityRegionKind::Block, CityLandUse::UrbanBlock, block.use, block.boundary.outer, false);
    if(out.regions.size()==beforeBlock)continue;
    blockIds[block.id] = out.regions.back().id;

    const size_t blockRegion=out.regions.size()-1;
    std::vector<std::vector<Position>> plots;
    std::vector<generator::CityBlockLot> plannedLots;
    auto ring = block.boundary.outer;
    double signedArea=0;
    for(size_t i=0;i<ring.size();++i) signedArea+=ring[i].x*ring[(i+1)%ring.size()].z-ring[(i+1)%ring.size()].x*ring[i].z;
    if(signedArea<0)std::reverse(ring.begin(),ring.end());
    out.regions[blockRegion].blockStrategy=CityBlockStrategy::Infill;

    const double blockArea=std::abs(signedArea)*.5;
    const bool inwardCourt=(block.use==DistrictUse::Residential || block.use==DistrictUse::Merchant ||
      block.use==DistrictUse::Craftsmen || block.use==DistrictUse::Guild) &&
      blockArea>=800. && blockArea<=2500. && block.id%2==1;
    if (ring.size() >= 3 && (!block.frontagePacked || inwardCourt))
    {
      // An edge faces a street when a street runs along it, close and
      // parallel: a street that merely crosses the block's corner is not a
      // frontage and a row laid against it faces nothing.
      generator::CityBlockPlanInput in;
      in.id = block.id;
      in.seed = fabric.revision ^ block.id;
      in.use = block.use;
      // Match the native dwelling fitter: four 1.4 m modules and two 0.9 m setbacks.
      // Six-metre frontage strips were allocated here then rejected downstream.
      in.lotFrontageMetres = 8.4;
      in.minimumLotWidthMetres = 7.4;
      in.inwardCourt=inwardCourt;
      if(inwardCourt) { in.lotDepthMetres=10.; in.corridorMetres=2.4; }
      in.boundary = ring;
      in.streetEdges.assign(ring.size(), false);
      in.streetAxis.assign(ring.size(), Position{});

      for (size_t i = 0; i < ring.size(); ++i)
      {
        const Position a = ring[i];
        const Position b = ring[(i + 1) % ring.size()];
        const double dx = b.x - a.x, dz = b.z - a.z, run = std::hypot(dx, dz);

        if (run < 2.)
        {
          continue;
        }

        const Position mid{(a.x + b.x) * .5, 0, (a.z + b.z) * .5};

        for (const auto &street : fabric.layout.streets)
        {
          const double sx = street.b.x - street.a.x, sz = street.b.z - street.a.z;
          const double length = std::hypot(sx, sz);

          if (length < 1e-6)
          {
            continue;
          }

          const double t = std::clamp(((mid.x - street.a.x) * sx + (mid.z - street.a.z) * sz) / (length * length), 0., 1.);
          const double away = std::hypot(mid.x - street.a.x - sx * t, mid.z - street.a.z - sz * t);
          const double along = std::abs((dx * sx + dz * sz) / (run * length));

          if (away <= street.halfWidth + 3.5 && along > .82)
          {
            in.streetEdges[i] = true;
            in.streetAxis[i] = {sx / length, 0, sz / length};
            break;
          }
        }
      }

      const bool anyStreet = std::find(in.streetEdges.begin(), in.streetEdges.end(), true) != in.streetEdges.end();

      if (anyStreet)
      {
        const auto enclosed=[&]() {
          if(!walls)return false;
          const auto point=centroid(ring);
          for(const auto &circuit:walls->circuits) {
            bool inside=false;
            const auto &boundary=circuit.boundary;
            for(size_t i=0,j=boundary.size()?boundary.size()-1:0;i<boundary.size();j=i++) {
              const auto a=boundary[i],b=boundary[j];
              if((a.z>point.z)!=(b.z>point.z) &&
                 point.x<(b.x-a.x)*(point.z-a.z)/(b.z-a.z)+a.x)inside=!inside;
            }
            if(inside)return true;
          }
          return false;
        };
        // Land inside the walls is urban even when its district programme
        // says suburb. Long agricultural crofts belong outside the circuit.
        if (block.use == DistrictUse::Suburb && !enclosed())
        {
          in.crofts = true;
          in.lotFrontageMetres = in.croftFrontageMetres;
          in.lotDepthMetres = in.toftDepthMetres + in.yardDepthMetres + in.penDepthMetres + 6.;
          in.minimumLotMetres2 = 70.;
          in.sidewalkMetres = .9;
        }

        generator::CityBlockPlan planned;
        std::string blockError;

        const bool plannedOk=generator::planCityBlock(in, planned, &blockError) && !planned.lots.empty();
        double courtArea=0;
        for(const auto &court:planned.paving) {
          for(size_t i=0;i<court.size();++i)
            courtArea+=court[i].x*court[(i+1)%court.size()].z-court[(i+1)%court.size()].x*court[i].z;
        }
        courtArea=std::abs(courtArea)*.5;
        const size_t inwardHomes=std::count_if(planned.lots.begin(),planned.lots.end(),
            [](const auto &lot){return lot.courtFacing;});
        // A tiny leftover patch is not a neighbourhood square. Retain the
        // existing packed lots, or the ordinary block plan where no packed
        // lots exist, if this block cannot support an inward-facing court.
        const bool acceptedCourt=plannedOk && courtArea>=110. && inwardHomes>=3;
        if(inwardCourt && !acceptedCourt && !block.frontagePacked) {
          in.inwardCourt=false;
          in.lotDepthMetres=11.;
          planned={};
          generator::planCityBlock(in,planned,&blockError);
        }
        if ((!inwardCourt || acceptedCourt || !block.frontagePacked) && !planned.lots.empty())
        {
          plannedLots=planned.lots;
          out.regions[blockRegion].blockStrategy=in.crofts?CityBlockStrategy::Crofts:
              planned.paving.empty()?CityBlockStrategy::Frontage:CityBlockStrategy::Courtyard;
          for (const auto &lot : planned.lots)
          {
            plots.push_back(lot.role == generator::CityBlockLotRole::Cottage && lot.toft.size() >= 3 ? lot.toft : lot.boundary);
          }

          for (const auto &walk : planned.sidewalks)
            out.publicSpaces.push_back({blockIds[block.id],CityPublicSpace::Kind::Sidewalk,walk,0});
          for (const auto &court : planned.paving)
            out.publicSpaces.push_back({blockIds[block.id],CityPublicSpace::Kind::Court,court,acceptedCourt?0.:.54});

          for (const auto &garden : planned.gardens)
            out.publicSpaces.push_back({blockIds[block.id],CityPublicSpace::Kind::Garden,garden,0});
          for (const auto &lot : planned.lots) {
            for(const auto *yard : {&lot.yard,&lot.pen})
              if(yard->size()>=3)out.publicSpaces.push_back({blockIds[block.id],CityPublicSpace::Kind::Yard,*yard,0});
            if(lot.garden.size()>=3)out.publicSpaces.push_back({blockIds[block.id],CityPublicSpace::Kind::Garden,lot.garden,0});
          }

          for (const auto &corridor : planned.corridors)
          {
            if (corridor.size() >= 3)
            {
              out.alleys.push_back(corridor);
            }
          }
        }
      }
    }

    if (plots.empty())
    {
      plots.assign(block.lots.begin(), block.lots.end());
    }

    const bool authoredBuildings=std::any_of(fabric.layout.objects.begin(),fabric.layout.objects.end(),
        [&](const auto &object){return object.block==block.id && object.footprint.size()>=3;});
    if(plots.empty() && !authoredBuildings) {
      out.regions[blockRegion].blockStrategy=CityBlockStrategy::Common;
      out.publicSpaces.push_back({blockIds[block.id],CityPublicSpace::Kind::Garden,ring,0});
    }
    for (size_t i = 0; i < plots.size(); ++i) {
      const size_t before=out.regions.size();
      addRegion(out, ids, 6, block.id, i, blockIds[block.id], CityRegionKind::Lot, CityLandUse::UrbanLot, block.use, plots[i], false);
      if(out.regions.size()>before && i<plannedLots.size()) {
        out.regions.back().lotProgramme=generator::nameOf(plannedLots[i].role);
        out.regions.back().attached=plannedLots[i].before!=UINT32_MAX || plannedLots[i].after!=UINT32_MAX;
        out.regions.back().courtFacing=plannedLots[i].courtFacing;
      }
    }
    for (const auto &alley : block.alleys)
      if (alley.size() >= 3)
        out.alleys.push_back(alley);
  }
  // The market square. The layout reserved it and paved it; what stands on
  // it is planned here, where the streets that reach it are known: a gate
  // where each one arrives, an arcade between the gates, a stepped dais and
  // a monument at the centre, and the shambles and the hall as lots of the
  // square's own region, so the city builds them like any other building.
  if (fabric.layout.market.size() >= 3)
  {
    const auto &market = fabric.layout.market;
    generator::PlazaBrief brief;
    brief.id = stableId(9, polygonKey(market));
    brief.seed = fabric.revision * 31 + polygonKey(market);
    brief.ring = market;
    const Position centre = centroid(market);
    brief.level = fabric.terrain ? fabric.terrain(centre.x, centre.z) : 0.;

    const auto segmentDistance = [](const Position &p, const Position &a, const Position &b) {
      const double dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz;
      const double t = l2 > 1e-12 ? std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / l2, 0., 1.) : 0.;

      return std::hypot(p.x - (a.x + dx * t), p.z - (a.z + dz * t));
    };

    // A street arrives where one of its ends stands on the square's edge.
    for (const auto &street : fabric.layout.streets)
    {
      for (const Position end : {street.a, street.b})
      {
        size_t edge = market.size();
        double nearest = 1e300;

        for (size_t i = 0; i < market.size(); ++i)
        {
          const double d = segmentDistance(end, market[i], market[(i + 1) % market.size()]);

          if (d < nearest)
          {
            nearest = d;
            edge = i;
          }
        }

        if (edge == market.size() || nearest > std::max(1.5, street.halfWidth + .5))
        {
          continue;
        }

        const Position &a = market[edge], &b = market[(edge + 1) % market.size()];
        const double dx = b.x - a.x, dz = b.z - a.z, run = std::hypot(dx, dz);
        const double t = std::clamp(((end.x - a.x) * dx + (end.z - a.z) * dz) / (run * run), 0., 1.);
        const Position at{a.x + dx * t, 0, a.z + dz * t};
        bool known = false;

        for (const auto &entry : brief.entries)
        {
          known = known || std::hypot(entry.at.x - at.x, entry.at.z - at.z) < 5.;
        }

        if (!known)
        {
          brief.entries.push_back({at, {-dz / run, 0, dx / run}, std::clamp(street.halfWidth * 2., 3.5, 7.)});
        }
      }
    }

    generator::Ledger plazaLedger;
    CityPlazaPlan plaza;
    std::string why;

    const bool planned = generator::PlazaPlanner::plan(brief, plaza, plazaLedger, &why);

    if (std::getenv("CITY_DEBUG"))
    {
      std::fprintf(stderr, "plaza: market of %zu vertices, %zu street entries, %s\n", market.size(), brief.entries.size(),
                   planned ? plaza.notes.empty() ? "planned" : plaza.notes.back().c_str() : why.c_str());
    }

    if (planned)
    {
      const size_t before = out.regions.size();
      addRegion(out, ids, 9, polygonKey(market), 0, 0, CityRegionKind::Block, CityLandUse::UrbanBlock, DistrictUse::Civic, market, false);

      if (out.regions.size() > before)
      {
        const uint64_t square = out.regions.back().id;
        out.regions.back().blockStrategy = CityBlockStrategy::Common;
        out.publicSpaces.push_back({square, CityPublicSpace::Kind::Court, market, 0});

        size_t lot = 0;

        for (const auto *buildings : {&plaza.stalls, &plaza.halls})
        {
          for (const auto &ring : *buildings)
          {
            const size_t beforeLot = out.regions.size();
            addRegion(out, ids, 10, polygonKey(market), lot++, square, CityRegionKind::Lot, CityLandUse::UrbanLot, DistrictUse::Merchant, ring, false);

            if (out.regions.size() > beforeLot)
            {
              out.regions.back().lotProgramme = generator::nameOf(generator::CityBlockLotRole::ShopHouse);
              out.regions.back().attached = buildings == &plaza.stalls;
            }
          }
        }

        // The houses behind the arcade: rows of shop-houses with an inn, a
        // pub and a bakehouse among them, which completion already knows how
        // to build from the role name.
        for (const auto &house : plaza.lots)
        {
          const size_t beforeLot = out.regions.size();
          addRegion(out, ids, 10, polygonKey(market), lot++, square, CityRegionKind::Lot, CityLandUse::UrbanLot,
                    house.role == "civic-hall" ? DistrictUse::Civic : DistrictUse::Merchant, house.ring, false);

          if (out.regions.size() > beforeLot)
          {
            out.regions.back().lotProgramme = house.role;
            out.regions.back().attached = house.attached;
            out.regions.back().courtFacing = true;
            // The inner edge faces the square; the outer edge faces the street.
            out.regions.back().publicFrontage = {house.ring[2], house.ring[3]};
          }
        }

        out.plazas.push_back(std::move(plaza));
      }
    }
  }

  std::set<std::pair<uint64_t, uint64_t>> objectPolygons;
  for (const auto &object : fabric.layout.objects)
  {
    if (object.footprint.size() < 3 || !object.block)
      continue;
    const auto key = std::make_pair(object.block, polygonKey(object.footprint));
    if (!objectPolygons.insert(key).second)
      continue;
    const uint64_t parent = blockIds.count(object.block) ? blockIds[object.block] : 0;
    addRegion(out, ids, 7, object.id, 0, parent, CityRegionKind::Lot, CityLandUse::UrbanLot, DistrictUse::Residential, object.footprint, object.outsideWalls);
  }
  for (size_t i = 0; i < fabric.reservations.size(); ++i)
    addRegion(out, ids, 8, polygonKey(fabric.reservations[i]), i, 0, CityRegionKind::Reservation, CityLandUse::MilitaryReserve, DistrictUse::Military, fabric.reservations[i], false);

  // Explicit hierarchy contacts.
  for (const auto &r : out.regions)
    if (r.parent)
    {
      const auto it = std::find_if(out.regions.begin(), out.regions.end(), [&](const auto &p) { return p.id == r.parent; });
      if (it == out.regions.end())
      {
        if (error)
          *error = "city skeleton region names a missing parent";
        return false;
      }
      const Position p = centroid(r.boundary);
      out.regionContacts.push_back({stableId(20, r.parent, r.id), r.parent, r.id, CityRegionContactKind::Contains, p, local(p, it->boundary), local(p, r.boundary), 0});
    }

  // Spatial contacts only between peers. Containment is never mistaken for a street.
  for (size_t i = 0; i < out.regions.size(); ++i)
    for (size_t j = i + 1; j < out.regions.size(); ++j)
    {
      const auto &a = out.regions[i], &b = out.regions[j];
      if (a.kind != b.kind || a.parent != b.parent)
        continue;
      Position p;
      double length = 0;
      if (sharedBoundary(a.boundary, b.boundary, p, length))
        out.regionContacts.push_back({stableId(21, a.id, b.id), a.id, b.id, CityRegionContactKind::SharedBoundary, p, local(p, a.boundary), local(p, b.boundary), length});
    }

  std::map<uint64_t, size_t> roadNodes;
  for (const auto &street : fabric.layout.streets)
  {
    auto path = street.path;
    if (path.size() < 2)
      path = {street.a, street.b};
    addRoad(out, roadNodes, 30, street.id, CityRoadKind::Street, std::move(path), street.halfWidth);
  }
  for (size_t i = 0; i < fabric.approaches.size(); ++i)
    addRoad(out, roadNodes, 31, i + 1, CityRoadKind::Approach, fabric.approaches[i], 1.5);
  for (const auto &patch : territory.rural.patches)
    addRoad(out, roadNodes, 32, patch.id, CityRoadKind::FieldTrack, patch.access, 1.5);
  nodeRoads(out, roadNodes);
  for (auto &node : out.roadNodes)
    if (node.edges.size() > 1)
      node.kind = CityRoadNodeKind::Junction;

  std::map<uint64_t, size_t> wallNodes;
  if (walls)
    for (const auto &circuit : walls->circuits)
      for (size_t i = 0; i < circuit.boundary.size(); ++i)
      {
        const Position a = circuit.boundary[i], b = circuit.boundary[(i + 1) % circuit.boundary.size()];
        const uint64_t from = stableId(130, circuit.era, pointKey(a));
        const uint64_t to = stableId(130, circuit.era, pointKey(b));
        if (!wallNodes.count(from))
        {
          wallNodes[from] = out.wallNodes.size();
          out.wallNodes.push_back({from, circuit.era, a, {}, {}});
        }
        if (!wallNodes.count(to))
        {
          wallNodes[to] = out.wallNodes.size();
          out.wallNodes.push_back({to, circuit.era, b, {}, {}});
        }
        const uint64_t edgeId = stableId(40, circuit.era, stableId(41, std::min(pointKey(a), pointKey(b)), std::max(pointKey(a), pointKey(b))));
        out.wallEdges.push_back({edgeId, circuit.era, from, to, a, b});
        out.wallNodes[wallNodes[from]].edges.push_back(edgeId);
        out.wallNodes[wallNodes[to]].edges.push_back(edgeId);
      }
  else
    for (const auto &wall : fabric.layout.walls)
    {
      auto path = wall.path;
      if (path.size() < 2)
        path = {wall.a, wall.b};
      for (size_t i = 1; i < path.size(); ++i)
      {
        const Position a = path[i - 1], b = path[i];
        const uint64_t from = stableId(130, 1, pointKey(a));
        const uint64_t to = stableId(130, 1, pointKey(b));
        if (!wallNodes.count(from))
        {
          wallNodes[from] = out.wallNodes.size();
          out.wallNodes.push_back({from, 1, a, {}, {}});
        }
        if (!wallNodes.count(to))
        {
          wallNodes[to] = out.wallNodes.size();
          out.wallNodes.push_back({to, 1, b, {}, {}});
        }
        const uint64_t edgeId = stableId(40, wall.id, stableId(41, std::min(pointKey(a), pointKey(b)), std::max(pointKey(a), pointKey(b))));
        out.wallEdges.push_back({edgeId, 1, from, to, a, b});
        out.wallNodes[wallNodes[from]].edges.push_back(edgeId);
        out.wallNodes[wallNodes[to]].edges.push_back(edgeId);
      }
    }

  // A gate is a shared wall/road graph contact. Snap to existing nodes where
  // possible; otherwise create explicit nodes at the stored opening midpoint.
  struct GateInput
  {
    uint64_t id = 0, era = 0;
    Position point;
  };
  std::vector<GateInput> gateInputs;
  if (walls)
    for (const auto &gate : walls->gates)
      gateInputs.push_back({gate.id, gate.era, {(gate.a.x + gate.b.x) * .5, (gate.a.y + gate.b.y) * .5, (gate.a.z + gate.b.z) * .5}});
  else
    for (const auto &gate : fabric.gates)
      gateInputs.push_back({gate.id, 1, gate.position});
  for (const auto &gate : gateInputs)
    {
      // Layout exits also use CityGate, including unwalled villages. They are
      // road entrances in that case, not contacts with a wall graph.
      if (out.wallEdges.empty())
        continue;
      const Position p = gate.point;
      uint64_t wallNode = 0;
      double wallD = 1e100;
      for (const auto &edge : out.wallEdges)
      {
        const Position q = closestPoint(p, edge.a, edge.b);
        if (distance2(p, q) < wallD)
        {
          wallD = distance2(p, q);
          wallNode = distance2(q, edge.a) < distance2(q, edge.b) ? edge.from : edge.to;
        }
      }
      uint64_t roadNode = 0;
      double roadD = 1e100;
      // A planned road may cross an opening between its existing vertices.
      // Split that road at the contact without adding any new road geometry.
      for (size_t i = 0; i < out.roadEdges.size(); ++i)
      {
        const auto edge = out.roadEdges[i];
        const auto q = closestPoint(p, edge.path.front(), edge.path.back());
        if (distance2(p, q) > epsilon * epsilon ||
            distance2(p, edge.path.front()) <= epsilon * epsilon ||
            distance2(p, edge.path.back()) <= epsilon * epsilon)
          continue;
        for (auto id : {edge.from, edge.to})
        {
          auto &incident = out.roadNodes[roadNodes[id]].edges;
          incident.erase(std::remove(incident.begin(), incident.end(), edge.id), incident.end());
        }
        out.roadEdges.erase(out.roadEdges.begin() + i);
        addRoad(out, roadNodes, 34, edge.source, edge.kind, {edge.path.front(), p}, edge.halfWidth);
        addRoad(out, roadNodes, 34, edge.source, edge.kind, {p, edge.path.back()}, edge.halfWidth);
        break;
      }
      for (const auto &node : out.roadNodes)
        if (distance2(p, node.position) < roadD)
        {
          roadD = distance2(p, node.position);
          roadNode = node.id;
        }
      if (!wallNode)
      {
        if (error)
          *error = "city skeleton gate cannot contact the wall graph";
        return false;
      }

      // The skeleton records planned circulation; it must not invent a
      // straight shortcut from a gate to an arbitrary nearby road node.
      // An unserved opening remains explicit and disconnected for validation.
      if (!roadNode || roadD > epsilon * epsilon)
      {
        roadNode = stableId(120, pointKey(p));
        while (roadNodes.count(roadNode))
          roadNode = mix(roadNode, gate.id);
        roadNodes[roadNode] = out.roadNodes.size();
        out.roadNodes.push_back({roadNode, CityRoadNodeKind::Gate, p, {}});
      }
      out.gates.push_back({stableId(50, gate.id, gate.era), gate.id, wallNode, roadNode, gate.era, p});
      out.wallNodes[wallNodes[wallNode]].gates.push_back(out.gates.back().id);
      out.roadNodes[roadNodes[roadNode]].kind = CityRoadNodeKind::Gate;
    }

  auto byId = [](const auto &a, const auto &b) { return a.id < b.id; };
  std::sort(out.regions.begin(), out.regions.end(), byId);
  std::sort(out.regionContacts.begin(), out.regionContacts.end(), byId);
  std::sort(out.regionAccess.begin(), out.regionAccess.end(), byId);
  std::sort(out.roadNodes.begin(), out.roadNodes.end(), byId);
  std::sort(out.roadEdges.begin(), out.roadEdges.end(), byId);
  std::sort(out.wallNodes.begin(), out.wallNodes.end(), byId);
  std::sort(out.wallEdges.begin(), out.wallEdges.end(), byId);
  std::sort(out.gates.begin(), out.gates.end(), byId);
  result = std::move(out);
  return true;
}

const char *cityLandUseName(CityLandUse use)
{
  static constexpr const char *names[] = {"urban-district", "urban-block", "urban-lot", "arable", "pasture", "orchard", "vineyard", "common", "managed-wood", "wild-land", "wetland", "floodplain", "quarry", "clay-pit", "cemetery", "mill", "farmstead", "road-reserve", "wall-clearance", "moat", "harbour", "shore", "military-reserve", "future-growth", "unusable", "water"};
  return names[size_t(use)];
}

const char *cityRegionKindName(CityRegionKind kind)
{
  static constexpr const char *names[] = {"district", "block", "lot", "rural-patch", "field", "farmyard", "reservation", "exterior-parcel"};
  return names[size_t(kind)];
}
} // namespace procedural::world::rewrite
