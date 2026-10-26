#include "RoadGraph.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace procedural::world::generator {

namespace {

double distance(Position a, Position b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
}

// Parameters (t along a-b, u along c-d) of the crossing of two segments, if they cross properly or touch within tolerance.
bool crossing(Position a, Position b, Position c, Position d, double tolerance, double &t, double &u)
{
  const double dx1 = b.x - a.x, dz1 = b.z - a.z, dx2 = d.x - c.x, dz2 = d.z - c.z;
  const double denominator = dx1 * dz2 - dz1 * dx2;

  if (std::abs(denominator) < 1e-12)
  {
    return false;
  }

  t = ((c.x - a.x) * dz2 - (c.z - a.z) * dx2) / denominator;
  u = ((c.x - a.x) * dz1 - (c.z - a.z) * dx1) / denominator;
  const double l1 = std::max(1e-9, distance(a, b)), l2 = std::max(1e-9, distance(c, d));
  const double e1 = tolerance / l1, e2 = tolerance / l2;
  return t > -e1 && t < 1 + e1 && u > -e2 && u < 1 + e2;
}

} // namespace

RoadGraph::RoadGraph(double snapMetres) : snap_(std::max(1e-6, snapMetres))
{
}

void RoadGraph::link(uint32_t a, uint32_t b, uint8_t type, double halfWidth)
{
  if (a == b)
  {
    return;
  }

  for (auto e : adjacency_[a])
  {
    if ((edges_[e].a == a && edges_[e].b == b) || (edges_[e].a == b && edges_[e].b == a))
    {
      return;
    }
  }

  edges_.push_back({a, b, type, halfWidth});
  adjacency_[a].push_back(uint32_t(edges_.size() - 1));
  adjacency_[b].push_back(uint32_t(edges_.size() - 1));
}

void RoadGraph::splitEdge(uint32_t edge, uint32_t at)
{
  const RoadGraphEdge old = edges_[edge];

  if (old.a == at || old.b == at)
  {
    return;
  }

  // The old edge keeps its slot as (a, at); (at, b) is a new edge.
  edges_[edge].b = at;
  auto &listB = adjacency_[old.b];
  listB.erase(std::remove(listB.begin(), listB.end(), edge), listB.end());
  adjacency_[at].push_back(edge);
  edges_.push_back({at, old.b, old.type, old.halfWidth});
  adjacency_[at].push_back(uint32_t(edges_.size() - 1));
  adjacency_[old.b].push_back(uint32_t(edges_.size() - 1));
}

uint32_t RoadGraph::vertex(Position p)
{
  for (uint32_t i = 0; i < vertices_.size(); ++i)
  {
    if (distance(vertices_[i], p) <= snap_)
    {
      return i;
    }
  }

  // On an edge's interior: split it there.
  for (uint32_t e = 0; e < edges_.size(); ++e)
  {
    const Position a = vertices_[edges_[e].a], b = vertices_[edges_[e].b];

    if (segmentDistance(p, a, b) <= snap_ && distance(p, a) > snap_ && distance(p, b) > snap_)
    {
      const double l = std::max(1e-9, distance(a, b));
      const double t = ((p.x - a.x) * (b.x - a.x) + (p.z - a.z) * (b.z - a.z)) / (l * l);
      const Position on{a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t};
      vertices_.push_back(on);
      adjacency_.emplace_back();
      const uint32_t at = uint32_t(vertices_.size() - 1);
      splitEdge(e, at);
      return at;
    }
  }

  vertices_.push_back(p);
  adjacency_.emplace_back();
  return uint32_t(vertices_.size() - 1);
}

