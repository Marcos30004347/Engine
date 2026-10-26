#include "TownStreets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <queue>
#include <set>

namespace procedural::world::generator {

namespace {

using Key = TilingKey;

double distance(Position a, Position b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
}

Key keyOf(Position p)
{
  // Shared vertices coincide to well under a millimetre; the key rounds to one.
  return {int64_t(std::llround(p.x * 1e3)), int64_t(std::llround(p.z * 1e3))};
}

struct Graph
{
  std::vector<Position> points;
  std::map<Key, uint32_t> index;
  std::vector<std::vector<std::pair<uint32_t, double>>> links;

  uint32_t node(Position p)
  {
    const Key k = keyOf(p);
    auto it = index.find(k);

    if (it != index.end())
    {
      return it->second;
    }

    const uint32_t n = uint32_t(points.size());
    index[k] = n;
    points.push_back(p);
    links.emplace_back();
    return n;
  }

  void link(uint32_t a, uint32_t b)
  {
    if (a == b)
    {
      return;
    }

    for (auto &[c, w] : links[a])
    {
      if (c == b)
      {
        return;
      }
    }

    const double w = distance(points[a], points[b]);
    links[a].push_back({b, w});
    links[b].push_back({a, w});
  }

  // Dijkstra over the nodes not excluded and not blocked (the ends are always allowed).
  std::vector<uint32_t> path(uint32_t from, uint32_t to, const std::vector<uint8_t> &blocked, const std::vector<uint8_t> &excluded) const
  {
    std::vector<double> cost(points.size(), std::numeric_limits<double>::infinity());
    std::vector<uint32_t> previous(points.size(), UINT32_MAX);
    using Entry = std::pair<double, uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
    cost[from] = 0;
    queue.push({0, from});

    while (!queue.empty())
    {
      auto [c, i] = queue.top();
      queue.pop();

      if (c != cost[i])
      {
        continue;
      }

      if (i == to)
      {
        break;
      }

      for (auto [j, w] : links[i])
      {
        if (j != to && (blocked[j] || excluded[j]))
        {
          continue;
        }

        if (c + w < cost[j])
        {
          cost[j] = c + w;
          previous[j] = i;
          queue.push({c + w, j});
        }
      }
    }

    std::vector<uint32_t> result;

    if (!std::isfinite(cost[to]))
    {
      return result;
    }

    for (uint32_t i = to; i != UINT32_MAX; i = previous[i])
    {
      result.push_back(i);

      if (i == from)
      {
        break;
      }
    }

    std::reverse(result.begin(), result.end());
    return result;
  }
};

} // namespace

TownStreets::TownStreets(uint64_t domain) : Generator("town.streets", domain, "town.streets-input", "town.streets-plan", GenerationScope::Patch)
{
}

void TownStreets::remap(const TownStreetsPlan &plan, ConvexRing &polygon)
{
  for (auto &p : polygon)
  {
    for (const auto &[from, to] : plan.moves)
    {
      if (distance(p, from) < 1e-6)
      {
        p = to;
        break;
      }
    }
  }
}

bool TownStreets::plan(const TownStreetsInput &s, TownStreetsPlan &r, Ledger &ledger, std::string *error)
{
  auto fail = [&](const std::string &message) {
    ledger.add(LedgerKind::Rejected, 0, "town streets: " + message);

    if (error != nullptr)
    {
      *error = message;
    }

    return false;
  };

  r = TownStreetsPlan{};

  if (s.patches.empty() || s.border.size() < 3 || s.gates.empty())
  {
    return fail("streets need patches, a border and gates");
  }

  // The graph: every patch vertex a node, every patch edge a link; a vertex
  // lying on another patch's edge (a T junction) links along that edge too.
  Graph graph;

  for (const auto &patch : s.patches)
  {
    for (auto v : patch.ring)
    {
      graph.node(v);
    }
  }

  for (const auto &patch : s.patches)
  {
    const size_t n = patch.ring.size();

    for (size_t i = 0; i < n; ++i)
    {
      const Position a = patch.ring[i], b = patch.ring[(i + 1) % n];
      const double l = distance(a, b);

      if (l < 1e-9)
      {
        continue;
      }

      // Every node on this edge, in order along it.
      std::vector<std::pair<double, uint32_t>> along;

      for (uint32_t k = 0; k < graph.points.size(); ++k)
      {
        const Position p = graph.points[k];
        const double t = ((p.x - a.x) * (b.x - a.x) + (p.z - a.z) * (b.z - a.z)) / (l * l);

        if (t < -1e-6 || t > 1 + 1e-6)
        {
          continue;
        }

        if (std::abs(cross(a, b, p)) / l < 1e-3)
        {
          along.push_back({t, k});
        }
      }

      std::sort(along.begin(), along.end());

      for (size_t k = 1; k < along.size(); ++k)
      {
        graph.link(along[k - 1].second, along[k].second);
      }
    }
  }

  // Gates snap to the nearest border vertex (Watabou's gates are wall vertices).
  std::vector<uint32_t> gateNodes;

  for (auto gate : s.gates)
  {
    Position best = s.border[0];

    for (auto v : s.border)
    {
      if (distance(v, gate) < distance(best, gate))
      {
        best = v;
      }
    }

    r.gates.push_back(best);
    gateNodes.push_back(graph.node(best));
  }

  // Blocked: border vertices except the gates. Inner: vertices of walled
  // patches; outer: vertices of the other patches; neither on the border.
  std::vector<uint8_t> blocked(graph.points.size(), 0), inner(graph.points.size(), 0), outer(graph.points.size(), 0), onBorder(graph.points.size(), 0);

  // On the border geometrically: the circumference drops collinear vertices,
  // so a wall vertex may not be a border vertex while still lying on the wall.
  for (uint32_t k = 0; k < graph.points.size(); ++k)
  {
    if (onRing(s.border, graph.points[k], 1e-3))
    {
      onBorder[k] = 1;
      blocked[k] = 1;
    }
  }

  for (auto g : gateNodes)
  {
    blocked[g] = 0;
  }

  const TownPatch *plaza = nullptr;

  for (const auto &patch : s.patches)
  {
    if (patch.plaza)
    {
      plaza = &patch;
    }

    for (auto v : patch.ring)
    {
      const uint32_t n = graph.node(v);

      if (onBorder[n])
      {
        continue;
      }

      (patch.withinWalls ? inner : outer)[n] = 1;
    }
  }

  if (std::getenv("CITY_DEBUG"))
  {
    size_t links = 0, blockedCount = 0, innerCount = 0, outerCount = 0, borderCount = 0;

    for (uint32_t k = 0; k < graph.points.size(); ++k)
    {
      links += graph.links[k].size();
      blockedCount += blocked[k];
      innerCount += inner[k];
      outerCount += outer[k];
      borderCount += onBorder[k];
    }

    std::fprintf(
        stderr,
        "town streets: %zu nodes, %zu links, %zu on border, %zu blocked, %zu inner, %zu outer, plaza %d\n",
        graph.points.size(),
        links / 2,
        borderCount,
        blockedCount,
        innerCount,
        outerCount,
        int(plaza != nullptr));

    for (size_t g = 0; g < gateNodes.size(); ++g)
    {
      std::fprintf(stderr, "town streets: gate %zu node %u links %zu snapped %.1f m\n", g, gateNodes[g], graph.links[gateNodes[g]].size(), distance(s.gates[g], graph.points[gateNodes[g]]));
    }
  }

  // Streets: every gate to the nearest plaza vertex (or the centre) over
  // inner vertices; roads: from the outer vertex nearest the gate's exit (or
  // far out along the gate's direction) back to the gate over outer vertices.
  std::vector<std::vector<uint32_t>> streets, roads;

  for (size_t g = 0; g < gateNodes.size(); ++g)
  {
    const Position gate = graph.points[gateNodes[g]];
    uint32_t end = UINT32_MAX;
    double nearest = std::numeric_limits<double>::infinity();

    if (plaza != nullptr)
    {
      for (auto v : plaza->ring)
      {
        const double d = distance(v, gate);

        if (d < nearest)
        {
          nearest = d;
          end = graph.node(v);
        }
      }
    }
    else
    {
      for (uint32_t k = 0; k < graph.points.size(); ++k)
      {
        const double d = distance(graph.points[k], s.centre);

        if (inner[k] && d < nearest)
        {
          nearest = d;
          end = k;
        }
      }
    }

    if (end == UINT32_MAX)
    {
      ++r.failed;
      continue;
    }

    auto street = graph.path(gateNodes[g], end, blocked, outer);

    if (street.empty())
    {
      ++r.failed;
      ledger.add(LedgerKind::Omitted, 0, "town streets: gate " + std::to_string(g) + " has no street to the plaza");
      continue;
    }

    // A gate on the plaza itself has a trivial street: no segments, still a gate.
    if (street.size() >= 2)
    {
      streets.push_back(street);
    }

    ++r.streets;

    Position target;

    if (g < s.exits.size())
    {
      target = s.exits[g];
    }
    else
    {
      const double dx = gate.x - s.centre.x, dz = gate.z - s.centre.z, l = std::max(1e-9, std::hypot(dx, dz));
      target = {s.centre.x + dx / l * 1000, 0, s.centre.z + dz / l * 1000};
    }

    uint32_t start = UINT32_MAX;
    nearest = std::numeric_limits<double>::infinity();

    for (uint32_t k = 0; k < graph.points.size(); ++k)
    {
      const double d = distance(graph.points[k], target);

      if (outer[k] && !blocked[k] && d < nearest)
      {
        nearest = d;
        start = k;
      }
    }

    if (start == UINT32_MAX || start == gateNodes[g])
    {
      continue;
    }

    auto road = graph.path(start, gateNodes[g], blocked, inner);

    if (road.size() >= 2)
    {
      roads.push_back(road);
      ++r.roads;
    }
  }

  if (r.streets == 0)
  {
    return fail("no gate reaches the plaza");
  }

  // tidyUpRoads: segments once each, none along the plaza, chained.
  std::vector<std::pair<uint32_t, uint32_t>> segments;
  std::set<std::pair<uint32_t, uint32_t>> seen;

  auto onPlaza = [&](uint32_t n) {
    if (plaza == nullptr)
    {
      return false;
    }

    for (auto v : plaza->ring)
    {
      if (keyOf(v) == keyOf(graph.points[n]))
      {
        return true;
      }
    }

    return false;
  };

  auto cut = [&](const std::vector<uint32_t> &path) {
    for (size_t i = 1; i < path.size(); ++i)
    {
      const uint32_t v0 = path[i - 1], v1 = path[i];

      if (onPlaza(v0) && onPlaza(v1))
      {
        continue;
      }

      if (seen.insert({std::min(v0, v1), std::max(v0, v1)}).second)
      {
        segments.push_back({v0, v1});
      }
    }
  };

  for (const auto &street : streets)
  {
    cut(street);
  }

  for (const auto &road : roads)
  {
    cut(road);
  }

  r.segments = uint32_t(segments.size());
  std::vector<std::vector<uint32_t>> arteries;

  while (!segments.empty())
  {
    auto seg = segments.back();
    segments.pop_back();
    bool attached = false;

    for (auto &a : arteries)
    {
      if (a.front() == seg.second)
      {
        a.insert(a.begin(), seg.first);
        attached = true;
        break;
      }

      if (a.back() == seg.first)
      {
        a.push_back(seg.second);
        attached = true;
        break;
      }
    }

    if (!attached)
    {
      arteries.push_back({seg.first, seg.second});
    }
  }

  // smoothStreet: interior vertices pulled toward their neighbours along the
  // artery; the vertex is shared with the patches, which follow (the moves).
  std::vector<Position> current = graph.points;

  if (s.smoothing > 0)
  {
    for (const auto &a : arteries)
    {
      if (a.size() < 3)
      {
        continue;
      }

      std::vector<Position> smoothed(a.size());

      for (size_t i = 0; i < a.size(); ++i)
      {
        const Position v0 = current[a[(i + a.size() - 1) % a.size()]], v1 = current[a[i]], v2 = current[a[(i + 1) % a.size()]];
        smoothed[i] = {(v0.x + v1.x * s.smoothing + v2.x) / (2 + s.smoothing), 0, (v0.z + v1.z * s.smoothing + v2.z) / (2 + s.smoothing)};
      }

      for (size_t i = 1; i + 1 < a.size(); ++i)
      {
        current[a[i]] = smoothed[i];
      }
    }
  }

  for (uint32_t k = 0; k < graph.points.size(); ++k)
  {
    if (distance(current[k], graph.points[k]) > 1e-9)
    {
      r.moves.push_back({graph.points[k], current[k]});
    }
  }

  for (const auto &a : arteries)
  {
    std::vector<Position> polyline;

    for (auto n : a)
    {
      polyline.push_back(current[n]);
    }

    r.arteries.push_back(std::move(polyline));
  }

  ledger.add(
      LedgerKind::Repaired,
      0,
      "town streets: " + std::to_string(r.streets) + " streets and " + std::to_string(r.roads) + " roads from " + std::to_string(gateNodes.size()) + " gates, " + std::to_string(r.arteries.size()) + " arteries of " +
          std::to_string(r.segments) + " segments, " + std::to_string(r.moves.size()) + " vertices smoothed");
  return true;
}

bool TownStreets::run(const TownStreetsInput &input, TownStreetsPlan &output, Ledger &ledger, std::string *error)
{
  return plan(input, output, ledger, error);
}

} // namespace procedural::world::generator
