#pragma once

#include "voxel/svraster/CityVoxelAssetCompiler.hpp"
#include "procedural/geom2/Polygon.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace virtualgeometry {

struct CityVoxelTerrainTileSpec {
  int32_t tileX=0,tileZ=0;
  uint32_t cellsPerSide=64;
  uint32_t overlapHaloCells=0; // Duplicate edge cover for independently triangulated tiles.
  double voxelMetres=.1;
  double originX=0,originZ=0;
  uint32_t rgba=0xff637477u,materialProfile=6;
  bool naturalPalette=false; // Deterministic grass, earth and exposed rock values.
  std::function<std::pair<uint32_t,uint32_t>(double,double)> topAppearance;
  std::function<bool(double,double)> excludeColumn; // Authored base owns this footprint.
  uint32_t maximumCells=1'000'000;
};

// Authored city foundations are rasterized as a surface shell. A base tile
// owns its global lattice cells, so adjacent tiles meet without duplicate
// boundary cells or floating point edge interpolation.
struct CityVoxelBasePrism {
  procedural::geom2::Polygon footprint;
  double bottomMetres=0,topMetres=0;
  bool terrainFollowing=false; // Heights are offsets above the city ground field.
  uint32_t topRgba=0xff879196u,sideRgba=0xff5d6469u;
  uint32_t topMaterial=37,sideMaterial=54;
};

bool sampleCityVoxelBaseTile(const CityVoxelTerrainTileSpec &,
    const std::vector<CityVoxelBasePrism> &prisms,
    std::vector<voxel::svraster::CityVoxelCell> &result,
    std::string *error=nullptr,
    const std::function<double(double,double)> &ground={});

// Samples one bounded tile from the same city-wide height function. Neighbour
// Height queries include a one-cell halo. Optional overlap cover moves mesh
// boundary faces behind a neighbour's top surface when tiles triangulate
// independently; both tiles sample identical colors and heights there.
bool sampleCityVoxelTerrainTile(const CityVoxelTerrainTileSpec &,
    const std::function<double(double,double)> &height,
    std::vector<voxel::svraster::CityVoxelCell> &result,
    std::string *error=nullptr);

} // namespace virtualgeometry
