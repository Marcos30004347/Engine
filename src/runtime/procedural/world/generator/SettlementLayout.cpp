#include "SettlementLayout.hpp"

#include "LotSplitter.hpp"
#include "RoadGraph.hpp"
#include "RoadGrowth.hpp"
#include "WardBuildings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace procedural::world::generator {

namespace {

using rewrite::CityLayout;
using rewrite::CityPlanDistrict;
using rewrite::CityStreet;
using rewrite::CityStreetBlock;
using rewrite::DistrictUse;
using rewrite::SurfaceContour;
using rewrite::SurfaceRole;

double distance(Position a, Position b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
}

Position centroidOf(const ConvexRing &p)
{
  Position c{0, 0, 0};

  for (auto v : p)
  {
    c.x += v.x / p.size();
    c.z += v.z / p.size();
  }

  return c;
}

ConvexRing ribbon(Position a, Position b, double halfWidth)
{
  const double l = distance(a, b), x = -(b.z - a.z) / l * halfWidth, z = (b.x - a.x) / l * halfWidth;
  return {{a.x - x, 0, a.z - z}, {b.x - x, 0, b.z - z}, {b.x + x, 0, b.z + z}, {a.x + x, 0, a.z + z}};
}

// Watabou's ward class per use (see TownLayout), scaled from his units.
struct WardClass
{
  double minSq, gridChaos, sizeChaos, emptyProb;
};

WardClass wardClass(DistrictUse use, double unit, double r1, double r2, double r3)
{
  switch (use)
  {
  case DistrictUse::Suburb:
  case DistrictUse::WorkerHousing:
    return {(10 + 30 * r1 * r2) * unit, .6 + .4 * r3, .8, .03};
  case DistrictUse::Merchant:
  case DistrictUse::Guild:
    return {(50 + 60 * r1 * r2) * unit, .5 + .3 * r3, .7, .15};
  case DistrictUse::WealthyResidential:
  case DistrictUse::Manor:
    return {(80 + 30 * r1 * r2) * unit, .5 + .3 * r3, .8, .2};
  case DistrictUse::Civic:
    return {(80 + 30 * r1 * r2) * unit, .1 + .3 * r3, .3, .04};
  default:
    return {(10 + 80 * r1 * r2) * unit, .5 + .2 * r3, .6, .04};
  }
}

double lotTarget(DistrictUse use)
{
  switch (use)
  {
  case DistrictUse::Merchant:
  case DistrictUse::Guild:
    return 900;
  case DistrictUse::WealthyResidential:
  case DistrictUse::Manor:
    return 1400;
  case DistrictUse::Suburb:
  case DistrictUse::WorkerHousing:
    return 420;
  case DistrictUse::Craftsmen:
    return 640;
  default:
    return 560;
  }
}

} // namespace

SettlementLayout::SettlementLayout(uint64_t domain) : Generator("settlement.layout", domain, "settlement.layout-input", "city.layout", GenerationScope::Patch)
{
}

