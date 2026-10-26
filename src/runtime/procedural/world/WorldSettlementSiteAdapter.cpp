#include "WorldSettlementSiteAdapter.hpp"
#include "MapClimate.hpp"
#include "MapHierarchy.hpp"
#include "MapPartition.hpp"
#include "WorldNativeGenerator.hpp"
#include "WorldRoadNetwork.hpp"
#include "generator/PatchTiling.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

namespace procedural::world::rewrite {
namespace {
using namespace ::procedural::world::generator;
using Poly = std::vector<Position>;
constexpr double kNoWater = 1e9;

Poly toPoly(const std::vector<Point2> &area)
{
  Poly p;
  p.reserve(area.size());
  for (auto q : area)
  {
    p.push_back({q.x, 0, q.y});
  }
  if (signedArea(p) < 0)
  {
    std::reverse(p.begin(), p.end());
  }
  return p;
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
  {
    return RuralLandCover::Water;
  }
  if (node.type == CellType::Mountain || node.type == CellType::Cliff || node.type == CellType::Canyon || node.type == CellType::Rocks)
  {
    return RuralLandCover::Rock;
  }
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
      {
        continue;
      }
      uint32_t index = uint32_t(leaves.size());
      leaves.push_back(id);
      polys.push_back(std::move(poly));
      int cx0 = std::clamp(int((bx0 - minX) / size), 0, int(side) - 1), cx1 = std::clamp(int((bx1 - minX) / size), 0, int(side) - 1);
      int cz0 = std::clamp(int((bz0 - minZ) / size), 0, int(side) - 1), cz1 = std::clamp(int((bz1 - minZ) / size), 0, int(side) - 1);
      for (int z = cz0; z <= cz1; ++z)
      {
        for (int x = cx0; x <= cx1; ++x)
        {
          buckets[size_t(z) * side + x].push_back(index);
        }
      }
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
      {
        continue;
      }
      uint32_t index = uint32_t(rivers.size());
      rivers.push_back(c);
      int cx0 = std::clamp(int((bx0 - minX) / size), 0, int(side) - 1), cx1 = std::clamp(int((bx1 - minX) / size), 0, int(side) - 1);
      int cz0 = std::clamp(int((bz0 - minZ) / size), 0, int(side) - 1), cz1 = std::clamp(int((bz1 - minZ) / size), 0, int(side) - 1);
      for (int z = cz0; z <= cz1; ++z)
      {
        for (int x = cx0; x <= cx1; ++x)
        {
          riverBuckets[size_t(z) * side + x].push_back(index);
        }
      }
    }
  }

  // Containing leaf, or the nearest site when a boundary rounding gap swallows the point.
  uint32_t leafAt(const GeneratedWorld &world, double x, double z) const
  {
    Position p{x, 0, z};
    for (auto i : buckets[bucket(x, z)])
    {
      if (insideEvenOdd(polys[i], p))
      {
        return leaves[i];
      }
    }
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
    {
      for (int x = bx - ring; x <= bx + ring; ++x)
      {
        if (x < 0 || z < 0 || x >= int(side) || z >= int(side))
        {
          continue;
        }
        for (auto i : buckets[size_t(z) * side + x])
        {
          const auto &n = world.graph.node(leaves[i]);
          if (n.land || !seen.insert(i).second)
          {
            continue;
          }
          double d = polygonDistance(p, polys[i]);
          if (d < best)
          {
            best = d;
            if (level)
            {
              *level = n.type == CellType::Ocean ? world.graph.brief().seaLevelMetres : world.surface.sample(n.site).elevationMetres;
            }
            if (ocean)
            {
              *ocean = n.type == CellType::Ocean;
            }
          }
        }
        for (auto i : riverBuckets[size_t(z) * side + x])
        {
          if (!seenRivers.insert(i).second)
          {
            continue;
          }
          const auto &c = rivers[i];
          double d = std::max(0., segmentDistance(p, c.a, c.b) - c.halfWidth);
          if (d < best)
          {
            best = d;
            if (level)
            {
              *level = c.level;
            }
            if (ocean)
            {
              *ocean = false;
            }
          }
        }
      }
    }
    return best;
  }
};
} // namespace

SettlementClass settlementClassOf(uint32_t population)
{
  if (population < 60)
  {
    return SettlementClass::Hamlet;
  }
  if (population < 300)
  {
    return SettlementClass::Village;
  }
  if (population < 1500)
  {
    return SettlementClass::Town;
  }
  if (population < 5000)
  {
    return SettlementClass::City;
  }
  return SettlementClass::Capital;
}

const char *settlementClassName(SettlementClass c)
{
  switch (c)
  {
  case SettlementClass::Hamlet:
    return "hamlet";
  case SettlementClass::Village:
    return "village";
  case SettlementClass::Town:
    return "town";
  case SettlementClass::City:
    return "city";
  default:
    return "capital";
  }
}

