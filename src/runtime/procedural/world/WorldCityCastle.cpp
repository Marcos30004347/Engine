#include "WorldCityCastle.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace procedural::world::rewrite {
namespace {
double distance2(const Position &a, const Position &b)
{
  const double x = a.x - b.x, z = a.z - b.z;
  return x * x + z * z;
}

double cross(const Position &a, const Position &b, const Position &p)
{
  return (b.x - a.x) * (p.z - a.z) - (b.z - a.z) * (p.x - a.x);
}

bool overlaps(const Position &a, const Position &b, const Position &c, const Position &d, double tolerance)
{
  const double length = std::hypot(b.x - a.x, b.z - a.z);
  if (length <= tolerance || std::abs(cross(a, b, c)) > tolerance * length || std::abs(cross(a, b, d)) > tolerance * length)
    return false;
  const double ux = (b.x - a.x) / length, uz = (b.z - a.z) / length;
  double c0 = (c.x - a.x) * ux + (c.z - a.z) * uz;
  double c1 = (d.x - a.x) * ux + (d.z - a.z) * uz;
  if (c0 > c1) std::swap(c0, c1);
  return std::min(length, c1) - std::max(0.0, c0) > tolerance;
}

bool inside(const Position &p, const std::vector<Position> &poly)
{
  bool result = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    if ((poly[i].z > p.z) != (poly[j].z > p.z) &&
        p.x < (poly[j].x - poly[i].x) * (p.z - poly[i].z) / (poly[j].z - poly[i].z) + poly[i].x)
      result = !result;
  return result;
}

Position centre(const std::vector<Position> &p)
{
  Position c;
  for (const auto &v : p) { c.x += v.x; c.y += v.y; c.z += v.z; }
  if (!p.empty()) { c.x /= p.size(); c.y /= p.size(); c.z /= p.size(); }
  return c;
}
} // namespace

bool composeCityCastles(const SettlementFabric &fabric, const CitySkeleton &skeleton, const CityCastleSettings &settings, CityCastlePlan &result, std::string *error)
{
  CityCastlePlan out;
  out.revision = std::max(fabric.revision, skeleton.revision);
  for (const auto &compound : fabric.compounds)
  {
    if (!compound.solved || compound.kind != generator::CompoundKind::Castle)
      continue;
    CityCastleTopology castle;
    castle.id = compound.id;
    castle.compound = compound.id;
    castle.precinct = compound.precinct;
    castle.enclosure = compound.enclosure;
    castle.gate = compound.gate;
    for (const auto &wall : skeleton.wallEdges)
      for (size_t i = 0; i < castle.enclosure.size(); ++i)
        if (overlaps(castle.enclosure[i], castle.enclosure[(i + 1) % castle.enclosure.size()], wall.a, wall.b, settings.wallContactTolerance))
        {
          castle.sharedWallEdges.push_back(wall.id);
          break;
        }
    auto forced = settings.modes.find(compound.id);
    if (forced != settings.modes.end())
      castle.mode = forced->second;
    else if (!castle.sharedWallEdges.empty())
      castle.mode = CityCastleMode::WallAttached;
    else if (inside(centre(castle.precinct), fabric.polygon.outer))
      castle.mode = CityCastleMode::InteriorKeep;
    else
      castle.mode = CityCastleMode::DetachedHillfort;

    std::vector<std::pair<double, uint64_t>> roads;
    for (const auto &node : skeleton.roadNodes)
      roads.push_back({distance2(castle.gate, node.position), node.id});
    std::sort(roads.begin(), roads.end());
    if (!roads.empty()) castle.cityAccessNode = roads[0].second;
    if (roads.size() > 1) castle.exteriorAccessNode = roads[1].second;
    else castle.exteriorAccessNode = castle.cityAccessNode;

    if (castle.mode == CityCastleMode::WallAttached && castle.sharedWallEdges.empty())
    {
      if (error) *error = "wall-attached castle does not share an exact wall interval";
      return false;
    }
    if (castle.mode == CityCastleMode::WallAttached && (!castle.cityAccessNode || !castle.exteriorAccessNode || castle.cityAccessNode == castle.exteriorAccessNode))
    {
      if (error) *error = "wall-attached castle requires distinct city and exterior access nodes";
      return false;
    }
    out.castles.push_back(std::move(castle));
  }
  std::sort(out.castles.begin(), out.castles.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  result = std::move(out);
  if (error) error->clear();
  return true;
}

const char *cityCastleModeName(CityCastleMode mode)
{
  static constexpr const char *names[] = {"interior-keep", "wall-attached", "outside-gate", "detached-hillfort", "harbour-fort"};
  return names[size_t(mode)];
}
} // namespace procedural::world::rewrite
