#include "WorldSettlementFabricAdapter.hpp"
#include "generator/CurtainWall.hpp"
#include "generator/PatchTiling.hpp"
#include "generator/SettlementLayout.hpp"
#include "generator/TownLayout.hpp"
#include "procedural/world/settlement/FabricStage.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>

namespace procedural::world::rewrite {
namespace {
using namespace ::procedural::world::generator;
using Poly = std::vector<Position>;

bool housingUse(DistrictUse u)
{
  return u == DistrictUse::Residential || u == DistrictUse::Suburb || u == DistrictUse::Merchant || u == DistrictUse::Craftsmen || u == DistrictUse::WealthyResidential || u == DistrictUse::WorkerHousing;
}

void appendTriangle(SplineFaceMesh &mesh, std::vector<uint32_t> &materials, std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> &index, const std::array<Position, 3> &p, uint32_t material)
{
  std::array<uint32_t, 3> ids;
  for (int k = 0; k < 3; ++k)
  {
    auto key = std::make_tuple(int64_t(std::llround(p[k].x * 1e4)), int64_t(std::llround(p[k].y * 1e4)), int64_t(std::llround(p[k].z * 1e4)));
    auto it = index.find(key);
    if (it == index.end())
    {
      it = index.emplace(key, uint32_t(mesh.vertices.size())).first;
      mesh.vertices.push_back(p[k]);
    }
    ids[k] = it->second;
  }
  const auto a = p[0], b = p[1], c = p[2];
  Position n{(b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y), (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z), (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)};
  double l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
  if (!(l > 1e-12))
  {
    return;
  }
  mesh.triangles.push_back(ids);
  mesh.expectedNormals.push_back({n.x / l, n.y / l, n.z / l});
  materials.push_back(material);
}
} // namespace

JettyDecision decideJetty(const JettyRequest &q)
{
  JettyDecision d;
  d.perFloor.assign(q.floors, 0);
  if (q.institutional || !q.poorOrMiddleHousehold || !q.poorOrMiddleNeighbourhood)
  {
    d.reason = q.institutional ? "institutional buildings never jetty" : !q.poorOrMiddleHousehold ? "wealthy household excluded" : "wealthy neighbourhood excluded";
    return d;
  }
  if (q.floors < 2)
  {
    d.reason = "single storey has no upper floor to project";
    return d;
  }
  d.eligible = true;
  // Deterministic mix: about half of the eligible houses project.
  if (featureSeed(q.seed, 7, q.house, 1) % 2)
  {
    d.reason = "flush front by deterministic mix";
    return d;
  }
  // Clearance: the corridor at upper-floor height between this facade's
  // projection and the accepted projection across the street.
  const double perFloor = q.floors == 2 ? .6 : .45;
  double allowed = std::min(1.0, perFloor * (q.floors - 1));
  const double available = q.streetWidthMetres - q.oppositeJettyMetres - q.minimumCorridorMetres;
  if (available < .2)
  {
    d.reason = "street too narrow for a projection with the corridor kept clear";
    return d;
  }
  allowed = std::min(allowed, available);
  const double step = std::min(perFloor, allowed / (q.floors - 1));
  for (uint32_t f = 1; f < q.floors; ++f)
  {
    d.perFloor[f] = step;
  }
  d.total = step * (q.floors - 1);
  d.jettied = d.total > 1e-6;
  d.reason = d.jettied ? (allowed < perFloor * (q.floors - 1) - 1e-9 ? "projection reduced to keep the street corridor" : "projected") : "no usable projection";
  return d;
}

bool composeSettlementFabric(const WorldSettlementSite &site, const CityTerritory &territory, const SettlementFabricSettings &s, SettlementFabric &out, std::string *error)
{
  auto fail = [&](const std::string &m) {
    if (error)
    {
      *error = m;
    }
    return false;
  };
  if (!site.id || !site.brief.terrain || !site.water || !site.waterSupply || !site.brief.suitability.fingerprint())
  {
    return fail("settlement site is not composed");
  }
  if (territory.revision != site.revision || territory.suitabilityFingerprint != site.brief.suitability.fingerprint())
  {
    return fail("territory does not belong to this site revision");
  }
  if (!std::isfinite(s.maximumTerraceStepMetres) || s.maximumTerraceStepMetres <= 0 || !std::isfinite(s.arteryHalfWidth) || s.arteryHalfWidth < .5 || s.arteryHalfWidth > 20)
  {
    return fail("invalid fabric settings");
  }
  SettlementFabric r;
  r.landforms=site.landforms;
  r.historicalBuildings=site.historicalBuildings;
  r.historicalEnclosures=site.historicalEnclosures;
  r.revision = site.revision;
  const auto &plan = territory.regions;
  if (plan.districts.empty())
  {
    return fail("territory allocated no districts");
  }
  std::map<uint64_t, const CityPlanCell *> cellById;
  for (const auto &cell : site.brief.input.cells)
  {
    cellById[cell.id] = &cell;
  }
  // Urban polygon: allocated pieces plus the circulation cells on the routes.
  std::set<uint64_t> allocated, routeCells;
  std::vector<Poly> tiles;
  for (const auto &d : plan.districts)
  {
    for (auto id : d.cells)
    {
      allocated.insert(id);
    }
  }
  // Gate roads converge on the plaza (the civic anchor at the centre), as
  // Watabou routes every gate to the central patch. Routes run over usable
  // planning cells through shared-edge portals; their cells join the polygon.
  uint64_t plazaCell = site.plazaCell;
  for (const auto &d : plan.districts)
  {
    if (d.use == DistrictUse::Civic && (!plazaCell || d.anchorCell == site.plazaCell))
    {
      plazaCell = d.anchorCell;
    }
  }
  r.plazaCell = plazaCell;
  r.growthCentre = site.localCentre;
  if (const auto anchor=cellById.find(plazaCell);anchor!=cellById.end() && !anchor->second->boundary.empty()) {
    Position centre{};
    for(auto p:anchor->second->boundary){centre.x+=p.x;centre.z+=p.z;}
    centre.x/=anchor->second->boundary.size();centre.z/=anchor->second->boundary.size();
    r.growthCentre=centre;
  }

  struct CellLink
  {
    uint64_t to;
    Position portal;
  };

  std::map<uint64_t, std::vector<CellLink>> adjacency;
  {
    // Compound cells (temple, hall...) are never crossed by a gate road; the
    // plaza itself is the destination.
    std::set<uint64_t> compoundCells;
    for (const auto &d : plan.districts)
    {
      if (!housingUse(d.use) && d.anchorCell != plazaCell)
      {
        for (auto id : d.cells)
        {
          compoundCells.insert(id);
        }
      }
    }
    std::vector<const CityPlanCell *> usableCells;
    for (const auto &cell : site.brief.input.cells)
    {
      if (cell.buildable && cell.slope <= site.brief.settings.planning.maximumSlope && cell.floodRisk <= site.brief.settings.planning.maximumFloodRisk && !compoundCells.count(cell.id))
      {
        usableCells.push_back(&cell);
      }
    }
    for (size_t i = 0; i < usableCells.size(); ++i)
    {
      for (size_t j = i + 1; j < usableCells.size(); ++j)
      {
        std::vector<Position> shared;
        for (auto a : usableCells[i]->boundary)
        {
          for (auto b : usableCells[j]->boundary)
          {
            if (length(a, b) < 1e-6)
            {
              shared.push_back(a);
            }
          }
        }
        if (shared.size() < 2)
        {
          continue;
        }
        Position lo = shared.front(), hi = shared.front();
        for (auto p : shared)
        {
          if (length(p, hi) > length(lo, hi))
          {
            lo = p;
          }
        }
        for (auto p : shared)
        {
          if (length(p, lo) > length(hi, lo))
          {
            hi = p;
          }
        }
        if (length(lo, hi) < site.brief.settings.planning.minimumPortalWidth)
        {
          continue;
        }
        Position portal{(lo.x + hi.x) * .5, 0, (lo.z + hi.z) * .5};
        adjacency[usableCells[i]->id].push_back({usableCells[j]->id, portal});
        adjacency[usableCells[j]->id].push_back({usableCells[i]->id, portal});
      }
    }
  }
  // Village form: linear when the two main gates lie roughly opposite each
  // other, so one through road carries the houses; nucleated otherwise.
  {
    const auto &gates = site.brief.input.gateCells;
    bool linear = s.villageForm == SettlementFabricSettings::VillageForm::Linear;
    using SiteForm = WorldSiteSelectionSettings::VillageForm;
    const bool siteDecides = s.villageForm == SettlementFabricSettings::VillageForm::Auto && site.villageForm != SiteForm::Auto && site.settlementClass <= SettlementClass::Village;
    if (siteDecides)
    {
      linear = site.villageForm == SiteForm::Street && gates.size() >= 2;
    }
    if (!siteDecides && s.villageForm == SettlementFabricSettings::VillageForm::Auto && site.settlementClass <= SettlementClass::Village && gates.size() >= 2 && plazaCell && cellById.count(plazaCell))
    {
      auto centre = vertexMean(cellById[plazaCell]->boundary);
      double widest = 0;
      for (size_t i = 0; i < gates.size(); ++i)
      {
        for (size_t j = i + 1; j < gates.size(); ++j)
        {
          if (!cellById.count(gates[i]) || !cellById.count(gates[j]))
          {
            continue;
          }
          auto a = vertexMean(cellById[gates[i]]->boundary), b = vertexMean(cellById[gates[j]]->boundary);
          double ax = a.x - centre.x, az = a.z - centre.z, bx = b.x - centre.x, bz = b.z - centre.z;
          double angle = std::acos(std::clamp((ax * bx + az * bz) / std::max(1e-9, std::hypot(ax, az) * std::hypot(bx, bz)), -1., 1.));
          widest = std::max(widest, angle);
        }
      }
      linear = widest > 2.1; // Beyond 120 degrees the road runs through rather than around.
    }
    r.linear = linear && site.settlementClass <= SettlementClass::Village;
  }
  auto routeBetween = [&](uint64_t from, uint64_t to, std::vector<Position> &route, std::set<uint64_t> &cells) {
    std::map<uint64_t, std::pair<uint64_t, Position>> parent;
    std::map<uint64_t, double> cost;
    std::set<std::pair<double, uint64_t>> open;
    cost[from] = 0;
    open.insert({0, from});
    while (!open.empty())
    {
      auto [c, id] = *open.begin();
      open.erase(open.begin());
      if (id == to)
      {
        break;
      }
      for (const auto &link : adjacency[id])
      {
        double next = c + length(vertexMean(cellById[id]->boundary), vertexMean(cellById[link.to]->boundary));
        if (!cost.count(link.to) || next < cost[link.to])
        {
          if (cost.count(link.to))
          {
            open.erase({cost[link.to], link.to});
          }
          cost[link.to] = next;
          parent[link.to] = {id, link.portal};
          open.insert({next, link.to});
        }
      }
    }
    if (!cost.count(to))
    {
      return false;
    }
    route = {vertexMean(cellById[to]->boundary)};
    cells.insert(to);
    for (uint64_t at = to; at != from;)
    {
      auto [previous, portal] = parent[at];
      route.push_back(portal);
      route.push_back(vertexMean(cellById[previous]->boundary));
      cells.insert(previous);
      at = previous;
    }
    std::reverse(route.begin(), route.end());
    return true;
  };
  std::vector<std::vector<Position>> gateRoutes; // centre, portal, centre... ending at the plaza cell centre.
  std::vector<uint64_t> gateRouteIds;            // Gate cell of each gate route (0 for a linear branch).
  std::vector<Position> throughRoad;             // Linear form: gate centre ... gate centre.
  std::set<uint64_t> throughCells;               // Linear form: the cells the through road and its branches cross.
  if (r.linear)
  {
    const auto &gates = site.brief.input.gateCells;
    uint64_t first = 0, second = 0;
    double farthest = -1;
    for (size_t i = 0; i < gates.size(); ++i)
    {
      for (size_t j = i + 1; j < gates.size(); ++j)
      {
        if (cellById.count(gates[i]) && cellById.count(gates[j]))
        {
          double d = length(vertexMean(cellById[gates[i]]->boundary), vertexMean(cellById[gates[j]]->boundary));
          if (d > farthest)
          {
            farthest = d;
            first = gates[i];
            second = gates[j];
          }
        }
      }
    }
    std::set<uint64_t> roadCells;
    if (first && second && routeBetween(first, second, throughRoad, roadCells))
    {
      // Houses stand along the road: the road cells and their usable neighbours.
      // A Strassendorf keeps to the road cells; the older linear form also
      // takes the road's usable neighbours.
      const bool roadOnly = site.villageForm == WorldSiteSelectionSettings::VillageForm::Street;
      for (auto id : roadCells)
      {
        routeCells.insert(id);
        throughCells.insert(id);
        if (!roadOnly)
        {
          for (const auto &link : adjacency[id])
          {
            routeCells.insert(link.to);
          }
        }
      }
      for (auto gate : gates)
      {
        if (gate != first && gate != second && cellById.count(gate))
        {
          std::vector<Position> branch;
          std::set<uint64_t> branchCells;
          uint64_t nearest = first;
          double best = INFINITY;
          for (auto id : roadCells)
          {
            if (length(vertexMean(cellById[id]->boundary), vertexMean(cellById[gate]->boundary)) < best)
            {
              best = length(vertexMean(cellById[id]->boundary), vertexMean(cellById[gate]->boundary));
              nearest = id;
            }
          }
          if (routeBetween(gate, nearest, branch, branchCells))
          {
            for (auto id : branchCells)
            {
              routeCells.insert(id);
              throughCells.insert(id);
            }
            gateRoutes.push_back(branch);
            gateRouteIds.push_back(0);
          }
        }
      }
    }
    else
    {
      r.rejections.push_back("linear village: no usable through road between the main gates; nucleated form used");
      r.linear = false;
    }
  }
  // Gates sit where the road from the plaza to each rural exit leaves the
  // urban land: walk the ray from the plaza centre toward the exit through
  // usable cells and take the last usable cell as the gate cell.
  std::vector<std::pair<uint64_t, Position>> gateCells; // gate cell, exit
  if (plazaCell && cellById.count(plazaCell))
  {
    auto plazaCentre = vertexMean(cellById[plazaCell]->boundary);
    for (const auto &src : site.brief.input.ruralSources)
    {
      uint64_t last = plazaCell;
      double dx0 = src.exit.x - plazaCentre.x, dz0 = src.exit.z - plazaCentre.z, len = std::hypot(dx0, dz0);
      if (len < 1e-6)
      {
        continue;
      }
      // A compound beside the plaza can block the straight ray; the road then
      // bends around it, so nearby bearings are tried in widening steps.
      for (double bearing : {0., .2, -.2, .4, -.4, .6, -.6})
      {
        double dx = (dx0 * std::cos(bearing) - dz0 * std::sin(bearing)) / len, dz = (dx0 * std::sin(bearing) + dz0 * std::cos(bearing)) / len;
        for (double t = 2; t < len; t += 2)
        {
          Position q{plazaCentre.x + dx * t, 0, plazaCentre.z + dz * t};
          uint64_t found = 0;
          // The gate sits where the ray leaves the walled core (allocatable
          // cells); circulation-only cells beyond it carry the approach road.
          for (const auto &cell : site.brief.input.cells)
          {
            if (adjacency.count(cell.id) && cell.allocatable && (!r.linear || throughCells.count(cell.id)) && (strictlyInsideConvex(cell.boundary, q) || onRing(cell.boundary, q, 1e-6)))
            {
              found = cell.id; // A linear village's gates lie on its through road.
              break;
            }
          }
          if (!found)
          {
            break;
          }
          last = found;
        }
        if (last != plazaCell)
        {
          break;
        }
      }
      if (last == plazaCell)
      {
        r.rejections.push_back("no usable cell between the plaza and the exit toward rural source " + std::to_string(src.id));
        continue;
      }
      bool duplicate = false;
      for (auto &g : gateCells)
      {
        duplicate |= g.first == last;
      }
      if (!duplicate)
      {
        gateCells.push_back({last, src.exit});
      }
    }
  }
  if (!r.linear)
  {
    for (auto &[gate, exit] : gateCells)
    {
      std::vector<Position> route;
      std::set<uint64_t> cells;
      if (!routeBetween(gate, plazaCell, route, cells))
      {
        r.rejections.push_back("gate cell " + std::to_string(gate) + " has no usable route to the plaza");
        continue;
      }
      for (auto id : cells)
      {
        routeCells.insert(id);
      }
      gateRoutes.push_back(std::move(route));
      gateRouteIds.push_back(gate);
    }
  }
  for (auto id : allocated)
  {
    if (cellById.count(id))
    {
      tiles.push_back(cellById[id]->boundary);
    }
  }
  // Towns and cities wall the whole core (Watabou's nearest patches), not only
  // the cells the districts took: the plaza stays central and the unallocated
  // core cells become infill inside the wall.
  {
    using Form = WorldSiteSelectionSettings::VillageForm;
    const bool ringVillage = site.settlementClass <= SettlementClass::Village && (site.villageForm == Form::Round || site.villageForm == Form::Anger || site.villageForm == Form::Scattered);
    if ((s.convexWalls && site.settlementClass >= SettlementClass::Town) || ringVillage)
    {
      for (const auto &cell : site.brief.input.cells)
      {
        if (cell.allocatable && cell.buildable && !allocated.count(cell.id))
        {
          routeCells.insert(cell.id); // Pushed as a tile by the route-cell loop below; a ring village is its whole ring.
        }
      }
    }
  }
  for (auto id : routeCells)
  {
    if (!allocated.count(id) && cellById.count(id))
    {
      tiles.push_back(cellById[id]->boundary);
      ++r.routeCells;
    }
  }
  std::vector<Poly> outers, holes;
  auto hullOf = [&](const std::vector<Poly> &polys) {
    std::vector<Position> points;
    for (const auto &tile : polys)
    {
      points.insert(points.end(), tile.begin(), tile.end());
    }
    std::sort(points.begin(), points.end(), [](Position a, Position b) {
      return a.x != b.x ? a.x < b.x : a.z < b.z;
    });
    points.erase(
        std::unique(
            points.begin(),
            points.end(),
            [](Position a, Position b) {
              return length(a, b) < 1e-6;
            }),
        points.end());
    Poly hull;
    if (points.size() < 3)
    {
      return hull;
    }
    for (int pass = 0; pass < 2; ++pass)
    {
      size_t base = hull.size();
      for (size_t i = 0; i < points.size(); ++i)
      {
        auto q = pass ? points[points.size() - 1 - i] : points[i];
        while (hull.size() >= base + 2 && cross(hull[hull.size() - 2], hull.back(), q) <= 1e-9)
        {
          hull.pop_back();
        }
        hull.push_back(q);
      }
      if (!hull.empty())
      {
        hull.pop_back();
      }
    }
    return hull;
  };
  if (!unionTiles(tiles, outers, holes))
  {
    if (std::getenv("CITY_DEBUG"))
    {
      // Which directed edges stay unmatched: T-junctions or gaps in the tiling.
      std::map<std::pair<std::pair<long long, long long>, std::pair<long long, long long>>, int> edges;
      auto key = [](Position p) {
        return std::make_pair((long long)std::llround(p.x * 1e6), (long long)std::llround(p.z * 1e6));
      };
      for (const auto &tile : tiles)
      {
        for (size_t i = 0; i < tile.size(); ++i)
        {
          auto a = key(tile[i]), b = key(tile[(i + 1) % tile.size()]);
          if (edges.count({b, a}))
          {
            --edges[{b, a}];
          }
          else
          {
            ++edges[{a, b}];
          }
        }
      }
      size_t unmatched = 0;
      for (auto &[e, c] : edges)
      {
        if (c > 0 && (unmatched++ < 8))
        {
          std::fprintf(stderr, "union: unmatched edge (%.2f,%.2f)-(%.2f,%.2f)\n", e.first.first / 1e6, e.first.second / 1e6, e.second.first / 1e6, e.second.second / 1e6);
        }
      }
      std::fprintf(stderr, "union: %zu tiles (%zu allocated, %zu route/core), %zu unmatched boundary edges\n", tiles.size(), allocated.size(), routeCells.size(), unmatched);
      for (auto id : routeCells)
      {
        if (cellById.count(id) && !cellById[id]->allocatable)
        {
          std::fprintf(stderr, "union: route cell %llu outside the core\n", (unsigned long long)id);
        }
      }
      if (FILE *dump = std::fopen("/tmp/union-tiles.json", "w"))
      {
        std::fprintf(dump, "[");
        for (size_t t = 0; t < tiles.size(); ++t)
        {
          std::fprintf(dump, "%s[", t ? "," : "");
          for (size_t i = 0; i < tiles[t].size(); ++i)
          {
            std::fprintf(dump, "%s[%.9f,%.9f]", i ? "," : "", tiles[t][i].x, tiles[t][i].z);
          }
          std::fprintf(dump, "]");
        }
        std::fprintf(dump, "]");
        std::fclose(dump);
      }
    }
    // Bounded repair: the tiling pinched or its shared edges did not cancel;
    // the convex hull of the urban cells is used and the repair is recorded.
    Poly hull = hullOf(tiles);
    if (hull.size() < 3)
    {
      return fail(
          "urban cells do not form a closed polygon (" + std::to_string(tiles.size()) + " tiles, " + std::to_string(allocated.size()) + " allocated cells, " + std::to_string(cellById.size()) + " planning cells)");
    }
    outers = {hull};
    holes.clear();
    r.rejections.push_back("urban cells did not form a closed polygon; their convex hull is used");
  }
  size_t largest = 0;
  for (size_t i = 1; i < outers.size(); ++i)
  {
    if (signedArea(outers[i]) > signedArea(outers[largest]))
    {
      largest = i;
    }
  }
  r.polygon.outer = outers[largest];
  for (const auto &hole : holes)
  {
    if (insideEvenOdd(r.polygon.outer, vertexMean(hole)))
    {
      Poly h = hole;
      std::reverse(h.begin(), h.end());
      r.polygon.holes.push_back(h);
    }
  }
  if (outers.size() > 1)
  {
    r.rejections.push_back(std::to_string(outers.size() - 1) + " detached urban components were left out of the polygon");
  }
  std::vector<Poly> suburbLand;
  std::vector<Poly> outerWardCells; // Planning cells beyond the wall, offered as outer wards.
  if (s.convexWalls && site.settlementClass >= SettlementClass::Town)
  {
    // Curtain wall through the class-based generator (generator::CurtainWall):
    // Watabou's smoothed circumference, shared vertices moved in every cell,
    // vertices on the leaf boundary pinned, then same-sized straight sections.
    Poly outline = r.polygon.outer;
    if (outline.size() >= 3)
    {
      generator::CurtainWallInput wallInput;
      wallInput.outline = outline;
      for (const auto &cell : site.brief.input.cells)
      {
        wallInput.cells.push_back(cell.boundary);
      }
      for (const auto &region : site.regions)
      {
        wallInput.pinnedRings.push_back(region.boundary);
      }
      // Detailed estates have already passed a footprint-fit gate. Smoothing
      // the city wall must not shrink their reserved land afterward.
      for (const auto &district : plan.districts)
        if (district.use == DistrictUse::Manor || district.use == DistrictUse::Castle || district.use == DistrictUse::Citadel)
          for (const auto &piece : district.pieces) wallInput.pinnedRings.push_back(piece);
      wallInput.coreCells = tiles.size();
      wallInput.sectionMetres = site.settlementClass >= SettlementClass::City ? 95 : 75;
      generator::CurtainWallPlan wallPlan;
      generator::Ledger wallLedger;
      std::string wallError;
      if (!generator::CurtainWall::plan(wallInput, wallPlan, wallLedger, &wallError))
      {
        return fail("curtain wall: " + wallError);
      }
      for (auto &tile : tiles)
      {
        generator::CurtainWall::remap(wallPlan, tile);
      }
      r.polygon.outer = wallPlan.ring;
      r.walled = true;
      r.wallMoves = wallPlan.moves;
      for (const auto &entry : wallLedger.entries())
      {
        r.rejections.push_back(entry.message);
      }
      // The strip between the chords and the cells, and the land around the
      // gates, is outskirts land: the hull of the core is offered as one mask.
      suburbLand.push_back(hullOf(tiles));
      // Unbuildable cells enclosed by the wall are reservations, never holes.
      for (auto hole : r.polygon.holes)
      {
        generator::CurtainWall::remap(wallPlan, hole);
        std::reverse(hole.begin(), hole.end());
        if (hole.size() >= 3 && signedArea(hole) > 25)
        {
          r.reservations.push_back(hole);
        }
      }
      r.polygon.holes.clear();
      for (const auto &cell : site.brief.input.cells)
      {
        bool usable = cell.buildable && cell.slope <= site.brief.settings.planning.maximumSlope && cell.floodRisk <= site.brief.settings.planning.maximumFloodRisk;
        bool inTiles = allocated.count(cell.id) || routeCells.count(cell.id);
        if (inTiles)
        {
          continue;
        }
        auto c = vertexMean(cell.boundary);
        bool inside = insideEvenOdd(wallPlan.ring, c);
        if (!inside && usable)
        {
          bool touches = false;
          for (auto v : cell.boundary)
          {
            touches |= onRing(wallPlan.ring, v, 1e-6) || insideEvenOdd(wallPlan.ring, v);
          }
          if (touches)
          {
            Poly piece = cell.boundary;
            generator::CurtainWall::remap(wallPlan, piece);
            suburbLand.push_back(piece);
          }
        }
      }
      // Beyond the urban leaf only hinterland regions exist: open regions that
      // touch the outline and carry no farm allocation are offered as outer-ward
      // land as well (Watabou's gate suburbs and outer farms sit there).
      {
        std::set<uint64_t> farmed;
        for (const auto &patch : territory.rural.patches)
        {
          farmed.insert(patch.originalRegion);
        }
        // Two rings of open regions beyond the wall: those touching the outline and
        // those touching them, so outer wards can grow well beyond the gate.
        std::vector<Poly> ring1;
        for (const auto &region : site.regions)
        {
          if (region.cover != rewrite::RuralLandCover::Open || farmed.count(region.id))
          {
            continue;
          }
          // The site adapter's suburb belt is outer-ward land by definition;
          // other open, unfarmed regions qualify when they touch the wall.
          bool touches = region.reserved;
          for (auto v : region.boundary)
          {
            touches |= onRing(wallPlan.ring, v, 1e-6) || insideEvenOdd(wallPlan.ring, v);
          }
          for (auto v : wallPlan.ring)
          {
            touches |= strictlyInsideConvex(region.boundary, v) || onRing(region.boundary, v, 1e-6);
          }
          if (touches)
          {
            ring1.push_back(region.boundary);
          }
        }
        for (const auto &region : site.regions)
        {
          if (region.cover != rewrite::RuralLandCover::Open || farmed.count(region.id))
          {
            continue;
          }
          bool second = false;
          for (const auto &first : ring1)
          {
            int shared = 0;
            for (auto a : first)
            {
              for (auto b : region.boundary)
              {
                shared += length(a, b) < 1e-6;
              }
            }
            second |= shared >= 2;
          }
          bool already = false;
          for (const auto &first : ring1)
          {
            already |= first.size() == region.boundary.size() && length(first[0], region.boundary[0]) < 1e-9;
          }
          if (second && !already)
          {
            suburbLand.push_back(region.boundary);
          }
        }
        suburbLand.insert(suburbLand.end(), ring1.begin(), ring1.end());
      }
      // The outskirts are planning cells too: two rings of usable cells beyond
      // the wall (touching it, and touching those) are the outer wards, rung
      // like villages along the gate roads. They are offered as outer land too.
      {
        auto remapped = [&](Poly cell) {
          for (auto &p : cell)
          {
            for (const auto &[from, to] : r.wallMoves)
            {
              if (length(p, from) < 1e-6)
              {
                p = to;
                break;
              }
            }
          }
          return cell;
        };
        // The site adapter chose the outskirts cells (nearest the core, up to
        // a share of its area); every usable one beyond the core is an outer ward.
        std::vector<Poly> outer;
        for (const auto &cell : site.brief.input.cells)
        {
          bool usable = cell.buildable && cell.slope <= site.brief.settings.planning.maximumSlope && cell.floodRisk <= site.brief.settings.planning.maximumFloodRisk;
          if (cell.outskirts && usable && !allocated.count(cell.id) && !routeCells.count(cell.id))
          {
            outer.push_back(remapped(cell.boundary));
          }
        }
        for (const auto &piece : outer)
        {
          outerWardCells.push_back(piece);
          bool already = false;
          for (const auto &mask : suburbLand)
          {
            already |= mask.size() == piece.size() && length(mask[0], piece[0]) < 1e-9;
          }
          if (!already)
          {
            suburbLand.push_back(piece);
          }
        }
      }
      // The layout accepts at most 32 suburb masks: keep those nearest the plaza
      // (the gates lie between them and the centre), in id order for determinism.
      if (suburbLand.size() > 96)
      {
        std::stable_sort(suburbLand.begin(), suburbLand.end(), [&](const Poly &a, const Poly &b) {
          return length(vertexMean(a), site.localCentre) < length(vertexMean(b), site.localCentre);
        });
        suburbLand.resize(96);
      }
    }
  }
  auto insidePolygon = [&](Position p) {
    if (onRing(r.polygon.outer, p, 1e-6))
    {
      return true;
    }
    if (!insideEvenOdd(r.polygon.outer, p))
    {
      return false;
    }
    for (const auto &hole : r.polygon.holes)
    {
      if (insideEvenOdd(hole, p) && !onRing(hole, p, 1e-6))
      {
        return false;
      }
    }
    return true;
  };
  std::vector<CityPlanDistrict> districts;
  for (auto d : plan.districts)
  {
    // The curtain wall moved shared vertices: district pieces follow them.
    for (auto &piece : d.pieces)
    {
      for (auto &p : piece)
      {
        for (const auto &[from, to] : r.wallMoves)
        {
          if (length(p, from) < 1e-6)
          {
            p = to;
            break;
          }
        }
      }
    }
    bool inside = true;
    for (const auto &piece : d.pieces)
    {
      inside &= insidePolygon(vertexMean(piece));
    }
    if (inside)
    {
      districts.push_back(d);
    }
    else
    {
      r.rejections.push_back("district " + std::to_string(d.id) + " (" + districtUseName(d.use) + ") lies outside the connected urban polygon");
    }
  }
  // Terrace: area-weighted mean of the actual surface over the urban cells,
  // accepted only when the largest cut or fill stays within the budget.
  //
  // Unless the town is meant to stand on a hill. One plane for a whole town is
  // a 2.5 m budget over several hundred metres - a third of a percent of
  // grade, which is to say flat - and it rejected every site with any relief
  // at all. With a relief amplitude the terrace is not taken: the surface is
  // the world's plus the hill, the street network is graded down it and each
  // lot terraces off the streets it fronts, in `WorldCityRelief`.
  const bool hill = !site.landforms.cells.empty() || s.raisedDistricts || s.relief.amplitudeMetres > 0 || !s.terraceWholeTown;
  double urbanReach = 0;

  for (const auto &v : r.polygon.outer)
  {
    urbanReach = std::max(urbanReach, std::hypot(v.x - site.localCentre.x, v.z - site.localCentre.z));
  }

  const std::function<double(double, double)> base =
      !site.landforms.cells.empty() ? site.brief.terrain : hill ? cityReliefSurface(
                 site.brief.terrain,
                 s.relief,
                 site.localCentre.x,
                 site.localCentre.z,
                 std::max(60., urbanReach),
                 std::max(40., s.relief.wavelengthMetres * .6))
           : site.brief.terrain;

  if (hill)
  {
    r.terrain = base;
  }

  double weight = 0, sum = 0, step = 0;
  for (const auto &tile : tiles)
  {
    double a = std::abs(signedArea(tile));
    auto c = vertexMean(tile);
    sum += a * base(c.x, c.z);
    weight += a;
  }
  r.terraceLevel = weight > 0 ? sum / weight : base(site.localCentre.x, site.localCentre.z);
  for (const auto &tile : tiles)
  {
    for (auto v : tile)
    {
      step = std::max(step, std::abs(base(v.x, v.z) - r.terraceLevel));
    }
    auto c = vertexMean(tile);
    step = std::max(step, std::abs(base(c.x, c.z) - r.terraceLevel));
  }
  if (r.walled)
  {
    for (const auto &cell : site.brief.input.cells)
    {
      if (insideEvenOdd(r.polygon.outer, vertexMean(cell.boundary)))
      {
        for (auto v : cell.boundary)
        {
          step = std::max(step, std::abs(base(v.x, v.z) - r.terraceLevel));
        }
      }
    }
  }
  if (!hill)
  {
    // Suburb land that would need more cut/fill than the budget is dropped.
    // On a hill there is no budget to exceed: the suburb stands on the ground
    // like everything else.
    std::vector<Poly> level;
    for (const auto &piece : suburbLand)
    {
      double pieceStep = 0;
      for (auto v : piece)
      {
        pieceStep = std::max(pieceStep, std::abs(base(v.x, v.z) - r.terraceLevel));
      }
      if (pieceStep <= s.maximumTerraceStepMetres)
      {
        level.push_back(piece);
      }
    }
    if (level.size() != suburbLand.size())
    {
      r.rejections.push_back(std::to_string(suburbLand.size() - level.size()) + " suburb cells dropped: beyond the terrace cut/fill budget");
    }
    suburbLand = std::move(level);
  }
  r.terraceStep = step;
  r.terraceAccepted = hill || step <= s.maximumTerraceStepMetres;

  if (hill)
  {
    // The surface is already published - it is the hill - so nothing here
    // flattens anything. The level stays as the datum the consumers that
    // still want one read; the relief plan grades the streets and terraces
    // the lots once the skeleton exists to grade.
    r.rejections.push_back(
        "settlement stands on its relief: " + std::to_string(step) + " m of fall across the urban polygon, no whole-town terrace taken");
  }
  else if (!r.terraceAccepted)
  {
    r.rejections.push_back("urban terrace needs " + std::to_string(step) + " m of cut/fill, above the " + std::to_string(s.maximumTerraceStepMetres) + " m budget; no streets or houses were built");
    r.terrain = base;
    out = std::move(r);
    return true;
  }
  if (!hill)
  {
    auto outer = r.polygon.outer;
    auto holesCopy = r.polygon.holes;
    auto suburbs = suburbLand;
    double level = r.terraceLevel;
    const double skirt = std::max(1.0, s.terraceSkirtMetres);
    r.terrain = [outer, holesCopy, suburbs, level, skirt, base](double x, double z) {
      Position p{x, 0, z};
      bool in = onRing(outer, p, 1e-6) || insideEvenOdd(outer, p);
      for (const auto &hole : holesCopy)
      {
        if (in && insideEvenOdd(hole, p) && !onRing(hole, p, 1e-6))
        {
          in = false;
        }
      }
      for (const auto &piece : suburbs)
      {
        if (!in && (strictlyInsideConvex(piece, p) || onRing(piece, p, 1e-6)))
        {
          in = true;
        }
      }
      if (in)
      {
        return level;
      }

      // Outside, the terrace is graded back into the ground over a skirt
      // rather than dropped off a wall.
      //
      // This returned base(x, z) the instant the point left the polygon, so the
      // terraced surface stepped by the whole cut or fill - up to the 2.5 m the
      // terrace budget allows - across zero distance. A surface arrangement
      // stores one height per vertex, so it cannot hold a vertical wall: a
      // vertex on the terrace edge took the terrace's height while the terrain
      // face beside it needed the ground's, and every terrain-following job
      // downstream failed "frozen boundary needs upstream stations" against a
      // ten centimetre tolerance.
      //
      // Stationing cannot repair that, because subdividing a step leaves a
      // step: measured, an edge cut from 2.70 m down to 0.17 m kept the same
      // 0.62 m error. A real terrace has an embankment, so the fix is to build
      // one. The interior stays exactly flat, which is what the street layout
      // needs; only the ground outside ramps, over a distance the surface can
      // follow.
      double distance = polygonDistance(p, outer);
      for (const auto &hole : holesCopy)
      {
        distance = std::min(distance, polygonDistance(p, hole));
      }
      for (const auto &piece : suburbs)
      {
        distance = std::min(distance, polygonDistance(p, piece));
      }

      const double ground = base(x, z);

      if (!std::isfinite(distance) || distance >= skirt)
      {
        return ground;
      }

      const double t = std::clamp(distance / skirt, 0.0, 1.0);

      return level + (ground - level) * (t * t * (3.0 - 2.0 * t));
    };
  }
  // Gates and fixed arteries from the master plan's connected gate tree.
  std::set<std::pair<TilingKey, TilingKey>> hopKeys;
  uint64_t arteryId = 1000;
  auto addHop = [&](Position a, Position b) {
    if (length(a, b) < 1)
    {
      return;
    }
    // std::minmax returns a pair of *references to its arguments*. Storing it
    // from two temporaries leaves the pair dangling at the end of the
    // statement, so the lookup on the next line compared freed memory and
    // discarded hops that were never duplicates - which is how the gate tree
    // came apart and the layout reported a disconnected fixed artery.
    const TilingKey first = tilingKey(a);
    const TilingKey second = tilingKey(b);
    const std::pair<TilingKey, TilingKey> key = first < second ? std::make_pair(first, second) : std::make_pair(second, first);

    if (!hopKeys.insert(key).second)
    {
      return;
    }
    r.arteries.push_back({++arteryId, a, b, s.arteryHalfWidth, true});
  };
  std::vector<const CityPlanDistrict *> compounds;
  for (const auto &d : districts)
  {
    if (!housingUse(d.use))
    {
      compounds.push_back(&d);
    }
  }
  // The connected gate tree (root gate cell -> every gate cell) is the fixed
  // circulation; district access inside the village comes from the layout's
  // own street net, so housing districts add no further fixed arteries.
  // Routes alternate cell centre, portal, cell centre. A chord between two
  // consecutive centres crosses their shared edge once and stays inside the
  // union of the two convex cells, so the portal points can be dropped.
  // The shared edge can be short and the cells skewed, in which case the
  // chord leaves both cells; then the portal point is kept for that hop.
  auto cellOf = [&](Position p) -> const Poly * {
    for (const auto &tile : tiles)
    {
      if (strictlyInsideConvex(tile, p) || onRing(tile, p, 1e-6))
      {
        return &tile;
      }
    }
    return nullptr;
  };
  auto centresOnly = [&](const std::vector<Position> &route) {
    std::vector<Position> out;
    if (route.empty())
    {
      return out;
    }
    out.push_back(route[0]);
    for (size_t i = 2; i < route.size(); i += 2)
    {
      auto a = route[i - 2], b = route[i];
      const Poly *ca = cellOf(a), *cb = cellOf(b);
      bool chord = ca && cb;
      for (int k = 1; chord && k < 10; ++k)
      {
        Position q{a.x + (b.x - a.x) * k * .1, 0, a.z + (b.z - a.z) * k * .1};
        chord = strictlyInsideConvex(*ca, q) || strictlyInsideConvex(*cb, q) || onRing(*ca, q, 1e-6) || onRing(*cb, q, 1e-6);
      }
      if (!chord)
      {
        out.push_back(route[i - 1]);
      }
      out.push_back(b);
    }
    return out;
  };
  if (r.linear && throughRoad.size() >= 3)
  {
    auto pts = centresOnly(throughRoad);
    for (size_t i = 1; i < pts.size(); ++i)
    {
      addHop(pts[i - 1], pts[i]);
    }
  }
  // Gate roads end at the plaza cell's boundary portal; a street around the
  // plaza connects them and frames the square.
  std::map<uint64_t, Position> gateAnchor; // Watabou streets: the gate cell corner the road leaves from.
  if (s.watabouStreets && !r.linear && plazaCell && cellById.count(plazaCell))
  {
    // Watabou's topology.buildPath: the road from a gate to the plaza runs
    // along the cell edges (vertex graph of the core cells), so no cell is
    // crossed and every block stays a whole patch.
    std::vector<Position> vertices;
    std::map<TilingKey, size_t> vertexIds;
    auto vertexOf = [&](Position p) {
      auto key = tilingKey(p);
      auto it = vertexIds.find(key);
      if (it != vertexIds.end())
      {
        return it->second;
      }
      vertexIds[key] = vertices.size();
      vertices.push_back(p);
      return vertices.size() - 1;
    };
    // Edges usable by a road: inside the wall ring, and not along a compound
    // (the road ribbon would cross the reservation); the plaza's own ring is
    // the destination and stays.
    auto roadEdge = [&](Position a, Position b) {
      const Position mid{(a.x + b.x) * .5, 0, (a.z + b.z) * .5};
      if (!insidePolygon(mid) || onRing(r.polygon.outer, mid, 1e-6) || !insidePolygon(a) || !insidePolygon(b))
      {
        return false;
      }
      for (const auto *d : compounds)
      {
        if (d->anchorCell != plazaCell)
        {
          for (const auto &piece : d->pieces)
          {
            if (onRing(piece, mid, 1e-6) || strictlyInsideConvex(piece, mid))
            {
              return false;
            }
          }
        }
      }
      return true;
    };
    std::vector<std::vector<std::pair<size_t, double>>> graph;
    for (const auto &tile : tiles)
    {
      for (size_t k = 0; k < tile.size(); ++k)
      {
        const Position u = tile[k], v = tile[(k + 1) % tile.size()];
        if (!roadEdge(u, v))
        {
          continue;
        }
        size_t a = vertexOf(u), b = vertexOf(v);
        graph.resize(vertices.size());
        double routeCost=length(u,v);
        if (!r.landforms.cells.empty() && r.terrain)
          routeCost=cityLandformRouteCost(u,v,r.terrain,s.reliefPlan.maximumStreetGrade);
        graph[a].push_back({b, routeCost});
        graph[b].push_back({a, routeCost});
      }
    }
    graph.resize(vertices.size());
    const auto &plazaRing = cellById[plazaCell]->boundary;
    for (const auto &[gateId, exitPoint] : gateCells)
    {
      if (!cellById.count(gateId))
      {
        continue;
      }
      // Source: the gate cell corner nearest the exit that is not on the wall.
      size_t source = SIZE_MAX;
      double nearest = INFINITY;
      for (auto v : cellById[gateId]->boundary)
      {
        if (!onRing(r.polygon.outer, v, 1e-6) && length(v, exitPoint) < nearest)
        {
          nearest = length(v, exitPoint);
          source = vertexOf(v);
        }
      }
      if (source == SIZE_MAX)
      {
        continue;
      }
      // Target: the plaza corner nearest the source (Watabou: plaza.shape.min).
      size_t target = SIZE_MAX;
      double best = INFINITY;
      for (auto v : plazaRing)
      {
        if (length(v, vertices[source]) < best)
        {
          best = length(v, vertices[source]);
          target = vertexOf(v);
        }
      }
      graph.resize(vertices.size());
      std::vector<double> cost(vertices.size(), INFINITY);
      std::vector<size_t> via(vertices.size(), SIZE_MAX);
      std::set<std::pair<double, size_t>> open;
      cost[source] = 0;
      open.insert({0, source});
      while (!open.empty())
      {
        auto [c, i] = *open.begin();
        open.erase(open.begin());
        if (i == target)
        {
          break;
        }
        for (auto [j, w] : graph[i])
        {
          if (c + w < cost[j])
          {
            open.erase({cost[j], j});
            cost[j] = c + w;
            via[j] = i;
            open.insert({cost[j], j});
          }
        }
      }
      if (!std::isfinite(cost[target]))
      {
        continue;
      }
      for (size_t i = target; via[i] != SIZE_MAX; i = via[i])
      {
        addHop(vertices[via[i]], vertices[i]);
      }
      gateAnchor[gateId] = vertices[source];
    }
  }
  for (size_t g = 0; g < gateRoutes.size(); ++g)
  {
    const auto &route = gateRoutes[g];
    if (route.size() < 3)
    {
      continue;
    }
    if (g < gateRouteIds.size() && gateAnchor.count(gateRouteIds[g]))
    {
      continue; // This gate has its edge path.
    }
    std::vector<Position> trimmed(route.begin(), route.end() - 2); // Drop the plaza centre and its portal.
    auto pts = centresOnly(trimmed);
    pts.push_back(route[route.size() - 2]);
    for (size_t i = 1; i < pts.size(); ++i)
    {
      addHop(pts[i - 1], pts[i]);
    }
  }
  if (!r.linear && plazaCell && cellById.count(plazaCell))
  {
    const auto &ring = cellById[plazaCell]->boundary;
    for (size_t i = 0; i < ring.size(); ++i)
    {
      addHop(ring[i], ring[(i + 1) % ring.size()]);
    }
  }
  if (gateRoutes.empty())
  {
    for (const auto &route : plan.gateConnections)
    {
      auto pts = centresOnly(route);
      for (size_t i = 1; i < pts.size(); ++i)
      {
        addHop(pts[i - 1], pts[i]);
      }
    }
  }
  std::vector<std::pair<uint64_t, Position>> gateList = gateCells;
  if (gateList.empty())
  {
    for (auto id : site.brief.input.gateCells)
    {
      Position exit;
      double nearest = INFINITY;
      for (const auto &src : site.brief.input.ruralSources)
      {
        if (cellById.count(id) && length(src.exit, vertexMean(cellById[id]->boundary)) < nearest)
        {
          nearest = length(src.exit, vertexMean(cellById[id]->boundary));
          exit = src.exit;
        }
      }
      gateList.push_back({id, exit});
    }
  }
  for (size_t k = 0; k < gateList.size(); ++k)
  {
    auto id = gateList[k].first;
    if (!cellById.count(id) || !insidePolygon(vertexMean(cellById[id]->boundary)))
    {
      continue;
    }
    auto centre = gateAnchor.count(id) ? gateAnchor[id] : vertexMean(cellById[id]->boundary);
    Position exit = gateList[k].second;
    Position gate;
    double bestT = INFINITY;
    const auto &ring = r.polygon.outer;
    for (size_t i = 0; i < ring.size(); ++i)
    {
      auto a = ring[i], b = ring[(i + 1) % ring.size()];
      Position d{exit.x - centre.x, 0, exit.z - centre.z}, e{b.x - a.x, 0, b.z - a.z}, w{a.x - centre.x, 0, a.z - centre.z};
      double denom = d.x * e.z - d.z * e.x;
      if (std::abs(denom) < 1e-12)
      {
        continue;
      }
      double t = (w.x * e.z - w.z * e.x) / denom, u = (w.x * d.z - w.z * d.x) / denom;
      if (t > 1e-9 && u >= -1e-9 && u <= 1 + 1e-9 && t < bestT)
      {
        bestT = t;
        u = std::clamp(u, 0., 1.);
        gate = {a.x + e.x * u, 0, a.z + e.z * u};
      }
    }
    if (!std::isfinite(bestT))
    {
      double best = INFINITY;
      for (size_t i = 0; i < ring.size(); ++i)
      {
        auto a = ring[i], b = ring[(i + 1) % ring.size()];
        double dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz;
        double u = l2 > 0 ? std::clamp(((exit.x - a.x) * dx + (exit.z - a.z) * dz) / l2, 0., 1.) : 0;
        Position q{a.x + dx * u, 0, a.z + dz * u};
        if (length(q, exit) < best)
        {
          best = length(q, exit);
          gate = q;
        }
      }
    }
    if (length(gate, centre) < 1)
    {
      r.rejections.push_back("gate cell " + std::to_string(id) + " touches the polygon edge too closely for an approach");
      continue;
    }
    r.arteries.push_back({++arteryId, gate, centre, s.arteryHalfWidth, true});
    r.gates.push_back({uint64_t(k + 1), gate, arteryId});
    r.approaches.push_back({gate, exit});
  }
  std::set<uint64_t> released;
  for (const auto &hop : r.arteries)
  {
    for (auto *d : compounds)
    {
      if (plazaCell && d->anchorCell == plazaCell && d->use == DistrictUse::Temple && site.villageForm == WorldSiteSelectionSettings::VillageForm::Round)
      {
        continue; // A church on the green (Runddorf) is framed by the ring street, never crossed.
      }
      for (const auto &piece : d->pieces)
      {
        for (double t : {.25, .5, .75})
        {
          if (strictlyInsideConvex(piece, {hop.a.x + (hop.b.x - hop.a.x) * t, 0, hop.a.z + (hop.b.z - hop.a.z) * t}))
          {
            released.insert(d->id);
          }
        }
      }
    }
  }
  for (auto id : released)
  {
    r.rejections.push_back("compound district " + std::to_string(id) + " is crossed by a gate route and was released to ordinary land");
  }
  r.releasedCompounds.assign(released.begin(), released.end());
  if (std::getenv("CITY_DEBUG"))
  {
    for (const auto &a : r.arteries)
    {
      std::fprintf(stderr, "fabric artery %llu (%.1f,%.1f)-(%.1f,%.1f)\n", (unsigned long long)a.id, a.a.x, a.a.z, a.b.x, a.b.z);
    }
    for (const auto &g : r.gates)
    {
      std::fprintf(stderr, "fabric gate %llu at (%.1f,%.1f) road %llu\n", (unsigned long long)g.id, g.position.x, g.position.z, (unsigned long long)g.externalRoad);
    }
    for (const auto &q : r.reservations)
    {
      std::fprintf(stderr, "fabric reservation %zu vertices near (%.1f,%.1f)\n", q.size(), vertexMean(q).x, vertexMean(q).z);
    }
    std::fprintf(stderr, "fabric polygon %zu vertices, %zu holes, tiles %zu, allocated %zu, route cells %zu\n", r.polygon.outer.size(), r.polygon.holes.size(), tiles.size(), allocated.size(), routeCells.size());
    for (const auto &[gateId, exitPoint] : gateCells)
    {
      std::fprintf(
          stderr,
          "fabric gate cell %llu allocated=%d route=%d centre (%.1f,%.1f)\n",
          (unsigned long long)gateId,
          int(allocated.count(gateId)),
          int(routeCells.count(gateId)),
          vertexMean(cellById[gateId]->boundary).x,
          vertexMean(cellById[gateId]->boundary).z);
    }
    for (const auto &d : districts)
    {
      std::fprintf(stderr, "fabric district %llu %s pieces %zu\n", (unsigned long long)d.id, districtUseName(d.use), d.pieces.size());
    }
  }
  if (r.gates.empty())
  {
    r.rejections.push_back("no gate could be placed on the urban polygon; no streets or houses were built");
    out = std::move(r);
    return true;
  }
  // Street and house layout on the terrace.
  CitySettings city = s.city;
  if (s.classGrammar)
  {
    // Authored grammar per settlement class: organic lanes for hamlets and
    // villages, packed planned blocks with walls for towns, cities and capitals.
    switch (site.settlementClass)
    {
    case SettlementClass::Hamlet:
      city.recipe = CityRecipe::OrganicVoronoi;
      city.siteSpacing = 70;
      city.maximumSites = 8;
      city.targetBlockArea = 3000;
      break;
    case SettlementClass::Village:
      city.recipe = CityRecipe::OrganicVoronoi;
      city.siteSpacing = 60;
      city.maximumSites = 16;
      city.targetBlockArea = 2400;
      break;
    case SettlementClass::Town:
      // Watabou wards: relaxed Voronoi patches whose edges are the streets;
      // lots come from the block bisection (createAlleys), never a grid.
      city.recipe = CityRecipe::OrganicVoronoi;
      city.siteSpacing = 48;
      city.siteRelaxations = 3;
      city.alleyAccess = true;
      city.mixedBlockGrammar = true;
      city.laneHierarchy = true; // Ward edges are lanes; gate-to-gate paths are district streets.
      city.watabouLots = true;   // Lots by createAlleys; the block type sets chaos and emptiness.
      city.gridChaos = .5;
      city.maximumBlockSides = 5;  // Six-sided wards are cut along their best chord (the plaza is a reservation, not a ward).
      city.maximumBlockDepth = 34; // Two lot rows: every lot fronts a street.
      city.setback = .6;           // Houses hug the street.
      city.blockHousing = true;
      city.targetBlockArea = 3600; // A ward larger than this is cut into blocks.
      city.targetPlotArea = 95;
      city.streetHalfWidth = 2.2;
      city.walls = true;
      city.wallSetback = 6;
      city.wallCornerInset = 30;   // Rounded wall: corners are faceted twice.
      city.wallSectionLength = 75; // Same-sized wall sections joined by towers.
      city.maximumSites = 64;
      city.plannedDistrictTurnRadians = .18; // Districts turn their block axes, as the reference divisions vary orientation.
      city.plannedAxisRadians = double(site.id % 628) / 100;
      break;
    default:
      city.recipe = CityRecipe::OrganicVoronoi;
      city.siteSpacing = 44;
      city.siteRelaxations = 3;
      city.alleyAccess = true;
      city.mixedBlockGrammar = true;
      city.laneHierarchy = true;
      city.watabouLots = true;
      city.gridChaos = .5;
      city.maximumBlockSides = 5;
      city.maximumBlockDepth = 30;
      city.setback = .6;
      city.blockHousing = true;
      city.targetBlockArea = 3000;
      city.targetPlotArea = 70;
      city.streetHalfWidth = 2.4;
      city.walls = true;
      city.wallSetback = 6;
      city.wallCornerInset = 40;
      city.wallSectionLength = 95;
      city.maximumSites = 96;
      city.maximumObjects = 4000;
      city.plannedDistrictTurnRadians = .22;
      city.plannedAxisRadians = double(site.id % 628) / 100;
      break;
    }
  }
  if (s.lotsOnly || (s.streetFirstBlocks && site.settlementClass>=SettlementClass::Town))
  {
    city.watabouLots = true;
    city.lotsOnly = true;
    city.blockHousing = true; // Villages take their lots from the blocks too.
  }
  if (city.watabouLots && site.settlementClass <= SettlementClass::Village)
  {
    city.roadRowLots = true;       // Villages: simple houses along the road, gardens behind.
    city.targetBlockArea = 100000; // Village wards are never cut into blocks.
  }
  if (city.watabouLots)
  {
    // Houses per hectare by wealth: packed poor quarters, roomy estates.
    city.housesPerHectare[unsigned(DistrictUse::Suburb)] = 150;
    city.housesPerHectare[unsigned(DistrictUse::WorkerHousing)] = 90;
    city.housesPerHectare[unsigned(DistrictUse::Residential)] = 80;
    city.housesPerHectare[unsigned(DistrictUse::Craftsmen)] = 55;
    city.housesPerHectare[unsigned(DistrictUse::Merchant)] = 65;
    city.housesPerHectare[unsigned(DistrictUse::WealthyResidential)] = 10;
    city.housesPerHectare[unsigned(DistrictUse::Manor)] = 4;
    // Watabou, faithfully: the settlement's planning cells are the wards and
    // the blocks (patch = ward = block), streets are their straight edges,
    // and the lots come from createAlleys on each whole ward. No ward is cut
    // by area, sides or depth, so no sliver block exists to make a bad lot.
    for (const auto &cell : site.brief.input.cells)
    {
      if (cell.allocatable && cell.buildable && (allocated.count(cell.id) || routeCells.count(cell.id)))
      {
        Poly ward = cell.boundary;
        for (auto &p : ward)
        {
          for (const auto &[from, to] : r.wallMoves)
          {
            if (length(p, from) < 1e-6)
            {
              p = to;
              break;
            }
          }
        }
        city.wardCells.push_back(ward);
      }
    }
    // Outer cells that survived the terrace budget are the outer wards.
    for (const auto &piece : outerWardCells)
    {
      bool level = false;
      for (const auto &mask : suburbLand)
      {
        level |= mask.size() == piece.size() && length(mask[0], piece[0]) < 1e-9;
      }
      if (level)
      {
        city.outerWardCells.push_back(piece);
      }
    }
    city.wardBuildings = true; // Watabou's createAlleys per ward, filterOutskirts beyond the wall.
    city.streetCurvature = 0;
    city.maximumBlockSides = 0;
    city.maximumBlockDepth = 0;
    city.frontageStrips = false;
    r.rejections.push_back("watabou wards: " + std::to_string(city.wardCells.size()) + " planning cells are the blocks, " + std::to_string(city.outerWardCells.size()) + " outer cells the outskirts");
  }
  if (s.recipeOverride >= 0 && s.recipeOverride <= 3)
  {
    city.recipe = CityRecipe(s.recipeOverride);
    if (s.patternOverride >= 0)
    {
      city.lsystemPattern = uint8_t(std::min(3, s.patternOverride));
    }
    r.rejections.push_back("recipe override: " + std::string(city.recipe == CityRecipe::LSystemRoads ? "Parish-Muller L-system roads" : city.recipe == CityRecipe::PlannedBlocks ? "planned blocks" : "organic wards"));
  }
  if (r.walled && city.walls && city.blockHousing)
  {
    city.suburbLand = suburbLand;
    // Outskirts grow along the gate roads and their side roads, far beyond
    // the gates: the poorest, most organic fabric.
    city.suburbRoadLength = 200;
    city.suburbGrowthRadius = 120;
    city.suburbCellSpacing = 34;
    city.maximumSuburbWards = 60;
    r.rejections.push_back("suburb land offered outside the wall: " + std::to_string(suburbLand.size()) + " cells");
  }
  if (s.variantSeed)
  {
    // Variant grammar: bounded jitter of the class recipe so several distinct
    // layouts can be generated for the same site and compared.
    auto unit = [&](uint64_t k) {
      uint64_t h = (s.variantSeed + 1) * 0x9E3779B97F4A7C15ULL ^ (k + 1) * 0xBF58476D1CE4E5B9ULL;
      h ^= h >> 31;
      h *= 0x94D049BB133111EBULL;
      h ^= h >> 29;
      return double(h % 1000000) / 1000000.;
    };
    city.targetBlockArea *= .8 + .5 * unit(1);
    city.targetPlotArea *= .85 + .4 * unit(2);
    city.plannedAxisRadians = 6.28 * unit(3);
    city.plannedDistrictTurnRadians = .06 + .28 * unit(4);
    city.streetCurvature *= .4 + 1.2 * unit(5);
    city.siteSpacing *= .85 + .3 * unit(7);
    if (city.walls)
    {
      city.wallCornerInset = 12 + 36 * unit(6);
    }
    city.lsystemPattern = uint8_t(unit(12) * 4);
    if (!city.suburbLand.empty())
    {
      city.suburbRoadLength = 140 + 60 * unit(8);
      city.suburbGrowthRadius = 80 + 70 * unit(9);
      city.suburbCellSpacing = 26 + 16 * unit(10);
    }
    r.rejections.push_back(
        "variant " + std::to_string(s.variantSeed) + ": block " + std::to_string(int(city.targetBlockArea)) + " m2, plot " + std::to_string(int(city.targetPlotArea)) + " m2, wall inset " +
        std::to_string(int(city.wallCornerInset)) + " m, outskirts road " + std::to_string(int(city.suburbRoadLength)) + " m");
  }
  city.seed = site.id ^ 0x9E3779B97F4A7C15ULL;
  city.revision = site.revision;
  city.housingAreas.clear();
  city.districts.clear();
  for (const auto &d : districts)
  {
    if (!released.count(d.id))
    {
      city.districts.push_back(d);
    }
  }
  std::string layoutError;
  const bool villageTown = city.wardBuildings && !r.walled && site.settlementClass <= SettlementClass::Village;
  if (s.roadSettlement)
  {
    // Roads first (docs/architecture/settlement-generator.md): the whole
    // usable leaf is the region, water cells are obstacles, the rural exits
    // pull the main roads; districts, lots and buildings follow the roads.
    generator::SettlementLayoutInput settlement;
    std::vector<Poly> usableCells, waterCells;
    for (const auto &cell : site.brief.input.cells)
    {
      const bool usable = cell.buildable && cell.slope <= site.brief.settings.planning.maximumSlope && cell.floodRisk <= site.brief.settings.planning.maximumFloodRisk;
      (usable ? usableCells : waterCells).push_back(cell.boundary);
    }
    std::vector<Poly> outers, holes;
    if (generator::unionTiles(usableCells, outers, holes) && !outers.empty())
    {
      size_t largest = 0;
      for (size_t i = 1; i < outers.size(); ++i)
      {
        if (std::abs(signedArea(outers[i])) > std::abs(signedArea(outers[largest])))
        {
          largest = i;
        }
      }
      settlement.region = outers[largest];
      if (signedArea(settlement.region) < 0)
      {
        std::reverse(settlement.region.begin(), settlement.region.end());
      }
      for (auto &hole : holes)
      {
        settlement.water.push_back(hole);
      }
    }
    else
    {
      settlement.region = r.polygon.outer;
    }
    for (const auto &cell : waterCells)
    {
      settlement.water.push_back(cell);
    }
    settlement.centre = site.localCentre;
    for (const auto &src : site.brief.input.ruralSources)
    {
      settlement.exits.push_back(src.exit);
    }
    settlement.population = site.demand.population;
    settlement.farmsteads = site.settlementClass <= SettlementClass::Village;
    settlement.seed = city.seed;
    settlement.revision = city.revision;
    settlement.terrain = r.terrain;
    const uint64_t seed = city.seed;
    settlement.random = [seed](uint64_t id, uint64_t purpose) {
      uint64_t h = (seed + 1) * 0x9E3779B97F4A7C15ULL ^ (id + 1) * 0xBF58476D1CE4E5B9ULL ^ (purpose + 1) * 0x94D049BB133111EBULL;
      h ^= h >> 31;
      h *= 0x94D049BB133111EBULL;
      h ^= h >> 29;
      return double(h % 1000000) / 1000000.;
    };
    generator::Ledger settlementLedger;
    r.layoutAccepted = generator::SettlementLayout::plan(settlement, r.layout, settlementLedger, &layoutError);
    for (const auto &entry : settlementLedger.entries())
    {
      r.rejections.push_back(entry.message);
    }
  }
  else if (city.wardBuildings && (r.walled || villageTown))
  {
    // The town, Watabou's way (generator::TownLayout): the planning cells are
    // the patches, the wall ring the border, the gates on it; streets, wall
    // sections, blocks and buildings follow in their own generators.
    generator::TownLayoutInput town;
    std::map<uint64_t, uint8_t> useOf;
    for (const auto &d : districts)
    {
      for (auto id : d.cells)
      {
        useOf[id] = uint8_t(d.use);
      }
    }
    auto remapWall = [&](Poly cell) {
      for (auto &p : cell)
      {
        for (const auto &[from, to] : r.wallMoves)
        {
          if (length(p, from) < 1e-6)
          {
            p = to;
            break;
          }
        }
      }
      return cell;
    };
    for (const auto &cell : site.brief.input.cells)
    {
      const bool usable = cell.buildable && cell.slope <= site.brief.settings.planning.maximumSlope && cell.floodRisk <= site.brief.settings.planning.maximumFloodRisk;
      const bool core = cell.allocatable && cell.buildable && (allocated.count(cell.id) || routeCells.count(cell.id));
      const bool outer = !core && cell.outskirts && usable && !allocated.count(cell.id) && !routeCells.count(cell.id);
      if (!core && !outer)
      {
        continue;
      }
      generator::TownPatch patch;
      patch.id = cell.id;
      patch.ring = remapWall(cell.boundary);
      if (signedArea(patch.ring) < 0)
      {
        std::reverse(patch.ring.begin(), patch.ring.end());
      }
      patch.withinWalls = core;
      patch.outskirts = outer;
      patch.plaza = cell.id == plazaCell;
      patch.use = useOf.count(cell.id) ? useOf[cell.id] : uint8_t(outer ? DistrictUse::Suburb : DistrictUse::Residential);
      town.patches.push_back(std::move(patch));
    }
    // The border is the circumference of the core patches (their shared
    // vertices are the wall's vertices, as in Watabou), not the chord ring.
    {
      std::vector<Poly> outers, holes;
      if (generator::unionTiles(tiles, outers, holes) && !outers.empty())
      {
        size_t largest = 0;
        for (size_t i = 1; i < outers.size(); ++i)
        {
          if (std::abs(signedArea(outers[i])) > std::abs(signedArea(outers[largest])))
          {
            largest = i;
          }
        }
        town.border = outers[largest];
        if (signedArea(town.border) < 0)
        {
          std::reverse(town.border.begin(), town.border.end());
        }
      }
      else
      {
        town.border = r.polygon.outer;
      }
    }
    for (const auto &g : r.gates)
    {
      town.gates.push_back(g.position);
      Position exit = g.position;
      for (const auto &approach : r.approaches)
      {
        if (approach.size() >= 2 && length(approach.front(), g.position) < 1e-6)
        {
          exit = approach.back();
        }
      }
      town.exits.push_back(exit);
    }
    town.centre = site.localCentre;
    for (const auto &d : districts)
    {
      if (!housingUse(d.use) && d.anchorCell != plazaCell)
      {
        for (const auto &piece : d.pieces)
        {
          town.compounds.push_back(piece);
        }
      }
    }
    for (const auto &hole : r.reservations)
    {
      town.compounds.push_back(hole);
    }
    town.districts = districts;
    town.walled = r.walled;
    town.villageFarmsteads = villageTown; // Farmstead clusters on the outskirts; dense frontage lots in the core.
    town.wallSectionMetres = city.wallSectionLength;
    town.wallTowerRadius = city.wallTowerRadius;
    town.wallTowerHeight = city.wallTowerHeight;
    town.wallSetback = city.wallSetback;
    town.arteryHalfWidth = s.arteryHalfWidth;
    town.seed = city.seed;
    town.revision = city.revision;
    town.terrain = r.terrain;
    const uint64_t seed = city.seed;
    town.random = [seed](uint64_t id, uint64_t purpose) {
      uint64_t h = (seed + 1) * 0x9E3779B97F4A7C15ULL ^ (id + 1) * 0xBF58476D1CE4E5B9ULL ^ (purpose + 1) * 0x94D049BB133111EBULL;
      h ^= h >> 31;
      h *= 0x94D049BB133111EBULL;
      h ^= h >> 29;
      return double(h % 1000000) / 1000000.;
    };
    generator::Ledger townLedger;
    r.layoutAccepted = generator::TownLayout::plan(town, r.layout, townLedger, &layoutError);
    if (r.layoutAccepted)
    {
      // Gate-to-exit rays supplied routing targets, not additional roads.
      // TownLayout now owns the routed approaches through its street graph.
      r.approaches.clear();
    }
    for (const auto &entry : townLedger.entries())
    {
      r.rejections.push_back(entry.message);
    }
  }
  // Bounded retry ladder against the layout's fixed contour budget: straight
  // lanes first, then fewer wards. Each step taken is recorded on the product.
  for (int attempt = 0; attempt < 5 && !r.layoutAccepted && !s.roadSettlement && !(city.wardBuildings && (r.walled || villageTown)); ++attempt)
  {
    CitySettings variant = city;
    if (attempt >= 1)
    {
      variant.streetCurvature = 0;
    }
    if (attempt >= 2)
    {
      variant.maximumSites = std::max(4u, city.maximumSites / 2);
      variant.maximumSuburbWards = std::max(4u, city.maximumSuburbWards / 2);
    }
    if (attempt >= 3)
    {
      variant.maximumSites = std::max(4u, city.maximumSites / 4);
      variant.targetBlockArea = city.targetBlockArea * 2;
      variant.maximumSuburbWards = std::max(4u, city.maximumSuburbWards / 4);
    }
    if (attempt >= 4)
    {
      variant.maximumSites = std::max(4u, city.maximumSites / 8);
      variant.targetBlockArea = city.targetBlockArea * 3;
      variant.targetPlotArea = city.targetPlotArea * 1.5;
      variant.maximumSuburbWards = std::max(4u, city.maximumSuburbWards / 8);
    }
    r.layoutAccepted = generateCityLayout(r.polygon, r.reservations, r.arteries, r.gates, variant, r.terrain, r.layout, &layoutError);
    if (std::getenv("CITY_DEBUG"))
    {
      std::fprintf(stderr, "layout attempt %d: %s (%s)\n", attempt, r.layoutAccepted ? "accepted" : "rejected", layoutError.c_str());
    }
    if (r.layoutAccepted && attempt)
    {
      r.rejections.push_back(
          std::string("city layout accepted only ") +
          (attempt == 1   ? "with straight lanes"
           : attempt == 2 ? "with straight lanes and half the ward sites and outer wards"
           : attempt == 3 ? "with straight lanes, a quarter of the ward sites and outer wards, and double blocks"
                          : "with straight lanes, an eighth of the ward sites and outer wards, triple blocks and larger plots") +
          " under the contour budget");
    }
    if (!r.layoutAccepted && layoutError.find("budget") == std::string::npos)
    {
      break;
    }
  }
  if (!r.layoutAccepted)
  {
    r.rejections.push_back("city layout rejected: " + layoutError);
    out = std::move(r);
    return true;
  }
  // The tail of this function now lives as stage classes in
  // procedural/world/settlement: population, compounds, neighbourhood ledgers
  // and instances. They touch nothing this function left on the stack beyond
  // what FabricBuild names explicitly, which is what made them separable
  // first; the phases above are still entangled and move out in turn.
  {
    settlement::FabricBuild build{site, territory, s, r, city, plazaCell};

    for (const auto &stage : settlement::fabricTailStages())
    {
      std::string stageError;

      if (!stage->run(build, &stageError))
      {
        return fail(std::string(stage->name()) + " stage failed: " + stageError);
      }
    }
  }

  if (error)
  {
    error->clear();
  }
  out = std::move(r);
  return true;
}

bool composeSettlementRuntimeInput(const WorldSettlementSite &site, const CityTerritory &territory, const SettlementFabric &fabric, const SettlementRuntimeSettings &s, RuntimeBuildInput &out, std::string *error, const std::function<double(double, double)> *relief)
{
  auto fail = [&](const std::string &m) {
    if (error)
    {
      *error = m;
    }
    return false;
  };
  if (!site.id || !site.brief.terrain || !territory.revision)
  {
    return fail("runtime brief needs a composed site and territory");
  }
  for (double v : {s.groundSpacingMetres, s.modifiedSpacingMetres, s.coverSpacingMetres, s.tileMetres})
  {
    if (!std::isfinite(v) || v <= 0)
    {
      return fail("invalid runtime settings");
    }
  }
  RuntimeBuildInput in;
  in.revision = site.revision;
  in.samplingSeed = site.id | 1;
  in.dependencies = {site.revision, site.revision, site.revision, site.revision, site.revision};
  // Job domain: the whole hinterland, or the settlement core (urban polygon
  // plus margin) which is what one arrangement job holds at village scale.
  double minX = 0, minZ = 0, maxX = site.bounds.maxX, maxZ = site.bounds.maxZ;
  if (s.domain == SettlementRuntimeDomain::Core)
  {
    if (!fabric.layoutAccepted || fabric.polygon.outer.empty())
    {
      return fail("core runtime job needs an accepted urban polygon");
    }
    minX = maxX = fabric.polygon.outer.front().x;
    minZ = maxZ = fabric.polygon.outer.front().z;
    for (auto p : fabric.polygon.outer)
    {
      minX = std::min(minX, p.x);
      maxX = std::max(maxX, p.x);
      minZ = std::min(minZ, p.z);
      maxZ = std::max(maxZ, p.z);
    }
    minX = std::max(0., std::floor((minX - s.coreMarginMetres) / 10) * 10);
    minZ = std::max(0., std::floor((minZ - s.coreMarginMetres) / 10) * 10);
    maxX = std::min(site.bounds.maxX, std::ceil((maxX + s.coreMarginMetres) / 10) * 10);
    maxZ = std::min(site.bounds.maxZ, std::ceil((maxZ + s.coreMarginMetres) / 10) * 10);
  }
  if (s.domain == SettlementRuntimeDomain::Tile)
  {
    minX = std::max(0., s.tileMinX);
    minZ = std::max(0., s.tileMinZ);
    maxX = std::min(site.bounds.maxX, s.tileMaxX);
    maxZ = std::min(site.bounds.maxZ, s.tileMaxZ);
    if (!(maxX > minX) || !(maxZ > minZ))
    {
      return fail("empty runtime tile domain");
    }
  }
  in.map.minX = minX;
  in.map.minZ = minZ;
  in.map.maxX = maxX;
  in.map.maxZ = maxZ;
  in.map.maximumFaces = s.maximumFaces;
  for (double x = std::floor(minX / s.tileMetres + 1) * s.tileMetres; x < maxX - 1e-6; x += s.tileMetres)
  {
    if (x > minX + 1e-6)
    {
      in.map.cutsX.push_back(x);
    }
  }
  for (double z = std::floor(minZ / s.tileMetres + 1) * s.tileMetres; z < maxZ - 1e-6; z += s.tileMetres)
  {
    if (z > minZ + 1e-6)
    {
      in.map.cutsZ.push_back(z);
    }
  }
  // The graded surface wins where there is one: the city model was built on
  // it, so the runtime ground has to be the same surface or the houses stand
  // on one hill and the player walks on another.
  in.terrain = relief != nullptr && *relief  ? *relief
               : fabric.terraceAccepted && fabric.terrain ? fabric.terrain
                                                          : site.brief.terrain;
  WorldContourBatch groundBatch{site.id * 4 + 1, 0, {}};
  std::vector<double> xs{minX}, zs{minZ};
  xs.insert(xs.end(), in.map.cutsX.begin(), in.map.cutsX.end());
  xs.push_back(maxX);
  zs.insert(zs.end(), in.map.cutsZ.begin(), in.map.cutsZ.end());
  zs.push_back(maxZ);
  for (size_t j = 0; j + 1 < zs.size(); ++j)
  {
    for (size_t i = 0; i + 1 < xs.size(); ++i)
    {
      SurfaceContour ground;
      ground.id = groundBatch.contours.size() + 1;
      ground.material = 0;
      ground.loop = {{xs[i], 0, zs[j]}, {xs[i + 1], 0, zs[j]}, {xs[i + 1], 0, zs[j + 1]}, {xs[i], 0, zs[j + 1]}};
      groundBatch.contours.push_back(ground);
    }
  }
  in.producers.push_back(groundBatch);
  // Territory contours clipped to the job domain (convex against a rectangle
  // stays convex); contours outside it are simply not part of this job.
  WorldContourBatch territoryBatch{territory.surfaces.producer, territory.surfaces.priority, {}};
  for (auto c : territory.surfaces.contours)
  {
    c.loop = clipToRect(c.loop, minX, minZ, maxX, maxZ);
    if (c.loop.empty())
    {
      continue;
    }
    territoryBatch.contours.push_back(c);
  }
  if (!territoryBatch.contours.empty())
  {
    in.producers.push_back(territoryBatch);
  }
  if (fabric.layoutAccepted)
  {
    WorldContourBatch cityBatch{site.id * 4 + 2, 20, {}};
    for (auto c : fabric.layout.contours)
    {
      c.loop = clipToRect(c.loop, minX, minZ, maxX, maxZ);
      if (!c.loop.empty())
      {
        cityBatch.contours.push_back(c);
      }
    }
    if (!cityBatch.contours.empty())
    {
      in.producers.push_back(cityBatch);
    }
  }
  std::set<uint64_t> producers;
  for (const auto &p : in.producers)
  {
    if (!p.producer || !producers.insert(p.producer).second)
    {
      return fail("runtime producers collide; assign distinct producer identities");
    }
  }
  // At most sixteen cover definitions: group materials by what they are.
  std::set<uint32_t> cropMaterials;
  for (const auto &fields : territory.fields)
  {
    for (const auto &f : fields.fields)
    {
      cropMaterials.insert(uint32_t(20 + f.id));
    }
  }
  std::map<uint32_t, bool> hard;
  for (const auto &p : in.producers)
  {
    for (const auto &c : p.contours)
    {
      hard[c.material] = hard[c.material] || (c.role != SurfaceRole::Ground && c.role != SurfaceRole::RiverBank);
    }
  }
  std::map<std::string, CoverDefinition> groups;
  auto group = [&](const std::string &name, uint32_t id, uint32_t material, bool isHard, bool crop) {
    auto &def = groups[name];
    def.id = id;
    def.hard |= isHard || crop;
    def.crop |= crop;
    def.rowSpacingMetres = 1;
    if (std::find(def.materials.begin(), def.materials.end(), material) == def.materials.end())
    {
      def.materials.push_back(material);
    }
  };
  for (auto [material, isHard] : hard)
  {
    if (cropMaterials.count(material))
    {
      group("crop", 6, material, isHard, true);
    }
    else if (material == 0)
    {
      group("ground", 0, material, isHard, false);
    }
    else if (material == 1)
    {
      group("road", 1, material, true, false);
    }
    else if (material == 11)
    {
      group("urban", 2, material, isHard, false);
    }
    else if (material == 16)
    {
      group("holding", 3, material, isHard, false);
    }
    else if (material == 17)
    {
      group("farmyard", 4, material, isHard, false);
    }
    else if (material >= 100 && material < 100 + unsigned(DistrictUse::Count))
    {
      group("district", 5, material, isHard, false);
    }
    else
    {
      group(isHard ? "engineered" : "other", isHard ? 7 : 8, material, isHard, false);
    }
  }
  for (auto &[name, def] : groups)
  {
    in.covers.push_back(def);
  }
  in.errors.resize(1);
  in.errors[0].heightMetres = s.heightErrorMetres;
  in.errors[0].normalDegrees = s.normalErrorDegrees;
  in.errors[0].maximumPasses = s.refinementPasses;
  in.errors[0].maximumAddedVertices = s.maximumAddedVertices;
  in.initial.groundSpacingMetres = s.groundSpacingMetres;
  in.initial.modifiedSpacingMetres = s.modifiedSpacingMetres;
  // The freeze stations every edge beside terrain-following faces, including
  // the arrangement's own cuts and support scanlines, to half the LOD error.
  // Structured shoulders can change curvature inside the old 5 m boundary
  // segment. Freeze denser stations before tiles independently triangulate it.
  in.initial.stationSpacingMetres = fabric.landforms.cells.empty() ? s.stationSpacingMetres : std::min(s.stationSpacingMetres,1.);
  in.initial.stationToleranceMetres = s.heightErrorMetres * .5;
  in.densities.resize(1);
  in.densities[0].groundSpacingMetres = s.groundSpacingMetres;
  in.densities[0].modifiedSpacingMetres = s.modifiedSpacingMetres;
  in.cover.seed = site.id | 1;
  in.coverSpacingMetres = s.coverSpacingMetres;
  in.coverFootprintMetres = s.coverSpacingMetres;
  // Structures inside the job domain only; a farm building beyond it belongs
  // to another job.
  {
    std::map<uint32_t, uint32_t> remap;
    for (size_t i = 0; i < fabric.structures.triangles.size(); ++i)
    {
      auto t = fabric.structures.triangles[i];
      bool inside = true;
      for (auto vi : t)
      {
        auto v = fabric.structures.vertices[vi];
        inside &= v.x >= minX && v.x <= maxX && v.z >= minZ && v.z <= maxZ;
      }
      if (!inside)
      {
        continue;
      }
      std::array<uint32_t, 3> ids;
      for (int k = 0; k < 3; ++k)
      {
        auto it = remap.find(t[k]);
        if (it == remap.end())
        {
          it = remap.emplace(t[k], uint32_t(in.structures.vertices.size())).first;
          in.structures.vertices.push_back(fabric.structures.vertices[t[k]]);
        }
        ids[k] = it->second;
      }
      in.structures.triangles.push_back(ids);
      in.structures.expectedNormals.push_back(fabric.structures.expectedNormals[i]);
      in.structuralMaterials.push_back(fabric.structuralMaterials[i]);
    }
  }
  if (error)
  {
    error->clear();
  }
  out = std::move(in);
  return true;
}

GenerationTask settlementFabricGenerator(uint64_t d)
{
  GenerationKey siteKey{"world.settlement-site", d}, territoryKey{"city.territory", d}, settingsKey{"settlement.fabric-settings", d}, outKey{"settlement.fabric", d};
  GenerationTask task;
  task.descriptor = {
    "settlement.fabric",
    d,
    1,
    GenerationScope::Neighbourhood,
    {{siteKey, typeid(WorldSettlementSite), 1}, {territoryKey, typeid(CityTerritory), 1}, {settingsKey, typeid(SettlementFabricSettings), 1}},
    {{outKey, typeid(SettlementFabric), 1}}};
  task.generate = [=](const GenerationRequest &r, GenerationSnapshot &delta, std::string *e) {
    auto site = r.context.get<WorldSettlementSite>(siteKey);
    auto territory = r.context.get<CityTerritory>(territoryKey);
    auto settings = r.context.get<SettlementFabricSettings>(settingsKey);
    if (!site || !territory || !settings)
    {
      return false;
    }
    SettlementFabric fabric;
    if (!composeSettlementFabric(*site, *territory, *settings, fabric, e))
    {
      return false;
    }
    delta.emplace(outKey, GenerationProduct{std::move(fabric), 1});
    return true;
  };
  return task;
}

GenerationTask settlementRuntimeBriefGenerator(uint64_t d)
{
  GenerationKey siteKey{"world.settlement-site", d}, territoryKey{"city.territory", d}, fabricKey{"settlement.fabric", d}, settingsKey{"settlement.runtime-settings", d}, outKey{"runtime.brief", d};
  GenerationTask task;
  task.descriptor = {
    "settlement.runtime-brief",
    d,
    1,
    GenerationScope::Neighbourhood,
    {{siteKey, typeid(WorldSettlementSite), 1}, {territoryKey, typeid(CityTerritory), 1}, {fabricKey, typeid(SettlementFabric), 1}, {settingsKey, typeid(SettlementRuntimeSettings), 1}},
    {{outKey, typeid(RuntimeBuildInput), 1}}};
  task.generate = [=](const GenerationRequest &r, GenerationSnapshot &delta, std::string *e) {
    auto site = r.context.get<WorldSettlementSite>(siteKey);
    auto territory = r.context.get<CityTerritory>(territoryKey);
    auto fabric = r.context.get<SettlementFabric>(fabricKey);
    auto settings = r.context.get<SettlementRuntimeSettings>(settingsKey);
    if (!site || !territory || !fabric || !settings)
    {
      return false;
    }
    RuntimeBuildInput input;
    if (!composeSettlementRuntimeInput(*site, *territory, *fabric, *settings, input, e))
    {
      return false;
    }
    delta.emplace(outKey, GenerationProduct{std::move(input), 1});
    return true;
  };
  return task;
}
} // namespace procedural::world::rewrite
