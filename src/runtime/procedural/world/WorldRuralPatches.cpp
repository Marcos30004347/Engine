#include "WorldRuralPatches.hpp"

#include "procedural/world/rural/PatchGrowth.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <queue>
#include <set>
#include <tuple>

namespace procedural::world::rewrite {
namespace {
using Poly = std::vector<Position>;

double cross(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

double area(const Poly &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
  {
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  }
  return a * .5;
}

bool overlaps(const Poly &a, const Poly &b)
{
  for (auto *p : {&a, &b})
  {
    for (size_t i = 0; i < p->size(); ++i)
    {
      bool separated = true;
      for (auto q : p == &a ? b : a)
      {
        if (cross((*p)[i], (*p)[(i + 1) % p->size()], q) > 1e-8)
        {
          separated = false;
          break;
        }
      }
      if (separated)
      {
        return false;
      }
    }
  }
  return true;
}

Poly cellPoly(const WorldSuitability &w, uint32_t i)
{
  auto p = w.cells()[i].center;
  double h = w.settings().spacingMetres * .5;
  return {{p.x - h, 0, p.z - h}, {p.x + h, 0, p.z - h}, {p.x + h, 0, p.z + h}, {p.x - h, 0, p.z + h}};
}

bool outline(const WorldSuitability &w, const std::vector<uint32_t> &cells, Poly &out)
{
  std::set<uint32_t> owned(cells.begin(), cells.end());
  std::map<uint64_t, uint64_t> edges;
  uint64_t stride = w.columns() + 1;
  for (auto i : cells)
  {
    uint64_t x = i % w.columns(), z = i / w.columns();
    uint64_t v[] = {z * stride + x, z * stride + x + 1, (z + 1) * stride + x + 1, (z + 1) * stride + x};
    bool exposed[] = {z == 0 || !owned.count(i - w.columns()), x + 1 == w.columns() || !owned.count(i + 1), z + 1 == w.rows() || !owned.count(i + w.columns()), x == 0 || !owned.count(i - 1)};
    for (int k = 0; k < 4; ++k)
    {
      if (exposed[k] && !edges.emplace(v[k], v[(k + 1) % 4]).second)
      {
        return false;
      }
    }
  }
  if (edges.empty())
  {
    return false;
  }
  auto start = edges.begin()->first, v = start;
  double h = w.settings().spacingMetres;
  do
  {
    auto it = edges.find(v);
    if (it == edges.end())
    {
      return false;
    }
    out.push_back({w.settings().minX + (v % stride) * h, 0, w.settings().minZ + (v / stride) * h});
    v = it->second;
    edges.erase(it);
  } while (v != start);
  if (!edges.empty())
  {
    return false; // No holes or disconnected fallback polygons.
  }
  for (int pass = 0; pass < 2; ++pass)
  {
    Poly next;
    for (size_t i = 0; i < out.size(); ++i)
    {
      auto a = out[(i + out.size() - 1) % out.size()], b = out[i], c = out[(i + 1) % out.size()];
      double turn = cross(a, b, c);
      if (std::abs(turn) < 1e-8)
      {
        continue;
      }
      if (turn > 0)
      {
        double ta = std::min(.2, h * .2 / std::hypot(a.x - b.x, a.z - b.z));
        double tc = std::min(.2, h * .2 / std::hypot(c.x - b.x, c.z - b.z));
        next.push_back({b.x * (1 - ta) + a.x * ta, 0, b.z * (1 - ta) + a.z * ta});
        next.push_back({b.x * (1 - tc) + c.x * tc, 0, b.z * (1 - tc) + c.z * tc});
      }
      else
      {
        next.push_back(b);
      }
    }
    out = std::move(next);
  }
  return out.size() >= 3 && area(out) > 0;
}

bool triangulate(const Poly &p, PatchMesh &m, std::string *error)
{
  BoundaryRegistry registry;
  PatchDomain d;
  d.id = 1;
  std::vector<BoundaryInput> edges;
  for (size_t i = 0; i < p.size(); ++i)
  {
    edges.push_back({i + 1, {i + 1, p[i]}, {(i + 1) % p.size() + 1, p[(i + 1) % p.size()]}, {}});
    d.outer.push_back({i + 1, i + 1 == p.size()});
  }
  return registry.commit(edges, error) && compilePatchMesh(
                                              d,
                                              registry,
                                              {},
                                              [](double, double) {
                                                return 0.;
                                              },
                                              m,
                                              error);
}
} // namespace

bool growRuralPatches(const WorldSuitability &w, const std::vector<SurfaceContour> &inputReservations, const std::vector<RuralSource> &sources, const RuralPatchSettings &s, RuralPatchProduct &out, std::string *error)
{
  auto fail = [&](const std::string &e) {
    if (error)
    {
      *error = e;
    }
    return false;
  };
  auto reservations = inputReservations;
  auto regions = s.regions;
  if (regions.size() > 2048)
  {
    return fail("rural region budget exceeded");
  }
  std::sort(regions.begin(), regions.end(), [](const auto &a, const auto &b) {
    return a.id < b.id;
  });
  for (size_t i = 0; i < regions.size(); ++i)
  {
    const auto &region = regions[i];
    const auto &poly = region.boundary;
    if (!region.id || (i && regions[i - 1].id == region.id) || poly.size() < 3 || poly.size() > 256 || !std::isfinite(area(poly)) || area(poly) <= 0 || unsigned(region.cover) > unsigned(RuralLandCover::Water))
    {
      return fail("invalid original rural region");
    }
    for (size_t k = 0; k < poly.size(); ++k)
    {
      auto p = poly[k];
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || p.x < w.settings().minX || p.x > w.settings().maxX || p.z < w.settings().minZ || p.z > w.settings().maxZ ||
          cross(p, poly[(k + 1) % poly.size()], poly[(k + 2) % poly.size()]) < -1e-8)
      {
        return fail("original rural regions must be finite convex CCW and in bounds");
      }
    }
    for (size_t k = 0; k < poly.size(); ++k)
    {
      for (auto p : poly)
      {
        if (cross(poly[k], poly[(k + 1) % poly.size()], p) < -1e-8)
        {
          return fail("original rural region is not convex");
        }
      }
    }
    for (size_t j = 0; j < i; ++j)
    {
      if (overlaps(poly, regions[j].boundary))
      {
        if (std::getenv("CITY_DEBUG"))
        {
          for (const auto &q : {poly, regions[j].boundary})
          {
            std::fprintf(stderr, "overlapping region:");
            for (auto v : q)
            {
              std::fprintf(stderr, " (%.1f,%.1f)", v.x, v.z);
            }
            std::fprintf(stderr, "\n");
          }
        }
        return fail(
            "original rural regions overlap: " + std::to_string(regions[i].id) + " and " + std::to_string(regions[j].id) + " (" + std::to_string(int(area(poly))) + " and " +
            std::to_string(int(area(regions[j].boundary))) + " m2)");
      }
    }
    if (region.cover != RuralLandCover::Open)
    {
      SurfaceContour c;
      c.loop = poly;
      reservations.push_back(c);
    }
  }
  double h = w.settings().spacingMetres;
  if (!w.fingerprint() || w.cells().empty() || w.cells().size() > 65536 || sources.empty() || sources.size() > 64 || reservations.size() > 2048)
  {
    return fail("invalid rural dependency or input budget");
  }
  for (double v : {s.minimumWidth, s.minimumArea, s.maximumArea, s.maximumAccessCost, s.patchSpacing, s.trackWidth})
  {
    if (!std::isfinite(v) || v <= 0)
    {
      return fail("invalid rural dimensions");
    }
  }
  if (s.minimumWidth < 2 * h || s.minimumWidth > 64 * h || s.patchSpacing > 32 * h || s.minimumArea > s.maximumArea || s.maximumArea > 1e8 || s.trackWidth > h * .8 || s.maximumPatches == 0 || s.maximumPatches > 256)
  {
    return fail("unsupported rural dimensions or patch budget");
  }
  auto src = sources;
  std::sort(src.begin(), src.end(), [](auto a, auto b) {
    return a.id < b.id;
  });
  for (size_t i = 0; i < src.size(); ++i)
  {
    if (!src[i].id || (i && src[i - 1].id == src[i].id) || !std::isfinite(src[i].exit.y) || !std::isfinite(src[i].areaBudget) || src[i].areaBudget < 0 || src[i].areaBudget > 1e9 ||
        w.cellAt(src[i].exit.x, src[i].exit.z) == SIZE_MAX)
    {
      return fail("invalid rural source");
    }
  }
  for (auto &c : reservations)
  {
    if (c.loop.size() < 3 || c.loop.size() > 256 || area(c.loop) <= 0)
    {
      return fail("invalid rural reservation");
    }
    for (size_t i = 0; i < c.loop.size(); ++i)
    {
      if (!std::isfinite(c.loop[i].x) || !std::isfinite(c.loop[i].z) || cross(c.loop[i], c.loop[(i + 1) % c.loop.size()], c.loop[(i + 2) % c.loop.size()]) < -1e-8)
      {
        return fail("rural reservations must be finite convex CCW");
      }
    }
  }
  RuralPatchProduct r;
  r.suitabilityFingerprint = w.fingerprint();
  size_t n = w.cells().size();
  r.travelCost.assign(n, INFINITY);
  r.catchments.assign(n, 0);
  r.eligible.resize(n);
  std::vector<uint8_t> walk(n), reservedCell(n);
  std::vector<uint32_t> parent(n, UINT32_MAX), clearance(n, UINT32_MAX), owner(n, 0);
  for (uint32_t i = 0; i < n; ++i)
  {
    auto &c = w.cells()[i];
    walk[i] = c.walkable;
    r.eligible[i] = c.reasons == 0;
    auto poly = cellPoly(w, i);
    for (auto &reservation : reservations)
    {
      if (overlaps(poly, reservation.loop))
      {
        walk[i] = r.eligible[i] = false;
        reservedCell[i] = 1;
        break;
      }
    }
    if (r.eligible[i])
    {
      r.eligibleArea += h * h;
    }
  }
  using Visit = std::tuple<double, uint64_t, uint32_t>;
  std::priority_queue<Visit, std::vector<Visit>, std::greater<Visit>> travel;
  for (auto source : src)
  {
    auto i = uint32_t(w.cellAt(source.exit.x, source.exit.z));
    if (!walk[i])
    {
      if (std::getenv("CITY_DEBUG"))
      {
        std::fprintf(
            stderr,
            "rural exit %llu at (%.1f, %.1f): cell %u walkable %d reasons %u reserved %d\n",
            (unsigned long long)source.id,
            source.exit.x,
            source.exit.z,
            i,
            int(w.cells()[i].walkable),
            unsigned(w.cells()[i].reasons),
            int(reservedCell[i]));
        auto poly = cellPoly(w, i);

        for (size_t k = 0; k < reservations.size(); ++k)
        {
          if (overlaps(poly, reservations[k].loop))
          {
            double minX = 1e30, minZ = 1e30, maxX = -1e30, maxZ = -1e30;

            for (auto v : reservations[k].loop)
            {
              minX = std::min(minX, v.x), maxX = std::max(maxX, v.x), minZ = std::min(minZ, v.z), maxZ = std::max(maxZ, v.z);
            }

            std::fprintf(stderr, "  reservation %zu of %zu: %zu vertices, x %.0f..%.0f z %.0f..%.0f\n", k, reservations.size(), reservations[k].loop.size(), minX, maxX, minZ, maxZ);
          }
        }
      }

      return fail("rural exit is not passable");
    }
    auto center = w.cells()[i].center;
    if (std::abs(source.exit.x - center.x) + s.trackWidth * .5 > h * .5 || std::abs(source.exit.z - center.z) + s.trackWidth * .5 > h * .5)
    {
      return fail("rural exit lacks full-width clearance inside its analysis cell");
    }
    double initial = std::hypot(source.exit.x - center.x, source.exit.z - center.z) * (1 + 4 * w.cells()[i].slope);
    if (initial > s.maximumAccessCost)
    {
      return fail("rural exit connection exceeds access cost");
    }
    if (initial < r.travelCost[i] || (initial == r.travelCost[i] && source.id < r.catchments[i]))
    {
      r.travelCost[i] = initial;
      r.catchments[i] = source.id;
      travel.emplace(initial, source.id, i);
    }
    r.budgets.push_back({source.id, source.areaBudget, 0, source.areaBudget});
  }
  while (!travel.empty())
  {
    auto [cost, source, i] = travel.top();
    travel.pop();
    if (cost != r.travelCost[i] || source != r.catchments[i])
    {
      continue;
    }
    for (auto j : w.neighbors(i))
    {
      if (walk[j])
      {
        double step = h * (1 + 4 * std::max(w.cells()[i].slope, w.cells()[j].slope));
        double next = cost + step;
        if (next <= s.maximumAccessCost && (next < r.travelCost[j] || (next == r.travelCost[j] && source < r.catchments[j])))
        {
          r.travelCost[j] = next;
          r.catchments[j] = source;
          parent[j] = i;
          travel.emplace(next, source, j);
        }
      }
    }
  }
  std::queue<uint32_t> q;
  for (uint32_t i = 0; i < n; ++i)
  {
    if (!r.eligible[i] || !r.catchments[i] || i % w.columns() == 0 || i % w.columns() + 1 == w.columns() || i < w.columns() || i + w.columns() >= n)
    {
      clearance[i] = 0;
      q.push(i);
    }
  }
  while (!q.empty())
  {
    auto i = q.front();
    q.pop();
    for (auto j : w.neighbors(i))
    {
      if (clearance[j] > clearance[i] + 1)
      {
        clearance[j] = clearance[i] + 1;
        q.push(j);
      }
    }
  }
  // Which answer to "what is a farm" this hinterland calls for. The two were a
  // four-hundred-line branch against its else; they are strategies now, with a
  // name each, in procedural/world/rural.
  std::vector<rural::Growth> growth;

