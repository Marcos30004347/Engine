#include "TownLayout.hpp"

#include "RingLotPlanner.hpp"
#include "WardBuildings.hpp"

#include <algorithm>
#include <cmath>

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

  for (const auto &patch : s.patches)
  {
    if (patch.plaza)
    {
      r.reservations.push_back(patch.ring); // The plaza is open ground: the civic square.
    }
  }

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

  // 1. Streets.
  TownStreetsInput streetInput;
  streetInput.patches = patches;
  streetInput.border = s.border;
  streetInput.gates = gates;
  streetInput.exits = s.exits;
  streetInput.centre = s.centre;
  TownStreetsPlan streets;

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

  for (const auto &artery : streets.arteries)
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
  blockInput.arteries = streets.arteries;
  blockInput.wallRing = s.walled ? border : ConvexRing{};
  blockInput.gates = gates;
  blockInput.compounds = s.compounds;
  blockInput.arteryHalfWidth = s.arteryHalfWidth;
  blockInput.mainStreet = s.mainStreet;
  blockInput.regularStreet = s.regularStreet;
  blockInput.alley = s.alley;
  blockInput.seed = s.seed;
  blockInput.random = s.random;
  TownBlocksPlan blocks;

  if (!TownBlocks::plan(blockInput, blocks, ledger, error))
  {
    return false;
  }

  // 4. Buildings per block: Watabou's ward classes, court wards rung.
  uint64_t blockId = 1;
  uint32_t buildings = 0, courts = 0, thinned = 0;

  for (const auto &block : blocks.blocks)
  {
    CityStreetBlock city;
    city.id = blockId++;
    city.boundary.outer = block.ring;
    city.use = DistrictUse(block.use);
    city.district = block.patch;
    const double r1 = s.random(city.id, 101), r2 = s.random(city.id, 102), r3 = s.random(city.id, 103);
    const WardClass ward = wardClass(block.use, block.outskirts, s.unit, r1, r2, r3);
    const bool courtWard = !block.outskirts && (DistrictUse(block.use) == DistrictUse::Merchant ||
                                                ((DistrictUse(block.use) == DistrictUse::Residential || DistrictUse(block.use) == DistrictUse::Craftsmen) && s.random(city.id, 105) < s.courtWardShare));
    bool rung = false;

    if (s.villageFarmsteads)
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

    if (courtWard)
    {
      const double plot = DistrictUse(block.use) == DistrictUse::Merchant ? 7000. / 65 : 7000. / 80;
      RingLotInput ringInput;
      ringInput.block = block.ring;
      ringInput.depth = std::max(5., std::min({DistrictUse(block.use) == DistrictUse::Merchant ? 16. : 13., plot / 4.5, std::sqrt(plot * 1.6)}));
      ringInput.targetArea = plot;
      ringInput.divideCourtAbove = DistrictUse(block.use) == DistrictUse::Merchant ? 2500 : 700;
      ringInput.fillCourtBelow = DistrictUse(block.use) == DistrictUse::Merchant ? 0 : plot * 2.5;
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

      if (RingLotPlanner::plan(ringInput, ringPlan, ringLedger, &ringError) && !ringPlan.lots.empty())
      {
        city.lots = ringPlan.lots;
        city.openLots = ringPlan.open;

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
      wardInput.gridChaos = ward.gridChaos;
      wardInput.sizeChaos = ward.sizeChaos;
      wardInput.emptyProb = ward.emptyProb;
      wardInput.alley = s.alley;
      wardInput.outskirts = block.outskirts;
      wardInput.populated = block.populated;
      wardInput.vertexDensity = block.vertexDensity;
      wardInput.key = city.id;
      wardInput.random = s.random;
      WardBuildingsPlan wardPlan;
      Ledger wardLedger;
      std::string wardError;

      if (WardBuildings::plan(wardInput, wardPlan, wardLedger, &wardError))
      {
        city.lots = wardPlan.buildings;
        city.openLots = wardPlan.open;
        thinned += wardPlan.filtered;
      }
      else
      {
        ledger.add(LedgerKind::Omitted, 0, "town layout: block " + std::to_string(city.id) + " has no buildings: " + wardError);
      }
    }

    buildings += uint32_t(city.lots.size());
    r.blocks.push_back(std::move(city));
  }

  // Wards, suburbs and the ground and road contours the world builder reads.
  uint64_t contourId = 1;

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
    c.terrainSurface = false;
    c.plane.constant = s.terrain ? s.terrain(street.a.x, street.a.z) : 0;
    c.loop = ribbon(street.a, street.b, street.halfWidth);
    r.contours.push_back(std::move(c));
  }

  r.subdivisionCells = uint32_t(patches.size());
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
