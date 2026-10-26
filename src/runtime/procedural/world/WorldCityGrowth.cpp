#include "WorldCityGrowth.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace procedural::world::rewrite {
namespace {
double cross(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

double distance(Position a, Position b)
{
  return std::hypot(a.x - b.x, a.z - b.z);
}

double signedArea(const std::vector<Position> &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  return a * .5;
}

Position centre(const std::vector<Position> &p)
{
  Position c;
  for (auto v : p) { c.x += v.x; c.y += v.y; c.z += v.z; }
  if (!p.empty()) { c.x /= p.size(); c.y /= p.size(); c.z /= p.size(); }
  return c;
}

bool inside(Position p, const std::vector<Position> &poly)
{
  bool result = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    if ((poly[i].z > p.z) != (poly[j].z > p.z) &&
        p.x < (poly[j].x - poly[i].x) * (p.z - poly[i].z) / (poly[j].z - poly[i].z) + poly[i].x)
      result = !result;
  return result;
}

std::vector<Position> hull(std::vector<Position> points)
{
  std::sort(points.begin(), points.end(), [](Position a, Position b) { return a.x == b.x ? a.z < b.z : a.x < b.x; });
  points.erase(std::unique(points.begin(), points.end(), [](Position a, Position b) { return distance(a, b) < 1e-7; }), points.end());
  if (points.size() < 3) return {};
  std::vector<Position> result;
  for (int pass = 0; pass < 2; ++pass)
  {
    const size_t base = result.size();
    for (auto p : points)
    {
      while (result.size() >= base + 2 && cross(result[result.size() - 2], result.back(), p) <= 1e-8)
        result.pop_back();
      result.push_back(p);
    }
    result.pop_back();
    std::reverse(points.begin(), points.end());
  }
  if (signedArea(result) < 0) std::reverse(result.begin(), result.end());
  return result;
}

Position rayBoundary(Position origin, Position toward, const std::vector<Position> &boundary)
{
  const double dx = toward.x - origin.x, dz = toward.z - origin.z;
  double best = 1e30;
  Position hit = toward;
  for (size_t i = 0; i < boundary.size(); ++i)
  {
    Position a = boundary[i], b = boundary[(i + 1) % boundary.size()];
    const double ex = b.x - a.x, ez = b.z - a.z;
    const double den = dx * ez - dz * ex;
    if (std::abs(den) < 1e-10) continue;
    const double t = ((a.x - origin.x) * ez - (a.z - origin.z) * ex) / den;
    const double u = ((a.x - origin.x) * dz - (a.z - origin.z) * dx) / den;
    if (t > 0 && u >= -1e-8 && u <= 1 + 1e-8 && t < best)
    {
      best = t;
      hit = {origin.x + dx * t, 0, origin.z + dz * t};
    }
  }
  return hit;
}

void finishWall(CityGrowthWall &wall, const std::vector<CityGate> &gates, double spacing)
{
  for (size_t i = 0; i < wall.boundary.size(); ++i)
  {
    const auto a = wall.boundary[i], b = wall.boundary[(i + 1) % wall.boundary.size()];
    const double length = distance(a, b);
    wall.perimeter += length;
    const unsigned count = std::max(1u, unsigned(std::ceil(length / spacing)));
    for (unsigned k = 0; k < count; ++k)
    {
      const double t = double(k) / count;
      wall.towers.push_back({a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t});
    }
  }
  wall.enclosedArea = std::abs(signedArea(wall.boundary));
  const Position c = centre(wall.boundary);
  for (const auto &gate : gates)
    wall.gates.push_back(rayBoundary(c, gate.position, wall.boundary));
}
} // namespace

bool planCityGrowth(
    const SettlementFabric &fabric,
    const CitySkeleton &skeleton,
    const CityCastlePlan &castles,
    const CityGrowthSettings &settings,
    CityGrowthPlan &result,
    std::string *error)
{
  auto fail = [&](const char *message) { if (error) *error = message; return false; };
  if (!fabric.revision || !skeleton.revision || !std::isfinite(settings.oldCoreFraction) || settings.oldCoreFraction <= 0 || settings.oldCoreFraction >= 1 ||
      !std::isfinite(settings.minimumOuterGrowth) || settings.minimumOuterGrowth < 0 || !std::isfinite(settings.towerSpacing) || settings.towerSpacing <= 0)
    return fail("invalid city growth input");

  const auto &standingBoundary = fabric.layout.envelope.outer.empty() ? fabric.polygon.outer : fabric.layout.envelope.outer;
  if (standingBoundary.size() < 3 || std::abs(signedArea(standingBoundary)) < 1)
    return fail("city growth requires a standing urban envelope");

  CityGrowthPlan out;
  out.revision = std::max({fabric.revision, skeleton.revision, castles.revision});

  // Oldest blocks nearest the centre define the absorbed circuit. Lots remain
  // attached to their blocks; only the wall lifecycle changes.
  const Position cityCentre = centre(standingBoundary);
  std::vector<const CitySkeletonRegion *> blocks;
  for (const auto &region : skeleton.regions)
    if (region.kind == CityRegionKind::Block && !region.exterior)
      blocks.push_back(&region);
  std::sort(blocks.begin(), blocks.end(), [&](const auto *a, const auto *b) {
    const double da = distance(centre(a->boundary), cityCentre), db = distance(centre(b->boundary), cityCentre);
    return da == db ? a->id < b->id : da < db;
  });
  std::vector<Position> oldPoints;
  const size_t oldCount = std::min(blocks.size(), std::max<size_t>(3, size_t(std::ceil(blocks.size() * settings.oldCoreFraction))));
  for (size_t i = 0; i < oldCount; ++i)
    oldPoints.insert(oldPoints.end(), blocks[i]->boundary.begin(), blocks[i]->boundary.end());
  auto oldBoundary = hull(std::move(oldPoints));
  if (oldBoundary.size() >= 3 && std::abs(signedArea(oldBoundary)) < std::abs(signedArea(standingBoundary)) * .92)
  {
    CityGrowthWall old{1, 1, CityWallLifecycle::Absorbed, std::move(oldBoundary)};
    finishWall(old, fabric.gates, settings.towerSpacing);
    out.traces.push_back(std::move(old));
  }

  CityGrowthWall standing{2, 2, CityWallLifecycle::Standing, standingBoundary};
  finishWall(standing, fabric.gates, settings.towerSpacing);
  out.walls.push_back(std::move(standing));

  std::vector<Position> futurePoints = standingBoundary;
  uint64_t zoneId = 1;
  for (const auto &suburb : fabric.layout.suburbs)
  {
    if (suburb.size() < 3) continue;
    CityGrowthZone zone;
    zone.id = zoneId++;
    zone.kind = CityGrowthZoneKind::Suburb;
    zone.boundary = suburb;
    zone.insideStandingWall = inside(centre(suburb), standingBoundary);
    out.zones.push_back(zone);
    futurePoints.insert(futurePoints.end(), suburb.begin(), suburb.end());
  }
  for (const auto &castle : castles.castles)
  {
    if (castle.precinct.size() < 3) continue;
    CityGrowthZone zone;
    zone.id = zoneId++;
    zone.source = castle.id;
    zone.kind = CityGrowthZoneKind::Castle;
    zone.boundary = castle.precinct;
    zone.insideStandingWall = inside(centre(castle.precinct), standingBoundary);
    out.zones.push_back(zone);
    futurePoints.insert(futurePoints.end(), castle.precinct.begin(), castle.precinct.end());
  }
  auto futureBoundary = hull(std::move(futurePoints));
  const double standingArea = std::abs(signedArea(standingBoundary));
  if (futureBoundary.size() >= 3 && std::abs(signedArea(futureBoundary)) >= standingArea * (1 + settings.minimumOuterGrowth))
  {
    CityGrowthWall future{3, 3, CityWallLifecycle::Planned, std::move(futureBoundary)};
    finishWall(future, fabric.gates, settings.towerSpacing);
    for (auto &zone : out.zones) zone.insidePlannedWall = inside(centre(zone.boundary), future.boundary);
    out.traces.push_back(std::move(future));
  }
  else
    for (auto &zone : out.zones) zone.insidePlannedWall = zone.insideStandingWall;

  // A history with dated circuits replaces the generic curtain. One with
  // none (the world story graph runs no defence ledger) keeps the fabric's
  // planned wall: an empty list is not a history of an open town.
  if (fabric.historicalEnclosures && !fabric.historicalEnclosures->empty())
  {
    // The occupancy pass supplies real dated circuits after binding history to
    // lots. No generic curtain or percentage-based old core may impersonate it.
    out.historical = true;
    out.walls.clear();
    out.traces.clear();
  }
  result = std::move(out);
  if (error) error->clear();
  return true;
}

const char *cityWallLifecycleName(CityWallLifecycle state)
{
  static constexpr const char *names[] = {"absorbed", "standing", "planned"};
  return names[size_t(state)];
}

const char *cityGrowthZoneKindName(CityGrowthZoneKind kind)
{
  static constexpr const char *names[] = {"suburb", "castle"};
  return names[size_t(kind)];
}
} // namespace procedural::world::rewrite
