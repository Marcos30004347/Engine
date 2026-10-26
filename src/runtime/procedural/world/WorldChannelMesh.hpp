#pragma once
#include "WorldRiverChannels.hpp"
#include <array>

namespace procedural::world::rewrite {
struct ChannelMeshSettings
{
  double minX = -60, maxX = 60, minZ = -70, maxZ = 70, spacingMetres = 1;
};

struct ChannelMeshProduct
{
  std::vector<Position> vertices;
  std::vector<std::array<uint32_t, 3>> triangles;
  std::vector<uint8_t> roles; // 0 terrain/bed, 1 water; no overlapping water ribbons.
  double waterAreaMetres2 = 0, terrainAreaMetres2 = 0;
  uint32_t shorelineSegments = 0;
};

// A bounded acceptance/compiler path. Shared grid-edge roots define the physical
// shoreline approximation; every clipped polygon is triangulated by CDT.
// Full feature-driven arrangement/refinement remain Steps 14/15.
bool buildChannelMesh(const WorldRiverChannels &, const WorldRiverChannels::BaseSurface &, const ChannelMeshSettings &, ChannelMeshProduct &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
