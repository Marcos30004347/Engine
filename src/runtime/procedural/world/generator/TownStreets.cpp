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
  const TownStreetsInput *settings = nullptr;
  std::set<std::pair<uint32_t, uint32_t>> built;

  double terrainCost(Position a, Position b) const
  {
    const double length = distance(a, b);
    const unsigned samples = std::max(2u, unsigned(std::ceil(length / 2.)));
    double cost = length;
    const double nx=-(b.z-a.z)/std::max(length,1e-9), nz=(b.x-a.x)/std::max(length,1e-9);
    for (double side : {-settings->corridorHalfWidth, 0., settings->corridorHalfWidth})
    {
      const Position from{a.x+nx*side,0,a.z+nz*side};
      const Position to{b.x+nx*side,0,b.z+nz*side};
      // Convex clipping tests the entire segment, including thin obstacles.
      for (const auto &obstacle : settings->obstacles)
      {
        double lo=0,hi=1;
        for(size_t e=0;e<obstacle.size() && lo<hi;++e)
        {
          const auto u=obstacle[e],v=obstacle[(e+1)%obstacle.size()];
          const double start=cross(u,v,from)-1e-6, finish=cross(u,v,to)-1e-6;
          if(start<=0 && finish<=0) { hi=lo; break; }
          if(start<0) lo=std::max(lo,-start/(finish-start));
          if(finish<0) hi=std::min(hi,-start/(finish-start));
        }
        if(lo<hi) return std::numeric_limits<double>::infinity();
      }
      if (!settings->terrain) continue;
      double previous=settings->terrain(from.x,from.z);
      if (!std::isfinite(previous)) return std::numeric_limits<double>::infinity();
      for(unsigned i=1;i<=samples;++i)
      {
        const double t=double(i)/samples;
        const double height=settings->terrain(from.x+(to.x-from.x)*t,from.z+(to.z-from.z)*t);
        const double grade=std::abs(height-previous)/std::max(1e-9,length/samples);
        if(!std::isfinite(height) || grade>settings->maximumGrade)
          return std::numeric_limits<double>::infinity();
        const double excess=std::max(0.,grade-settings->preferredGrade);
        cost+=length/samples*(4*grade*grade+24*excess*excess)/3.;
        previous=height;
      }
    }
    return cost;
  }

  void retain(const std::vector<uint32_t> &path)
  {
    for (size_t i=1; i<path.size(); ++i) built.insert(std::minmax(path[i-1],path[i]));
  }

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

    const double w = terrainCost(points[a], points[b]);
    if (!std::isfinite(w)) return;
    links[a].push_back({b, w});
    links[b].push_back({a, w});
  }

  // A* with an admissible bound scaled by the cheapest reuse multiplier;
  // blocked/excluded vertices keep exactly the same reachability semantics.
  std::vector<uint32_t> path(uint32_t from, uint32_t to, const std::vector<uint8_t> &blocked, const std::vector<uint8_t> &excluded) const
  {
    std::vector<double> cost(points.size(), std::numeric_limits<double>::infinity());
    std::vector<uint32_t> previous(points.size(), UINT32_MAX);
    using Entry = std::pair<double, uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
    cost[from] = 0;
    const auto heuristic = [&](uint32_t n) { return settings->existingStreetCost * distance(points[n], points[to]); };
    queue.push({heuristic(from), from});

    while (!queue.empty())
    {
      auto [c, i] = queue.top();
      queue.pop();

      if (c > cost[i] + heuristic(i) + 1e-9)
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

        if (built.count(std::minmax(i,j))) w *= settings->existingStreetCost;
        if (cost[i] + w < cost[j])
        {
          cost[j] = cost[i] + w;
          previous[j] = i;
          queue.push({cost[j] + heuristic(j), j});
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

  if (!std::isfinite(s.preferredGrade) || !std::isfinite(s.maximumGrade) ||
      s.preferredGrade <= 0 || s.maximumGrade < s.preferredGrade ||
      !std::isfinite(s.corridorHalfWidth) || s.corridorHalfWidth < 0 ||
      !std::isfinite(s.maximumGateRelocation) || s.maximumGateRelocation < 0 ||
      !std::isfinite(s.existingStreetCost) || s.existingStreetCost <= 0 || s.existingStreetCost > 1)
    return fail("invalid street routing costs");

  // The graph a street is routed on.
  //
  // Not just the patch boundaries. A* over the cell edges can only ever
  // return cell edges, so every street in the town was a Voronoi edge however
  // much it was afterwards smoothed. The lattice also carries, per patch, its
  // centroid and the midpoint of each of its edges, linked into a star and
  // across to its neighbours' - so a street can cut THROUGH a cell, and the
  // blocks are then cut by the street rather than the street by the blocks.
  Graph graph;
  graph.settings = &s;

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

  // The interior lattice: a centroid per patch, the midpoint of every edge,
  // and the star between them. Midpoints are shared between the two patches
  // that meet along an edge, so a route can cross from one cell to the next
  // without going round by a corner.
  for (const auto &patch : s.patches)
  {
    if (patch.ring.size() < 3)
    {
      continue;
    }

    Position centre{0, 0, 0};

    for (auto p : patch.ring)
    {
      centre.x += p.x / double(patch.ring.size());
      centre.z += p.z / double(patch.ring.size());
    }

    const uint32_t middle = graph.node(centre);

    for (size_t i = 0; i < patch.ring.size(); ++i)
    {
      const Position a = patch.ring[i], b = patch.ring[(i + 1) % patch.ring.size()];
      const Position mid{(a.x + b.x) * .5, 0, (a.z + b.z) * .5};
      const uint32_t m = graph.node(mid);
      graph.link(middle, m);
      graph.link(middle, graph.node(a));
      graph.link(m, graph.node(a));
      graph.link(m, graph.node(b));
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

    std::vector<Position> members = patch.ring;
    Position centre{0, 0, 0};

    for (auto p : patch.ring)
    {
      centre.x += p.x / double(patch.ring.size());
      centre.z += p.z / double(patch.ring.size());
    }

    members.push_back(centre);

    for (size_t i = 0; i < patch.ring.size(); ++i)
    {
      const Position a = patch.ring[i], b = patch.ring[(i + 1) % patch.ring.size()];
      members.push_back({(a.x + b.x) * .5, 0, (a.z + b.z) * .5});
    }

    for (auto v : members)
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
    Position gate = graph.points[gateNodes[g]];
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
    // The nearest plaza corner can lie behind a reserved compound or cliff.
    // Try its other entrances before declaring an otherwise reachable gate lost.
    if (street.empty() && plaza != nullptr)
    {
      auto entrances=plaza->ring;
      std::stable_sort(entrances.begin(),entrances.end(),[&](Position a,Position b){return distance(a,gate)<distance(b,gate);});
      for(auto entrance:entrances)
      {
        const auto target=graph.index.at(keyOf(entrance));
        if(target==end) continue;
        street=graph.path(gateNodes[g],target,blocked,outer);
        if(!street.empty()) break;
      }
    }

    if (street.empty() && s.maximumGateRelocation > 0)
    {
      std::vector<uint32_t> alternatives;
      for(uint32_t n=0;n<graph.points.size();++n)
        if(onBorder[n] && distance(gate,graph.points[n])<=s.maximumGateRelocation &&
           std::find(gateNodes.begin(),gateNodes.end(),n)==gateNodes.end()) alternatives.push_back(n);
      std::stable_sort(alternatives.begin(),alternatives.end(),[&](uint32_t a,uint32_t b){return distance(gate,graph.points[a])<distance(gate,graph.points[b]);});
      for(auto candidate:alternatives)
      {
        auto open=blocked; open[candidate]=0;
        street=graph.path(candidate,end,open,outer);
        if(street.empty() && plaza)
          for(auto entrance:plaza->ring) {
            street=graph.path(candidate,graph.index.at(keyOf(entrance)),open,outer);
            if(!street.empty()) break;
          }
        if(street.empty()) continue;
        ledger.add(LedgerKind::Repaired,0,"town streets: gate "+std::to_string(g)+" moved "+
                   std::to_string(distance(gate,graph.points[candidate]))+" m to a reachable boundary entrance");
        blocked[gateNodes[g]]=1; blocked[candidate]=0;
        gateNodes[g]=candidate; gate=graph.points[candidate]; r.gates[g]=gate;
        break;
      }
    }

    if (street.empty())
    {
      ++r.failed;
      ledger.add(LedgerKind::Omitted, 0, "town streets: gate " + std::to_string(g) + " has no street to the plaza");
      continue;
    }

    // A gate on the plaza itself has a trivial street: no segments, still a gate.
    if (street.size() >= 2)
    {
      graph.retain(street);
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
      graph.retain(road);
      roads.push_back(road);
      ++r.roads;
    }
  }

  if (r.streets == 0)
  {
    return fail("no gate reaches the plaza");
  }

  // The anchors, the same way: a path to the plaza over inner vertices. An
  // anchor on the border is allowed to start there even though the border is
  // blocked - it is a starting point, not a way through.
  for (auto anchor : s.anchors)
  {
    uint32_t from = UINT32_MAX;
    double nearest = std::numeric_limits<double>::infinity();

    for (uint32_t k = 0; k < graph.points.size(); ++k)
    {
      const double d = distance(graph.points[k], anchor);

      if (d < nearest && (inner[k] || onBorder[k]))
      {
        nearest = d;
        from = k;
      }
    }

    if (from == UINT32_MAX || nearest > 30)
    {
      continue;
    }

    uint32_t end = UINT32_MAX;
    nearest = std::numeric_limits<double>::infinity();

    if (plaza != nullptr)
    {
      for (auto v : plaza->ring)
      {
        const double d = distance(v, graph.points[from]);

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

    if (end == UINT32_MAX || end == from)
    {
      continue;
    }

    std::vector<uint8_t> open = blocked;
    open[from] = 0;
    const auto street = graph.path(from, end, open, outer);

    if (street.size() >= 2)
    {
      graph.retain(street);
      streets.push_back(street);
      ++r.streets;
    }
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

  // Chain the undirected network by degree, not input orientation/order.
  // The old greedy append left bend vertices at artificial chain ends where
  // smoothing could never touch them, including the gate approaches.
  std::vector<std::vector<uint32_t>> adjacency(graph.points.size());
  std::set<std::pair<uint32_t,uint32_t>> remaining;
  for (auto [a,b]:segments)
  { adjacency[a].push_back(b);adjacency[b].push_back(a);remaining.insert(std::minmax(a,b)); }
  const auto trace=[&](uint32_t from,uint32_t to)
  {
    std::vector<uint32_t> chain{from};
    while (remaining.erase(std::minmax(from,to)))
    {
      chain.push_back(to);
      if (adjacency[to].size()!=2) break;
      const auto next=adjacency[to][0]==from ? adjacency[to][1] : adjacency[to][0];
      from=to;to=next;
    }
    if (chain.size()>1) arteries.push_back(std::move(chain));
  };
  for (uint32_t n=0;n<adjacency.size();++n)
    if (adjacency[n].size()!=2) for (auto to:adjacency[n]) trace(n,to);
  while (!remaining.empty()) {const auto edge=*remaining.begin();trace(edge.first,edge.second);}

  // smoothStreet: interior vertices pulled toward their neighbours along the
  // artery; the vertex is shared with the patches, which follow (the moves).
  std::vector<Position> current = graph.points;

  // Relax the shared layout BEFORE lot subdivision, not just its rendered
  // ribbons. Protect the convex patch contract and fixed wall/gate anchors.
  std::vector<std::vector<uint32_t>> rings;
  std::vector<std::vector<size_t>> incident(current.size());
  for (const auto &patch : s.patches)
  {
    std::vector<uint32_t> ring;
    for (auto p : patch.ring)
    {
      const auto n = graph.index.at(keyOf(p));
      ring.push_back(n);
      incident[n].push_back(rings.size());
    }
    rings.push_back(std::move(ring));
  }
  std::set<uint32_t> fixed;
  for (auto p : s.border)
    if (graph.index.count(keyOf(p))) fixed.insert(graph.index.at(keyOf(p)));
  for (auto n : gateNodes) fixed.insert(n);
  for (auto p : s.fixedVertices)
    if(graph.index.count(keyOf(p)))fixed.insert(graph.index.at(keyOf(p)));
  for (uint32_t n = 0; n < current.size(); ++n)
    // Unsplit T-junction edge. A node that belongs to no cell at all is an
    // interior lattice point instead: nothing shares it, so moving it can
    // break nothing, and pinning it froze every street that cut through a
    // cell rather than round it.
    if (!incident[n].empty() && incident[n].size() < graph.links[n].size()) fixed.insert(n);

  if (s.smoothing > 0)
  {
    for (uint32_t pass = 0; pass < 4; ++pass)
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
        const auto n = a[i];
        if (fixed.count(n)) continue;
        const auto old = current[n];
        // Backtracking keeps shared cells convex and non-collapsed. All
        // incident patches see the SAME accepted coordinate.
        for (double weight = 1.; weight >= .0625; weight *= .5)
        {
          current[n] = {old.x + (smoothed[i].x-old.x)*weight, old.y,
                        old.z + (smoothed[i].z-old.z)*weight};
          bool valid = true;
          // A visual relaxation must not undo the route's terrain/obstacle decision.
          for (auto neighbour : adjacency[n])
            valid = valid && graph.terrainCost(current[n], current[neighbour]) <=
                graph.terrainCost(old, current[neighbour]) * 1.05 + 1e-9;
          for (auto r : incident[n])
          {
            const auto &ring = rings[r];
            for (size_t k = 0; k < ring.size(); ++k)
            {
              auto p = current[ring[k]], q = current[ring[(k+1)%ring.size()]], t = current[ring[(k+2)%ring.size()]];
              valid = valid && distance(p,q) > .1 &&
                      (q.x-p.x)*(t.z-q.z)-(q.z-p.z)*(t.x-q.x) >= -1e-7;
            }
          }
          if (valid) break;
          current[n] = old;
        }
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
