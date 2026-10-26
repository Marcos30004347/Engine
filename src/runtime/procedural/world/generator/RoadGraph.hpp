#pragma once

#include "PatchTiling.hpp"

#include <cstdint>
#include <vector>

namespace procedural::world::generator {

// The edge network the settlement's roads are placed into: a planar graph.
// Adding a segment splits every existing edge it crosses at the crossing
// (and the segment itself), merges endpoints that fall within the snap
// distance of a vertex or onto an edge, and never leaves two edges crossing.
// Lot detection is a left-hugging edge walk: from every directed half-edge
// the walk turns to the most clockwise outgoing edge at each vertex until it
// closes; a closed walk with positive area is a face (a block), the
// unbounded walk is dropped. Dead-end spurs inside a face are folded out of
// its ring but kept on its edge list.
struct RoadGraphEdge
{
  uint32_t a = 0, b = 0;
  uint8_t type = 1; // 0 main road, 1 minor road, 2 alley.
  double halfWidth = 2;
};

struct RoadGraphFace
{
  std::vector<uint32_t> vertices; // The ring, CCW, spurs folded out.
  std::vector<uint32_t> edges;    // Every edge the walk used, spurs included.
  ConvexRing ring;
  double area = 0;
};

class RoadGraph
{
public:
  explicit RoadGraph(double snapMetres = .5);

  const std::vector<Position> &vertices() const
  {
    return vertices_;
  }

  const std::vector<RoadGraphEdge> &edges() const
  {
    return edges_;
  }

  // The vertex at p, or a new one; a point within the snap distance of an
  // existing edge's interior splits that edge and becomes a vertex on it.
  uint32_t vertex(Position p);

  // Adds the segment, splitting it and the edges it crosses at every
  // crossing. Returns the number of edges the segment was split into.
  uint32_t add(Position a, Position b, uint8_t type, double halfWidth);

  std::vector<RoadGraphFace> faces() const;

  // The first edge a ray from `origin` in direction (dx, dz) reaches within
  // `reach`, ignoring edges at `origin` itself; the hit point is returned.
  bool firstHit(Position origin, double dx, double dz, double reach, Position &hit) const;

  uint32_t degree(uint32_t vertex) const
  {
    return uint32_t(adjacency_[vertex].size());
  }

  // Whether a segment from a to b would cross an existing edge closer than
  // `clearance` to a parallel edge (a sliver face); used by the road growth.
  bool slivers(Position a, Position b, double clearance) const;

private:
  double snap_;
  std::vector<Position> vertices_;
  std::vector<RoadGraphEdge> edges_;
  std::vector<std::vector<uint32_t>> adjacency_; // Edge indices per vertex.

  void link(uint32_t a, uint32_t b, uint8_t type, double halfWidth);
  void splitEdge(uint32_t edge, uint32_t at);
};

} // namespace procedural::world::generator
