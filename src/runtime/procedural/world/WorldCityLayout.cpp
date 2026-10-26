#include "WorldCityLayout.hpp"
#include "procedural/ConstrainedDelaunay.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
namespace procedural::world::rewrite
{
namespace
{
using Poly = std::vector<Position>;
double cross(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}
double dist(Position a, Position b)
{
  return std::hypot(a.x - b.x, a.z - b.z);
}
Position mix(Position a, Position b, double t)
{
  return {a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t};
}
double area(const Poly &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  return a * .5;
}
Poly clip(const Poly &p, double nx, double nz, double c)
{
  Poly r;
  if (p.empty())
    return r;
  for (size_t i = 0; i < p.size(); ++i)
  {
    auto a = p[i], b = p[(i + 1) % p.size()];
    double da = nx * a.x + nz * a.z - c, db = nx * b.x + nz * b.z - c;
    if (da <= 1e-8)
      r.push_back(a);
    if ((da < -1e-8 && db > 1e-8) || (db < -1e-8 && da > 1e-8))
      r.push_back(mix(a, b, da / (da - db)));
  }
  Poly q;
  for (auto v : r)
    if (q.empty() || dist(q.back(), v) > 1e-7)
      q.push_back(v);
  if (q.size() > 1 && dist(q.front(), q.back()) < 1e-7)
    q.pop_back();
  if (q.size() < 3 || area(q) < 1e-7)
    q.clear();
  return q;
}
bool inside(const Poly &p, Position q)
{
  for (size_t i = 0; i < p.size(); ++i)
    if (cross(p[i], p[(i + 1) % p.size()], q) < -1e-6)
      return false;
  return !p.empty();
}
Poly intersection(Poly p, const Poly &q)
{
  for (size_t i = 0; i < q.size(); ++i)
  {
    auto a = q[i], b = q[(i + 1) % q.size()];
    p = clip(p, b.z - a.z, a.x - b.x, (b.z - a.z) * a.x + (a.x - b.x) * a.z);
  }
  return p;
}
// Exact interval coverage over convex pieces catches exclusions narrower than
// the centerline sampling spacing.
bool coveredSegment(Position a, Position b, const std::vector<Poly> &pieces)
{
  std::vector<std::pair<double, double>> intervals;
  for (const auto &p : pieces)
  {
    double lo = 0, hi = 1;
    for (size_t k = 0; k < p.size(); ++k)
    {
      double da = cross(p[k], p[(k + 1) % p.size()], a), db = cross(p[k], p[(k + 1) % p.size()], b);
      if (da < -1e-8 && db < -1e-8)
      {
        hi = -1;
        break;
      }
      if (da < -1e-8)
        lo = std::max(lo, da / (da - db));
      if (db < -1e-8)
        hi = std::min(hi, da / (da - db));
    }
    if (hi >= lo)
      intervals.emplace_back(lo, hi);
  }
  std::sort(intervals.begin(), intervals.end());
  double end = 0;
  for (auto [lo, hi] : intervals)
  {
    if (lo > end + 1e-8)
      break;
    end = std::max(end, hi);
  }
  return end >= 1 - 1e-8;
}
std::vector<Poly> subtract(Poly p, const Poly &q)
{
  std::vector<Poly> r;
  for (size_t i = 0; i < q.size() && !p.empty(); ++i)
  {
    auto a = q[i], b = q[(i + 1) % q.size()];
    double nx = b.z - a.z, nz = a.x - b.x, c = nx * a.x + nz * a.z;
    auto out = clip(p, -nx, -nz, -c);
    if (!out.empty())
      r.push_back(out);
    p = clip(p, nx, nz, c);
  }
  return r;
}
Poly hull(Poly p)
{
  std::sort(
      p.begin(),
      p.end(),
      [](auto a, auto b)
      {
        return a.x == b.x ? a.z < b.z : a.x < b.x;
      });
  Poly h;
  for (auto v : p)
  {
    while (h.size() > 1 && cross(h[h.size() - 2], h.back(), v) <= 1e-8)
      h.pop_back();
    h.push_back(v);
  }
  size_t k = h.size();
  for (auto it = p.rbegin() + 1; it != p.rend(); ++it)
  {
    while (h.size() > k && cross(h[h.size() - 2], h.back(), *it) <= 1e-8)
      h.pop_back();
    h.push_back(*it);
  }
  if (h.size() > 1)
    h.pop_back();
  return h;
}
void merge(std::vector<Poly> &p)
{
  bool changed = true;
  while (changed)
  {
    changed = false;
    for (size_t i = 0; i < p.size() && !changed; ++i)
      for (size_t j = i + 1; j < p.size(); ++j)
      {
        Poly all = p[i];
        all.insert(all.end(), p[j].begin(), p[j].end());
        auto h = hull(all);
        if (std::abs(area(h) - area(p[i]) - area(p[j])) < 1e-5)
        {
          p[i] = h;
          p.erase(p.begin() + j);
          changed = true;
          break;
        }
      }
  }
}
double segmentDistance(Position p, Position a, Position b)
{
  double d = dist(a, b);
  if (d < 1e-9)
    return dist(p, a);
  return dist(p, mix(a, b, std::clamp(((p.x - a.x) * (b.x - a.x) + (p.z - a.z) * (b.z - a.z)) / (d * d), 0., 1.)));
}
bool segmentsMeet(Position a, Position b, Position c, Position d)
{
  if (std::min({segmentDistance(a, c, d), segmentDistance(b, c, d), segmentDistance(c, a, b), segmentDistance(d, a, b)}) < 1e-5)
    return true;
  return cross(a, b, c) * cross(a, b, d) < 0 && cross(c, d, a) * cross(c, d, b) < 0;
}
Poly ribbon(const CityStreet &s)
{
  double l = dist(s.a, s.b), x = -(s.b.z - s.a.z) / l * s.halfWidth, z = (s.b.x - s.a.x) / l * s.halfWidth;
  return {{s.a.x - x, 0, s.a.z - z}, {s.b.x - x, 0, s.b.z - z}, {s.b.x + x, 0, s.b.z + z}, {s.a.x + x, 0, s.a.z + z}};
}
bool convex(const Poly &p)
{
  if (p.size() < 3 || p.size() > 128 || area(p) <= 1e-6)
    return false;
  for (size_t i = 0; i < p.size(); ++i)
  {
    if (!std::isfinite(p[i].x) || !std::isfinite(p[i].z) || std::max(std::abs(p[i].x), std::abs(p[i].z)) > 1e6 || cross(p[i], p[(i + 1) % p.size()], p[(i + 2) % p.size()]) < -1e-7)
      return false;
  }
  return true;
}
} // namespace
bool cityPolygonsOverlap(const Poly &a, const Poly &b)
{
  if (a.empty() || b.empty())
    return false;
  for (auto *p : {&a, &b})
    for (size_t i = 0; i < p->size(); ++i)
    {
      auto u = (*p)[i], v = (*p)[(i + 1) % p->size()];
      bool outside = true;
      for (auto q : p == &a ? b : a)
        if (cross(u, v, q) >= -1e-7)
        {
          outside = false;
          break;
        }
      if (outside)
        return false;
    }
  return true;
}
bool generateCityLayout(
    const CityPolygon &inputDomain,
    const std::vector<Poly> &inputReserved,
    const std::vector<CityStreet> &arteries,
    const std::vector<CityGate> &gates,
    const CitySettings &inputSettings,
    const std::function<double(double, double)> &terrain,
    CityLayout &out,
    std::string *error)
{
  CitySettings s = inputSettings;
  CityPolygon domain = inputDomain;
  std::vector<Poly> reserved = inputReserved;
  auto fail = [&](const char *m)
  {
    if (error)
      *error = m;
    return false;
  };
  if (s.districts.size() > 32)
    return fail("city district budget exceeded");
  std::set<uint64_t> districtIDs;
  std::vector<Poly> districtMasks;
  for (const auto &district : s.districts)
  {
    if (!district.id || !districtIDs.insert(district.id).second || unsigned(district.use) >= unsigned(DistrictUse::Count) || district.pieces.empty() || district.pieces.size() > 128)
      return fail("invalid accepted city district");
    for (const auto &p : district.pieces)
    {
      if (!convex(p))
        return fail("district pieces must be convex CCW");
      for (const auto &other : districtMasks)
        if (area(intersection(p, other)) > 1e-6)
          return fail("overlapping accepted districts");
      districtMasks.push_back(p);
    }
    if (district.use == DistrictUse::Residential || district.use == DistrictUse::Suburb || district.use == DistrictUse::Merchant || district.use == DistrictUse::Craftsmen ||
        district.use == DistrictUse::WealthyResidential || district.use == DistrictUse::WorkerHousing)
    {
      double plotArea = district.use == DistrictUse::Suburb ? 45 : district.use == DistrictUse::Merchant ? 95 : district.use == DistrictUse::Craftsmen ? 120 : 65;
      if (district.use == DistrictUse::WealthyResidential)
        plotArea = 650;
      if (district.use == DistrictUse::WorkerHousing)
        plotArea = 140;
      s.housingAreas.push_back({district.id, district.use, district.pieces, plotArea, district.use == DistrictUse::WealthyResidential, district.use == DistrictUse::WealthyResidential ? .55 : 0});
    }
    else
    {
      // Reserve net compound space inside the coarse allocation, leaving the
      // published bordering street hierarchy available for public access.
      double reservedArea = 0;
      const double margin = std::max(4., s.streetHalfWidth * 1.75 + s.setback + 1);
      auto compoundPieces = district.pieces;
      merge(compoundPieces); // Reclaim internal allocation seams before setting back the compound boundary.
      for (const auto &piece : compoundPieces)
      {
        Poly inner = piece;
        for (size_t k = 0; k < piece.size(); ++k)
        {
          auto a = piece[k], b = piece[(k + 1) % piece.size()];
          double nx = b.z - a.z, nz = a.x - b.x;
          inner = clip(inner, nx, nz, nx * a.x + nz * a.z - margin * dist(a, b));
        }
        if (area(inner) >= 40)
        {
          reservedArea += area(inner);
          reserved.push_back(std::move(inner));
        }
      }
      if (reservedArea < 200)
        return fail("district cannot retain a useful compound after street clearance");
    }
  }
  for (double v : {s.siteSpacing, s.targetBlockArea,  s.minimumBlockArea,        s.streetHalfWidth,   s.minimumWidth,    s.maximumWidth,      s.minimumDepth,    s.maximumDepth,   s.frontageGap,
                   s.setback,     s.overhang,         s.maximumFoundationRelief, s.houseWeight,       s.workshopWeight,  s.gardenWeight,      s.streetCurvature, s.junctionRadius, s.targetPlotArea,
                   s.wallSetback, s.suburbRoadLength, s.suburbGrowthRadius,      s.suburbCellSpacing, s.wallCornerInset, s.wallSectionLength, s.wallTowerRadius, s.wallTowerHeight})
    if (!std::isfinite(v) || v < 0 || v > 1e5)
      return fail("invalid city settings");
  if (!std::isfinite(s.plannedAxisRadians) || !std::isfinite(s.plannedDistrictTurnRadians) || !std::isfinite(s.plannedBlockAspect) || std::abs(s.plannedAxisRadians) > 6.3 || s.plannedDistrictTurnRadians < 0 ||
      s.plannedDistrictTurnRadians > .6 || s.plannedBlockAspect < 1 || s.plannedBlockAspect > 3)
    return fail("invalid planned street axes");
  if (s.houseWeight + s.workshopWeight + s.gardenWeight <= 0 || int(s.recipe) > 2 || !terrain || s.siteSpacing < 5 || s.minimumBlockArea < 1 || s.targetBlockArea < s.minimumBlockArea || s.streetHalfWidth < .5 ||
      s.targetPlotArea < 20 || s.targetPlotArea > 2000 || s.minimumWidth < 2 || s.minimumDepth < 2 || s.maximumWidth < s.minimumWidth || s.maximumDepth < s.minimumDepth || s.maximumSites < 4 || s.maximumSites > 128 ||
      s.maximumObjects > 10000 || !s.maximumObjects || s.maximumSplitDepth > 12 || domain.outer.size() < 3 || domain.outer.size() > 128 || domain.holes.size() > 16 || reserved.size() > 32 || arteries.size() > 64 ||
      gates.empty())
    return fail("city input budget or dimensions invalid");
  for (auto &p : reserved)
    if (!convex(p))
      return fail("reservation must be convex CCW");
  if (s.suburbLand.size() > 32 || s.suburbRoadLength < 10 || s.suburbRoadLength > 200 || s.suburbGrowthRadius < 10 || s.suburbGrowthRadius > 160 || s.suburbCellSpacing < 15 || s.suburbCellSpacing > 100 ||
      !s.maximumSuburbWards || s.maximumSuburbWards > 64 || s.wallCornerInset > 60 || s.wallSectionLength < 12 || s.wallSectionLength > 100 || s.wallSetback > 50 || s.wallTowerRadius < 1 || s.wallTowerRadius > 5 ||
      s.wallTowerHeight < 4 || s.wallTowerHeight > 20)
    return fail("invalid suburb or wall setback dimensions");
  if (!s.suburbLand.empty() && (!s.walls || !s.blockHousing))
    return fail("gate suburbs require walls and block housing");
  for (const auto &p : s.suburbLand)
    if (!convex(p))
      return fail("suburb land must be convex CCW");
  if (s.housingAreas.size() > 32)
    return fail("housing area budget exceeded");
  std::set<uint64_t> housingIDs;
  std::vector<Poly> housingMasks;
  for (const auto &housing : s.housingAreas)
  {
    if (!housing.id || !housingIDs.insert(housing.id).second || !std::isfinite(housing.targetPlotArea) || housing.targetPlotArea < 20 || housing.targetPlotArea > 2000 || housing.pieces.empty() ||
        housing.pieces.size() > 128 || !std::isfinite(housing.gardenFraction) || housing.gardenFraction < 0 || housing.gardenFraction > .85 || (housing.gardenFraction > 0 && !s.regularBuildingShells) ||
        (housing.use != DistrictUse::Residential && housing.use != DistrictUse::Suburb && housing.use != DistrictUse::Merchant && housing.use != DistrictUse::Manor && housing.use != DistrictUse::Craftsmen &&
         housing.use != DistrictUse::WealthyResidential && housing.use != DistrictUse::WorkerHousing))
      return fail("invalid ordinary housing area");
    for (const auto &p : housing.pieces)
    {
      if (!convex(p))
        return fail("housing mask must be convex CCW");
      for (const auto &q : housingMasks)
        if (area(intersection(p, q)) > 1e-6)
          return fail("overlapping housing masks");
      housingMasks.push_back(p);
    }
  }
  std::set<uint64_t> arterialIds;
  for (const auto &a : arteries)
  {
    if (!a.id || !arterialIds.insert(a.id).second || !std::isfinite(a.a.x) || !std::isfinite(a.a.z) || !std::isfinite(a.b.x) || !std::isfinite(a.b.z) || !std::isfinite(a.halfWidth) || a.halfWidth < .5 ||
        a.halfWidth > 20 || dist(a.a, a.b) < 1)
      return fail("invalid fixed arterial identity or geometry");
  }
  std::set<uint64_t> gateIds;
  for (const auto &g : gates)
    if (!g.id || !gateIds.insert(g.id).second || !std::isfinite(g.position.x) || !std::isfinite(g.position.z))
      return fail("invalid gate identity or position");
  if (s.walls && s.wallCornerInset > 0)
  {
    Poly faceted;
    for (size_t i = 0; i < domain.outer.size(); ++i)
    {
      auto prev = domain.outer[(i + domain.outer.size() - 1) % domain.outer.size()], corner = domain.outer[i], next = domain.outer[(i + 1) % domain.outer.size()];
      double cut = std::min({s.wallCornerInset, dist(prev, corner) * .22, dist(corner, next) * .22});
      for (const auto &gate : gates)
      {
        if (segmentDistance(gate.position, prev, corner) < 1e-5 || segmentDistance(gate.position, corner, next) < 1e-5)
          cut = std::min(cut, std::max(0., dist(gate.position, corner) - 12));
      }
      if (cut < .1 || cross(prev, corner, next) <= 1e-6)
      {
        faceted.push_back(corner);
        continue;
      }
      auto a = mix(corner, prev, cut / dist(prev, corner)), b = mix(corner, next, cut / dist(corner, next));
      // Two deliberately straight facets replace the formerly sampled curve.
      faceted.push_back(a);
      faceted.push_back(mix(mix(a, corner, .5), mix(corner, b, .5), .5));
      faceted.push_back(b);
    }
    if (faceted.size() > 128)
      return fail("faceted wall envelope vertex budget exceeded");
    domain.outer = std::move(faceted);
  }
  CityLayout r;
  r.envelope = domain;
  r.revision = s.revision;
  r.wallSetback = s.walls ? s.wallSetback : 0;
  r.districts = s.districts;
  r.reservations = reserved;
  // CDT is used only to decompose the input envelope, not to infer land labels.
  BoundaryRegistry registry;
  std::vector<BoundaryInput> edges;
  PatchDomain patch;
  patch.id = 1;
  uint64_t eid = 1, vid = 1;
  auto ring = [&](const Poly &p, std::vector<BoundaryUse> &uses)
  {
    uint64_t first = vid;
    for (size_t i = 0; i < p.size(); ++i)
    {
      edges.push_back({eid, {vid++, p[i]}, {i + 1 == p.size() ? first : vid, p[(i + 1) % p.size()]}, {}});
      uses.push_back({eid++, i + 1 == p.size()});
    }
  };
  ring(domain.outer, patch.outer);
  for (auto &p : domain.holes)
  {
    patch.holes.emplace_back();
    ring(p, patch.holes.back());
  }
  PatchMesh base;
  if (!registry.commit(edges, error) || !compilePatchMesh(
                                            patch,
                                            registry,
                                            {},
                                            [](double, double)
                                            {
                                              return 0.;
                                            },
                                            base,
                                            error))
    return false;
  std::vector<Poly> usable;
  for (auto &t : base.triangles)
  {
    Poly p;
    for (auto i : t)
      p.push_back(base.vertices[i].position);
    if (area(p) < 0)
      std::reverse(p.begin(), p.end());
    usable.push_back(p);
  }
  merge(usable);
  for (auto &q : reserved)
  {
    std::vector<Poly> next;
    for (auto &p : usable)
    {
      auto pieces = subtract(p, q);
      next.insert(next.end(), pieces.begin(), pieces.end());
    }
    usable = std::move(next);
  }
  merge(usable);
  if (usable.empty())
    return fail("no usable city land");
  double minX = 1e30, maxX = -1e30, minZ = 1e30, maxZ = -1e30;
  for (auto p : domain.outer)
  {
    minX = std::min(minX, p.x);
    maxX = std::max(maxX, p.x);
    minZ = std::min(minZ, p.z);
    maxZ = std::max(maxZ, p.z);
  }
  if (maxX - minX > 2000 || maxZ - minZ > 2000)
    return fail("city extent budget exceeded");
  auto random = [&](uint64_t id, uint64_t purpose)
  {
    return double(featureSeed(s.seed, 11, id, purpose) >> 11) * 0x1.0p-53;
  };
  auto valid = [&](Position p)
  {
    for (auto &q : usable)
      if (inside(q, p))
        return true;
    return false;
  };
  struct PlannedCut
  {
    double nx, nz, offset;
    uint32_t depth;
  };
  std::vector<PlannedCut> plannedCuts;
  const bool planned = s.recipe == CityRecipe::PlannedBlocks;
  std::vector<Poly> cells;
  if (s.recipe == CityRecipe::OrganicVoronoi)
  {
    for (uint64_t attempt = 0; attempt < 8192 && r.sites.size() < s.maximumSites; ++attempt)
    {
      Position p{minX + (maxX - minX) * random(attempt, 1), 0, minZ + (maxZ - minZ) * random(attempt, 2)};
      if (!valid(p))
        continue;
      bool ok = true;
      for (auto q : r.sites)
        if (dist(p, q) < s.siteSpacing)
        {
          ok = false;
          break;
        }
      if (ok)
        r.sites.push_back(p);
    }
    if (r.sites.size() < 4)
      return fail("insufficient Poisson sites");
    std::vector<procedural::Point2> points;
    for (auto p : r.sites)
      points.emplace_back(p.x, p.z);
    procedural::ConstrainedDelaunayTriangulator dt;
    if (!dt.triangulate(points, {}))
      return fail("site Delaunay failed");
    std::vector<std::set<size_t>> neighbors(r.sites.size());
    for (uint64_t t = 0; t < dt.triangleCount(); ++t)
      if (dt.triangleExists(t))
        for (int k = 0; k < 3; ++k)
        {
          auto a = dt.vertexOfTriangle(t, k), b = dt.vertexOfTriangle(t, (k + 1) % 3);
          if (a >= 0 && b >= 0 && size_t(a) < points.size() && size_t(b) < points.size())
          {
            neighbors[a].insert(b);
            neighbors[b].insert(a);
          }
        }
    for (size_t i = 0; i < r.sites.size(); ++i)
    {
      Poly p{{minX, 0, minZ}, {maxX, 0, minZ}, {maxX, 0, maxZ}, {minX, 0, maxZ}};
      auto a = r.sites[i];
      for (auto j : neighbors[i])
      {
        auto b = r.sites[j];
        p = clip(p, b.x - a.x, b.z - a.z, (b.x * b.x + b.z * b.z - a.x * a.x - a.z * a.z) * .5);
      }
      cells.push_back(p);
    }
  }
  // Refine coarse Voronoi patches too. The site budget controls neighborhood
  // structure; targetBlockArea independently controls walkable block scale.
  {
    auto coarse = std::move(cells);
    cells.clear();
    std::function<void(Poly, uint32_t, uint64_t, double)> split = [&](Poly p, uint32_t depth, uint64_t id, double districtAxis)
    {
      if (cells.size() > 256)
        return; // Caller reports exhaustion atomically; never publishes truncation.
      if (area(p) <= s.targetBlockArea || depth == s.maximumSplitDepth)
      {
        if (area(p) > s.targetBlockArea)
          ++r.unrefinedBlocks;
        cells.push_back(p);
        return;
      }
      ++r.splitAttempts;
      double loX = 1e30, hiX = -1e30, loZ = 1e30, hiZ = -1e30;
      for (auto v : p)
      {
        loX = std::min(loX, v.x);
        hiX = std::max(hiX, v.x);
        loZ = std::min(loZ, v.z);
        hiZ = std::max(hiZ, v.z);
      }
      double theta = (hiX - loX > hiZ - loZ ? 0 : 1.57079632679) + (random(id, 3) - .5) * .45, nx = std::cos(theta), nz = std::sin(theta), lo = 1e30, hi = -1e30;
      if (planned)
      {
        if (depth == 2)
          districtAxis += (random(id, 91) - .5) * 2 * s.plannedDistrictTurnRadians;
        const double ux = std::cos(districtAxis), uz = std::sin(districtAxis);
        double u0 = 1e30, u1 = -1e30, v0 = 1e30, v1 = -1e30;
        for (auto q : p)
        {
          double u = q.x * ux + q.z * uz, v = -q.x * uz + q.z * ux;
          u0 = std::min(u0, u);
          u1 = std::max(u1, u);
          v0 = std::min(v0, v);
          v1 = std::max(v1, v);
        }
        theta = districtAxis + ((u1 - u0) / (v1 - v0) > s.plannedBlockAspect ? 0 : 1.5707963267948966);
        nx = std::cos(theta);
        nz = std::sin(theta);
      }
      for (auto v : p)
      {
        lo = std::min(lo, nx * v.x + nz * v.z);
        hi = std::max(hi, nx * v.x + nz * v.z);
      }
      double c = lo + (hi - lo) * (planned ? .46 + random(id, 4) * .08 : .4 + random(id, 4) * .2);
      auto a = clip(p, nx, nz, c), b = clip(p, -nx, -nz, -c);
      auto hasWorkingDepth = [&](Poly q)
      {
        // A useful block supports opposing frontage rows and a small interior
        // court. Area alone accepts thin triangles that become mostly street.
        const double inset = s.streetHalfWidth + s.setback + s.overhang + s.minimumDepth;
        const auto boundary = q;
        for (size_t k = 0; k < boundary.size(); ++k)
        {
          const auto u = boundary[k], v = boundary[(k + 1) % boundary.size()];
          const double nx = v.z - u.z, nz = u.x - v.x;
          q = clip(q, nx, nz, nx * u.x + nz * u.z - inset * dist(u, v));
        }
        return area(q) >= s.minimumWidth * s.minimumWidth;
      };
      if (area(a) < s.minimumBlockArea || area(b) < s.minimumBlockArea || !hasWorkingDepth(a) || !hasWorkingDepth(b))
      {
        ++r.unrefinedBlocks;
        cells.push_back(p);
        return;
      }
      if (planned)
        plannedCuts.push_back({nx, nz, c, depth});
      split(a, depth + 1, id * 2, districtAxis);
      split(b, depth + 1, id * 2 + 1, districtAxis);
    };
    // Split before domain clipping, so holes and concave boundaries survive.
    if (s.recipe == CityRecipe::RecursiveBlocks)
      coarse.push_back({{minX, 0, minZ}, {maxX, 0, minZ}, {maxX, 0, maxZ}, {minX, 0, maxZ}});
    if (planned)
    {
      double ux = std::cos(s.plannedAxisRadians), uz = std::sin(s.plannedAxisRadians), u0 = 1e30, u1 = -1e30, v0 = 1e30, v1 = -1e30;
      for (auto q : domain.outer)
      {
        double u = q.x * ux + q.z * uz, v = -q.x * uz + q.z * ux;
        u0 = std::min(u0, u);
        u1 = std::max(u1, u);
        v0 = std::min(v0, v);
        v1 = std::max(v1, v);
      }
      auto at = [&](double u, double v)
      {
        return Position{ux * u - uz * v, 0, uz * u + ux * v};
      };
      coarse.push_back({at(u0, v0), at(u1, v0), at(u1, v1), at(u0, v1)});
    }
    for (size_t i = 0; i < coarse.size(); ++i)
      split(coarse[i], 0, s.recipe == CityRecipe::OrganicVoronoi ? featureSeed(s.seed, 11, i + 1, 30) : 1, s.plannedAxisRadians);
  }
  if (cells.size() > 256)
    return fail("district subdivision budget exceeded");
  uint64_t wid = 1;
  for (auto &cell : cells)
  {
    CityWard w;
    w.id = wid++;
    for (auto &p : usable)
    {
      auto q = intersection(p, cell);
      if (!q.empty())
        w.pieces.push_back(q);
    }
    merge(w.pieces);
    if (!w.pieces.empty())
      r.wards.push_back(w);
  }
  r.subdivisionCells = r.wards.size();
  // Candidate streets are shared boundaries of different wards, never decomposition diagonals.
  std::map<uint64_t, std::pair<uint64_t, uint64_t>> streetWards;
  uint64_t sid = 1000;
  auto nextStreetId = [&]()
  {
    while (arterialIds.count(sid))
      ++sid;
    return sid++;
  };
  for (size_t i = 0; i < r.wards.size(); ++i)
    for (size_t j = i + 1; j < r.wards.size(); ++j)
      for (auto &p : r.wards[i].pieces)
        for (auto &q : r.wards[j].pieces)
          for (size_t a = 0; a < p.size(); ++a)
            for (size_t b = 0; b < q.size(); ++b)
            {
              auto u = p[a], v = p[(a + 1) % p.size()], c = q[b], d = q[(b + 1) % q.size()];
              double l = dist(u, v);
              if (l < 1e-6 || std::abs(cross(u, v, c)) / l > 1e-5 || std::abs(cross(u, v, d)) / l > 1e-5)
                continue;
              double t0 = ((c.x - u.x) * (v.x - u.x) + (c.z - u.z) * (v.z - u.z)) / (l * l), t1 = ((d.x - u.x) * (v.x - u.x) + (d.z - u.z) * (v.z - u.z)) / (l * l);
              double lo = std::max(0., std::min(t0, t1)), hi = std::min(1., std::max(t0, t1));
              if ((hi - lo) * l > 1)
              {
                const auto id = nextStreetId();
                r.streets.push_back({id, mix(u, v, lo), mix(u, v, hi), s.streetHalfWidth * (.7 + .3 * random(id, 31)), false});
                if (planned)
                {
                  auto &street = r.streets.back();
                  street.halfWidth = s.streetHalfWidth;
                  const auto mid = mix(street.a, street.b, .5);
                  for (const auto &cut : plannedCuts)
                    if (std::abs(cut.nx * mid.x + cut.nz * mid.z - cut.offset) < 1e-5)
                    {
                      street.hierarchy = cut.depth == 0 ? 2 : cut.depth < 3 ? 1 : 0;
                      street.halfWidth = s.streetHalfWidth * (street.hierarchy == 2 ? 1.75 : street.hierarchy == 1 ? 1.3 : cut.depth >= 4 ? .6 : 1.);
                      break;
                    }
                }
                streetWards[id] = {r.wards[i].id, r.wards[j].id};
              }
            }
  // Re-cut wards at immutable arterial centerlines, so those streets also own frontage.
  for (auto &w : r.wards)
    for (const auto &street : arteries)
    {
      std::vector<Poly> pieces;
      double nx = street.b.z - street.a.z, nz = street.a.x - street.b.x, c = nx * street.a.x + nz * street.a.z;
      for (auto &p : w.pieces)
      {
        auto left = clip(p, nx, nz, c), right = clip(p, -nx, -nz, -c);
        if (!left.empty())
          pieces.push_back(left);
        if (!right.empty())
          pieces.push_back(right);
      }
      w.pieces = std::move(pieces);
    }
  std::set<uint64_t> ids;
  for (auto &a : arteries)
  {
    if (!a.id || !ids.insert(a.id).second || dist(a.a, a.b) < 1 || !std::isfinite(a.halfWidth) || a.halfWidth < .5 || a.halfWidth > 20)
      return fail("invalid fixed artery");
    std::vector<std::pair<double, double>> intervals;
    for (const auto &p : usable)
    {
      double lo = 0, hi = 1;
      for (size_t k = 0; k < p.size(); ++k)
      {
        auto u = p[k], v = p[(k + 1) % p.size()];
        double da = cross(u, v, a.a), db = cross(u, v, a.b);
        if (da < -1e-8 && db < -1e-8)
        {
          hi = -1;
          break;
        }
        if (da < -1e-8)
          lo = std::max(lo, da / (da - db));
        if (db < -1e-8)
          hi = std::min(hi, da / (da - db));
      }
      if (hi >= lo)
        intervals.push_back({lo, hi});
    }
    std::sort(intervals.begin(), intervals.end());
    double covered = 0;
    for (auto [lo, hi] : intervals)
    {
      if (lo > covered + 1e-8)
        break;
      covered = std::max(covered, hi);
    }
    if (covered < 1 - 1e-8)
      return fail("fixed artery leaves usable city land");
    for (const auto &q : reserved)
      if (area(intersection(ribbon(a), q)) > 1e-7)
        return fail("fixed artery width crosses a reservation");
    r.streets.push_back({a.id, a.a, a.b, a.halfWidth, true});
  }
  if (arteries.empty())
    return fail("city needs fixed external circulation");
  // Keep only the circulation component attached to the first artery. Reject stranded gates.
  std::vector<bool> connected(r.streets.size());
  connected[r.streets.size() - arteries.size()] = true;
  bool changed = true;
  while (changed)
  {
    changed = false;
    for (size_t i = 0; i < r.streets.size(); ++i)
      if (connected[i])
        for (size_t j = 0; j < r.streets.size(); ++j)
          if (!connected[j] && segmentsMeet(r.streets[i].a, r.streets[i].b, r.streets[j].a, r.streets[j].b))
          {
            connected[j] = true;
            changed = true;
          }
  }
  std::vector<CityStreet> network;
  for (size_t i = 0; i < r.streets.size(); ++i)
    if (connected[i])
      network.push_back(r.streets[i]);
    else if (r.streets[i].arterial)
      return fail("disconnected fixed artery");
  r.streets = network;
  // Remove a bounded selection of redundant local links while preserving all
  // remaining streets' connection to fixed circulation. Not every ward edge is a road.
  for (size_t omitted = 0; omitted < r.streets.size();)
  {
    bool reclaim = false;
    if (!r.streets[omitted].arterial && r.streets[omitted].hierarchy == 0)
      for (const auto &housing : s.housingAreas)
        if (housing.reclaimLocalStreets)
        {
          auto band = ribbon(r.streets[omitted]);
          double covered = 0;
          for (const auto &piece : housing.pieces)
            covered += area(intersection(band, piece));
          reclaim |= std::abs(covered - area(band)) < 1e-6;
        }
    if (r.streets[omitted].arterial || (!reclaim && (planned || random(r.streets[omitted].id, 12) > .16)))
    {
      ++omitted;
      continue;
    }
    std::vector<bool> seen(r.streets.size());
    std::vector<size_t> queue;
    for (size_t i = 0; i < r.streets.size(); ++i)
      if (r.streets[i].arterial)
      {
        seen[i] = true;
        queue.push_back(i);
        break;
      }
    for (size_t k = 0; k < queue.size(); ++k)
    {
      auto &a = r.streets[queue[k]];
      for (size_t j = 0; j < r.streets.size(); ++j)
        if (j != omitted && !seen[j] && segmentsMeet(a.a, a.b, r.streets[j].a, r.streets[j].b))
        {
          seen[j] = true;
          queue.push_back(j);
        }
    }
    if (queue.size() + 1 == r.streets.size())
    {
      if (reclaim)
        r.reclaimedStreets.push_back(r.streets[omitted].id);
      r.streets.erase(r.streets.begin() + omitted);
    }
    else
      ++omitted;
  }
  // Dissolve ownership seams where a local street was discarded. Retained
  // pavement still bounds every plot through the later clearance checks.
  std::map<uint64_t, uint64_t> parentWard;
  for (const auto &w : r.wards)
    parentWard[w.id] = w.id;
  auto rootWard = [&](uint64_t id)
  {
    while (parentWard.at(id) != id)
      id = parentWard.at(id);
    return id;
  };
  std::set<uint64_t> retained;
  for (const auto &street : r.streets)
    retained.insert(street.id);
  for (const auto &[id, pair] : streetWards)
    if (!retained.count(id))
    {
      const auto a = rootWard(pair.first), b = rootWard(pair.second);
      parentWard[std::max(a, b)] = std::min(a, b);
    }
  std::map<uint64_t, CityWard> mergedWards;
  for (const auto &w : r.wards)
  {
    auto &joined = mergedWards[rootWard(w.id)];
    joined.id = rootWard(w.id);
    joined.pieces.insert(joined.pieces.end(), w.pieces.begin(), w.pieces.end());
  }
  r.wards.clear();
  for (auto &[id, w] : mergedWards)
  {
    merge(w.pieces);
    r.wards.push_back(std::move(w));
  }
  for (auto &g : gates)
  {
    bool ok = false;
    if (!g.id || !g.externalRoad)
      return fail("gate has no external road");
    for (auto &a : r.streets)
      if (a.arterial && a.id == g.externalRoad && segmentDistance(g.position, a.a, a.b) < 1e-5)
        ok = true;
    bool boundary = false;
    for (size_t i = 0; i < domain.outer.size(); ++i)
      if (segmentDistance(g.position, domain.outer[i], domain.outer[(i + 1) % domain.outer.size()]) < 1e-5)
        boundary = true;
    if (!ok || !boundary)
      return fail("gate disconnected from named arterial or envelope boundary");
  }
  // Lock every graph intersection before adding one modest bend per span.
  for (size_t i = 0; i < r.streets.size(); ++i)
  {
    auto &street = r.streets[i];
    auto a = street.a, b = street.b;
    double dx = b.x - a.x, dz = b.z - a.z, length = dist(a, b);
    std::vector<double> cuts{0, 1};
    for (size_t j = 0; j < r.streets.size(); ++j)
      if (i != j)
      {
        auto c = r.streets[j].a, d = r.streets[j].b;
        double ex = d.x - c.x, ez = d.z - c.z, den = dx * ez - dz * ex;
        if (std::abs(den) < 1e-9)
          continue;
        double t = ((c.x - a.x) * ez - (c.z - a.z) * ex) / den, u = ((c.x - a.x) * dz - (c.z - a.z) * dx) / den;
        if (t > 1e-6 && t < 1 - 1e-6 && u >= -1e-6 && u <= 1 + 1e-6)
          cuts.push_back(t);
      }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(
        std::unique(
            cuts.begin(),
            cuts.end(),
            [](double a, double b)
            {
              return std::abs(a - b) < 1e-6;
            }),
        cuts.end());
    street.path.push_back(a);
    for (size_t k = 1; k < cuts.size(); ++k)
    {
      auto from = mix(a, b, cuts[k - 1]), to = mix(a, b, cuts[k]);
      double span = dist(from, to), amplitude = (street.arterial || planned) ? 0 : std::min(s.streetCurvature, span * .10) * (random(street.id * 17 + k, 20) < .5 ? -1 : 1);
      const double bend = .35 + .3 * random(street.id * 17 + k, 32);
      const int count = street.arterial ? 1 : 2; // One bend, without collinear sample fragments.
      std::vector<Position> curve;
      for (int retry = 0; retry < 5; ++retry)
      {
        curve.clear();
        bool fits = true;
        for (int n = 1; n <= count; ++n)
        {
          const double t = n == count ? 1. : bend;
          auto p = mix(from, to, t);
          double offset = amplitude * (t < bend ? t / bend : (1 - t) / (1 - bend));
          p.x += -dz / length * offset;
          p.z += dx / length * offset;
          if (!valid(p) || !coveredSegment(curve.empty() ? from : curve.back(), p, usable))
            fits = false;
          curve.push_back(p);
        }
        if (fits)
          break;
        if (retry == 4)
          return fail("curved street crosses excluded land");
        amplitude = retry == 3 ? 0 : amplitude * .5;
      }
      street.path.insert(street.path.end(), curve.begin(), curve.end());
    }
  }
  if (s.walls)
    for (size_t i = 0; i < domain.outer.size(); ++i)
    {
      auto a = domain.outer[i], b = domain.outer[(i + 1) % domain.outer.size()];
      double l = dist(a, b);
      std::vector<std::pair<double, double>> openings;
      for (auto &g : gates)
        if (segmentDistance(g.position, a, b) < 1e-5)
        {
          double t = ((g.position.x - a.x) * (b.x - a.x) + (g.position.z - a.z) * (b.z - a.z)) / (l * l), width = 0;
          for (auto &road : arteries)
            if (road.id == g.externalRoad)
            {
              const double crossing = std::abs((b.x - a.x) * (road.b.z - road.a.z) - (b.z - a.z) * (road.b.x - road.a.x)) / (l * dist(road.a, road.b));
              if (crossing < 1e-5)
                return fail("wall gate route is tangent to perimeter");
              width = (road.halfWidth + 1.5 + s.wallTowerRadius) / crossing;
            }
          openings.push_back({std::max(0., t - width / l), std::min(1., t + width / l)});
        }
      std::sort(openings.begin(), openings.end());
      double start = 0;
      for (auto [lo, hi] : openings)
      {
        if (lo - start > 1e-6)
          r.walls.push_back({nextStreetId(), mix(a, b, start), mix(a, b, lo), .7, false});
        start = std::max(start, hi);
      }
      if (1 - start > 1e-6)
        r.walls.push_back({nextStreetId(), mix(a, b, start), b, .7, false});
    }
  if (s.walls)
  {
    std::vector<CityStreet> refined;
    for (const auto &wall : r.walls)
    {
      unsigned count = std::max(1u, unsigned(std::ceil(dist(wall.a, wall.b) / s.wallSectionLength)));
      for (unsigned k = 0; k < count; ++k)
        refined.push_back({nextStreetId(), mix(wall.a, wall.b, double(k) / count), mix(wall.a, wall.b, double(k + 1) / count), wall.halfWidth, false});
    }
    r.walls = std::move(refined);
  }
  for (const auto &wall : r.walls)
    for (auto center : {wall.a, wall.b})
    {
      if (std::any_of(
              r.towers.begin(),
              r.towers.end(),
              [&](const auto &tower)
              {
                return dist(tower.center, center) < 1e-5;
              }))
        continue;
      CityWallTower tower;
      tower.id = (uint64_t(1) << 63) | nextStreetId();
      tower.center = center;
      tower.height = s.wallTowerHeight;
      for (unsigned k = 0; k < 8; ++k)
      {
        double angle = k * 3.14159265358979323846 / 4;
        tower.footprint.push_back({center.x + s.wallTowerRadius * std::cos(angle), 0, center.z + s.wallTowerRadius * std::sin(angle)});
      }
      r.towers.push_back(std::move(tower));
    }
  // Very short wall piers share one elongated tower instead of overlapping
  // separate structural instances. Their endpoint connections remain covered.
  for (size_t i = 0; i < r.towers.size(); ++i)
    for (size_t j = i + 1; j < r.towers.size();)
    {
      if (cityPolygonsOverlap(r.towers[i].footprint, r.towers[j].footprint))
      {
        auto points = r.towers[i].footprint;
        points.insert(points.end(), r.towers[j].footprint.begin(), r.towers[j].footprint.end());
        r.towers[i].footprint = hull(points);
        r.towers[i].center = {};
        for (auto p : r.towers[i].footprint)
        {
          r.towers[i].center.x += p.x / r.towers[i].footprint.size();
          r.towers[i].center.z += p.z / r.towers[i].footprint.size();
        }
        r.towers.erase(r.towers.begin() + j);
        j = i + 1;
      }
      else
        ++j;
    }
  if (r.towers.size() > 512)
    return fail("wall tower budget exceeded");
  auto towerClearance = [&](const CityWallTower &tower)
  {
    double x0 = INFINITY, z0 = INFINITY, x1 = -INFINITY, z1 = -INFINITY;
    for (auto p : tower.footprint)
    {
      x0 = std::min(x0, p.x);
      z0 = std::min(z0, p.z);
      x1 = std::max(x1, p.x);
      z1 = std::max(z1, p.z);
    }
    return Poly{{x0 - s.wallSetback, 0, z0 - s.wallSetback}, {x1 + s.wallSetback, 0, z0 - s.wallSetback}, {x1 + s.wallSetback, 0, z1 + s.wallSetback}, {x0 - s.wallSetback, 0, z1 + s.wallSetback}};
  };
  const size_t interiorPieces = usable.size();
  if (!s.suburbLand.empty())
  {
    // Fill the envelope holes for this exclusion: a lake/courtyard inside the
    // enclosure must never be reclassified as exterior developable land.
    auto enclosureDomain = patch;
    enclosureDomain.holes.clear();
    PatchMesh enclosureMesh;
    if (!compilePatchMesh(
            enclosureDomain,
            registry,
            {},
            [](double, double)
            {
              return 0.;
            },
            enclosureMesh,
            error))
      return false;
    std::vector<Poly> enclosure;
    for (const auto &t : enclosureMesh.triangles)
    {
      Poly p;
      for (auto index : t)
        p.push_back(enclosureMesh.vertices[index].position);
      if (area(p) < 0)
        std::reverse(p.begin(), p.end());
      enclosure.push_back(std::move(p));
    }
    merge(enclosure);
    auto remove = [&](std::vector<Poly> pieces, const std::vector<Poly> &masks)
    {
      for (const auto &mask : masks)
      {
        std::vector<Poly> next;
        for (const auto &p : pieces)
        {
          auto rest = intersection(p, mask).empty() ? std::vector<Poly>{p} : subtract(p, mask);
          next.insert(next.end(), rest.begin(), rest.end());
        }
        pieces = std::move(next);
      }
      return pieces;
    };
    // Stable gate order keeps IDs and frontage subdivision independent of input order.
    auto orderedGates = gates;
    std::sort(
        orderedGates.begin(),
        orderedGates.end(),
        [](const auto &a, const auto &b)
        {
          return a.id < b.id;
        });
    uint64_t wardID = 0;
    for (const auto &ward : r.wards)
      wardID = std::max(wardID, ward.id);
    std::vector<CityStreet> approaches;
    for (const auto &gate : orderedGates)
    {
      const auto &artery = *std::find_if(
          arteries.begin(),
          arteries.end(),
          [&](const auto &road)
          {
            return road.id == gate.externalRoad;
          });
      double length = dist(artery.a, artery.b);
      Position direction{(artery.b.x - artery.a.x) / length, 0, (artery.b.z - artery.a.z) / length};
      auto along = [&](double t)
      {
        return Position{gate.position.x + direction.x * t, 0, gate.position.z + direction.z * t};
      };
      auto enclosed = [&](Position p)
      {
        return std::any_of(
            enclosure.begin(),
            enclosure.end(),
            [&](const auto &q)
            {
              return inside(q, p);
            });
      };
      bool forward = enclosed(along(.1)), backward = enclosed(along(-.1));
      if (forward == backward)
        continue; // Tangential arrival cannot seed an exterior road.
      if (forward)
      {
        direction.x = -direction.x;
        direction.z = -direction.z;
      }
      // Clip the outward road to continuous supplied land. Do not jump a water,
      // terrain or reservation gap, even when buildable land exists beyond it.
      auto available = remove(s.suburbLand, reserved);
      double reach = 0;
      for (double candidate = 1; candidate <= s.suburbRoadLength; candidate += 1)
      {
        auto band = ribbon({0, gate.position, along(candidate), artery.halfWidth, false});
        auto exteriorRoad = remove({band}, enclosure);
        if (!remove(exteriorRoad, available).empty())
          break;
        reach = candidate;
      }
      if (reach < s.wallSetback + 18)
        continue;
      // Begin with a short gate connection. Outer wards first occupy the wall
      // frontage; their local lanes can later carry growth farther from the core.
      reach = std::min(reach, s.wallSetback + s.suburbCellSpacing * .9);
      auto end = along(reach);
      approaches.push_back({nextStreetId(), gate.position, end, artery.halfWidth, false, {gate.position, end}, 0});
    }
    if (!approaches.empty())
    {
      std::vector<Poly> available;
      for (const auto &mask : s.suburbLand)
      {
        auto unique = remove({mask}, available);
        available.insert(available.end(), unique.begin(), unique.end());
      }
      available = remove(std::move(available), reserved);
      available = remove(std::move(available), enclosure);
      const double wallBand = s.suburbCellSpacing * 1.8;
      double x0 = minX - wallBand, x1 = maxX + wallBand, z0 = minZ - wallBand, z1 = maxZ + wallBand;
      for (const auto &road : approaches)
        for (auto p : {road.a, road.b})
        {
          x0 = std::min(x0, p.x - s.suburbGrowthRadius);
          x1 = std::max(x1, p.x + s.suburbGrowthRadius);
          z0 = std::min(z0, p.z - s.suburbGrowthRadius);
          z1 = std::max(z1, p.z + s.suburbGrowthRadius);
        }
      auto wallDistance = [&](Position p)
      {
        double nearest = 1e30;
        for (size_t k = 0; k < domain.outer.size(); ++k)
          nearest = std::min(nearest, segmentDistance(p, domain.outer[k], domain.outer[(k + 1) % domain.outer.size()]));
        return nearest;
      };
      auto roadDistance = [&](Position p)
      {
        double nearest = 1e30;
        for (const auto &road : approaches)
          nearest = std::min(nearest, segmentDistance(p, road.a, road.b));
        return nearest;
      };
      // A jittered site lattice yields a bounded, deterministic tessellation
      // across the outer planning domain; settlement selection happens afterward.
      std::vector<Position> outerSites;
      uint64_t key = 0;
      for (double z = z0; z < z1; z += s.suburbCellSpacing)
        for (double x = x0; x < x1; x += s.suburbCellSpacing)
        {
          ++key;
          Position p{x + s.suburbCellSpacing * (.2 + .6 * random(key, 91)), 0, z + s.suburbCellSpacing * (.2 + .6 * random(key, 92))};
          if (roadDistance(p) > s.suburbGrowthRadius + s.suburbCellSpacing && wallDistance(p) > s.suburbCellSpacing * 1.8)
            continue;
          outerSites.push_back(p);
        }
      if (outerSites.size() > 512)
        return fail("outer Voronoi site budget exceeded");
      struct OuterCell
      {
        Poly boundary;
        Position site;
        std::vector<Poly> pieces;
        bool selected = false;
      };
      std::vector<OuterCell> outerCells;
      for (size_t i = 0; i < outerSites.size(); ++i)
      {
        Poly face{{x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}};
        auto a = outerSites[i];
        for (size_t j = 0; j < outerSites.size() && !face.empty(); ++j)
          if (i != j)
          {
            auto b = outerSites[j];
            face = clip(face, b.x - a.x, b.z - a.z, (b.x * b.x + b.z * b.z - a.x * a.x - a.z * a.z) * .5);
          }
        OuterCell cell;
        cell.boundary = face;
        cell.site = a;
        if (roadDistance(a) <= s.suburbGrowthRadius || wallDistance(a) <= s.suburbCellSpacing * 1.4)
          for (const auto &mask : available)
          {
            auto p = intersection(face, mask);
            if (area(p) > 1e-6)
              cell.pieces.push_back(std::move(p));
          }
        merge(cell.pieces);
        outerCells.push_back(std::move(cell));
      }
      // Return actual covered intervals of a line, never bridge an excluded gap.
      auto intervals = [&](Position a, Position b, const std::vector<Poly> &land)
      {
        std::vector<std::pair<double, double>> result;
        for (const auto &p : land)
        {
          double lo = 0, hi = 1;
          for (size_t k = 0; k < p.size(); ++k)
          {
            double da = cross(p[k], p[(k + 1) % p.size()], a), db = cross(p[k], p[(k + 1) % p.size()], b);
            if (da < -1e-7 && db < -1e-7)
            {
              hi = -1;
              break;
            }
            if (da < -1e-7)
              lo = std::max(lo, da / (da - db));
            if (db < -1e-7)
              hi = std::min(hi, da / (da - db));
          }
          if (hi - lo > 1e-6)
            result.emplace_back(lo, hi);
        }
        std::sort(result.begin(), result.end());
        std::vector<std::pair<double, double>> joined;
        for (auto span : result)
          if (!joined.empty() && span.first <= joined.back().second + 1e-7)
            joined.back().second = std::max(joined.back().second, span.second);
          else
            joined.push_back(span);
        return joined;
      };
      std::vector<OuterCell> connectedCells;
      for (const auto &cell : outerCells)
      {
        std::vector<bool> visited(cell.pieces.size());
        for (size_t start = 0; start < cell.pieces.size(); ++start)
          if (!visited[start])
          {
            OuterCell component;
            component.boundary = cell.boundary;
            component.site = cell.site;
            std::vector<size_t> queue{start};
            visited[start] = true;
            for (size_t q = 0; q < queue.size(); ++q)
            {
              const auto &piece = cell.pieces[queue[q]];
              component.pieces.push_back(piece);
              for (size_t j = 0; j < cell.pieces.size(); ++j)
                if (!visited[j])
                {
                  bool joins = false;
                  for (size_t k = 0; k < piece.size(); ++k)
                    for (auto [lo, hi] : intervals(piece[k], piece[(k + 1) % piece.size()], {cell.pieces[j]}))
                      joins |= (hi - lo) * dist(piece[k], piece[(k + 1) % piece.size()]) >= 4;
                  if (joins)
                  {
                    visited[j] = true;
                    queue.push_back(j);
                  }
                }
            }
            connectedCells.push_back(std::move(component));
          }
      }
      outerCells = std::move(connectedCells);
      struct OuterLink
      {
        size_t a, b;
        Position from, to;
      };
      std::vector<OuterLink> links;
      for (size_t i = 0; i < outerCells.size(); ++i)
        if (!outerCells[i].pieces.empty())
          for (size_t j = i + 1; j < outerCells.size(); ++j)
            if (!outerCells[j].pieces.empty())
            {
              const auto a = outerCells[i].site, b = outerCells[j].site;
              if (dist(a, b) < 1e-6)
                continue;
              const double nx = b.x - a.x, nz = b.z - a.z, c = (b.x * b.x + b.z * b.z - a.x * a.x - a.z * a.z) * .5;
              Poly edge;
              for (auto p : outerCells[i].boundary)
                if (std::abs(nx * p.x + nz * p.z - c) < 1e-5)
                  edge.push_back(p);
              if (edge.size() != 2 || dist(edge[0], edge[1]) < 4)
                continue;
              for (auto first : intervals(edge[0], edge[1], outerCells[i].pieces))
                for (auto second : intervals(edge[0], edge[1], outerCells[j].pieces))
                {
                  double lo = std::max(first.first, second.first), hi = std::min(first.second, second.second);
                  if ((hi - lo) * dist(edge[0], edge[1]) >= 4)
                    links.push_back({i, j, mix(edge[0], edge[1], lo), mix(edge[0], edge[1], hi)});
                }
            }
      size_t selected = 0;
      // Seed all wards traversed by a validated gate approach, then grow only
      // through shared edges. This produces neighbourhoods, not parallel strips.
      for (auto &cell : outerCells)
        for (const auto &road : approaches)
          if (!cell.selected && !intervals(road.a, road.b, cell.pieces).empty())
          {
            cell.selected = true;
            ++selected;
          }
      if (selected > s.maximumSuburbWards)
        return fail("gate approach wards exceed suburb budget");
      while (selected < s.maximumSuburbWards)
      {
        size_t best = outerCells.size();
        double bestScore = 1e30;
        for (size_t i = 0; i < outerCells.size(); ++i)
          if (!outerCells[i].selected && !outerCells[i].pieces.empty())
          {
            unsigned neighbours = 0;
            for (const auto &link : links)
              if ((link.a == i && outerCells[link.b].selected) || (link.b == i && outerCells[link.a].selected))
                ++neighbours;
            if (!neighbours)
              continue;
            double score =
                std::max(0., wallDistance(outerCells[i].site) - s.suburbCellSpacing * .6) * 4 + roadDistance(outerCells[i].site) * .25 - neighbours * s.suburbCellSpacing * .3 + random(i, 93) * s.suburbCellSpacing * .15;
            if (score < bestScore)
            {
              best = i;
              bestScore = score;
            }
          }
        if (best == outerCells.size())
          break;
        outerCells[best].selected = true;
        ++selected;
      }
      for (auto &cell : outerCells)
        if (cell.selected)
        {
          r.wards.push_back({++wardID, cell.pieces});
          r.suburbs.insert(r.suburbs.end(), cell.pieces.begin(), cell.pieces.end());
        }
      if (r.suburbs.size() > 512)
        return fail("outer ward piece budget exceeded");
      // Shared Voronoi edges become local lanes; retain only the road component
      // connected to the gate approaches. Parcel cuts will add no extra roads.
      std::vector<CityStreet> lanes;
      for (const auto &link : links)
        if (outerCells[link.a].selected && outerCells[link.b].selected)
          lanes.push_back({nextStreetId(), link.from, link.to, 1.5, false, {link.from, link.to}, 0});
      // A lane outside the defensive clearance connects adjacent wall-side
      // neighbourhoods. Clip it to grown land; unbuilt sectors get no ring road.
      Poly outerLane;
      const double offset = s.wallSetback + 12;
      for (size_t k = 0; k < domain.outer.size(); ++k)
      {
        auto prev = domain.outer[(k + domain.outer.size() - 1) % domain.outer.size()], p = domain.outer[k], next = domain.outer[(k + 1) % domain.outer.size()];
        double a = dist(prev, p), b = dist(p, next);
        Position n0{(p.z - prev.z) / a, 0, -(p.x - prev.x) / a}, n1{(next.z - p.z) / b, 0, -(next.x - p.x) / b};
        double denominator = 1 + n0.x * n1.x + n0.z * n1.z;
        double miter = offset / std::max(.4, denominator);
        outerLane.push_back({p.x + (n0.x + n1.x) * miter, 0, p.z + (n0.z + n1.z) * miter});
      }
      for (size_t k = 0; k < outerLane.size(); ++k)
      {
        auto a = outerLane[k], b = outerLane[(k + 1) % outerLane.size()];
        for (auto [lo, hi] : intervals(a, b, r.suburbs))
          if ((hi - lo) * dist(a, b) >= 4)
          {
            auto from = mix(a, b, lo), to = mix(a, b, hi);
            lanes.push_back({nextStreetId(), from, to, 1.6, false, {from, to}, 0});
          }
      }
      std::vector<bool> connected(lanes.size());
      bool changed = true;
      while (changed)
      {
        changed = false;
        for (size_t i = 0; i < lanes.size(); ++i)
          if (!connected[i])
          {
            bool reaches = false;
            for (const auto &road : approaches)
              reaches |= segmentsMeet(lanes[i].a, lanes[i].b, road.a, road.b);
            for (size_t j = 0; j < lanes.size(); ++j)
              if (connected[j])
                reaches |= segmentsMeet(lanes[i].a, lanes[i].b, lanes[j].a, lanes[j].b);
            if (reaches)
            {
              connected[i] = true;
              changed = true;
            }
          }
      }
      r.streets.insert(r.streets.end(), approaches.begin(), approaches.end());
      for (size_t i = 0; i < lanes.size(); ++i)
        if (connected[i])
          r.streets.push_back(lanes[i]);
      usable.insert(usable.end(), r.suburbs.begin(), r.suburbs.end());
      for (const auto &p : r.suburbs)
        for (auto v : p)
        {
          minX = std::min(minX, v.x);
          maxX = std::max(maxX, v.x);
          minZ = std::min(minZ, v.z);
          maxZ = std::max(maxZ, v.z);
        }
    }
  }
  std::vector<Poly> roads;
  uint64_t cid = 1;
  // Ward cells are planning ownership, not visible ground seams. Publish the
  // usable ground once; actual streets define the surface blocks downstream.
  for (size_t ground = 0; ground < usable.size(); ++ground)
  {
    const auto &p = usable[ground];
    SurfaceContour c;
    c.id = cid++;
    c.material = ground < interiorPieces ? 11 : 16; // Distinct exterior ownership across the wall.
    c.loop = p;
    r.contours.push_back(c);
  }
  auto publishRoad = [&](const Poly &band, double height, bool join = false)
  {
    std::vector<Poly> covered;
    double coveredArea = 0;
    for (const auto &p : usable)
    {
      auto q = intersection(p, band);
      if (!q.empty())
      {
        coveredArea += area(q);
        covered.push_back(std::move(q));
      }
    }
    // Usable pieces have disjoint interiors. Exact area coverage proves that a
    // band can cross their planning seams without publishing invisible cuts.
    if (std::abs(coveredArea - area(band)) < 1e-7)
      covered = {band};
    for (auto &q : covered)
    {
      bool alreadyCovered = std::any_of(
          roads.begin(),
          roads.end(),
          [&](const Poly &existing)
          {
            return std::all_of(
                q.begin(),
                q.end(),
                [&](Position v)
                {
                  return inside(existing, v);
                });
          });
      if (join && !alreadyCovered)
      {
        // A shoulder wedge can straddle two existing ribbons while remaining
        // completely covered by their union. Do not emit those hidden seams.
        std::vector<Poly> uncovered{q};
        for (const auto &existing : roads)
        {
          std::vector<Poly> next;
          for (const auto &piece : uncovered)
          {
            if (!cityPolygonsOverlap(piece, existing))
              next.push_back(piece);
            else
            {
              auto rest = subtract(piece, existing);
              next.insert(next.end(), rest.begin(), rest.end());
            }
          }
          uncovered = std::move(next);
          if (uncovered.empty())
            break;
        }
        alreadyCovered = uncovered.empty();
      }
      SurfaceContour c;
      c.id = cid++;
      c.material = 1;
      c.role = SurfaceRole::Road;
      c.terrainSurface = false;
      c.plane.constant = height;
      if (!std::isfinite(height))
        return false;
      for (size_t k = 0; k < q.size(); ++k)
      {
        auto u = q[k], v = q[(k + 1) % q.size()];
        int steps = std::max(1, int(std::ceil(dist(u, v))));
        for (int j = 0; j <= steps; ++j)
        {
          auto sample = mix(u, v, double(j) / steps);
          double y = terrain(sample.x, sample.z);
          if (!std::isfinite(y) || std::abs(y - height) > 1e-5)
            return false;
        }
      }
      // Covered join wedges add no street surface. Publishing them creates
      // microscopic overlapping edges that can pinch the downstream arrangement.
      if (alreadyCovered)
        continue;
      roads.push_back(q);
      c.loop = q;
      r.contours.push_back(c);
    }
    return true;
  };
  struct Ray
  {
    Position direction;
    double width, length;
  };
  std::map<std::pair<int64_t, int64_t>, std::pair<Position, std::vector<Ray>>> junctions;
  for (auto &street : r.streets)
  {
    double height = terrain(street.a.x, street.a.z);
    for (size_t i = 1; i < street.path.size(); ++i)
    {
      auto a = street.path[i - 1], b = street.path[i];
      double length = dist(a, b);
      if (length < 1e-6)
        continue;
      if (!publishRoad(ribbon({street.id, a, b, street.halfWidth, street.arterial}), height))
        return fail("city streets require an engineered level platform");
      for (auto pair : std::vector<std::pair<Position, Position>>{{a, b}, {b, a}})
      {
        auto p = pair.first, q = pair.second;
        auto &node = junctions[{std::llround(p.x * 1e6), std::llround(p.z * 1e6)}];
        node.first = p;
        node.second.push_back({{(q.x - p.x) / length, 0, (q.z - p.z) / length}, street.halfWidth, length});
      }
    }
  }
  for (auto &[key, node] : junctions)
  {
    auto p = node.first;
    auto rays = node.second;
    std::sort(
        rays.begin(),
        rays.end(),
        [](const Ray &a, const Ray &b)
        {
          return std::atan2(a.direction.z, a.direction.x) < std::atan2(b.direction.z, b.direction.x);
        });
    for (size_t i = 0; i < rays.size(); ++i)
    {
      auto a = rays[i], b = rays[(i + 1) % rays.size()];
      double den = a.direction.x * b.direction.z - a.direction.z * b.direction.x, dot = a.direction.x * b.direction.x + a.direction.z * b.direction.z, angle = std::atan2(den, dot);
      if (angle <= 0)
        angle += 2 * 3.141592653589793;
      if (angle > 3.141592653589793 + 1e-5 && angle < 6.27)
      {
        Poly wedge{p};
        double start = std::atan2(a.direction.x, -a.direction.z);
        for (int k = 0; k <= 4; ++k)
        {
          double t = k / 4., alpha = start + (angle - 3.141592653589793) * t, width = a.width * (1 - t) + b.width * t;
          wedge.push_back({p.x + width * std::cos(alpha), 0, p.z + width * std::sin(alpha)});
        }
        if (area(wedge) > 1e-7 && !publishRoad(wedge, terrain(p.x, p.z), true))
          return fail("curved shoulder requires engineered platform");
        continue;
      }
      if (angle < .25 || angle > 3.10)
        continue;
      auto offsetIntersection = [&](double radius)
      {
        Position q{p.x - a.direction.z * (a.width + radius), 0, p.z + a.direction.x * (a.width + radius)}, r{p.x + b.direction.z * (b.width + radius), 0, p.z - b.direction.x * (b.width + radius)};
        double t = ((r.x - q.x) * b.direction.z - (r.z - q.z) * b.direction.x) / den;
        return Position{q.x + a.direction.x * t, 0, q.z + a.direction.z * t};
      };
      auto corner = offsetIntersection(0);
      double alongA = (corner.x - p.x) * a.direction.x + (corner.z - p.z) * a.direction.z, alongB = (corner.x - p.x) * b.direction.x + (corner.z - p.z) * b.direction.z;
      double radius = std::min(s.junctionRadius, std::max(0., std::min(a.length * .8 - alongA, b.length * .8 - alongB) * std::tan(angle * .5)));
      if (radius < .05)
        continue;
      auto center = offsetIntersection(radius);
      double start = std::atan2(-a.direction.x, a.direction.z);
      Position last{center.x + radius * std::cos(start), 0, center.z + radius * std::sin(start)};
      for (int j = 1; j <= 4; ++j)
      {
        double alpha = start + (angle - 3.141592653589793) * j / 4;
        Position next{center.x + radius * std::cos(alpha), 0, center.z + radius * std::sin(alpha)};
        Poly triangle{corner, last, next};
        if (area(triangle) < 0)
          std::reverse(triangle.begin(), triangle.end());
        if (area(triangle) > 1e-7 && !publishRoad(triangle, terrain(p.x, p.z), true))
          return fail("junction requires an engineered platform");
        last = next;
      }
    }
  }
  if (r.contours.size() > 2048)
    return fail("city contour budget exceeded");
  if (!s.emitBuildings)
  {
    out = std::move(r);
    return true;
  }
  if (s.blockHousing)
  {
    // Reuse the final-map block extractor. Voronoi cells and CDT seams are not
    // house frontages; only land edges adjoining a published road qualify.
    SurfaceMapSettings bounds;
    bounds.minX = minX;
    bounds.minZ = minZ;
    bounds.maxX = maxX;
    bounds.maxZ = maxZ;
    bounds.maximumFaces = 20000;
    SurfaceArrangement arranged;
    if (!arrangeSurfaceContours(r.contours, bounds, terrain, s.revision, arranged, error) || !extractCityStreetBlocks(arranged, 11, r.blocks, error))
      return false;
    std::set<uint64_t> exteriorBlocks;
    if (!r.suburbs.empty())
    {
      std::vector<CityStreetBlock> suburbs;
      if (!extractCityStreetBlocks(arranged, 16, suburbs, error))
        return false;
      uint64_t blockID = 0;
      for (const auto &block : r.blocks)
        blockID = std::max(blockID, block.id);
      for (auto &block : suburbs)
      {
        block.id = ++blockID;
        block.use = DistrictUse::Suburb;
        exteriorBlocks.insert(block.id);
        r.blocks.push_back(std::move(block));
      }
    }
    if (r.blocks.size() > 256)
      return fail("physical city block budget exceeded");
    struct BlockFront
    {
      Position a, b;
      uint64_t street = 0;
    };
    uint64_t objectID = 1, frontageID = 1;
    const double inset = s.setback + s.overhang;
    auto closest = [](Position p, Position a, Position b)
    {
      double length2 = (b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z);
      return mix(a, b, std::clamp(((p.x - a.x) * (b.x - a.x) + (p.z - a.z) * (b.z - a.z)) / length2, 0., 1.));
    };
    for (auto &block : r.blocks)
    {
      // Decompose for clipping only; all pieces participate in the same parcel
      // cut, and artificial decomposition edges never acquire street access.
      BoundaryRegistry blockRegistry;
      std::vector<BoundaryInput> blockEdges;
      PatchDomain blockDomain;
      blockDomain.id = block.id;
      uint64_t edgeID = 1, vertexID = 1;
      auto addRing = [&](const Poly &p, std::vector<BoundaryUse> &uses)
      {
        const auto first = vertexID;
        for (size_t k = 0; k < p.size(); ++k)
        {
          blockEdges.push_back({edgeID, {vertexID++, p[k]}, {k + 1 == p.size() ? first : vertexID, p[(k + 1) % p.size()]}, {}});
          uses.push_back({edgeID++, k + 1 == p.size()});
        }
      };
      addRing(block.boundary.outer, blockDomain.outer);
      for (const auto &hole : block.boundary.holes)
      {
        blockDomain.holes.emplace_back();
        addRing(hole, blockDomain.holes.back());
      }
      PatchMesh blockMesh;
      if (!blockRegistry.commit(blockEdges, error) || !compilePatchMesh(
                                                          blockDomain,
                                                          blockRegistry,
                                                          {},
                                                          [](double, double)
                                                          {
                                                            return 0.;
                                                          },
                                                          blockMesh,
                                                          error))
        return false;
      std::vector<Poly> pieces;
      for (auto t : blockMesh.triangles)
      {
        Poly p;
        for (auto i : t)
          p.push_back(blockMesh.vertices[i].position);
        if (area(p) < 0)
          std::reverse(p.begin(), p.end());
        pieces.push_back(std::move(p));
      }
      merge(pieces);
      const bool exterior = exteriorBlocks.count(block.id) != 0;
      // Outer neighbourhoods reserve some larger work/home courts before
      // parcel cuts, instead of trying to relabel tiny cottage plots afterward.
      const double outerProgramme = random(block.id, 95);
      double gardenFraction = 0;
      double plotArea = exterior ? (outerProgramme < .25 ? 240 : outerProgramme < .55 ? 110 : 70) : s.targetPlotArea, bestOverlap = 0;
      for (const auto &housing : s.housingAreas)
      {
        double overlap = 0;
        for (const auto &mask : housing.pieces)
          for (const auto &p : pieces)
            overlap += area(intersection(p, mask));
        if (overlap > bestOverlap + 1e-6 || (overlap > 0 && std::abs(overlap - bestOverlap) < 1e-6 && housing.id < block.district))
        {
          bestOverlap = overlap;
          block.district = housing.id;
          block.use = housing.use;
          plotArea = housing.targetPlotArea;
          gardenFraction = housing.gardenFraction;
        }
      }
      std::vector<BlockFront> fronts;
      auto rings = block.boundary.holes;
      rings.insert(rings.begin(), block.boundary.outer);
      for (const auto &ring : rings)
        for (size_t k = 0; k < ring.size(); ++k)
        {
          const auto a = ring[k], b = ring[(k + 1) % ring.size()], mid = mix(a, b, .5);
          uint64_t streetID = 0;
          double best = 1e30;
          for (const auto &street : r.streets)
            for (size_t j = 1; j < street.path.size(); ++j)
            {
              auto q = closest(mid, street.path[j - 1], street.path[j]);
              double d = dist(q, mid);
              // A road edge must be parallel to, and at the width of, its
              // supporting centerline. Envelope edges near roads do not count.
              auto u = street.path[j - 1], v = street.path[j];
              double parallel = std::abs((b.x - a.x) * (v.z - u.z) - (b.z - a.z) * (v.x - u.x)) / (dist(a, b) * dist(u, v));
              if (parallel < 1e-5 && std::abs(d - street.halfWidth) < .005 && d < best)
              {
                best = d;
                streetID = street.id;
              }
            }
          if (streetID)
            fronts.push_back({a, b, streetID});
          if (inset > 0)
          {
            auto band = ribbon({0, a, b, inset, false});
            std::vector<Poly> next;
            for (const auto &p : pieces)
            {
              auto clipped = intersection(p, band).empty() ? std::vector<Poly>{p} : subtract(p, band);
              next.insert(next.end(), clipped.begin(), clipped.end());
            }
            pieces = std::move(next);
          }
        }
      // Existing wall massing is protected even on blocks crossed by its ribbon.
      for (const auto &wall : r.walls)
      {
        auto expanded = wall;
        expanded.halfWidth += std::max(inset, s.wallSetback);
        // Extend end caps too: gate corners need the same clearance as wall faces.
        const double extension = std::max(inset, s.wallSetback);
        const double length = dist(wall.a, wall.b);
        expanded.a = mix(wall.a, wall.b, -extension / length);
        expanded.b = mix(wall.a, wall.b, 1 + extension / length);
        std::vector<Poly> next;
        for (const auto &p : pieces)
        {
          auto mask = ribbon(expanded);
          auto clipped = intersection(p, mask).empty() ? std::vector<Poly>{p} : subtract(p, mask);
          next.insert(next.end(), clipped.begin(), clipped.end());
        }
        pieces = std::move(next);
      }
      for (const auto &tower : r.towers)
      {
        std::vector<Poly> next;
        for (const auto &p : pieces)
        {
          auto clipped = subtract(p, towerClearance(tower));
          next.insert(next.end(), clipped.begin(), clipped.end());
        }
        pieces = std::move(next);
      }
      // Parcel-local merging avoids a cubic all-pairs merge of an entire city block.
      if (pieces.empty() || fronts.empty())
      {
        block.rejection = "no buildable road frontage";
        continue;
      }
      std::vector<std::vector<Poly>> leaves;
      uint32_t parcelCuts = 0;
      std::function<void(std::vector<Poly>, unsigned, uint64_t)> partition;
      partition = [&](std::vector<Poly> land, unsigned depth, uint64_t key)
      {
        double total = 0;
        for (const auto &p : land)
          total += area(p);
        if (total <= plotArea * (.8 + .6 * random(key, 81)) || depth == 12 || parcelCuts >= 4096)
        {
          merge(land);
          leaves.push_back(std::move(land));
          return;
        }
        // Choose a local constructed axis from the convex hull's longest edge.
        Poly vertices;
        for (const auto &p : land)
          vertices.insert(vertices.end(), p.begin(), p.end());
        auto outline = hull(vertices);
        Position a{}, b{};
        double longest = 0;
        for (size_t k = 0; k < outline.size(); ++k)
          if (dist(outline[k], outline[(k + 1) % outline.size()]) > longest)
          {
            a = outline[k];
            b = outline[(k + 1) % outline.size()];
            longest = dist(a, b);
          }
        double nx = (b.x - a.x) / longest, nz = (b.z - a.z) / longest, lo = 1e30, hi = -1e30;
        for (auto p : outline)
        {
          lo = std::min(lo, nx * p.x + nz * p.z);
          hi = std::max(hi, nx * p.x + nz * p.z);
        }
        double cut = lo + (hi - lo) * (.42 + .16 * random(key, 82));
        std::vector<Poly> left, right;
        double leftArea = 0, rightArea = 0;
        for (const auto &p : land)
        {
          auto l = clip(p, nx, nz, cut), q = clip(p, -nx, -nz, -cut);
          if (!l.empty())
          {
            leftArea += area(l);
            left.push_back(std::move(l));
          }
          if (!q.empty())
          {
            rightArea += area(q);
            right.push_back(std::move(q));
          }
        }
        if (std::min(leftArea, rightArea) < plotArea * .22)
        {
          merge(land);
          leaves.push_back(std::move(land));
          return;
        }
        ++parcelCuts;
        partition(std::move(left), depth + 1, key * 2 + 1);
        partition(std::move(right), depth + 1, key * 2 + 2);
      };
      partition(std::move(pieces), 0, block.id * 8192);
      for (auto &leaf : leaves)
      {
        // A mostly convex parcel may use its dominant component. Keep the
        // exact remainder separately; a convex hull would cover excluded land.
        if (leaf.size() != 1)
        {
          auto dominant = std::max_element(
              leaf.begin(),
              leaf.end(),
              [](const Poly &a, const Poly &b)
              {
                return area(a) < area(b);
              });
          double total = 0;
          for (const auto &p : leaf)
            total += area(p);
          if (dominant == leaf.end() || area(*dominant) < total * .75)
          {
            for (auto &p : leaf)
              block.unallocated.push_back({std::move(p), {}});
            continue;
          }
          std::iter_swap(leaf.begin(), dominant);
          for (size_t i = 1; i < leaf.size(); ++i)
            block.unallocated.push_back({std::move(leaf[i]), {}});
          leaf.resize(1);
        }
        const auto &plot = leaf.front();
        const BlockFront *chosen = nullptr;
        Position entrance{}, access{};
        double frontageLength = 0;
        bool boundaryLot = false;
        for (const auto &f : fronts)
        {
          double len = dist(f.a, f.b), tx = (f.b.x - f.a.x) / len, tz = (f.b.z - f.a.z) / len, nx = -tz, nz = tx;
          double lo = 1e30, hi = -1e30;
          for (auto p : plot)
            if (std::abs((p.x - f.a.x) * nx + (p.z - f.a.z) * nz - inset) < .005)
            {
              double t = (p.x - f.a.x) * tx + (p.z - f.a.z) * tz;
              lo = std::min(lo, t);
              hi = std::max(hi, t);
            }
          lo = std::max(0., lo);
          hi = std::min(len, hi);
          boundaryLot |= hi - lo > 1e-5;
          if (hi - lo < 2 || hi - lo <= frontageLength)
            continue;
          auto origin = mix(f.a, f.b, (lo + hi) * .5 / len);
          auto e = Position{origin.x + nx * (inset + s.frontageGap * .5), 0, origin.z + nz * (inset + s.frontageGap * .5)};
          if (!inside(plot, e))
            continue;
          Position roadPoint{};
          double best = 1e30;
          for (const auto &street : r.streets)
            if (street.id == f.street)
              for (size_t j = 1; j < street.path.size(); ++j)
              {
                auto q = closest(origin, street.path[j - 1], street.path[j]);
                if (dist(q, origin) < best)
                {
                  best = dist(q, origin);
                  roadPoint = q;
                }
              }
          if (!coveredSegment(e, roadPoint, usable))
            continue;
          bool blocked = false;
          auto passage = ribbon({0, e, roadPoint, .25, false});
          for (const auto &wall : r.walls)
            blocked |= cityPolygonsOverlap(passage, ribbon(wall));
          for (const auto &tower : r.towers)
            blocked |= cityPolygonsOverlap(passage, tower.footprint);
          for (const auto &o : r.objects)
            blocked |= cityPolygonsOverlap(passage, o.clearance);
          if (blocked)
            continue;
          chosen = &f;
          entrance = e;
          access = roadPoint;
          frontageLength = hi - lo;
        }
        if (!chosen)
        {
          (boundaryLot ? block.unallocated : block.courtyards).push_back({plot, {}});
          continue;
        }
        // Shrink the allocated parcel rather than guessing an independent box.
        // This preserves trapezoidal corners and coherent rows around the block.
        Poly roof = plot;
        for (size_t k = 0; k < plot.size(); ++k)
        {
          auto a = plot[k], b = plot[(k + 1) % plot.size()];
          double nx = b.z - a.z, nz = a.x - b.x;
          roof = clip(roof, nx, nz, nx * a.x + nz * a.z - dist(a, b) * s.frontageGap * .5);
        }
        if (s.regularBuildingShells)
        {
          // Fit a usable orthogonal shell to the actual street frontage. The
          // irregular plot survives as ownership/yard; never roof its acute tips.
          const double length = dist(chosen->a, chosen->b);
          const double tx = (chosen->b.x - chosen->a.x) / length, tz = (chosen->b.z - chosen->a.z) / length;
          auto at = [&](double x, double z)
          {
            return Position{entrance.x + tx * x - tz * z, 0, entrance.z + tz * x + tx * z};
          };
          Poly bestRoof;
          double bestArea = 0;
          const double widthLimit = std::min(s.maximumWidth, frontageLength - s.frontageGap);
          for (unsigned attempt = 0; attempt < 10; ++attempt)
          {
            const double width = widthLimit * (1 - attempt * .07);
            if (width < 3.5)
              continue;
            if (!inside(roof, at(-width * .5, 0)) || !inside(roof, at(width * .5, 0)))
              continue;
            double lo = 0, hi = std::min({s.maximumDepth, width * 3., area(plot) * (1 - gardenFraction) / width});
            for (unsigned k = 0; k < 28; ++k)
            {
              double depth = (lo + hi) * .5;
              if (inside(roof, at(-width * .5, depth)) && inside(roof, at(width * .5, depth)))
                lo = depth;
              else
                hi = depth;
            }
            if (lo < 3.5 || width > lo * 3 || width * lo <= bestArea)
              continue;
            bestArea = width * lo;
            bestRoof = {at(-width * .5, 0), at(width * .5, 0), at(width * .5, lo), at(-width * .5, lo)};
          }
          if (bestRoof.empty())
          {
            block.unallocated.push_back({plot, {}});
            continue;
          }
          roof = std::move(bestRoof);
        }
        // Clipping can leave sub-centimetre corner edges. Collapse those
        // conservatively inside the convex plot before float mesh packing;
        // otherwise their vertical wall normals depend on rounding noise.
        bool simplified = true;
        while (simplified && roof.size() > 3)
        {
          simplified = false;
          for (size_t k = 0; k < roof.size(); ++k)
            if (dist(roof[k], roof[(k + 1) % roof.size()]) < .01)
            {
              roof.erase(roof.begin() + k);
              simplified = true;
              break;
            }
        }
        double perimeter = 0;
        for (size_t k = 0; k < roof.size(); ++k)
          perimeter += dist(roof[k], roof[(k + 1) % roof.size()]);
        Poly roomCore = roof;
        for (size_t k = 0; k < roof.size(); ++k)
        {
          auto a = roof[k], b = roof[(k + 1) % roof.size()];
          double nx = b.z - a.z, nz = a.x - b.x;
          roomCore = clip(roomCore, nx, nz, nx * a.x + nz * a.z - dist(a, b) * 1.5);
        }
        if (roof.size() < 3 || roomCore.empty() || area(roof) < s.minimumWidth * s.minimumDepth * .6 || 2 * area(roof) < perimeter * 1.5)
        {
          block.unallocated.push_back({plot, {}});
          continue;
        }
        bool blocksEntrance = false;
        for (const auto &placed : r.objects)
          blocksEntrance |= cityPolygonsOverlap(plot, ribbon({0, placed.entrance, placed.access, .25, false}));
        if (blocksEntrance)
        {
          block.unallocated.push_back({plot, {}});
          continue;
        }
        double low = 1e30, high = -1e30;
        for (auto p : roof)
        {
          double h = terrain(p.x, p.z);
          if (!std::isfinite(h))
            return fail("nonfinite parcel terrain");
          low = std::min(low, h);
          high = std::max(high, h);
        }
        if (high - low > s.maximumFoundationRelief)
        {
          block.unallocated.push_back({plot, {}});
          continue;
        }
        if (r.objects.size() >= s.maximumObjects)
          return fail("city object budget exceeded");
        CityObject object;
        object.id = objectID++;
        object.block = block.id;
        object.outsideWalls = exterior;
        // Allocation ownership is metadata only; physical blocks control fits.
        Position center{};
        for (size_t k = 0; k < roof.size(); ++k)
        {
          auto a = roof[k], b = roof[(k + 1) % roof.size()];
          double weight = a.x * b.z - a.z * b.x;
          center.x += (a.x + b.x) * weight / (6 * area(roof));
          center.z += (a.z + b.z) * weight / (6 * area(roof));
        }
        for (const auto &ward : r.wards)
          for (const auto &p : ward.pieces)
            if (inside(p, center))
              object.ward = ward.id;
        object.street = chosen->street;
        object.frontage = frontageID++;
        object.kind = CityObjectKind::House;
        object.buildingType = object.requestedBuildingType = block.use == DistrictUse::Craftsmen                                                 ? CityBuildingType::WorkshopYard
                                                             : (block.use == DistrictUse::Manor || block.use == DistrictUse::WealthyResidential) ? CityBuildingType::MerchantHouse
                                                             : block.use == DistrictUse::WorkerHousing                                           ? CityBuildingType::WorkerLodging
                                                             : block.use == DistrictUse::Suburb                                                  ? CityBuildingType::Cottage
                                                             : area(plot) > plotArea * 1.25                                                      ? CityBuildingType::MerchantHouse
                                                                                                                                                 : CityBuildingType::ShopHouse;
        if (block.use == DistrictUse::Craftsmen)
          object.kind = CityObjectKind::Workshop;
        object.footprint = plot;
        object.clearance = plot;
        double height = exterior ? 4 + 2 * std::floor(random(object.id, 83) * 2) : 5 + 2 * std::floor(random(object.id, 83) * 3);
        object.parts.push_back({roof, height});
        if (s.regularBuildingShells && roof.size() == 4)
        {
          const double width = dist(roof[0], roof[1]), depth = dist(roof[1], roof[2]);
          double variety = random(object.id, 94);
          if (exterior && block.use == DistrictUse::Suburb)
          {
            object.buildingType = object.requestedBuildingType = variety < .55 ? CityBuildingType::Cottage : variety < .8 ? CityBuildingType::ShopHouse : CityBuildingType::WorkshopYard;
            object.kind = object.buildingType == CityBuildingType::WorkshopYard ? CityObjectKind::Workshop : CityObjectKind::House;
          }
          // Larger programmes use a front range plus one or two rear wings.
          // Each wing is rectangular and the remaining court stays unroofed.
          if (width >= 10 && depth >= 10 && (variety > .65 || (exterior && plotArea >= 200) || block.use == DistrictUse::Manor || block.use == DistrictUse::WealthyResidential || block.use == DistrictUse::Craftsmen))
          {
            auto at = [&](double x, double z)
            {
              return Position{roof[0].x + (roof[1].x - roof[0].x) * x / width + (roof[3].x - roof[0].x) * z / depth, 0, roof[0].z + (roof[1].z - roof[0].z) * x / width + (roof[3].z - roof[0].z) * z / depth};
            };
            auto rectangle = [&](double x0, double z0, double x1, double z1)
            {
              return Poly{at(x0, z0), at(x1, z0), at(x1, z1), at(x0, z1)};
            };
            const double front = std::min(5., depth * .4), wing = std::min(4., width * .28);
            bool courtyard = width >= 13 && variety > .86;
            object.parts = {{rectangle(0, 0, width, front), height}, {rectangle(0, front, wing, depth), std::max(4., height - 2)}};
            if (courtyard)
              object.parts.push_back({rectangle(width - wing, front, width, depth), std::max(4., height - 2)});
            object.courtyard = rectangle(wing, front, courtyard ? width - wing : width, depth);
            object.buildingType = object.requestedBuildingType = block.use == DistrictUse::WealthyResidential || block.use == DistrictUse::Manor ? CityBuildingType::MerchantHouse
                                                                 : block.use == DistrictUse::WorkerHousing                                       ? CityBuildingType::WorkerLodging
                                                                 : block.use == DistrictUse::Craftsmen || exterior                               ? CityBuildingType::WorkshopYard
                                                                 : courtyard                                                                     ? CityBuildingType::Inn
                                                                                                                                                 : CityBuildingType::MerchantHouse;
            if (object.buildingType == CityBuildingType::WorkshopYard)
              object.kind = CityObjectKind::Workshop;
          }
        }
        center = {};
        double shellArea = 0;
        for (const auto &part : object.parts)
        {
          shellArea += area(part.footprint);
          for (size_t k = 0; k < part.footprint.size(); ++k)
          {
            auto a = part.footprint[k], b = part.footprint[(k + 1) % part.footprint.size()];
            double weight = a.x * b.z - a.z * b.x;
            center.x += (a.x + b.x) * weight / 6;
            center.z += (a.z + b.z) * weight / 6;
          }
        }
        center.x /= shellArea;
        center.z /= shellArea;
        for (const auto &ward : r.wards)
          for (const auto &piece : ward.pieces)
            if (inside(piece, center))
              object.ward = ward.id;
        object.height = object.parts.front().height;
        object.foundation = high;
        object.entrance = entrance;
        object.access = access;
        for (const auto &street : r.streets)
          if (street.id == object.street)
          {
            int side = 1;
            double nearest = 1e30;
            for (size_t k = 1; k < street.path.size(); ++k)
            {
              double distance = segmentDistance(access, street.path[k - 1], street.path[k]);
              if (distance < nearest)
              {
                nearest = distance;
                side = cross(street.path[k - 1], street.path[k], entrance) >= 0 ? 1 : -1;
              }
            }
            r.frontages.push_back({object.frontage, object.ward, street.a, street.b, object.street, side, street.path});
          }
        r.objects.push_back(std::move(object));
      }
    }
    out = std::move(r);
    return true;
  }
  // Arc-length frontage walks, with bounded smaller variants and explicit failed gaps.
  const double binSize = s.maximumWidth + s.maximumDepth + 2 * s.overhang + 1;
  std::map<std::pair<int, int>, std::vector<size_t>> occupiedBins;
  auto bins = [&](const Poly &p)
  {
    double x0 = 1e30, x1 = -1e30, z0 = 1e30, z1 = -1e30;
    for (auto v : p)
    {
      x0 = std::min(x0, v.x);
      x1 = std::max(x1, v.x);
      z0 = std::min(z0, v.z);
      z1 = std::max(z1, v.z);
    }
    std::vector<std::pair<int, int>> keys;
    for (int x = int(std::floor(x0 / binSize)); x <= int(std::floor(x1 / binSize)); ++x)
      for (int z = int(std::floor(z0 / binSize)); z <= int(std::floor(z1 / binSize)); ++z)
        keys.push_back({x, z});
    return keys;
  };
  uint64_t oid = 1, fid = 1;
  std::map<std::tuple<uint64_t, uint64_t, int>, uint64_t> frontageIds;
  // Walk retained street paths, not the stale straight Voronoi chords or the
  // convex decomposition seams. Sampling is by arc length across path vertices.
  for (const auto &street : r.streets)
  {
    std::vector<double> lengths{0};
    for (size_t i = 1; i < street.path.size(); ++i)
      lengths.push_back(lengths.back() + dist(street.path[i - 1], street.path[i]));
    const double length = lengths.back();
    auto sample = [&](double along)
    {
      along = std::clamp(along, 0., length);
      size_t i = std::min(size_t(std::upper_bound(lengths.begin(), lengths.end(), along) - lengths.begin()), lengths.size() - 1);
      i = std::max(size_t(1), i);
      return mix(street.path[i - 1], street.path[i], (along - lengths[i - 1]) / (lengths[i] - lengths[i - 1]));
    };
    for (int side : {1, -1})
      for (double cursor = 3; cursor + s.minimumWidth < length - 3;)
      {
        if (r.objects.size() >= s.maximumObjects)
          return fail("city object budget exceeded");
        const uint64_t id = oid++;
        const double choice = random(id, 7) * (s.houseWeight + s.workshopWeight + s.gardenWeight);
        const auto kind = choice < s.houseWeight ? CityObjectKind::House : choice < s.houseWeight + s.workshopWeight ? CityObjectKind::Workshop : CityObjectKind::Garden;
        const double variant = random(id, 40);
        CityBuildingType type = CityBuildingType::Garden;
        if (kind == CityObjectKind::Workshop)
          type = variant < .7 ? CityBuildingType::WorkshopYard : CityBuildingType::Warehouse;
        else if (kind == CityObjectKind::House)
          type = variant < (street.arterial ? .18 : .55) ? CityBuildingType::Cottage : variant < .72 ? CityBuildingType::ShopHouse : variant < .91 ? CityBuildingType::MerchantHouse : CityBuildingType::Inn;
        auto dimensions = [&](CityBuildingType program)
        {
          double w0 = 0, w1 = .22, d0 = 0, d1 = .25;
          if (program == CityBuildingType::ShopHouse)
          {
            w1 = .25;
            d0 = .15;
            d1 = .6;
          }
          if (program == CityBuildingType::MerchantHouse)
          {
            w0 = .4;
            w1 = .8;
            d0 = .4;
            d1 = .8;
          }
          if (program == CityBuildingType::Inn)
          {
            w0 = .6;
            w1 = 1;
            d0 = .5;
            d1 = 1;
          }
          if (program == CityBuildingType::WorkshopYard)
          {
            w0 = .2;
            w1 = .65;
            d0 = .35;
            d1 = .85;
          }
          if (program == CityBuildingType::Warehouse)
          {
            w0 = .35;
            w1 = .8;
            d0 = .55;
            d1 = 1;
          }
          return std::make_pair(s.minimumWidth + (w0 + random(id, 5) * (w1 - w0)) * (s.maximumWidth - s.minimumWidth), s.minimumDepth + (d0 + random(id, 6) * (d1 - d0)) * (s.maximumDepth - s.minimumDepth));
        };
        double advance = s.minimumWidth;
        CityObject obj;
        obj.id = id;
        obj.street = street.id;
        obj.requestedBuildingType = type;
        obj.kind = CityObjectKind::RejectedGap;
        obj.entrance = sample(cursor);
        for (int attempt = 0; attempt < 6; ++attempt)
        {
          // Large compounds yield to a compact frontage building if the plot
          // cannot fit them. Publish that substitution instead of a tiny "inn".
          const auto program = attempt < 3 || kind == CityObjectKind::Garden ? type : kind == CityObjectKind::House ? CityBuildingType::Cottage : CityBuildingType::WorkshopYard;
          auto [width, depth] = dimensions(attempt < 3 ? program : CityBuildingType::Cottage);
          const double ww = std::max(s.minimumWidth, width * (1 - .08 * (attempt % 3))), dd = std::max(s.minimumDepth, depth * (1 - .08 * (attempt % 3)));
          if (cursor + ww > length - 3)
            continue;
          const auto access = sample(cursor + ww * .5);
          const auto a = sample(cursor), b = sample(cursor + ww);
          const double span = dist(a, b);
          if (span < 1e-6)
            continue;
          const double tx = (b.x - a.x) / span * side, tz = (b.z - a.z) / span * side;
          const double nx = -tz, nz = tx;
          auto rectangle = [&](double extra)
          {
            auto at = [&](double t, double n)
            {
              return Position{access.x + tx * t + nx * n, 0, access.z + tz * t + nz * n};
            };
            const double offset = street.halfWidth + s.setback;
            return Poly{at(-ww * .5 - extra, offset - extra), at(ww * .5 + extra, offset - extra), at(ww * .5 + extra, offset + dd + extra), at(-ww * .5 - extra, offset + dd + extra)};
          };
          auto clearance = rectangle(s.overhang);
          // Sum the clipped areas: a building may cross an artificial convex
          // decomposition seam, but must fit completely inside one actual ward.
          uint64_t ward = 0;
          for (const auto &w : r.wards)
          {
            double covered = 0;
            for (const auto &piece : w.pieces)
              covered += area(intersection(clearance, piece));
            if (std::abs(covered - area(clearance)) < 1e-6)
            {
              ward = w.id;
              break;
            }
          }
          bool ok = ward != 0;
          for (const auto &q : roads)
            if (cityPolygonsOverlap(clearance, q))
              ok = false;
          for (const auto &wall : r.walls)
          {
            auto expanded = wall;
            expanded.halfWidth += s.wallSetback;
            expanded.a = mix(wall.a, wall.b, -s.wallSetback / dist(wall.a, wall.b));
            expanded.b = mix(wall.a, wall.b, 1 + s.wallSetback / dist(wall.a, wall.b));
            if (cityPolygonsOverlap(clearance, ribbon(expanded)))
              ok = false;
          }
          for (const auto &tower : r.towers)
            if (cityPolygonsOverlap(clearance, towerClearance(tower)))
              ok = false;
          std::set<size_t> candidates;
          for (auto key : bins(clearance))
          {
            auto it = occupiedBins.find(key);
            if (it != occupiedBins.end())
              candidates.insert(it->second.begin(), it->second.end());
          }
          for (auto index : candidates)
            if (cityPolygonsOverlap(clearance, r.objects[index].clearance))
              ok = false;
          const auto footprint = rectangle(0);
          const auto entrance = mix(footprint[0], footprint[1], .5);
          if (!coveredSegment(entrance, access, usable))
            ok = false;
          // Preserve both new and already published entrance paths.
          const auto passage = ribbon({0, entrance, access, .25, false});
          double passageArea = 0;
          for (const auto &piece : usable)
            passageArea += area(intersection(passage, piece));
          if (std::abs(passageArea - area(passage)) > 1e-6)
            ok = false;
          for (const auto &wall : r.walls)
            if (cityPolygonsOverlap(passage, ribbon(wall)))
              ok = false;
          for (const auto &tower : r.towers)
            if (cityPolygonsOverlap(passage, tower.footprint))
              ok = false;
          for (const auto &placed : r.objects)
            if (placed.kind != CityObjectKind::RejectedGap && (cityPolygonsOverlap(passage, placed.clearance) || cityPolygonsOverlap(clearance, ribbon({0, placed.entrance, placed.access, .25, false}))))
              ok = false;
          double low = 1e30, high = -1e30;
          for (auto q : clearance)
          {
            const double y = terrain(q.x, q.z);
            if (!std::isfinite(y))
              return fail("nonfinite city terrain");
            low = std::min(low, y);
            high = std::max(high, y);
          }
          if (high - low > s.maximumFoundationRelief)
            ok = false;
          if (!ok)
            continue;
          auto [it, inserted] = frontageIds.emplace(std::make_tuple(ward, street.id, side), fid);
          if (inserted)
          {
            r.frontages.push_back({fid++, ward, street.a, street.b});
            r.frontages.back().street = street.id;
            r.frontages.back().side = side;
            r.frontages.back().path = street.path;
          }
          obj.frontage = it->second;
          obj.ward = ward;
          obj.footprint = footprint;
          obj.clearance = clearance;
          obj.foundation = high;
          obj.entrance = entrance;
          obj.kind = kind;
          obj.buildingType = program;
          obj.compactFallback = attempt >= 3;
          obj.height = kind == CityObjectKind::Garden           ? 0
                       : program == CityBuildingType::Cottage   ? 3 + 2 * std::floor(random(id, 8) * 2)
                       : program == CityBuildingType::Warehouse ? 5
                                                                : 5 + 2 * std::floor(random(id, 8) * 2);
          // Non-overlapping wings share constructed axes. Clear plot setbacks
          // and roof overhangs were checked before choosing the massing.
          auto at = [&](double u, double v)
          {
            return Position{footprint[0].x + tx * u + nx * v, 0, footprint[0].z + tz * u + nz * v};
          };
          auto rect = [&](double u, double v, double w, double d)
          {
            return Poly{at(u, v), at(u + w, v), at(u + w, v + d), at(u, v + d)};
          };
          auto part = [&](double u, double v, double w, double d, double height)
          {
            obj.parts.push_back({rect(u, v, w, d), height});
          };
          if (kind != CityObjectKind::Garden)
          {
            if ((program == CityBuildingType::Inn || (program == CityBuildingType::MerchantHouse && variant > .82)) && ww >= 12 && dd >= 13)
            {
              const double wing = std::min(ww * .28, 5.), rear = std::min(dd * .32, 6.);
              part(0, dd - rear, ww, rear, obj.height);
              part(0, 0, wing, dd - rear, obj.height - 1);
              part(ww - wing, 0, wing, dd - rear, obj.height - 1);
              obj.courtyard = rect(wing, 0, ww - 2 * wing, dd - rear);
              // The open front gives this court a direct path to the street.
              obj.entrance = at(ww * .5, dd - rear);
            }
            else if (program == CityBuildingType::WorkshopYard && dd >= 12 && ww >= 8)
            {
              const double front = dd * .35, rear = dd * .22;
              part(0, 0, ww, front, obj.height);
              part(0, dd - rear, ww * .55, rear, 3);
              obj.courtyard = rect(0, front, ww, dd - front - rear);
            }
            else if (program == CityBuildingType::MerchantHouse || (program != CityBuildingType::Warehouse && random(id, 41) < .4))
            {
              const double front = dd * .48, wing = ww * .46;
              part(0, 0, ww, front, obj.height);
              part(0, front, wing, dd - front, std::max(3., obj.height - 2));
              obj.courtyard = rect(wing, front, ww - wing, dd - front);
            }
            else
              part(0, 0, ww, dd, obj.height);
          }
          obj.access = access;
          advance = ww;
          break;
        }
        if (obj.kind == CityObjectKind::RejectedGap)
          ++r.rejectedAreas;
        else
          for (auto key : bins(obj.clearance))
            occupiedBins[key].push_back(r.objects.size());
        r.objects.push_back(obj);
        cursor += advance + s.frontageGap;
      }
  }
  if (r.contours.size() > 2048)
    return fail("city contour budget exceeded");
  out = std::move(r);
  return true;
}
bool meshCityInstances(const CityLayout &city, const std::function<double(double, double)> &terrain, std::vector<CityInstanceTriangle> &out, std::string *error)
{
  std::vector<CityInstanceTriangle> result;
  auto add = [&](Position a, Position b, Position c, uint64_t id, uint32_t mat)
  {
    result.push_back({{a, b, c}, id, mat});
  };
  auto block = [&](Poly p, double foundation, double height, uint64_t id, bool roof, bool hip = false)
  {
    if (roof && p.size() == 4 && dist(p[0], p[1]) < dist(p[0], p[3]))
      std::rotate(p.begin(), p.begin() + 1, p.end()); // Ridge follows the wing's long axis.
    Poly bottom = p, top = p;
    for (size_t i = 0; i < p.size(); ++i)
    {
      bottom[i].y = terrain(p[i].x, p[i].z);
      top[i].y = foundation + height;
      if (!std::isfinite(bottom[i].y) || bottom[i].y > foundation + 1e-6)
        return false;
    }
    for (size_t i = 0; i < p.size(); ++i)
    {
      size_t j = (i + 1) % p.size();
      add(bottom[i], top[i], top[j], id, 12);
      add(bottom[i], top[j], bottom[j], id, 12);
    }
    if (roof && p.size() != 4)
    {
      Position peak{};
      double perimeter = 0;
      for (size_t i = 0; i < p.size(); ++i)
      {
        peak.x += p[i].x / p.size();
        peak.z += p[i].z / p.size();
        perimeter += dist(p[i], p[(i + 1) % p.size()]);
      }
      peak.y = foundation + height + std::min(3., area(p) / perimeter * .5);
      for (size_t i = 0; i < p.size(); ++i)
        add(top[i], peak, top[(i + 1) % p.size()], id, 13);
    }
    else if (roof)
    {
      Position a = mix(top[0], top[3], .5), b = mix(top[1], top[2], .5);
      a.y = b.y = foundation + height + dist(p[0], p[3]) * .28;
      if (hip)
      {
        const auto start = a, end = b;
        a = mix(start, end, .18);
        b = mix(start, end, .82);
        a.y = b.y = foundation + height + (dist(p[0], p[3]) + dist(p[1], p[2])) * .14;
      }
      add(top[0], a, b, id, 13);
      add(top[0], b, top[1], id, 13);
      add(top[3], top[2], b, id, 13);
      add(top[3], b, a, id, 13);
      add(top[0], top[3], a, id, hip ? 13 : 12);
      add(top[1], b, top[2], id, hip ? 13 : 12);
    }
    else
    {
      for (size_t i = 1; i + 1 < top.size(); ++i)
        add(top[0], top[i + 1], top[i], id, 12);
    }
    return true;
  };
  if (!terrain)
  {
    if (error)
      *error = "missing instance terrain";
    return false;
  }
  for (auto &o : city.objects)
    if (o.kind == CityObjectKind::House || o.kind == CityObjectKind::Workshop)
    {
      if (o.footprint.size() < 3 || o.footprint.size() > 128 || !convex(o.footprint) || !std::isfinite(o.foundation) || !std::isfinite(o.height) || o.height <= 0 || o.parts.empty())
      {
        if (error)
          *error = "invalid building or unsupported foundation";
        return false;
      }
      if (!o.courtyard.empty() && (!convex(o.courtyard) || area(intersection(o.courtyard, o.footprint)) < area(o.courtyard) - 1e-6))
      {
        if (error)
          *error = "invalid courtyard";
        return false;
      }
      for (size_t i = 0; i < o.parts.size(); ++i)
      {
        const auto &part = o.parts[i];
        for (size_t j = 0; j < i; ++j)
          if (area(intersection(part.footprint, o.parts[j].footprint)) > 1e-6)
          {
            if (error)
              *error = "overlapping building wings";
            return false;
          }
        if (!o.courtyard.empty() && area(intersection(part.footprint, o.courtyard)) > 1e-6)
        {
          if (error)
            *error = "building wing covers courtyard";
          return false;
        }
        if (part.footprint.size() < 3 || part.footprint.size() > 128 || (!o.block && part.footprint.size() != 4) || !convex(part.footprint) || !std::isfinite(part.height) || part.height <= 0 ||
            area(intersection(part.footprint, o.footprint)) < area(part.footprint) - 1e-6 || !block(part.footprint, o.foundation, part.height, o.id, true, o.block != 0))
        {
          if (error)
            *error = "invalid building wing";
          return false;
        }
      }
    }
  for (auto &w : city.walls)
  {
    auto p = ribbon(w);
    double high = -1e30;
    for (auto v : p)
      high = std::max(high, terrain(v.x, v.z));
    if (!block(p, high, 4, w.id, false))
    {
      if (error)
        *error = "invalid wall foundation";
      return false;
    }
  }
  for (const auto &tower : city.towers)
  {
    if (!convex(tower.footprint) || !std::isfinite(tower.height) || tower.height <= 0)
    {
      if (error)
        *error = "invalid wall tower";
      return false;
    }
    double high = -1e30;
    for (auto v : tower.footprint)
      high = std::max(high, terrain(v.x, v.z));
    if (!block(tower.footprint, high, tower.height, tower.id, false))
    {
      if (error)
        *error = "invalid tower foundation";
      return false;
    }
  }
  out = std::move(result);
  return true;
}
} // namespace procedural::world::rewrite
