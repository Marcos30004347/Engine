#pragma once

#include "procedural/PlanetPackage.hpp"
#include "procedural/SphericalTerrain.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace procedural
{

struct PlanetMeshExportStatistics
{
  uint32_t regionCount = 0u;
  uint32_t sourceTriangleCount = 0u;
  uint32_t vertexCount = 0u;
  uint32_t faceCount = 0u;
  double minimumElevationMetres = 0.0;
  double maximumElevationMetres = 0.0;
  uint64_t fileBytes = 0u;
};

// Writes the generated displaced dual mesh as binary little-endian PLY.
// Coordinates are planet-centred kilometres to retain useful F32 precision.
// Custom vertex/face properties preserve material and stable region mappings.
bool writeSphericalTerrainMeshPly(
    const std::filesystem::path &path,
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    PlanetMeshExportStatistics *statistics = nullptr,
    std::string *error = nullptr);

// Human-readable companion for tools that do not need to open the indexed
// package just to discover physical dimensions and mesh channel semantics.
bool writePlanetMetadataManifest(
    const std::filesystem::path &path,
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    const PlanetMeshExportStatistics &meshStatistics,
    std::string *error = nullptr);

} // namespace procedural
