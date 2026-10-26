#include "WorldSettlementFabricAdapter.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
namespace procedural::world::rewrite
{
namespace
{
using Poly = std::vector<Position>;
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
Position vertexMean(const Poly &p)
{
  Position c;
  for (auto q : p)
  {
    c.x += q.x / p.size();
    c.z += q.z / p.size();
  }
  return c;
}
bool insideEvenOdd(const Poly &p, Position q)
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
bool onRing(const Poly &ring, Position p, double tolerance)
{
  for (size_t i = 0; i < ring.size(); ++i)
    if (segmentDistance(p, ring[i], ring[(i + 1) % ring.size()]) <= tolerance)
      return true;
  return false;
}
bool strictlyInsideConvex(const Poly &p, Position q)
{
  for (size_t k = 0; k < p.size(); ++k)
    if (cross(p[k], p[(k + 1) % p.size()], q) <= 1e-6)
      return false;
  return !p.empty();
}
using Key = std::pair<int64_t, int64_t>;
Key keyOf(Position p)
{
  return {int64_t(std::llround(p.x * 1e6)), int64_t(std::llround(p.z * 1e6))};
}
// Union of exactly tiling convex cells: shared directed edges cancel, the
// remaining edges chain into rings. At a pinch vertex the rightmost turn is
// taken so every ring stays simple. Positive rings are outer boundaries.
bool unionTiles(const std::vector<Poly> &tiles, std::vector<Poly> &outers, std::vector<Poly> &holes)
{
  std::map<Key, Position> positions;
  std::map<std::pair<Key, Key>, int> edges;
  for (const auto &tile : tiles)
  {
    Poly t = tile;
    if (signedArea(t) < 0)
      std::reverse(t.begin(), t.end());
    for (size_t i = 0; i < t.size(); ++i)
    {
      auto a = keyOf(t[i]), b = keyOf(t[(i + 1) % t.size()]);
      if (a == b)
        continue;
      positions.emplace(a, t[i]);
      positions.emplace(b, t[(i + 1) % t.size()]);
      auto reverse = edges.find({b, a});
      if (reverse != edges.end())
      {
        if (--reverse->second == 0)
          edges.erase(reverse);
      }
      else
        ++edges[{a, b}];
    }
  }
  std::map<Key, std::vector<Key>> out;
  for (auto &[edge, count] : edges)
    if (count > 0)
      out[edge.first].push_back(edge.second);
  std::set<std::pair<Key, Key>> used;
  for (auto &[edge, count] : edges)
  {
    if (count <= 0 || used.count(edge))
      continue;
    Poly ring{positions[edge.first]};
    Key start = edge.first, previous = edge.first, current = edge.second;
    used.insert(edge);
    size_t guard = 0;
    while (current != start && guard++ < 100000)
    {
      ring.push_back(positions[current]);
      auto d = positions[current], back = positions[previous];
      Position incoming{d.x - back.x, 0, d.z - back.z};
      Key best{};
      double bestTurn = INFINITY;
      bool found = false;
      for (auto next : out[current])
      {
        if (used.count({current, next}))
          continue;
        auto n = positions[next];
        Position e{n.x - d.x, 0, n.z - d.z};
        double turn = std::atan2(incoming.x * e.z - incoming.z * e.x, incoming.x * e.x + incoming.z * e.z);
        if (turn < bestTurn)
        {
          bestTurn = turn;
          best = next;
          found = true;
        }
      }
      if (!found)
        return false;
      used.insert({current, best});
      previous = current;
      current = best;
    }
    if (current != start)
      return false;
    (signedArea(ring) > 0 ? outers : holes).push_back(std::move(ring));
  }
  return !outers.empty();
}
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
    return;
  mesh.triangles.push_back(ids);
  mesh.expectedNormals.push_back({n.x / l, n.y / l, n.z / l});
  materials.push_back(material);
}
} // namespace
bool composeSettlementFabric(const WorldSettlementSite &site, const CityTerritory &territory, const SettlementFabricSettings &s, SettlementFabric &out, std::string *error)
{
  auto fail = [&](const std::string &m)
  {
    if (error)
      *error = m;
    return false;
  };
  if (!site.id || !site.brief.terrain || !site.water || !site.waterSupply || !site.brief.suitability.fingerprint())
    return fail("settlement site is not composed");
  if (territory.revision != site.revision || territory.suitabilityFingerprint != site.brief.suitability.fingerprint())
    return fail("territory does not belong to this site revision");
  if (!std::isfinite(s.maximumTerraceStepMetres) || s.maximumTerraceStepMetres <= 0 || !std::isfinite(s.arteryHalfWidth) || s.arteryHalfWidth < .5 || s.arteryHalfWidth > 20)
    return fail("invalid fabric settings");
  SettlementFabric r;
  r.revision = site.revision;
  const auto &plan = territory.regions;
  if (plan.districts.empty())
    return fail("territory allocated no districts");
  std::map<uint64_t, const CityPlanCell *> cellById;
  for (const auto &cell : site.brief.input.cells)
    cellById[cell.id] = &cell;
  // Urban polygon: allocated pieces plus the circulation cells on the routes.
  std::set<uint64_t> allocated, routeCells;
  std::vector<Poly> tiles;
  for (const auto &d : plan.districts)
  {
    for (auto id : d.cells)
      allocated.insert(id);
    for (auto id : d.accessCells)
      routeCells.insert(id);
  }
  for (auto id : allocated)
    if (cellById.count(id))
      tiles.push_back(cellById[id]->boundary);
  for (auto id : routeCells)
    if (!allocated.count(id) && cellById.count(id))
    {
      tiles.push_back(cellById[id]->boundary);
      ++r.routeCells;
    }
  std::vector<Poly> outers, holes;
  if (!unionTiles(tiles, outers, holes))
    return fail("urban cells do not form a closed polygon");
  size_t largest = 0;
  for (size_t i = 1; i < outers.size(); ++i)
    if (signedArea(outers[i]) > signedArea(outers[largest]))
      largest = i;
  r.polygon.outer = outers[largest];
  for (const auto &hole : holes)
    if (insideEvenOdd(r.polygon.outer, vertexMean(hole)))
    {
      Poly h = hole;
      std::reverse(h.begin(), h.end());
      r.polygon.holes.push_back(h);
    }
  if (outers.size() > 1)
    r.rejections.push_back(std::to_string(outers.size() - 1) + " detached urban components were left out of the polygon");
  auto insidePolygon = [&](Position p)
  {
    if (onRing(r.polygon.outer, p, 1e-6))
      return true;
    if (!insideEvenOdd(r.polygon.outer, p))
      return false;
    for (const auto &hole : r.polygon.holes)
      if (insideEvenOdd(hole, p) && !onRing(hole, p, 1e-6))
        return false;
    return true;
  };
  std::vector<CityPlanDistrict> districts;
  for (const auto &d : plan.districts)
  {
    bool inside = true;
    for (const auto &piece : d.pieces)
      inside &= insidePolygon(vertexMean(piece));
    if (inside)
      districts.push_back(d);
    else
      r.rejections.push_back("district " + std::to_string(d.id) + " (" + districtUseName(d.use) + ") lies outside the connected urban polygon");
  }
  // Terrace: area-weighted mean of the actual surface over the urban cells,
  // accepted only when the largest cut or fill stays within the budget.
  const auto &base = site.brief.terrain;
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
      step = std::max(step, std::abs(base(v.x, v.z) - r.terraceLevel));
    auto c = vertexMean(tile);
    step = std::max(step, std::abs(base(c.x, c.z) - r.terraceLevel));
  }
  r.terraceStep = step;
  r.terraceAccepted = step <= s.maximumTerraceStepMetres;
  if (!r.terraceAccepted)
  {
    r.rejections.push_back("urban terrace needs " + std::to_string(step) + " m of cut/fill, above the " + std::to_string(s.maximumTerraceStepMetres) + " m budget; no streets or houses were built");
    r.terrain = base;
    out = std::move(r);
    return true;
  }
  {
    auto outer = r.polygon.outer;
    auto holesCopy = r.polygon.holes;
    double level = r.terraceLevel;
    r.terrain = [outer, holesCopy, level, base](double x, double z)
    {
      Position p{x, 0, z};
      bool in = onRing(outer, p, 1e-6) || insideEvenOdd(outer, p);
      for (const auto &hole : holesCopy)
        if (in && insideEvenOdd(hole, p) && !onRing(hole, p, 1e-6))
          in = false;
      return in ? level : base(x, z);
    };
  }
  // Gates and fixed arteries from the master plan's connected gate tree.
  std::set<std::pair<Key, Key>> hopKeys;
  uint64_t arteryId = 1000;
  auto addHop = [&](Position a, Position b)
  {
    if (length(a, b) < 1)
      return;
    auto key = std::minmax(keyOf(a), keyOf(b));
    if (!hopKeys.insert(key).second)
      return;
    r.arteries.push_back({++arteryId, a, b, s.arteryHalfWidth, true});
  };
  std::vector<const CityPlanDistrict *> compounds;
  for (const auto &d : districts)
    if (!housingUse(d.use))
      compounds.push_back(&d);
  // The connected gate tree (root gate cell -> every gate cell) is the fixed
  // circulation; district access inside the village comes from the layout's
  // own street net, so housing districts add no further fixed arteries.
  // Routes alternate cell centre, portal, cell centre. A chord between two
  // consecutive centres crosses their shared edge once and stays inside the
  // union of the two convex cells, so the portal points can be dropped.
  // The shared edge can be short and the cells skewed, in which case the
  // chord leaves both cells; then the portal point is kept for that hop.
  auto cellOf = [&](Position p) -> const Poly *
  {
    for (const auto &tile : tiles)
      if (strictlyInsideConvex(tile, p) || onRing(tile, p, 1e-6))
        return &tile;
    return nullptr;
  };
  auto centresOnly = [&](const std::vector<Position> &route)
  {
    std::vector<Position> out;
    if (route.empty())
      return out;
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
        out.push_back(route[i - 1]);
      out.push_back(b);
    }
    return out;
  };
  for (const auto &route : plan.gateConnections)
  {
    auto pts = centresOnly(route);
    for (size_t i = 1; i < pts.size(); ++i)
      addHop(pts[i - 1], pts[i]);
  }
  for (const auto &d : districts)
  {
    if (housingUse(d.use) || d.access.size() < 3)
      continue;
    // Compounds are reached at their boundary portal: the last centre before
    // the anchor, then the portal on the compound's edge.
    std::vector<Position> trimmed(d.access.begin(), d.access.end() - 2); // Drop the anchor and its portal.
    auto pts = centresOnly(trimmed);
    pts.push_back(d.access[d.access.size() - 2]);
    for (size_t i = 1; i < pts.size(); ++i)
      addHop(pts[i - 1], pts[i]);
  }
  std::set<uint64_t> released;
  for (const auto &hop : r.arteries)
    for (auto *d : compounds)
      for (const auto &piece : d->pieces)
        for (double t : {.25, .5, .75})
          if (strictlyInsideConvex(piece, {hop.a.x + (hop.b.x - hop.a.x) * t, 0, hop.a.z + (hop.b.z - hop.a.z) * t}))
            released.insert(d->id);
  for (auto id : released)
    r.rejections.push_back("compound district " + std::to_string(id) + " is crossed by a gate route and was released to ordinary land");
  r.releasedCompounds.assign(released.begin(), released.end());
  for (size_t k = 0; k < site.brief.input.gateCells.size(); ++k)
  {
    auto id = site.brief.input.gateCells[k];
    if (!cellById.count(id) || !insidePolygon(vertexMean(cellById[id]->boundary)))
      continue;
    auto centre = vertexMean(cellById[id]->boundary);
    Position exit = centre;
    double nearest = INFINITY;
    for (const auto &src : site.brief.input.ruralSources)
      if (length(src.exit, centre) < nearest)
      {
        nearest = length(src.exit, centre);
        exit = src.exit;
      }
    Position gate;
    double bestT = INFINITY;
    const auto &ring = r.polygon.outer;
    for (size_t i = 0; i < ring.size(); ++i)
    {
      auto a = ring[i], b = ring[(i + 1) % ring.size()];
      Position d{exit.x - centre.x, 0, exit.z - centre.z}, e{b.x - a.x, 0, b.z - a.z}, w{a.x - centre.x, 0, a.z - centre.z};
      double denom = d.x * e.z - d.z * e.x;
      if (std::abs(denom) < 1e-12)
        continue;
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
  }
  if (r.gates.empty())
  {
    r.rejections.push_back("no gate could be placed on the urban polygon; no streets or houses were built");
    out = std::move(r);
    return true;
  }
  // Street and house layout on the terrace.
  CitySettings city = s.city;
  city.seed = site.id ^ 0x9E3779B97F4A7C15ULL;
  city.revision = site.revision;
  city.housingAreas.clear();
  city.districts.clear();
  for (const auto &d : districts)
    if (!released.count(d.id))
      city.districts.push_back(d);
  // Bounded retry ladder against the layout's fixed contour budget: straight
  // lanes first, then fewer wards. Each step taken is recorded on the product.
  std::string layoutError;
  for (int attempt = 0; attempt < 3 && !r.layoutAccepted; ++attempt)
  {
    CitySettings variant = city;
    if (attempt >= 1)
      variant.streetCurvature = 0;
    if (attempt >= 2)
      variant.maximumSites = std::max(4u, city.maximumSites / 2);
    r.layoutAccepted = generateCityLayout(r.polygon, r.reservations, r.arteries, r.gates, variant, r.terrain, r.layout, &layoutError);
    if (r.layoutAccepted && attempt)
      r.rejections.push_back(std::string("city layout accepted only ") + (attempt == 1 ? "with straight lanes" : "with straight lanes and half the ward sites") + " under the contour budget");
    if (!r.layoutAccepted && layoutError.find("budget") == std::string::npos)
      break;
  }
  if (!r.layoutAccepted)
  {
    r.rejections.push_back("city layout rejected: " + layoutError);
    out = std::move(r);
    return true;
  }
  // Residents through the director with explicit terrain/water eligibility.
  SettlementDirectorInput policy = s.director;
  policy.settlement = site.id;
  policy.seed = site.populationBrief.seed;
  policy.population = site.populationBrief;
  if (policy.catalogue.empty())
    policy.catalogue = settlementProgrammeCatalogue();
  policy.sites.clear();
  const auto &w = site.brief.suitability;
  for (const auto &o : r.layout.objects)
  {
    if (o.kind != CityObjectKind::House && o.kind != CityObjectKind::Workshop)
      continue;
    SettlementSite ss;
    ss.id = o.id;
    size_t cell = w.cellAt(o.entrance.x, o.entrance.z);
    ss.buildable = cell != SIZE_MAX && w.cells()[cell].walkable && !(w.cells()[cell].reasons & Wet) && !site.water(o.entrance.x, o.entrance.z);
    ss.water = site.waterSupply(o.entrance.x, o.entrance.z);
    ss.slope = 0; // On the accepted terrace.
    policy.sites.push_back(ss);
  }
  std::string directorError;
  if (!prepareCityDirector(r.layout, policy, r.directorInput, &directorError))
    r.rejections.push_back("director adapter rejected the layout: " + directorError);
  else if (!planSettlement(r.directorInput, r.population, &directorError))
    r.rejections.push_back("settlement director rejected the programme: " + directorError);
  else
    r.populated = true;
  // Instances: city massing on the terrace, farm buildings on the real ground.
  std::string meshError;
  if (!meshCityInstances(r.layout, r.terrain, r.instances, &meshError))
    r.rejections.push_back("city instance meshing failed: " + meshError);
  std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> index;
  for (const auto &t : r.instances)
    appendTriangle(r.structures, r.structuralMaterials, index, t.points, t.material);
  if (s.meshFarmBuildings)
    for (const auto &holding : territory.holdings)
      for (const auto &b : holding.buildings)
      {
        Poly p = b.footprint;
        if (p.size() < 3)
          continue;
        if (signedArea(p) < 0)
          std::reverse(p.begin(), p.end());
        Poly bottom = p, top = p;
        bool seated = true;
        for (size_t i = 0; i < p.size(); ++i)
        {
          bottom[i].y = base(p[i].x, p[i].z);
          top[i].y = b.foundation + b.height;
          seated &= std::isfinite(bottom[i].y) && bottom[i].y <= b.foundation + 1e-6;
        }
        if (!seated)
        {
          r.rejections.push_back("farm building " + std::to_string(b.id) + " is not seated on its foundation");
          continue;
        }
        size_t before = r.structures.triangles.size();
        for (size_t i = 0; i < p.size(); ++i)
        {
          size_t j = (i + 1) % p.size();
          appendTriangle(r.structures, r.structuralMaterials, index, {bottom[i], top[i], top[j]}, 12);
          appendTriangle(r.structures, r.structuralMaterials, index, {bottom[i], top[j], bottom[j]}, 12);
        }
        Position peak = vertexMean(p);
        double perimeter = 0;
        for (size_t i = 0; i < p.size(); ++i)
          perimeter += length(p[i], p[(i + 1) % p.size()]);
        peak.y = b.foundation + b.height + std::min(3., std::abs(signedArea(p)) / perimeter * .5);
        for (size_t i = 0; i < p.size(); ++i)
          appendTriangle(r.structures, r.structuralMaterials, index, {top[i], peak, top[(i + 1) % p.size()]}, 13);
        r.farmTriangles += uint32_t(r.structures.triangles.size() - before);
      }
  if (error)
    error->clear();
  out = std::move(r);
  return true;
}
bool composeSettlementRuntimeInput(const WorldSettlementSite &site, const CityTerritory &territory, const SettlementFabric &fabric, const SettlementRuntimeSettings &s, RuntimeBuildInput &out, std::string *error)
{
  auto fail = [&](const std::string &m)
  {
    if (error)
      *error = m;
    return false;
  };
  if (!site.id || !site.brief.terrain || !territory.revision)
    return fail("runtime brief needs a composed site and territory");
  for (double v : {s.groundSpacingMetres, s.modifiedSpacingMetres, s.coverSpacingMetres, s.tileMetres})
    if (!std::isfinite(v) || v <= 0)
      return fail("invalid runtime settings");
  RuntimeBuildInput in;
  in.revision = site.revision;
  in.samplingSeed = site.id | 1;
  in.dependencies = {site.revision, site.revision, site.revision, site.revision, site.revision};
  in.map.minX = 0;
  in.map.minZ = 0;
  in.map.maxX = site.bounds.maxX;
  in.map.maxZ = site.bounds.maxZ;
  in.map.maximumFaces = s.maximumFaces;
  for (double x = s.tileMetres; x < site.bounds.maxX - 1e-6; x += s.tileMetres)
    in.map.cutsX.push_back(x);
  for (double z = s.tileMetres; z < site.bounds.maxZ - 1e-6; z += s.tileMetres)
    in.map.cutsZ.push_back(z);
  in.terrain = fabric.terraceAccepted && fabric.terrain ? fabric.terrain : site.brief.terrain;
  SurfaceContour ground;
  ground.id = 1;
  ground.material = 0;
  ground.loop = {{0, 0, 0}, {site.bounds.maxX, 0, 0}, {site.bounds.maxX, 0, site.bounds.maxZ}, {0, 0, site.bounds.maxZ}};
  in.producers.push_back({site.id * 4 + 1, 0, {ground}});
  in.producers.push_back(territory.surfaces);
  if (fabric.layoutAccepted)
    in.producers.push_back({site.id * 4 + 2, 20, fabric.layout.contours});
  std::set<uint64_t> producers;
  for (const auto &p : in.producers)
    if (!p.producer || !producers.insert(p.producer).second)
      return fail("runtime producers collide; assign distinct producer identities");
  std::set<uint32_t> cropMaterials;
  for (const auto &fields : territory.fields)
    for (const auto &f : fields.fields)
      cropMaterials.insert(uint32_t(20 + f.id));
  std::map<uint32_t, bool> hard;
  for (const auto &p : in.producers)
    for (const auto &c : p.contours)
      hard[c.material] = hard[c.material] || (c.role != SurfaceRole::Ground && c.role != SurfaceRole::RiverBank) || cropMaterials.count(c.material);
  for (auto [material, isHard] : hard)
    in.covers.push_back({material, {material}, isHard, cropMaterials.count(material) > 0, 1, 0});
  in.errors.resize(1);
  in.initial.groundSpacingMetres = s.groundSpacingMetres;
  in.initial.modifiedSpacingMetres = s.modifiedSpacingMetres;
  in.densities.resize(1);
  in.densities[0].groundSpacingMetres = s.groundSpacingMetres;
  in.densities[0].modifiedSpacingMetres = s.modifiedSpacingMetres;
  in.cover.seed = site.id | 1;
  in.coverSpacingMetres = s.coverSpacingMetres;
  in.coverFootprintMetres = s.coverSpacingMetres;
  in.structures = fabric.structures;
  in.structuralMaterials = fabric.structuralMaterials;
  if (error)
    error->clear();
  out = std::move(in);
  return true;
}
GenerationTask settlementFabricGenerator(uint64_t d)
{
  GenerationKey siteKey{"world.settlement-site", d}, territoryKey{"city.territory", d}, settingsKey{"settlement.fabric-settings", d}, outKey{"settlement.fabric", d};
  GenerationTask task;
  task.descriptor = {"settlement.fabric", d, 1, GenerationScope::Neighbourhood, {{siteKey, typeid(WorldSettlementSite), 1}, {territoryKey, typeid(CityTerritory), 1}, {settingsKey, typeid(SettlementFabricSettings), 1}}, {{outKey, typeid(SettlementFabric), 1}}};
  task.generate = [=](const GenerationRequest &r, GenerationSnapshot &delta, std::string *e)
  {
    auto site = r.context.get<WorldSettlementSite>(siteKey);
    auto territory = r.context.get<CityTerritory>(territoryKey);
    auto settings = r.context.get<SettlementFabricSettings>(settingsKey);
    if (!site || !territory || !settings)
      return false;
    SettlementFabric fabric;
    if (!composeSettlementFabric(*site, *territory, *settings, fabric, e))
      return false;
    delta.emplace(outKey, GenerationProduct{std::move(fabric), 1});
    return true;
  };
  return task;
}
GenerationTask settlementRuntimeBriefGenerator(uint64_t d)
{
  GenerationKey siteKey{"world.settlement-site", d}, territoryKey{"city.territory", d}, fabricKey{"settlement.fabric", d}, settingsKey{"settlement.runtime-settings", d}, outKey{"runtime.brief", d};
  GenerationTask task;
  task.descriptor = {"settlement.runtime-brief", d, 1, GenerationScope::Neighbourhood, {{siteKey, typeid(WorldSettlementSite), 1}, {territoryKey, typeid(CityTerritory), 1}, {fabricKey, typeid(SettlementFabric), 1}, {settingsKey, typeid(SettlementRuntimeSettings), 1}}, {{outKey, typeid(RuntimeBuildInput), 1}}};
  task.generate = [=](const GenerationRequest &r, GenerationSnapshot &delta, std::string *e)
  {
    auto site = r.context.get<WorldSettlementSite>(siteKey);
    auto territory = r.context.get<CityTerritory>(territoryKey);
    auto fabric = r.context.get<SettlementFabric>(fabricKey);
    auto settings = r.context.get<SettlementRuntimeSettings>(settingsKey);
    if (!site || !territory || !fabric || !settings)
      return false;
    RuntimeBuildInput input;
    if (!composeSettlementRuntimeInput(*site, *territory, *fabric, *settings, input, e))
      return false;
    delta.emplace(outKey, GenerationProduct{std::move(input), 1});
    return true;
  };
  return task;
}
} // namespace procedural::world::rewrite
