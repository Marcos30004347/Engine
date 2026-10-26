#include "WorldRoads.hpp"

#include "procedural/world/MapHierarchy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

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

  const GeneratedWorld *worldPointer = &world;
  const auto terrain = [worldPointer, land](double x, double z) {
    return land->onLand({x, 0, z}) ? worldPointer->surface.sample({x, z}).elevationMetres : std::numeric_limits<double>::infinity();
  };
  const double sample = std::max(8.0, settings.sampleStepMetres * .25);
  const auto resistance = [terrain, sample](double x, double z) {
    const double h = terrain(x, z), hx0 = terrain(x - sample, z), hx1 = terrain(x + sample, z), hz0 = terrain(x, z - sample), hz1 = terrain(x, z + sample);
    if (!std::isfinite(h) || !std::isfinite(hx0) || !std::isfinite(hx1) || !std::isfinite(hz0) || !std::isfinite(hz1))
      return std::numeric_limits<double>::infinity();
    const double gx = (hx1 - hx0) / (2 * sample), gz = (hz1 - hz0) / (2 * sample);
    return gx * gx + gz * gz;
  };

  RoadNetwork routed;
  for (const auto &link : kingdom.links)
  {
    const double total = std::hypot(link.b.x - link.a.x, link.b.z - link.a.z);
    const int legs = std::max(1, int(std::ceil(total / std::max(100.0, settings.maximumRoutingLegMetres))));
    for (int leg = 0; leg < legs; ++leg)
    {
      const double ta = double(leg) / legs, tb = double(leg + 1) / legs;
      Position a{link.a.x + (link.b.x - link.a.x) * ta, 0, link.a.z + (link.b.z - link.a.z) * ta};
      Position b{link.a.x + (link.b.x - link.a.x) * tb, 0, link.a.z + (link.b.z - link.a.z) * tb};
      a.y = terrain(a.x, a.z);
      b.y = terrain(b.x, b.z);
      rewrite::RoadProduct design;
      if (!rewrite::routeRoad(a, b, terrain, resistance, settings.routing, design, error))
      {
        if (design.sections.size() < 2)
        {
          if (error)
            *error = "kingdom link " + std::to_string(link.from) + " -> " + std::to_string(link.to) + ", leg " + std::to_string(leg + 1) + "/" + std::to_string(legs) + ": " + *error;
          return false;
        }
        ledger.add(LedgerKind::Capacity, link.from,
                   "world road centreline routed but structural profile deferred for link " + std::to_string(link.from) + " -> " + std::to_string(link.to) + ": " + (error ? *error : std::string("profile rejected")));
        if (error) error->clear();
      }
      ++routed.modules;
      for (size_t i = 1; i < design.sections.size(); ++i)
        routed.segments.push_back({design.sections[i - 1].center, design.sections[i].center, link.trunk, uint32_t(leg)});
    }
  }
  output = std::move(routed);
  const uint32_t highways = uint32_t(std::count_if(output.segments.begin(), output.segments.end(), [](const auto &s) { return s.highway; }));
  ledger.add(
      LedgerKind::Repaired, 0, "world roads: routed and smoothed " + std::to_string(highways) + " highway and " + std::to_string(output.segments.size() - highways) + " branch segments from " + std::to_string(kingdom.links.size()) + " required links");
  return true;
}

} // namespace procedural::world::generator