uint32_t RoadGraph::add(Position a, Position b, uint8_t type, double halfWidth)
{
  if (distance(a, b) <= snap_)
  {
    return 0;
  }

  const uint32_t start = vertex(a), end = vertex(b);

  if (start == end)
  {
    return 0;
  }

  // Every crossing with an existing edge, in order along the segment.
  struct Hit
  {
    double t;
    uint32_t vertex;
  };

  std::vector<Hit> hits;
  const Position pa = vertices_[start], pb = vertices_[end];
  const size_t edgeCount = edges_.size();

  for (uint32_t e = 0; e < edgeCount; ++e)
  {
    const RoadGraphEdge edge = edges_[e];

    if (edge.a == start || edge.b == start || edge.a == end || edge.b == end)
    {
      continue;
    }

    double t = 0, u = 0;

    if (!crossing(pa, pb, vertices_[edge.a], vertices_[edge.b], snap_, t, u))
    {
      continue;
    }

    const Position at{pa.x + (pb.x - pa.x) * t, 0, pa.z + (pb.z - pa.z) * t};
    uint32_t v;

    if (u <= snap_ / std::max(1e-9, distance(vertices_[edge.a], vertices_[edge.b])))
    {
      v = edge.a;
    }
    else if (u >= 1 - snap_ / std::max(1e-9, distance(vertices_[edge.a], vertices_[edge.b])))
    {
      v = edge.b;
    }
    else
    {
      vertices_.push_back(at);
      adjacency_.emplace_back();
      v = uint32_t(vertices_.size() - 1);
      splitEdge(e, v);
    }

    hits.push_back({t, v});
  }

  std::sort(hits.begin(), hits.end(), [](const Hit &x, const Hit &y) {
    return x.t < y.t;
  });

  uint32_t previous = start, pieces = 0;

  for (const auto &hit : hits)
  {
    if (hit.vertex != previous)
    {
      link(previous, hit.vertex, type, halfWidth);
      ++pieces;
      previous = hit.vertex;
    }
  }

  if (previous != end)
  {
    link(previous, end, type, halfWidth);
    ++pieces;
  }

  return pieces;
}

bool RoadGraph::slivers(Position a, Position b, double clearance) const
{
  const double l = distance(a, b);

  if (l < 1e-9)
  {
    return false;
  }

  for (const auto &edge : edges_)
  {
    const Position c = vertices_[edge.a], d = vertices_[edge.b];
    const double m = distance(c, d);

    if (m < 1e-9)
    {
      continue;
    }

    // Nearly parallel and overlapping in projection, closer than the clearance.
    const double parallel = std::abs((b.x - a.x) * (d.z - c.z) - (b.z - a.z) * (d.x - c.x)) / (l * m);

    if (parallel > .25)
    {
      continue;
    }

    const double t0 = ((c.x - a.x) * (b.x - a.x) + (c.z - a.z) * (b.z - a.z)) / (l * l);
    const double t1 = ((d.x - a.x) * (b.x - a.x) + (d.z - a.z) * (b.z - a.z)) / (l * l);

    if (std::max(t0, t1) < .1 || std::min(t0, t1) > .9)
    {
      continue;
    }

    const double gap = std::min(std::abs(cross(a, b, c)) / l, std::abs(cross(a, b, d)) / l);

    if (gap > 1e-6 && gap < clearance)
    {
      return true;
    }
  }

  return false;
}

bool RoadGraph::firstHit(Position origin, double dx, double dz, double reach, Position &hit) const
{
  double best = reach;
  bool found = false;

  for (const auto &edge : edges_)
  {
    const Position a = vertices_[edge.a], b = vertices_[edge.b];

    if (distance(a, origin) <= snap_ || distance(b, origin) <= snap_)
    {
      continue;
    }

    const double ex = b.x - a.x, ez = b.z - a.z;
    const double denominator = dx * ez - dz * ex;

    if (std::abs(denominator) < 1e-12)
    {
      continue;
    }

    const double t = ((a.x - origin.x) * ez - (a.z - origin.z) * ex) / denominator;
    const double u = ((a.x - origin.x) * dz - (a.z - origin.z) * dx) / denominator;

    if (t <= 1e-6 || u < -1e-9 || u > 1 + 1e-9 || t >= best)
    {
      continue;
    }

    best = t;
    found = true;
    hit = {origin.x + dx * t, 0, origin.z + dz * t};
  }

  return found;
}

