#include "TownBlocks.hpp"

#include "procedural/geom2/Boolean.hpp"
#include "procedural/geom2/Polygon.hpp"
#include "procedural/world/WorldCityMasterPlan.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <queue>
#include <set>

namespace procedural::world::generator {

namespace {

double distance(Position a, Position b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
}

Position mix(Position a, Position b, double t)
{
  return {a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t};
}

// Keep the half-plane nx x + nz z <= c of a convex ring.
ConvexRing clipHalfPlane(const ConvexRing &p, double nx, double nz, double c)
{
  ConvexRing q;

  for (size_t i = 0; i < p.size(); ++i)
  {
    const Position a = p[i], b = p[(i + 1) % p.size()];
    const double da = nx * a.x + nz * a.z - c, db = nx * b.x + nz * b.z - c;

    if (da <= 0)
    {
      q.push_back(a);
    }

    if ((da < 0) != (db < 0))
    {
      q.push_back(mix(a, b, da / (da - db)));
    }
  }

  ConvexRing r;

  for (auto v : q)
  {
    if (r.empty() || distance(r.back(), v) > 1e-7)
    {
      r.push_back(v);
    }
  }

  while (r.size() > 1 && distance(r.front(), r.back()) < 1e-7)
  {
    r.pop_back();
  }

  return r;
}

// Whether the segment (c, d) runs along the edge (a, b): parallel within
// about ten degrees, within `tolerance` of it, overlapping it in projection.
bool alongEdge(Position a, Position b, Position c, Position d, double tolerance)
{
  const double l = distance(a, b), m = distance(c, d);

  if (l < 1e-9 || m < 1e-9)
  {
    return false;
  }

  if (std::abs((b.x - a.x) * (d.z - c.z) - (b.z - a.z) * (d.x - c.x)) / (l * m) > .17)
  {
    return false;
  }

  if (std::abs(cross(a, b, c)) / l > tolerance || std::abs(cross(a, b, d)) / l > tolerance)
  {
    return false;
  }

  const double t0 = ((c.x - a.x) * (b.x - a.x) + (c.z - a.z) * (b.z - a.z)) / (l * l);
  const double t1 = ((d.x - a.x) * (b.x - a.x) + (d.z - a.z) * (b.z - a.z)) / (l * l);
  return std::max(t0, t1) > 1e-3 && std::min(t0, t1) < 1 - 1e-3;
}

bool segmentThrough(const ConvexRing &p, Position a, Position b)
{
  for (int k = 1; k < 8; ++k)
  {
    const Position q = mix(a, b, k / 8.);

    if (!strictlyInsideConvex(p, q))
    {
      continue;
    }

    double edge = 1e30;

    for (size_t e = 0; e < p.size(); ++e)
    {
      edge = std::min(edge, segmentDistance(q, p[e], p[(e + 1) % p.size()]));
    }

    if (edge > .3)
    {
      return true;
    }
  }

  return false;
}

} // namespace

TownBlocks::TownBlocks(uint64_t domain) : Generator("town.blocks", domain, "town.blocks-input", "town.blocks-plan", GenerationScope::Patch)
{
}

bool TownBlocks::plan(const TownBlocksInput &s, TownBlocksPlan &r, Ledger &ledger, std::string *error)
{
  auto fail = [&](const std::string &message) {
    ledger.add(LedgerKind::Rejected, 0, "town blocks: " + message);

    if (error != nullptr)
    {
      *error = message;
    }

    return false;
  };

  r = TownBlocksPlan{};

  if (s.patches.empty() || !s.random)
  {
    return fail("blocks need patches and a random source");
  }
  if (!std::isfinite(s.maximumInteriorBlockArea) || s.maximumInteriorBlockArea<0 ||
      !std::isfinite(s.maximumInteriorBlockSpan) || s.maximumInteriorBlockSpan<0)
    return fail("interior block limits must be finite and nonnegative");

  auto random = [&](uint64_t id, uint64_t purpose) {
    return s.random(id, purpose);
  };

  auto onWall = [&](Position a, Position b) {
    for (size_t i = 0; i < s.wallRing.size(); ++i)
    {
      // An outer patch edge may sit a couple of metres off the smoothed wall
      // (its vertices did not move with the core's): still the wall side.
      if (alongEdge(s.wallRing[i], s.wallRing[(i + 1) % s.wallRing.size()], a, b, 2.5) || alongEdge(a, b, s.wallRing[i], s.wallRing[(i + 1) % s.wallRing.size()], 2.5))
      {
        return true;
      }
    }

    return false;
  };

  auto onArtery = [&](Position a, Position b) {
    for (const auto &artery : s.arteries)
    {
      for (size_t i = 1; i < artery.size(); ++i)
      {
        if (alongEdge(a, b, artery[i - 1], artery[i], 1.))
        {
          return true;
        }
      }
    }

    return false;
  };

  const TownPatch *plaza = nullptr;

  for (const auto &patch : s.patches)
  {
    if (patch.plaza)
    {
      plaza = &patch;
    }
  }

  auto onPlaza = [&](Position a, Position b) {
    if (plaza == nullptr)
    {
      return false;
    }

    for (size_t i = 0; i < plaza->ring.size(); ++i)
    {
      if (alongEdge(plaza->ring[i], plaza->ring[(i + 1) % plaza->ring.size()], a, b, .05))
      {
        return true;
      }
    }

    return false;
  };

  // The patch across an edge: the patch (other than `self`) with an edge along it.
  auto neighbour = [&](const TownPatch &self, Position a, Position b) -> const TownPatch * {
    for (const auto &other : s.patches)
    {
      if (other.id == self.id)
      {
        continue;
      }

      for (size_t i = 0; i < other.ring.size(); ++i)
      {
        if (alongEdge(other.ring[i], other.ring[(i + 1) % other.ring.size()], a, b, .05))
        {
          return &other;
        }
      }
    }

    return nullptr;
  };

  auto enclosed = [&](const TownPatch &patch) {
    if (patch.withinWalls)
    {
      return true;
    }

    if (!patch.outskirts)
    {
      return false;
    }

    for (size_t i = 0; i < patch.ring.size(); ++i)
    {
      const TownPatch *other = neighbour(patch, patch.ring[i], patch.ring[(i + 1) % patch.ring.size()]);

      if (other == nullptr || !(other->withinWalls || other->outskirts))
      {
        return false;
      }
    }

    return true;
  };

  std::vector<std::pair<Position,double>> junctions;
  for (const auto &patch:s.patches) if (patch.withinWalls || patch.outskirts)
    for (size_t k=0;k<patch.ring.size();++k) {
      const auto a=patch.ring[k],b=patch.ring[(k+1)%patch.ring.size()];
      double width=patch.withinWalls ? s.regularStreet*.5 : s.alley*.5;
      if (onPlaza(a,b)) width=s.mainStreet*.5;
      if (onArtery(a,b)) width=std::max(width,s.arteryHalfWidth);
      junctions.push_back({a,width}); junctions.push_back({b,width});
    }
  for (const auto &artery:s.arteries) for (auto p:artery) junctions.push_back({p,s.arteryHalfWidth});
  for (const auto &artery:s.curvedArteries)
    for (size_t i=1;i<artery.size();++i) {
      const auto a=artery[i-1],b=artery[i];
      const auto steps=std::max(1u,uint32_t(std::ceil(distance(a,b)/1.)));
      for (uint32_t k=0;k<=steps;++k) {
        const double t=double(k)/steps;
        junctions.push_back({{a.x+(b.x-a.x)*t,0,a.z+(b.z-a.z)*t},s.arteryHalfWidth+.1});
      }
    }

  // ---- which edges are actually streets -----------------------------------
  //
  // A cell edge is not a street. The wall is, an artery is, the plaza's
  // frontage is - and then every block the network has not reached gets one
  // routed to it, shortest first over the lattice, which is the same A* the
  // approaches and the farm tracks use. Everything left over is not a road
  // and its two cells are one block.
  const auto key = [](Position p) { return std::make_pair(int64_t(std::llround(p.x * 100)), int64_t(std::llround(p.z * 100))); };

  struct Link
  {
    uint32_t a = 0, b = 0;
    double length = 0;
    bool claimed = false;
  };

  std::map<std::pair<int64_t, int64_t>, uint32_t> nodeOf;
  std::vector<Position> nodes;
  std::vector<Link> links;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> linkOf;
  std::vector<std::vector<uint32_t>> incident;

  const auto node = [&](Position p) {
    const auto k = key(p);
    const auto it = nodeOf.find(k);

    if (it != nodeOf.end())
    {
      return it->second;
    }

    const uint32_t n = uint32_t(nodes.size());
    nodeOf[k] = n;
    nodes.push_back(p);
    incident.emplace_back();

    return n;
  };

  const auto link = [&](Position p, Position q) {
    const uint32_t a = node(p), b = node(q);

    if (a == b)
    {
      return uint32_t(UINT32_MAX);
    }

    const auto pair = std::minmax(a, b);
    const auto it = linkOf.find({pair.first, pair.second});

    if (it != linkOf.end())
    {
      return it->second;
    }

    const uint32_t e = uint32_t(links.size());
    linkOf[{pair.first, pair.second}] = e;
    links.push_back({a, b, distance(p, q), false});
    incident[a].push_back(e);
    incident[b].push_back(e);

    return e;
  };

  if (s.dissolveCells)
  {
    for (const auto &patch : s.patches)
    {
      if (!(patch.withinWalls || patch.outskirts))
      {
        continue;
      }

      for (size_t i = 0; i < patch.ring.size(); ++i)
      {
        const Position a = patch.ring[i], b = patch.ring[(i + 1) % patch.ring.size()];
        const uint32_t e = link(a, b);

        if (e != UINT32_MAX && (onWall(a, b) || onArtery(a, b) || onPlaza(a, b)))
        {
          links[e].claimed = true;
        }
      }
    }

    // Every block the network has not reached: route to it.
    std::vector<uint8_t> onNetwork(nodes.size(), 0);

    for (const auto &e : links)
    {
      if (e.claimed)
      {
        onNetwork[e.a] = 1;
        onNetwork[e.b] = 1;
      }
    }

    std::vector<const TownPatch *> ordered;

    for (const auto &patch : s.patches)
    {
      if (patch.withinWalls || patch.outskirts)
      {
        ordered.push_back(&patch);
      }
    }

    std::stable_sort(ordered.begin(), ordered.end(), [](const TownPatch *p, const TownPatch *q) { return p->id < q->id; });

    uint32_t routed = 0;

    for (const TownPatch *patch : ordered)
    {
      bool served = false;

      for (size_t i = 0; i < patch->ring.size() && !served; ++i)
      {
        const uint32_t a = node(patch->ring[i]), b = node(patch->ring[(i + 1) % patch->ring.size()]);
        const auto pair = std::minmax(a, b);
        const auto it = linkOf.find({pair.first, pair.second});
        served = it != linkOf.end() && links[it->second].claimed;
      }

      if (served)
      {
        continue;
      }

      // Dijkstra from the patch's own vertices to the nearest node already on
      // the network, over the lattice; the path becomes a lane.
      std::vector<double> cost(nodes.size(), 1e30);
      std::vector<uint32_t> cameFrom(nodes.size(), UINT32_MAX), cameBy(nodes.size(), UINT32_MAX);
      std::priority_queue<std::pair<double, uint32_t>, std::vector<std::pair<double, uint32_t>>, std::greater<>> queue;

      for (auto p : patch->ring)
      {
        const uint32_t n = node(p);
        cost[n] = 0;
        queue.push({0, n});
      }

      uint32_t reached = UINT32_MAX;

      while (!queue.empty())
      {
        const auto [c, n] = queue.top();
        queue.pop();

        if (c > cost[n] + 1e-9)
        {
          continue;
        }

        if (onNetwork[n])
        {
          reached = n;

          break;
        }

        if (c > s.serviceReachMetres)
        {
          break;
        }

        for (const uint32_t e : incident[n])
        {
          const uint32_t m = links[e].a == n ? links[e].b : links[e].a;
          const double next = c + links[e].length;

          if (next < cost[m])
          {
            cost[m] = next;
            cameFrom[m] = n;
            cameBy[m] = e;
            queue.push({next, m});
          }
        }
      }

      if (reached == UINT32_MAX)
      {
        continue;
      }

      for (uint32_t n = reached; cameBy[n] != UINT32_MAX; n = cameFrom[n])
      {
        links[cameBy[n]].claimed = true;
        onNetwork[n] = 1;
        onNetwork[cameFrom[n]] = 1;
      }

      onNetwork[reached] = 1;
      ++routed;
    }

    ledger.add(LedgerKind::Repaired, 0, "town blocks: " + std::to_string(routed) + " blocks had a lane routed to them; " +
                                            std::to_string(std::count_if(links.begin(), links.end(), [](const Link &e) { return e.claimed; })) + " of " +
                                            std::to_string(links.size()) + " lattice edges carry a street");
  }

  // Whether a street runs along this edge of `piece`, a piece of `patch`.
  //
  // An edge that lies along one of the patch's own edges is a lattice edge and
  // is a street only if the network claimed it. An edge that does not is a cut
  // this pass made - an artery through the cell, or an interior split - and a
  // cut IS a street: it was drawn because something had to get past.
  const auto claimed = [&](const TownPatch &patch, Position a, Position b) {
    if (!s.dissolveCells)
    {
      return true;
    }

    bool onLattice = false;

    for (size_t i = 0; i < patch.ring.size() && !onLattice; ++i)
    {
      onLattice = alongEdge(patch.ring[i], patch.ring[(i + 1) % patch.ring.size()], a, b, .05);
    }

    if (!onLattice)
    {
      return true;
    }

    // Which lattice edge, and was it claimed: the piece's edge is a part of
    // it, so the nodes are matched by containment rather than by identity.
    for (const auto &e : links)
    {
      if (!e.claimed)
      {
        continue;
      }

      if (alongEdge(nodes[e.a], nodes[e.b], a, b, .05))
      {
        return true;
      }
    }

    return false;
  };

  // ---- the land between the streets ---------------------------------------
  //
  // With the lattice dissolved a block is not a cell and not a piece of one:
  // it is what is left of the built-up land once every street's ribbon is
  // taken out of it. Cells that share an unclaimed edge merge into one
  // polygon, usually concave, and the lot stage cuts that.
  if (s.dissolveCells)
  {
    const auto width = [&](const Position a, const Position b, const bool inside) {
      if (onWall(a, b) || onPlaza(a, b))
      {
        return s.mainStreet * .5;
      }

      if (onArtery(a, b))
      {
        return s.arteryHalfWidth;
      }

      return inside ? s.regularStreet * .5 : s.alley * .5;
    };

    // Group the patches: union-find across every UNCLAIMED shared edge,
    // between cells of the same standing and the same use.
    std::map<uint64_t, uint64_t> parent;
    const std::function<uint64_t(uint64_t)> find = [&](uint64_t x) {
      while (parent[x] != x)
      {
        parent[x] = parent[parent[x]];
        x = parent[x];
      }

      return x;
    };

    std::vector<const TownPatch *> built;

    for (const auto &patch : s.patches)
    {
      if (!(patch.withinWalls || patch.outskirts))
      {
        continue;
      }

      bool yard = false;

      for (const auto &compound : s.compounds)
      {
        Position c{0, 0, 0};

        for (auto v : compound)
        {
          c.x += v.x / compound.size();
          c.z += v.z / compound.size();
        }

        yard = yard || strictlyInsideConvex(patch.ring, c);
      }

      if (yard)
      {
        ++r.yards;

        continue;
      }

      parent[patch.id] = patch.id;
      built.push_back(&patch);
    }

    for (const TownPatch *patch : built)
    {
      for (size_t i = 0; i < patch->ring.size(); ++i)
      {
        const Position a = patch->ring[i], b = patch->ring[(i + 1) % patch->ring.size()];

        if (claimed(*patch, a, b))
        {
          continue;
        }

        const TownPatch *other = neighbour(*patch, a, b);

        if (other == nullptr || !parent.count(other->id))
        {
          continue;
        }

        if (other->use != patch->use || other->withinWalls != patch->withinWalls || other->outskirts != patch->outskirts)
        {
          continue;
        }

        const uint64_t x = find(patch->id), y = find(other->id);

        if (x != y)
        {
          parent[x] = y;
        }
      }
    }

    // Every street's ribbon, once, with the box it covers: a block is cut
    // against the handful of streets that reach it, never against the whole
    // town's network at once - the arrangement's segment budget is finite and
    // subtracting six hundred ribbons from one block exhausts it.
    struct Ribbon
    {
      geom2::Operand polygon;
      double minX = 0, minZ = 0, maxX = 0, maxZ = 0;
    };

    std::vector<Ribbon> ribbons;
    std::set<std::pair<uint32_t, uint32_t>> drawn;

    const auto ribbon = [&](Position a, Position b, double halfWidth) {
      const double l = distance(a, b);

      if (l < 1e-6)
      {
        return;
      }

      const double nx = -(b.z - a.z) / l * halfWidth, nz = (b.x - a.x) / l * halfWidth;
      const double ex = (b.x - a.x) / l * halfWidth, ez = (b.z - a.z) / l * halfWidth;
      geom2::Polygon polygon;

      for (auto p : {Position{a.x + nx - ex, 0, a.z + nz - ez},
                     Position{b.x + nx + ex, 0, b.z + nz + ez},
                     Position{b.x - nx + ex, 0, b.z - nz + ez},
                     Position{a.x - nx - ex, 0, a.z - nz - ez}})
      {
        polygon.outer.push_back({geom2::toUnit(p.x), geom2::toUnit(p.z)});
      }

      if (!geom2::counterClockwise(polygon.outer))
      {
        geom2::reverse(polygon.outer);
      }

      Ribbon entry;
      entry.polygon = geom2::toOperand(polygon);
      entry.minX = std::min({a.x, b.x}) - halfWidth * 1.5;
      entry.maxX = std::max({a.x, b.x}) + halfWidth * 1.5;
      entry.minZ = std::min({a.z, b.z}) - halfWidth * 1.5;
      entry.maxZ = std::max({a.z, b.z}) + halfWidth * 1.5;
      ribbons.push_back(std::move(entry));
    };

    for (const TownPatch *patch : built)
    {
      for (size_t i = 0; i < patch->ring.size(); ++i)
      {
        const Position a = patch->ring[i], b = patch->ring[(i + 1) % patch->ring.size()];

        if (!claimed(*patch, a, b))
        {
          continue;
        }

        const auto ka = nodeOf.find(key(a)), kb = nodeOf.find(key(b));

        if (ka != nodeOf.end() && kb != nodeOf.end())
        {
          const auto pair = std::minmax(ka->second, kb->second);

          if (!drawn.insert({pair.first, pair.second}).second)
          {
            continue;
          }
        }

        const double halfWidth = width(a, b, patch->withinWalls);
        ribbon(a, b, halfWidth);

        if (!onWall(a, b) && !onArtery(a, b) && !onPlaza(a, b))
        {
          r.lanes.push_back({a, b, halfWidth, false});
        }
      }
    }

    for (const auto &artery : s.curvedArteries)
    {
      for (size_t i = 1; i < artery.size(); ++i)
      {
        ribbon(artery[i - 1], artery[i], s.arteryHalfWidth);
      }
    }

    // The market square, before any block is cut: a square at the heart of the
    // plaza cell, turned to the street that reaches it, reserved like a
    // compound so the blocks are cut around it rather than over it.
    for (const auto &patch : s.patches)
    {
      if (!patch.plaza || patch.ring.size() < 3 || !r.market.empty())
      {
        continue;
      }

      Position centre{0, 0, 0};

      for (auto p : patch.ring)
      {
        centre.x += p.x / double(patch.ring.size());
        centre.z += p.z / double(patch.ring.size());
      }

      // As wide as the settlement wants, but never more than its share of the
      // cell: a square bigger than the ground it stands on is not a square.
      const double cell = std::abs(signedArea(patch.ring));
      const double side = std::min(s.marketSideMetres, std::sqrt(std::max(0., cell * s.marketShare)));

      if (side < 12)
      {
        continue;
      }

      // Turned to the longest edge of the cell, which is the street the
      // square opens onto.
      double ex = 1, ez = 0, longest = 0;

      for (size_t i = 0; i < patch.ring.size(); ++i)
      {
        const auto a = patch.ring[i], b = patch.ring[(i + 1) % patch.ring.size()];
        const double length = distance(a, b);

        if (length > longest)
        {
          longest = length;
          ex = (b.x - a.x) / length;
          ez = (b.z - a.z) / length;
        }
      }

      // The square as wide as it may be, turned to the street, and then
      // shrunk until it stands inside the cell: a square of the cell's
      // share of area rotated to its longest edge pokes out of a hexagon,
      // and refusing it outright left the town with no market at all.
      ConvexRing square;
      bool held = false;

      for (double scale = 1.; scale >= .5 && !held; scale -= .1)
      {
        const double width = side * scale;

        if (width < 12)
        {
          break;
        }

        square.clear();

        for (const auto &[u, v] : std::vector<std::pair<double, double>>{{-.5, -.5}, {.5, -.5}, {.5, .5}, {-.5, .5}})
        {
          square.push_back({centre.x + (ex * u - ez * v) * width, 0, centre.z + (ez * u + ex * v) * width});
        }

        // Inside the cell, or it is somebody else's ground.
        held = true;

        for (auto p : square)
        {
          held = held && (strictlyInsideConvex(patch.ring, p) || onRing(patch.ring, p, .01));
        }
      }

      if (!held)
      {
        if (std::getenv("CITY_DEBUG"))
        {
          std::fprintf(stderr, "town blocks: market square of %.1f m does not fit its plaza cell\n", side);
        }

        continue;
      }

      r.market = square;

      geom2::Polygon polygon;

      for (auto p : square)
      {
        polygon.outer.push_back({geom2::toUnit(p.x), geom2::toUnit(p.z)});
      }

      if (!geom2::counterClockwise(polygon.outer))
      {
        geom2::reverse(polygon.outer);
      }

      Ribbon entry;
      entry.polygon = geom2::toOperand(polygon);
      entry.minX = entry.minZ = 1e30;
      entry.maxX = entry.maxZ = -1e30;

      for (auto p : square)
      {
        entry.minX = std::min(entry.minX, p.x);
        entry.maxX = std::max(entry.maxX, p.x);
        entry.minZ = std::min(entry.minZ, p.z);
        entry.maxZ = std::max(entry.maxZ, p.z);
      }

      ribbons.push_back(std::move(entry));
    }

    for (const auto &compound : s.compounds)
    {
      geom2::Polygon polygon;

      for (auto p : compound)
      {
        polygon.outer.push_back({geom2::toUnit(p.x), geom2::toUnit(p.z)});
      }

      if (polygon.outer.size() >= 3)
      {
        if (!geom2::counterClockwise(polygon.outer))
        {
          geom2::reverse(polygon.outer);
        }

        Ribbon entry;
        entry.polygon = geom2::toOperand(polygon);
        entry.minX = entry.minZ = 1e30;
        entry.maxX = entry.maxZ = -1e30;

        for (auto p : compound)
        {
          entry.minX = std::min(entry.minX, p.x);
          entry.maxX = std::max(entry.maxX, p.x);
          entry.minZ = std::min(entry.minZ, p.z);
          entry.maxZ = std::max(entry.maxZ, p.z);
        }

        ribbons.push_back(std::move(entry));
      }
    }

    // One block per group, minus every ribbon.
    std::map<uint64_t, std::vector<const TownPatch *>> groups;

    for (const TownPatch *patch : built)
    {
      groups[find(patch->id)].push_back(patch);
    }

    uint32_t merged = 0, failures = 0;

    for (const auto &[root, members] : groups)
    {
      bool plazaGroup = false;

      for (const TownPatch *patch : members)
      {
        plazaGroup = plazaGroup || patch->plaza;
      }

      std::vector<ConvexRing> tiles;

      for (const TownPatch *patch : members)
      {
        tiles.push_back(patch->ring);
      }

      std::vector<ConvexRing> outers, holes;
      std::vector<geom2::Polygon> land;

      if (tiles.size() > 1 && unionTiles(tiles, outers, holes) && !outers.empty())
      {
        ++merged;

        for (const auto &outer : outers)
        {
          geom2::Polygon polygon;

          for (auto p : outer)
          {
            polygon.outer.push_back({geom2::toUnit(p.x), geom2::toUnit(p.z)});
          }

          if (!geom2::counterClockwise(polygon.outer))
          {
            geom2::reverse(polygon.outer);
          }

          land.push_back(std::move(polygon));
        }
      }
      else
      {
        for (const TownPatch *patch : members)
        {
          geom2::Polygon polygon;

          for (auto p : patch->ring)
          {
            polygon.outer.push_back({geom2::toUnit(p.x), geom2::toUnit(p.z)});
          }

          if (!geom2::counterClockwise(polygon.outer))
          {
            geom2::reverse(polygon.outer);
          }

          land.push_back(std::move(polygon));
        }
      }

      for (const auto &polygon : land)
      {
        std::vector<geom2::Polygon> pieces;
        std::string why;
        double minX = 1e30, maxX = -1e30, minZ = 1e30, maxZ = -1e30;

        for (auto p : polygon.outer)
        {
          const double x = geom2::toMetres(p.x), z = geom2::toMetres(p.y);
          minX = std::min(minX, x);
          maxX = std::max(maxX, x);
          minZ = std::min(minZ, z);
          maxZ = std::max(maxZ, z);
        }

        std::vector<geom2::Operand> nearby;

        for (const auto &entry : ribbons)
        {
          if (entry.maxX >= minX && entry.minX <= maxX && entry.maxZ >= minZ && entry.minZ <= maxZ)
          {
            nearby.push_back(entry.polygon);
          }
        }

        // In batches, and never all at once.
        //
        // A merged group is a quarter of a town and the streets reaching it
        // are dozens of ribbons; one boolean with all of them exhausted the
        // arrangement's segment budget and the whole group was dropped -
        // which is exactly the empty quarter inside the wall. Subtracting a
        // few at a time keeps every call small, and a call that still fails
        // keeps its piece rather than losing the land: the lot stage carves
        // the street out of the lots afterwards anyway.
        pieces.push_back(polygon);

        for (size_t first = 0; first < nearby.size(); first += 6)
        {
          const std::vector<geom2::Operand> batch(nearby.begin() + first, nearby.begin() + std::min(nearby.size(), first + 6));
          std::vector<geom2::Polygon> next;

          for (const auto &piece : pieces)
          {
            if (geom2::areaMetres(piece) < 24)
            {
              continue;
            }

            std::vector<geom2::Polygon> cut;

            if (!geom2::differenceOf(geom2::toOperand(piece), batch, cut, &why))
            {
              ++failures;
              next.push_back(piece);

              continue;
            }

            for (auto &part : cut)
            {
              next.push_back(std::move(part));
            }
          }

          pieces = std::move(next);

          if (pieces.empty())
          {
            break;
          }
        }

        // A dissolved block can be several cells across, which is not a
        // block - it is a district with no way into the middle of it. Cut it
        // with streets until it is one, exactly as the land was cut from the
        // arteries: the cut is a ribbon, so the halves come out of the same
        // boolean and a concave piece stays whole.
        std::vector<geom2::Polygon> settled;
        std::function<void(const geom2::Polygon &, uint32_t, uint64_t)> divide = [&](const geom2::Polygon &piece, uint32_t depth, uint64_t key) {
          const double area = geom2::areaMetres(piece);
          std::vector<Position> ring;

          for (auto p : piece.outer)
          {
            ring.push_back({geom2::toMetres(p.x), 0, geom2::toMetres(p.y)});
          }

          // The axis it is longest along, taken from its own edges.
          double ex = 1, ez = 0, longest = 0;

          for (size_t i = 0; i < ring.size(); ++i)
          {
            const auto a = ring[i], b = ring[(i + 1) % ring.size()];
            const double length = distance(a, b);

            if (length > longest)
            {
              longest = length;
              ex = (b.x - a.x) / length;
              ez = (b.z - a.z) / length;
            }
          }

          // Watabou's Cutter.bisect, at block scale: the cut goes through a
          // point along the LONGEST EDGE, square to that edge, turned by a
          // small angle. The ratio and the angle are the grid chaos - his
          // spread is 0.8 * chaos about the middle and the angle up to 30
          // degrees, and the angle is dropped on a piece that is nearly at
          // its target size so small blocks stay rectangular.
          size_t longestEdge = 0;
          longest = 0;

          for (size_t i = 0; i < ring.size(); ++i)
          {
            const auto a = ring[i], b = ring[(i + 1) % ring.size()];
            const double length = distance(a, b);

            if (length > longest)
            {
              longest = length;
              longestEdge = i;
              ex = (b.x - a.x) / length;
              ez = (b.z - a.z) / length;
            }
          }

          const double chaos = .5;
          const double spread = .8 * chaos;
          const double ratio = (1 - spread) * .5 + random(key, 173) * spread;
          const double turn = (random(key, 174) - .5) * (3.14159265358979323846 / 6.) * chaos * (area < s.maximumInteriorBlockArea * 2.5 ? 0. : 1.);

          // The cut RUNS square to that edge, so the edge is its normal: the
          // ribbon below is laid along the perpendicular of this normal, and
          // getting the two the wrong way round cuts the block along its
          // longest edge instead of across it - one sliver and one block.
          const double nx = ex * std::cos(turn) - ez * std::sin(turn), nz = ez * std::cos(turn) + ex * std::sin(turn);

          const Position seed = ring[longestEdge];
          const Position next = ring[(longestEdge + 1) % ring.size()];
          const Position through{seed.x + (next.x - seed.x) * ratio, 0, seed.z + (next.z - seed.z) * ratio};
          const double at = nx * through.x + nz * through.z;

          double lo = 1e100, hi = -1e100, span = 0;

          for (auto p : ring)
          {
            const double t = nx * p.x + nz * p.z;
            lo = std::min(lo, t);
            hi = std::max(hi, t);
          }

          span = hi - lo;

          const bool oversized = (s.maximumInteriorBlockArea > 0 && area > s.maximumInteriorBlockArea) || (s.maximumInteriorBlockSpan > 0 && span > s.maximumInteriorBlockSpan);

          if (!oversized || depth >= 8 || span < 24 + 2 * s.regularStreet || ring.size() < 3)
          {
            settled.push_back(piece);

            return;
          }

          const bool avenue = depth == 0 && (plazaGroup || random(key, 172) < .4);
          const double halfWidth = avenue ? s.arteryHalfWidth : s.regularStreet * .5;

          // The cut, as a ribbon across the whole piece.
          double minX = 1e100, maxX = -1e100, minZ = 1e100, maxZ = -1e100;

          for (auto p : ring)
          {
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minZ = std::min(minZ, p.z);
            maxZ = std::max(maxZ, p.z);
          }

          const double reach = std::hypot(maxX - minX, maxZ - minZ) + 4;
          const Position along{-nz, 0, nx};

          // On the cut line, beside the piece: the point of the line nearest
          // the piece's own centre. Taking the point nearest the ORIGIN put
          // the band a town's width away from the block it was cutting, and
          // the cut silently did nothing.
          const Position centre{(minX + maxX) * .5, 0, (minZ + maxZ) * .5};
          const double slide = along.x * centre.x + along.z * centre.z;
          const Position middle{nx * at + along.x * slide, 0, nz * at + along.z * slide};
          const Position from{middle.x - along.x * reach, 0, middle.z - along.z * reach};
          const Position to{middle.x + along.x * reach, 0, middle.z + along.z * reach};

          geom2::Polygon cut;

          for (auto p : {Position{from.x + nx * halfWidth, 0, from.z + nz * halfWidth},
                         Position{to.x + nx * halfWidth, 0, to.z + nz * halfWidth},
                         Position{to.x - nx * halfWidth, 0, to.z - nz * halfWidth},
                         Position{from.x - nx * halfWidth, 0, from.z - nz * halfWidth}})
          {
            cut.outer.push_back({geom2::toUnit(p.x), geom2::toUnit(p.z)});
          }

          if (!geom2::counterClockwise(cut.outer))
          {
            geom2::reverse(cut.outer);
          }

          std::vector<geom2::Polygon> halves;
          std::string why;

          if (!geom2::differenceOf(geom2::toOperand(piece), {geom2::toOperand(cut)}, halves, &why) || halves.size() < 2)
          {
            settled.push_back(piece);

            return;
          }

          bool usable = true;

          for (const auto &half : halves)
          {
            double n0=1e100,n1=-1e100,t0=1e100,t1=-1e100;
            for (auto p : half.outer)
            {
              const double x=geom2::toMetres(p.x), z=geom2::toMetres(p.y);
              const double n=nx*x+nz*z, t=-nz*x+nx*z;
              n0=std::min(n0,n); n1=std::max(n1,n);
              t0=std::min(t0,t); t1=std::max(t1,t);
            }
            // Keep room for modular homes after the street takes its width.
            usable = usable && geom2::areaMetres(half) > 120 && n1-n0 >= 12 && t1-t0 >= 12;
          }

          if (!usable)
          {
            settled.push_back(piece);

            return;
          }

          ++r.cuts;

          // The cut is a street, clipped to the piece it divides.
          std::vector<geom2::Polygon> inside;

          if (geom2::intersectionOf({geom2::toOperand(piece), geom2::toOperand(cut)}, inside, &why))
          {
            for (const auto &band : inside)
            {
              std::vector<Position> boundary;
              for (auto p : band.outer)
                boundary.push_back({geom2::toMetres(p.x),0,geom2::toMetres(p.y)});
              std::vector<double> stations;
              for(size_t e=0;e<boundary.size();++e)
              {
                const auto a=boundary[e],b=boundary[(e+1)%boundary.size()];
                const double da=nx*a.x+nz*a.z-at,db=nx*b.x+nz*b.z-at;
                if(std::abs(da)<1e-7) stations.push_back(along.x*a.x+along.z*a.z);
                if((da<0 && db>0) || (da>0 && db<0))
                {
                  const double t=da/(da-db);
                  stations.push_back(along.x*(a.x+(b.x-a.x)*t)+along.z*(a.z+(b.z-a.z)*t));
                }
              }
              std::sort(stations.begin(),stations.end());
              stations.erase(std::unique(stations.begin(),stations.end(),[](double a,double b){return std::abs(a-b)<1e-6;}),stations.end());
              // Intersect the centreline itself with the clipped ribbon.
              // Bounding-box diagonals overshoot oblique ends; disjoint
              // intervals in a concave block must not become a shortcut.
              for(size_t j=1;j<stations.size();++j)
              {
                const double begin=stations[j-1],end=stations[j],mid=(begin+end)*.5;
                if(!insideEvenOdd(boundary,{nx*at+along.x*mid,0,nz*at+along.z*mid})) continue;
                r.lanes.push_back({{nx*at+along.x*begin,0,nz*at+along.z*begin},
                                   {nx*at+along.x*end,0,nz*at+along.z*end},halfWidth,avenue});
              }
            }
          }

          for (size_t h = 0; h < halves.size(); ++h)
          {
            divide(halves[h], depth + 1, key * 2 + 1 + h);
          }
        };

        for (const auto &piece : pieces)
        {
          if (geom2::areaMetres(piece) < 24)
          {
            continue;
          }

          divide(piece, 0, members.front()->id * 131 + settled.size());
        }

        for (const auto &piece : settled)
        {
          if (geom2::areaMetres(piece) < 24)
          {
            continue;
          }

          TownBlock block;
          block.patch = members.front()->id;
          block.withinWalls = members.front()->withinWalls;
          block.outskirts = members.front()->outskirts;
          block.use = members.front()->use;
          block.dissolved = true;

          for (auto p : piece.outer)
          {
            block.ring.push_back({geom2::toMetres(p.x), 0, geom2::toMetres(p.y)});
          }

          if (block.ring.size() < 3)
          {
            continue;
          }

          if (signedArea(block.ring) < 0)
          {
            std::reverse(block.ring.begin(), block.ring.end());
          }

          // Outskirts thinning wants the edges buildings gather along and a
          // density per vertex; on a dissolved block those are its own
          // street edges and its corners' standing.
          if (block.outskirts)
          {
            for (size_t i = 0; i < block.ring.size(); ++i)
            {
              const Position a = block.ring[i], b = block.ring[(i + 1) % block.ring.size()];

              if (onArtery(a, b) || onWall(a, b))
              {
                block.populated.push_back({a, b, 1});
              }
              else
              {
                block.populated.push_back({a, b, .4});
              }
            }

            for (size_t i = 0; i < block.ring.size(); ++i)
            {
              bool gate = false;

              for (auto g : s.gates)
              {
                gate = gate || distance(g, block.ring[i]) < 3;
              }

              block.vertexDensity.push_back(gate ? 1. : 2. * random(members.front()->id * 131 + i, 104));
            }
          }

          r.blocks.push_back(std::move(block));
        }
      }
    }

    ledger.add(LedgerKind::Repaired,
               0,
               "town blocks: " + std::to_string(r.blocks.size()) + " blocks from " + std::to_string(built.size()) + " patches in " + std::to_string(groups.size()) + " groups (" + std::to_string(merged) +
                   " merged), " + std::to_string(r.lanes.size()) + " lanes, " + std::to_string(r.yards) + " compound yards, " + std::to_string(failures) + " street subtractions refused");

    return true;
  }

  for (const auto &patch : s.patches)
  {
    if (!(patch.withinWalls || patch.outskirts))
    {
      continue;
    }

    // A compound's own patch is its yard.
    bool yard = false;

    for (const auto &compound : s.compounds)
    {
      Position c{0, 0, 0};

      for (auto v : compound)
      {
        c.x += v.x / compound.size();
        c.z += v.z / compound.size();
      }

      yard |= strictlyInsideConvex(patch.ring, c);
    }

    if (yard)
    {
      ++r.yards;
      continue;
    }

    // Pieces: the patch cut by every artery segment running through it.
    std::vector<ConvexRing> pieces{patch.ring};

    for (const auto &artery : s.arteries)
    {
      for (size_t i = 1; i < artery.size(); ++i)
      {
        const Position sa = artery[i - 1], sb = artery[i];
        const double l = distance(sa, sb);

        if (l < 1e-6)
        {
          continue;
        }

        std::vector<ConvexRing> next;

        for (const auto &piece : pieces)
        {
          if (!segmentThrough(piece, sa, sb))
          {
            next.push_back(piece);
            continue;
          }

          const double nx = (sb.z - sa.z) / l, nz = (sa.x - sb.x) / l, c = nx * sa.x + nz * sa.z;
          ConvexRing left = clipHalfPlane(piece, nx, nz, c), right = clipHalfPlane(piece, -nx, -nz, -c);
          ++r.cuts;

          for (auto &half : {left, right})
          {
            if (half.size() >= 3 && signedArea(half) > 1)
            {
              next.push_back(half);
            }
          }
        }

        pieces = std::move(next);
      }
    }

    // Reserve circulation before assigning lots: frontage on just the coarse
    // patch perimeter leaves deep urban land unreachable. Bisect in the local
    // edge frame, retaining shared split edges so the ordinary street inset
    // below reserves both halves of a continuous, connected lane.
    using rewrite::DistrictUse;
    std::vector<std::pair<Position,Position>> commercialCuts;
    const auto use=DistrictUse(patch.use);
    const bool dense=patch.withinWalls && !patch.outskirts &&
      (use==DistrictUse::Residential || use==DistrictUse::Merchant ||
       use==DistrictUse::Craftsmen || use==DistrictUse::WorkerHousing || use==DistrictUse::Guild);
    if (dense)
    {
      std::vector<ConvexRing> refined;
      std::function<void(const ConvexRing &,uint32_t,uint64_t)> divide;
      divide=[&](const ConvexRing &piece,uint32_t depth,uint64_t key) {
        // Orient by the longest boundary edge, not global X/Z or the root
        // Voronoi seed. Split across the longest span of that local frame.
        double ex=1,ez=0,longest=0;
        for (size_t i=0;i<piece.size();++i) {
          const auto a=piece[i],b=piece[(i+1)%piece.size()];
          const double length=distance(a,b);
          if (length>longest) { longest=length; ex=(b.x-a.x)/length; ez=(b.z-a.z)/length; }
        }
        double nx=ex,nz=ez,lo=0,hi=0,span=-1;
        for (unsigned axis=0;axis<2;++axis) {
          const double ax=axis ? -ez : ex,az=axis ? ex : ez;
          double low=1e100,high=-1e100;
          for (auto p:piece) { const double t=ax*p.x+az*p.z; low=std::min(low,t); high=std::max(high,t); }
          if (high-low>span) { span=high-low; nx=ax; nz=az; lo=low; hi=high; }
        }
        const double angle=(random(key,173)-.5)*.5;
        const double rotatedX=nx*std::cos(angle)-nz*std::sin(angle);
        nz=nx*std::sin(angle)+nz*std::cos(angle);nx=rotatedX;
        lo=1e100;hi=-1e100;
        for(auto p:piece){const double t=nx*p.x+nz*p.z;lo=std::min(lo,t);hi=std::max(hi,t);}
        span=hi-lo;
        const bool oversized=(s.maximumInteriorBlockArea>0 && signedArea(piece)>s.maximumInteriorBlockArea) ||
          (s.maximumInteriorBlockSpan>0 && span>s.maximumInteriorBlockSpan);
        if (oversized && depth<10 && span>24+2*s.regularStreet) {
          const double c=lo+(hi-lo)*(.46+.08*random(key,171));
          auto a=clipHalfPlane(piece,nx,nz,c),b=clipHalfPlane(piece,-nx,-nz,-c);
          if (a.size()>=3 && b.size()>=3 && signedArea(a)>160 && signedArea(b)>160) {
            if (depth==0 && (patch.plaza || random(key,172)<.4)) {
              std::vector<Position> ends;
              for (auto p:a) if (std::abs(nx*p.x+nz*p.z-c)<1e-5) ends.push_back(p);
              if (ends.size()==2 && distance(ends[0],ends[1])>25) commercialCuts.push_back({ends[0],ends[1]});
            }
            ++r.cuts;
            divide(a,depth+1,key*2+1); divide(b,depth+1,key*2+2);
            return;
          }
        }
        refined.push_back(piece);
      };
      for (size_t i=0;i<pieces.size();++i) divide(pieces[i],0,patch.id*131+i);
      pieces=std::move(refined);
    }

    for (const auto &piece : pieces)
    {
      TownBlock block;
      block.patch = patch.id;
      block.withinWalls = patch.withinWalls;
      block.outskirts = patch.outskirts;
      block.use = patch.use;

      // getCityBlock: every edge moved inward by its clearance.
      ConvexRing ring = piece;

      for (size_t i = 0; i < piece.size() && !ring.empty(); ++i)
      {
        const Position a = piece[i], b = piece[(i + 1) % piece.size()];
        const double l = distance(a, b);

        if (l < 1e-9)
        {
          continue;
        }

        double inset = patch.withinWalls ? s.regularStreet * .5 : s.alley * .5;
        bool promoted=false;
        for (const auto &[from,to]:commercialCuts)
          promoted=promoted || alongEdge(from,to,a,b,.05);

        if (onWall(a, b) || onPlaza(a, b))
        {
          inset = s.mainStreet * .5;
        }
        else if (onArtery(a, b))
        {
          inset = s.arteryHalfWidth;
        }
        if (promoted) inset=std::max(inset,s.arteryHalfWidth);

        // An edge no street runs along is not inset and carries no lane: the
        // land simply continues into the next cell, and the two are one block.
        if (!claimed(patch, a, b) && !promoted)
        {
          continue;
        }

        const double nx = (b.z - a.z) / l, nz = (a.x - b.x) / l; // Outward for CCW.
        if (!onWall(a,b) && !onArtery(a,b))
          r.lanes.push_back({a,b,inset,promoted});
        ring = clipHalfPlane(ring, nx, nz, nx * a.x + nz * a.z - inset);
      }

      // At an arterial endpoint the round road junction reaches into the
      // neighbouring block corner even when neither of its edges is arterial.
      // Reserve that corner with a tangent cut, not a wider whole-block inset.
      for (const auto &[node,radius]:junctions)
      {
        if (ring.size()<3) break;
        double nearest=1e100;
        Position centre{};
        for (size_t k=0;k<ring.size();++k) {
          nearest=std::min(nearest,segmentDistance(node,ring[k],ring[(k+1)%ring.size()]));
          centre.x+=ring[k].x/ring.size(); centre.z+=ring[k].z/ring.size();
        }
        if (nearest>=radius && !strictlyInsideConvex(ring,node)) continue;
        const double length=distance(centre,node);
        if (length<=radius) continue;
        const double nx=(node.x-centre.x)/length,nz=(node.z-centre.z)/length;
        ring=clipHalfPlane(ring,nx,nz,nx*node.x+nz*node.z-radius);
      }

      if (ring.size() < 3 || signedArea(ring) < 20)
      {
        continue;
      }

      block.ring = ring;

      // Outskirts: populated edges and vertex densities of the piece.
      if (patch.outskirts)
      {
        for (size_t i = 0; i < piece.size(); ++i)
        {
          const Position a = piece[i], b = piece[(i + 1) % piece.size()];

          if (onArtery(a, b))
          {
            block.populated.push_back({a, b, 1});
            continue;
          }

          const TownPatch *other = neighbour(patch, a, b);

          if (other != nullptr && (other->withinWalls || other->outskirts))
          {
            block.populated.push_back({a, b, enclosed(*other) ? 1. : .4});
          }
        }

        std::vector<double> pieceDensity;

        for (size_t i = 0; i < piece.size(); ++i)
        {
          const Position v = piece[i];
          bool gate = false;

          for (auto g : s.gates)
          {
            gate |= distance(g, v) < 1.5;
          }

          bool cityOnly = true;

          for (const auto &other : s.patches)
          {
            bool has = false;

            for (auto q : other.ring)
            {
              has |= distance(q, v) < 1e-3;
            }

            if (has && !(other.withinWalls || other.outskirts))
            {
              cityOnly = false;
            }
          }

          pieceDensity.push_back(gate ? 1 : cityOnly ? 2 * random(patch.id * 131 + i, 104) : 0);
        }

        for (auto v : ring)
        {
          size_t nearest = 0;

          for (size_t i = 1; i < piece.size(); ++i)
          {
            if (distance(piece[i], v) < distance(piece[nearest], v))
            {
              nearest = i;
            }
          }

          block.vertexDensity.push_back(pieceDensity[nearest]);
        }
      }

      r.blocks.push_back(std::move(block));
    }
  }

  // Every lane is known now. Reserve the complete shared network before
  // allocating lots: a cut authored in a neighbouring planning cell can cross
  // this cell after routing/smoothing. Local edge insets cannot catch it.
  // Subtract circumscribed capsules as convex half-plane pieces, retaining
  // both sides of a crossing and all positive usable remnants.
  auto network=r.lanes;
  for(const auto &path:s.curvedArteries)for(size_t k=1;k<path.size();++k)
    network.push_back({path[k-1],path[k],s.arteryHalfWidth,true});
  std::vector<TownBlock> reservedBlocks;
  for(const auto &block:r.blocks) {
    std::vector<ConvexRing> pieces{block.ring};
    for(const auto &lane:network) {
      const double length=distance(lane.a,lane.b);if(length<1e-8)continue;
      const double angle=std::atan2(lane.b.z-lane.a.z,lane.b.x-lane.a.x)+1.5707963267948966;
      std::vector<std::array<double,3>> planes;
      for(unsigned k:std::array<unsigned,16>{0,8,4,12,1,2,3,5,6,7,9,10,11,13,14,15}) {
        const double a=angle+k*6.283185307179586/16,nx=std::cos(a),nz=std::sin(a);
        planes.push_back({nx,nz,std::max(nx*lane.a.x+nz*lane.a.z,nx*lane.b.x+nz*lane.b.z)+lane.halfWidth});
      }
      std::vector<ConvexRing> next;
      for(const auto &piece:pieces) {
        auto intersection=piece;
        for(const auto &p:planes){intersection=clipHalfPlane(intersection,p[0],p[1],p[2]);if(intersection.size()<3)break;}
        if(intersection.size()<3 || signedArea(intersection)<1e-8){next.push_back(piece);continue;}
        auto remainder=piece;
        for(const auto &p:planes) {
          auto outside=clipHalfPlane(remainder,-p[0],-p[1],-p[2]);
          if(outside.size()>=3 && signedArea(outside)>=20)next.push_back(std::move(outside));
          remainder=clipHalfPlane(remainder,p[0],p[1],p[2]);if(remainder.size()<3)break;
        }
      }
      pieces=std::move(next);if(pieces.empty())break;
    }
    for(auto &piece:pieces) {
      auto accepted=block;accepted.ring=std::move(piece);accepted.vertexDensity.clear();
      if(!block.vertexDensity.empty())for(auto p:accepted.ring) {
        size_t nearest=0;for(size_t k=1;k<block.ring.size();++k)if(distance(p,block.ring[k])<distance(p,block.ring[nearest]))nearest=k;
        accepted.vertexDensity.push_back(block.vertexDensity[nearest]);
      }
      reservedBlocks.push_back(std::move(accepted));
    }
  }
  r.blocks=std::move(reservedBlocks);

  ledger.add(
      LedgerKind::Repaired,
      0,
      "town blocks: " + std::to_string(r.blocks.size()) + " blocks from " + std::to_string(s.patches.size()) + " patches, " + std::to_string(r.cuts) + " road cuts, " + std::to_string(r.yards) + " compound yards");
  return true;
}

bool TownBlocks::run(const TownBlocksInput &input, TownBlocksPlan &output, Ledger &ledger, std::string *error)
{
  return plan(input, output, ledger, error);
}

} // namespace procedural::world::generator
