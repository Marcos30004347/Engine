#pragma once
#include "WorldRuntimePublication.hpp"

namespace procedural::world::rewrite {
enum RuntimeCollisionMask : uint32_t
{
  CollisionSurface = 1,
  CollisionWater = 2,
  CollisionStructure = 4,
  CollisionSolid = CollisionSurface | CollisionStructure,
  CollisionAll = 7
};

struct RuntimeCollisionHit
{
  Position position, normal; // Original winding; never flipped toward the query.
  double fraction = 0, distanceMetres = 0;
  uint64_t revision = 0, face = 0;
  uint32_t primitive = 0, material = 0, category = 0;
};

class WorldRuntimeCollision
{
public:
  // Snapshot data is copied into a bounded index; display LOD selection is irrelevant.
  // Concurrent const queries are supported; build requires exclusive ownership.
  // Failure preserves the previous index. Structures keep their full 3D geometry.
  bool build(const RuntimeSnapshot &, uint32_t maximumTriangles = 1000000, std::string *error = nullptr);
  // Nearest two-sided intersection on the closed finite segment. Coplanar motion
  // is not a hit. Miss/invalid input leaves output unchanged. This is not a swept
  // capsule, contact manifold, penetration solver or character controller.
  bool segment(Position from, Position to, uint32_t mask, RuntimeCollisionHit &, bool indexed = true) const;

  uint64_t revision() const
  {
    return revision_;
  }

  size_t triangleCount() const
  {
    return primitives_.size();
  }

  size_t nodeCount() const
  {
    return nodes_.size();
  }

private:
  struct Primitive
  {
    std::array<Position, 3> points;
    uint64_t face = 0;
    uint32_t material = 0, category = 0;
  };

  struct Node
  {
    Position lo, hi;
    uint32_t begin = 0, count = 0, left = 0, right = 0;
  };

  uint64_t revision_ = 0;
  std::vector<Primitive> primitives_;
  std::vector<uint32_t> order_;
  std::vector<Node> nodes_;
  uint32_t split(uint32_t begin, uint32_t end);
};
} // namespace procedural::world::rewrite