std::vector<RoadGraphFace> RoadGraph::faces() const
{
  std::vector<RoadGraphFace> result;
  // Visited directed half-edges: edge index * 2 + direction (0: a->b, 1: b->a).
  std::vector<uint8_t> visited(edges_.size() * 2, 0);

  auto other = [&](uint32_t e, uint32_t v) {
    return edges_[e].a == v ? edges_[e].b : edges_[e].a;
  };

  auto angle = [&](uint32_t from, uint32_t to) {
    return std::atan2(vertices_[to].z - vertices_[from].z, vertices_[to].x - vertices_[from].x);
  };

  for (uint32_t e0 = 0; e0 < edges_.size(); ++e0)
  {
    for (int direction = 0; direction < 2; ++direction)
    {
      if (visited[e0 * 2 + direction])
      {
        continue;
      }

      uint32_t edge = e0;
      uint32_t from = direction == 0 ? edges_[e0].a : edges_[e0].b;
      uint32_t to = other(e0, from);
      RoadGraphFace face;
      std::vector<uint32_t> walk{from};
      bool closed = false;

      for (size_t guard = 0; guard < edges_.size() * 2 + 2; ++guard)
      {
        const int dir = edges_[edge].a == from ? 0 : 1;

        if (visited[edge * 2 + dir])
        {
          break;
        }

        visited[edge * 2 + dir] = 1;
        face.edges.push_back(edge);
        walk.push_back(to);

        // At `to`, coming from `from`: the most clockwise outgoing edge from
        // the reversed incoming direction keeps the face on the left.
        const double back = angle(to, from);
        uint32_t next = UINT32_MAX;
        double best = 1e30;

        for (auto e : adjacency_[to])
        {
          const uint32_t w = other(e, to);

          if (w == from && adjacency_[to].size() > 1)
          {
            continue;
          }

          double turn = back - angle(to, w); // Clockwise angle from the reverse direction.

          while (turn <= 1e-9)
          {
            turn += 6.283185307179586;
          }

          while (turn > 6.283185307179586)
          {
            turn -= 6.283185307179586;
          }

          if (turn < best)
          {
            best = turn;
            next = e;
          }
        }

        if (next == UINT32_MAX)
        {
          break;
        }

        from = to;
        to = other(next, from);
        edge = next;

        if (from == walk.front() && edge == e0 && (edges_[e0].a == from ? 0 : 1) == direction)
        {
          closed = true;
          break;
        }
      }

      if (!closed || walk.size() < 4)
      {
        continue;
      }

      walk.pop_back(); // The walk returned to its start.

      // Fold spurs (u, w, u) out of the ring, repeatedly.
      std::vector<uint32_t> ring = walk;
      bool folded = true;

      while (folded && ring.size() >= 3)
      {
        folded = false;

        for (size_t i = 0; i < ring.size(); ++i)
        {
          const uint32_t u = ring[i], w = ring[(i + 1) % ring.size()], x = ring[(i + 2) % ring.size()];

          if (u == x)
          {
            // Remove w and one of the u's.
            ring.erase(ring.begin() + ((i + 1) % ring.size()));
            const size_t j = i < ring.size() ? i : 0;
            ring.erase(ring.begin() + ((j + 1) % ring.size()));
            folded = true;
            break;
          }
        }
      }

      if (ring.size() < 3)
      {
        continue;
      }

      for (auto v : ring)
      {
        face.ring.push_back(vertices_[v]);
      }

      face.vertices = ring;
      face.area = signedArea(face.ring);

      if (face.area > 1)
      {
        result.push_back(std::move(face));
      }
    }
  }

  return result;
}

} // namespace procedural::world::generator