bool SettlementLayout::plan(const SettlementLayoutInput &s, CityLayout &out, Ledger &ledger, std::string *error)
{
  auto fail = [&](const std::string &message) {
    ledger.add(LedgerKind::Rejected, 0, "settlement layout: " + message);

    if (error != nullptr)
    {
      *error = message;
    }

    return false;
  };

  if (s.region.size() < 3 || !s.random)
  {
    return fail("a settlement needs a region and a random source");
  }

  CityLayout r;
  r.revision = s.revision;
  r.envelope.outer = s.region;

  // 1. Demand: the buildings the population needs (homes by household, a
  // share of workshops and shops), and the built area they will take.
  const uint32_t homes = std::max(4u, uint32_t(std::ceil(s.population / s.householdSize)));
  const uint32_t workshops = std::max(1u, homes / 4), shops = std::max(1u, homes / 8);
  const uint32_t demand = homes + workshops + shops;
  const double demandArea = demand * 95 * 2.4; // Footprint, alleys, yards and lots around it.

  // 2. District outline roads: the road L-system inside the region, seeded
  // from the centre outward and from every exit toward the centre.
  auto insideRegion = [&](Position p) {
    if (!insideEvenOdd(s.region, p))
    {
      return false;
    }

    for (const auto &w : s.water)
    {
      if (insideEvenOdd(w, p))
      {
        return false;
      }
    }

    return true;
  };

  // The settlement's own area: a disc about the centre sized by the demand
  // (with a margin for roads and squares), inside the region. The roads grow
  // within it; the exterior beyond is left to the suburbs and the farms.
  // A village is loose: its farmsteads sit along a few roads, so it spreads
  // over several times the demand's area and grows few branches.
  const double spread = s.farmsteads ? 16. : 1.7;
  const double radius = std::max(s.farmsteads ? 200. : 70., std::sqrt(demandArea * spread / 3.14159265358979));
  // The disc's rim wanders with the bearing so the outer roads stop on an
  // irregular outline, not a circle.
  const double wobble1 = s.random(3, 42) * 6.283185307179586, wobble2 = s.random(4, 42) * 6.283185307179586;
  auto insideTown = [&](Position p) {
    const double bearing = std::atan2(p.z - s.centre.z, p.x - s.centre.x);
    const double rim = radius * (1 + .12 * std::sin(3 * bearing + wobble1) + .08 * std::sin(5 * bearing + wobble2));
    return insideRegion(p) && distance(p, s.centre) <= rim;
  };

  RoadGrowthInput growth;
  growth.pattern = RoadPattern::Basic;
  growth.highwayLength = s.farmsteads ? s.mainRoadLength * 1.3 : s.mainRoadLength;
  growth.streetLength = s.farmsteads ? s.minorRoadLength * 1.6 : s.minorRoadLength;
  growth.intersectionSnapMetres = s.minorRoadLength * .35;
  growth.roadSnapMetres = s.minorRoadLength * .3;
  growth.highwayBranchProbability = s.farmsteads ? 0 : .3;
  growth.streetBranchProbability = s.farmsteads ? .45 : .8;
  growth.streetDelay = s.farmsteads ? 3 : 1;
  growth.branchDensityFloor = s.farmsteads ? .5 : .7;
  growth.maximumSegments = s.farmsteads ? uint32_t(std::clamp(demand * 2. + 12, 24., 300.)) : uint32_t(std::clamp(3.14159265358979 * radius * radius / s.blockTargetArea * 6 + 32, 64., 1200.));
  growth.onLand = insideTown;
  growth.covered = [&](Position a, Position b) {
    for (int k = 0; k <= 6; ++k)
    {
      const double t = k / 6.;

      if (!insideTown({a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t}))
      {
        return false;
      }
    }

    return true;
  };
  growth.terrain = s.terrain ? s.terrain : [](double, double) {
    return 0.;
  };
  double reach = 1;

  for (auto v : s.region)
  {
    reach = std::max(reach, distance(v, s.centre));
  }

  (void)reach;
  growth.density = [&, radius](Position p) {
    return s.farmsteads ? std::clamp(1.4 - distance(p, s.centre) / (radius * .7), 0., 1.) : std::max(.15, 1 - distance(p, s.centre) / (radius * 1.2));
  };
  growth.random = s.random;

  // Main roads leave the centre toward every exit (and fill the compass when
  // there are few exits); the world's roads continue them beyond the town.
  std::vector<double> bearings;

  for (auto exit : s.exits)
  {
    bearings.push_back(std::atan2(exit.z - s.centre.z, exit.x - s.centre.x));
  }

  // A village lies along a through road: two or three roads meet at its centre.
  const size_t spokes = s.farmsteads ? 3 : 4;

  for (unsigned k = 0; bearings.size() < spokes; ++k)
  {
    bearings.push_back(6.283185307179586 * k / spokes + .3 + s.random(k + 1, 41) * .6);
  }

  for (size_t k = 0; k < bearings.size(); ++k)
  {
    growth.seeds.push_back({{s.centre.x + std::cos(bearings[k]) * 10, 0, s.centre.z + std::sin(bearings[k]) * 10}, bearings[k], true, 0});
  }

  RoadNetwork network;

  if (!RoadGrowth::grow(growth, network, ledger, error))
  {
    return false;
  }

  // 3. The edge graph and its faces.
  RoadGraph graph(.6);

  for (const auto &segment : network.segments)
  {
    if (!graph.slivers(segment.a, segment.b, s.minimumFaceWidth) || segment.highway)
    {
      graph.add(segment.a, segment.b, segment.highway ? 0 : 1, segment.highway ? s.mainRoadHalfWidth : s.minorRoadHalfWidth);
    }
  }

  // Dead ends are extended to the first road they meet, so the roads close
  // into faces instead of leaving spurs inside them.
  uint32_t closed = 0;

  for (uint32_t pass = 0; pass < 2; ++pass)
  {
    const size_t vertexCount = graph.vertices().size();

    for (uint32_t v = 0; v < vertexCount; ++v)
    {
      if (graph.degree(v) != 1)
      {
        continue;
      }

      const auto &edge = graph.edges()[0];
      (void)edge;
      uint32_t neighbour = UINT32_MAX;

      for (const auto &e : graph.edges())
      {
        if (e.a == v)
        {
          neighbour = e.b;
        }
        else if (e.b == v)
        {
          neighbour = e.a;
        }
      }

      if (neighbour == UINT32_MAX)
      {
        continue;
      }

      const Position from = graph.vertices()[v], back = graph.vertices()[neighbour];
      const double l = distance(from, back);

      if (l < 1e-6)
      {
        continue;
      }

      const double dx = (from.x - back.x) / l, dz = (from.z - back.z) / l;
      Position hit;

      if (graph.firstHit(from, dx, dz, s.minorRoadLength * s.deadEndReach, hit) && insideTown({(from.x + hit.x) * .5, 0, (from.z + hit.z) * .5}))
      {
        graph.add(from, hit, 1, s.minorRoadHalfWidth);
        ++closed;
      }
    }
  }

  std::vector<RoadGraphFace> faces = graph.faces();
  std::vector<RoadGraphFace> districts;

  for (auto &face : faces)
  {
    const Position c = centroidOf(face.ring);

    if (face.area < 150 || !insideRegion(c) || face.area > 3.14159265358979 * radius * radius * .6)
    {
      continue;
    }

    districts.push_back(std::move(face));
  }

  if (districts.empty() && !s.farmsteads)
  {
    return fail("the roads enclose no district");
  }

  // 4. Districts: a use per face by heuristics against the demand. The most
  // central face small enough is the market square; faces on main roads are
  // craftsmen; the next ring merchants and wealthy; the rest homes; faces
  // beyond the demand are outskirts.
  std::vector<std::pair<double, size_t>> byDistance;

  for (size_t i = 0; i < districts.size(); ++i)
  {
    byDistance.emplace_back(distance(centroidOf(districts[i].ring), s.centre), i);
  }

  std::sort(byDistance.begin(), byDistance.end());
  std::vector<DistrictUse> uses(districts.size(), DistrictUse::Residential);
  std::vector<bool> market(districts.size(), false);
  double placedArea = 0;
  bool marketPlaced = false;

  for (size_t rank = 0; rank < byDistance.size(); ++rank)
  {
    const size_t i = byDistance[rank].second;
    const auto &face = districts[i];
    bool onMain = false;

    for (auto e : face.edges)
    {
      onMain |= graph.edges()[e].type == 0;
    }

    double perimeter = 0;

    for (size_t k = 0; k < face.ring.size(); ++k)
    {
      perimeter += distance(face.ring[k], face.ring[(k + 1) % face.ring.size()]);
    }

    const double compactness = 4 * 3.14159265358979 * face.area / std::max(1e-9, perimeter * perimeter);

    if (!marketPlaced && face.area <= s.marketArea && face.area >= 400 && compactness >= .45)
    {
      market[i] = true;
      uses[i] = DistrictUse::Civic;
      marketPlaced = true;
      continue;
    }

    if (placedArea >= demandArea)
    {
      uses[i] = DistrictUse::Suburb; // Beyond the demand: land, no buildings yet.
      continue;
    }

    const double t = double(rank) / std::max<size_t>(1, byDistance.size() - 1);
    const double roll = s.random(uint64_t(i) + 7, 51);

    if (t < .25 && onMain)
    {
      uses[i] = roll < .5 ? DistrictUse::Merchant : DistrictUse::Craftsmen;
    }
    else if (t < .35)
    {
      uses[i] = roll < .4 ? DistrictUse::WealthyResidential : DistrictUse::Residential;
    }
    else if (onMain)
    {
      uses[i] = roll < .6 ? DistrictUse::Craftsmen : DistrictUse::Residential;
    }
    else
    {
      uses[i] = roll < .2 ? DistrictUse::WorkerHousing : DistrictUse::Residential;
    }

    placedArea += face.area;
  }

  // Streets from the graph edges.
  uint64_t streetId = 1000;

  for (const auto &edge : graph.edges())
  {
    CityStreet street;
    street.id = streetId++;
    street.a = graph.vertices()[edge.a];
    street.b = graph.vertices()[edge.b];
    street.halfWidth = edge.halfWidth;
    street.arterial = edge.type == 0;
    street.hierarchy = edge.type == 0 ? 2 : 0;
    street.path = {street.a, street.b};
    r.streets.push_back(std::move(street));
  }

  // 5 and 6. Lots and buildings per district.
  uint64_t blockId = 1, districtId = 1;
  uint32_t buildings = 0, lotsTotal = 0, alleys = 0;

  if (s.farmsteads)
  {
    // A village: no blocks. Lots are strung along the roads and paths, one
    // side or both, a farmstead each, the nearest to the centre first, until
    // the demand is met; the land between stays fields.
    // Roads chained through their pass-through vertices into polylines, so a
    // lot can sit across the graph's short pieces.
    struct Chain
    {
      std::vector<Position> points;
      std::vector<uint32_t> edges;
      double rank = 0, halfWidth = 0;
    };

    std::vector<Chain> chains;
    std::vector<uint8_t> used(graph.edges().size(), 0);

    auto follow = [&](uint32_t startVertex, uint32_t firstEdge) {
      Chain chain;
      uint32_t v = startVertex, e = firstEdge;
      chain.points.push_back(graph.vertices()[v]);
      chain.halfWidth = graph.edges()[e].halfWidth;

      for (size_t guard = 0; guard < graph.edges().size() && !used[e]; ++guard)
      {
        used[e] = 1;
        chain.edges.push_back(e);
        v = graph.edges()[e].a == v ? graph.edges()[e].b : graph.edges()[e].a;
        chain.points.push_back(graph.vertices()[v]);

        if (graph.degree(v) != 2)
        {
          break;
        }

        uint32_t next = UINT32_MAX;

        for (uint32_t k = 0; k < graph.edges().size(); ++k)
        {
          if (k != e && !used[k] && (graph.edges()[k].a == v || graph.edges()[k].b == v))
          {
            next = k;
          }
        }

        if (next == UINT32_MAX)
        {
          break;
        }

        e = next;
      }

      double best = 1e30;

      for (auto p : chain.points)
      {
        best = std::min(best, distance(p, s.centre));
      }

      chain.rank = best + (graph.edges()[firstEdge].type == 0 ? 0 : 25);
      chains.push_back(std::move(chain));
    };

    for (uint32_t v = 0; v < graph.vertices().size(); ++v)
    {
      if (graph.degree(v) == 2)
      {
        continue;
      }

      for (uint32_t e = 0; e < graph.edges().size(); ++e)
      {
        if (!used[e] && (graph.edges()[e].a == v || graph.edges()[e].b == v))
        {
          follow(v, e);
        }
      }
    }

    for (uint32_t e = 0; e < graph.edges().size(); ++e)
    {
      if (!used[e])
      {
        follow(graph.edges()[e].a, e); // A closed loop of pass-through vertices.
      }
    }

    std::sort(chains.begin(), chains.end(), [](const Chain &x, const Chain &y) {
      return x.rank < y.rank;
    });

    std::vector<ConvexRing> taken;
    const double frontageLength = 26, depth = 32, gap = 6;
    uint64_t key = 1;
    uint32_t rejectedRegion = 0, rejectedRoad = 0, rejectedTaken = 0, slotsTried = 0, skipped = 0;

    for (const auto &chain : chains)
    {
      if (buildings >= demand)
      {
        break;
      }

      std::vector<double> at{0};

      for (size_t k = 1; k < chain.points.size(); ++k)
      {
        at.push_back(at.back() + distance(chain.points[k - 1], chain.points[k]));
      }

      const double l = at.back();

      if (l < frontageLength + gap)
      {
        continue;
      }

      const uint32_t slots = uint32_t((l - gap) / (frontageLength + gap));
      const double start = (l - slots * (frontageLength + gap) + gap) * .5;

      for (uint32_t slot = 0; slot < slots && buildings < demand; ++slot)
      {
        const double tm = start + slot * (frontageLength + gap) + frontageLength * .5;
        size_t piece = 0;

        while (piece + 2 < at.size() && at[piece + 1] < tm)
        {
          ++piece;
        }

        const Position pa = chain.points[piece], pb = chain.points[piece + 1];
        const double pl = std::max(1e-9, distance(pa, pb));
        const double ux = (pb.x - pa.x) / pl, uz = (pb.z - pa.z) / pl;
        const Position mid{pa.x + ux * (tm - at[piece]), 0, pa.z + uz * (tm - at[piece])};

        for (int side = -1; side <= 1; side += 2)
        {
          ++slotsTried;

          if (buildings >= demand || s.random(key++, 131) < .3)
          {
            ++skipped;
            continue; // Not every slot is taken: the village is loose.
          }

          const double nx = -uz * side, nz = ux * side;
          const double setback = chain.halfWidth + 1.5, d = depth * (.8 + .4 * s.random(key++, 132)), half = frontageLength * .5;
          const ConvexRing lot{
            {mid.x - ux * half + nx * setback, 0, mid.z - uz * half + nz * setback},
            {mid.x + ux * half + nx * setback, 0, mid.z + uz * half + nz * setback},
            {mid.x + ux * half + nx * (setback + d), 0, mid.z + uz * half + nz * (setback + d)},
            {mid.x - ux * half + nx * (setback + d), 0, mid.z - uz * half + nz * (setback + d)},
          };
          const ConvexRing ring = signedArea(lot) > 0 ? lot : ConvexRing{lot[3], lot[2], lot[1], lot[0]};
          bool free = true;

          for (auto v : ring)
          {
            free &= insideRegion(v);
          }

          if (!free)
          {
            ++rejectedRegion;
            continue;
          }

          for (uint32_t e = 0; e < graph.edges().size(); ++e)
          {
            const auto &edge = graph.edges()[e];
            const Position u = graph.vertices()[edge.a], v = graph.vertices()[edge.b];

            if (e == chain.edges[piece])
            {
              continue;
            }

            // Another road through or against the lot.
            for (size_t k = 0; k < ring.size() && free; ++k)
            {
              const Position a = ring[k], b = ring[(k + 1) % ring.size()];
              const double d1 = cross(a, b, u), d2 = cross(a, b, v), d3 = cross(u, v, a), d4 = cross(u, v, b);
              free &= !((d1 > 0) != (d2 > 0) && (d3 > 0) != (d4 > 0));
            }

            free &= !strictlyInsideConvex(ring, u) && !strictlyInsideConvex(ring, v);
          }

          if (!free)
          {
            ++rejectedRoad;
            continue;
          }

          for (const auto &other : taken)
          {
            free &= !tilesOverlap(ring, other, .5);
          }

          if (!free)
          {
            ++rejectedTaken;
            continue;
          }

          taken.push_back(ring);
          CityStreetBlock block;
          block.id = blockId++;
          block.use = DistrictUse::Residential;
          block.district = 0;
          block.boundary.outer = ring;

          // The farmstead: a house square (Watabou's Farm ward, 10 m) near
          // the road, cut into a small cluster, the rest yard and fields.
          const double along = (s.random(block.id, 121) - .5) * (frontageLength - 12), back = setback + 3 + 6 * s.random(block.id, 122);
          const double angle = std::atan2(uz, ux) + (s.random(block.id, 123) - .5) * .4, side10 = 13;
          const Position centre10{mid.x + ux * along + nx * (back + 5), 0, mid.z + uz * along + nz * (back + 5)};
          ConvexRing house;

          for (auto [dx, dz] : {std::pair{-.5, -.5}, std::pair{.5, -.5}, std::pair{.5, .5}, std::pair{-.5, .5}})
          {
            const double x = dx * side10, z = dz * side10;
            house.push_back({centre10.x + x * std::cos(angle) - z * std::sin(angle), 0, centre10.z + x * std::sin(angle) + z * std::cos(angle)});
          }

          block.lots = WardBuildings::orthoBuilding(house, 9 * s.unit, .55, block.id, s.random);
          block.openLots = {ring}; // The yard and the fields behind the house.
          buildings += 1;          // One household per farmstead against the demand.
          ++lotsTotal;
          r.blocks.push_back(std::move(block));
        }
      }
    }

    if (std::getenv("CITY_DEBUG"))
    {
      std::fprintf(
          stderr,
          "village lots: %u frontages, %u slots, %u skipped, rejected region %u road %u taken %u, placed %u of %u\n",
          unsigned(chains.size()),
          slotsTried,
          skipped,
          rejectedRegion,
          rejectedRoad,
          rejectedTaken,
          lotsTotal,
          demand);
    }
  }

  for (size_t i = 0; i < districts.size() && !s.farmsteads; ++i)
  {
    const auto &face = districts[i];
    CityPlanDistrict district;
    district.id = districtId++;
    district.use = uses[i];
    district.pieces = {face.ring};
    district.area = face.area;
    r.districts.push_back(district);
    r.wards.push_back({district.id, {face.ring}});

    SurfaceContour ground;
    ground.id = uint64_t(r.contours.size() + 1);
    ground.material = uses[i] == DistrictUse::Suburb ? 16 : 11;
    ground.loop = face.ring;
    r.contours.push_back(std::move(ground));

    if (market[i])
    {
      r.reservations.push_back(face.ring);
      continue;
    }

    if (uses[i] == DistrictUse::Suburb)
    {
      continue; // Beyond the demand.
    }

    // The district polygon inset by its roads on every edge.
    ConvexRing inset = face.ring;
    std::vector<double> edgeInsets;

    for (size_t k = 0; k < face.ring.size(); ++k)
    {
      const Position a = face.ring[k], b = face.ring[(k + 1) % face.ring.size()];
      double width = s.minorRoadHalfWidth;

      for (const auto &edge : graph.edges())
      {
        const Position u = graph.vertices()[edge.a], v = graph.vertices()[edge.b];

        if ((distance(u, a) < 1e-6 && distance(v, b) < 1e-6) || (distance(u, b) < 1e-6 && distance(v, a) < 1e-6))
        {
          width = edge.halfWidth;
        }
      }

      edgeInsets.push_back(width);
    }

    LotSplitInput split;
    split.polygon = face.ring;
    split.edgeKinds.assign(face.ring.size(), LotEdgeKind::Road);
    split.targetArea = lotTarget(uses[i]);
    split.minimumWidth = 7;
    split.key = district.id;
    split.random = s.random;
    LotSplitPlan lots;
    Ledger splitLedger;
    std::string splitError;

    if (!LotSplitter::plan(split, lots, splitLedger, &splitError))
    {
      ledger.add(LedgerKind::Omitted, 0, "settlement layout: district " + std::to_string(district.id) + " has no lots: " + splitError);
      continue;
    }

    lotsTotal += uint32_t(lots.lots.size());

    for (const auto &chord : lots.chords)
    {
      CityStreet alley;
      alley.id = streetId++;
      alley.a = chord.first;
      alley.b = chord.second;
      alley.halfWidth = s.alley * .5;
      alley.path = {alley.a, alley.b};
      r.streets.push_back(std::move(alley));
      ++alleys;
    }

    for (const auto &lot : lots.lots)
    {
      CityStreetBlock block;
      block.id = blockId++;
      block.use = uses[i];
      block.district = district.id;
      block.boundary.outer = lot.ring;
      const double r1 = s.random(block.id, 101), r2 = s.random(block.id, 102), r3 = s.random(block.id, 103);
      const WardClass ward = wardClass(uses[i], s.unit, r1, r2, r3);
      WardBuildingsInput wardInput;
      wardInput.patch = lot.ring;

      for (size_t k = 0; k < lot.ring.size(); ++k)
      {
        const LotEdgeKind kind = k < lot.edgeKinds.size() ? lot.edgeKinds[k] : LotEdgeKind::Road;
        double inset = .5;

        if (kind == LotEdgeKind::Chord)
        {
          inset = s.alley * .5;
        }
        else if (kind == LotEdgeKind::Road)
        {
          inset = s.minorRoadHalfWidth;
          const Position m{(lot.ring[k].x + lot.ring[(k + 1) % lot.ring.size()].x) * .5, 0, (lot.ring[k].z + lot.ring[(k + 1) % lot.ring.size()].z) * .5};

          for (const auto &edge : graph.edges())
          {
            if (segmentDistance(m, graph.vertices()[edge.a], graph.vertices()[edge.b]) < 1e-3)
            {
              inset = edge.halfWidth;
            }
          }
        }

        wardInput.edgeInset.push_back(inset);
      }

      wardInput.minSq = ward.minSq;
      wardInput.gridChaos = ward.gridChaos;
      wardInput.sizeChaos = ward.sizeChaos;
      wardInput.emptyProb = ward.emptyProb;
      wardInput.alley = s.alley;
      wardInput.key = block.id;
      wardInput.random = s.random;
      WardBuildingsPlan wardPlan;
      Ledger wardLedger;
      std::string wardError;

      if (WardBuildings::plan(wardInput, wardPlan, wardLedger, &wardError))
      {
        block.lots = wardPlan.buildings;
        block.openLots = wardPlan.open;
      }

      buildings += uint32_t(block.lots.size());
      r.blocks.push_back(std::move(block));
    }
  }

  for (const auto &street : r.streets)
  {
    SurfaceContour c;
    c.id = uint64_t(r.contours.size() + 1);
    c.material = 1;
    c.role = SurfaceRole::Road;
    c.terrainSurface = false;
    c.plane.constant = s.terrain ? s.terrain(street.a.x, street.a.z) : 0;
    c.loop = ribbon(street.a, street.b, street.halfWidth);
    r.contours.push_back(std::move(c));
  }

  r.subdivisionCells = uint32_t(districts.size());
  ledger.add(
      LedgerKind::Repaired,
      0,
      "settlement layout: demand " + std::to_string(demand) + " buildings (" + std::to_string(int(demandArea)) + " m2, radius " + std::to_string(int(radius)) + " m), " + std::to_string(closed) + " dead ends closed, " +
          std::to_string(network.segments.size()) + " road segments, " + std::to_string(faces.size()) + " faces, " + std::to_string(districts.size()) + " districts, " + std::to_string(lotsTotal) + " lots, " +
          std::to_string(alleys) + " alleys, " + std::to_string(buildings) + " buildings");
  out = std::move(r);
  return true;
}

bool SettlementLayout::run(const SettlementLayoutInput &input, CityLayout &output, Ledger &ledger, std::string *error)
{
  return plan(input, output, ledger, error);
}

} // namespace procedural::world::generator
