#pragma once
#include "WorldRuntimePublication.hpp"

namespace procedural::world::rewrite {
struct RuntimeDisplayMesh
{
  std::vector<Position> vertices;
  std::vector<std::array<uint32_t, 3>> triangles;
  std::vector<uint32_t> materials;
};

// Select complete tile LODs, preserve winding/materials and append physical structures.
// Does not reinterpret the semantic contours, resample heights or weld different layers.
bool buildRuntimeDisplayMesh(const RuntimeSnapshot &, uint64_t maximumTriangles, RuntimeDisplayMesh &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
