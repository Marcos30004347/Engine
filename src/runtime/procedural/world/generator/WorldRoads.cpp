#include "WorldRoads.hpp"

#include "procedural/world/MapHierarchy.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::generator {

namespace {

// Land leaves bucketed on a grid, for point-in-land tests over the whole map.
struct LandIndex
{
  double minX = 0, minZ = 0, size = 1;
  uint32_t side = 64;
  std::vector<ConvexRing> polys;
  std::vector<std::vector<uint32_t>> buckets;

  size_t bucket(double x, double z) const
  {
    const int bx = std::clamp(int((x - minX) / size), 0, int(side) - 1), bz = std::clamp(int((z - minZ) / size), 0, int(side) - 1);
    return size_t(bz) * side + bx;
  }

  void build(const GeneratedWorld &world)
  {
    double x0 = 1e30, z0 = 1e30, x1 = -1e30, z1 = -1e30;

    for (auto id : leafNodes(world.graph))
    {
      const auto &node = world.graph.node(id);

      if (!node.land || node.area.size() < 3)
      {
        continue;
      }

      ConvexRing poly;

      for (auto q : node.area)
      {
        poly.push_back({q.x, 0, q.y});
        x0 = std::min(x0, q.x);
        z0 = std::min(z0, q.y);
        x1 = std::max(x1, q.x);
        z1 = std::max(z1, q.y);
      }

      if (signedArea(poly) < 0)
      {
        std::reverse(poly.begin(), poly.end());
      }

      polys.push_back(std::move(poly));
    }

    minX = x0;
    minZ = z0;
    size = std::max(x1 - x0, z1 - z0) / side + 1e-9;
    buckets.assign(size_t(side) * side, {});

    for (uint32_t i = 0; i < polys.size(); ++i)
    {
      double px0 = 1e30, pz0 = 1e30, px1 = -1e30, pz1 = -1e30;

      for (auto p : polys[i])
      {
        px0 = std::min(px0, p.x);
        pz0 = std::min(pz0, p.z);
        px1 = std::max(px1, p.x);
        pz1 = std::max(pz1, p.z);
      }

      for (int bz = int((pz0 - minZ) / size); bz <= int((pz1 - minZ) / size); ++bz)
      {
        for (int bx = int((px0 - minX) / size); bx <= int((px1 - minX) / size); ++bx)
        {
          if (bx >= 0 && bz >= 0 && bx < int(side) && bz < int(side))
          {
            buckets[size_t(bz) * side + bx].push_back(i);
          }
        }
      }
    }
  }

  bool onLand(Position p) const
  {
    if (polys.empty() || p.x < minX || p.z < minZ || p.x > minX + size * side || p.z > minZ + size * side)
    {
      return false;
    }

    for (auto i : buckets[bucket(p.x, p.z)])
    {
      if (insideEvenOdd(polys[i], p))
      {
        return true;
      }
    }

    return false;
  }
};

} // namespace

WorldRoads::WorldRoads(uint64_t domain) : Generator("world.roads", domain, "world.roads-input", "world.road-network", GenerationScope::Network)
{
}

bool WorldRoads::run(const WorldRoadsInput &input, RoadNetwork &output, Ledger &ledger, std::string *error)
{
  if (!input.world)
  {
    if (error != nullptr)
    {
      *error = "world roads need a generated world";
    }

    return false;
  }

  return grow(*input.world, input.kingdom, input.settings, output, ledger, error);
}

bool WorldRoads::grow(const GeneratedWorld &world, const rewrite::KingdomPlan &kingdom, const WorldRoadsSettings &settings, RoadNetwork &output, Ledger &ledger, std::string *error)
{
  if (kingdom.settlements.empty())
  {
    if (error != nullptr)
    {
      *error = "world roads need at least one settlement";
    }

    return false;
  }

  auto land = std::make_shared<LandIndex>();
  land->build(world);

  RoadGrowthInput growth;
  growth.pattern = RoadPattern::Elevation;
  growth.highwayLength = settings.highwayLength;
  growth.streetLength = settings.laneLength;
  growth.intersectionSnapMetres = settings.intersectionSnapMetres;
  growth.roadSnapMetres = settings.roadSnapMetres;
  growth.highwayBranchProbability = settings.highwayBranchProbability;
  growth.streetBranchProbability = settings.laneBranchProbability;
  growth.streetDelay = settings.laneDelay;
  growth.maximumSegments = settings.maximumSegments;
  growth.maximumPruneFraction = .5;
  growth.maximumRotationRadians = .7;
  growth.branchDensityFloor = 0; // Lanes only where people live.
  growth.terminalDensity = .04;  // A highway with no settlement ahead ends.

  for (const auto &s : kingdom.settlements)
  {
    growth.snapPoints.push_back(s.centre);
  }

  growth.onLand = [land](Position p) {
    return land->onLand(p);
  };

  const double step = std::max(10., settings.sampleStepMetres);

  growth.covered = [land, step](Position a, Position b) {
    const double l = std::hypot(b.x - a.x, b.z - a.z);
    const int samples = std::max(1, int(std::ceil(l / step)));

    for (int i = 0; i <= samples; ++i)
    {
      const double t = double(i) / samples;

      if (!land->onLand({a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t}))
      {
        return false;
      }
    }

    return true;
  };

  const GeneratedWorld *worldPointer = &world;

  growth.terrain = [worldPointer](double x, double z) {
    return worldPointer->surface.sample({x, z}).elevationMetres;
  };

  // Population density: every settlement pulls with its population, so the
  // highways bend toward the towns and the capital.
  double peak = 1;

  for (const auto &s : kingdom.settlements)
  {
    peak = std::max(peak, double(s.population));
  }

  const double sigma = std::max(100., settings.densitySigmaMetres);
  const std::vector<rewrite::KingdomSettlement> settlements = kingdom.settlements;

  growth.density = [settlements, peak, sigma](Position p) {
    double total = 0;

    for (const auto &s : settlements)
    {
      const double d2 = (p.x - s.centre.x) * (p.x - s.centre.x) + (p.z - s.centre.z) * (p.z - s.centre.z);
      total += double(s.population) / peak * std::exp(-d2 / (2 * sigma * sigma));
    }

    return std::min(1., total);
  };

  const uint64_t seed = world.graph.brief().seed;

  growth.random = [seed](uint64_t id, uint64_t purpose) {
    uint64_t h = (seed + 1) * 0x9E3779B97F4A7C15ULL ^ (id + 1) * 0xBF58476D1CE4E5B9ULL ^ (purpose + 1) * 0x94D049BB133111EBULL;
    h ^= h >> 31;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 29;
    return double(h % 1000000) / 1000000.;
  };

  // Seeds: every kingdom link sends a highway from each end toward the other
  // (trunk links first); the density fan and the terrain do the rest.
  for (const auto &link : kingdom.links)
  {
    const double bearing = std::atan2(link.b.z - link.a.z, link.b.x - link.a.x);
    growth.seeds.push_back({link.a, bearing, true, link.trunk ? 0 : 2});
    growth.seeds.push_back({link.b, bearing + 3.14159265358979, true, link.trunk ? 0 : 2});
  }

  if (!RoadGrowth::grow(growth, output, ledger, error))
  {
    return false;
  }

  uint32_t highways = 0;

  for (const auto &segment : output.segments)
  {
    highways += segment.highway;
  }

  ledger.add(
      LedgerKind::Repaired, 0, "world roads: " + std::to_string(highways) + " highway and " + std::to_string(output.segments.size() - highways) + " lane segments from " + std::to_string(kingdom.links.size()) + " links");
  return true;
}

} // namespace procedural::world::generator
