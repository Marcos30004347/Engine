#pragma once

#include "procedural/PlanetPackage.hpp"
#include "procedural/SphericalTerrain.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace procedural
{

struct SphericalTerrainCubePackageSettings
{
  // Levels are cube-quadtree storage levels. metadata.maximumTileLevel is the
  // finest baked level; setting minimumTileLevel to the same value omits
  // coarser fallback tiles.
  uint8_t minimumTileLevel = 0u;
  // Guards the current in-memory baker from accidental planet-scale output.
  // Zero disables the limit. A streaming producer can later remove this
  // restriction without changing the package format.
  uint64_t maximumUncompressedBytes = 1024ull * 1024ull * 1024ull;
};

struct SphericalTerrainCubePackageStatistics
{
  uint64_t tileCount = 0u;
  uint64_t storedTexelCount = 0u;
  uint64_t uncompressedBytes = 0u;
  uint32_t channelCount = 0u;
  uint32_t blockCount = 0u;
};

// Converts the authoritative spherical simulation graph to structure-of-arrays
// package blocks. When the generated dual mesh is present, its directions,
// elevations, indices, per-vertex attributes, and triangle-region mappings are
// appended as independently readable graph channels. No equirectangular raster
// is included.
bool buildSphericalTerrainGraphPackage(
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    std::vector<PlanetPackageChannel> &channels,
    std::vector<PlanetPackageBlockInput> &blocks,
    std::string *error = nullptr);

// Reprojects all generated equirectangular products into independently
// addressable cube-quadtree channel tiles, including cross-face gutters.
bool buildSphericalTerrainCubePackage(
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    const SphericalTerrainCubePackageSettings &settings,
    std::vector<PlanetPackageChannel> &channels,
    std::vector<PlanetPackageBlockInput> &blocks,
    SphericalTerrainCubePackageStatistics *statistics = nullptr,
    std::string *error = nullptr);

// Builds both authoritative graph blocks and derived cube raster blocks.
bool buildSphericalTerrainPackage(
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    const SphericalTerrainCubePackageSettings &settings,
    std::vector<PlanetPackageChannel> &channels,
    std::vector<PlanetPackageBlockInput> &blocks,
    SphericalTerrainCubePackageStatistics *statistics = nullptr,
    std::string *error = nullptr);

// Writes a graph-only planet package. Raster cube tiles can be added by the
// cube-surface baker without changing any graph block or stable region ID.
bool writeSphericalTerrainGraphPackage(
    const std::filesystem::path &path,
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    const PlanetPackageWriteOptions &options = {},
    PlanetPackageStatistics *statistics = nullptr,
    std::string *error = nullptr);

bool writeSphericalTerrainPackage(
    const std::filesystem::path &path,
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    const SphericalTerrainCubePackageSettings &cubeSettings = {},
    const PlanetPackageWriteOptions &writeOptions = {},
    PlanetPackageStatistics *statistics = nullptr,
    SphericalTerrainCubePackageStatistics *cubeStatistics = nullptr,
    std::string *error = nullptr);

} // namespace procedural
