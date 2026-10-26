#pragma once
#include "procedural/geometry/TerrainMeshBuild.hpp"
namespace procedural::geometry
{
void simplifyTerrainTile(MeshTile &tile, std::vector<uint8_t> locked, const TerrainMeshSettings &settings, const HeightFunction &height, const MaterialFunction &material);
}
