#include "WorldSettlementSiteAdapter.hpp"
#include "MapClimate.hpp"
#include "MapHierarchy.hpp"
#include "WorldNativeGenerator.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
namespace procedural::world::rewrite
{
namespace
{
using Poly = std::vector<Position>;
constexpr double kNoWater = 1e9;
double cross(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}
double signedArea(const Poly &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  return a * .5;
}
double length(Position a, Position b)
{
  return std::hypot(a.x - b.x, a.z - b.z);
}
Position centroid(const Poly &p)
{
  Position c;
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
  {
    auto u = p[i], v = p[(i + 1) % p.size()];
    double w = u.x * v.z - v.x * u.z;
    a += w;
    c.x += (u.x + v.x) * w;
    c.z += (u.z + v.z) * w;
  }
  if (std::abs(a) < 1e-12)
    return p.empty() ? Position{} : p.front();
  return {c.x / (3 * a), 0, c.z / (3 * a)};
}
Poly toPoly(const std::vector<Point2> &area)
{
  Poly p;
  p.reserve(area.size());
  for (auto q : area)
    p.push_back({q.x, 0, q.y});
  if (signedArea(p) < 0)
    std::reverse(p.begin(), p.end());
  return p;
}
bool inside(const Poly &p, Position q)
{
  bool in = false;
  for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
    if ((p[i].z > q.z) != (p[j].z > q.z))
    {
      double t = (q.z - p[i].z) / (p[j].z - p[i].z);
      if (q.x < p[i].x + t * (p[j].x - p[i].x))
        in = !in;
    }
  return in;
}
double segmentDistance(Position p, Position a, Position b)
{
  double dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz;
  double t = l2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / l2, 0., 1.) : 0;
  return std::hypot(p.x - (a.x + dx * t), p.z - (a.z + dz * t));
}
double polygonDistance(Position p, const Poly &poly)
{
  if (inside(poly, p))
    return 0;
  double d = INFINITY;
  for (size_t i = 0; i < poly.size(); ++i)
    d = std::min(d, segmentDistance(p, poly[i], poly[(i + 1) % poly.size()]));
  return d;
}
Poly clipToRect(Poly p, double minX, double minZ, double maxX, double maxZ)
{
  // Sutherland-Hodgman against the four window half-planes; convex in, convex out.
  for (int side = 0; side < 4 && !p.empty(); ++side)
  {
    Poly out;
    auto keep = [&](Position q)
    {
      switch (side)
      {
      case 0:
        return q.x >= minX;
      case 1:
        return q.x <= maxX;
      case 2:
        return q.z >= minZ;
      default:
        return q.z <= maxZ;
      }
    };
    auto cut = [&](Position u, Position v)
    {
      double t = 0;
      Position r;
      switch (side)
      {
      case 0:
        t = (minX - u.x) / (v.x - u.x);
        r = {minX, 0, u.z + (v.z - u.z) * t};
        break;
      case 1:
        t = (maxX - u.x) / (v.x - u.x);
        r = {maxX, 0, u.z + (v.z - u.z) * t};
        break;
      case 2:
        t = (minZ - u.z) / (v.z - u.z);
        r = {u.x + (v.x - u.x) * t, 0, minZ};
        break;
      default:
        t = (maxZ - u.z) / (v.z - u.z);
        r = {u.x + (v.x - u.x) * t, 0, maxZ};
        break;
      }
      return r;
    };
    for (size_t i = 0; i < p.size(); ++i)
    {
      auto u = p[i], v = p[(i + 1) % p.size()];
      bool ku = keep(u), kv = keep(v);
      if (ku)
        out.push_back(u);
      if (ku != kv)
        out.push_back(cut(u, v));
    }
    p = std::move(out);
  }
  Poly clean;
  for (auto q : p)
    if (clean.empty() || length(clean.back(), q) > 1e-4)
      clean.push_back(q);
  while (clean.size() > 1 && length(clean.front(), clean.back()) <= 1e-4)
    clean.pop_back();
  return clean.size() < 3 || signedArea(clean) < 1e-3 ? Poly{} : clean;
}
// Shared boundaries between cells refined under different parents meet at
// T-junctions whose vertices sit a few nanometres off the neighbour's edge.
// Snap those vertices onto the edge and insert them, so every tiling test
// downstream sees exactly shared vertices instead of micron-sized overlaps.
void weldTiling(std::vector<Poly> &polys)
{
  struct Hit
  {
    double t;
    Position p;
  };
  for (size_t pass = 0; pass < 2; ++pass)
    for (size_t i = 0; i < polys.size(); ++i)
    {
      std::vector<Poly> rebuilt;
      Poly out;
      for (size_t k = 0; k < polys[i].size(); ++k)
      {
        auto a = polys[i][k], b = polys[i][(k + 1) % polys[i].size()];
        double len = length(a, b);
        out.push_back(a);
        if (len < 1e-6)
          continue;
        std::vector<Hit> hits;
        for (size_t j = 0; j < polys.size(); ++j)
        {
          if (j == i)
            continue;
          for (auto &q : polys[j])
          {
            double t = ((q.x - a.x) * (b.x - a.x) + (q.z - a.z) * (b.z - a.z)) / (len * len);
            if (t <= 1e-4 / len || t >= 1 - 1e-4 / len)
              continue;
            Position on{a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t};
            if (length(q, on) > 1e-5)
              continue;
            if (pass == 0)
              q = on; // Snap the neighbour's vertex exactly onto this edge.
            else
              hits.push_back({t, q});
          }
        }
        std::sort(
            hits.begin(),
            hits.end(),
            [](const Hit &x, const Hit &y)
            {
              return x.t < y.t;
            });
        for (auto &h : hits)
          if (length(out.back(), h.p) > 1e-5 && length(h.p, b) > 1e-5)
            out.push_back(h.p);
      }
      if (pass == 1)
        polys[i] = std::move(out);
    }
}
// The rural and master-plan generators test convexity, duplicates and overlap
// with fixed absolute tolerances. Judge every offered patch with the same
// predicates so a rejected patch is omitted here with its reason rather than
// failing the whole territory downstream.
std::string tilingFault(const Poly &p, double minX, double minZ, double maxX, double maxZ, double crossTolerance, double vertexTolerance)
{
  if (p.size() < 3 || p.size() > 32)
    return "vertex count " + std::to_string(p.size());
  if (signedArea(p) <= 1e-4)
    return "degenerate area";
  for (size_t k = 0; k < p.size(); ++k)
  {
    auto a = p[k], b = p[(k + 1) % p.size()];
    if (!std::isfinite(a.x) || !std::isfinite(a.z) || a.x < minX || a.x > maxX || a.z < minZ || a.z > maxZ)
      return "vertex outside the domain";
    if (length(a, b) < vertexTolerance)
      return "degenerate edge";
    for (size_t j = k + 1; j < p.size(); ++j)
      if (length(a, p[j]) < vertexTolerance)
        return "duplicate vertex";
    for (auto q : p)
      if (cross(a, b, q) < -crossTolerance)
        return "not convex within tolerance";
  }
  return "";
}
bool tilesOverlap(const Poly &a, const Poly &b, double tolerance)
{
  for (auto *p : {&a, &b})
    for (size_t i = 0; i < p->size(); ++i)
    {
      bool separated = true;
      for (auto q : p == &a ? b : a)
        if (cross((*p)[i], (*p)[(i + 1) % p->size()], q) > tolerance)
        {
          separated = false;
          break;
        }
      if (separated)
        return false;
    }
  return true;
}
double biomeFertility(Biome b)
{
  // Ordinal soil-quality class of the map's explicit Whittaker biome. The map
  // has no soil stage; this is the classification it does publish, recorded as
  // such rather than presented as a soil survey.
  switch (b)
  {
  case Biome::Grassland:
    return .85;
  case Biome::TemperateForest:
  case Biome::SubtropicalForest:
    return .8;
  case Biome::TropicalSeasonalForest:
  case Biome::TemperateRainforest:
    return .7;
  case Biome::Mediterranean:
    return .65;
  case Biome::TropicalRainforest:
  case Biome::Savanna:
    return .6;
  case Biome::Steppe:
  case Biome::Wetland:
    return .5;
  case Biome::Shrubland:
  case Biome::MontaneForest:
    return .45;
  case Biome::Taiga:
  case Biome::Swamp:
    return .4;
  case Biome::AlpineMeadow:
    return .35;
  case Biome::Mangrove:
  case Biome::Shore:
    return .3;
  case Biome::Tundra:
    return .15;
  case Biome::ColdDesert:
  case Biome::Desert:
    return .1;
  case Biome::SandDesert:
  case Biome::SaltFlat:
    return .05;
  default:
    return 0; // Glacier, bare rock, ocean, lake.
  }
}
RuralLandCover coverOf(const MapNode &node)
{
  if (!node.land)
    return RuralLandCover::Water;
  if (node.type == CellType::Mountain || node.type == CellType::Cliff || node.type == CellType::Canyon || node.type == CellType::Rocks)
    return RuralLandCover::Rock;
  switch (biomeOf(node))
  {
  case Biome::Glacier:
  case Biome::BareRock:
    return RuralLandCover::Rock;
  case Biome::Taiga:
  case Biome::TemperateForest:
  case Biome::TemperateRainforest:
  case Biome::MontaneForest:
  case Biome::TropicalSeasonalForest:
  case Biome::TropicalRainforest:
  case Biome::SubtropicalForest:
  case Biome::Mangrove:
  case Biome::Swamp:
    return RuralLandCover::Forest;
  default:
    return RuralLandCover::Open;
  }
}
struct RiverCorridor
{
  Position a, b;
  double halfWidth = 1, level = 0;
};
// Bucketed lookup over the leaves and river corridors near one window.
struct LocalMap
{
  double minX = 0, minZ = 0, size = 1;
  uint32_t side = 1;
  std::vector<uint32_t> leaves;
  std::vector<Poly> polys;
  std::vector<std::vector<uint32_t>> buckets;
  std::vector<RiverCorridor> rivers;
  std::vector<std::vector<uint32_t>> riverBuckets;
  size_t bucket(double x, double z) const
  {
    int bx = std::clamp(int((x - minX) / size), 0, int(side) - 1), bz = std::clamp(int((z - minZ) / size), 0, int(side) - 1);
    return size_t(bz) * side + bx;
  }
  void build(const GeneratedWorld &world, double x0, double z0, double x1, double z1, const std::vector<uint32_t> &leafIds)
  {
    minX = x0;
    minZ = z0;
    side = 48;
    size = std::max(x1 - x0, z1 - z0) / side + 1e-9;
    buckets.assign(size_t(side) * side, {});
    riverBuckets.assign(size_t(side) * side, {});
    for (auto id : leafIds)
    {
      const auto &n = world.graph.node(id);
      auto poly = toPoly(n.area);
      double bx0 = INFINITY, bz0 = INFINITY, bx1 = -INFINITY, bz1 = -INFINITY;
      for (auto p : poly)
      {
        bx0 = std::min(bx0, p.x);
        bz0 = std::min(bz0, p.z);
        bx1 = std::max(bx1, p.x);
        bz1 = std::max(bz1, p.z);
      }
      if (bx1 < x0 || bx0 > x1 || bz1 < z0 || bz0 > z1)
        continue;
      uint32_t index = uint32_t(leaves.size());
      leaves.push_back(id);
      polys.push_back(std::move(poly));
      int cx0 = std::clamp(int((bx0 - minX) / size), 0, int(side) - 1), cx1 = std::clamp(int((bx1 - minX) / size), 0, int(side) - 1);
      int cz0 = std::clamp(int((bz0 - minZ) / size), 0, int(side) - 1), cz1 = std::clamp(int((bz1 - minZ) / size), 0, int(side) - 1);
      for (int z = cz0; z <= cz1; ++z)
        for (int x = cx0; x <= cx1; ++x)
          buckets[size_t(z) * side + x].push_back(index);
    }
    for (const auto &s : world.rivers.segments)
    {
      RiverCorridor c;
      c.a = {s.from.x, 0, s.from.y};
      c.b = {s.to.x, 0, s.to.y};
      c.halfWidth = .5 * std::clamp(std::sqrt(std::max(0., s.dischargeCumecs)), 2., 20.);
      c.level = s.node < world.rivers.nodes.size() ? world.rivers.nodes[s.node].filledElevationMetres : world.surface.sample(s.from).elevationMetres;
      double bx0 = std::min(c.a.x, c.b.x) - c.halfWidth, bx1 = std::max(c.a.x, c.b.x) + c.halfWidth;
      double bz0 = std::min(c.a.z, c.b.z) - c.halfWidth, bz1 = std::max(c.a.z, c.b.z) + c.halfWidth;
      if (bx1 < x0 || bx0 > x1 || bz1 < z0 || bz0 > z1)
        continue;
      uint32_t index = uint32_t(rivers.size());
      rivers.push_back(c);
      int cx0 = std::clamp(int((bx0 - minX) / size), 0, int(side) - 1), cx1 = std::clamp(int((bx1 - minX) / size), 0, int(side) - 1);
      int cz0 = std::clamp(int((bz0 - minZ) / size), 0, int(side) - 1), cz1 = std::clamp(int((bz1 - minZ) / size), 0, int(side) - 1);
      for (int z = cz0; z <= cz1; ++z)
        for (int x = cx0; x <= cx1; ++x)
          riverBuckets[size_t(z) * side + x].push_back(index);
    }
  }
  // Containing leaf, or the nearest site when a boundary rounding gap swallows the point.
  uint32_t leafAt(const GeneratedWorld &world, double x, double z) const
  {
    Position p{x, 0, z};
    for (auto i : buckets[bucket(x, z)])
      if (inside(polys[i], p))
        return leaves[i];
    uint32_t best = kInvalidNode;
    double nearest = INFINITY;
    for (size_t i = 0; i < leaves.size(); ++i)
    {
      auto s = world.graph.node(leaves[i]).site;
      double d = std::hypot(s.x - x, s.y - z);
      if (d < nearest)
      {
        nearest = d;
        best = leaves[i];
      }
    }
    return best;
  }
  // Distance to the nearest explicit water: ocean/lake cell polygon or river
  // corridor. Searches a bounded ring of buckets; beyond it returns kNoWater.
  double waterDistance(const GeneratedWorld &world, Position p, double reach, double *level = nullptr, bool *ocean = nullptr) const
  {
    double best = kNoWater;
    int ring = int(std::ceil(reach / size));
    int bx = int((p.x - minX) / size), bz = int((p.z - minZ) / size);
    std::set<uint32_t> seen, seenRivers;
    for (int z = bz - ring; z <= bz + ring; ++z)
      for (int x = bx - ring; x <= bx + ring; ++x)
      {
        if (x < 0 || z < 0 || x >= int(side) || z >= int(side))
          continue;
        for (auto i : buckets[size_t(z) * side + x])
        {
          const auto &n = world.graph.node(leaves[i]);
          if (n.land || !seen.insert(i).second)
            continue;
          double d = polygonDistance(p, polys[i]);
          if (d < best)
          {
            best = d;
            if (level)
              *level = n.type == CellType::Ocean ? world.graph.brief().seaLevelMetres : world.surface.sample(n.site).elevationMetres;
            if (ocean)
              *ocean = n.type == CellType::Ocean;
          }
        }
        for (auto i : riverBuckets[size_t(z) * side + x])
        {
          if (!seenRivers.insert(i).second)
            continue;
          const auto &c = rivers[i];
          double d = std::max(0., segmentDistance(p, c.a, c.b) - c.halfWidth);
          if (d < best)
          {
            best = d;
            if (level)
              *level = c.level;
            if (ocean)
              *ocean = false;
          }
        }
      }
    return best;
  }
};
} // namespace
bool selectWorldSettlementSite(const GeneratedWorld &world, const WorldSiteSelectionSettings &s, WorldSettlementSite &out, std::string *error)
{
  auto fail = [&](const std::string &m)
  {
    if (error)
      *error = m;
    return false;
  };
  const double extent = world.graph.brief().extentMetres;
  if (!world.graph.nodeCount() || world.surface.empty() || !(extent > 0) || !std::isfinite(extent))
    return fail("world lacks a generated graph or elevation surface");
  if (world.climate.cells.size() != world.graph.nodeCount())
    return fail("world lacks the climate stage; moisture cannot be assumed");
  if (world.rivers.nodes.size() != world.graph.nodeCount())
    return fail("world lacks the river stage; water cannot be assumed");
  for (double v :
       {s.windowMetres,
        s.suitabilitySpacingMetres,
        s.planningSpacingMetres,
        s.hinterlandSpacingMetres,
        s.maximumSiteGradient,
        s.minimumWaterClearanceMetres,
        s.waterReachMetres,
        s.riverBankMetres,
        s.waterSupplyReachMetres,
        s.cropAreaPerResident,
        s.exitOffsetMetres})
    if (!std::isfinite(v) || v <= 0)
      return fail("invalid site selection settings");
  if (!s.producer || !s.revision || s.suitabilitySpacingMetres < 1 || s.suitabilitySpacingMetres > 20 || !s.maximumRuralSources || s.maximumRuralSources > 64 || !s.maximumAttempts)
    return fail("invalid site selection identity or budget");
  const double h = s.suitabilitySpacingMetres;
  const double window = std::round(s.windowMetres / h) * h;
  const double axis = window / h;
  if (axis < 8 || axis * axis > 65536)
    return fail("settlement window must fit the suitability grid budget");
  if (window > extent)
    return fail("settlement window is larger than the world");
  const auto leaves = leafNodes(world.graph);
  // Whole-world water index for candidate scoring.
  LocalMap globalMap;
  globalMap.build(world, 0, 0, extent, extent, leaves);
  auto terrainOf = [&](double x, double z)
  {
    return world.surface.sample({x, z}).elevationMetres;
  };
  const double step = std::max(10., world.surface.spacingMetres());
  auto gradientAt = [&](double x, double z)
  {
    double gx = (terrainOf(x + step, z) - terrainOf(x - step, z)) / (2 * step), gz = (terrainOf(x, z + step) - terrainOf(x, z - step)) / (2 * step);
    return std::hypot(gx, gz);
  };
  WorldSettlementSite site;
  site.id = featureSeed(world.graph.brief().seed, 1, s.producer, 9) | 1;
  site.revision = s.revision;
  for (auto id : leaves)
  {
    const auto &n = world.graph.node(id);
    if (!n.land || (s.preferredCell != kInvalidNode && s.preferredCell != id))
      continue;
    WorldSiteCandidate c;
    c.cell = id;
    c.region = n.region;
    c.centre = {n.site.x, 0, n.site.y};
    c.centre.y = terrainOf(c.centre.x, c.centre.z);
    c.elevation = c.centre.y;
    c.gradient = gradientAt(c.centre.x, c.centre.z);
    c.fertility = biomeFertility(biomeOf(n));
    c.moisture = world.climate.cells[id].moisture;
    c.biome = n.biome;
    c.waterDistance = globalMap.waterDistance(world, c.centre, s.waterReachMetres);
    // Hinterland: mean gradient at the neighbouring cells' sites and the share
    // of neighbours with open cover, both read from the map's own products.
    double ringGradient = 0, ringOpen = 0;
    size_t ring = 0;
    for (auto nb : n.neighbours)
    {
      const auto &m = world.graph.node(nb);
      ringGradient += m.land ? gradientAt(m.site.x, m.site.y) : s.maximumSiteGradient;
      ringOpen += m.land && coverOf(m) == RuralLandCover::Open;
      ++ring;
    }
    if (ring)
    {
      ringGradient /= ring;
      ringOpen /= ring;
    }
    c.hinterlandGradient = ringGradient;
    c.openNeighbours = ringOpen;
    c.accepted = true;
    if (c.gradient > s.maximumSiteGradient)
    {
      c.accepted = false;
      c.rejection = "site gradient " + std::to_string(c.gradient) + " exceeds " + std::to_string(s.maximumSiteGradient);
    }
    else if (c.waterDistance < s.minimumWaterClearanceMetres)
    {
      c.accepted = false;
      c.rejection = "site centre within " + std::to_string(c.waterDistance) + " m of explicit water";
    }
    else if (c.fertility < s.suitability.minimumSoil)
    {
      c.accepted = false;
      c.rejection = std::string("infertile biome ") + biomeName(biomeOf(n));
    }
    else if (c.moisture > s.suitability.maximumMoisture)
    {
      c.accepted = false;
      c.rejection = "saturated ground";
    }
    c.score = .3 * c.fertility + .2 * std::max(0., 1 - c.gradient / s.maximumSiteGradient) + .2 * std::max(0., 1 - c.hinterlandGradient / (2 * s.maximumSiteGradient)) + .15 * c.openNeighbours +
              .15 * std::exp(-c.waterDistance / s.waterReachMetres);
    site.candidates.push_back(std::move(c));
  }
  if (site.candidates.empty())
    return fail(s.preferredCell != kInvalidNode ? "preferred cell is not a land leaf" : "world has no land leaf cells");
  std::stable_sort(
      site.candidates.begin(),
      site.candidates.end(),
      [](const WorldSiteCandidate &a, const WorldSiteCandidate &b)
      {
        if (a.accepted != b.accepted)
          return a.accepted;
        return a.score != b.score ? a.score > b.score : a.cell < b.cell;
      });
  if (site.candidates.size() > s.maximumCandidates)
    site.candidates.resize(s.maximumCandidates);
  auto compose = [&](WorldSiteCandidate &candidate, WorldSettlementSite &r, std::string &why)
  {
    const auto &siteNode = world.graph.node(candidate.cell);
    r.cell = candidate.cell;
    r.region = candidate.region;
    r.centre = candidate.centre;
    if (candidate.region < world.regions.regions.size())
      r.regionName = world.regions.regions[candidate.region].name;
    // Parent patch region: the urban leaf and its neighbouring map cells (two
    // rings when they fit the window). Children work only inside these patches;
    // the analysis grid is their bounding box, cropped to the grid budget.
    std::set<uint32_t> patchSet{candidate.cell};
    for (auto nb : siteNode.neighbours)
      patchSet.insert(nb);
    auto patchBounds = [&](const std::set<uint32_t> &set, double &x0, double &z0, double &x1, double &z1)
    {
      x0 = z0 = INFINITY;
      x1 = z1 = -INFINITY;
      for (auto id : set)
        for (auto p : world.graph.node(id).area)
        {
          x0 = std::min(x0, p.x);
          z0 = std::min(z0, p.y);
          x1 = std::max(x1, p.x);
          z1 = std::max(z1, p.y);
        }
    };
    {
      std::set<uint32_t> second = patchSet;
      for (auto id : patchSet)
        for (auto nb : world.graph.node(id).neighbours)
          second.insert(nb);
      double x0, z0, x1, z1;
      patchBounds(second, x0, z0, x1, z1);
      if (x1 - x0 <= window && z1 - z0 <= window)
        patchSet = second;
    }
    double px0, pz0, px1, pz1;
    patchBounds(patchSet, px0, pz0, px1, pz1);
    double sideX = std::min(window, std::ceil((px1 - px0) / h) * h), sideZ = std::min(window, std::ceil((pz1 - pz0) / h) * h);
    sideX = std::max(sideX, 8 * h);
    sideZ = std::max(sideZ, 8 * h);
    // World-frame domain corner; every brief coordinate is local to it so the
    // settlement generators' absolute geometric tolerances stay meaningful.
    double originX = std::clamp(candidate.centre.x - sideX * .5, 0., extent - sideX), originZ = std::clamp(candidate.centre.z - sideZ * .5, 0., extent - sideZ);
    if (sideX >= px1 - px0)
      originX = std::clamp(px0, 0., extent - sideX);
    if (sideZ >= pz1 - pz0)
      originZ = std::clamp(pz0, 0., extent - sideZ);
    originX = std::round(originX / h) * h;
    originZ = std::round(originZ / h) * h;
    if (originX < 0 || originZ < 0 || originX + sideX > extent || originZ + sideZ > extent)
    {
      why = "hinterland patches do not fit inside the world";
      return false;
    }
    r.origin = {originX, 0, originZ};
    r.bounds = s.suitability;
    r.bounds.spacingMetres = h;
    r.bounds.minX = 0;
    r.bounds.minZ = 0;
    r.bounds.maxX = sideX;
    r.bounds.maxZ = sideZ;
    auto toLocal = [&](Position p)
    {
      return Position{p.x - originX, p.y, p.z - originZ};
    };
    auto toWorld = [&](Position p)
    {
      return Position{p.x + originX, p.y, p.z + originZ};
    };
    r.localCentre = toLocal(candidate.centre);
    const double reach = std::max({s.suitability.floodReachMetres, s.waterSupplyReachMetres, s.waterReachMetres});
    LocalMap local;
    local.build(world, originX - reach, originZ - reach, originX + sideX + reach, originZ + sideZ + reach, leaves);
    auto localClip = [&](const std::vector<Point2> &area)
    {
      Poly poly = clipToRect(toPoly(area), originX, originZ, originX + sideX, originZ + sideZ);
      for (auto &p : poly)
        p = toLocal(p);
      return poly;
    };
    // Refine the map's own hierarchy inside the patch region: the urban cell at
    // planning spacing, its hinterland at field spacing. Children inherit the
    // parent's land/biome classification; elevation comes from the surface.
    MapGraph refined = world.graph;
    std::vector<uint32_t> windowLeaves(patchSet.begin(), patchSet.end());
    std::sort(windowLeaves.begin(), windowLeaves.end());
    for (auto id : windowLeaves)
    {
      std::string refineError;
      if (!refineNode(id, id == candidate.cell ? s.planningSpacingMetres : s.hinterlandSpacingMetres, world.graph.brief().seed, refined, &refineError))
      {
        why = "map refinement failed: " + refineError;
        return false;
      }
      r.refinedLeaves.push_back(id);
    }
    struct Piece
    {
      uint64_t id;
      uint32_t leaf;
      Poly poly;
      uint32_t node = kInvalidNode;
      int depth = 0;
      uint32_t blocked = 0;
    };
    std::vector<Piece> planning, hinterland, pending;
    for (auto id : windowLeaves)
    {
      const auto &leaf = refined.node(id);
      std::vector<uint32_t> parts = leaf.children;
      if (parts.empty())
        parts.push_back(id);
      for (size_t k = 0; k < parts.size(); ++k)
      {
        Poly poly = localClip(refined.node(parts[k]).area);
        if (poly.empty())
          continue;
        Piece piece{uint64_t(id + 1) * 1024 + (leaf.children.empty() ? 0 : k + 1), id, std::move(poly), parts[k], 0, 0};
        (id == candidate.cell ? planning : pending).push_back(std::move(piece));
      }
    }
    if (planning.empty())
    {
      why = "urban cell has no planning cells inside the window";
      return false;
    }
    if (planning.size() > 512)
    {
      why = "planning spacing produces more than 512 cells";
      return false;
    }
    {
      std::vector<Poly> polys;
      for (auto &p : planning)
        polys.push_back(p.poly);
      weldTiling(polys);
      for (size_t i = 0; i < polys.size(); ++i)
        planning[i].poly = polys[i];
    }
    {
      std::vector<Piece> kept;
      for (auto &p : planning)
      {
        auto fault = tilingFault(p.poly, 0, 0, sideX, sideZ, 1e-7, 1e-5);
        for (auto &k : kept)
          if (fault.empty() && tilesOverlap(p.poly, k.poly, 1e-7))
            fault = "overlaps planning cell " + std::to_string(k.id);
        if (fault.empty())
          kept.push_back(p);
        else
          r.rejections.push_back("planning cell " + std::to_string(p.id) + " omitted: " + fault);
      }
      planning = std::move(kept);
      if (planning.empty())
      {
        why = "no planning cell survives the master-plan tolerances";
        return false;
      }
    }
    for (auto &p : planning)
      r.planningArea += signedArea(p.poly);
    // Immutable captures for the brief's function-backed fields (local frame).
    auto surface = std::make_shared<const MapSurfaceField>(world.surface);
    r.surface = surface;
    auto terrain = [surface, originX, originZ](double x, double z)
    {
      return surface->sample({x + originX, z + originZ}).elevationMetres;
    };
    auto localMap = std::make_shared<const LocalMap>(local);
    auto worldRef = &world;
    r.water = [localMap, worldRef, originX, originZ, bank = s.riverBankMetres](double x, double z)
    {
      x += originX;
      z += originZ;
      auto leaf = localMap->leafAt(*worldRef, x, z);
      if (leaf == kInvalidNode || !worldRef->graph.node(leaf).land)
        return true;
      for (auto i : localMap->riverBuckets[localMap->bucket(x, z)])
      {
        const auto &c = localMap->rivers[i];
        if (segmentDistance({x, 0, z}, c.a, c.b) <= c.halfWidth + bank)
          return true;
      }
      return false;
    };
    r.waterSupply = [localMap, worldRef, originX, originZ, reach = s.waterSupplyReachMetres](double x, double z)
    {
      return localMap->waterDistance(*worldRef, {x + originX, 0, z + originZ}, reach) <= reach;
    };
    auto fertilityAt = [&](double x, double z)
    {
      auto leaf = local.leafAt(world, x + originX, z + originZ);
      return leaf == kInvalidNode ? 0. : biomeFertility(biomeOf(world.graph.node(leaf)));
    };
    auto moistureAt = [&](double x, double z)
    {
      auto leaf = local.leafAt(world, x + originX, z + originZ);
      return leaf == kInvalidNode ? 1. : std::clamp(world.climate.cells[leaf].moisture, 0., 1.);
    };
    WorldSplineLandforms forms;
    WorldPhysicalTerrain physical;
    WorldRiverChannels channels;
    std::string terrainError;
    if (!forms.build({}, {}, &terrainError) || !physical.build(forms, terrain, {}, {}, {}, &terrainError))
    {
      why = "physical terrain wrapper failed: " + terrainError;
      return false;
    }
    auto snap = [&](Position p)
    {
      return Position{(std::floor(p.x / h) + .5) * h, 0, (std::floor(p.z / h) + .5) * h};
    };
    Position seed = snap(r.localCentre);
    WorldSuitability suitability;
    std::string suitabilityError;
    if (!suitability.build(physical, channels, fertilityAt, moistureAt, {seed}, r.bounds, s.revision, &suitabilityError, r.water))
    {
      why = "suitability over actual terrain: " + suitabilityError;
      return false;
    }
    const size_t seedCell = suitability.cellAt(seed.x, seed.z);
    if (seedCell == SIZE_MAX || !suitability.cells()[seedCell].walkable)
    {
      why = "site centre is not traversable on the actual surface";
      return false;
    }
    const uint32_t component = suitability.cells()[seedCell].component;
    r.brief.suitability = suitability;
    // Road exits: one per land neighbour of the urban cell, placed just outside
    // the shared boundary on traversable ground connected to the centre.
    Poly urban = toPoly(siteNode.area);
    for (auto &p : urban)
      p = toLocal(p);
    struct Exit
    {
      uint32_t neighbour;
      double fertility;
      Position point;
    };
    std::vector<Exit> exits;
    for (auto nb : siteNode.neighbours)
    {
      const auto &n = world.graph.node(nb);
      if (!n.land || coverOf(n) != RuralLandCover::Open)
        continue;
      Poly other = toPoly(n.area);
      for (auto &p : other)
        p = toLocal(p);
      Position mid;
      int shared = 0;
      for (auto a : urban)
        for (auto b : other)
          if (length(a, b) < 1e-6)
          {
            mid.x += a.x;
            mid.z += a.z;
            ++shared;
          }
      Position localSite = toLocal({siteNode.site.x, 0, siteNode.site.y}), localOther = toLocal({n.site.x, 0, n.site.y});
      if (shared < 2)
        mid = {(localSite.x + localOther.x) * .5, 0, (localSite.z + localOther.z) * .5};
      else
        mid = {mid.x / shared, 0, mid.z / shared};
      double dx = localOther.x - localSite.x, dz = localOther.z - localSite.z, len = std::hypot(dx, dz);
      if (len < 1e-6)
        continue;
      Position target{mid.x + dx / len * s.exitOffsetMetres, 0, mid.z + dz / len * s.exitOffsetMetres};
      Position chosen;
      bool found = false;
      for (int ring = 0; ring <= 4 && !found; ++ring)
        for (int oz = -ring; oz <= ring && !found; ++oz)
          for (int ox = -ring; ox <= ring && !found; ++ox)
          {
            if (std::max(std::abs(ox), std::abs(oz)) != ring)
              continue;
            Position p = snap({target.x + ox * h, 0, target.z + oz * h});
            size_t cell = suitability.cellAt(p.x, p.z);
            if (cell == SIZE_MAX || !suitability.cells()[cell].walkable || suitability.cells()[cell].component != component || inside(urban, p))
              continue;
            chosen = p;
            found = true;
          }
      if (!found)
      {
        r.rejections.push_back("no traversable road exit toward neighbour cell " + std::to_string(nb));
        continue;
      }
      exits.push_back({nb, biomeFertility(biomeOf(n)), chosen});
    }
    std::stable_sort(
        exits.begin(),
        exits.end(),
        [](const Exit &a, const Exit &b)
        {
          return a.fertility != b.fertility ? a.fertility > b.fertility : a.neighbour < b.neighbour;
        });
    if (exits.size() > s.maximumRuralSources)
      exits.resize(s.maximumRuralSources);
    if (exits.empty())
    {
      why = "no traversable road exit leaves the urban cell";
      return false;
    }
    // Population-led demand before any street exists.
    std::string demandError;
    r.populationBrief = s.population;
    if (!generateSettlementPopulation(s.population, r.households, &demandError))
    {
      why = "population brief: " + demandError;
      return false;
    }
    double maxPlanningArea = 0;
    for (auto &p : planning)
      maxPlanningArea = std::max(maxPlanningArea, signedArea(p.poly));
    auto demandSettings = s.demand;
    demandSettings.planningCellAreaAllowance = maxPlanningArea;
    if (!deriveCityPopulationDemand(r.households, demandSettings, r.demand, &demandError))
    {
      why = "population demand: " + demandError;
      return false;
    }
    // Planning cells from the refined urban cell with measured attributes.
    CityTerritoryInput &input = r.brief.input;
    input.producer = s.producer;
    input.revision = s.revision;
    double minElevation = INFINITY, maxElevation = -INFINITY;
    std::vector<double> elevations;
    for (auto &p : planning)
    {
      auto c = centroid(p.poly);
      double e = terrain(c.x, c.z);
      elevations.push_back(e);
      minElevation = std::min(minElevation, e);
      maxElevation = std::max(maxElevation, e);
    }
    for (size_t i = 0; i < planning.size(); ++i)
    {
      auto &p = planning[i];
      CityPlanCell cell;
      cell.id = p.id;
      cell.boundary = p.poly;
      auto c = centroid(p.poly);
      auto cw = toWorld(c);
      double slope = gradientAt(cw.x, cw.z);
      for (auto v : p.poly)
        slope = std::max(slope, gradientAt(cw.x + (v.x - c.x) * .8, cw.z + (v.z - c.z) * .8));
      cell.slope = std::clamp(slope, 0., 1.);
      double level = 0;
      bool ocean = false;
      double d = local.waterDistance(world, cw, s.suitability.floodReachMetres, &level, &ocean);
      if (d <= s.suitability.waterSetbackMetres)
        cell.floodRisk = 1;
      else if (d <= s.suitability.floodReachMetres && elevations[i] <= level + s.suitability.floodRiseMetres)
        cell.floodRisk = std::clamp(1 - (d - s.suitability.waterSetbackMetres) / (s.suitability.floodReachMetres - s.suitability.waterSetbackMetres), 0., 1.);
      cell.fertility = biomeFertility(biomeOf(siteNode));
      cell.defense = maxElevation > minElevation + 1e-6 ? (elevations[i] - minElevation) / (maxElevation - minElevation) : .5;
      cell.buildable = !r.water(c.x, c.z);
      cell.navigableShore = siteNode.coastal && ocean && d <= s.planningSpacingMetres;
      cell.secure = true; // The map publishes no threat model; nothing upstream marks insecurity.
      cell.outsideOldWall = true;
      input.cells.push_back(std::move(cell));
      r.urbanGround.push_back(p.poly);
    }
    input.districts = r.demand.districts;
    double cropBudget = std::max(1., double(r.demand.population)) * s.cropAreaPerResident * 1.5 / exits.size();
    for (size_t i = 0; i < exits.size(); ++i)
    {
      input.ruralSources.push_back({uint64_t(exits[i].neighbour) + 1, exits[i].point, cropBudget});
      // Gate: a usable planning cell one planning step from the centre toward
      // the exit, so the urban polygon stays compact; the stretch from the
      // gate to the exit is a rural approach road, not urban land.
      uint64_t gate = 0;
      double nearest = INFINITY;
      double dx = exits[i].point.x - r.localCentre.x, dz = exits[i].point.z - r.localCentre.z, len = std::hypot(dx, dz);
      Position target{r.localCentre.x + dx / std::max(1e-9, len) * s.planningSpacingMetres, 0, r.localCentre.z + dz / std::max(1e-9, len) * s.planningSpacingMetres};
      for (auto &cell : input.cells)
      {
        if (!cell.buildable || cell.slope > s.territory.planning.maximumSlope || cell.floodRisk > s.territory.planning.maximumFloodRisk)
          continue;
        double d = length(centroid(cell.boundary), target);
        if (d < nearest)
        {
          nearest = d;
          gate = cell.id;
        }
      }
      if (gate && std::find(input.gateCells.begin(), input.gateCells.end(), gate) == input.gateCells.end())
        input.gateCells.push_back(gate);
    }
    if (input.gateCells.empty())
    {
      why = "no usable planning cell can serve as a gate";
      return false;
    }
    // Hinterland regions: the map's refined cells with explicit cover labels.
    // A patch whose owned analysis cells include unsuitable ground, or whose
    // box exceeds the field generator's grid, is refined again through the map
    // hierarchy and its children are judged on their own. Failing leaves at the
    // finest depth are still offered, so the rural generator records them.
    auto blockedCells = [&](const Poly &poly)
    {
      double x0 = INFINITY, z0 = INFINITY, x1 = -INFINITY, z1 = -INFINITY;
      for (auto p : poly)
      {
        x0 = std::min(x0, p.x);
        z0 = std::min(z0, p.z);
        x1 = std::max(x1, p.x);
        z1 = std::max(z1, p.z);
      }
      const auto &w = r.brief.suitability;
      uint32_t blocked = 0;
      for (const auto &cell : w.cells())
      {
        auto c = cell.center;
        if (c.x < x0 || c.x > x1 || c.z < z0 || c.z > z1)
          continue;
        bool owned = true;
        for (size_t k = 0; k < poly.size() && owned; ++k)
          owned = cross(poly[k], poly[(k + 1) % poly.size()], c) >= -1e-8;
        blocked += owned && cell.reasons != 0;
      }
      return blocked;
    };
    const double fieldGridSide = 240 * s.territory.fields.entranceWidth * .5;
    for (size_t index = 0; index < pending.size(); ++index)
    {
      Piece piece = pending[index];
      double bx0 = INFINITY, bz0 = INFINITY, bx1 = -INFINITY, bz1 = -INFINITY;
      for (auto p : piece.poly)
      {
        bx0 = std::min(bx0, p.x);
        bz0 = std::min(bz0, p.z);
        bx1 = std::max(bx1, p.x);
        bz1 = std::max(bz1, p.z);
      }
      piece.blocked = blockedCells(piece.poly);
      const bool tooWide = std::max(bx1 - bx0, bz1 - bz0) > fieldGridSide;
      const double childSpacing = s.hinterlandSpacingMetres / std::pow(2., piece.depth + 1);
      const bool refine = piece.depth < 2 && piece.node != kInvalidNode && !refined.node(piece.node).refined() && (tooWide || piece.blocked > 0) &&
                          signedArea(toPoly(refined.node(piece.node).area)) >= childSpacing * childSpacing * 3;
      if (refine)
      {
        std::string refineError;
        if (!refineNode(piece.node, childSpacing, world.graph.brief().seed, refined, &refineError))
        {
          why = "map refinement failed: " + refineError;
          return false;
        }
        const auto &children = refined.node(piece.node).children;
        if (!children.empty())
        {
          for (size_t k = 0; k < children.size(); ++k)
          {
            Poly poly = localClip(refined.node(children[k]).area);
            if (poly.empty())
              continue;
            pending.push_back({piece.id * 1024 + k + 1, piece.leaf, std::move(poly), children[k], piece.depth + 1, 0});
          }
          continue;
        }
      }
      hinterland.push_back(std::move(piece));
    }
    if (hinterland.size() > 512)
    {
      // Keep eligible patches first, then the nearest; the omission is recorded.
      std::stable_sort(
          hinterland.begin(),
          hinterland.end(),
          [&](const Piece &a, const Piece &b)
          {
            if ((a.blocked > 0) != (b.blocked > 0))
              return a.blocked == 0;
            return length(centroid(a.poly), r.localCentre) < length(centroid(b.poly), r.localCentre);
          });
      r.rejections.push_back(std::to_string(hinterland.size() - 512) + " hinterland regions beyond the 512-region budget were omitted");
      hinterland.resize(512);
    }
    std::stable_sort(
        hinterland.begin(),
        hinterland.end(),
        [](const Piece &a, const Piece &b)
        {
          return a.id < b.id;
        });
    {
      std::vector<Poly> polys;
      for (auto &p : hinterland)
        polys.push_back(p.poly);
      weldTiling(polys);
      for (size_t i = 0; i < polys.size(); ++i)
        hinterland[i].poly = polys[i];
    }
    {
      std::vector<Piece> kept;
      for (auto &p : hinterland)
      {
        auto fault = tilingFault(p.poly, 0, 0, sideX, sideZ, 1e-8, 1e-5);
        if (fault == "vertex count " + std::to_string(p.poly.size()) && p.poly.size() > 32 && p.poly.size() <= 256)
          fault.clear();
        for (auto &k : kept)
          if (fault.empty() && tilesOverlap(p.poly, k.poly, 1e-8))
            fault = "overlaps region " + std::to_string(k.id);
        if (fault.empty())
          kept.push_back(p);
        else
          r.rejections.push_back("hinterland region " + std::to_string(p.id) + " omitted: " + fault);
      }
      hinterland = std::move(kept);
    }
    double meanArea = 0, largest = 0;
    for (auto &p : hinterland)
    {
      RuralRegion region;
      region.id = p.id;
      region.boundary = p.poly;
      region.cover = coverOf(world.graph.node(p.leaf));
      double a = signedArea(p.poly);
      r.hinterlandArea += a;
      if (region.cover == RuralLandCover::Open)
        r.openArea += a;
      meanArea += a;
      largest = std::max(largest, a);
      r.regions.push_back(std::move(region));
    }
    meanArea /= std::max<size_t>(1, hinterland.size());
    auto &territory = r.brief.settings;
    territory = s.territory;
    territory.planning.seed = world.graph.brief().seed;
    territory.planning.revision = s.revision;
    territory.rural.seed = world.graph.brief().seed;
    territory.rural.regions = r.regions;
    territory.rural.minimumWidth = std::clamp(territory.rural.minimumWidth, 2 * h, 64 * h);
    territory.rural.patchSpacing = std::min(territory.rural.patchSpacing, 32 * h);
    territory.rural.trackWidth = std::min(territory.rural.trackWidth, .8 * h);
    territory.fields.trackWidth = std::min(territory.fields.trackWidth, .8 * h);
    territory.rural.minimumArea = std::max(4 * h * h, std::min(territory.rural.minimumArea, .2 * meanArea));
    territory.rural.maximumArea = std::min(1e8, std::max(territory.rural.maximumArea, 1.25 * largest));
    territory.rural.maximumAccessCost = std::max(territory.rural.maximumAccessCost, 1.5 * std::max(sideX, sideZ));
    territory.rural.maximumPatches = std::clamp<uint32_t>(territory.rural.maximumPatches, 1, 64);
    r.brief.terrain = terrain;
    return true;
  };
  for (auto &candidate : site.candidates)
  {
    if (!candidate.accepted)
      continue;
    if (site.attempts >= s.maximumAttempts)
      break;
    ++site.attempts;
    WorldSettlementSite attempt = site;
    std::string why;
    if (compose(candidate, attempt, why))
    {
      attempt.candidates = site.candidates;
      out = std::move(attempt);
      if (error)
        error->clear();
      return true;
    }
    candidate.accepted = false;
    candidate.rejection = why;
  }
  std::string summary = "no candidate site composed within " + std::to_string(site.attempts) + " attempts";
  for (auto &c : site.candidates)
    if (!c.rejection.empty())
    {
      summary += "; cell " + std::to_string(c.cell) + ": " + c.rejection;
      break;
    }
  return fail(summary);
}
GenerationTask worldSettlementSiteGenerator(uint64_t d)
{
  return nativeGenerator<WorldSiteGenerationBrief, WorldSettlementSite>(
      d,
      "world.settlement-site",
      "world.site-brief",
      "world.settlement-site",
      GenerationScope::Neighbourhood,
      [](const WorldSiteGenerationBrief &b, WorldSettlementSite &o, std::string *e)
      {
        if (!b.world)
        {
          if (e)
            *e = "site brief lacks a generated world";
          return false;
        }
        return selectWorldSettlementSite(*b.world, b.settings, o, e);
      });
}
GenerationTask settlementTerritoryBriefGenerator(uint64_t d)
{
  return nativeGenerator<WorldSettlementSite, TerritoryGenerationBrief>(
      d,
      "settlement.territory-brief",
      "world.settlement-site",
      "city.territory-brief",
      GenerationScope::Neighbourhood,
      [](const WorldSettlementSite &site, TerritoryGenerationBrief &o, std::string *e)
      {
        if (!site.brief.terrain || !site.brief.suitability.fingerprint())
        {
          if (e)
            *e = "settlement site carries no composed territory brief";
          return false;
        }
        o = site.brief;
        return true;
      });
}
} // namespace procedural::world::rewrite
