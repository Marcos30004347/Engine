#pragma once

#include "procedural/PolygonTerrain.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace procedural
{

// This is deliberately a simple, seekable package layout. Each tile is stored
// independently, with a one-texel clamped apron, so the runtime can read and
// decode only the tiles needed by its camera-centred chunk window. Tiles are
// independently LZ4-compressed blocks.
struct TerrainPackageSettings
{
  uint32_t tileSizePixels = 256u;
  double voxelSizeMetres = 0.20;
  double heightRangeMetres = 256.0;
};

struct TerrainPackageStatistics
{
  uint32_t tileColumns = 0u;
  uint32_t tileRows = 0u;
  uint64_t heightBytes = 0u;
  uint64_t materialBytes = 0u;
  uint64_t regionBytes = 0u;
  uint64_t uncompressedBytes = 0u;
  uint64_t compressedBytes = 0u;
};

// Writes:
//   terrain-package.json
//   tiles/x_<column>_z_<row>.terrain-tile.lz4
//
// Height data is unsigned little-endian R16 UNORM over heightRangeMetres.
// Material data is one PolygonTerrainMaterial byte per texel and source region
// data is one stable uint32 Voronoi ID per texel. All include a one-texel
// clamped apron on every edge of every tile. Each terrain tile is an
// independently LZ4-compressed blob so runtime workers can stream it directly.
bool writeTerrainPackage(
    const std::filesystem::path &outputDirectory, const PolygonTerrainSettings &terrainSettings,
    const PolygonTerrainRasterSettings &rasterSettings, const PolygonTerrainMaps &maps,
    const TerrainPackageSettings &packageSettings, TerrainPackageStatistics *statistics = nullptr,
    std::string *error = nullptr);

} // namespace procedural