  {
    const rural::GrowthContext context{w, s, src, regions, r, clearance, reservedCell, h, owner};

    if (!rural::growPatches(context, growth, error))
    {
      return false;
    }
  }
  // Imported cells supply the lane graph too: original vertices and edge
  // midpoints, shared by neighbouring estates. Only the source approach may
  // leave that graph, and it cannot cut through an allocated field interior.
  std::vector<Poly> boundaryRoutes(growth.size());
  std::vector<double> boundaryCosts(growth.size(), INFINITY);
  if (!regions.empty())
  {
    std::vector<Position> vertices;
    std::map<std::pair<int64_t, int64_t>, uint32_t> vertexIds;
    auto vertex = [&](Position p) {
      auto key = std::make_pair(int64_t(std::llround(p.x * 1e6)), int64_t(std::llround(p.z * 1e6)));
      auto [it, added] = vertexIds.emplace(key, vertices.size());
      if (added)
      {
        vertices.push_back(p);
      }
      return it->second;
    };
    std::set<std::pair<uint32_t, uint32_t>> edges;
    std::map<uint64_t, std::vector<uint32_t>> frontage;
    for (auto &region : regions)
    {
      if (region.cover == RuralLandCover::Open)
      {
        for (size_t k = 0; k < region.boundary.size(); ++k)
        {
          auto a = region.boundary[k], b = region.boundary[(k + 1) % region.boundary.size()];
          auto u = vertex(a), v = vertex(b), m = vertex({(a.x + b.x) * .5, 0, (a.z + b.z) * .5});
          edges.insert(std::minmax(u, m));
          edges.insert(std::minmax(m, v));
          if (std::hypot(b.x - a.x, b.z - a.z) >= std::max(52., s.minimumWidth))
          {
            frontage[region.id].push_back(m);
          }
        }
      }
    }
    auto corridorCost = [&](Position a, Position b, bool approach) -> double {
      double len = std::hypot(b.x - a.x, b.z - a.z), half = s.trackWidth * .5;
      if (len < 1e-6)
      {
        return 0.;
      }
      if (approach)
      {
        for (const auto &g : growth)
        {
          // The exit usually lies inside a candidate region of its own; the
          // approach may leave that region, it must not cut through others.
          bool startsInside = true;
          for (size_t k = 0; k < g.boundary.size() && startsInside; ++k)
          {
            startsInside = cross(g.boundary[k], g.boundary[(k + 1) % g.boundary.size()], a) >= -1e-7;
          }
          if (startsInside)
          {
            continue;
          }
          double lo = 0, hi = 1;
          for (size_t k = 0; k < g.boundary.size(); ++k)
          {
            auto u = g.boundary[k], v = g.boundary[(k + 1) % g.boundary.size()];
            double da = cross(u, v, a), db = cross(u, v, b);
            if (da < 0 && db < 0)
            {
              hi = -1;
              break;
            }
            if (da < 0)
            {
              lo = std::max(lo, da / (da - db));
            }
            if (db < 0)
            {
              hi = std::min(hi, da / (da - db));
            }
          }
          if (hi > lo + 1e-7)
          {
            Position mid{a.x + (b.x - a.x) * (lo + hi) * .5, 0, a.z + (b.z - a.z) * (lo + hi) * .5};
            bool strict = true;
            for (size_t k = 0; k < g.boundary.size(); ++k)
            {
              strict &= cross(g.boundary[k], g.boundary[(k + 1) % g.boundary.size()], mid) > 1e-7;
            }
            if (strict)
            {
              return INFINITY;
            }
          }
        }
      }
      double x = -(b.z - a.z) / len * half, z = (b.x - a.x) / len * half;
      std::vector<Poly> ribbons = {
        {{a.x - x, 0, a.z - z}, {b.x - x, 0, b.z - z}, {b.x + x, 0, b.z + z}, {a.x + x, 0, a.z + z}},
        {{a.x - half, 0, a.z - half}, {a.x + half, 0, a.z - half}, {a.x + half, 0, a.z + half}, {a.x - half, 0, a.z + half}},
        {{b.x - half, 0, b.z - half}, {b.x + half, 0, b.z - half}, {b.x + half, 0, b.z + half}, {b.x - half, 0, b.z + half}}};
      double slope = 0;
      for (auto &ribbon : ribbons)
      {
        double x0 = INFINITY, z0 = INFINITY, x1 = -INFINITY, z1 = -INFINITY;
        for (auto p : ribbon)
        {
          x0 = std::min(x0, p.x);
          x1 = std::max(x1, p.x);
          z0 = std::min(z0, p.z);
          z1 = std::max(z1, p.z);
        }
        if (x0 < w.settings().minX || x1 > w.settings().maxX || z0 < w.settings().minZ || z1 > w.settings().maxZ)
        {
          return INFINITY;
        }
        int c0 = std::max(0, int((x0 - w.settings().minX) / h)), c1 = std::min(int(w.columns()) - 1, int((x1 - w.settings().minX) / h));
        int r0 = std::max(0, int((z0 - w.settings().minZ) / h)), r1 = std::min(int(w.rows()) - 1, int((z1 - w.settings().minZ) / h));
        for (int z = r0; z <= r1; ++z)
        {
          for (int x = c0; x <= c1; ++x)
          {
            uint32_t i = z * w.columns() + x;
            if (!overlaps(ribbon, cellPoly(w, i)))
            {
              continue;
            }
            if (!walk[i])
            {
              return INFINITY;
            }
            slope = std::max(slope, w.cells()[i].slope);
          }
        }
      }
      return len * (1 + 4 * slope);
    };
    std::vector<std::vector<std::pair<uint32_t, double>>> adjacency(vertices.size());
    for (auto [a, b] : edges)
    {
      double cost = corridorCost(vertices[a], vertices[b], false);
      if (std::isfinite(cost))
      {
        adjacency[a].push_back({b, cost});
        adjacency[b].push_back({a, cost});
      }
    }
    for (auto source : src)
    {
      uint32_t attachment = UINT32_MAX;
      double nearest = INFINITY;
      for (uint32_t i = 0; i < vertices.size(); ++i)
      {
        if (!adjacency[i].empty())
        {
          double cost = corridorCost(source.exit, vertices[i], true);
          if (cost < nearest)
          {
            nearest = cost;
            attachment = i;
          }
        }
      }
      if (attachment == UINT32_MAX)
      {
        continue;
      }
      std::vector<double> costs(vertices.size(), INFINITY);
      std::vector<uint32_t> previous(vertices.size(), UINT32_MAX);
      using Entry = std::pair<double, uint32_t>;
      std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
      costs[attachment] = nearest;
      queue.push({nearest, attachment});
      while (!queue.empty())
      {
        auto [cost, i] = queue.top();
        queue.pop();
        if (cost != costs[i])
        {
          continue;
        }
        for (auto [j, length] : adjacency[i])
        {
          if (cost + length < costs[j] && cost + length <= s.maximumAccessCost)
          {
            costs[j] = cost + length;
            previous[j] = i;
            queue.push({costs[j], j});
          }
        }
      }
      for (size_t k = 0; k < growth.size(); ++k)
      {
        if (growth[k].source == source.id)
        {
          uint32_t end = UINT32_MAX;
          for (auto i : frontage[growth[k].region])
          {
            if (i != attachment && costs[i] < boundaryCosts[k] && costs[i] <= s.maximumAccessCost)
            {
              end = i;
              boundaryCosts[k] = costs[i];
            }
          }
          if (end == UINT32_MAX)
          {
            continue;
          }
          auto i = end;
          while (i != UINT32_MAX)
          {
            boundaryRoutes[k].push_back(vertices[i]);
            i = previous[i];
          }
          std::reverse(boundaryRoutes[k].begin(), boundaryRoutes[k].end());
          boundaryRoutes[k].insert(boundaryRoutes[k].begin(), source.exit);
        }
      }
    }
  }
  uint64_t contourId = 1;
  size_t reservedTrackContours = 0;
  for (auto &g : growth)
  {
    AgriculturalPatch p;
    p.id = r.patches.size() + 1;
    p.source = g.source;
    p.cells.assign(g.cells.begin(), g.cells.end());
    p.originalRegion = g.region;
    p.boundary = g.boundary;
    if (p.boundary.empty() && !outline(w, p.cells, p.boundary))
    {
      r.rejections.push_back("patch support has a hole or invalid boundary");
      continue;
    }
    p.area = area(p.boundary);
    if (p.area < s.minimumArea)
    {
      r.rejections.push_back("patch falls below minimum area after regularization");
      continue;
    }
    for (auto i : p.cells)
    {
      p.meanSuitability += w.cells()[i].score / p.cells.size();
    }
    p.accessCost = r.travelCost[g.seed];
    auto i = g.seed;
    while (i != UINT32_MAX)
    {
      p.access.push_back(w.cells()[i].center);
      p.accessCells.push_back(i);
      i = parent[i];
    }
    std::reverse(p.access.begin(), p.access.end());
    std::reverse(p.accessCells.begin(), p.accessCells.end());
    // Exit is anchored at its analysis-cell center; include the authored endpoint.
    auto source = std::find_if(src.begin(), src.end(), [&](auto v) {
      return v.id == p.source;
    });
    p.access.insert(p.access.begin(), source->exit);
    Poly simplified;
    for (auto v : p.access)
    {
      while (simplified.size() >= 2 && std::abs(cross(simplified[simplified.size() - 2], simplified.back(), v)) < 1e-8)
      {
        simplified.pop_back();
      }
      if (simplified.empty() || std::hypot(v.x - simplified.back().x, v.z - simplified.back().z) > 1e-8)
      {
        simplified.push_back(v);
      }
    }
    p.access = std::move(simplified);
    if (g.region)
    {
      size_t k = &g - growth.data();
      if (boundaryRoutes[k].size() < 3)
      {
        r.rejections.push_back("original region " + std::to_string(g.region) + " lacks a connected boundary lane");
        continue;
      }
      p.access = boundaryRoutes[k];
      p.accessCost = boundaryCosts[k];
      p.accessCells.clear();
      p.boundaryAccessStart = 1;
    }
    PatchMesh mesh;
    if (!triangulate(p.boundary, mesh, error))
    {
      return false;
    }
    // Fail soft on the contour budget: the tracks (one ribbon per access
    // segment plus one join square per vertex) are emitted after this loop,
    // so they are counted here before the patch is admitted.
    const size_t trackContours = p.access.size() > 1 ? 2 * p.access.size() - 1 : 0;
    if (r.contours.size() + reservedTrackContours + mesh.triangles.size() + trackContours > 8192)
    {
      r.rejections.push_back("patch " + std::to_string(p.id) + (g.region ? " (original region " + std::to_string(g.region) + ")" : std::string()) + " omitted: rural contour budget");
      continue;
    }
    reservedTrackContours += trackContours;
    for (auto t : mesh.triangles)
    {
      SurfaceContour c;
      c.id = contourId++;
      c.material = 20 + p.id;
      for (auto v : t)
      {
        c.loop.push_back(mesh.vertices[v].position);
      }
      if (area(c.loop) < 0)
      {
        std::reverse(c.loop.begin(), c.loop.end());
      }
      r.contours.push_back(c);
    }
    auto b = std::find_if(r.budgets.begin(), r.budgets.end(), [&](auto b) {
      return b.source == p.source;
    });
    b->allocated += p.area;
    r.patchArea += p.area;
    r.patches.push_back(std::move(p));
  }
  // Swept tracks follow validated grid or original boundary routes. An exit's
  // subcell segment is bounded by the same cell. Join squares prevent gaps.
  for (auto &p : r.patches)
  {
    for (size_t i = 1; i < p.access.size(); ++i)
    {
      auto a = p.access[i - 1], b = p.access[i];
      double len = std::hypot(b.x - a.x, b.z - a.z);
      if (len < 1e-8)
      {
        continue;
      }
      double dx = (b.x - a.x) / len, dz = (b.z - a.z) / len, hf = s.trackWidth * .5;
      SurfaceContour c;
      c.id = contourId++;
      c.material = 1;
      c.role = SurfaceRole::Road;
      c.loop = {{a.x - dz * hf, 0, a.z + dx * hf}, {a.x + dz * hf, 0, a.z - dx * hf}, {b.x + dz * hf, 0, b.z - dx * hf}, {b.x - dz * hf, 0, b.z + dx * hf}};
      r.contours.push_back(c);
    }
  }
  for (auto &p : r.patches)
  {
    for (auto v : p.access)
    {
      double hf = s.trackWidth * .5;
      SurfaceContour c;
      c.id = contourId++;
      c.material = 1;
      c.role = SurfaceRole::Road;
      c.loop = {{v.x - hf, 0, v.z - hf}, {v.x + hf, 0, v.z - hf}, {v.x + hf, 0, v.z + hf}, {v.x - hf, 0, v.z + hf}};
      r.contours.push_back(c);
    }
  }
  for (auto &b : r.budgets)
  {
    b.shortfall = std::max(0., b.requested - b.allocated);
  }
  r.unallocatedArea = r.eligibleArea - r.patchArea;
  if (r.contours.size() > 8192)
  {
    return fail("rural contour budget exceeded");
  }
  if (error)
  {
    error->clear();
  }
  out = std::move(r);
  return true;
}
} // namespace procedural::world::rewrite
