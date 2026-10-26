#pragma once

#include "voxel/svraster/CityVoxelAssetCompiler.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace virtualgeometry
{

// City structures that are not cell dwellings - curtain walls, towers, the
// plaza's arcade, trees, fences, bridges - exist only as triangles of the city
// mesh. This turns such a triangle set into voxel cells.
struct CityVoxelMeshTriangle
{
  // Lattice units: cell (x, y, z) spans [x, x+1) x [y, y+1) x [z, z+1).
  std::array<std::array<double, 3>, 3> corners{};
  uint32_t rgba = 0u; // 0xAABBGGRR, as CityVoxelCell.
  uint32_t materialProfile = 1u;
};

struct CityVoxelMeshVoxelizeStats
{
  uint64_t surfaceCells = 0u;
  uint64_t filledCells = 0u; // Enclosed by a closed surface.
  bool filled = false;       // False when the box was too big to flood.
};

// Every cell a triangle touches (separating-axis test against the cell box),
// then, when `fillEnclosed`, every cell no 6-connected path from outside the
// bounds reaches, coloured as the surface cell above it: a thick wall becomes
// solid instead of two skins a cell apart, which also lets its LOD chain
// simplify. An open mesh (a fence board, a canopy) fills nothing. Cells come
// back sorted by position. Fails over `maximumCells` surface cells.
bool voxelizeCityVoxelMesh(const std::vector<CityVoxelMeshTriangle> &triangles, bool fillEnclosed, uint64_t maximumCells,
                           std::vector<voxel::svraster::CityVoxelCell> &cells, CityVoxelMeshVoxelizeStats *stats = nullptr,
                           std::string *error = nullptr);

// The yaw (radians) whose frame boxes these points tightest in plan, tried
// over the directions of the longest horizontal edges: a diagonal wall
// segment voxelised in the world frame floods a box 25x its own volume.
double cityVoxelMeshPlanYaw(const std::vector<std::array<std::array<double, 3>, 3>> &worldTriangles);

} // namespace virtualgeometry