bool selectWorldSettlementSite(const GeneratedWorld &world, const WorldSiteSelectionSettings &s, WorldSettlementSite &out, std::string *error)
{
  auto fail = [&](const std::string &m) {
    if (error)
    {
      *error = m;
    }
    return false;
  };
  const double extent = world.graph.brief().extentMetres;
  if (!world.graph.nodeCount() || world.surface.empty() || !(extent > 0) || !std::isfinite(extent))
  {
    return fail("world lacks a generated graph or elevation surface");
  }
  if (world.climate.cells.size() != world.graph.nodeCount())
  {
    return fail("world lacks the climate stage; moisture cannot be assumed");
  }
  if (world.rivers.nodes.size() != world.graph.nodeCount())
  {
    return fail("world lacks the river stage; water cannot be assumed");
  }
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
  {
    if (!std::isfinite(v) || v <= 0)
    {
      return fail("invalid site selection settings");
    }
  }
  if (!s.producer || !s.revision || s.suitabilitySpacingMetres < 1 || s.suitabilitySpacingMetres > 20 || !s.maximumRuralSources || s.maximumRuralSources > 64 || !s.maximumAttempts)
  {
    return fail("invalid site selection identity or budget");
  }
  const double h = s.suitabilitySpacingMetres;
  const double window = std::round(s.windowMetres / h) * h;
  const double axis = window / h;
  if (axis < 8 || axis * axis > 65536)
  {
    return fail("settlement window must fit the suitability grid budget");
  }
  if (window > extent)
  {
    return fail("settlement window is larger than the world");
  }
  const auto leaves = leafNodes(world.graph);
  // Whole-world water index for candidate scoring.
  LocalMap globalMap;
  globalMap.build(world, 0, 0, extent, extent, leaves);
  auto terrainOf = [&](double x, double z) {
    return world.surface.sample({x, z}).elevationMetres;
  };
  const double step = std::max(10., world.surface.spacingMetres());
  auto gradientAt = [&](double x, double z) {
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
    {
      continue;
    }
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
  {
    return fail(s.preferredCell != kInvalidNode ? "preferred cell is not a land leaf" : "world has no land leaf cells");
  }
  std::stable_sort(site.candidates.begin(), site.candidates.end(), [](const WorldSiteCandidate &a, const WorldSiteCandidate &b) {
    if (a.accepted != b.accepted)
    {
      return a.accepted;
    }
    return a.score != b.score ? a.score > b.score : a.cell < b.cell;
  });
  if (site.candidates.size() > s.maximumCandidates)
  {
    site.candidates.resize(s.maximumCandidates);
  }
  auto compose = [&](WorldSiteCandidate &candidate, WorldSettlementSite &r, std::string &why) {
    const auto &siteNode = world.graph.node(candidate.cell);
    r.cell = candidate.cell;
    r.region = candidate.region;
    r.centre = candidate.centre;
    if (candidate.region < world.regions.regions.size())
    {
      r.regionName = world.regions.regions[candidate.region].name;
    }
    // Parent patch region: the urban leaf and its neighbouring map cells (two
    // rings when they fit the window). Children work only inside these patches;
    // the analysis grid is their bounding box, cropped to the grid budget.
    std::set<uint32_t> patchSet{candidate.cell};
    for (auto nb : siteNode.neighbours)
    {
      patchSet.insert(nb);
    }
    auto patchBounds = [&](const std::set<uint32_t> &set, double &x0, double &z0, double &x1, double &z1) {
      x0 = z0 = INFINITY;
      x1 = z1 = -INFINITY;
      for (auto id : set)
      {
        for (auto p : world.graph.node(id).area)
        {
          x0 = std::min(x0, p.x);
          z0 = std::min(z0, p.y);
          x1 = std::max(x1, p.x);
          z1 = std::max(z1, p.y);
        }
      }
    };
    {
      std::set<uint32_t> second = patchSet;
      for (auto id : patchSet)
      {
        for (auto nb : world.graph.node(id).neighbours)
        {
          second.insert(nb);
        }
      }
      double x0, z0, x1, z1;
      patchBounds(second, x0, z0, x1, z1);
      if (x1 - x0 <= window && z1 - z0 <= window)
      {
        patchSet = second;
      }
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
    {
      originX = std::clamp(px0, 0., extent - sideX);
    }
    if (sideZ >= pz1 - pz0)
    {
      originZ = std::clamp(pz0, 0., extent - sideZ);
    }
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
    auto toLocal = [&](Position p) {
      return Position{p.x - originX, p.y, p.z - originZ};
    };
    auto toWorld = [&](Position p) {
      return Position{p.x + originX, p.y, p.z + originZ};
    };
    r.localCentre = toLocal(candidate.centre);
    const double reach = std::max({s.suitability.floodReachMetres, s.waterSupplyReachMetres, s.waterReachMetres});
    LocalMap local;
    local.build(world, originX - reach, originZ - reach, originX + sideX + reach, originZ + sideZ + reach, leaves);
    auto localClip = [&](const std::vector<Point2> &area) {
      Poly poly = clipToRect(toPoly(area), originX, originZ, originX + sideX, originZ + sideZ);
      for (auto &p : poly)
      {
        p = toLocal(p);
      }
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
      {
        parts.push_back(id);
      }
      for (size_t k = 0; k < parts.size(); ++k)
      {
        Poly poly = localClip(refined.node(parts[k]).area);
        if (poly.empty())
        {
          continue;
        }
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
      {
        polys.push_back(p.poly);
      }
      weldTiling(polys);
      for (size_t i = 0; i < polys.size(); ++i)
      {
        planning[i].poly = polys[i];
      }
    }
    {
      std::vector<Piece> kept;
      for (auto &p : planning)
      {
        auto fault = tilingFault(p.poly, 0, 0, sideX, sideZ, 1e-7, 1e-5);
        for (auto &k : kept)
        {
          if (fault.empty() && tilesOverlap(p.poly, k.poly, 1e-7))
          {
            fault = "overlaps planning cell " + std::to_string(k.id);
          }
        }
        if (fault.empty())
        {
          kept.push_back(p);
        }
        else
        {
          r.rejections.push_back("planning cell " + std::to_string(p.id) + " omitted: " + fault);
        }
      }
      planning = std::move(kept);
      if (planning.empty())
      {
        why = "no planning cell survives the master-plan tolerances";
        return false;
      }
    }
    // Planned civic centre, Watabou-style: the urban cell is re-tiled as a
    // Voronoi of a centre site, a symmetric ring of sites (which makes the
    // centre cell an exact regular hexagon or square) and the refined sites
    // farther out. Cells stay convex and tile the leaf by construction.
    // Village form: chosen here because the plaza shape depends on it.
    {
      using Form = WorldSiteSelectionSettings::VillageForm;
      const SettlementClass approximate = settlementClassOf(s.population.maximum);
      Form form = s.villageForm;
      if (form == Form::Auto)
      {
        const uint64_t draw = (uint64_t(candidate.cell) * 0x9E3779B97F4A7C15ULL ^ world.graph.brief().seed) >> 17;
        if (approximate == SettlementClass::Hamlet)
        {
          form = draw % 2 ? Form::Scattered : Form::Cluster;
        }
        else if (approximate == SettlementClass::Village)
        {
          form = Form(uint8_t(Form::Cluster) + draw % 4); // Cluster, Street, Round, Anger.
        }
        else
        {
          form = Form::Cluster;
        }
      }
      r.villageForm = form;
    }
    if (s.plazaSides >= 3 && s.plazaSides <= 12)
    {
      using Form = WorldSiteSelectionSettings::VillageForm;
      const uint32_t population = s.population.maximum;
      const double radius = s.plazaRadiusMetres > 0 ? s.plazaRadiusMetres : population < 60 ? 18 : population < 300 ? 26 : population < 1500 ? 38 : 50;
      const Position worldCentre = candidate.centre;
      // An Anger is a four-sided green stretched along the axis between the
      // two farthest land neighbours (the through road); a round village keeps
      // a wide green for its ring of farms.
      const uint32_t sides = r.villageForm == Form::Anger ? 4 : s.plazaSides;
      const double aspect = r.villageForm == Form::Anger ? std::max(2.4, s.plazaAspect) : std::max(1., s.plazaAspect);
      double axis = 0;
      if (aspect > 1)
      {
        double farthest = -1;
        for (auto na : siteNode.neighbours)
        {
          for (auto nb : siteNode.neighbours)
          {
            const auto &a = world.graph.node(na), &b = world.graph.node(nb);
            if (!a.land || !b.land)
            {
              continue;
            }
            const double d = std::hypot(a.site.x - b.site.x, a.site.y - b.site.y);
            if (d > farthest)
            {
              farthest = d;
              axis = std::atan2(b.site.y - a.site.y, b.site.x - a.site.x);
            }
          }
        }
      }
      std::vector<Point2> sites{{worldCentre.x, worldCentre.z}};
      for (uint32_t k = 0; k < sides; ++k)
      {
        const double angle = axis + 6.283185307179586 * k / sides;
        const double reach = 2 * radius * (aspect > 1 && k % 2 == 0 ? aspect : 1);
        sites.push_back({worldCentre.x + std::cos(angle) * reach, worldCentre.z + std::sin(angle) * reach});
      }
      for (auto child : refined.node(candidate.cell).children)
      {
        auto site = refined.node(child).site;
        if (std::hypot(site.x - worldCentre.x, site.y - worldCentre.z) > 2.8 * radius * aspect)
        {
          sites.push_back(site);
        }
      }
      std::vector<VoronoiCell> cells;
      std::string voronoiError;
      if (buildVoronoiCells(sites, siteNode.area, 1e-6, cells, &voronoiError))
      {
        std::vector<Piece> planned;
        uint64_t plazaId = 0;
        for (size_t k = 0; k < cells.size(); ++k)
        {
          ConvexRing poly = localClip(cells[k].area);
          if (poly.empty())
          {
            continue;
          }
          Piece piece{uint64_t(candidate.cell + 1) * 1024 + k + 1, candidate.cell, poly, kInvalidNode, 0, 0};
          if (std::hypot(cells[k].site.x - worldCentre.x, cells[k].site.y - worldCentre.z) < 1e-6)
          {
            plazaId = piece.id;
          }
          planned.push_back(std::move(piece));
        }
        if (plazaId && !planned.empty())
        {
          planning = std::move(planned);
          std::vector<ConvexRing> polys;
          for (auto &p : planning)
          {
            polys.push_back(p.poly);
          }
          weldTiling(polys);
          for (size_t i = 0; i < polys.size(); ++i)
          {
            planning[i].poly = polys[i];
          }
          std::vector<Piece> kept;
          for (auto &p : planning)
          {
            auto fault = tilingFault(p.poly, 0, 0, sideX, sideZ, 1e-7, 1e-5);
            for (auto &k : kept)
            {
              if (fault.empty() && tilesOverlap(p.poly, k.poly, 1e-7))
              {
                fault = "overlaps planning cell " + std::to_string(k.id);
              }
            }
            if (fault.empty())
            {
              kept.push_back(p);
            }
            else
            {
              r.rejections.push_back("planning cell " + std::to_string(p.id) + " omitted in the planned tiling: " + fault);
            }
          }
          planning = std::move(kept);
          r.plazaCell = plazaId;
        }
        else
        {
          r.rejections.push_back("planned plaza tiling produced no centre cell; the refined cells are used");
        }
      }
      else
      {
        r.rejections.push_back("planned plaza tiling failed: " + voronoiError);
      }
    }
    // The town's tiling grows into the neighbouring leaves: every hinterland
    // piece within reach of the urban cell is tiled at planning spacing and
    // its cells join the planning set (outskirts candidates, or farmland), so
    // a city's core never uses up its whole leaf.
    {
      const double reach = r.demand.population >= 2000 ? 300. : 160.;
      const Poly urbanLocal = localClip(siteNode.area);
      std::vector<Piece> keep;
      std::set<uint64_t> grownIds;
      size_t grown = 0;
      const uint64_t seed = world.graph.brief().seed;
      for (auto &piece : pending)
      {
        bool near = false;
        for (auto v : piece.poly)
        {
          near |= polygonDistance(v, urbanLocal) < reach;
        }
        // A piece that overlaps another hinterland piece (the map's own
        // refinement is not exact) is left to the hinterland filter, which
        // omits it; grown cells must never inherit such an overlap.
        bool overlapping = false;
        for (const auto &other : pending)
        {
          overlapping |= &other != &piece && tilesOverlap(piece.poly, other.poly, 1e-8);
        }
        if (!near || overlapping || piece.node == kInvalidNode || planning.size() > 480)
        {
          keep.push_back(piece);
          continue;
        }
        double bx0 = INFINITY, bz0 = INFINITY, bx1 = -INFINITY, bz1 = -INFINITY;
        for (auto v : piece.poly)
        {
          bx0 = std::min(bx0, v.x);
          bz0 = std::min(bz0, v.z);
          bx1 = std::max(bx1, v.x);
          bz1 = std::max(bz1, v.z);
        }
        std::vector<Point2> sites;
        const double spacing = s.planningSpacingMetres;
        for (double z = bz0; z < bz1; z += spacing)
        {
          for (double x = bx0; x < bx1; x += spacing)
          {
            const uint64_t h = (seed + 1) * 0x9E3779B97F4A7C15ULL ^ (uint64_t(std::llround(x)) + 7) * 0xBF58476D1CE4E5B9ULL ^ (uint64_t(std::llround(z)) + 13) * 0x94D049BB133111EBULL;
            const double jx = double((h >> 11) % 1000) / 1000., jz = double((h >> 31) % 1000) / 1000.;
            const Position p{x + spacing * (.2 + .6 * jx), 0, z + spacing * (.2 + .6 * jz)};
            if (strictlyInsideConvex(piece.poly, p))
            {
              sites.push_back({p.x + originX, p.z + originZ});
            }
          }
        }
        // The Voronoi window is the piece itself (in world coordinates), and
        // every cell is clipped to the piece again, so no cell spills over a
        // sibling piece of the same leaf.
        std::vector<Point2> window;
        for (auto v : piece.poly)
        {
          window.push_back({v.x + originX, v.z + originZ});
        }
        std::vector<VoronoiCell> cells;
        std::string voronoiError;
        if (sites.size() < 2 || !buildVoronoiCells(sites, window, 1e-6, cells, &voronoiError))
        {
          keep.push_back(piece);
          continue;
        }
        for (size_t k = 0; k < cells.size(); ++k)
        {
          Poly poly = localClip(cells[k].area);
          for (size_t e = 0; e < piece.poly.size() && !poly.empty(); ++e)
          {
            const Position a = piece.poly[e], b = piece.poly[(e + 1) % piece.poly.size()];
            const double nx = b.z - a.z, nz = a.x - b.x, c = nx * a.x + nz * a.z;
            Poly q;
            for (size_t i = 0; i < poly.size(); ++i)
            {
              const Position u = poly[i], v = poly[(i + 1) % poly.size()];
              const double du = nx * u.x + nz * u.z - c, dv = nx * v.x + nz * v.z - c;
              if (du <= 1e-9)
              {
                q.push_back(u);
              }
              if ((du < -1e-9 && dv > 1e-9) || (dv < -1e-9 && du > 1e-9))
              {
                q.push_back({u.x + (v.x - u.x) * du / (du - dv), 0, u.z + (v.z - u.z) * du / (du - dv)});
              }
            }
            poly = q;
          }
          if (poly.size() < 3 || std::abs(signedArea(poly)) < 20)
          {
            continue;
          }
          planning.push_back({piece.id * 1024 + k + 1, piece.leaf, std::move(poly), kInvalidNode, 0, 0});
          grownIds.insert(planning.back().id);
          ++grown;
        }
      }
      pending = std::move(keep);
      if (std::getenv("CITY_DEBUG"))
      {
        std::fprintf(stderr, "voronoi growth: %zu cells grown, %zu hinterland pieces kept, %zu planning cells\n", grown, pending.size(), planning.size());
      }
      if (grown)
      {
        // The grown cells meet the urban cell and their neighbours at T
        // junctions: those vertices are welded into the neighbouring edges
        // (planning and hinterland pieces together), so the overlap
        // predicates downstream see exact tilings.
        std::vector<Poly> polys;
        for (auto &p : planning)
        {
          polys.push_back(p.poly);
        }
        for (auto &p : pending)
        {
          polys.push_back(p.poly);
        }
        weldTiling(polys);
        for (size_t i = 0; i < planning.size(); ++i)
        {
          planning[i].poly = polys[i];
        }
        for (size_t i = 0; i < pending.size(); ++i)
        {
          pending[i].poly = polys[planning.size() + i];
        }
        // A grown cell that still overlaps another planning cell or a
        // hinterland piece (the map's refinement is not exact at every
        // seam) is omitted: the tiling downstream must be exact.
        std::vector<Piece> exact;
        size_t omitted = 0;
        for (size_t i = 0; i < planning.size(); ++i)
        {
          bool overlapping = false;
          if (grownIds.count(planning[i].id))
          {
            for (size_t j = 0; j < planning.size() && !overlapping; ++j)
            {
              overlapping = i != j && tilesOverlap(planning[i].poly, planning[j].poly, 1e-8);
            }
            for (const auto &other : pending)
            {
              overlapping = overlapping || tilesOverlap(planning[i].poly, other.poly, 1e-8);
            }
          }
          if (overlapping)
          {
            ++omitted;
          }
          else
          {
            exact.push_back(planning[i]);
          }
        }
        planning = std::move(exact);
        if (omitted)
        {
          r.rejections.push_back(std::to_string(omitted) + " grown planning cells omitted: they overlapped a neighbour at an inexact seam");
        }
        r.rejections.push_back(std::to_string(grown) + " planning cells grown into the neighbouring leaves within " + std::to_string(int(reach)) + " m of the town");
      }
    }
    // Watabou's optimizeJunctions: an edge shorter than 8 of his units (a
    // fraction of the cell spacing here) collapses to its midpoint in every
    // cell that shares its ends, so no ward keeps a needle corner. The plaza
    // keeps its shape, and a merge that would bend a cell concave is skipped
    // (the layout clips by cell half-planes).
    {
      const double shortEdge = s.planningSpacingMetres * .4;
      auto convexPoly = [](const Poly &q) {
        for (size_t i = 0; i < q.size(); ++i)
        {
          if (cross(q[i], q[(i + 1) % q.size()], q[(i + 2) % q.size()]) < -1e-6)
          {
            return false;
          }
        }
        return q.size() >= 3;
      };
      auto tidyPoly = [](Poly &q) {
        Poly t;
        for (auto v : q)
        {
          if (t.empty() || length(t.back(), v) > 1e-6)
          {
            t.push_back(v);
          }
        }
        while (t.size() > 1 && length(t.front(), t.back()) < 1e-6)
        {
          t.pop_back();
        }
        q = t;
      };
      size_t merged = 0;
      for (auto &cell : planning)
      {
        if (cell.id == r.plazaCell)
        {
          continue;
        }
        for (size_t index = 0; index < cell.poly.size() && cell.poly.size() > 3;)
        {
          const Position v0 = cell.poly[index], v1 = cell.poly[(index + 1) % cell.poly.size()];
          if (length(v0, v1) >= shortEdge)
          {
            ++index;
            continue;
          }
          bool onPlaza = false;
          for (const auto &other : planning)
          {
            if (other.id == r.plazaCell)
            {
              for (auto q : other.poly)
              {
                onPlaza |= length(q, v0) < 1e-6 || length(q, v1) < 1e-6;
              }
            }
          }
          // A vertex on the edge of a polygon that does not carry it as a
          // vertex (a T junction with a grown or hinterland piece) cannot
          // move: the other polygon would not follow and they would overlap.
          bool pinned = false;
          auto pins = [&](const Poly &poly) {
            for (auto v : {v0, v1})
            {
              if (!onRing(poly, v, 1e-6))
              {
                continue;
              }
              bool vertex = false;
              for (auto q : poly)
              {
                vertex |= length(q, v) < 1e-6;
              }
              pinned |= !vertex;
            }
          };
          for (const auto &other : planning)
          {
            pins(other.poly);
          }
          for (const auto &other : pending)
          {
            pins(other.poly);
          }
          if (onPlaza || pinned)
          {
            ++index;
            continue;
          }
          const Position mid{(v0.x + v1.x) * .5, 0, (v0.z + v1.z) * .5};
          // Every cell sharing either end moves it to the midpoint; the merge
          // stands only if every touched cell stays convex with enough sides.
          std::vector<std::pair<Piece *, Poly>> trial;
          for (auto &other : planning)
          {
            bool touches = false;
            for (auto q : other.poly)
            {
              touches |= length(q, v0) < 1e-6 || length(q, v1) < 1e-6;
            }
            if (!touches)
            {
              continue;
            }
            Poly moved = other.poly;
            for (auto &q : moved)
            {
              if (length(q, v0) < 1e-6 || length(q, v1) < 1e-6)
              {
                q = mid;
              }
            }
            tidyPoly(moved);
            trial.emplace_back(&other, std::move(moved));
          }
          bool ok = !trial.empty();
          for (const auto &[piece, moved] : trial)
          {
            ok = ok && moved.size() >= 3 && convexPoly(moved) && signedArea(moved) > 20;
          }
          if (!ok)
          {
            ++index;
            continue;
          }
          for (auto &[piece, moved] : trial)
          {
            piece->poly = std::move(moved);
          }
          ++merged;
          // The cell changed under the loop: restart its scan.
          index = 0;
        }
      }
      if (merged)
      {
        r.rejections.push_back(std::to_string(merged) + " short planning edges merged (optimizeJunctions)");
      }
    }
    for (auto &p : planning)
    {
      r.planningArea += signedArea(p.poly);
    }
    // Immutable captures for the brief's function-backed fields (local frame).
    auto surface = std::make_shared<const MapSurfaceField>(world.surface);
    r.surface = surface;
    auto terrain = [surface, originX, originZ](double x, double z) {
      return surface->sample({x + originX, z + originZ}).elevationMetres;
    };
    auto localMap = std::make_shared<const LocalMap>(local);
    auto worldRef = &world;
    r.water = [localMap, worldRef, originX, originZ, bank = s.riverBankMetres](double x, double z) {
      x += originX;
      z += originZ;
      auto leaf = localMap->leafAt(*worldRef, x, z);
      if (leaf == kInvalidNode || !worldRef->graph.node(leaf).land)
      {
        return true;
      }
      for (auto i : localMap->riverBuckets[localMap->bucket(x, z)])
      {
        const auto &c = localMap->rivers[i];
        if (segmentDistance({x, 0, z}, c.a, c.b) <= c.halfWidth + bank)
        {
          return true;
        }
      }
      return false;
    };
    r.waterSupply = [localMap, worldRef, originX, originZ, reach = s.waterSupplyReachMetres](double x, double z) {
      return localMap->waterDistance(*worldRef, {x + originX, 0, z + originZ}, reach) <= reach;
    };
    auto fertilityAt = [&](double x, double z) {
      auto leaf = local.leafAt(world, x + originX, z + originZ);
      return leaf == kInvalidNode ? 0. : biomeFertility(biomeOf(world.graph.node(leaf)));
    };
    auto moistureAt = [&](double x, double z) {
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
    auto snap = [&](Position p) {
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
    {
      p = toLocal(p);
    }
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
      {
        continue;
      }
      Poly other = toPoly(n.area);
      for (auto &p : other)
      {
        p = toLocal(p);
      }
      Position mid;
      int shared = 0;
      for (auto a : urban)
      {
        for (auto b : other)
        {
          if (length(a, b) < 1e-6)
          {
            mid.x += a.x;
            mid.z += a.z;
            ++shared;
          }
        }
      }
      Position localSite = toLocal({siteNode.site.x, 0, siteNode.site.y}), localOther = toLocal({n.site.x, 0, n.site.y});
      if (shared < 2)
      {
        mid = {(localSite.x + localOther.x) * .5, 0, (localSite.z + localOther.z) * .5};
      }
      else
      {
        mid = {mid.x / shared, 0, mid.z / shared};
      }
      double dx = localOther.x - localSite.x, dz = localOther.z - localSite.z, len = std::hypot(dx, dz);
      if (len < 1e-6)
      {
        continue;
      }
      Position target{mid.x + dx / len * s.exitOffsetMetres, 0, mid.z + dz / len * s.exitOffsetMetres};
      Position chosen;
      bool found = false;
      for (int ring = 0; ring <= 4 && !found; ++ring)
      {
        for (int oz = -ring; oz <= ring && !found; ++oz)
        {
          for (int ox = -ring; ox <= ring && !found; ++ox)
          {
            if (std::max(std::abs(ox), std::abs(oz)) != ring)
            {
              continue;
            }
            Position p = snap({target.x + ox * h, 0, target.z + oz * h});
            size_t cell = suitability.cellAt(p.x, p.z);
            if (cell == SIZE_MAX || !suitability.cells()[cell].walkable || suitability.cells()[cell].component != component || insideEvenOdd(urban, p))
            {
              continue;
            }
            chosen = p;
            found = true;
          }
        }
      }
      if (!found)
      {
        r.rejections.push_back("no traversable road exit toward neighbour cell " + std::to_string(nb));
        continue;
      }
      exits.push_back({nb, biomeFertility(biomeOf(n)), chosen});
    }
    std::stable_sort(exits.begin(), exits.end(), [](const Exit &a, const Exit &b) {
      return a.fertility != b.fertility ? a.fertility > b.fertility : a.neighbour < b.neighbour;
    });
    if (exits.size() > s.maximumRuralSources)
    {
      exits.resize(s.maximumRuralSources);
    }
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
    {
      maxPlanningArea = std::max(maxPlanningArea, signedArea(p.poly));
    }
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
      {
        slope = std::max(slope, gradientAt(cw.x + (v.x - c.x) * .8, cw.z + (v.z - c.z) * .8));
      }
      cell.slope = std::clamp(slope, 0., 1.);
      double level = 0;
      bool ocean = false;
      double d = local.waterDistance(world, cw, s.suitability.floodReachMetres, &level, &ocean);
      if (d <= s.suitability.waterSetbackMetres)
      {
        cell.floodRisk = 1;
      }
      else if (d <= s.suitability.floodReachMetres && elevations[i] <= level + s.suitability.floodRiseMetres)
      {
        cell.floodRisk = std::clamp(1 - (d - s.suitability.waterSetbackMetres) / (s.suitability.floodReachMetres - s.suitability.waterSetbackMetres), 0., 1.);
      }
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
    r.settlementClass = settlementClassOf(r.demand.population);
    // Institutions by class: towns get a manor, cities and capitals a castle
    // and a manor. Their eligibility (defence, fertility) is judged by the
    // master plan; an unplaceable request is a recorded omission.
    if (r.settlementClass >= SettlementClass::Town)
    {
      // Wealthy quarter beside the centre: large plots with private gardens
      // (the layout's WealthyResidential grammar), a tenth of the housing demand.
      for (auto &d : input.districts)
      {
        if (d.use == DistrictUse::Residential)
        {
          DistrictRequest wealthy;
          wealthy.id = input.districts.size() + 1;
          wealthy.use = DistrictUse::WealthyResidential;
          wealthy.minimumArea = std::max(650., d.targetArea * .1);
          wealthy.targetArea = std::max(wealthy.minimumArea, d.targetArea * .2);
          wealthy.required = false;
          input.districts.push_back(wealthy);
          break;
        }
      }
      DistrictRequest manor;
      manor.id = input.districts.size() + 1;
      manor.use = DistrictUse::Manor;
      manor.minimumArea = 900;
      manor.targetArea = 2400;
      manor.required = false;
      input.districts.push_back(manor);
    }
    if (r.settlementClass >= SettlementClass::City)
    {
      DistrictRequest castle;
      castle.id = input.districts.size() + 1;
      castle.use = DistrictUse::Castle;
      castle.minimumArea = 2000;
      castle.targetArea = 4800;
      castle.required = false;
      input.districts.push_back(castle);
    }
    // Towns and cities keep a packed poor quarter inside the walls: a Suburb-use
    // district (45 m2 plots in the layout) carved from the residential demand.
    if (r.settlementClass >= SettlementClass::Town)
    {
      for (auto &d : input.districts)
      {
        if (d.use == DistrictUse::Residential)
        {
          DistrictRequest poor;
          poor.id = input.districts.size() + 1;
          poor.use = DistrictUse::Suburb;
          poor.minimumArea = std::max(d.minimumArea, d.targetArea * .2);
          poor.targetArea = std::max(poor.minimumArea, d.targetArea * .3);
          poor.required = true;
          d.targetArea = std::max(d.minimumArea, d.targetArea * .75);
          input.districts.push_back(poor);
          break;
        }
      }
    }
    // Prepared institutional sites: the plaza is the usable planning cell at
    // the settlement centre, the temple a usable neighbour of it (Watabou's
    // central patch and adjacent cathedral ward).
    {
      auto usableCell = [&](const CityPlanCell &cell) {
        return cell.buildable && cell.slope <= s.territory.planning.maximumSlope && cell.floodRisk <= s.territory.planning.maximumFloodRisk;
      };
      double nearest = INFINITY;
      const CityPlanCell *plaza = nullptr;
      for (auto &cell : input.cells)
      {
        if (!usableCell(cell))
        {
          continue;
        }
        if (r.plazaCell && cell.id == r.plazaCell)
        {
          plaza = &cell;
          break;
        }
        double d = length(centroid(cell.boundary), r.localCentre);
        if (d < nearest)
        {
          nearest = d;
          plaza = &cell;
        }
      }
      if (plaza)
      {
        r.plazaCell = plaza->id;
        const CityPlanCell *temple = nullptr;
        double best = INFINITY;
        for (auto &cell : input.cells)
        {
          if (&cell == plaza || !usableCell(cell))
          {
            continue;
          }
          int shared = 0;
          for (auto a : plaza->boundary)
          {
            for (auto b : cell.boundary)
            {
              shared += length(a, b) < 1e-6;
            }
          }
          if (shared < 2)
          {
            continue;
          }
          double d = length(centroid(cell.boundary), r.localCentre);
          if (d < best)
          {
            best = d;
            temple = &cell;
          }
        }
        if (temple)
        {
          r.templeCell = temple->id;
        }
        bool civic = false;
        const bool churchOnGreen = r.villageForm == WorldSiteSelectionSettings::VillageForm::Round && r.settlementClass <= SettlementClass::Village;
        for (auto &d : input.districts)
        {
          if (d.use == DistrictUse::Civic && !civic)
          {
            d.preferredCell = r.plazaCell;
            civic = true;
          }
          if (d.use == DistrictUse::Temple || d.use == DistrictUse::Church || d.use == DistrictUse::Civic)
          {
            // A compound is one patch (Watabou's Cathedral, Administration): its
            // demand is exactly its cell, never a second cell beside it.
            const uint64_t home = d.use == DistrictUse::Civic ? r.plazaCell : r.templeCell ? r.templeCell : r.plazaCell;
            for (const auto &cell : input.cells)
            {
              if (cell.id == home)
              {
                d.targetArea = signedArea(cell.boundary) + 1;
                d.minimumArea = std::min(d.minimumArea, signedArea(cell.boundary) * .5);
              }
            }
          }
          if (d.use == DistrictUse::Temple && r.templeCell)
          {
            d.preferredCell = churchOnGreen ? r.plazaCell : r.templeCell; // Runddorf: the church stands on the green.
            if (churchOnGreen)
            {
              // The church keeps to the green itself; the farms ring it.
              d.targetArea = signedArea(plaza->boundary) + 1;
              d.minimumArea = std::min(d.minimumArea, signedArea(plaza->boundary));
            }
          }
        }
        if (churchOnGreen)
        {
          // The ring of farms frames the green itself; the church takes it, so
          // the separate civic request goes.
          civic = true;
          input.districts.erase(
              std::remove_if(
                  input.districts.begin(),
                  input.districts.end(),
                  [](const DistrictRequest &d) {
                    return d.use == DistrictUse::Civic;
                  }),
              input.districts.end());
        }
        if (r.villageForm == WorldSiteSelectionSettings::VillageForm::Scattered && r.settlementClass <= SettlementClass::Village)
        {
          // Streusiedlung: the households live on the farmsteads; the hamlet
          // core keeps only what fits beside the green, nothing is required.
          for (auto &d : input.districts)
          {
            d.required = false;
            d.minimumArea = std::min(d.minimumArea, 120.);
            d.targetArea = std::min(d.targetArea, 1200.);
          }
        }
        if (!civic)
        {
          // A hamlet or village still has its green at the centre.
          DistrictRequest green;
          green.id = input.districts.size() + 1;
          green.use = DistrictUse::Civic;
          green.minimumArea = std::min(200., signedArea(plaza->boundary));
          green.targetArea = signedArea(plaza->boundary) + 1;
          green.preferredCell = r.plazaCell;
          input.districts.push_back(green);
        }
      }
    }
    // Walled core, Watabou-style: the planning cells nearest the plaza, up to
    // the demanded urban area, are the only allocatable ones. The wall then
    // follows a compact, roughly round set instead of scattered districts.
    if (r.settlementClass <= SettlementClass::Village && r.plazaCell && r.villageForm != WorldSiteSelectionSettings::VillageForm::Cluster && r.villageForm != WorldSiteSelectionSettings::VillageForm::Street)
    {
      // Round and Anger villages are one ring of farms about the green; a
      // scattered hamlet keeps only the green and one neighbour, the rest of
      // its households live on the farmsteads.
      const CityPlanCell *plaza = nullptr;
      for (const auto &cell : input.cells)
      {
        if (cell.id == r.plazaCell)
        {
          plaza = &cell;
        }
      }
      size_t kept = 0, excluded = 0;
      const size_t ringLimit = r.villageForm == WorldSiteSelectionSettings::VillageForm::Scattered ? 2 : 12;
      for (auto &cell : input.cells)
      {
        if (!plaza || cell.id == r.plazaCell || !cell.allocatable)
        {
          continue;
        }
        int shared = 0;
        for (auto a : plaza->boundary)
        {
          for (auto b : cell.boundary)
          {
            shared += length(a, b) < 1e-6;
          }
        }
        if (shared >= 2 && kept < ringLimit)
        {
          ++kept;
        }
        else
        {
          cell.allocatable = false;
          ++excluded;
        }
      }
      if (excluded)
      {
        r.rejections.push_back(std::to_string(excluded) + " planning cells beyond the village ring are kept for circulation only");
      }
    }
    if (r.settlementClass >= SettlementClass::Town && r.plazaCell)
    {
      double demanded = 0;
      for (const auto &d : input.districts)
      {
        demanded += d.targetArea;
      }
      Position plazaCentre = r.localCentre;
      for (const auto &cell : input.cells)
      {
        if (cell.id == r.plazaCell)
        {
          plazaCentre = centroid(cell.boundary);
        }
      }
      std::vector<std::pair<double, size_t>> order;
      for (size_t i = 0; i < input.cells.size(); ++i)
      {
        order.emplace_back(length(centroid(input.cells[i].boundary), plazaCentre), i);
      }
      std::sort(order.begin(), order.end());
      double accumulated = 0;
      size_t excluded = 0;
      for (auto [d, i] : order)
      {
        auto &cell = input.cells[i];
        if (!cell.allocatable)
        {
          continue;
        }
        if (accumulated >= demanded * s.coreAreaFactor)
        {
          cell.allocatable = false;
          ++excluded;
          continue;
        }
        accumulated += signedArea(cell.boundary);
      }
      if (excluded)
      {
        r.rejections.push_back(std::to_string(excluded) + " planning cells beyond the walled core are kept for circulation only");
      }
      // A round wall: the core is made convex. Cells the convex hull of the
      // core crosses are split along it; their inside part joins the core,
      // their outside parts stay beyond the wall; usable cells wholly inside
      // the hull join the core too.
      {
        std::vector<Position> points;
        for (const auto &cell : input.cells)
        {
          if (cell.allocatable)
          {
            points.insert(points.end(), cell.boundary.begin(), cell.boundary.end());
          }
        }
        std::sort(points.begin(), points.end(), [](Position a, Position b) {
          return a.x == b.x ? a.z < b.z : a.x < b.x;
        });
        Poly hull;
        for (int pass = 0; pass < 2 && points.size() >= 3; ++pass)
        {
          const size_t base = hull.size();
          for (auto q : points)
          {
            while (hull.size() >= base + 2 && cross(hull[hull.size() - 2], hull.back(), q) <= 0)
            {
              hull.pop_back();
            }
            hull.push_back(q);
          }
          hull.pop_back();
          std::reverse(points.begin(), points.end());
        }
        // No repeated vertex: a zero-length hull edge would keep every point
        // on both of its sides.
        {
          Poly clean;
          for (auto v : hull)
          {
            if (clean.empty() || length(clean.back(), v) > 1e-6)
            {
              clean.push_back(v);
            }
          }
          while (clean.size() > 1 && length(clean.front(), clean.back()) < 1e-6)
          {
            clean.pop_back();
          }
          hull = clean;
        }
        auto clipInside = [](Poly p, Position a, Position b) {
          // Keep the left of a -> b (inside a CCW hull).
          const double nx = b.z - a.z, nz = a.x - b.x, c = nx * a.x + nz * a.z;
          Poly q;
          for (size_t i = 0; i < p.size(); ++i)
          {
            const Position u = p[i], v = p[(i + 1) % p.size()];
            const double du = nx * u.x + nz * u.z - c, dv = nx * v.x + nz * v.z - c;
            if (du <= 1e-9)
            {
              q.push_back(u);
            }
            if ((du < -1e-9 && dv > 1e-9) || (dv < -1e-9 && du > 1e-9))
            {
              q.push_back({u.x + (v.x - u.x) * du / (du - dv), 0, u.z + (v.z - u.z) * du / (du - dv)});
            }
          }
          return q;
        };
        auto clipOutside = [](Poly p, Position a, Position b) {
          const double nx = -(b.z - a.z), nz = -(a.x - b.x), c = nx * a.x + nz * a.z;
          Poly q;
          for (size_t i = 0; i < p.size(); ++i)
          {
            const Position u = p[i], v = p[(i + 1) % p.size()];
            const double du = nx * u.x + nz * u.z - c, dv = nx * v.x + nz * v.z - c;
            if (du <= 1e-9)
            {
              q.push_back(u);
            }
            if ((du < -1e-9 && dv > 1e-9) || (dv < -1e-9 && du > 1e-9))
            {
              q.push_back({u.x + (v.x - u.x) * du / (du - dv), 0, u.z + (v.z - u.z) * du / (du - dv)});
            }
          }
          return q;
        };
        std::vector<CityPlanCell> added;
        size_t filled = 0, split = 0;
        for (auto &cell : input.cells)
        {
          if (cell.allocatable || hull.size() < 3)
          {
            continue;
          }
          const bool usable = cell.buildable && cell.slope <= s.territory.planning.maximumSlope && cell.floodRisk <= s.territory.planning.maximumFloodRisk;
          size_t insideCount = 0;
          for (auto v : cell.boundary)
          {
            insideCount += strictlyInsideConvex(hull, v) || onRing(hull, v, 1e-6);
          }
          bool hullInside = false;
          for (auto h : hull)
          {
            hullInside |= strictlyInsideConvex(cell.boundary, h);
          }
          if (insideCount == cell.boundary.size())
          {
            if (usable)
            {
              cell.allocatable = true;
              ++filled;
            }
            continue;
          }
          if (insideCount == 0 && !hullInside)
          {
            continue;
          }
          Poly inside = cell.boundary;
          std::vector<Poly> outside;
          Poly rest = cell.boundary;
          for (size_t k = 0; k < hull.size() && !rest.empty(); ++k)
          {
            const Position a = hull[k], b = hull[(k + 1) % hull.size()];
            if (length(a, b) < 1e-6)
            {
              continue;
            }
            inside = clipInside(inside, a, b);
            Poly out = clipOutside(rest, a, b);
            if (out.size() >= 3 && std::abs(signedArea(out)) > 20)
            {
              outside.push_back(out);
            }
            rest = clipInside(rest, a, b);
          }
          // A cell mostly outside the hull stays outside (a shallow notch the
          // wall smoothing rounds); mostly inside, it joins whole. Only a real
          // crossing is split, so no sliver cell is made for a compound to take.
          // Every cell the hull crosses is split, so the core's circumference
          // is the hull itself: a convex wall. A cell almost wholly inside
          // joins whole; a sliver under 20 m2 stays outside.
          const double share = std::abs(signedArea(inside)) / std::max(1e-9, std::abs(signedArea(cell.boundary)));
          if (inside.size() < 3 || std::abs(signedArea(inside)) < 20)
          {
            continue;
          }
          if (outside.empty() || share > .97)
          {
            if (usable)
            {
              cell.allocatable = true;
              ++filled;
            }
            continue;
          }
          if (std::getenv("CITY_DEBUG"))
          {
            std::fprintf(stderr, "hull split cell %llu: inside %.0f m2 (%zu vertices), %zu outside pieces:", (unsigned long long)cell.id, std::abs(signedArea(inside)), inside.size(), outside.size());
            for (const auto &o : outside)
            {
              std::fprintf(stderr, " %.0f m2", std::abs(signedArea(o)));
            }
            std::fprintf(stderr, ", cell %.0f m2, share %.2f\n", std::abs(signedArea(cell.boundary)), share);
          }
          CityPlanCell core = cell;
          core.boundary = inside;
          core.allocatable = usable;
          core.id = cell.id * 16 + 1;
          if (r.templeCell == cell.id)
          {
            r.templeCell = core.id; // The temple keeps its cell under its new id.
          }
          for (size_t k = 0; k < outside.size(); ++k)
          {
            CityPlanCell beyond = cell;
            beyond.boundary = outside[k];
            beyond.allocatable = false;
            beyond.id = cell.id * 16 + 2 + k;
            added.push_back(std::move(beyond));
          }
          cell = core;
          ++split;
        }
        input.cells.insert(input.cells.end(), added.begin(), added.end());
        if (filled || split)
        {
          r.rejections.push_back(std::to_string(split) + " planning cells split along the convex wall, " + std::to_string(filled) + " filled into the core");
        }
      }
    }
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
        if (!cell.buildable || cell.slope > s.territory.planning.maximumSlope || cell.floodRisk > s.territory.planning.maximumFloodRisk || cell.id == r.plazaCell || cell.id == r.templeCell)
        {
          continue;
        }
        double d = length(centroid(cell.boundary), target);
        if (d < nearest)
        {
          nearest = d;
          gate = cell.id;
        }
      }
      if (gate && std::find(input.gateCells.begin(), input.gateCells.end(), gate) == input.gateCells.end())
      {
        input.gateCells.push_back(gate);
      }
    }
    if (input.gateCells.empty())
    {
      why = "no usable planning cell can serve as a gate";
      return false;
    }
    // The outskirts are chosen once the gate cells exist.
    // Outskirts, Watabou's way: the patches at the gates. A usable cell
    // beyond the core is an outskirts cell when it touches a gate cell or
    // the approach from the gate cell to its exit, and so is one ring of
    // their neighbours (our cells are smaller than Watabou's outer
    // patches); the outskirts filter thins their buildings by distance
    // from the roads. Every other cell of the leaf is farmland below.
    std::vector<Poly> core;
    double coreArea = 0;
    for (const auto &cell : input.cells)
    {
      if (cell.allocatable)
      {
        core.push_back(cell.boundary);
        coreArea += signedArea(cell.boundary);
      }
    }
    auto usableCell = [&](const CityPlanCell &cell) {
      return cell.buildable && cell.slope <= s.territory.planning.maximumSlope && cell.floodRisk <= s.territory.planning.maximumFloodRisk;
    };
    auto touching = [](const Poly &a, const Poly &b) {
      int shared = 0;
      for (auto u : a)
      {
        if (onRing(b, u, 1e-6))
        {
          ++shared;
        }
      }
      for (auto v : b)
      {
        if (onRing(a, v, 1e-6))
        {
          ++shared;
        }
      }
      return shared >= 2;
    };
    std::set<uint64_t> chosen;
    double outerArea = 0;
    for (size_t e = 0; e < exits.size(); ++e)
    {
      // The gate cell of this exit: the gate cell nearest the exit (the gate
      // list is deduplicated, so it cannot be paired by index).
      // The core cell nearest this exit: the wall gate opens from it.
      const CityPlanCell *gateCell = nullptr;
      double nearestGate = INFINITY;
      for (const auto &cell : input.cells)
      {
        if (cell.allocatable)
        {
          const double d = polygonDistance(exits[e].point, cell.boundary);
          if (d < nearestGate)
          {
            nearestGate = d;
            gateCell = &cell;
          }
        }
      }
      if (!gateCell)
      {
        continue;
      }
      const Position from = centroid(gateCell->boundary), to = exits[e].point;
      if (std::getenv("CITY_DEBUG"))
      {
        size_t candidates = 0, unusable = 0;
        for (const auto &cell : input.cells)
        {
          if (cell.allocatable)
          {
            continue;
          }
          ++candidates;
          unusable += !usableCell(cell);
        }
        std::fprintf(stderr, "outskirts exit %zu: gate cell %llu at %.0f m from the exit, %zu candidate cells beyond the core (%zu unusable)\n", e, (unsigned long long)gateCell->id, nearestGate, candidates, unusable);
      }
      for (const auto &cell : input.cells)
      {
        if (cell.allocatable || !usableCell(cell) || chosen.count(cell.id))
        {
          continue;
        }
        bool at = touching(cell.boundary, gateCell->boundary);
        for (double t = 0; t <= 1 && !at; t += .05)
        {
          at = strictlyInsideConvex(cell.boundary, {from.x + (to.x - from.x) * t, 0, from.z + (to.z - from.z) * t});
        }
        if (at)
        {
          chosen.insert(cell.id);
          outerArea += signedArea(cell.boundary);
        }
      }
    }
    // One ring of neighbours around the gate cells, within a per-gate share
    // of the core's area (the outskirts are the patches at the gates, not a
    // second town).
    const double outerBudget = coreArea * s.outskirtsAreaFactor * .4;
    std::vector<uint64_t> ring(chosen.begin(), chosen.end());
    for (auto id : ring)
    {
      const CityPlanCell *first = nullptr;
      for (const auto &cell : input.cells)
      {
        if (cell.id == id)
        {
          first = &cell;
        }
      }
      for (const auto &cell : input.cells)
      {
        if (cell.allocatable || !usableCell(cell) || chosen.count(cell.id) || outerArea >= outerBudget)
        {
          continue;
        }
        if (first && touching(cell.boundary, first->boundary))
        {
          chosen.insert(cell.id);
          outerArea += signedArea(cell.boundary);
        }
      }
    }
    size_t outer = 0;
    for (auto &cell : input.cells)
    {
      if (chosen.count(cell.id))
      {
        cell.outskirts = true;
        ++outer;
      }
    }
    if (std::getenv("CITY_DEBUG"))
    {
      std::fprintf(stderr, "outskirts: %zu exits, %zu gate cells, %zu cells chosen (%.0f m2 against core %.0f m2)\n", exits.size(), input.gateCells.size(), outer, outerArea, coreArea);
    }
    r.rejections.push_back(std::to_string(outer) + " planning cells are outskirts, the remaining " + std::to_string(input.cells.size() - core.size() - outer) + " of the leaf are farmland");
    // Hinterland regions: the map's refined cells with explicit cover labels.
    // A patch whose owned analysis cells include unsuitable ground, or whose
    // box exceeds the field generator's grid, is refined again through the map
    // hierarchy and its children are judged on their own. Failing leaves at the
    // finest depth are still offered, so the rural generator records them.
    auto blockedCells = [&](const Poly &poly) {
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
        {
          continue;
        }
        bool owned = true;
        for (size_t k = 0; k < poly.size() && owned; ++k)
        {
          owned = cross(poly[k], poly[(k + 1) % poly.size()], c) >= -1e-8;
        }
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
      const bool refine =
          piece.depth < 2 && piece.node != kInvalidNode && !refined.node(piece.node).refined() && (tooWide || piece.blocked > 0) && signedArea(toPoly(refined.node(piece.node).area)) >= childSpacing * childSpacing * 3;
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
            {
              continue;
            }
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
      std::stable_sort(hinterland.begin(), hinterland.end(), [&](const Piece &a, const Piece &b) {
        if ((a.blocked > 0) != (b.blocked > 0))
        {
          return a.blocked == 0;
        }
        return length(centroid(a.poly), r.localCentre) < length(centroid(b.poly), r.localCentre);
      });
      r.rejections.push_back(std::to_string(hinterland.size() - 512) + " hinterland regions beyond the 512-region budget were omitted");
      hinterland.resize(512);
    }
    std::stable_sort(hinterland.begin(), hinterland.end(), [](const Piece &a, const Piece &b) {
      return a.id < b.id;
    });
    {
      std::vector<Poly> polys;
      for (auto &p : hinterland)
      {
        polys.push_back(p.poly);
      }
      weldTiling(polys);
      for (size_t i = 0; i < polys.size(); ++i)
      {
        hinterland[i].poly = polys[i];
      }
    }
    {
      std::vector<Piece> kept;
      for (auto &p : hinterland)
      {
        auto fault = tilingFault(p.poly, 0, 0, sideX, sideZ, 1e-8, 1e-5);
        if (fault == "vertex count " + std::to_string(p.poly.size()) && p.poly.size() > 32 && p.poly.size() <= 256)
        {
          fault.clear();
        }
        for (auto &k : kept)
        {
          if (fault.empty() && tilesOverlap(p.poly, k.poly, 1e-8))
          {
            fault = "overlaps region " + std::to_string(k.id);
          }
        }
        if (fault.empty())
        {
          kept.push_back(p);
        }
        else
        {
          r.rejections.push_back("hinterland region " + std::to_string(p.id) + " omitted: " + fault);
        }
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
      {
        r.openArea += a;
      }
      meanArea += a;
      largest = std::max(largest, a);
      r.regions.push_back(std::move(region));
    }
    meanArea /= std::max<size_t>(1, hinterland.size());
    // The leaf's own spare planning cells (neither core nor outskirts) are
    // farmland too, so the fields reach the town instead of stopping at the
    // leaf boundary. Water cells keep their cover from the leaf.
    {
      size_t fieldsCells = 0;
      for (const auto &cell : input.cells)
      {
        if (cell.allocatable || cell.outskirts || cell.boundary.size() < 3)
        {
          continue;
        }
        bool overlapping = false;
        for (const auto &region : r.regions)
        {
          overlapping = overlapping || tilesOverlap(cell.boundary, region.boundary, 1e-8);
        }
        if (overlapping)
        {
          continue;
        }
        RuralRegion region;
        region.id = cell.id;
        region.boundary = cell.boundary;
        region.cover = cell.buildable ? RuralLandCover::Open : coverOf(siteNode);
        const double a = signedArea(cell.boundary);
        r.hinterlandArea += a;
        if (region.cover == RuralLandCover::Open)
        {
          r.openArea += a;
        }
        r.regions.push_back(std::move(region));
        ++fieldsCells;
      }
      if (fieldsCells)
      {
        r.rejections.push_back(std::to_string(fieldsCells) + " spare planning cells offered as farmland");
      }
    }
    // Suburb belt: towns and cities keep the open regions touching the urban
    // cell (two rings for cities) for outer wards; farms take the land beyond.
    if (r.settlementClass >= SettlementClass::Town)
    {
      const uint32_t rings = r.settlementClass >= SettlementClass::City ? 2 : 1;
      // Refined children split the shared edge, so touching is a vertex of one
      // ring lying on the other's boundary, not two coincident vertices.
      auto touching = [](const Poly &a, const Poly &b) {
        for (auto u : a)
        {
          if (onRing(b, u, 1e-6))
          {
            return true;
          }
        }
        for (auto v : b)
        {
          if (onRing(a, v, 1e-6))
          {
            return true;
          }
        }
        return false;
      };
      // Only the belt around the gate roads is kept for outskirts (the regions
      // holding a rural exit, or within a short walk of one); the other
      // regions touching the wall are farmed, so fields reach the town.
      std::vector<Poly> frontier = {urban};
      for (uint32_t ring = 0; ring < rings; ++ring)
      {
        std::vector<Poly> next;
        for (auto &region : r.regions)
        {
          if (region.cover != RuralLandCover::Open || region.reserved)
          {
            continue;
          }
          bool touches = false;
          for (const auto &f : frontier)
          {
            touches |= touching(region.boundary, f);
          }
          bool nearRoad = ring > 0;
          for (const auto &exit : exits)
          {
            nearRoad |= polygonDistance(exit.point, region.boundary) <= 60;
          }
          if (touches && nearRoad)
          {
            region.reserved = true;
            next.push_back(region.boundary);
          }
        }
        frontier = std::move(next);
      }
    }
    auto &territory = r.brief.settings;
    territory = s.territory;
    if (r.villageForm == WorldSiteSelectionSettings::VillageForm::Scattered)
    {
      territory.farmsteadInterval = 1; // Streusiedlung: a farmstead on every patch.
    }
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
    territory.rural.maximumPatches = std::clamp<uint32_t>(territory.rural.maximumPatches, 1, 256);
    territory.rural.partialRegions = true; // Farm the suitable part of a region rather than lose it.
    r.brief.terrain = terrain;
    return true;
  };
  for (auto &candidate : site.candidates)
  {
    if (!candidate.accepted)
    {
      continue;
    }
    if (site.attempts >= s.maximumAttempts)
    {
      break;
    }
    ++site.attempts;
    WorldSettlementSite attempt = site;
    std::string why;
    if (compose(candidate, attempt, why))
    {
      attempt.candidates = site.candidates;
      out = std::move(attempt);
      if (error)
      {
        error->clear();
      }
      return true;
    }
    candidate.accepted = false;
    candidate.rejection = why;
  }
  std::string summary = "no candidate site composed within " + std::to_string(site.attempts) + " attempts";
  for (auto &c : site.candidates)
  {
    if (!c.rejection.empty())
    {
      summary += "; cell " + std::to_string(c.cell) + ": " + c.rejection;
      break;
    }
  }
  return fail(summary);
}

bool selectKingdomSites(const GeneratedWorld &world, const WorldSiteSelectionSettings &s, const KingdomSettings &k, KingdomPlan &out, std::string *error)
{
  auto fail = [&](const std::string &m) {
    if (error)
    {
      *error = m;
    }
    return false;
  };
  // Rank every land leaf once through the site ledger without composing a domain.
  WorldSiteSelectionSettings ranking = s;
  ranking.maximumAttempts = 1;
  ranking.maximumCandidates = 100000;
  WorldSettlementSite ledger;
  std::string ledgerError;
  bool composed = selectWorldSettlementSite(world, ranking, ledger, &ledgerError);
  if (!composed && ledger.candidates.empty())
  {
    return fail("kingdom ranking: " + ledgerError);
  }
  std::vector<const WorldSiteCandidate *> accepted;
  for (const auto &c : ledger.candidates)
  {
    if (c.accepted || c.rejection.rfind("no ", 0) == 0 || c.rejection.rfind("hinterland", 0) == 0)
    {
      accepted.push_back(&c);
    }
  }
  if (accepted.empty())
  {
    return fail("kingdom has no acceptable site");
  }
  KingdomPlan plan;
  auto spaced = [&](Position p, double spacing) {
    for (const auto &settled : plan.settlements)
    {
      if (length(settled.centre, p) < spacing)
      {
        return false;
      }
    }
    return true;
  };
  auto place = [&](SettlementClass role, uint32_t count, double spacing, uint32_t population) {
    uint32_t placed = 0;
    for (const auto *c : accepted)
    {
      if (placed >= count)
      {
        break;
      }
      bool used = false;
      for (const auto &settled : plan.settlements)
      {
        used |= settled.cell == c->cell;
      }
      if (used || !spaced(c->centre, spacing))
      {
        continue;
      }
      KingdomSettlement settlement;
      settlement.id = plan.settlements.size() + 1;
      settlement.cell = c->cell;
      settlement.role = role;
      settlement.centre = c->centre;
      settlement.score = c->score;
      settlement.population = population;
      if (c->region < world.regions.regions.size())
      {
        settlement.regionName = world.regions.regions[c->region].name;
      }
      plan.settlements.push_back(std::move(settlement));
      ++placed;
    }
    if (placed < count)
    {
      plan.rejections.push_back(std::string(settlementClassName(role)) + ": placed " + std::to_string(placed) + " of " + std::to_string(count) + " within spacing " + std::to_string(int(spacing)) + " m");
    }
  };
  place(SettlementClass::Capital, 1, k.capitalSpacingMetres, k.capitalPopulation);
  place(SettlementClass::Town, k.towns, k.townSpacingMetres, k.townPopulation);
  place(SettlementClass::Village, k.villages, k.villageSpacingMetres, k.villagePopulation);
  place(SettlementClass::Hamlet, k.hamlets, k.hamletSpacingMetres, k.hamletPopulation);
  // Planning roads: capital and towns form a spanning tree; every village and
  // hamlet joins its nearest town-class settlement.
  std::vector<RoadAnchor> anchors;
  for (auto &settled : plan.settlements)
  {
    RoadAnchor anchor;
    anchor.id = settled.id;
    anchor.position = settled.centre;
    anchor.town = settled.role == SettlementClass::Capital || settled.role == SettlementClass::Town;
    if (!anchor.town)
    {
      double nearest = INFINITY;
      for (const auto &other : plan.settlements)
      {
        if ((other.role == SettlementClass::Capital || other.role == SettlementClass::Town) && length(other.centre, settled.centre) < nearest)
        {
          nearest = length(other.centre, settled.centre);
          settled.parentTown = other.id;
        }
      }
      anchor.parentTown = settled.parentTown;
    }
    anchors.push_back(anchor);
  }
  std::vector<RoadLink> links;
  std::string linkError;
  if (!selectRoadLinks(anchors, links, &linkError))
  {
    plan.rejections.push_back("road links: " + linkError);
  }
  for (const auto &link : links)
  {
    KingdomLink out;
    out.from = link.from;
    out.to = link.to;
    for (const auto &settled : plan.settlements)
    {
      if (settled.id == link.from)
      {
        out.a = settled.centre;
      }
      if (settled.id == link.to)
      {
        out.b = settled.centre;
      }
    }
    bool fromTown = false, toTown = false;
    for (const auto &settled : plan.settlements)
    {
      if (settled.id == link.from)
      {
        fromTown = settled.role == SettlementClass::Capital || settled.role == SettlementClass::Town;
      }
      if (settled.id == link.to)
      {
        toTown = settled.role == SettlementClass::Capital || settled.role == SettlementClass::Town;
      }
    }
    out.trunk = fromTown && toTown;
    plan.links.push_back(out);
  }
  // Road villages: along each trunk road, far from the capital, a linear
  // village where an acceptable leaf lies within reach of the road.
  {
    Position capital;
    for (const auto &settled : plan.settlements)
    {
      if (settled.role == SettlementClass::Capital)
      {
        capital = settled.centre;
      }
    }
    uint32_t placed = 0;
    std::vector<KingdomLink> trunks;
    for (const auto &link : plan.links)
    {
      if (link.trunk)
      {
        trunks.push_back(link);
      }
    }
    for (const auto &link : trunks)
    {
      double total = length(link.a, link.b);
      for (double along = k.roadVillageIntervalMetres; along < total - k.roadVillageIntervalMetres * .5 && placed < k.roadVillages; along += k.roadVillageIntervalMetres)
      {
        Position at{link.a.x + (link.b.x - link.a.x) * along / total, 0, link.a.z + (link.b.z - link.a.z) * along / total};
        if (length(at, capital) < k.roadVillageMinimumFromCapitalMetres)
        {
          continue;
        }
        const WorldSiteCandidate *best = nullptr;
        double nearest = k.roadVillageReachMetres;
        for (const auto *c : accepted)
        {
          bool used = false;
          for (const auto &settled : plan.settlements)
          {
            used |= settled.cell == c->cell;
          }
          if (used)
          {
            continue;
          }
          double d = length(c->centre, at);
          if (d < nearest)
          {
            nearest = d;
            best = c;
          }
        }
        if (!best || !spaced(best->centre, k.villageSpacingMetres))
        {
          continue;
        }
        KingdomSettlement settlement;
        settlement.id = plan.settlements.size() + 1;
        settlement.cell = best->cell;
        settlement.role = SettlementClass::Village;
        settlement.centre = best->centre;
        settlement.score = best->score;
        settlement.population = k.villagePopulation;
        settlement.alongRoad = true;
        settlement.parentTown = link.to;
        if (best->region < world.regions.regions.size())
        {
          settlement.regionName = world.regions.regions[best->region].name;
        }
        plan.settlements.push_back(settlement);
        plan.links.push_back({settlement.id, link.from, settlement.centre, link.a, false});
        plan.links.push_back({settlement.id, link.to, settlement.centre, link.b, false});
        ++placed;
      }
    }
    if (placed < k.roadVillages)
    {
      plan.rejections.push_back("road villages: placed " + std::to_string(placed) + " of " + std::to_string(k.roadVillages) + " along trunk roads");
    }
  }
  if (error)
  {
    error->clear();
  }
  out = std::move(plan);
  return true;
}

GenerationTask worldSettlementSiteGenerator(uint64_t d)
{
  return nativeGenerator<WorldSiteGenerationBrief, WorldSettlementSite>(
      d, "world.settlement-site", "world.site-brief", "world.settlement-site", GenerationScope::Neighbourhood, [](const WorldSiteGenerationBrief &b, WorldSettlementSite &o, std::string *e) {
        if (!b.world)
        {
          if (e)
          {
            *e = "site brief lacks a generated world";
          }
          return false;
        }
        return selectWorldSettlementSite(*b.world, b.settings, o, e);
      });
}

GenerationTask settlementTerritoryBriefGenerator(uint64_t d)
{
  return nativeGenerator<WorldSettlementSite, TerritoryGenerationBrief>(
      d, "settlement.territory-brief", "world.settlement-site", "city.territory-brief", GenerationScope::Neighbourhood, [](const WorldSettlementSite &site, TerritoryGenerationBrief &o, std::string *e) {
        if (!site.brief.terrain || !site.brief.suitability.fingerprint())
        {
          if (e)
          {
            *e = "settlement site carries no composed territory brief";
          }
          return false;
        }
        o = site.brief;
        return true;
      });
}
} // namespace procedural::world::rewrite
