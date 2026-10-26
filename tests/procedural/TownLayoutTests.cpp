#include "procedural/world/generator/TownLayout.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace procedural::world;
using namespace procedural::world::generator;

namespace {

int failures = 0, assertions = 0;

void check(bool ok, const std::string &what)
{
  ++assertions;

  if (!ok)
  {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
  else
  {
    std::printf("PASS %s\n", what.c_str());
  }
}

double distance(Position a, Position b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
}

bool insideOrOn(const ConvexRing &ring, Position p, double tolerance)
{
  return strictlyInsideConvex(ring, p) || onRing(ring, p, tolerance);
}

// A 6 x 6 grid of 40 m cells: the central sixteen are the walled core (one
// the plaza), the twenty around them the outskirts. The border is the core's
// circumference with every cell corner on it (where two core cells meet).
TownLayoutInput town()
{
  TownLayoutInput in;
  uint64_t id = 1;

  for (int row = 0; row < 6; ++row)
  {
    for (int column = 0; column < 6; ++column)
    {
      TownPatch patch;
      patch.id = id++;
      const double x0 = column * 40., z0 = row * 40.;
      patch.ring = {{x0, 0, z0}, {x0 + 40, 0, z0}, {x0 + 40, 0, z0 + 40}, {x0, 0, z0 + 40}};
      const bool core = row >= 1 && row <= 4 && column >= 1 && column <= 4;
      patch.withinWalls = core;
      patch.outskirts = !core;
      patch.plaza = row == 2 && column == 2;
      patch.use = core ? 7 : 8;
      in.patches.push_back(patch);
    }
  }

  for (double x = 40; x < 200; x += 40)
  {
    in.border.push_back({x, 0, 40});
  }

  for (double z = 40; z < 200; z += 40)
  {
    in.border.push_back({200, 0, z});
  }

  for (double x = 200; x > 40; x -= 40)
  {
    in.border.push_back({x, 0, 200});
  }

  for (double z = 200; z > 40; z -= 40)
  {
    in.border.push_back({40, 0, z});
  }

  in.gates = {{120, 0, 40}, {120, 0, 200}};
  in.exits = {{120, 0, -60}, {120, 0, 300}};
  in.centre = {120, 0, 120};
  in.walled = true;
  in.wallSectionMetres = 40;
  in.seed = 3;
  in.random = [](uint64_t id, uint64_t purpose) {
    uint64_t h = (id + 1) * 0x9E3779B97F4A7C15ULL ^ (purpose + 1) * 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 31;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 29;
    return double(h % 1000000) / 1000000.;
  };
  return in;
}

} // namespace

int main()
{
  TownLayoutInput in = town();
  rewrite::CityLayout layout;
  Ledger ledger;
  std::string error;
  const bool ok = TownLayout::plan(in, layout, ledger, &error);
  check(ok, "town layout plans a walled grid town: " + error);

  if (ok)
  {
    // Streets: both gates reach the plaza inside, and roads leave outward.
    size_t inside = 0, outside = 0;

    for (const auto &street : layout.streets)
    {
      const Position m{(street.a.x + street.b.x) * .5, 0, (street.a.z + street.b.z) * .5};
      const bool in = m.x > 40 && m.x < 200 && m.z > 40 && m.z < 200;
      (in ? inside : outside)++;
    }

    check(inside >= 2 && outside >= 2, "streets run from both gates to the plaza and roads leave both gates");

    for (const auto &street : layout.streets)
    {
      check(
          !(street.a.x > 40 && street.a.x < 120 && street.a.z > 40 && street.a.z < 120 && street.b.x > 40 && street.b.x < 120 && street.b.z > 40 && street.b.z < 120 && std::abs(street.a.x - 80) < 1e-6 &&
            std::abs(street.b.x - 80) < 1e-6 && street.a.z > 40 && street.a.z < 80 && street.b.z > 40 && street.b.z < 80),
          "no segment runs along the plaza's own edge");
    }

    // Wall: sections on the circumference, opened at the gates, towers off the gates.
    check(layout.walls.size() == in.border.size(), "one wall section per border edge");
    bool openedAtGates = true;

    for (const auto &wall : layout.walls)
    {
      for (auto gate : in.gates)
      {
        openedAtGates &= distance(wall.a, gate) > 3 && distance(wall.b, gate) > 3;
      }
    }

    check(openedAtGates, "wall sections stop short of the gates");
    bool towersOffGates = !layout.towers.empty();

    for (const auto &tower : layout.towers)
    {
      for (auto gate : in.gates)
      {
        towersOffGates &= distance(tower.center, gate) > 1;
      }
    }

    check(towersOffGates, "towers stand on the wall but never on a gate");

    // Blocks: one per patch but the plaza, each inside its patch; buildings
    // inside their block, four corners or more, none on a street.
    check(layout.blocks.size() == in.patches.size() - 1, "every patch but the plaza is a block");
    bool blocksInside = true, buildingsInside = true, noTriangles = true, offStreets = true, anyBuilding = false, anyOuter = false;

    for (const auto &block : layout.blocks)
    {
      const TownPatch *patch = nullptr;

      for (const auto &p : in.patches)
      {
        if (p.id == block.district)
        {
          patch = &p;
        }
      }

      if (patch == nullptr)
      {
        blocksInside = false;
        continue;
      }

      for (auto v : block.boundary.outer)
      {
        blocksInside &= insideOrOn(patch->ring, v, 1e-6);
      }

      for (const auto &lot : block.lots)
      {
        anyBuilding = true;
        anyOuter |= block.use == rewrite::DistrictUse::Suburb;
        noTriangles &= lot.size() >= 4;

        for (auto v : lot)
        {
          buildingsInside &= insideOrOn(block.boundary.outer, v, 1e-6);

          for (const auto &street : layout.streets)
          {
            offStreets &= segmentDistance(v, street.a, street.b) >= street.halfWidth - 1e-6;
          }
        }
      }
    }

    check(blocksInside, "blocks lie within their patches");
    check(anyBuilding && buildingsInside, "buildings lie within their blocks");
    check(noTriangles, "no building is a triangle");
    check(offStreets, "no building reaches into a street");
    check(anyOuter, "the outskirts keep buildings along their roads");
    check(!layout.contours.empty() && !layout.wards.empty() && !layout.suburbs.empty(), "ground, wards and suburbs are published for the world builder");
  }

  std::printf("TOWN_TESTS assertions=%d failures=%d\n", assertions, failures);
  return failures == 0 ? 0 : 1;
}
