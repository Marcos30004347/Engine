#pragma once

#include "procedural/PlanetPackage.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct alignas(16) PlanetOverviewVertex
{
  // xyz is a unit package-space direction; w is elevation in metres.
  std::array<float, 4> directionElevation{};
  // Linear terrain colour plus derived forest coverage.
  std::array<float, 4> baseColorForest{};
  // Linear soil/rock colour plus authored mountain coverage.
  std::array<float, 4> soilColorMountain{};
  // Temperature, precipitation, river, and water mask in [0, 1].
  std::array<float, 4> climateRiverWater{};
};
static_assert(sizeof(PlanetOverviewVertex) == 64u);

struct PlanetOverviewMesh
{
  struct SurfaceMaps
  {
    struct MipLevel
    {
      uint32_t faceResolution = 0u;
      uint32_t width = 0u;
      uint32_t height = 0u;
      std::vector<uint8_t> albedoRoughnessRGBA8;
      std::vector<uint8_t> terrainFeaturesRGBA8;
      // Normalized visible-surface elevation and separate bathymetry. Both
      // use IEEE-754 binary16 payloads uploaded to R16Float textures.
      std::vector<uint16_t> heightR16Float;
      std::vector<uint16_t> oceanFloorR16Float;
      // Normalized conservative (minimum, maximum) visible height.
      std::vector<uint16_t> heightMinMaxRG16Float;
    };

    uint32_t faceResolution = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    // sRGB albedo plus linear roughness.
    std::vector<uint8_t> albedoRoughnessRGBA8;
    // Material ID, mountain, river, and normalized height-derived slope.
    std::vector<uint8_t> terrainFeaturesRGBA8;
    std::vector<uint16_t> heightR16Float;
    std::vector<uint16_t> oceanFloorR16Float;
    std::vector<uint16_t> heightMinMaxRG16Float;
    // Mip zero is stored in the fields above for compatibility. This vector
    // starts at mip one and ends at one texel per cube face.
    std::vector<MipLevel> mipLevels;
    double detailMinimumMetres = 0.0;
    double detailRangeMetres = 0.0;
    double oceanFloorMinimumMetres = 0.0;
    double oceanFloorRangeMetres = 0.0;

    bool valid() const
    {
      const size_t bytes = static_cast<size_t>(width) * height * 4u;
      return faceResolution != 0u && width == faceResolution * 3u &&
          height == faceResolution * 2u &&
          albedoRoughnessRGBA8.size() == bytes &&
          terrainFeaturesRGBA8.size() == bytes &&
          heightR16Float.size() == bytes / 4u &&
          oceanFloorR16Float.size() == bytes / 4u &&
          heightMinMaxRG16Float.size() == bytes / 2u;
    }

    uint32_t mipCount() const
    {
      return valid() ? static_cast<uint32_t>(mipLevels.size()) + 1u : 1u;
    }
  };

  std::vector<PlanetOverviewVertex> vertices;
  std::vector<uint32_t> indices;
  SurfaceMaps surfaceMaps;
  double sourceRadiusMetres = 0.0;
  double maximumElevationMetres = 0.0;
  bool graphNative = false;
};

struct PlanetOverviewMeshSettings
{
  // Per-face cells for the cube-sphere fallback. The graph-native surface is
  // normally preferred and ignores this setting. The renderer sample opts
  // into this dense raster geometry as its static safety path so a coarse
  // authored graph can never expose a faceted planet silhouette.
  uint32_t fallbackFaceResolution = 512u;
  bool preferRasterFallbackGeometry = false;
  // Per-face resolution of the fragment-shaded material and height maps.
  // 1024 preserves exported drainage and ridge gradients in a full-globe
  // preview; finer apparent material detail remains procedural in the shader.
  // Cube-face resolution of the displaced surface atlas. Six faces at 12 bytes
  // a texel, measured on the reference planet:
  //
  //   1024 -> 72 MB, 1.1 s, 153 m texels at R=100 km
  //   2048 -> 288 MB, 4.3 s, 77 m
  //   4096 -> 1.15 GB, ~17 s, 38 m
  //   8192 -> 4.6 GB, ~68 s, 19 m
  //
  // 4096 is the default: it sits inside a 5 GB planet budget with room for the
  // rest of the package, and 38 m texels are fine enough that the adaptive CBT
  // has real data to subdivide into. The clamp allows 8192 for a machine with
  // the memory and the patience, but generation is linear in texel count and
  // that costs over a minute.
  uint32_t surfaceFaceResolution = 4096u;
};

double planetOverviewForestWeight(uint8_t material, double temperature, double precipitation, double mountain);

uint16_t planetOverviewFloatToHalf(float value);
float planetOverviewHalfToFloat(uint16_t value);

// Stable FNV-1a signature of the decoded visual mesh. This deliberately hashes
// scalar bit patterns rather than object padding so captured baselines remain
// reproducible across compilers.
uint64_t planetOverviewMeshSignature(const PlanetOverviewMesh &mesh);

bool loadPlanetOverviewMesh(const procedural::PlanetPackageReader &package, const PlanetOverviewMeshSettings &settings, PlanetOverviewMesh &mesh, std::string *error = nullptr);

} // namespace voxel::svraster
