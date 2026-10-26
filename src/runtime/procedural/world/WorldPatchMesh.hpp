#pragma once
#include "procedural/world/WorldGenerationContracts.hpp"
#include <array>
#include <functional>

namespace procedural::world::rewrite {
struct BoundaryUse
{
  uint64_t edgeId = 0;
  bool reversed = false;
};

struct PatchDomain
{
  uint64_t id = 0;
  std::vector<BoundaryUse> outer;
  std::vector<std::vector<BoundaryUse>> holes;
};

struct PatchConstraint
{
  uint64_t sourceEdge = 0;
  uint32_t a = 0, b = 0;
};

struct PatchMesh
{
  uint64_t patchId = 0;
  std::vector<BoundaryVertex> vertices;
  std::vector<std::array<uint32_t, 3>> triangles;
  std::vector<PatchConstraint> constraints;
  double domainArea = 0, triangleArea = 0;
};

// Compiles an already reconciled simple PSLG; it never moves boundary geometry.
// Interior points sample the supplied surface; boundary heights come from registry.
// Atomic on failure. Crossing or touching rings need arrangement repair upstream.
bool compilePatchMesh(
    const PatchDomain &domain,
    const BoundaryRegistry &registry,
    const std::vector<Position> &interiorSamples,
    const std::function<double(double x, double z)> &surface,
    PatchMesh &mesh,
    std::string *error = nullptr);
} // namespace procedural::world::rewrite
