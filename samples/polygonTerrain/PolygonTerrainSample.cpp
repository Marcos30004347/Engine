#include "PolygonTerrainAdaptiveGlobe.hpp"
#include "PolygonTerrainPreviewPasses.hpp"
#include "os/Logger.hpp"
#include "procedural/PolygonTerrain.hpp"
#include "procedural/PlanetMeshExport.hpp"
#include "procedural/SphericalTerrain.hpp"
#include "procedural/SphericalTerrainPackage.hpp"
#include "procedural/rendering/PlanetErosionPass.hpp"
#include "procedural/TerrainPackage.hpp"
#include "time/TimeSpan.hpp"
#include "window/sdl3/SDL3Window.hpp"

#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef POLYGON_TERRAIN_SPHERE_SHADER_PATH
#define POLYGON_TERRAIN_SPHERE_SHADER_PATH "polygon-terrain-sphere.spirv"
#endif
#ifndef POLYGON_TERRAIN_EROSION_SHADER_PATH
#define POLYGON_TERRAIN_EROSION_SHADER_PATH "polygon-terrain-hydraulic-erosion.spirv"
#endif
#ifndef POLYGON_TERRAIN_RASTER_SHADER_PATH
#define POLYGON_TERRAIN_RASTER_SHADER_PATH "polygon-terrain-preview-raster.spirv"
#endif

namespace
{
namespace gpu = rendering;
namespace vkbackend = rendering::backend::vulkan;
using polygonterrain::GpuPreviewTexture;
using polygonterrain::PreviewTextureSet;
using polygonterrain::TerrainPreviewImGuiOverlayPass;
using polygonterrain::TerrainPreviewHydraulicErosionPass;
using polygonterrain::TerrainPreviewHydraulicErosionSettings;
using polygonterrain::TerrainPreviewRHI;
using polygonterrain::TerrainPreviewRasterPass;
using polygonterrain::TerrainPreviewSphereLine;
using polygonterrain::TerrainPreviewSpherePass;
using polygonterrain::TerrainPreviewSphereSettings;
using polygonterrain::TerrainPreviewTextureUploadPass;

enum class SpherePreviewLayer : int
{
  Terrain = 0,
  Materials = 1,
  Height = 2,
  Continents = 3,
  Tectonics = 4,
  Mountains = 5,
  Temperature = 6,
  Rainfall = 7,
  Plates = 8,
  Rivers = 9,
};

struct GenerationJob
{
  std::thread worker;
  std::mutex mutex;
  bool running = false;
  bool finished = false;
  bool success = false;
  bool cancelRequested = false;
  double progress = 0.0;
  double elapsedSeconds = 0.0;
  std::string phase;
  std::string error;
  procedural::PolygonTerrainMaps maps{};
  procedural::PolygonTerrainStatistics statistics{};
  std::vector<uint8_t> mountainDebugMap;
  std::vector<procedural::SphericalTerrainLine> sphereLines;
  std::vector<procedural::SpherePoint> sphereSurfacePositions;
  std::vector<uint16_t> sphereSurfaceElevations;
  std::vector<uint8_t> sphereSurfaceContinents;
  std::vector<uint8_t> sphereSurfaceTectonics;
  std::vector<uint8_t> sphereSurfaceMountains;
  std::vector<uint8_t> sphereSurfaceTemperature;
  std::vector<uint8_t> sphereSurfacePrecipitation;
  std::vector<uint8_t> sphereSurfacePlates;
  std::vector<uint8_t> sphereSurfaceRivers;
  std::vector<uint8_t> sphereSurfaceMaterials;
  std::vector<uint32_t> sphereSurfaceIndices;
  std::vector<procedural::SphericalTerrainNode> sphereRegionNodes;
  std::vector<uint32_t> sphereRegionNeighbourOffsets;
  std::vector<uint32_t> sphereRegionNeighbourIndices;
};

struct ApplicationState
{
  procedural::PolygonTerrainSettings settings{};
  // Spherical is the default world, and its export atlas is equirectangular, so
  // the default has to be 2:1. It also has to out-resolve the Voronoi graph it
  // samples: 204k regions over R=100 km is ~785 m between region centres, and a
  // 512-wide atlas is 1227 m per equatorial texel. That undersampled the
  // simulation - rivers collapsed to single blocky texels and ridge lines
  // aliased - and every consumer downstream then upsampled the damage. 2048
  // gives 307 m texels, comfortably inside the graph's own detail.
  procedural::PolygonTerrainRasterSettings raster{2048u, 1024u};
  procedural::PolygonTerrainMaps maps{};
  procedural::PolygonTerrainStatistics statistics{};
  std::vector<uint8_t> mountainDebugMap;
  std::vector<procedural::SphericalTerrainLine> sphereLines;
  std::vector<procedural::SpherePoint> sphereSurfacePositions;
  std::vector<uint16_t> sphereSurfaceElevations;
  std::vector<uint8_t> sphereSurfaceContinents;
  std::vector<uint8_t> sphereSurfaceTectonics;
  std::vector<uint8_t> sphereSurfaceMountains;
  std::vector<uint8_t> sphereSurfaceTemperature;
  std::vector<uint8_t> sphereSurfacePrecipitation;
  std::vector<uint8_t> sphereSurfacePlates;
  std::vector<uint8_t> sphereSurfaceRivers;
  std::vector<uint8_t> sphereSurfaceMaterials;
  std::vector<uint32_t> sphereSurfaceIndices;
  std::vector<procedural::SphericalTerrainNode> sphereRegionNodes;
  std::vector<uint32_t> sphereRegionNeighbourOffsets;
  std::vector<uint32_t> sphereRegionNeighbourIndices;
  procedural::SphericalTerrainMaps sphereExportMaps{};
  std::vector<uint32_t> sphereSurfaceColors;
  procedural::TerrainPackageSettings package{};
  procedural::TerrainPackageStatistics packageStatistics{};
  procedural::PlanetPackageStatistics planetPackageStatistics{};
  procedural::SphericalTerrainCubePackageStatistics planetCubeStatistics{};
  procedural::SphericalTerrainProfile planetProfile{};
  // Supplies all fluvial and hillslope relief. Not owned; main() points this at
  // a solver backed by its own compute device, so the generation thread never
  // touches the device that is drawing the preview.
  procedural::SphericalTerrainFluvialSolver *fluvialSolver = nullptr;
  procedural::rendering::PlanetStreamPowerSettings planetErosion{};
  procedural::rendering::PlanetStreamPowerStatistics planetErosionStatistics{};
  bool planetErosionEnabled = true;
  PreviewTextureSet textures{};
  std::array<char, 512> outputDirectory{};
  std::array<char, 512> configurationPath{};
  std::string status;
  bool generated = false;
  bool generatedFromSphericalWorld = false;
  bool useSphericalWorld = true;
  // A 200 km-diameter world: 628 km of equatorial circumference and about
  // 126,000 km^2 of surface, of which roughly 40% comes out as land. Small
  // enough to traverse and to bake, large enough for continent-scale drainage.
  double planetRadiusMetres = 100000.0;
  // Total encoded relief, sea floor to summit. Zero falls back to the
  // Earth-like radius * 0.0015 ratio, which is what the package used before
  // this became an explicit knob; at R=100 km that ratio yields 150 m of
  // relief over a 628 km circumference, which reads as a billiard ball. Small
  // bodies hold far more relief per radius than Earth does (Vesta carries ~7%
  // of its radius), so a few kilometres here is both defensible and legible.
  double planetReliefMetres = 3000.0;
  uint32_t planetPlateCount = 80u;
  double planetOceanicPlateFraction = 0.58;
  double planetJitter = 0.75;
  double planetCollisionThreshold = 0.24;
  double planetTerrainWarp = 0.75;
  double planetRoughness = 0.40;
  uint32_t planetGraphWorkers = 0u;
  uint32_t planetGraphRegionCount = 204000u;
  uint32_t planetHydraulicIterations = 10u;
  uint32_t planetThermalIterations = 1u;
  double planetHydraulicErosion = 0.00030;
  double planetThermalErosion = 0.015;
  double planetTalusSlope = 1.16;
  uint32_t planetRidgeIterations = 3u;
  double planetRidgeSharpening = 0.04;
  uint32_t planetSoilIterations = 3u;
  double planetSoilCreep = 0.1125;
  double planetMountainUplift = 1.0;
  double planetMountainBeltWidth = 1.0;
  double planetMountainRidgeSpacing = 1.0;
  double planetMountainRidgeHeight = 1.0;
  double planetCoastalPlainWidth = 1.0;
  double planetShorelineDetail = 1.0;
  double planetInteriorRelief = 1.0;
  double planetLakeAbundance = 1.0;
  double planetDesertStrength = 1.0;
  double planetBiomeVariation = 1.0;
  double planetClimateWetness = 1.0;
  int spherePreviewLayer = static_cast<int>(SpherePreviewLayer::Terrain);
  float sphereYawDegrees = -25.0f;
  // Globe camera zoom. The preview pass has always supported this — it divides
  // the camera distance by it — but nothing ever drove it, so the value was
  // pinned at 1.0 and the globe could not be inspected up close.
  float sphereZoom = 1.0f;
  // Latched so a failing atlas build is attempted once, not every frame.
  bool previewRefreshFailed = false;
  // Adaptive CBT globe. The CBT displaces from a surface atlas that
  // loadPlanetOverviewMesh builds out of a PlanetPackageReader, so the preview
  // bakes a package to a working path once generation settles rather than
  // waiting for an explicit Save. Going through the package keeps exactly one
  // reconstruction path shared with SparseVoxelRendererSample: what this window
  // shows is what the export produces. Sampling the in-memory atlases instead
  // would be a second path, free to drift from the renderer without failing.
  voxel::svraster::PlanetOverviewMesh planetOverview{};
  std::filesystem::path adaptiveGlobePackagePath;
  bool adaptiveGlobeAvailable = false;
  // Latched for the same reason as previewRefreshFailed: one attempt, and a
  // failure must leave the fixed dual mesh on screen rather than an empty
  // window. The globe draw is never gated on this.
  bool adaptiveGlobeFailed = false;
  bool adaptiveGlobeRequested = false;
  bool adaptiveGlobeSupported = false;
  bool adaptiveGlobeEnabled = true;
  std::string adaptiveGlobeStatus;
  // Surface atlas resolution for the preview, per cube face. At R=100 km this
  // is 153 m texels at 1024, 77 m at 2048 and 38 m at 4096, against a 307 m
  // export atlas - so past 2048 the atlas is interpolating rather than carrying
  // new detail, and the way to get finer terrain is a bigger export atlas or a
  // smaller planet, not a bigger surface atlas. 2048 costs 288 MB and ~4 s.
  uint32_t adaptiveGlobeFaceResolution = 2048u;
  polygonterrain::AdaptiveGlobeSettings adaptiveGlobe{};
  // Mirrored out of the globe each frame so the UI can read them without
  // reaching into a renderer that only main() owns.
  uint32_t adaptiveGlobeActiveBisectors = 0u;
  uint32_t adaptiveGlobeCapacity = 0u;
  float spherePitchDegrees = -12.0f;
  bool sphereLighting = true;
  bool sphereShowPlateBoundaries = false;
  bool sphereDragging = false;
  float sphereLastMouseX = 0.0f;
  float sphereLastMouseY = 0.0f;
  uint64_t previewRevision = 0u;
  uint64_t sphereDirectRevision = 0u;
  int sphereDirectLayer = -1;
  std::vector<uint8_t> sphereDirectRGBA;
  uint64_t sphereLineRevision = 0u;
  bool sphereLineBoundaries = true;
  int sphereLineLayer = -1;
  TerrainPreviewHydraulicErosionSettings erosionSettings{};
  bool erosionRequested = false;
  bool erosionCancelRequested = false;
  bool erosionRunning = false;
  bool autoErodeAfterGeneration = false;
  bool saveAfterErosion = false;
  bool autoErosionStarted = false;
  float erosionProgress = 0.0f;
  uint32_t erosionCompletedIterations = 0u;
  GenerationJob generation{};

  ApplicationState()
  {
    // Default exports provide one texel per 512x512 cube face. The graph-only
    // interactive preview remains independent from this deferred atlas size.
    raster.width = 2048u;
    raster.height = 1024u;
    std::snprintf(outputDirectory.data(), outputDirectory.size(), "%s", "polygon-terrain");
    std::snprintf(configurationPath.data(), configurationPath.size(), "%s", "terrain-config.json");
  }
};

struct GenerationSnapshot
{
  bool running = false;
  bool finished = false;
  double progress = 0.0;
  double elapsedSeconds = 0.0;
  std::string phase;
};

bool parseUnsigned(const char *text, uint64_t &value)
{
  char *end = nullptr;
  value = std::strtoull(text, &end, 0);
  return end != text && *end == '\0';
}

bool parseDouble(const char *text, double &value)
{
  char *end = nullptr;
  value = std::strtod(text, &end);
  return end != text && *end == '\0';
}

bool findJsonValue(const std::string &json, const std::string_view key, std::string_view &value)
{
  const std::string quotedKey = "\"" + std::string(key) + "\"";
  const size_t keyPosition = json.find(quotedKey);
  if (keyPosition == std::string::npos)
    return false;
  const size_t colon = json.find(':', keyPosition + quotedKey.size());
  if (colon == std::string::npos)
    return false;
  size_t begin = colon + 1u;
  while (begin < json.size() && std::isspace(static_cast<unsigned char>(json[begin])))
    ++begin;
  size_t end = begin;
  while (end < json.size() && json[end] != ',' && json[end] != '}' && !std::isspace(static_cast<unsigned char>(json[end])))
    ++end;
  value = std::string_view(json).substr(begin, end - begin);
  return !value.empty();
}

bool loadConfiguration(ApplicationState &state, const std::filesystem::path &path, std::string &error)
{
  std::ifstream file(path);
  if (!file)
  {
    error = "Could not open configuration: " + path.string();
    return false;
  }
  const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  const auto loadDouble = [&](const std::string_view key, double &target)
  {
    std::string_view value;
    return !findJsonValue(json, key, value) || parseDouble(std::string(value).c_str(), target);
  };
  const auto loadUnsigned = [&](const std::string_view key, uint64_t &target)
  {
    std::string_view value;
    return !findJsonValue(json, key, value) || parseUnsigned(std::string(value).c_str(), target);
  };
  uint64_t seed = state.settings.seed;
  uint64_t rivers = state.settings.riverSourceCount;
  uint64_t rasterWidth = state.raster.width;
  uint64_t rasterHeight = state.raster.height;
  uint64_t rasterWorkers = state.raster.workerCount;
  uint64_t tileSize = state.package.tileSizePixels;
  uint64_t planetPlateCount = state.planetPlateCount;
  uint64_t planetGraphWorkers = state.planetGraphWorkers;
  uint64_t planetGraphRegionCount = state.planetGraphRegionCount;
  uint64_t planetHydraulicIterations = state.planetHydraulicIterations;
  uint64_t planetThermalIterations = state.planetThermalIterations;
  uint64_t planetRidgeIterations = state.planetRidgeIterations;
  uint64_t planetSoilIterations = state.planetSoilIterations;
  uint64_t erosionIterations = state.erosionSettings.iterations;
  uint64_t erosionBatch = state.erosionSettings.iterationsPerBatch;
  double erosionTimeStep = state.erosionSettings.timeStep;
  double erosionRainfall = state.erosionSettings.rainfall;
  double erosionEvaporation = state.erosionSettings.evaporation;
  double erosionGravity = state.erosionSettings.gravity;
  double erosionCapacity = state.erosionSettings.sedimentCapacity;
  double erosionDeposition = state.erosionSettings.depositionRate;
  double erosionRate = state.erosionSettings.erosionRate;
  double erosionMinimumSlope = state.erosionSettings.minimumSlope;
  if (!loadUnsigned("seed", seed) || !loadDouble("widthMetres", state.settings.widthMetres) || !loadDouble("heightMetres", state.settings.heightMetres) ||
      !loadDouble("siteRadiusMetres", state.settings.siteRadiusMetres) || !loadUnsigned("riverSourceCount", rivers) || !loadUnsigned("rasterWidth", rasterWidth) || !loadUnsigned("rasterHeight", rasterHeight) ||
      !loadUnsigned("rasterWorkers", rasterWorkers) || !loadUnsigned("tileSizePixels", tileSize) || !loadUnsigned("planetPlateCount", planetPlateCount) ||
      !loadDouble("planetOceanicPlateFraction", state.planetOceanicPlateFraction) || !loadDouble("planetJitter", state.planetJitter) || !loadDouble("planetCollisionThreshold", state.planetCollisionThreshold) ||
      !loadDouble("planetTerrainWarp", state.planetTerrainWarp) || !loadDouble("planetRoughness", state.planetRoughness) ||
      !loadUnsigned("planetGraphWorkers", planetGraphWorkers) ||
      !loadUnsigned("planetGraphRegionCount", planetGraphRegionCount) || !loadUnsigned("planetHydraulicIterations", planetHydraulicIterations) || !loadUnsigned("planetThermalIterations", planetThermalIterations) ||
      !loadDouble("planetHydraulicErosion", state.planetHydraulicErosion) || !loadDouble("planetThermalErosion", state.planetThermalErosion) || !loadDouble("planetTalusSlope", state.planetTalusSlope) ||
      !loadUnsigned("planetRidgeIterations", planetRidgeIterations) || !loadDouble("planetRidgeSharpening", state.planetRidgeSharpening) ||
      !loadUnsigned("planetSoilIterations", planetSoilIterations) || !loadDouble("planetSoilCreep", state.planetSoilCreep) ||
      !loadDouble("planetMountainUplift", state.planetMountainUplift) || !loadDouble("planetMountainBeltWidth", state.planetMountainBeltWidth) ||
      !loadDouble("planetMountainRidgeSpacing", state.planetMountainRidgeSpacing) || !loadDouble("planetMountainRidgeHeight", state.planetMountainRidgeHeight) ||
      !loadDouble("planetCoastalPlainWidth", state.planetCoastalPlainWidth) || !loadDouble("planetShorelineDetail", state.planetShorelineDetail) ||
      !loadDouble("planetInteriorRelief", state.planetInteriorRelief) || !loadDouble("planetLakeAbundance", state.planetLakeAbundance) ||
      !loadDouble("planetDesertStrength", state.planetDesertStrength) ||
      !loadDouble("planetBiomeVariation", state.planetBiomeVariation) ||
      !loadDouble("planetClimateWetness", state.planetClimateWetness) ||
      !loadDouble("planetRadiusMetres", state.planetRadiusMetres) ||
      !loadDouble("planetReliefMetres", state.planetReliefMetres) ||
      !loadUnsigned("erosionIterations", erosionIterations) || !loadUnsigned("erosionIterationsPerBatch", erosionBatch) || !loadDouble("erosionTimeStep", erosionTimeStep) ||
      !loadDouble("erosionRainfall", erosionRainfall) || !loadDouble("erosionEvaporation", erosionEvaporation) || !loadDouble("erosionGravity", erosionGravity) ||
      !loadDouble("erosionSedimentCapacity", erosionCapacity) || !loadDouble("erosionDepositionRate", erosionDeposition) || !loadDouble("erosionRate", erosionRate) ||
      !loadDouble("erosionMinimumSlope", erosionMinimumSlope) ||
      seed > std::numeric_limits<uint64_t>::max() || rivers > std::numeric_limits<uint32_t>::max() || rasterWidth == 0u || rasterWidth > 16384u || rasterHeight == 0u || rasterHeight > 16384u || rasterWorkers > 1024u ||
      tileSize == 0u || tileSize > 16384u || planetPlateCount > 255u || planetGraphWorkers > 64u || state.planetOceanicPlateFraction < 0.0 || state.planetOceanicPlateFraction > 1.0 || state.planetJitter < 0.0 || state.planetJitter > 1.0 ||
      state.planetCollisionThreshold < 0.02 || state.planetCollisionThreshold > 2.0 || state.planetTerrainWarp < 0.0 || state.planetTerrainWarp > 1.0 || state.planetRoughness < 0.0 || state.planetRoughness > 1.0 ||
      planetGraphRegionCount < 4096u || planetGraphRegionCount > 256000u || planetHydraulicIterations > 128u || planetThermalIterations > 64u || planetRidgeIterations > 64u || planetSoilIterations > 64u ||
      state.planetHydraulicErosion < 0.0 || state.planetHydraulicErosion > 0.01 || state.planetThermalErosion < 0.0 || state.planetThermalErosion > 0.5 || state.planetTalusSlope < 0.05 || state.planetTalusSlope > 4.0 ||
      state.planetRidgeSharpening < 0.0 || state.planetRidgeSharpening > 0.25 || state.planetSoilCreep < 0.0 || state.planetSoilCreep > 0.5 ||
      state.planetMountainUplift < 0.0 || state.planetMountainUplift > 3.0 || state.planetMountainBeltWidth < 0.25 || state.planetMountainBeltWidth > 3.0 ||
      state.planetMountainRidgeSpacing < 0.35 || state.planetMountainRidgeSpacing > 3.0 || state.planetMountainRidgeHeight < 0.0 || state.planetMountainRidgeHeight > 3.0 ||
      state.planetCoastalPlainWidth < 0.25 || state.planetCoastalPlainWidth > 3.0 || state.planetShorelineDetail < 0.0 || state.planetShorelineDetail > 3.0 ||
      state.planetInteriorRelief < 0.0 || state.planetInteriorRelief > 3.0 || state.planetLakeAbundance < 0.0 || state.planetLakeAbundance > 3.0 ||
      state.planetDesertStrength < 0.0 || state.planetDesertStrength > 3.0 ||
      state.planetBiomeVariation < 0.0 || state.planetBiomeVariation > 3.0 ||
      state.planetClimateWetness < 0.25 || state.planetClimateWetness > 2.0 ||
      state.planetRadiusMetres < 1000.0 || state.planetRadiusMetres > 1.0e9 ||
      state.planetReliefMetres < 0.0 || state.planetReliefMetres > state.planetRadiusMetres * 0.5 ||
      erosionIterations == 0u || erosionIterations > 4096u || erosionBatch == 0u || erosionBatch > 64u ||
      erosionTimeStep < 0.005 || erosionTimeStep > 0.5 || erosionRainfall < 0.0 || erosionRainfall > 0.02 || erosionEvaporation < 0.0 || erosionEvaporation > 0.5 || erosionGravity < 0.01 || erosionGravity > 20.0 ||
      erosionCapacity < 0.0 || erosionCapacity > 100.0 || erosionDeposition < 0.0 || erosionDeposition > 1.0 || erosionRate < 0.0 || erosionRate > 1.0 || erosionMinimumSlope < 0.0 || erosionMinimumSlope > 0.1)
  {
    error = "Configuration contains an invalid terrain value";
    return false;
  }
  state.settings.seed = seed;
  state.settings.riverSourceCount = static_cast<uint32_t>(rivers);
  state.raster.width = static_cast<uint32_t>(rasterWidth);
  state.raster.height = static_cast<uint32_t>(rasterHeight);
  state.raster.workerCount = static_cast<uint32_t>(rasterWorkers);
  state.package.tileSizePixels = static_cast<uint32_t>(tileSize);
  state.planetPlateCount = static_cast<uint32_t>(planetPlateCount);
  state.planetGraphWorkers = static_cast<uint32_t>(planetGraphWorkers);
  state.planetGraphRegionCount = static_cast<uint32_t>(planetGraphRegionCount);
  state.planetHydraulicIterations = static_cast<uint32_t>(planetHydraulicIterations);
  state.planetThermalIterations = static_cast<uint32_t>(planetThermalIterations);
  state.planetRidgeIterations = static_cast<uint32_t>(planetRidgeIterations);
  state.planetSoilIterations = static_cast<uint32_t>(planetSoilIterations);
  state.erosionSettings.iterations = static_cast<uint32_t>(erosionIterations);
  state.erosionSettings.iterationsPerBatch = static_cast<uint32_t>(erosionBatch);
  state.erosionSettings.timeStep = static_cast<float>(erosionTimeStep);
  state.erosionSettings.rainfall = static_cast<float>(erosionRainfall);
  state.erosionSettings.evaporation = static_cast<float>(erosionEvaporation);
  state.erosionSettings.gravity = static_cast<float>(erosionGravity);
  state.erosionSettings.sedimentCapacity = static_cast<float>(erosionCapacity);
  state.erosionSettings.depositionRate = static_cast<float>(erosionDeposition);
  state.erosionSettings.erosionRate = static_cast<float>(erosionRate);
  state.erosionSettings.minimumSlope = static_cast<float>(erosionMinimumSlope);
  std::string_view generatorMode;
  if (findJsonValue(json, "generatorMode", generatorMode))
  {
    if (generatorMode == "\"sphere\"")
      state.useSphericalWorld = true;
    else if (generatorMode == "\"polygon\"")
      state.useSphericalWorld = false;
  }
  return true;
}

bool writePGM8(const std::filesystem::path &path, const uint32_t width, const uint32_t height, const std::vector<uint8_t> &pixels)
{
  if (pixels.size() != static_cast<size_t>(width) * height)
    return false;
  std::ofstream file(path, std::ios::binary);
  if (!file)
    return false;
  file << "P5\n" << width << " " << height << "\n255\n";
  file.write(reinterpret_cast<const char *>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
  return static_cast<bool>(file);
}

bool writePGM16(const std::filesystem::path &path, const uint32_t width, const uint32_t height, const std::vector<uint16_t> &pixels)
{
  if (pixels.size() != static_cast<size_t>(width) * height)
    return false;
  std::ofstream file(path, std::ios::binary);
  if (!file)
    return false;
  file << "P5\n" << width << " " << height << "\n65535\n";
  for (const uint16_t pixel : pixels)
  {
    const std::array<char, 2> bigEndian = {static_cast<char>((pixel >> 8u) & 0xffu), static_cast<char>(pixel & 0xffu)};
    file.write(bigEndian.data(), static_cast<std::streamsize>(bigEndian.size()));
  }
  return static_cast<bool>(file);
}

// Per-stage generation cost. Printed as a table for reading and written as
// JSON so a measurement run can average several invocations without parsing
// stdout. Percentages are of totalMs, and the graph sub-stages deliberately do
// not sum to graphTotalMs: bookkeeping between them is unattributed.
void printSphericalProfile(const procedural::SphericalTerrainProfile &profile, const uint32_t width, const uint32_t height)
{
  const double total = profile.totalMs > 0.0 ? profile.totalMs : 1.0;
  const auto row = [total](const char *label, const double milliseconds)
  {
    std::cout << "  " << std::left << std::setw(22) << label << std::right << std::setw(10) << std::fixed << std::setprecision(1) << milliseconds << " ms  " << std::setw(5) << std::setprecision(1) << (milliseconds / total * 100.0) << " %\n";
  };
  std::cout << "Spherical generation profile at " << width << "x" << height << ":\n";
  row("graph mesh", profile.graphMeshMs);
  row("graph fields", profile.graphFieldsMs);
  row("graph erosion", profile.graphErosionMs);
  row("graph climate", profile.graphClimateMs);
  row("graph rivers", profile.graphRiversMs);
  row("graph surface", profile.graphSurfaceMs);
  row("graph rasterize", profile.graphRasterizeMs);
  row("graph total", profile.graphTotalMs);
  row("macro blur", profile.macroBlurMs);
  row("raster structure", profile.rasterStructureMs);
  row("raster coast", profile.rasterCoastMs);
  row("raster mountain", profile.rasterMountainMs);
  row("raster climate", profile.rasterClimateMs);
  row("raster routing", profile.rasterRoutingMs);
  row("raster erosion", profile.rasterErosionMs);
  row("raster classify", profile.rasterClassifyMs);
  row("total", profile.totalMs);
  std::cout.unsetf(std::ios::fixed);
}

// The solver's own domain, for diagnostics. Slope-area statistics measured on
// the equirectangular atlas are distorted by its polar rows, where thousands of
// texels cover one cube cell and report enormous drainage at near-zero slope.
// The cube faces have near-uniform cell area and no singularity, so the channel
// regime is only honestly measurable here.
bool writePlanetErosionField(
    const std::filesystem::path &path,
    const procedural::PlanetCubeField &elevation,
    const procedural::PlanetCubeField &drainage,
    const double radiusMetres)
{
  if (!elevation.valid() || !drainage.valid() || elevation.side != drainage.side)
    return false;
  std::ofstream file(path, std::ios::binary);
  if (!file)
    return false;
  const uint32_t side = elevation.side;
  const double radius = radiusMetres;
  file.write("PCUBE001", 8);
  file.write(reinterpret_cast<const char *>(&side), sizeof(side));
  file.write(reinterpret_cast<const char *>(&radius), sizeof(radius));
  file.write(reinterpret_cast<const char *>(elevation.values.data()), static_cast<std::streamsize>(elevation.values.size() * sizeof(float)));
  file.write(reinterpret_cast<const char *>(drainage.values.data()), static_cast<std::streamsize>(drainage.values.size() * sizeof(float)));
  return static_cast<bool>(file);
}

void printPlanetErosionStatistics(const procedural::rendering::PlanetStreamPowerStatistics &statistics)
{
  std::cout << "Stream power erosion on cube faces:\n"
            << "  face side           " << statistics.faceSide << " cells (" << statistics.cellCount << " total)\n"
            << "  iterations          " << statistics.iterations << " (dt " << statistics.timeStep << ")\n"
            << std::fixed << std::setprecision(1)
            << "  projection          " << statistics.projectionMilliseconds << " ms\n"
            << "  solve               " << statistics.solveMilliseconds << " ms\n"
            << "  readback            " << statistics.readbackMilliseconds << " ms\n"
            << "  total               " << statistics.totalMilliseconds << " ms\n"
            << "  relief in / out     " << statistics.inputReliefMetres << " m / " << statistics.outputReliefMetres << " m\n"
            << std::setprecision(4)
            << "  uplift mean / peak  " << statistics.meanUplift << " / " << statistics.peakUplift << "\n"
            << std::setprecision(1)
            << "  regolith mean / max " << statistics.meanRegolithMetres << " m / " << statistics.maximumRegolithMetres << " m\n"
            << std::scientific << std::setprecision(3)
            << "  bedrock lost        " << statistics.bedrockLostCubicMetres << " m3\n"
            << "  uplift source       " << statistics.upliftedCubicMetres << " m3\n"
            << "  clamp injected      " << statistics.injectedCubicMetres << " m3\n"
            << "  regolith gained     " << statistics.regolithGainedCubicMetres << " m3\n"
            << "  in transit          " << statistics.inTransitCubicMetres << " m3\n"
            << "  ocean export        " << statistics.oceanExportCubicMetres << " m3\n"
            << std::defaultfloat << std::setprecision(4)
            << "  mass balance error  " << statistics.massBalanceError << "\n";
  std::cout.unsetf(std::ios::fixed);
}

bool writeSphericalProfile(const std::filesystem::path &path, const procedural::SphericalTerrainProfile &profile, const uint32_t width, const uint32_t height, const uint64_t seed, const double planetRadiusMetres)
{
  std::ofstream file(path);
  if (!file)
    return false;
  file << std::setprecision(6) << std::fixed
       << "{\n"
       << "  \"rasterWidth\": " << width << ",\n"
       << "  \"rasterHeight\": " << height << ",\n"
       << "  \"seed\": " << seed << ",\n"
       << "  \"planetRadiusMetres\": " << planetRadiusMetres << ",\n"
       << "  \"graphMeshMs\": " << profile.graphMeshMs << ",\n"
       << "  \"graphFieldsMs\": " << profile.graphFieldsMs << ",\n"
       << "  \"graphErosionMs\": " << profile.graphErosionMs << ",\n"
       << "  \"graphClimateMs\": " << profile.graphClimateMs << ",\n"
       << "  \"graphRiversMs\": " << profile.graphRiversMs << ",\n"
       << "  \"graphSurfaceMs\": " << profile.graphSurfaceMs << ",\n"
       << "  \"graphRasterizeMs\": " << profile.graphRasterizeMs << ",\n"
       << "  \"graphTotalMs\": " << profile.graphTotalMs << ",\n"
       << "  \"macroBlurMs\": " << profile.macroBlurMs << ",\n"
       << "  \"rasterStructureMs\": " << profile.rasterStructureMs << ",\n"
       << "  \"rasterCoastMs\": " << profile.rasterCoastMs << ",\n"
       << "  \"rasterMountainMs\": " << profile.rasterMountainMs << ",\n"
       << "  \"rasterClimateMs\": " << profile.rasterClimateMs << ",\n"
       << "  \"rasterRoutingMs\": " << profile.rasterRoutingMs << ",\n"
       << "  \"rasterErosionMs\": " << profile.rasterErosionMs << ",\n"
       << "  \"rasterClassifyMs\": " << profile.rasterClassifyMs << ",\n"
       << "  \"totalMs\": " << profile.totalMs << "\n"
       << "}\n";
  return static_cast<bool>(file);
}

bool writeSourceRegionMap(const std::filesystem::path &path, const uint32_t width, const uint32_t height, const std::vector<uint32_t> &regions)
{
  if (regions.size() != static_cast<size_t>(width) * height)
    return false;
  std::ofstream file(path, std::ios::binary);
  if (!file)
    return false;
  const auto writeU32 = [&file](const uint32_t value)
  {
    const std::array<char, 4> bytes{
      static_cast<char>(value & 0xffu), static_cast<char>((value >> 8u) & 0xffu),
      static_cast<char>((value >> 16u) & 0xffu), static_cast<char>((value >> 24u) & 0xffu)};
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  };
  writeU32(0x44495250u); // PRID
  writeU32(1u);
  writeU32(width);
  writeU32(height);
  for (const uint32_t region : regions)
    writeU32(region);
  return static_cast<bool>(file);
}

bool writeConfiguration(const std::filesystem::path &path, const ApplicationState &state)
{
  std::ofstream file(path);
  if (!file)
    return false;
  file << "{\n"
       << "  \"generatorMode\": \"" << (state.useSphericalWorld ? "sphere" : "polygon") << "\",\n"
       << "  \"seed\": " << state.settings.seed << ",\n"
       << "  \"widthMetres\": " << state.settings.widthMetres << ",\n"
       << "  \"heightMetres\": " << state.settings.heightMetres << ",\n"
       << "  \"siteRadiusMetres\": " << state.settings.siteRadiusMetres << ",\n"
       << "  \"riverSourceCount\": " << state.settings.riverSourceCount << ",\n"
       << "  \"rasterWidth\": " << state.raster.width << ",\n"
       << "  \"rasterHeight\": " << state.raster.height << ",\n"
       << "  \"rasterWorkers\": " << state.raster.workerCount << ",\n"
       << "  \"tileSizePixels\": " << state.package.tileSizePixels << ",\n"
       << "  \"planetRadiusMetres\": " << state.planetRadiusMetres << ",\n"
       << "  \"planetReliefMetres\": " << state.planetReliefMetres << ",\n"
       << "  \"planetPlateCount\": " << state.planetPlateCount << ",\n"
       << "  \"planetOceanicPlateFraction\": " << state.planetOceanicPlateFraction << ",\n"
       << "  \"planetJitter\": " << state.planetJitter << ",\n"
       << "  \"planetCollisionThreshold\": " << state.planetCollisionThreshold << ",\n"
       << "  \"planetTerrainWarp\": " << state.planetTerrainWarp << ",\n"
       << "  \"planetRoughness\": " << state.planetRoughness << ",\n"
       << "  \"planetGraphWorkers\": " << state.planetGraphWorkers << ",\n"
       << "  \"planetGraphRegionCount\": " << state.planetGraphRegionCount << ",\n"
       << "  \"planetHydraulicIterations\": " << state.planetHydraulicIterations << ",\n"
       << "  \"planetThermalIterations\": " << state.planetThermalIterations << ",\n"
       << "  \"planetHydraulicErosion\": " << state.planetHydraulicErosion << ",\n"
       << "  \"planetThermalErosion\": " << state.planetThermalErosion << ",\n"
       << "  \"planetTalusSlope\": " << state.planetTalusSlope << ",\n"
       << "  \"planetRidgeIterations\": " << state.planetRidgeIterations << ",\n"
       << "  \"planetRidgeSharpening\": " << state.planetRidgeSharpening << ",\n"
       << "  \"planetSoilIterations\": " << state.planetSoilIterations << ",\n"
       << "  \"planetSoilCreep\": " << state.planetSoilCreep << ",\n"
       << "  \"planetMountainUplift\": " << state.planetMountainUplift << ",\n"
       << "  \"planetMountainBeltWidth\": " << state.planetMountainBeltWidth << ",\n"
       << "  \"planetMountainRidgeSpacing\": " << state.planetMountainRidgeSpacing << ",\n"
       << "  \"planetMountainRidgeHeight\": " << state.planetMountainRidgeHeight << ",\n"
       << "  \"planetCoastalPlainWidth\": " << state.planetCoastalPlainWidth << ",\n"
       << "  \"planetShorelineDetail\": " << state.planetShorelineDetail << ",\n"
       << "  \"planetInteriorRelief\": " << state.planetInteriorRelief << ",\n"
       << "  \"planetLakeAbundance\": " << state.planetLakeAbundance << ",\n"
       << "  \"planetDesertStrength\": " << state.planetDesertStrength << ",\n"
       << "  \"planetBiomeVariation\": " << state.planetBiomeVariation << ",\n"
       << "  \"planetClimateWetness\": " << state.planetClimateWetness << ",\n"
       << "  \"erosionIterations\": " << state.erosionSettings.iterations << ",\n"
       << "  \"erosionIterationsPerBatch\": " << state.erosionSettings.iterationsPerBatch << ",\n"
       << "  \"erosionTimeStep\": " << state.erosionSettings.timeStep << ",\n"
       << "  \"erosionRainfall\": " << state.erosionSettings.rainfall << ",\n"
       << "  \"erosionEvaporation\": " << state.erosionSettings.evaporation << ",\n"
       << "  \"erosionGravity\": " << state.erosionSettings.gravity << ",\n"
       << "  \"erosionSedimentCapacity\": " << state.erosionSettings.sedimentCapacity << ",\n"
       << "  \"erosionDepositionRate\": " << state.erosionSettings.depositionRate << ",\n"
       << "  \"erosionRate\": " << state.erosionSettings.erosionRate << ",\n"
       << "  \"erosionMinimumSlope\": " << state.erosionSettings.minimumSlope << "\n"
       << "}\n";
  return static_cast<bool>(file);
}

// Relief is authored, not derived. The old rule was the planar package default
// floored against an Earth-like radius * 0.0015 ratio, which gives about 9.6 km
// at Earth radius and a near-flat 256 m on a small world. Zero relief keeps
// that rule for callers that never set the knob.
//
// The generator and the package must agree on this number: it is both the
// encode range and the metric scale the erosion solver works in.
double planetReliefMetres(const ApplicationState &state)
{
  if (state.planetReliefMetres > 0.0)
    return state.planetReliefMetres;
  return std::max(state.package.heightRangeMetres, state.planetRadiusMetres * 0.0015);
}

bool generateSphericalPreview(ApplicationState &state, std::string &error);

// Copies the authoritative graph arrays into the export maps. Both the Save
// path and the adaptive globe's working package need this, and doing it in one
// place is what stops the preview and the exported planet drifting apart.
void populateSphereExportMesh(ApplicationState &state)
{
  procedural::SphericalTerrainMaps &sphereMaps = state.sphereExportMaps;
  sphereMaps.regionNodes = state.sphereRegionNodes;
  sphereMaps.regionNeighbourOffsets = state.sphereRegionNeighbourOffsets;
  sphereMaps.regionNeighbourIndices = state.sphereRegionNeighbourIndices;
  sphereMaps.surfacePositions = state.sphereSurfacePositions;
  sphereMaps.surfaceElevations = state.sphereSurfaceElevations;
  sphereMaps.surfaceContinents = state.sphereSurfaceContinents;
  sphereMaps.surfaceTectonics = state.sphereSurfaceTectonics;
  sphereMaps.surfaceMountains = state.sphereSurfaceMountains;
  sphereMaps.surfaceTemperature = state.sphereSurfaceTemperature;
  sphereMaps.surfacePrecipitation = state.sphereSurfacePrecipitation;
  sphereMaps.surfacePlates = state.sphereSurfacePlates;
  sphereMaps.surfaceRivers = state.sphereSurfaceRivers;
  sphereMaps.surfaceMaterials = state.sphereSurfaceMaterials;
  sphereMaps.surfaceIndices = state.sphereSurfaceIndices;
}

procedural::PlanetPackageMetadata planetPackageMetadata(const ApplicationState &state)
{
  const uint64_t faceResolution = std::max<uint64_t>(1u, (static_cast<uint64_t>(state.sphereExportMaps.width) + 3u) / 4u);
  const uint64_t tilesAcross = (faceResolution + state.package.tileSizePixels - 1u) / state.package.tileSizePixels;
  uint8_t maximumTileLevel = 0u;
  for (uint64_t dimension = 1u; dimension < tilesAcross; dimension <<= 1u)
    ++maximumTileLevel;

  procedural::PlanetPackageMetadata metadata{};
  metadata.contentHash = state.settings.seed;
  metadata.planetRadiusMetres = state.planetRadiusMetres;
  metadata.heightScaleMetres = planetReliefMetres(state) / 65535.0;
  metadata.nominalRenderPatchEdgeMetres = std::clamp(
      state.planetRadiusMetres * 0.125, 128.0, 1000.0);
  metadata.tileSizePixels = state.package.tileSizePixels;
  metadata.gutterPixels = 1u;
  metadata.maximumTileLevel = maximumTileLevel;
  return metadata;
}

bool saveMaps(ApplicationState &state, std::string &error)
{
  if (!state.generated)
  {
    error = "Generate terrain before saving";
    return false;
  }
  const size_t expectedPixels = static_cast<size_t>(state.raster.width) * state.raster.height;
  if (state.generatedFromSphericalWorld && state.maps.heightmap.size() != expectedPixels)
  {
    state.status = "Baking deferred export atlases from the deterministic spherical world...";
    std::cout << state.status << "\n";
    if (!generateSphericalPreview(state, error))
      return false;
  }
  const std::filesystem::path output(state.outputDirectory.data());
  std::error_code filesystemError;
  std::filesystem::create_directories(output, filesystemError);
  const bool hasMountainDebugMap = state.mountainDebugMap.size() == static_cast<size_t>(state.maps.width) * state.maps.height;
  if (filesystemError || !writePGM16(output / "heightmap.pgm", state.maps.width, state.maps.height, state.maps.heightmap) || !writePGM8(output / "river-map.pgm", state.maps.width, state.maps.height, state.maps.rivers) ||
      !writePGM8(output / "region-map.pgm", state.maps.width, state.maps.height, state.maps.regions) || !writePGM8(output / "material-map.pgm", state.maps.width, state.maps.height, state.maps.materials) ||
      !writeSourceRegionMap(output / "source-region-map.r32", state.maps.width, state.maps.height, state.maps.sourceRegions) ||
      !writePGM8(output / "terrain-form-map.pgm", state.maps.width, state.maps.height, state.maps.terrainForms) ||
      !writePGM8(output / "temperature-map.pgm", state.maps.width, state.maps.height, state.maps.temperature) ||
      !writePGM8(output / "precipitation-map.pgm", state.maps.width, state.maps.height, state.maps.precipitation) || !writePGM8(output / "wind-map.pgm", state.maps.width, state.maps.height, state.maps.wind) ||
      !writePGM8(output / "forest-map.pgm", state.maps.width, state.maps.height, state.maps.forests) || !writePGM8(output / "city-suitability-map.pgm", state.maps.width, state.maps.height, state.maps.settlements) ||
      !writePGM8(output / "continent-map.pgm", state.maps.width, state.maps.height, state.maps.continents) || !writePGM8(output / "tectonic-map.pgm", state.maps.width, state.maps.height, state.maps.tectonics) ||
      (hasMountainDebugMap && !writePGM8(output / "mountain-map.pgm", state.maps.width, state.maps.height, state.mountainDebugMap)) || !writeConfiguration(output / "terrain-config.json", state))
  {
    if (error.empty())
      error = "Could not write terrain maps or configuration";
    return false;
  }

  bool packageWritten = false;
  if (state.generatedFromSphericalWorld)
  {
    procedural::SphericalTerrainMaps &sphereMaps = state.sphereExportMaps;
    if (!writePGM16(output / "ocean-floor-heightmap.pgm",
            sphereMaps.width, sphereMaps.height,
            sphereMaps.oceanFloorHeightmap))
    {
      error = "Could not write ocean-floor heightmap";
      return false;
    }
    // Rock type is a small class index, so the raw PGM is nearly black; it is
    // written for tooling and histograms rather than for looking at.
    if (sphereMaps.lithology.size() == static_cast<size_t>(sphereMaps.width) * sphereMaps.height &&
        !writePGM8(output / "lithology-map.pgm", sphereMaps.width, sphereMaps.height, sphereMaps.lithology))
    {
      error = "Could not write lithology map";
      return false;
    }
    populateSphereExportMesh(state);

    const procedural::PlanetPackageMetadata metadata = planetPackageMetadata(state);
    procedural::SphericalTerrainCubePackageSettings cubeSettings{};
    packageWritten = procedural::writeSphericalTerrainPackage(
        output / "planet.planetpkg",
        sphereMaps,
        metadata,
        cubeSettings,
        {},
        &state.planetPackageStatistics,
        &state.planetCubeStatistics,
        &error);
    procedural::PlanetMeshExportStatistics meshStatistics{};
    if (packageWritten)
      packageWritten = procedural::writeSphericalTerrainMeshPly(
          output / "planet-mesh.ply", sphereMaps, metadata,
          &meshStatistics, &error);
    if (packageWritten)
      packageWritten = procedural::writePlanetMetadataManifest(
          output / "planet-metadata.json", sphereMaps, metadata,
          meshStatistics, &error);
    if (packageWritten)
      std::cout << "Exported planet mesh: vertices="
                << meshStatistics.vertexCount
                << " faces=" << meshStatistics.faceCount
                << " radius="
                << procedural::planetRadiusKilometres(metadata)
                << " km\n";
  }
  else
  {
    packageWritten = procedural::writeTerrainPackage(output, state.settings, state.raster, state.maps, state.package, &state.packageStatistics, &error);
  }
  if (!packageWritten)
  {
    if (error.empty())
      error = "Could not write streaming terrain package";
    return false;
  }
  return true;
}

// Bakes the working package the adaptive globe displaces from, then decodes it
// back into an overview mesh plus surface atlas.
//
// The round trip through a file looks wasteful and is deliberate. The atlas and
// its per-face mips are built by buildSurfaceMaps sampling package raster tiles;
// feeding that from the in-memory equirect maps instead would mean a second
// reconstruction path, and a preview that disagrees with the exported planet
// without anything failing. At the preview's 1024 face resolution the whole
// round trip costs about a second, which the generator pays once per generation.
bool buildAdaptiveGlobe(ApplicationState &state, std::string &error)
{
  if (!state.generated || !state.generatedFromSphericalWorld)
  {
    error = "Adaptive globe needs a generated spherical world";
    return false;
  }
  const size_t expectedPixels = static_cast<size_t>(state.raster.width) * state.raster.height;
  if (state.sphereExportMaps.heightmap.size() != expectedPixels &&
      !generateSphericalPreview(state, error))
    return false;

  populateSphereExportMesh(state);
  const procedural::PlanetPackageMetadata metadata = planetPackageMetadata(state);

  std::error_code filesystemError;
  const std::filesystem::path workingDirectory =
      std::filesystem::temp_directory_path(filesystemError) / "polygon-terrain-globe";
  if (filesystemError)
  {
    error = "Could not resolve a working directory for the globe package";
    return false;
  }
  std::filesystem::create_directories(workingDirectory, filesystemError);
  if (filesystemError)
  {
    error = "Could not create " + workingDirectory.string();
    return false;
  }
  const std::filesystem::path packagePath = workingDirectory / "globe-preview.planetpkg";

  const procedural::SphericalTerrainCubePackageSettings cubeSettings{};
  if (!procedural::writeSphericalTerrainPackage(
          packagePath, state.sphereExportMaps, metadata, cubeSettings, {},
          nullptr, nullptr, &error))
  {
    if (error.empty())
      error = "Could not write the globe preview package";
    return false;
  }

  procedural::PlanetPackageReader package;
  if (!package.open(packagePath, &error))
  {
    if (error.empty())
      error = "Could not open the globe preview package";
    return false;
  }

  voxel::svraster::PlanetOverviewMeshSettings overviewSettings{};
  overviewSettings.surfaceFaceResolution = state.adaptiveGlobeFaceResolution;
  voxel::svraster::PlanetOverviewMesh mesh{};
  if (!voxel::svraster::loadPlanetOverviewMesh(package, overviewSettings, mesh, &error))
  {
    if (error.empty())
      error = "Could not decode the globe preview package";
    return false;
  }
  if (!mesh.surfaceMaps.valid())
  {
    error = "The globe preview package produced no surface atlas";
    return false;
  }

  state.planetOverview = std::move(mesh);
  state.adaptiveGlobePackagePath = packagePath;
  return true;
}

std::array<uint8_t, 4> heightColour(const uint16_t height)
{
  const float elevation = static_cast<float>(height) / 65535.0f;
  const uint8_t shade = static_cast<uint8_t>(std::lround(std::clamp(elevation, 0.0f, 1.0f) * 255.0f));
  return {shade, shade, shade, 255u};
}

std::array<uint8_t, 4> terrainElevationColour(const uint16_t height)
{
  if (height == 0u)
    return {8u, 29u, 66u, 255u};
  // Undo encodedLandElevation so the colour bands operate in the same raw
  // elevation domain as Orogen's elevationToColor(), not display-height space.
  const float encoded = static_cast<float>(height) / 65535.0f;
  const float normalized = std::clamp((encoded - 0.025f) / 0.955f, 0.0f, 0.999999f);
  const float elevation = -std::log(1.0f - normalized) / 1.55f;
  std::array<float, 3> color{};
  if (elevation < 0.002f)
  {
    const float t = elevation / 0.002f;
    color = {0.72f + t * 0.08f, 0.68f - t * 0.02f, 0.46f - t * 0.10f};
  }
  else if (elevation < 0.25f)
  {
    const float t = (elevation - 0.002f) / 0.248f;
    color = {0.20f - t * 0.06f, 0.54f - t * 0.12f, 0.12f + t * 0.08f};
  }
  else if (elevation < 0.50f)
  {
    const float t = (elevation - 0.25f) / 0.25f;
    color = {0.14f + t * 0.30f, 0.42f - t * 0.14f, 0.20f - t * 0.06f};
  }
  else if (elevation < 0.75f)
  {
    const float t = (elevation - 0.50f) / 0.25f;
    color = {0.44f + t * 0.16f, 0.28f + t * 0.12f, 0.14f + t * 0.18f};
  }
  else
  {
    const float t = std::clamp((elevation - 0.75f) / 0.20f, 0.0f, 1.0f);
    color = {0.60f + t * 0.35f, 0.40f + t * 0.50f, 0.32f + t * 0.60f};
  }
  return {
      static_cast<uint8_t>(std::lround(std::clamp(color[0], 0.0f, 1.0f) * 255.0f)),
      static_cast<uint8_t>(std::lround(std::clamp(color[1], 0.0f, 1.0f) * 255.0f)),
      static_cast<uint8_t>(std::lround(std::clamp(color[2], 0.0f, 1.0f) * 255.0f)),
      255u};
}

std::array<uint8_t, 4> regionColour(const uint8_t region)
{
  if (region == 0u)
    return {12u, 36u, 76u, 255u};
  const uint32_t hash = static_cast<uint32_t>(region) * 0x9e3779b9u;
  return {static_cast<uint8_t>(72u + (hash & 0x7fu)), static_cast<uint8_t>(72u + ((hash >> 8u) & 0x7fu)), static_cast<uint8_t>(72u + ((hash >> 16u) & 0x7fu)), 255u};
}

std::array<uint8_t, 4> materialColour(const uint8_t material)
{
  using Material = procedural::PolygonTerrainMaterial;
  switch (static_cast<Material>(material))
  {
  case Material::Ocean:
    return {13u, 41u, 84u, 255u};
  case Material::Lake:
    return {34u, 94u, 142u, 255u};
  case Material::Beach:
    return {208u, 190u, 121u, 255u};
  case Material::River:
    return {38u, 125u, 184u, 255u};
  case Material::Marsh:
    return {87u, 124u, 73u, 255u};
  case Material::Ice:
    return {205u, 230u, 238u, 255u};
  case Material::Snow:
    return {232u, 235u, 230u, 255u};
  case Material::Tundra:
    return {160u, 171u, 141u, 255u};
  case Material::Bare:
    return {142u, 134u, 119u, 255u};
  case Material::Scorched:
    return {88u, 84u, 78u, 255u};
  case Material::Taiga:
    return {73u, 118u, 92u, 255u};
  case Material::Shrubland:
    return {125u, 148u, 81u, 255u};
  case Material::TemperateDesert:
    return {196u, 174u, 112u, 255u};
  case Material::TemperateRainForest:
    return {45u, 117u, 75u, 255u};
  case Material::TemperateDeciduousForest:
    return {65u, 139u, 74u, 255u};
  case Material::Grass:
  case Material::Grassland:
    return {98u, 156u, 72u, 255u};
  case Material::TropicalRainForest:
    return {32u, 128u, 63u, 255u};
  case Material::TropicalSeasonalForest:
    return {59u, 150u, 70u, 255u};
  case Material::SubtropicalDesert:
    return {213u, 188u, 114u, 255u};
  case Material::Rock:
    return {130u, 123u, 111u, 255u};
  }
  return {240u, 80u, 120u, 255u};
}

std::array<uint8_t, 4> lerpColour(const std::array<uint8_t, 4> &first, const std::array<uint8_t, 4> &second, const float t)
{
  const float amount = std::clamp(t, 0.0f, 1.0f);
  return {
    static_cast<uint8_t>(std::lround(first[0] + (second[0] - first[0]) * amount)),
    static_cast<uint8_t>(std::lround(first[1] + (second[1] - first[1]) * amount)),
    static_cast<uint8_t>(std::lround(first[2] + (second[2] - first[2]) * amount)),
    255u};
}

std::array<uint8_t, 4> shadeColour(std::array<uint8_t, 4> colour, const uint16_t height)
{
  const float shade = 0.62f + static_cast<float>(height) / 65535.0f * 0.46f;
  colour[0] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(colour[0] * shade)), 0, 255));
  colour[1] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(colour[1] * shade)), 0, 255));
  colour[2] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(colour[2] * shade)), 0, 255));
  return colour;
}

std::array<uint8_t, 4> terrainFormColour(const uint8_t form, const uint16_t height)
{
  switch (form)
  {
  case 0u:
    return {13u, 41u, 84u, 255u};
  case 1u:
    return shadeColour({116u, 178u, 78u, 255u}, height);
  case 2u:
    return shadeColour({50u, 142u, 126u, 255u}, height);
  case 3u:
    return shadeColour({132u, 148u, 79u, 255u}, height);
  case 4u:
    return shadeColour({126u, 112u, 98u, 255u}, height);
  case 5u:
    return shadeColour({218u, 221u, 216u, 255u}, height);
  default:
    return {240u, 80u, 120u, 255u};
  }
}

std::array<uint8_t, 4> temperatureColour(const uint8_t value)
{
  const float t = static_cast<float>(value) / 255.0f;
  if (t < 0.35f)
    return lerpColour({44u, 77u, 152u, 255u}, {126u, 202u, 218u, 255u}, t / 0.35f);
  if (t < 0.68f)
    return lerpColour({126u, 202u, 218u, 255u}, {227u, 205u, 96u, 255u}, (t - 0.35f) / 0.33f);
  return lerpColour({227u, 205u, 96u, 255u}, {187u, 63u, 43u, 255u}, (t - 0.68f) / 0.32f);
}

std::array<uint8_t, 4> precipitationColour(const uint8_t value)
{
  const float t = static_cast<float>(value) / 255.0f;
  if (t < 0.42f)
    return lerpColour({169u, 135u, 74u, 255u}, {125u, 159u, 83u, 255u}, t / 0.42f);
  if (t < 0.74f)
    return lerpColour({125u, 159u, 83u, 255u}, {48u, 135u, 92u, 255u}, (t - 0.42f) / 0.32f);
  return lerpColour({48u, 135u, 92u, 255u}, {53u, 128u, 188u, 255u}, (t - 0.74f) / 0.26f);
}

std::array<uint8_t, 4> windColour(const uint8_t value)
{
  const float t = static_cast<float>(value) / 255.0f;
  if (t < 0.50f)
    return lerpColour({38u, 35u, 80u, 255u}, {45u, 131u, 157u, 255u}, t / 0.50f);
  return lerpColour({45u, 131u, 157u, 255u}, {218u, 235u, 226u, 255u}, (t - 0.50f) / 0.50f);
}

std::array<uint8_t, 4> forestColour(const uint8_t value)
{
  const float t = static_cast<float>(value) / 255.0f;
  if (t < 0.45f)
    return lerpColour({52u, 63u, 44u, 255u}, {82u, 126u, 63u, 255u}, t / 0.45f);
  return lerpColour({82u, 126u, 63u, 255u}, {23u, 101u, 53u, 255u}, (t - 0.45f) / 0.55f);
}

std::array<uint8_t, 4> settlementColour(const uint8_t value)
{
  const float t = static_cast<float>(value) / 255.0f;
  if (t < 0.40f)
    return lerpColour({42u, 38u, 50u, 255u}, {112u, 98u, 72u, 255u}, t / 0.40f);
  if (t < 0.76f)
    return lerpColour({112u, 98u, 72u, 255u}, {219u, 180u, 89u, 255u}, (t - 0.40f) / 0.36f);
  return lerpColour({219u, 180u, 89u, 255u}, {255u, 238u, 171u, 255u}, (t - 0.76f) / 0.24f);
}

std::array<uint8_t, 4> continentColour(const uint8_t value)
{
  const float t = static_cast<float>(value) / 255.0f;
  if (t < 0.45f)
    return lerpColour({11u, 35u, 79u, 255u}, {42u, 111u, 151u, 255u}, t / 0.45f);
  if (t < 0.58f)
    return lerpColour({42u, 111u, 151u, 255u}, {206u, 190u, 121u, 255u}, (t - 0.45f) / 0.13f);
  return lerpColour({102u, 151u, 72u, 255u}, {111u, 91u, 67u, 255u}, (t - 0.58f) / 0.42f);
}

std::array<uint8_t, 4> tectonicColour(const uint8_t value)
{
  const float t = static_cast<float>(value) / 255.0f;
  if (t < 0.50f)
    return lerpColour({35u, 33u, 64u, 255u}, {102u, 76u, 120u, 255u}, t / 0.50f);
  return lerpColour({102u, 76u, 120u, 255u}, {238u, 187u, 104u, 255u}, (t - 0.50f) / 0.50f);
}

std::array<uint8_t, 4> mountainColour(const uint8_t value)
{
  const float t = static_cast<float>(value) / 255.0f;
  if (t < 0.35f)
    return lerpColour({28u, 44u, 58u, 255u}, {82u, 112u, 79u, 255u}, t / 0.35f);
  if (t < 0.72f)
    return lerpColour({82u, 112u, 79u, 255u}, {128u, 111u, 94u, 255u}, (t - 0.35f) / 0.37f);
  return lerpColour({128u, 111u, 94u, 255u}, {236u, 237u, 230u, 255u}, (t - 0.72f) / 0.28f);
}

std::array<uint8_t, 4> applyShade(std::array<uint8_t, 4> colour, const float shade)
{
  const float amount = std::clamp(shade, 0.30f, 1.55f);
  for (uint32_t channel = 0u; channel < 3u; ++channel)
    colour[channel] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(static_cast<float>(colour[channel]) * amount)), 0, 255));
  return colour;
}

float normalizedMapHeight(const procedural::PolygonTerrainMaps &maps, int32_t x, int32_t y)
{
  if (maps.width == 0u || maps.height == 0u)
    return 0.0f;
  if (y < 0)
  {
    y = -y - 1;
    x += static_cast<int32_t>(maps.width / 2u);
  }
  else if (y >= static_cast<int32_t>(maps.height))
  {
    y = static_cast<int32_t>(maps.height) * 2 - y - 1;
    x += static_cast<int32_t>(maps.width / 2u);
  }
  y = std::clamp(y, 0, static_cast<int32_t>(maps.height) - 1);
  while (x < 0)
    x += static_cast<int32_t>(maps.width);
  if (x >= static_cast<int32_t>(maps.width))
    x %= static_cast<int32_t>(maps.width);
  const size_t pixel = static_cast<size_t>(y) * maps.width + static_cast<uint32_t>(x);
  return static_cast<float>(maps.heightmap[pixel]) / 65535.0f;
}

size_t sphericalMapPixel(int32_t x, int32_t y, const uint32_t width, const uint32_t height)
{
  if (y < 0)
  {
    y = -y - 1;
    x += static_cast<int32_t>(width / 2u);
  }
  else if (y >= static_cast<int32_t>(height))
  {
    y = static_cast<int32_t>(height) * 2 - y - 1;
    x += static_cast<int32_t>(width / 2u);
  }
  y = std::clamp(y, 0, static_cast<int32_t>(height) - 1);
  x %= static_cast<int32_t>(width);
  if (x < 0)
    x += static_cast<int32_t>(width);
  return static_cast<size_t>(y) * width + static_cast<uint32_t>(x);
}

std::vector<uint16_t> previewDistanceFromOcean(const procedural::PolygonTerrainMaps &maps)
{
  const size_t pixelCount = static_cast<size_t>(maps.width) * maps.height;
  constexpr uint16_t unknown = std::numeric_limits<uint16_t>::max();
  std::vector<uint16_t> distance(pixelCount, unknown);
  std::vector<size_t> queue;
  queue.reserve(pixelCount / 3u);
  for (uint32_t y = 0u; y < maps.height; ++y)
    for (uint32_t x = 0u; x < maps.width; ++x)
    {
      const size_t pixel = static_cast<size_t>(y) * maps.width + x;
      if (maps.heightmap[pixel] == 0u)
      {
        distance[pixel] = 0u;
        continue;
      }
      for (int32_t oy = -1; oy <= 1 && distance[pixel] == unknown; ++oy)
        for (int32_t ox = -1; ox <= 1; ++ox)
          if ((ox != 0 || oy != 0) && maps.heightmap[sphericalMapPixel(static_cast<int32_t>(x) + ox, static_cast<int32_t>(y) + oy, maps.width, maps.height)] == 0u)
          {
            distance[pixel] = 1u;
            queue.push_back(pixel);
            break;
          }
    }
  for (size_t head = 0u; head < queue.size(); ++head)
  {
    const size_t pixel = queue[head];
    const uint32_t x = static_cast<uint32_t>(pixel % maps.width);
    const uint32_t y = static_cast<uint32_t>(pixel / maps.width);
    const uint16_t nextDistance = distance[pixel] >= unknown - 1u ? unknown : static_cast<uint16_t>(distance[pixel] + 1u);
    for (int32_t oy = -1; oy <= 1; ++oy)
      for (int32_t ox = -1; ox <= 1; ++ox)
      {
        if (ox == 0 && oy == 0)
          continue;
        const size_t neighbour = sphericalMapPixel(static_cast<int32_t>(x) + ox, static_cast<int32_t>(y) + oy, maps.width, maps.height);
        if (maps.heightmap[neighbour] != 0u && distance[neighbour] > nextDistance)
        {
          distance[neighbour] = nextDistance;
          queue.push_back(neighbour);
        }
      }
  }
  return distance;
}

float reliefShade(const procedural::PolygonTerrainMaps &maps, const std::vector<uint8_t> &mountains, const uint32_t x, const uint32_t y)
{
  const size_t pixel = static_cast<size_t>(y) * maps.width + x;
  const float center = static_cast<float>(maps.heightmap[pixel]) / 65535.0f;
  if (center <= 0.0f)
    return 1.0f;

  const int32_t ix = static_cast<int32_t>(x);
  const int32_t iy = static_cast<int32_t>(y);
  const float left = normalizedMapHeight(maps, ix - 1, iy);
  const float right = normalizedMapHeight(maps, ix + 1, iy);
  const float up = normalizedMapHeight(maps, ix, iy - 1);
  const float down = normalizedMapHeight(maps, ix, iy + 1);
  const float dx = right - left;
  const float dy = down - up;
  const float nx = -dx * 36.0f;
  const float ny = -dy * 36.0f;
  const float nz = 1.0f;
  const float invNormalLength = 1.0f / std::max(0.0001f, std::sqrt(nx * nx + ny * ny + nz * nz));
  const float lightX = -0.42f;
  const float lightY = -0.56f;
  const float lightZ = 0.72f;
  const float lambert = std::max(0.0f, (nx * lightX + ny * lightY + nz * lightZ) * invNormalLength);
  const float curvature = center * 4.0f - left - right - up - down;
  const float ridge = std::clamp(curvature * 18.0f, 0.0f, 0.26f);
  const float valley = std::clamp(-curvature * 22.0f, 0.0f, 0.22f);
  const float mountain = mountains.size() == maps.heightmap.size() ? static_cast<float>(mountains[pixel]) / 255.0f : 0.0f;
  return std::clamp(0.58f + lambert * 0.48f + ridge - valley + mountain * 0.12f, 0.34f, 1.48f);
}

std::array<uint8_t, 4> reliefHeightColour(const procedural::PolygonTerrainMaps &maps, const std::vector<uint8_t> &mountains, const uint32_t x, const uint32_t y)
{
  const size_t pixel = static_cast<size_t>(y) * maps.width + x;
  return applyShade(heightColour(maps.heightmap[pixel]), reliefShade(maps, mountains, x, y));
}

std::array<uint8_t, 4> reliefMountainColour(const procedural::PolygonTerrainMaps &maps, const std::vector<uint8_t> &mountains, const uint32_t x, const uint32_t y)
{
  const size_t pixel = static_cast<size_t>(y) * maps.width + x;
  const uint8_t mountain = mountains.size() == maps.heightmap.size() ? mountains[pixel] : 0u;
  std::array<uint8_t, 4> colour = mountainColour(mountain);
  if (maps.heightmap[pixel] == 0u)
    colour = {13u, 41u, 84u, 255u};
  return applyShade(colour, reliefShade(maps, mountains, x, y));
}

std::array<uint8_t, 4> reliefRiverColour(const procedural::PolygonTerrainMaps &maps, const std::vector<uint8_t> &mountains, const uint32_t x, const uint32_t y)
{
  const size_t pixel = static_cast<size_t>(y) * maps.width + x;
  std::array<uint8_t, 4> colour = applyShade(heightColour(maps.heightmap[pixel]), reliefShade(maps, mountains, x, y) * 0.58f);
  const uint8_t river = maps.rivers[pixel];
  if (river == 0u)
    return colour;
  const float t = static_cast<float>(river) / 255.0f;
  const std::array<uint8_t, 4> water = lerpColour({38u, 116u, 178u, 255u}, {120u, 205u, 244u, 255u}, t);
  const float mixAmount = std::clamp(0.48f + t * 0.52f, 0.0f, 1.0f);
  return lerpColour(colour, water, mixAmount);
}

uint8_t sphereMaterial(const uint16_t height, const uint8_t river, const uint8_t temperature, const uint8_t precipitation, const uint8_t mountain)
{
  using Material = procedural::PolygonTerrainMaterial;
  if (height == 0u)
    return static_cast<uint8_t>(Material::Ocean);
  if (river > 0u)
    return static_cast<uint8_t>(Material::River);
  const float elevation = static_cast<float>(height) / 65535.0f;
  const float temp = static_cast<float>(temperature) / 255.0f;
  const float rain = static_cast<float>(precipitation) / 255.0f;
  const float mountains = static_cast<float>(mountain) / 255.0f;
  if (temp < 0.12f && elevation > 0.16f)
    return static_cast<uint8_t>(Material::Ice);
  if (elevation < 0.10f)
    return static_cast<uint8_t>(Material::Beach);
  if (mountains > 0.68f || elevation > 0.72f)
  {
    if (temp < 0.34f)
      return static_cast<uint8_t>(Material::Snow);
    if (rain < 0.16f && temp > 0.68f)
      return static_cast<uint8_t>(Material::Scorched);
    return rain < 0.30f ? static_cast<uint8_t>(Material::Bare) : static_cast<uint8_t>(Material::Rock);
  }
  if (temp < 0.24f)
    return rain > 0.42f ? static_cast<uint8_t>(Material::Taiga) : static_cast<uint8_t>(Material::Tundra);
  if (rain < 0.18f)
    return temp > 0.66f ? static_cast<uint8_t>(Material::SubtropicalDesert) : static_cast<uint8_t>(Material::TemperateDesert);
  if (rain < 0.34f)
    return static_cast<uint8_t>(Material::Shrubland);
  if (temp > 0.70f)
  {
    if (rain > 0.72f)
      return static_cast<uint8_t>(Material::TropicalRainForest);
    if (rain > 0.44f)
      return static_cast<uint8_t>(Material::TropicalSeasonalForest);
    return static_cast<uint8_t>(Material::Grassland);
  }
  if (rain > 0.76f)
    return static_cast<uint8_t>(Material::TemperateRainForest);
  if (rain > 0.50f)
    return static_cast<uint8_t>(Material::TemperateDeciduousForest);
  if (temp < 0.42f && rain > 0.42f)
    return static_cast<uint8_t>(Material::Taiga);
  return static_cast<uint8_t>(Material::Grassland);
}

uint8_t sphereTerrainForm(const uint16_t height, const uint8_t river, const uint8_t mountain)
{
  if (height == 0u)
    return 0u;
  const float elevation = static_cast<float>(height) / 65535.0f;
  const float mountains = static_cast<float>(mountain) / 255.0f;
  if (mountains > 0.78f || elevation > 0.78f)
    return 5u;
  if (mountains > 0.42f || elevation > 0.56f)
    return 4u;
  if (river > 0u)
    return 2u;
  if (elevation > 0.34f)
    return 3u;
  return 1u;
}

procedural::PolygonTerrainMaps makePolygonPreviewMapsFromSphere(const procedural::SphericalTerrainMaps &sphere)
{
  procedural::PolygonTerrainMaps maps{};
  maps.width = sphere.width;
  maps.height = sphere.height;
  const size_t pixelCount = static_cast<size_t>(sphere.width) * sphere.height;
  maps.heightmap = sphere.heightmap;
  maps.sourceRegions = sphere.sourceRegions;
  maps.rivers = sphere.rivers;
  maps.regions.resize(pixelCount, 0u);
  for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    uint32_t hash = maps.sourceRegions[pixel] * 0x9e3779b9u + 0x85ebca6bu;
    hash ^= hash >> 16u;
    maps.regions[pixel] = static_cast<uint8_t>((hash >> 24u) | 1u);
  }
  maps.temperature = sphere.temperature;
  maps.precipitation = sphere.precipitation;
  maps.continents = sphere.continents;
  maps.tectonics = sphere.tectonics;
  const bool hasGeneratedMaterials = sphere.materials.size() == pixelCount;
  maps.materials = hasGeneratedMaterials ? sphere.materials : std::vector<uint8_t>(pixelCount, 0u);
  maps.terrainForms.assign(pixelCount, 0u);
  maps.wind = sphere.wind.size() == pixelCount ? sphere.wind : std::vector<uint8_t>(pixelCount, 0u);
  maps.forests.assign(pixelCount, 0u);
  maps.settlements.assign(pixelCount, 0u);
  for (uint32_t y = 0u; y < sphere.height; ++y)
  {
    const float latitude01 = (static_cast<float>(y) + 0.5f) / static_cast<float>(sphere.height);
    for (uint32_t x = 0u; x < sphere.width; ++x)
    {
      const size_t pixel = static_cast<size_t>(y) * sphere.width + x;
      const uint16_t height = sphere.heightmap[pixel];
      const uint8_t river = sphere.rivers[pixel];
      const uint8_t temperature = sphere.temperature[pixel];
      const uint8_t precipitation = sphere.precipitation[pixel];
      const uint8_t mountain = sphere.mountains[pixel];
      if (!hasGeneratedMaterials)
        maps.materials[pixel] = sphereMaterial(height, river, temperature, precipitation, mountain);
      maps.terrainForms[pixel] = sphereTerrainForm(height, river, mountain);
      if (sphere.wind.size() != pixelCount)
        maps.wind[pixel] = static_cast<uint8_t>(std::clamp(95 + static_cast<int>(std::lround(std::sin(latitude01 * 3.14159265f * 6.0f) * 62.0f)), 0, 255));
      if (height != 0u)
      {
        const float temp = static_cast<float>(temperature) / 255.0f;
        const float rain = static_cast<float>(precipitation) / 255.0f;
        const float mountains = static_cast<float>(mountain) / 255.0f;
        const float forest = std::clamp((rain - 0.30f) * 1.55f * (1.0f - std::max(0.0f, mountains - 0.35f)), 0.0f, 1.0f);
        const float settlement = std::clamp((1.0f - mountains * 1.4f) * (1.0f - std::abs(temp - 0.54f) * 1.8f) * (0.30f + (river > 0u ? 0.70f : rain * 0.32f)), 0.0f, 1.0f);
        maps.forests[pixel] = static_cast<uint8_t>(std::lround(forest * 255.0f));
        maps.settlements[pixel] = static_cast<uint8_t>(std::lround(settlement * 255.0f));
      }
    }
  }
  // Diagnostics the polygon path computes and the spherical path does not.
  // They have to exist at full size regardless: the preview uploader builds one
  // texture per map and treats a missing one as a failure, so leaving them empty
  // would turn "no forest diagnostic on a planet" into "generation failed".
  if (maps.terrainForms.size() != pixelCount)
    maps.terrainForms.assign(pixelCount, 0u);
  if (maps.forests.size() != pixelCount)
    maps.forests.assign(pixelCount, 0u);
  if (maps.settlements.size() != pixelCount)
    maps.settlements.assign(pixelCount, 0u);
  if (maps.regions.size() != pixelCount)
    maps.regions.assign(pixelCount, 0u);

  return maps;
}

void refreshDerivedMapsAfterErosion(ApplicationState &state)
{
  const size_t pixelCount = static_cast<size_t>(state.maps.width) * state.maps.height;
  if (state.maps.heightmap.size() != pixelCount || state.maps.rivers.size() != pixelCount)
    return;
  const std::vector<uint8_t> tectonicReliefMask = state.mountainDebugMap;
  const std::vector<uint16_t> coastDistance = previewDistanceFromOcean(state.maps);
  const float coastBand = static_cast<float>(std::clamp(state.maps.height / 24u, 10u, 36u));
  state.mountainDebugMap.assign(pixelCount, 0u);
  for (uint32_t y = 0u; y < state.maps.height; ++y)
    for (uint32_t x = 0u; x < state.maps.width; ++x)
    {
      const size_t pixel = static_cast<size_t>(y) * state.maps.width + x;
      const float center = normalizedMapHeight(state.maps, static_cast<int32_t>(x), static_cast<int32_t>(y));
      if (center <= 0.0f)
        continue;
      const std::array<float, 4> neighbours{
        normalizedMapHeight(state.maps, static_cast<int32_t>(x) - 1, static_cast<int32_t>(y)),
        normalizedMapHeight(state.maps, static_cast<int32_t>(x) + 1, static_cast<int32_t>(y)),
        normalizedMapHeight(state.maps, static_cast<int32_t>(x), static_cast<int32_t>(y) - 1),
        normalizedMapHeight(state.maps, static_cast<int32_t>(x), static_cast<int32_t>(y) + 1)};
      float maximumSlope = 0.0f;
      float neighbourMean = 0.0f;
      uint32_t landNeighbours = 0u;
      bool touchesOcean = false;
      for (const float neighbour : neighbours)
      {
        if (neighbour <= 0.0f)
        {
          touchesOcean = true;
          continue;
        }
        maximumSlope = std::max(maximumSlope, std::abs(center - neighbour));
        neighbourMean += neighbour;
        ++landNeighbours;
      }
      neighbourMean /= std::max(1u, landNeighbours);
      const float curvature = std::max(0.0f, center - neighbourMean);
      const float tectonics = state.maps.tectonics.size() == pixelCount ? static_cast<float>(state.maps.tectonics[pixel]) / 255.0f : 0.0f;
      const float reliefMask = tectonicReliefMask.size() == pixelCount ? static_cast<float>(tectonicReliefMask[pixel]) / 255.0f : tectonics;
      const float erodedRelief = std::clamp(maximumSlope * 9.0f + curvature * 28.0f, 0.0f, 1.0f);
      const float highland = std::clamp((center - 0.34f) / 0.38f, 0.0f, 1.0f);
      const float tectonicWeight = std::clamp((tectonics - 0.08f) / 0.62f, 0.0f, 1.0f);
      float mountain = std::clamp(erodedRelief * (0.08f + reliefMask * 0.72f + tectonicWeight * 0.16f) + highland * (0.10f + tectonicWeight * 0.14f), 0.0f, 1.0f);
      if (touchesOcean)
        mountain *= 0.25f;
      const float coastT = std::clamp((static_cast<float>(coastDistance[pixel]) - 1.0f) / std::max(1.0f, coastBand - 1.0f), 0.0f, 1.0f);
      mountain *= coastT * coastT * (3.0f - 2.0f * coastT);
      state.mountainDebugMap[pixel] = static_cast<uint8_t>(std::lround(mountain * 255.0f));
    }

  for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    const uint16_t height = state.maps.heightmap[pixel];
    const uint8_t river = state.maps.rivers[pixel];
    const uint8_t temperature = state.maps.temperature[pixel];
    const uint8_t precipitation = state.maps.precipitation[pixel];
    const uint8_t mountain = state.mountainDebugMap[pixel];
    if (state.maps.materials[pixel] != static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Lake))
      state.maps.materials[pixel] = sphereMaterial(height, river, temperature, precipitation, mountain);
    state.maps.terrainForms[pixel] = sphereTerrainForm(height, river, mountain);
    if (height == 0u)
    {
      state.maps.forests[pixel] = 0u;
      state.maps.settlements[pixel] = 0u;
      continue;
    }
    const float temp = static_cast<float>(temperature) / 255.0f;
    const float rain = static_cast<float>(precipitation) / 255.0f;
    const float mountains = static_cast<float>(mountain) / 255.0f;
    const float forest = std::clamp((rain - 0.30f) * 1.55f * (1.0f - std::max(0.0f, mountains - 0.35f)), 0.0f, 1.0f);
    const float settlement = std::clamp((1.0f - mountains * 1.4f) * (1.0f - std::abs(temp - 0.54f) * 1.8f) * (0.30f + (river > 0u ? 0.70f : rain * 0.32f)), 0.0f, 1.0f);
    state.maps.forests[pixel] = static_cast<uint8_t>(std::lround(forest * 255.0f));
    state.maps.settlements[pixel] = static_cast<uint8_t>(std::lround(settlement * 255.0f));
  }
}

uint32_t automaticWorkerCount(const procedural::PolygonTerrainRasterSettings &raster)
{
  if (raster.workerCount != 0u)
    return raster.workerCount;
  return 64u;
}

std::array<uint8_t, 4> directSphereColour(const ApplicationState &state, const size_t pixel)
{
  const uint8_t mountain = state.mountainDebugMap.size() == state.maps.heightmap.size() ? state.mountainDebugMap[pixel] : 0u;
  switch (static_cast<SpherePreviewLayer>(state.spherePreviewLayer))
  {
  case SpherePreviewLayer::Terrain:
    return terrainElevationColour(state.maps.heightmap[pixel]);
  case SpherePreviewLayer::Height:
    return heightColour(state.maps.heightmap[pixel]);
  case SpherePreviewLayer::Continents:
    return continentColour(state.maps.continents[pixel]);
  case SpherePreviewLayer::Tectonics:
    return tectonicColour(state.maps.tectonics[pixel]);
  case SpherePreviewLayer::Mountains:
    return state.maps.heightmap[pixel] == 0u ? std::array<uint8_t, 4>{13u, 41u, 84u, 255u} : mountainColour(mountain);
  case SpherePreviewLayer::Temperature:
    return temperatureColour(state.maps.temperature[pixel]);
  case SpherePreviewLayer::Rainfall:
    return precipitationColour(state.maps.precipitation[pixel]);
  case SpherePreviewLayer::Plates:
    return regionColour(state.maps.regions[pixel]);
  case SpherePreviewLayer::Rivers:
  {
    std::array<uint8_t, 4> colour = materialColour(state.maps.materials[pixel]);
    const uint8_t river = state.maps.rivers[pixel];
    if (river == 0u)
      return colour;
    const float strength = static_cast<float>(river) / 255.0f;
    const std::array<uint8_t, 4> water = lerpColour({31u, 101u, 167u, 255u}, {111u, 201u, 242u, 255u}, strength);
    return lerpColour(colour, water, std::clamp(0.52f + strength * 0.42f, 0.0f, 1.0f));
  }
  case SpherePreviewLayer::Materials:
  default:
    return materialColour(state.maps.materials[pixel]);
  }
}

void rebuildDirectSphereColours(ApplicationState &state)
{
  const size_t pixelCount = static_cast<size_t>(state.maps.width) * state.maps.height;
  state.sphereDirectRGBA.resize(pixelCount * 4u);
  const uint32_t workerCount = state.maps.height == 0u ? 1u : std::max(1u, std::min(automaticWorkerCount(state.raster), state.maps.height));
  std::vector<std::thread> workers;
  workers.reserve(workerCount);
  for (uint32_t worker = 0u; worker < workerCount; ++worker)
  {
    const uint32_t firstRow = static_cast<uint32_t>((static_cast<uint64_t>(state.maps.height) * worker) / workerCount);
    const uint32_t endRow = static_cast<uint32_t>((static_cast<uint64_t>(state.maps.height) * (worker + 1u)) / workerCount);
    workers.emplace_back(
        [&state, firstRow, endRow]
        {
          for (uint32_t y = firstRow; y < endRow; ++y)
            for (uint32_t x = 0u; x < state.maps.width; ++x)
            {
              const size_t pixel = static_cast<size_t>(y) * state.maps.width + x;
              const std::array<uint8_t, 4> colour = directSphereColour(state, pixel);
              std::copy(colour.begin(), colour.end(), state.sphereDirectRGBA.begin() + static_cast<std::ptrdiff_t>(pixel * 4u));
            }
        });
  }
  for (std::thread &worker : workers)
    worker.join();
  state.sphereDirectLayer = state.spherePreviewLayer;
}

uint32_t packSphereColor(const std::array<uint8_t, 4> &color)
{
  return (static_cast<uint32_t>(color[0]) << 24u) | (static_cast<uint32_t>(color[1]) << 16u) |
         (static_cast<uint32_t>(color[2]) << 8u) | static_cast<uint32_t>(color[3]);
}

void rebuildSurfaceSphereColors(ApplicationState &state)
{
  const size_t count = state.sphereSurfacePositions.size();
  state.sphereSurfaceColors.resize(count);
  const auto valid = [count](const auto &values) { return values.size() == count; };
  for (size_t vertex = 0u; vertex < count; ++vertex)
  {
    const uint16_t height = state.sphereSurfaceElevations[vertex];
    const uint8_t continent = valid(state.sphereSurfaceContinents) ? state.sphereSurfaceContinents[vertex] : 0u;
    const uint8_t tectonics = valid(state.sphereSurfaceTectonics) ? state.sphereSurfaceTectonics[vertex] : 0u;
    const uint8_t mountain = valid(state.sphereSurfaceMountains) ? state.sphereSurfaceMountains[vertex] : 0u;
    const uint8_t temperature = valid(state.sphereSurfaceTemperature) ? state.sphereSurfaceTemperature[vertex] : 127u;
    const uint8_t precipitation = valid(state.sphereSurfacePrecipitation) ? state.sphereSurfacePrecipitation[vertex] : 127u;
    const uint8_t plate = valid(state.sphereSurfacePlates) ? state.sphereSurfacePlates[vertex] : 0u;
    const uint8_t river = valid(state.sphereSurfaceRivers) ? state.sphereSurfaceRivers[vertex] : 0u;
    const uint8_t material = valid(state.sphereSurfaceMaterials) ? state.sphereSurfaceMaterials[vertex] : sphereMaterial(height, river, temperature, precipitation, mountain);
    std::array<uint8_t, 4> color{};
    switch (static_cast<SpherePreviewLayer>(state.spherePreviewLayer))
    {
    case SpherePreviewLayer::Height:
      color = heightColour(height);
      break;
    case SpherePreviewLayer::Continents:
      color = continentColour(continent);
      break;
    case SpherePreviewLayer::Tectonics:
      color = tectonicColour(tectonics);
      break;
    case SpherePreviewLayer::Mountains:
      color = height == 0u ? std::array<uint8_t, 4>{13u, 41u, 84u, 255u} : mountainColour(mountain);
      break;
    case SpherePreviewLayer::Temperature:
      color = temperatureColour(temperature);
      break;
    case SpherePreviewLayer::Rainfall:
      color = precipitationColour(precipitation);
      break;
    case SpherePreviewLayer::Plates:
      color = regionColour(plate);
      break;
    case SpherePreviewLayer::Rivers:
    {
      color = materialColour(material);
      if (river != 0u)
      {
        const float strength = static_cast<float>(river) / 255.0f;
        color = lerpColour(color, lerpColour({31u, 101u, 167u, 255u}, {111u, 201u, 242u, 255u}, strength), 0.08f + strength * 0.16f);
      }
      break;
    }
    case SpherePreviewLayer::Terrain:
      color = terrainElevationColour(height);
      break;
    case SpherePreviewLayer::Materials:
    default:
      color = materialColour(material);
      break;
    }
    state.sphereSurfaceColors[vertex] = packSphereColor(color);
  }
  state.sphereDirectLayer = state.spherePreviewLayer;
}

void updateSphereDirectPreview(ApplicationState &state, TerrainPreviewSpherePass &spherePass)
{
  if (!state.generated || !state.generatedFromSphericalWorld)
    return;
  if (state.sphereDirectRevision != state.previewRevision || state.sphereDirectLayer != state.spherePreviewLayer)
  {
    rebuildSurfaceSphereColors(state);
    spherePass.setSurfaceMesh(state.sphereSurfacePositions, state.sphereSurfaceElevations, state.sphereSurfaceIndices, &state.sphereSurfaceColors);
    state.sphereDirectRevision = state.previewRevision;
  }
}

void updateSphereLineOverlay(ApplicationState &state, TerrainPreviewSpherePass &spherePass)
{
  if (!state.generated || !state.generatedFromSphericalWorld)
    return;
  if (state.sphereLineRevision == state.previewRevision && state.sphereLineBoundaries == state.sphereShowPlateBoundaries && state.sphereLineLayer == state.spherePreviewLayer)
    return;
  std::vector<TerrainPreviewSphereLine> lines;
  lines.reserve(state.sphereLines.size());
  for (const procedural::SphericalTerrainLine &line : state.sphereLines)
  {
    const bool isRiver = line.color[2] > 0.70f && line.color[1] > 0.30f;
    const bool isPlateBoundary = !isRiver && line.color[3] >= 0.75f && line.width >= 0.002f;
    const SpherePreviewLayer layer = static_cast<SpherePreviewLayer>(state.spherePreviewLayer);
    if (isRiver && layer != SpherePreviewLayer::Terrain && layer != SpherePreviewLayer::Materials && layer != SpherePreviewLayer::Rivers)
      continue;
    if (!isRiver && (!state.sphereShowPlateBoundaries || !isPlateBoundary))
      continue;
    lines.push_back(
        TerrainPreviewSphereLine{
          {static_cast<float>(line.a.x), static_cast<float>(line.a.y), static_cast<float>(line.a.z)}, {static_cast<float>(line.b.x), static_cast<float>(line.b.y), static_cast<float>(line.b.z)}, line.color, line.width});
  }
  spherePass.setOverlayLines(lines);
  state.sphereLineRevision = state.previewRevision;
  state.sphereLineBoundaries = state.sphereShowPlateBoundaries;
  state.sphereLineLayer = state.spherePreviewLayer;
}

bool updatePreviewTextures(
    ApplicationState &state,
    TerrainPreviewRHI &rhi,
    TerrainPreviewTextureUploadPass &uploadPass,
    TerrainPreviewRasterPass &rasterPass)
{
  state.textures.reset(&rhi);
  ++state.previewRevision;
  state.sphereDirectRevision = 0u;
  state.sphereDirectLayer = -1;
  state.sphereDirectRGBA.clear();
  state.sphereLineRevision = 0u;
  state.sphereLineLayer = -1;
  const size_t pixelCount = static_cast<size_t>(state.maps.width) * state.maps.height;
  if (state.maps.heightmap.size() != pixelCount || state.maps.regions.size() != pixelCount || state.maps.materials.size() != pixelCount || state.maps.rivers.size() != pixelCount ||
      state.maps.terrainForms.size() != pixelCount || state.maps.temperature.size() != pixelCount || state.maps.precipitation.size() != pixelCount || state.maps.wind.size() != pixelCount ||
      state.maps.forests.size() != pixelCount || state.maps.settlements.size() != pixelCount || state.maps.continents.size() != pixelCount || state.maps.tectonics.size() != pixelCount)
  {
    state.status = "Generated maps have invalid preview dimensions";
    return false;
  }
  std::string gpuRasterError;
  if (rasterPass.rasterize(state.maps, state.mountainDebugMap, state.textures, gpuRasterError))
  {
    state.status = "Preview atlases rasterized on the GPU.";
    return true;
  }
  state.status = gpuRasterError + "; using the CPU preview fallback.";
  const bool hasMountainDebugMap = state.mountainDebugMap.size() == pixelCount;
  std::vector<uint8_t> heightRGBA(pixelCount * 4u);
  std::vector<uint8_t> riverRGBA(pixelCount * 4u);
  std::vector<uint8_t> regionRGBA(pixelCount * 4u);
  std::vector<uint8_t> materialRGBA(pixelCount * 4u);
  std::vector<uint8_t> terrainFormRGBA(pixelCount * 4u);
  std::vector<uint8_t> temperatureRGBA(pixelCount * 4u);
  std::vector<uint8_t> precipitationRGBA(pixelCount * 4u);
  std::vector<uint8_t> windRGBA(pixelCount * 4u);
  std::vector<uint8_t> forestRGBA(pixelCount * 4u);
  std::vector<uint8_t> settlementRGBA(pixelCount * 4u);
  std::vector<uint8_t> continentRGBA(pixelCount * 4u);
  std::vector<uint8_t> tectonicRGBA(pixelCount * 4u);
  std::vector<uint8_t> mountainRGBA(hasMountainDebugMap ? pixelCount * 4u : 0u);
  const uint32_t workerCount = state.maps.height == 0u ? 1u : std::max(1u, std::min(automaticWorkerCount(state.raster), state.maps.height));
  std::vector<std::thread> workers;
  workers.reserve(workerCount);
  const auto convertRows = [&](const uint32_t firstRow, const uint32_t endRow)
  {
    for (uint32_t y = firstRow; y < endRow; ++y)
      for (uint32_t x = 0u; x < state.maps.width; ++x)
      {
        const size_t pixel = static_cast<size_t>(y) * state.maps.width + x;
        const auto height = reliefHeightColour(state.maps, state.mountainDebugMap, x, y);
        const auto region = regionColour(state.maps.regions[pixel]);
        const auto material = applyShade(materialColour(state.maps.materials[pixel]), reliefShade(state.maps, state.mountainDebugMap, x, y));
        const auto terrainForm = terrainFormColour(state.maps.terrainForms[pixel], state.maps.heightmap[pixel]);
        const auto temperature = temperatureColour(state.maps.temperature[pixel]);
        const auto precipitation = precipitationColour(state.maps.precipitation[pixel]);
        const auto wind = windColour(state.maps.wind[pixel]);
        const auto forest = forestColour(state.maps.forests[pixel]);
        const auto settlement = settlementColour(state.maps.settlements[pixel]);
        const auto continent = continentColour(state.maps.continents[pixel]);
        const auto tectonic = tectonicColour(state.maps.tectonics[pixel]);
        const auto mountain = hasMountainDebugMap ? reliefMountainColour(state.maps, state.mountainDebugMap, x, y) : std::array<uint8_t, 4>{0u, 0u, 0u, 255u};
        const auto river = reliefRiverColour(state.maps, state.mountainDebugMap, x, y);
        for (uint32_t channel = 0u; channel < 4u; ++channel)
        {
          heightRGBA[pixel * 4u + channel] = height[channel];
          regionRGBA[pixel * 4u + channel] = region[channel];
          materialRGBA[pixel * 4u + channel] = material[channel];
          terrainFormRGBA[pixel * 4u + channel] = terrainForm[channel];
          temperatureRGBA[pixel * 4u + channel] = temperature[channel];
          precipitationRGBA[pixel * 4u + channel] = precipitation[channel];
          windRGBA[pixel * 4u + channel] = wind[channel];
          forestRGBA[pixel * 4u + channel] = forest[channel];
          settlementRGBA[pixel * 4u + channel] = settlement[channel];
          continentRGBA[pixel * 4u + channel] = continent[channel];
          tectonicRGBA[pixel * 4u + channel] = tectonic[channel];
          if (hasMountainDebugMap)
            mountainRGBA[pixel * 4u + channel] = mountain[channel];
          riverRGBA[pixel * 4u + channel] = river[channel];
        }
      }
  };
  for (uint32_t worker = 0u; worker < workerCount; ++worker)
  {
    const uint32_t firstRow = static_cast<uint32_t>((static_cast<uint64_t>(state.maps.height) * worker) / workerCount);
    const uint32_t endRow = static_cast<uint32_t>((static_cast<uint64_t>(state.maps.height) * (worker + 1u)) / workerCount);
    workers.emplace_back(convertRows, firstRow, endRow);
  }
  for (std::thread &worker : workers)
    worker.join();
  state.status = "Uploading preview textures...";
  state.textures.height = uploadPass.uploadRGBA("polygonTerrain.height", state.maps.width, state.maps.height, heightRGBA);
  state.textures.rivers = uploadPass.uploadRGBA("polygonTerrain.rivers", state.maps.width, state.maps.height, riverRGBA);
  state.textures.regions = uploadPass.uploadRGBA("polygonTerrain.regions", state.maps.width, state.maps.height, regionRGBA);
  state.textures.materials = uploadPass.uploadRGBA("polygonTerrain.materials", state.maps.width, state.maps.height, materialRGBA);
  state.textures.terrainForms = uploadPass.uploadRGBA("polygonTerrain.terrainForms", state.maps.width, state.maps.height, terrainFormRGBA);
  state.textures.temperature = uploadPass.uploadRGBA("polygonTerrain.temperature", state.maps.width, state.maps.height, temperatureRGBA);
  state.textures.precipitation = uploadPass.uploadRGBA("polygonTerrain.precipitation", state.maps.width, state.maps.height, precipitationRGBA);
  state.textures.wind = uploadPass.uploadRGBA("polygonTerrain.wind", state.maps.width, state.maps.height, windRGBA);
  state.textures.forests = uploadPass.uploadRGBA("polygonTerrain.forests", state.maps.width, state.maps.height, forestRGBA);
  state.textures.settlements = uploadPass.uploadRGBA("polygonTerrain.settlements", state.maps.width, state.maps.height, settlementRGBA);
  state.textures.continents = uploadPass.uploadRGBA("polygonTerrain.continents", state.maps.width, state.maps.height, continentRGBA);
  state.textures.tectonics = uploadPass.uploadRGBA("polygonTerrain.tectonics", state.maps.width, state.maps.height, tectonicRGBA);
  if (hasMountainDebugMap)
    state.textures.mountains = uploadPass.uploadRGBA("polygonTerrain.mountains", state.maps.width, state.maps.height, mountainRGBA);
  if (!state.textures.height.valid() || !state.textures.rivers.valid() || !state.textures.regions.valid() || !state.textures.materials.valid() || !state.textures.terrainForms.valid() ||
      !state.textures.temperature.valid() || !state.textures.precipitation.valid() || !state.textures.wind.valid() || !state.textures.forests.valid() || !state.textures.settlements.valid() ||
      !state.textures.continents.valid() || !state.textures.tectonics.valid() || (hasMountainDebugMap && !state.textures.mountains.valid()))
  {
    state.textures.reset(&rhi);
    state.status = "Could not create Vulkan preview textures";
    return false;
  }
  return true;
}

// Resamples the eroded raster onto the graph's dual mesh.
//
// The preview mesh carries the *graph* elevation, which is copied out of the
// macro pass before the fluvial solver runs and is never revisited. That made
// the generator's own 3D view show a planet it does not export: no drainage
// networks, no eroded ranges, however many times you regenerated.
//
// Only land moves. Ocean and lake vertices keep their graph value, which is the
// water surface the preview and the exported mesh already agree on.
void displaySurfaceFromErodedRaster(procedural::SphericalTerrainMaps &maps)
{
  const uint32_t width = maps.width;
  const uint32_t height = maps.height;
  if (width < 2u || height < 2u ||
      maps.heightmap.size() != static_cast<size_t>(width) * height ||
      maps.surfacePositions.size() != maps.surfaceElevations.size())
    return;
  constexpr double pi = 3.14159265358979323846;
  for (size_t vertex = 0u; vertex < maps.surfacePositions.size(); ++vertex)
  {
    const procedural::SpherePoint &direction = maps.surfacePositions[vertex];
    // Matches spherePoint() in SphericalTerrain.cpp: longitude zero sits at the
    // atlas centre and latitude runs from the north pole down.
    const double longitude = std::fmod(
        std::atan2(direction.z, direction.x) / (2.0 * pi) + 1.5, 1.0);
    const double latitude =
        0.5 - std::asin(std::clamp(direction.y, -1.0, 1.0)) / pi;
    const double sampleX = longitude * width - 0.5;
    const double sampleY = std::clamp(
        latitude * height - 0.5, 0.0, static_cast<double>(height - 1u));
    const int64_t baseX = static_cast<int64_t>(std::floor(sampleX));
    const uint32_t x0 = static_cast<uint32_t>(
        ((baseX % width) + width) % width);
    const uint32_t x1 = (x0 + 1u) % width;
    const uint32_t y0 = static_cast<uint32_t>(std::floor(sampleY));
    const uint32_t y1 = std::min(y0 + 1u, height - 1u);
    const double fractionX = sampleX - std::floor(sampleX);
    const double fractionY = sampleY - y0;
    const auto texel = [&](const uint32_t x, const uint32_t y)
    {
      return static_cast<double>(
          maps.heightmap[static_cast<size_t>(y) * width + x]);
    };
    // Ocean texels are zero and must not be blended into a coastal vertex; a
    // land vertex that samples across the shoreline would sink below the datum.
    const double a = texel(x0, y0);
    const double b = texel(x1, y0);
    const double c = texel(x0, y1);
    const double d = texel(x1, y1);
    if (a <= 0.0 || b <= 0.0 || c <= 0.0 || d <= 0.0)
      continue;
    const double top = a + (b - a) * fractionX;
    const double bottom = c + (d - c) * fractionX;
    const double sampled = top + (bottom - top) * fractionY;
    maps.surfaceElevations[vertex] = static_cast<uint16_t>(
        std::clamp(std::lround(sampled), 0l, 65535l));
  }
}

bool generateSphericalPreview(ApplicationState &state, std::string &error)
{
  procedural::SphericalTerrainSettings sphereSettings{};
  sphereSettings.seed = state.settings.seed;
  sphereSettings.width = state.raster.width;
  sphereSettings.height = std::max(4u, state.raster.height);
  sphereSettings.plateCount = state.planetPlateCount;
  sphereSettings.oceanicPlateFraction = state.planetOceanicPlateFraction;
  sphereSettings.jitter = state.planetJitter;
  sphereSettings.collisionThreshold = state.planetCollisionThreshold;
  sphereSettings.terrainWarp = state.planetTerrainWarp;
  sphereSettings.roughness = state.planetRoughness;
  sphereSettings.graphWorkerCount = state.planetGraphWorkers;
  sphereSettings.graphRegionCount = state.planetGraphRegionCount;
  sphereSettings.hydraulicIterations = state.planetHydraulicIterations;
  sphereSettings.thermalIterations = state.planetThermalIterations;
  sphereSettings.hydraulicErosion = state.planetHydraulicErosion;
  sphereSettings.thermalErosion = state.planetThermalErosion;
  sphereSettings.talusSlope = state.planetTalusSlope;
  sphereSettings.ridgeSharpeningIterations = state.planetRidgeIterations;
  sphereSettings.ridgeSharpening = state.planetRidgeSharpening;
  sphereSettings.soilCreepIterations = state.planetSoilIterations;
  sphereSettings.soilCreep = state.planetSoilCreep;
  sphereSettings.mountainUplift = state.planetMountainUplift;
  sphereSettings.mountainBeltWidth = state.planetMountainBeltWidth;
  sphereSettings.mountainRidgeSpacing = state.planetMountainRidgeSpacing;
  sphereSettings.mountainRidgeHeight = state.planetMountainRidgeHeight;
  sphereSettings.coastalPlainWidth = state.planetCoastalPlainWidth;
  sphereSettings.shorelineDetail = state.planetShorelineDetail;
  sphereSettings.interiorRelief = state.planetInteriorRelief;
  sphereSettings.lakeAbundance = state.planetLakeAbundance;
  sphereSettings.desertStrength = state.planetDesertStrength;
  sphereSettings.biomeVariation = state.planetBiomeVariation;
  sphereSettings.climateWetness = state.planetClimateWetness;
  sphereSettings.planetRadiusMetres = state.planetRadiusMetres;
  sphereSettings.reliefMetres = planetReliefMetres(state);
  sphereSettings.fluvialSolver = state.fluvialSolver;
  procedural::SphericalTerrainMaps sphereMaps{};
  procedural::SphericalTerrainStatistics sphereStatistics{};
  procedural::SphericalTerrainGenerator generator;
  if (!generator.generate(sphereSettings, sphereMaps, &sphereStatistics, &state.planetProfile))
  {
    error = "Spherical world generation failed";
    return false;
  }
  // Before anything reads the preview mesh, put the eroded surface on it.
  displaySurfaceFromErodedRaster(sphereMaps);
  state.maps = makePolygonPreviewMapsFromSphere(sphereMaps);
  state.mountainDebugMap = sphereMaps.mountains;
  state.sphereLines = sphereMaps.sphereLines;
  state.sphereSurfacePositions = sphereMaps.surfacePositions;
  state.sphereSurfaceElevations = sphereMaps.surfaceElevations;
  state.sphereSurfaceContinents = sphereMaps.surfaceContinents;
  state.sphereSurfaceTectonics = sphereMaps.surfaceTectonics;
  state.sphereSurfaceMountains = sphereMaps.surfaceMountains;
  state.sphereSurfaceTemperature = sphereMaps.surfaceTemperature;
  state.sphereSurfacePrecipitation = sphereMaps.surfacePrecipitation;
  state.sphereSurfacePlates = sphereMaps.surfacePlates;
  state.sphereSurfaceRivers = sphereMaps.surfaceRivers;
  state.sphereSurfaceMaterials = sphereMaps.surfaceMaterials;
  state.sphereSurfaceIndices = sphereMaps.surfaceIndices;
  state.sphereRegionNodes = sphereMaps.regionNodes;
  state.sphereRegionNeighbourOffsets = sphereMaps.regionNeighbourOffsets;
  state.sphereRegionNeighbourIndices = sphereMaps.regionNeighbourIndices;
  // Retain only the export rasters here. The graph and preview mesh already
  // live in dedicated state arrays and are reattached immediately before save.
  sphereMaps.regionPositions.clear();
  sphereMaps.regionElevations.clear();
  sphereMaps.regionRivers.clear();
  sphereMaps.regionNodes.clear();
  sphereMaps.regionNeighbourOffsets.clear();
  sphereMaps.regionNeighbourIndices.clear();
  sphereMaps.surfacePositions.clear();
  sphereMaps.surfaceElevations.clear();
  sphereMaps.surfaceContinents.clear();
  sphereMaps.surfaceTectonics.clear();
  sphereMaps.surfaceMountains.clear();
  sphereMaps.surfaceTemperature.clear();
  sphereMaps.surfacePrecipitation.clear();
  sphereMaps.surfacePlates.clear();
  sphereMaps.surfaceRivers.clear();
  sphereMaps.surfaceMaterials.clear();
  sphereMaps.surfaceIndices.clear();
  sphereMaps.sphereLines.clear();
  state.sphereExportMaps = std::move(sphereMaps);
  state.statistics = {};
  state.statistics.siteCount = sphereStatistics.plateCount;
  state.statistics.riverSourceCount = sphereStatistics.riverSourceCount;
  state.statistics.riverEdgeCount = sphereStatistics.riverPixelCount;
  state.statistics.oceanSiteCount = sphereStatistics.oceanPixelCount;
  state.statistics.lakeSiteCount = sphereStatistics.lakePixelCount;
  state.generated = true;
  state.generatedFromSphericalWorld = true;
  return true;
}

GenerationSnapshot generationSnapshot(ApplicationState &state)
{
  std::lock_guard<std::mutex> lock(state.generation.mutex);
  return GenerationSnapshot{state.generation.running, state.generation.finished, state.generation.progress, state.generation.elapsedSeconds, state.generation.phase};
}

bool generationRunning(ApplicationState &state)
{
  std::lock_guard<std::mutex> lock(state.generation.mutex);
  return state.generation.running;
}

void requestGenerationCancel(ApplicationState &state)
{
  std::lock_guard<std::mutex> lock(state.generation.mutex);
  if (state.generation.running)
  {
    state.generation.cancelRequested = true;
    state.status = "Cancelling terrain generation...";
  }
}

void startGeneration(ApplicationState &state)
{
  if (generationRunning(state))
    return;
  requestGenerationCancel(state);
  if (state.generation.worker.joinable())
    state.generation.worker.join();
  state.autoErosionStarted = false;
  const procedural::PolygonTerrainSettings settings = state.settings;
  const procedural::PolygonTerrainRasterSettings raster = state.raster;
  const bool useSphericalWorld = state.useSphericalWorld;
  const uint32_t planetPlateCount = state.planetPlateCount;
  const double planetOceanicPlateFraction = state.planetOceanicPlateFraction;
  const double planetJitter = state.planetJitter;
  const double planetCollisionThreshold = state.planetCollisionThreshold;
  const double planetTerrainWarp = state.planetTerrainWarp;
  const double planetRoughness = state.planetRoughness;
  const uint32_t planetGraphWorkers = state.planetGraphWorkers;
  const uint32_t planetGraphRegionCount = state.planetGraphRegionCount;
  const uint32_t planetHydraulicIterations = state.planetHydraulicIterations;
  const uint32_t planetThermalIterations = state.planetThermalIterations;
  const double planetHydraulicErosion = state.planetHydraulicErosion;
  const double planetThermalErosion = state.planetThermalErosion;
  const double planetTalusSlope = state.planetTalusSlope;
  const uint32_t planetRidgeIterations = state.planetRidgeIterations;
  const double planetRidgeSharpening = state.planetRidgeSharpening;
  const uint32_t planetSoilIterations = state.planetSoilIterations;
  const double planetSoilCreep = state.planetSoilCreep;
  const double planetMountainUplift = state.planetMountainUplift;
  const double planetMountainBeltWidth = state.planetMountainBeltWidth;
  const double planetMountainRidgeSpacing = state.planetMountainRidgeSpacing;
  const double planetMountainRidgeHeight = state.planetMountainRidgeHeight;
  const double planetCoastalPlainWidth = state.planetCoastalPlainWidth;
  const double planetShorelineDetail = state.planetShorelineDetail;
  const double planetInteriorRelief = state.planetInteriorRelief;
  const double planetLakeAbundance = state.planetLakeAbundance;
  const double planetDesertStrength = state.planetDesertStrength;
  const double planetBiomeVariation = state.planetBiomeVariation;
  const double planetClimateWetness = state.planetClimateWetness;
  {
    std::lock_guard<std::mutex> lock(state.generation.mutex);
    state.generation.running = true;
    state.generation.finished = false;
    state.generation.success = false;
    state.generation.cancelRequested = false;
    state.generation.progress = 0.0;
    state.generation.elapsedSeconds = 0.0;
    state.generation.phase = "Queued";
    state.generation.error.clear();
    state.generation.maps = {};
    state.generation.statistics = {};
    state.generation.mountainDebugMap.clear();
    state.generation.sphereLines.clear();
    state.generation.sphereSurfacePositions.clear();
    state.generation.sphereSurfaceElevations.clear();
    state.generation.sphereSurfaceContinents.clear();
    state.generation.sphereSurfaceTectonics.clear();
    state.generation.sphereSurfaceMountains.clear();
    state.generation.sphereSurfaceTemperature.clear();
    state.generation.sphereSurfacePrecipitation.clear();
    state.generation.sphereSurfacePlates.clear();
    state.generation.sphereSurfaceRivers.clear();
    state.generation.sphereSurfaceMaterials.clear();
    state.generation.sphereSurfaceIndices.clear();
    state.generation.sphereRegionNodes.clear();
    state.generation.sphereRegionNeighbourOffsets.clear();
    state.generation.sphereRegionNeighbourIndices.clear();
  }
  state.generated = false;
  state.generatedFromSphericalWorld = false;
  state.previewRefreshFailed = false;
  // A new planet needs a new package and a new atlas, and clearing the failure
  // latch gives the next generation a fresh attempt.
  state.adaptiveGlobeAvailable = false;
  state.adaptiveGlobeFailed = false;
  state.planetOverview = {};
  state.mountainDebugMap.clear();
  state.sphereLines.clear();
  state.sphereSurfacePositions.clear();
  state.sphereSurfaceElevations.clear();
  state.sphereSurfaceContinents.clear();
  state.sphereSurfaceTectonics.clear();
  state.sphereSurfaceMountains.clear();
  state.sphereSurfaceTemperature.clear();
  state.sphereSurfacePrecipitation.clear();
  state.sphereSurfacePlates.clear();
  state.sphereSurfaceRivers.clear();
  state.sphereSurfaceMaterials.clear();
  state.sphereSurfaceIndices.clear();
  state.sphereRegionNodes.clear();
  state.sphereRegionNeighbourOffsets.clear();
  state.sphereRegionNeighbourIndices.clear();
  state.sphereExportMaps = {};
  state.packageStatistics = {};
  state.planetPackageStatistics = {};
  state.planetCubeStatistics = {};
  state.status = useSphericalWorld ? "Generating spherical world preview in background..." : "Generating terrain preview in background...";
  state.generation.worker = std::thread(
      [&job = state.generation, settings, raster, useSphericalWorld, planetPlateCount, planetOceanicPlateFraction, planetJitter, planetCollisionThreshold, planetTerrainWarp, planetRoughness,
       planetGraphRegionCount, planetHydraulicIterations, planetThermalIterations, planetHydraulicErosion, planetThermalErosion, planetTalusSlope,
       planetRidgeIterations, planetRidgeSharpening, planetSoilIterations, planetSoilCreep, planetGraphWorkers,
       planetMountainUplift, planetMountainBeltWidth, planetMountainRidgeSpacing, planetMountainRidgeHeight,
       planetCoastalPlainWidth, planetShorelineDetail, planetInteriorRelief, planetLakeAbundance, planetDesertStrength, planetBiomeVariation, planetClimateWetness]
      {
        procedural::PolygonTerrainMaps maps{};
        procedural::PolygonTerrainStatistics statistics{};
        std::string error;
        const auto begin = std::chrono::steady_clock::now();
        auto setProgress = [&job, begin](const char *phase, const double fraction)
        {
          const auto now = std::chrono::steady_clock::now();
          std::lock_guard<std::mutex> lock(job.mutex);
          job.phase = phase != nullptr ? phase : "";
          job.progress = fraction;
          job.elapsedSeconds = std::chrono::duration<double>(now - begin).count();
          return !job.cancelRequested;
        };
        procedural::PolygonTerrainProgress progress{};
        progress.callback = setProgress;
        bool success = false;
        if (useSphericalWorld)
        {
          setProgress("Generating planet plates, continents, climate, mountains, and rivers", 0.10);
          procedural::SphericalTerrainSettings sphereSettings{};
          sphereSettings.seed = settings.seed;
          sphereSettings.width = raster.width;
          sphereSettings.height = std::max(4u, raster.height);
          sphereSettings.rasterizeMaps = false;
          sphereSettings.plateCount = planetPlateCount;
          sphereSettings.oceanicPlateFraction = planetOceanicPlateFraction;
          sphereSettings.jitter = planetJitter;
          sphereSettings.collisionThreshold = planetCollisionThreshold;
          sphereSettings.terrainWarp = planetTerrainWarp;
          sphereSettings.roughness = planetRoughness;
          sphereSettings.graphWorkerCount = planetGraphWorkers;
          sphereSettings.graphRegionCount = planetGraphRegionCount;
          sphereSettings.hydraulicIterations = planetHydraulicIterations;
          sphereSettings.thermalIterations = planetThermalIterations;
          sphereSettings.hydraulicErosion = planetHydraulicErosion;
          sphereSettings.thermalErosion = planetThermalErosion;
          sphereSettings.talusSlope = planetTalusSlope;
          sphereSettings.ridgeSharpeningIterations = planetRidgeIterations;
          sphereSettings.ridgeSharpening = planetRidgeSharpening;
          sphereSettings.soilCreepIterations = planetSoilIterations;
          sphereSettings.soilCreep = planetSoilCreep;
          sphereSettings.mountainUplift = planetMountainUplift;
          sphereSettings.mountainBeltWidth = planetMountainBeltWidth;
          sphereSettings.mountainRidgeSpacing = planetMountainRidgeSpacing;
          sphereSettings.mountainRidgeHeight = planetMountainRidgeHeight;
          sphereSettings.coastalPlainWidth = planetCoastalPlainWidth;
          sphereSettings.shorelineDetail = planetShorelineDetail;
          sphereSettings.interiorRelief = planetInteriorRelief;
          sphereSettings.lakeAbundance = planetLakeAbundance;
          sphereSettings.desertStrength = planetDesertStrength;
          sphereSettings.biomeVariation = planetBiomeVariation;
          sphereSettings.climateWetness = planetClimateWetness;
          procedural::SphericalTerrainMaps sphereMaps{};
          procedural::SphericalTerrainStatistics sphereStatistics{};
          procedural::SphericalTerrainGenerator generator;
          success = generator.generate(sphereSettings, sphereMaps, &sphereStatistics);
          if (success && setProgress("Preparing graph-native spherical preview", 0.82))
          {
            maps.width = sphereMaps.width;
            maps.height = sphereMaps.height;
            job.sphereLines = sphereMaps.sphereLines;
            job.sphereSurfacePositions = sphereMaps.surfacePositions;
            job.sphereSurfaceElevations = sphereMaps.surfaceElevations;
            job.sphereSurfaceContinents = sphereMaps.surfaceContinents;
            job.sphereSurfaceTectonics = sphereMaps.surfaceTectonics;
            job.sphereSurfaceMountains = sphereMaps.surfaceMountains;
            job.sphereSurfaceTemperature = sphereMaps.surfaceTemperature;
            job.sphereSurfacePrecipitation = sphereMaps.surfacePrecipitation;
            job.sphereSurfacePlates = sphereMaps.surfacePlates;
            job.sphereSurfaceRivers = sphereMaps.surfaceRivers;
            job.sphereSurfaceMaterials = sphereMaps.surfaceMaterials;
            job.sphereSurfaceIndices = sphereMaps.surfaceIndices;
            job.sphereRegionNodes = std::move(sphereMaps.regionNodes);
            job.sphereRegionNeighbourOffsets = std::move(sphereMaps.regionNeighbourOffsets);
            job.sphereRegionNeighbourIndices = std::move(sphereMaps.regionNeighbourIndices);
            statistics.siteCount = sphereStatistics.plateCount;
            statistics.riverSourceCount = sphereStatistics.riverSourceCount;
            statistics.riverEdgeCount = sphereStatistics.riverPixelCount;
            statistics.oceanSiteCount = sphereStatistics.oceanPixelCount;
            statistics.lakeSiteCount = sphereStatistics.lakePixelCount;
          }
          else if (!success)
          {
            error = "Spherical world generation failed";
          }
          success = success && !job.cancelRequested;
        }
        else
        {
          procedural::PolygonTerrainGenerator generator;
          success = generator.generate(settings, raster, maps, &statistics, &error, &progress);
        }
        const auto end = std::chrono::steady_clock::now();
        {
          std::lock_guard<std::mutex> lock(job.mutex);
          job.success = success;
          job.error = error;
          job.elapsedSeconds = std::chrono::duration<double>(end - begin).count();
          if (success)
          {
            job.maps = std::move(maps);
            job.statistics = statistics;
            job.phase = "Complete";
            job.progress = 1.0;
          }
          else if (job.phase.empty())
          {
            job.phase = "Failed";
          }
          job.running = false;
          job.finished = true;
        }
      });
}

void pollGeneration(
    ApplicationState &state,
    TerrainPreviewRHI &rhi,
    TerrainPreviewTextureUploadPass &uploadPass,
    TerrainPreviewRasterPass &rasterPass)
{
  {
    std::lock_guard<std::mutex> lock(state.generation.mutex);
    if (!state.generation.finished)
      return;
  }
  if (state.generation.worker.joinable())
    state.generation.worker.join();
  bool success = false;
  std::string error;
  double elapsedSeconds = 0.0;
  {
    std::lock_guard<std::mutex> lock(state.generation.mutex);
    success = state.generation.success;
    error = state.generation.error;
    elapsedSeconds = state.generation.elapsedSeconds;
    if (success)
    {
      state.maps = std::move(state.generation.maps);
      state.statistics = state.generation.statistics;
      state.mountainDebugMap = std::move(state.generation.mountainDebugMap);
      state.sphereLines = std::move(state.generation.sphereLines);
      state.sphereSurfacePositions = std::move(state.generation.sphereSurfacePositions);
      state.sphereSurfaceElevations = std::move(state.generation.sphereSurfaceElevations);
      state.sphereSurfaceContinents = std::move(state.generation.sphereSurfaceContinents);
      state.sphereSurfaceTectonics = std::move(state.generation.sphereSurfaceTectonics);
      state.sphereSurfaceMountains = std::move(state.generation.sphereSurfaceMountains);
      state.sphereSurfaceTemperature = std::move(state.generation.sphereSurfaceTemperature);
      state.sphereSurfacePrecipitation = std::move(state.generation.sphereSurfacePrecipitation);
      state.sphereSurfacePlates = std::move(state.generation.sphereSurfacePlates);
      state.sphereSurfaceRivers = std::move(state.generation.sphereSurfaceRivers);
      state.sphereSurfaceMaterials = std::move(state.generation.sphereSurfaceMaterials);
      state.sphereSurfaceIndices = std::move(state.generation.sphereSurfaceIndices);
      state.sphereRegionNodes = std::move(state.generation.sphereRegionNodes);
      state.sphereRegionNeighbourOffsets = std::move(state.generation.sphereRegionNeighbourOffsets);
      state.sphereRegionNeighbourIndices = std::move(state.generation.sphereRegionNeighbourIndices);
    }
    state.generation.finished = false;
  }
  if (!success)
  {
    state.generated = false;
    state.status = "Generation failed: " + error;
    return;
  }
  // The planar path treats a preview failure as a failed generation. The
  // spherical path must not: the globe is drawn from the graph mesh and does not
  // depend on these atlases at all, so letting an atlas problem clear
  // state.generated took the whole planet off screen.
  if (!state.useSphericalWorld && !updatePreviewTextures(state, rhi, uploadPass, rasterPass))
  {
    state.generated = false;
    return;
  }
  if (state.useSphericalWorld)
  {
    state.textures.reset(&rhi);
    ++state.previewRevision;
    state.sphereDirectRevision = 0u;
    state.sphereLineRevision = 0u;
  }
  state.generated = true;
  state.generatedFromSphericalWorld = state.useSphericalWorld;
  state.status = state.useSphericalWorld ?
      "Generated graph-native terrain in " + std::to_string(elapsedSeconds).substr(0, 5) + "s. Atlases are deferred until export." :
      "Generated terrain in " + std::to_string(elapsedSeconds).substr(0, 5) + "s; GPU preview atlases are ready. Save maps to write files.";
  std::cout << state.status << "\n";
}

void printUsage()
{
  std::cout << "Usage: PolygonTerrainSample [--config terrain-config.json] [--output directory] [--seed value] [--extent metres] "
               "[--site-radius metres] [--resolution pixels] [--tile-size pixels] [--threads count] [--rivers override] "
               "[--erosion-iterations count] [--erosion-mode 0|1|2] [--erosion-k value] [--erosion-uplift value] "
               "[--erosion-hillslope value] [--erosion-time value] [--erosion-face-side cells] [--uniform-erodibility] [--sediment-transport] [--closed-system] [--no-fluvial-erosion] "
               "[--sphere] [--polygon] [--planet-radius metres] [--planet-relief metres] [--regions count] [--plates count] [--oceanic-fraction value] [--jitter value] [--collision-threshold value] [--erode] [--headless]\n";
}

const GpuPreviewTexture &selectedSphereAtlas(const ApplicationState &state)
{
  switch (static_cast<SpherePreviewLayer>(state.spherePreviewLayer))
  {
  case SpherePreviewLayer::Height:
    return state.textures.height;
  case SpherePreviewLayer::Continents:
    return state.textures.continents;
  case SpherePreviewLayer::Tectonics:
    return state.textures.tectonics;
  case SpherePreviewLayer::Mountains:
    return state.textures.mountains.valid() ? state.textures.mountains : state.textures.terrainForms;
  case SpherePreviewLayer::Temperature:
    return state.textures.temperature;
  case SpherePreviewLayer::Rainfall:
    return state.textures.precipitation;
  case SpherePreviewLayer::Plates:
    return state.textures.regions;
  case SpherePreviewLayer::Rivers:
    return state.textures.rivers;
  case SpherePreviewLayer::Terrain:
  case SpherePreviewLayer::Materials:
  default:
    return state.textures.materials;
  }
}

TerrainPreviewSphereSettings sphereRenderSettings(const ApplicationState &state)
{
  TerrainPreviewSphereSettings settings{};
  settings.yawDegrees = state.sphereYawDegrees;
  settings.pitchDegrees = state.spherePitchDegrees;
  settings.zoom = state.sphereZoom;
  settings.lighting = state.sphereLighting ? 1.0f : 0.0f;
  settings.showPlateBoundaries = state.sphereShowPlateBoundaries ? 1.0f : 0.0f;
  const SpherePreviewLayer layer = static_cast<SpherePreviewLayer>(state.spherePreviewLayer);
  settings.showOcean = layer == SpherePreviewLayer::Terrain || layer == SpherePreviewLayer::Materials ? 1.0f : 0.0f;
  return settings;
}

void drawTexture(const char *label, const GpuPreviewTexture &texture)
{
  if (!texture.valid())
  {
    ImGui::TextDisabled("No %s preview", label);
    return;
  }
  ImGui::TextUnformatted(label);
  const ImVec2 available = ImGui::GetContentRegionAvail();
  const float size = std::max(64.0f, std::min(available.x, available.y - ImGui::GetTextLineHeightWithSpacing()));
  ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(texture.descriptor)), ImVec2(size, size));
}

void describeField(const char *description)
{
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    ImGui::SetTooltip("%s", description);
}

void drawControls(ApplicationState &state)
{
  const GenerationSnapshot generation = generationSnapshot(state);
  const bool busy = generation.running || state.erosionRunning;
  ImGui::SetNextWindowSize(ImVec2(580.0f, 900.0f), ImGuiCond_FirstUseEver);
  ImGui::Begin("Polygon Terrain Generator");
  ImGui::TextUnformatted("Offline map baker preview");
  ImGui::Separator();
  ImGui::BeginDisabled(busy);
  if (ImGui::Checkbox("Use spherical world source", &state.useSphericalWorld))
    state.raster.height = state.useSphericalWorld ? std::max(4u, state.raster.width / 2u) : state.raster.width;
  describeField("Switches between the planet-scale spherical graph generator and the older planar polygon-map generator.");
  ImGui::InputScalar("Seed", ImGuiDataType_U64, &state.settings.seed);
  describeField("Deterministic world seed. Identical settings and seed produce identical graph fields and exported maps.");
  if (state.useSphericalWorld)
  {
    ImGui::TextDisabled("The simulation-region count controls visible graph detail; atlas dimensions affect exports only.");
    if (ImGui::Button("Restore Orogen defaults"))
    {
      state.planetPlateCount = 80u;
      state.planetJitter = 0.75;
      state.planetTerrainWarp = 0.75;
      state.planetRoughness = 0.40;
      state.planetGraphRegionCount = 204000u;
      state.planetHydraulicIterations = 10u;
      state.planetThermalIterations = 1u;
      state.planetHydraulicErosion = 0.00030;
      state.planetThermalErosion = 0.015;
      state.planetTalusSlope = 1.16;
      state.planetRidgeIterations = 3u;
      state.planetRidgeSharpening = 0.04;
      state.planetSoilIterations = 3u;
      state.planetSoilCreep = 0.1125;
      state.planetGraphWorkers = 0u;
      state.planetMountainUplift = 1.0;
      state.planetMountainBeltWidth = 1.0;
      state.planetMountainRidgeSpacing = 1.0;
      state.planetMountainRidgeHeight = 1.0;
      state.planetCoastalPlainWidth = 1.0;
      state.planetShorelineDetail = 1.0;
      state.planetInteriorRelief = 1.0;
      state.planetLakeAbundance = 1.0;
      state.planetDesertStrength = 1.0;
      state.planetBiomeVariation = 1.0;
      state.planetClimateWetness = 1.0;
    }
    ImGui::SeparatorText("World topology and output");
    ImGui::TextDisabled("Topology builds the planet graph. Atlas controls affect saved maps, not globe mesh density.");
    ImGui::InputDouble("Planet radius (m)", &state.planetRadiusMetres, 0.0, 0.0, "%.0f");
    describeField("Physical radius written to the planet package. Runtime cube-quadtree refinement uses this with the 1 km render-patch target.");
    ImGui::InputDouble("Total relief (m)", &state.planetReliefMetres, 0.0, 0.0, "%.0f");
    describeField("Encoded elevation range from ocean datum to highest summit. Zero restores the old Earth-like radius * 0.0015 ratio, which is nearly flat on a small world.");
    int regionSamples = static_cast<int>(state.raster.width * state.raster.height);
    if (ImGui::InputInt("Atlas texels (export only)", &regionSamples))
    {
      regionSamples = std::clamp(regionSamples, 8192, 8388608);
      const uint32_t atlasWidth = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(std::sqrt(static_cast<double>(regionSamples) * 2.0))), 128, 4096));
      state.raster.width = atlasWidth;
      state.raster.height = std::max(4u, state.raster.width / 2u);
    }
    describeField("Approximate number of pixels in the derived 2:1 export atlas. This does not change spherical Voronoi cell count.");
    int planetWidth = static_cast<int>(state.raster.width);
    if (ImGui::SliderInt("Atlas width", &planetWidth, 128, 4096))
    {
      state.raster.width = static_cast<uint32_t>(planetWidth);
      state.raster.height = std::max(4u, state.raster.width / 2u);
    }
    describeField("Width of exported textures; height is kept at width / 2 for equirectangular maps.");
    int plates = static_cast<int>(state.planetPlateCount);
    if (ImGui::SliderInt("Number of plates (0 auto)", &plates, 0, 255))
      state.planetPlateCount = static_cast<uint32_t>(std::clamp(plates, 0, 255));
    describeField("Number of independently moving tectonic plates. More plates create more collision boundaries and shorter mountain systems. Zero selects 80.");
    const double minOceanicFraction = 0.05;
    const double maxOceanicFraction = 0.95;
    ImGui::SliderScalar("Percentage oceanic plates", ImGuiDataType_Double, &state.planetOceanicPlateFraction, &minOceanicFraction, &maxOceanicFraction, "%.2f");
    describeField("Fraction of plates initialized as dense oceanic crust. Higher values generally reduce continental land coverage.");
    const double minJitter = 0.0;
    const double maxJitter = 1.0;
    ImGui::SliderScalar("Jitter", ImGuiDataType_Double, &state.planetJitter, &minJitter, &maxJitter, "%.2f");
    describeField("Irregularity of spherical Voronoi sites and plate growth. Zero is uniform; one is maximally scattered.");
    ImGui::SliderScalar("Terrain warp", ImGuiDataType_Double, &state.planetTerrainWarp, &minJitter, &maxJitter, "%.2f");
    describeField("Large-scale tangent-space deformation used to bend coastlines, trenches, and mountain belts.");
    ImGui::SliderScalar("Terrain roughness", ImGuiDataType_Double, &state.planetRoughness, &minJitter, &maxJitter, "%.2f");
    describeField("Strength of smaller-scale crust detail. This adds texture but does not choose where tectonic mountain belts form.");
    int graphRegions = static_cast<int>(state.planetGraphRegionCount);
    if (ImGui::SliderInt("Simulation regions", &graphRegions, 5000, 256000, "%d", ImGuiSliderFlags_Logarithmic))
      state.planetGraphRegionCount = static_cast<uint32_t>(graphRegions);
    describeField("Authoritative spherical graph resolution. Higher values add real cells, drainage detail, and finer ridge geometry, but cost generation time and memory.");
    int graphWorkers = static_cast<int>(state.planetGraphWorkers);
    if (ImGui::SliderInt("Graph workers (0 auto)", &graphWorkers, 0, 64))
      state.planetGraphWorkers = static_cast<uint32_t>(graphWorkers);
    describeField("Worker threads for independent graph diffusion, terrain evaluation, and climate transport. Auto uses up to twice the detected hardware threads.");

    ImGui::SeparatorText("Mountain ranges");
    ImGui::TextDisabled("These controls modify generated elevations before erosion; regenerate the preview after changing them.");
    const double minimumMountainScale = 0.0;
    const double maximumMountainScale = 3.0;
    ImGui::SliderScalar("Tectonic uplift height", ImGuiDataType_Double, &state.planetMountainUplift, &minimumMountainScale, &maximumMountainScale, "%.2f");
    describeField("Scales broad elevation created by convergent plate motion. Increase for taller ranges without changing their width.");
    const double minimumBeltWidth = 0.25;
    ImGui::SliderScalar("Mountain belt width", ImGuiDataType_Double, &state.planetMountainBeltWidth, &minimumBeltWidth, &maximumMountainScale, "%.2f");
    describeField("Physical reach of convergence stress away from a plate boundary. Larger values create wider orogenic provinces.");
    const double minimumRidgeSpacing = 0.35;
    ImGui::SliderScalar("Ridge spacing", ImGuiDataType_Double, &state.planetMountainRidgeSpacing, &minimumRidgeSpacing, &maximumMountainScale, "%.2f");
    describeField("Distance between parallel folded ridges inside a mountain belt. Smaller values create more tightly packed ranges.");
    ImGui::SliderScalar("Ridge relief", ImGuiDataType_Double, &state.planetMountainRidgeHeight, &minimumMountainScale, &maximumMountainScale, "%.2f");
    describeField("Height of the warped ridge/valley phasor texture layered over broad tectonic uplift.");

    ImGui::SeparatorText("Continental relief");
    ImGui::TextDisabled("Coastal margins remain low while seamless province noise varies the interior between plains, hills, and old uplands.");
    const double minimumCoastalPlainWidth = 0.25;
    ImGui::SliderScalar("Coastal plain width", ImGuiDataType_Double, &state.planetCoastalPlainWidth, &minimumCoastalPlainWidth, &maximumMountainScale, "%.2f");
    describeField("Width of the geodesic lowland transition measured inward from the coastline. Larger values move steep mountains farther from beaches.");
    ImGui::SliderScalar("Shoreline detail", ImGuiDataType_Double, &state.planetShorelineDetail, &minimumMountainScale, &maximumMountainScale, "%.2f");
    describeField("Strength of seamless bays, capes, archipelagos, and continental-shelf irregularity. Zero follows the smoothed crust boundary; high values produce more intricate coasts.");
    ImGui::SliderScalar("Interior relief", ImGuiDataType_Double, &state.planetInteriorRelief, &minimumMountainScale, &maximumMountainScale, "%.2f");
    describeField("Strength of multi-scale rolling hills and old uplands inside continents. Tectonic ranges remain controlled by the mountain settings above.");
    ImGui::SliderScalar("Lake abundance", ImGuiDataType_Double, &state.planetLakeAbundance, &minimumMountainScale, &maximumMountainScale, "%.2f");
    describeField("Number and minimum catchment size of retained inland hydrological basins. Lakes use priority-flood spill levels and accumulated runoff, not random blue patches.");

    ImGui::SeparatorText("Graph and raster erosion");
    ImGui::TextDisabled("Erosion shapes macro catchments, then adds high-resolution channels and slope transport to the export maps.");
    int graphHydraulicIterations = static_cast<int>(state.planetHydraulicIterations);
    if (ImGui::SliderInt("Hydraulic iterations", &graphHydraulicIterations, 0, 64))
      state.planetHydraulicIterations = static_cast<uint32_t>(graphHydraulicIterations);
    describeField("Number of stream-power passes applied to the graph and raster maps. More iterations cut deeper, more connected valleys.");
    int graphThermalIterations = static_cast<int>(state.planetThermalIterations);
    if (ImGui::SliderInt("Thermal iterations", &graphThermalIterations, 0, 16))
      state.planetThermalIterations = static_cast<uint32_t>(graphThermalIterations);
    describeField("Number of slope-driven material transport passes applied at both scales to form talus and soften unstable peaks.");
    const double minimumHydraulicErosion = 0.0;
    const double maximumHydraulicErosion = 0.002;
    ImGui::SliderScalar("Stream-power erosion", ImGuiDataType_Double, &state.planetHydraulicErosion, &minimumHydraulicErosion, &maximumHydraulicErosion, "%.6f");
    describeField("Erosion coefficient applied to discharge and downhill slope. Small changes have a strong effect across multiple iterations.");
    const double maximumThermalErosion = 0.5;
    ImGui::SliderScalar("Thermal transport", ImGuiDataType_Double, &state.planetThermalErosion, &minJitter, &maximumThermalErosion, "%.3f");
    describeField("Fraction of excess slope material moved toward lower neighbours during each thermal pass.");
    const double minimumTalus = 0.05;
    const double maximumTalus = 2.0;
    ImGui::SliderScalar("Talus slope", ImGuiDataType_Double, &state.planetTalusSlope, &minimumTalus, &maximumTalus, "%.2f");
    describeField("Maximum stable elevation gradient. Lower values relax more slopes and produce broader, softer mountains.");
    int ridgeIterations = static_cast<int>(state.planetRidgeIterations);
    if (ImGui::SliderInt("Ridge sharpening iterations", &ridgeIterations, 0, 12))
      state.planetRidgeIterations = static_cast<uint32_t>(ridgeIterations);
    describeField("Post-erosion passes that raise local maxima and deepen valleys before final soil creep.");
    const double maximumRidgeSharpening = 0.12;
    ImGui::SliderScalar("Ridge sharpening", ImGuiDataType_Double, &state.planetRidgeSharpening, &minJitter, &maximumRidgeSharpening, "%.3f");
    describeField("Per-pass strength of ridge sharpening. Excessive values create needle-like peaks.");
    int soilIterations = static_cast<int>(state.planetSoilIterations);
    if (ImGui::SliderInt("Soil creep iterations", &soilIterations, 0, 12))
      state.planetSoilIterations = static_cast<uint32_t>(soilIterations);
    describeField("Always-stable diffusion passes over interior land that round hillslopes while locking coastlines.");
    const double maximumSoilCreep = 0.30;
    ImGui::SliderScalar("Soil creep", ImGuiDataType_Double, &state.planetSoilCreep, &minJitter, &maximumSoilCreep, "%.4f");
    describeField("Per-pass fraction moved toward neighbour-average elevation during soil creep.");

    ImGui::SeparatorText("Climate and tectonic activation");
    const double minimumWetness = 0.25;
    const double maximumWetness = 2.0;
    ImGui::SliderScalar("Global wetness", ImGuiDataType_Double, &state.planetClimateWetness, &minimumWetness, &maximumWetness, "%.2f");
    describeField("Scales graph precipitation after wind and rain-shadow transport. Higher values create more green biomes and denser rivers.");
    ImGui::SliderScalar("Desert strength", ImGuiDataType_Double, &state.planetDesertStrength, &minimumMountainScale, &maximumMountainScale, "%.2f");
    describeField("Strength of subtropical, continental-interior, and leeward drying. Regional noise breaks dry belts into deserts, steppes, and greener refuges.");
    ImGui::SliderScalar("Biome patch variation", ImGuiDataType_Double, &state.planetBiomeVariation, &minimumMountainScale, &maximumMountainScale, "%.2f");
    describeField("Strength of seamless regional moisture and temperature variation used for biome boundaries. Higher values create more local forests, shrublands, taiga, marshes, and deserts without changing drainage.");
    const double minCollision = 0.05;
    const double maxCollision = 0.90;
    ImGui::SliderScalar("Tectonic convergence threshold", ImGuiDataType_Double, &state.planetCollisionThreshold, &minCollision, &maxCollision, "%.2f");
    describeField("Relative plate-closing speed required to activate uplift or rifting. Lower values create more active boundaries and mountain chains.");
    ImGui::SeparatorText("Sphere renderer");
    ImGui::TextDisabled("Generated Voronoi surface mesh + sea-level water shell; atlases are shown only in the map tabs.");
    static constexpr const char *sphereLayers = "Terrain\0Materials\0Height\0Continents\0Tectonics\0Mountains\0Temperature\0Rainfall\0Plates\0Rivers\0";
    ImGui::Combo("Graph display field", &state.spherePreviewLayer, sphereLayers);
    describeField("Selects the graph-node attribute used to colour the direct sphere mesh. No atlas sampling is involved.");
    ImGui::SliderFloat("Sphere rotation", &state.sphereYawDegrees, -180.0f, 180.0f, "%.1f");
    describeField("Longitude-like camera rotation around the preview globe. You can also drag the globe directly.");
    ImGui::SliderFloat("Sphere tilt", &state.spherePitchDegrees, -80.0f, 80.0f, "%.1f");
    describeField("North/south viewing angle, limited before the camera flips over either pole.");
    ImGui::SliderFloat("Zoom", &state.sphereZoom, 1.0f, 64.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
    describeField("Camera distance to the globe. The mouse wheel does the same thing while the cursor is over the viewport.");
    ImGui::Checkbox("Lighting", &state.sphereLighting);
    describeField("Enables relief lighting from the generated mesh normals; disable it to inspect unlit biome colours.");
    ImGui::SameLine();
    ImGui::Checkbox("Plate boundaries", &state.sphereShowPlateBoundaries);
    describeField("Draws tectonic boundary overlays. These are diagnostics and are never written into terrain colours.");

    ImGui::SeparatorText("Adaptive globe");
    ImGui::BeginDisabled(!state.adaptiveGlobeSupported);
    ImGui::Checkbox("Subdivide on zoom", &state.adaptiveGlobeEnabled);
    ImGui::EndDisabled();
    describeField(
        "Replaces the fixed dual mesh with the adaptive CBT, which subdivides "
        "as you zoom instead of going flat past the mesh density. Needs a "
        "surface atlas, which is baked to a working package after generation.");
    if (!state.adaptiveGlobeSupported)
      ImGui::TextDisabled("Unsupported on this device.");
    else if (!state.adaptiveGlobeStatus.empty())
      ImGui::TextWrapped("%s", state.adaptiveGlobeStatus.c_str());
    ImGui::BeginDisabled(!state.adaptiveGlobeSupported || !state.adaptiveGlobeEnabled);
    static constexpr const char *globeDiagnostics = "Shaded\0Wireframe\0Subdivision depth\0";
    ImGui::Combo("Globe diagnostic", &state.adaptiveGlobe.diagnostic, globeDiagnostics);
    describeField("Wireframe and subdivision depth show what the CBT is actually doing as the camera moves.");
    ImGui::SliderFloat("CBT target area", &state.adaptiveGlobe.projectedAreaTarget, 4.0f, 256.0f, "%.0f px");
    describeField("Projected area a triangle aims for. Smaller subdivides harder and costs more bisectors.");
    ImGui::SliderFloat("Relief exaggeration", &state.adaptiveGlobe.reliefExaggeration, 1.0f, 128.0f, "%.0fx");
    describeField("Visual only. Package elevations stay physical; this is capped against the planet radius.");
    ImGui::Checkbox("Freeze subdivision", &state.adaptiveGlobe.freezeUpdates);
    describeField("Stops the CBT updating so you can inspect the current tessellation while moving the camera.");
    int globeFace = static_cast<int>(state.adaptiveGlobeFaceResolution);
    static constexpr const char *globeFaceLabels = "1024\0" "2048\0" "4096\0";
    int globeFaceChoice = globeFace >= 4096 ? 2 : (globeFace >= 2048 ? 1 : 0);
    if (ImGui::Combo("Surface atlas", &globeFaceChoice, globeFaceLabels))
    {
      state.adaptiveGlobeFaceResolution = 1024u << globeFaceChoice;
      // Re-bake on the next frame. Clearing the failure latch too, since a
      // larger atlas is a different attempt, not a retry of the failed one.
      state.adaptiveGlobeAvailable = false;
      state.adaptiveGlobeFailed = false;
    }
    describeField("Per-cube-face resolution of the displaced surface atlas: 72 MB at 1024, 288 MB at 2048, 1.15 GB at 4096. Changing it re-bakes.");
    if (state.adaptiveGlobeActiveBisectors != 0u)
    {
      ImGui::Text("Bisectors %u / %u", state.adaptiveGlobeActiveBisectors, state.adaptiveGlobeCapacity);
      describeField("Live CBT triangle count. It climbs as you zoom in and falls as you pull out; if it never moves, subdivision is not running.");
    }
    ImGui::EndDisabled();

    // Detail is limited by whichever of these is coarsest, and the usual
    // mistake is to raise the surface atlas when the export atlas or the graph
    // is what is actually binding. Showing all three in metres makes that
    // visible instead of guessable.
    const double circumference = 2.0 * 3.14159265358979323846 * state.planetRadiusMetres;
    const double area = 4.0 * 3.14159265358979323846 * state.planetRadiusMetres * state.planetRadiusMetres;
    const double exportTexel = circumference / std::max(1u, state.raster.width);
    const double graphSpacing = std::sqrt(area / std::max(1u, state.planetGraphRegionCount));
    const double atlasTexel = (circumference / 4.0) / std::max(1u, state.adaptiveGlobeFaceResolution);
    ImGui::SeparatorText("Effective detail");
    ImGui::Text("Voronoi graph   %7.0f m", graphSpacing);
    ImGui::Text("Export atlas    %7.0f m", exportTexel);
    ImGui::Text("Surface atlas   %7.0f m", atlasTexel);
    if (exportTexel > graphSpacing)
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
          "Export atlas is coarser than the graph: detail is\n"
          "being discarded. Raise atlas width or shrink the planet.");
    describeField("A smaller planet radius improves all three at once, which is usually cheaper than raising resolutions.");
  }
  else
  {
    ImGui::InputDouble("Width (m)", &state.settings.widthMetres, 0.0, 0.0, "%.1f");
    describeField("Physical east-west size of the planar source terrain in metres.");
    ImGui::InputDouble("Height (m)", &state.settings.heightMetres, 0.0, 0.0, "%.1f");
    describeField("Physical north-south size of the planar source terrain in metres.");
    ImGui::InputDouble("Site radius (m)", &state.settings.siteRadiusMetres, 0.0, 0.0, "%.2f");
    describeField("Minimum Poisson-disc spacing between planar Voronoi sites. Smaller values create more regions and cost more time.");
    ImGui::TextDisabled("Rivers are automatic by default; set a non-zero override only for debugging.");
    int rivers = static_cast<int>(state.settings.riverSourceCount);
    if (ImGui::InputInt("River source override (0 auto)", &rivers))
      state.settings.riverSourceCount = static_cast<uint32_t>(std::clamp(rivers, 0, 4096));
    describeField("Optional fixed source count for planar debugging. Zero derives the river count from rainfall and drainage area.");
    int resolution = static_cast<int>(state.raster.width);
    if (ImGui::SliderInt("Preview resolution", &resolution, 128, 2048))
    {
      state.raster.width = static_cast<uint32_t>(resolution);
      state.raster.height = static_cast<uint32_t>(resolution);
    }
    describeField("Square preview/export raster resolution for the planar generator.");
  }
  int tileSize = static_cast<int>(state.package.tileSizePixels);
  if (ImGui::InputInt("Package tile size (px)", &tileSize))
    state.package.tileSizePixels = static_cast<uint32_t>(std::max(1, tileSize));
  describeField("Pixel dimensions of independently LZ4-compressed streaming tiles in the saved terrain package.");
  int workers = static_cast<int>(state.raster.workerCount);
  if (ImGui::InputInt("Raster workers (0 auto)", &workers))
    state.raster.workerCount = static_cast<uint32_t>(std::clamp(workers, 0, 1024));
  describeField("CPU worker count for planar raster baking and fallback conversion. Spherical preview work uses Graph workers; its atlas bake is deferred until export.");
  ImGui::EndDisabled();
  ImGui::TextDisabled("Generation is explicit; adjusting a setting does not write files.");
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("Generate preview"))
    startGeneration(state);
  ImGui::EndDisabled();
  if (generation.running)
  {
    const std::string overlay = generation.phase + "  " + std::to_string(generation.elapsedSeconds).substr(0, 5) + "s";
    ImGui::ProgressBar(static_cast<float>(generation.progress), ImVec2(-1.0f, 0.0f), overlay.c_str());
    if (ImGui::Button("Cancel generation"))
      requestGenerationCancel(state);
  }
  if (state.useSphericalWorld)
  {
    ImGui::SeparatorText("Legacy raster export erosion");
    ImGui::TextDisabled("Optional export-only GPU pass. It does not alter the authoritative spherical mesh preview.");
    ImGui::BeginDisabled(state.erosionRunning);
    int erosionIterations = static_cast<int>(state.erosionSettings.iterations);
    if (ImGui::SliderInt("Erosion iterations", &erosionIterations, 16, 512))
      state.erosionSettings.iterations = static_cast<uint32_t>(erosionIterations);
    describeField("Number of optional raster hydraulic simulation steps. This legacy pass alters exported textures, not the spherical graph mesh.");
    int erosionBatch = static_cast<int>(state.erosionSettings.iterationsPerBatch);
    if (ImGui::SliderInt("Iterations per GPU batch", &erosionBatch, 1, 16))
      state.erosionSettings.iterationsPerBatch = static_cast<uint32_t>(erosionBatch);
    describeField("Dispatches grouped before yielding to the UI. Larger batches reduce overhead but update progress less frequently.");
    ImGui::SliderFloat("Rainfall", &state.erosionSettings.rainfall, 0.0f, 0.003f, "%.6f");
    describeField("Water added per raster erosion step, modulated by the generated precipitation texture.");
    ImGui::SliderFloat("Evaporation", &state.erosionSettings.evaporation, 0.0f, 0.12f, "%.4f");
    describeField("Fraction of surface water removed each erosion step. Higher values shorten transport distances.");
    ImGui::SliderFloat("Erosion strength", &state.erosionSettings.erosionRate, 0.0f, 0.30f, "%.4f");
    describeField("Rate at which under-capacity water removes terrain and carries it as sediment.");
    ImGui::SliderFloat("Deposition", &state.erosionSettings.depositionRate, 0.0f, 0.50f, "%.4f");
    describeField("Rate at which excess carried sediment is deposited into valleys and plains.");
    ImGui::SliderFloat("Sediment capacity", &state.erosionSettings.sedimentCapacity, 0.1f, 20.0f, "%.2f");
    describeField("Maximum sediment carried as a function of flow speed and slope. Higher values cut more before depositing.");
    ImGui::Checkbox("Run erosion after generation", &state.autoErodeAfterGeneration);
    describeField("Automatically starts the optional raster erosion pass when background generation completes.");
    ImGui::Checkbox("Save automatically after erosion", &state.saveAfterErosion);
    describeField("Writes maps and the streaming package after automatic GPU erosion completes.");
    ImGui::BeginDisabled(!state.generated || !state.generatedFromSphericalWorld || generation.running);
    if (ImGui::Button("Run GPU erosion"))
      state.erosionRequested = true;
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (state.erosionRunning)
    {
      const std::string overlay = std::to_string(state.erosionCompletedIterations) + " / " + std::to_string(state.erosionSettings.iterations) + " iterations";
      ImGui::ProgressBar(state.erosionProgress, ImVec2(-1.0f, 0.0f), overlay.c_str());
      if (ImGui::Button("Cancel erosion"))
        state.erosionCancelRequested = true;
      ImGui::TextDisabled("Flux, erosion/deposition, and sediment transport run asynchronously on the GPU.");
    }
  }
  ImGui::Separator();
  ImGui::BeginDisabled(busy);
  ImGui::InputText("Configuration", state.configurationPath.data(), state.configurationPath.size());
  describeField("Path of a terrain JSON configuration to load into the controls. Loading does not regenerate automatically.");
  if (ImGui::Button("Load configuration"))
  {
    std::string error;
    state.status = loadConfiguration(state, state.configurationPath.data(), error) ? "Loaded configuration. Generate preview to apply it." : error;
  }
  ImGui::InputText("Output directory", state.outputDirectory.data(), state.outputDirectory.size());
  describeField("Directory for raw diagnostic maps, generated configuration, and the tiled LZ4 terrain package.");
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!state.generated || busy);
  if (ImGui::Button("Save maps + config"))
  {
    std::string error;
    state.status = saveMaps(state, error) ?
        (state.generatedFromSphericalWorld ? "Saved diagnostics, terrain-config.json, and planet.planetpkg." : "Saved diagnostics, terrain-config.json, and the planar terrain package.") :
        error;
  }
  ImGui::EndDisabled();
  ImGui::Separator();
  if (state.generated)
  {
    if (state.generatedFromSphericalWorld)
    {
      ImGui::Text("Plates %u", state.statistics.siteCount);
      ImGui::Text("Graph nodes %zu  Adjacency entries %zu", state.sphereRegionNodes.size(), state.sphereRegionNeighbourIndices.size());
      ImGui::Text("River sources %u  Flowing nodes %u", state.statistics.riverSourceCount, state.statistics.riverEdgeCount);
      ImGui::Text("Ocean nodes %u  Lake nodes %u", state.statistics.oceanSiteCount, state.statistics.lakeSiteCount);
    }
    else
    {
      ImGui::Text("Sites %u  Triangles %u", state.statistics.siteCount, state.statistics.triangleCount);
      ImGui::Text("Voronoi corners %u", state.statistics.voronoiCornerCount);
      ImGui::Text("River sources %u  River edges %u", state.statistics.riverSourceCount, state.statistics.riverEdgeCount);
      ImGui::Text("Ocean sites %u  Lake sites %u", state.statistics.oceanSiteCount, state.statistics.lakeSiteCount);
    }
    if (state.packageStatistics.tileColumns != 0u)
    {
      ImGui::Text("Package tiles %u x %u", state.packageStatistics.tileColumns, state.packageStatistics.tileRows);
      const double ratio = state.packageStatistics.uncompressedBytes == 0u ? 0.0 : static_cast<double>(state.packageStatistics.compressedBytes) / static_cast<double>(state.packageStatistics.uncompressedBytes);
      ImGui::Text(
          "LZ4 %.2f MB / %.2f MB (%.1f%%)",
          static_cast<double>(state.packageStatistics.compressedBytes) / (1024.0 * 1024.0),
          static_cast<double>(state.packageStatistics.uncompressedBytes) / (1024.0 * 1024.0),
          ratio * 100.0);
    }
    if (state.planetPackageStatistics.blockCount != 0u)
    {
      const double ratio = state.planetPackageStatistics.uncompressedPayloadBytes == 0u ?
          0.0 :
          static_cast<double>(state.planetPackageStatistics.storedPayloadBytes) / static_cast<double>(state.planetPackageStatistics.uncompressedPayloadBytes);
      ImGui::Text("Cube tiles %llu  Channels %u  Blocks %u",
          static_cast<unsigned long long>(state.planetCubeStatistics.tileCount),
          state.planetPackageStatistics.channelCount,
          state.planetPackageStatistics.blockCount);
      ImGui::Text("LZ4 %.2f MB / %.2f MB (%.1f%%)",
          static_cast<double>(state.planetPackageStatistics.storedPayloadBytes) / (1024.0 * 1024.0),
          static_cast<double>(state.planetPackageStatistics.uncompressedPayloadBytes) / (1024.0 * 1024.0),
          ratio * 100.0);
    }
  }
  ImGui::TextWrapped("%s", state.status.c_str());
  ImGui::End();
}

void drawPreviews(const ApplicationState &state)
{
  ImGui::SetNextWindowSize(ImVec2(1160.0f, 900.0f), ImGuiCond_FirstUseEver);
  ImGui::Begin("Terrain Maps");
  if (state.generatedFromSphericalWorld && !state.textures.height.valid())
    ImGui::TextWrapped("Generate a world to populate the raster atlases.");
  if (ImGui::BeginTabBar("Maps"))
  {
    if (ImGui::BeginTabItem("Heightmap"))
    {
      drawTexture("Height + coast", state.textures.height);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Rivers"))
    {
      drawTexture("Downhill river edges", state.textures.rivers);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Regions"))
    {
      drawTexture("Voronoi regions", state.textures.regions);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Continents"))
    {
      drawTexture("Hierarchical continent/ocean macro field", state.textures.continents);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Tectonics"))
    {
      drawTexture("Plate uplift, rifts, and mountain-boundary influence", state.textures.tectonics);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Mountains"))
    {
      drawTexture("Slope and curvature produced by tectonic uplift and erosion", state.textures.mountains);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Forms"))
    {
      drawTexture("Terrain forms: water, plains, valleys, hills, mountains, peaks", state.textures.terrainForms);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Materials"))
    {
      drawTexture("Biomes + materials", state.textures.materials);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Temperature"))
    {
      drawTexture("Climate temperature: cold blue to hot red", state.textures.temperature);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Rainfall"))
    {
      drawTexture("Wind-driven precipitation: dry tan to wet blue", state.textures.precipitation);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Wind"))
    {
      drawTexture("Wind exposure and orographic lift", state.textures.wind);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Forests"))
    {
      drawTexture("Forest suitability / likely forest cover", state.textures.forests);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Cities"))
    {
      drawTexture("City suitability: flat, temperate, watered plains", state.textures.settlements);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::End();
}

} // namespace

int main(const int argc, char **argv)
{
  ApplicationState state{};
  bool headless = false;
  for (int argument = 1; argument < argc; ++argument)
  {
    const std::string option = argv[argument];
    if (option == "--help")
    {
      printUsage();
      return 0;
    }
    if (option == "--headless")
    {
      headless = true;
      continue;
    }
    if (option == "--sphere")
    {
      state.useSphericalWorld = true;
      if (state.raster.height == state.raster.width)
        state.raster.height = std::max(4u, state.raster.width / 2u);
      continue;
    }
    if (option == "--polygon")
    {
      state.useSphericalWorld = false;
      if (state.raster.height * 2u == state.raster.width)
        state.raster.height = state.raster.width;
      continue;
    }
    if (option == "--erode")
    {
      state.autoErodeAfterGeneration = true;
      state.saveAfterErosion = true;
      continue;
    }
    if (option == "--sediment-transport")
    {
      state.planetErosion.sedimentTransport = true;
      continue;
    }
    if (option == "--closed-system")
    {
      // Removes the ocean outlet so conservation can be checked for real.
      state.planetErosion.closedSystem = true;
      continue;
    }
    if (option == "--uniform-erodibility")
    {
      state.planetErosion.differentialErosion = false;
      continue;
    }
    if (option == "--no-fluvial-erosion")
    {
      // Bakes the graph's relief with no fluvial solver at all. This is the
      // before side of every phase-1 comparison, not a supported world.
      state.planetErosionEnabled = false;
      continue;
    }
    if (argument + 1 >= argc)
    {
      printUsage();
      return 1;
    }
    const char *value = argv[++argument];
    if (option == "--config")
    {
      std::snprintf(state.configurationPath.data(), state.configurationPath.size(), "%s", value);
      std::string error;
      if (!loadConfiguration(state, state.configurationPath.data(), error))
      {
        std::cerr << error << "\n";
        return 1;
      }
    }
    else if (option == "--output")
      std::snprintf(state.outputDirectory.data(), state.outputDirectory.size(), "%s", value);
    else if (option == "--seed")
    {
      if (!parseUnsigned(value, state.settings.seed))
        return 1;
    }
    else if (option == "--extent")
    {
      double extent = 0.0;
      if (!parseDouble(value, extent) || extent <= 0.0)
        return 1;
      state.settings.widthMetres = extent;
      state.settings.heightMetres = extent;
    }
    else if (option == "--site-radius")
    {
      if (!parseDouble(value, state.settings.siteRadiusMetres) || state.settings.siteRadiusMetres <= 0.0)
        return 1;
    }
    else if (option == "--resolution")
    {
      uint64_t resolution = 0u;
      if (!parseUnsigned(value, resolution) || resolution == 0u || resolution > 16384u)
        return 1;
      state.raster.width = static_cast<uint32_t>(resolution);
      state.raster.height = state.useSphericalWorld ? std::max(4u, static_cast<uint32_t>(resolution / 2u)) : static_cast<uint32_t>(resolution);
    }
    else if (option == "--tile-size")
    {
      uint64_t size = 0u;
      if (!parseUnsigned(value, size) || size == 0u || size > 16384u)
        return 1;
      state.package.tileSizePixels = static_cast<uint32_t>(size);
    }
    else if (option == "--threads")
    {
      uint64_t count = 0u;
      if (!parseUnsigned(value, count) || count > 1024u)
        return 1;
      state.raster.workerCount = static_cast<uint32_t>(count);
    }
    else if (option == "--rivers")
    {
      uint64_t count = 0u;
      if (!parseUnsigned(value, count) || count > 100000u)
        return 1;
      state.settings.riverSourceCount = static_cast<uint32_t>(count);
    }
    else if (option == "--plates")
    {
      uint64_t count = 0u;
      if (!parseUnsigned(value, count) || count > 255u)
        return 1;
      state.planetPlateCount = static_cast<uint32_t>(count);
    }
    else if (option == "--regions")
    {
      uint64_t count = 0u;
      if (!parseUnsigned(value, count) || count < 4096u || count > 256000u)
        return 1;
      state.planetGraphRegionCount = static_cast<uint32_t>(count);
    }
    else if (option == "--planet-radius")
    {
      if (!parseDouble(value, state.planetRadiusMetres) || state.planetRadiusMetres < 1000.0 || state.planetRadiusMetres > 1.0e9)
        return 1;
    }
    else if (option == "--erosion-iterations")
    {
      uint64_t count = 0u;
      if (!parseUnsigned(value, count) || count > 65536u)
        return 1;
      state.planetErosion.iterations = static_cast<uint32_t>(count);
    }
    else if (option == "--erosion-mode")
    {
      uint64_t mode = 0u;
      if (!parseUnsigned(value, mode) || mode > 2u)
        return 1;
      state.planetErosion.erosionMode = static_cast<uint32_t>(mode);
    }
    else if (option == "--erosion-k")
    {
      if (!parseDouble(value, state.planetErosion.streamPowerK) || state.planetErosion.streamPowerK < 0.0)
        return 1;
    }
    else if (option == "--erosion-uplift")
    {
      if (!parseDouble(value, state.planetErosion.upliftRate) || state.planetErosion.upliftRate < 0.0)
        return 1;
    }
    else if (option == "--erosion-hillslope")
    {
      if (!parseDouble(value, state.planetErosion.hillslopeK) || state.planetErosion.hillslopeK < 0.0)
        return 1;
    }
    else if (option == "--erosion-time")
    {
      if (!parseDouble(value, state.planetErosion.simulatedTime) || !(state.planetErosion.simulatedTime > 0.0))
        return 1;
    }
    else if (option == "--erosion-face-side")
    {
      uint64_t side = 0u;
      if (!parseUnsigned(value, side) || side > 8192u)
        return 1;
      state.planetErosion.faceSide = static_cast<uint32_t>(side);
    }
    else if (option == "--planet-relief")
    {
      if (!parseDouble(value, state.planetReliefMetres) || state.planetReliefMetres < 0.0 || state.planetReliefMetres > state.planetRadiusMetres * 0.5)
        return 1;
    }
    else if (option == "--oceanic-fraction")
    {
      if (!parseDouble(value, state.planetOceanicPlateFraction) || state.planetOceanicPlateFraction < 0.0 || state.planetOceanicPlateFraction > 1.0)
        return 1;
    }
    else if (option == "--jitter")
    {
      if (!parseDouble(value, state.planetJitter) || state.planetJitter < 0.0 || state.planetJitter > 1.0)
        return 1;
    }
    else if (option == "--collision-threshold")
    {
      if (!parseDouble(value, state.planetCollisionThreshold) || state.planetCollisionThreshold < 0.02 || state.planetCollisionThreshold > 2.0)
        return 1;
    }
    else
    {
      printUsage();
      return 1;
    }
  }

  // The spherical export atlas is equirectangular, so it must stay 2:1. Only
  // --resolution and the GUI's own checkbox ever applied that, which left a
  // plain windowed launch - the default, spherical, path - baking a square
  // atlas over a 2:1 domain and stretching every texel two to one in longitude.
  if (state.useSphericalWorld)
    state.raster.height = std::max(4u, state.raster.width / 2u);

  if (headless)
  {
    if (state.autoErodeAfterGeneration)
    {
      std::cerr << "--erode requires the Vulkan preview window; it is not available with --headless.\n";
      return 1;
    }
    std::string error;
    // Fluvial relief is a GPU pass, so even a headless bake needs a compute
    // device. No surface is presented; init() with an empty surface list is the
    // same setup the GPU test suites use. The solver outlives saveMaps because
    // saveMaps re-runs generation when the atlases were deferred.
    const gpu::DeviceRequiredLimits solverLimits{
      .minimumMemory = 0u,
      .minimumComputeSharedMemory = 0u,
      .minimumComputeWorkGroupInvocations = 64u,
    };
    std::unique_ptr<TerrainPreviewRHI> solverRhi;
    std::unique_ptr<procedural::rendering::PlanetStreamPowerSolver> solver;
    if (state.useSphericalWorld && state.planetErosionEnabled)
    {
      solverRhi = std::make_unique<TerrainPreviewRHI>(vkbackend::Vulkan_1_2, solverLimits, gpu::DeviceFeatures_Compute, std::vector<std::string>{});
      std::vector<VkSurfaceKHR> noSurfaces;
      solverRhi->init(noSurfaces);
      solver = std::make_unique<procedural::rendering::PlanetStreamPowerSolver>(*solverRhi, PLANET_STREAM_POWER_SHADER_PATH, state.planetErosion);
      state.fluvialSolver = solver.get();
    }
    if (state.useSphericalWorld)
    {
      state.generated = generateSphericalPreview(state, error);
    }
    else
    {
      procedural::PolygonTerrainGenerator generator;
      state.generated = generator.generate(state.settings, state.raster, state.maps, &state.statistics, &error);
    }
    const bool saved = state.generated && saveMaps(state, error);
    if (solver)
      state.planetErosionStatistics = solver->statistics();
    state.fluvialSolver = nullptr;
    if (!saved)
    {
      std::cerr << "Terrain generation failed: " << error << "\n";
      return 1;
    }
    std::cout << "Saved terrain maps to " << state.outputDirectory.data() << "\n";
    if (state.generatedFromSphericalWorld)
    {
      printSphericalProfile(state.planetProfile, state.raster.width, state.raster.height);
      if (solver)
      {
        printPlanetErosionStatistics(state.planetErosionStatistics);
        if (!writePlanetErosionField(
                std::filesystem::path(state.outputDirectory.data()) / "planet-erosion-field.bin",
                solver->solvedField(), solver->drainageField(), state.planetRadiusMetres))
          std::cerr << "Could not write planet-erosion-field.bin\n";
      }
      if (!writeSphericalProfile(std::filesystem::path(state.outputDirectory.data()) / "planet-profile.json", state.planetProfile, state.raster.width, state.raster.height, state.settings.seed, state.planetRadiusMetres))
        std::cerr << "Could not write planet-profile.json\n";
    }
    solver.reset();
    solverRhi.reset();
    return 0;
  }

  // Fluvial relief is a GPU pass, and the windowed path needs it just as much
  // as the headless one: without this the Generate button produced terrain with
  // no erosion at all, silently, because the solver was only ever constructed
  // inside the headless branch.
  //
  // It gets its own compute device rather than sharing the preview's. Generation
  // runs on a worker thread, and driving the device that is drawing the window
  // from that thread is not safe. Both outlive every generation, since the
  // worker may still be running when a regeneration is requested.
  const gpu::DeviceRequiredLimits solverLimits{
    .minimumMemory = 0u,
    .minimumComputeSharedMemory = 0u,
    .minimumComputeWorkGroupInvocations = 64u,
  };
  std::unique_ptr<TerrainPreviewRHI> solverRhi;
  std::unique_ptr<procedural::rendering::PlanetStreamPowerSolver> solver;
  if (state.useSphericalWorld && state.planetErosionEnabled)
  {
    solverRhi = std::make_unique<TerrainPreviewRHI>(vkbackend::Vulkan_1_2, solverLimits, gpu::DeviceFeatures_Compute, std::vector<std::string>{});
    std::vector<VkSurfaceKHR> noSurfaces;
    solverRhi->init(noSurfaces);
    solver = std::make_unique<procedural::rendering::PlanetStreamPowerSolver>(*solverRhi, PLANET_STREAM_POWER_SHADER_PATH, state.planetErosion);
    state.fluvialSolver = solver.get();
  }

  // The globe preview displaces its dual mesh by the eroded raster, so what it
  // shows is what the package exports.

  os::Logger::setIdleSleep(lib::time::TimeSpan::fromMilliseconds(10));
  os::Logger::start();

  auto window = std::make_unique<window::sdl3::SDL3Window>(window::WindowSurface_Vulkan, "Polygon Terrain Generator", 1720, 1080);
  if (!window->isRunning || window->sdlWindow == nullptr)
  {
    std::cerr << "Could not create terrain preview Vulkan window: " << SDL_GetError() << "\n";
    os::Logger::shutdown();
    return 1;
  }

  const gpu::DeviceRequiredLimits limits{
    .minimumMemory = 0u,
    .minimumComputeSharedMemory = 10u * 1024u,
    .minimumComputeWorkGroupInvocations = 256u,
  };
  TerrainPreviewRHI rhi(vkbackend::Vulkan_1_2, limits, gpu::DeviceFeatures_Compute, std::vector<std::string>{});
  std::vector<VkSurfaceKHR> surfaces{window->getVulkanSurface(rhi.getInstance())};
  rhi.init(surfaces);
  rhi.setSwapChainVSyncEnabled(true);
  gpu::SwapChain swapChain = rhi.createSwapChain(0u, window->getWidth(), window->getHeight());
  const gpu::SamplerId previewSampler = rhi.createSampler({
    .name = "polygonTerrain.previewSampler",
    .minFilter = gpu::Filter::Linear,
    .magFilter = gpu::Filter::Linear,
    .addressModeU = gpu::SamplerAddressMode::Repeat,
    .addressModeV = gpu::SamplerAddressMode::ClampToEdge,
    .addressModeW = gpu::SamplerAddressMode::ClampToEdge,
    .maxLod = 1.0f,
  });

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.FontGlobalScale = 1.35f;
  ImGui::GetStyle().ScaleAllSizes(1.20f);
  if (!ImGui_ImplSDL3_InitForVulkan(window->sdlWindow))
    throw std::runtime_error("Failed to initialize Dear ImGui SDL3 Vulkan backend");
  TerrainPreviewImGuiOverlayPass overlayPass(rhi);
  overlayPass.initialize(swapChain);
  TerrainPreviewSpherePass spherePass(rhi, previewSampler, POLYGON_TERRAIN_SPHERE_SHADER_PATH);
  spherePass.initialize(overlayPass.renderPass());
  TerrainPreviewHydraulicErosionPass erosionPass(rhi, POLYGON_TERRAIN_EROSION_SHADER_PATH);
  ImGui_ImplVulkan_InitInfo imguiInfo{};
  imguiInfo.ApiVersion = VK_API_VERSION_1_2;
  imguiInfo.Instance = rhi.getInstance();
  imguiInfo.PhysicalDevice = rhi.getPhysicalDevice();
  imguiInfo.Device = rhi.getDevice();
  imguiInfo.QueueFamily = rhi.getGraphicsQueueFamilyIndex();
  imguiInfo.Queue = rhi.getGraphicsQueue();
  imguiInfo.DescriptorPoolSize = 256u;
  imguiInfo.MinImageCount = std::max(2u, rhi.getSwapChainImagesCount(swapChain));
  imguiInfo.ImageCount = std::max(2u, rhi.getSwapChainImagesCount(swapChain));
  imguiInfo.PipelineInfoMain.RenderPass = overlayPass.renderPass();
  imguiInfo.PipelineInfoMain.Subpass = 0u;
  imguiInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  imguiInfo.CheckVkResultFn = TerrainPreviewImGuiOverlayPass::checkVkResult;
  if (!ImGui_ImplVulkan_Init(&imguiInfo))
    throw std::runtime_error("Failed to initialize Dear ImGui Vulkan backend");

  TerrainPreviewTextureUploadPass textureUploadPass(rhi, previewSampler);
  TerrainPreviewRasterPass previewRasterPass(rhi, previewSampler, POLYGON_TERRAIN_RASTER_SHADER_PATH);
  const std::vector<uint8_t> graphFallbackPixels{255u, 255u, 255u, 255u};
  GpuPreviewTexture graphFallbackTexture = textureUploadPass.uploadRGBA("polygonTerrain.graphFallback", 1u, 1u, graphFallbackPixels);
  spherePass.setAtlases(graphFallbackTexture, graphFallbackTexture);
  state.adaptiveGlobeSupported = polygonterrain::PolygonTerrainAdaptiveGlobe::deviceSupported(rhi);
  if (!state.adaptiveGlobeSupported)
    state.adaptiveGlobeStatus = "This device cannot run the adaptive CBT; the globe stays on the fixed dual mesh.";
  // Rebuilt whenever the planet or the swapchain changes. Null means the fixed
  // dual mesh draws instead, which is also the failure path: an adaptive globe
  // that cannot be built must never take the planet off screen.
  std::unique_ptr<polygonterrain::PolygonTerrainAdaptiveGlobe> adaptiveGlobe;

  startGeneration(state);

  bool running = true;
  while (running)
  {
    SDL_Event event{};
    while (SDL_PollEvent(&event))
    {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window->sdlWindow)))
        running = false;
      if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT && state.useSphericalWorld && !ImGui::GetIO().WantCaptureMouse)
      {
        state.sphereDragging = true;
        state.sphereLastMouseX = event.button.x;
        state.sphereLastMouseY = event.button.y;
      }
      if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT)
        state.sphereDragging = false;
      // Wheel zoom, multiplicative so each notch covers the same visual step
      // whether the camera is at the whole globe or down at a coastline.
      if (event.type == SDL_EVENT_MOUSE_WHEEL && state.useSphericalWorld &&
          !ImGui::GetIO().WantCaptureMouse)
        state.sphereZoom = std::clamp(
            state.sphereZoom * std::pow(1.18f, event.wheel.y), 1.0f, 64.0f);
      if (event.type == SDL_EVENT_MOUSE_MOTION && state.sphereDragging)
      {
        const float dx = event.motion.x - state.sphereLastMouseX;
        const float dy = event.motion.y - state.sphereLastMouseY;
        state.sphereLastMouseX = event.motion.x;
        state.sphereLastMouseY = event.motion.y;
        state.sphereYawDegrees += dx * 0.35f;
        state.spherePitchDegrees = std::clamp(state.spherePitchDegrees + dy * 0.25f, -80.0f, 80.0f);
        if (state.sphereYawDegrees > 180.0f)
          state.sphereYawDegrees -= 360.0f;
        if (state.sphereYawDegrees < -180.0f)
          state.sphereYawDegrees += 360.0f;
      }
      if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
        state.sphereDragging = false;
      if (event.type == SDL_EVENT_WINDOW_RESIZED && event.window.windowID == SDL_GetWindowID(window->sdlWindow))
      {
        window->width = static_cast<uint32_t>(std::max(1, event.window.data1));
        window->height = static_cast<uint32_t>(std::max(1, event.window.data2));
        rhi.waitIdle();
        // The globe's graph holds per-image command buffers and viewport-sized
        // targets, so it cannot outlive the swapchain it was compiled against.
        // Dropping it here rebuilds it on the next frame at the new size.
        adaptiveGlobe.reset();
        overlayPass.destroyFramebuffers();
        rhi.destroySwapChain(swapChain);
        swapChain = rhi.createSwapChain(0u, window->getWidth(), window->getHeight());
        ImGui_ImplVulkan_SetMinImageCount(std::max(2u, rhi.getSwapChainImagesCount(swapChain)));
        overlayPass.onSwapChainRecreated(swapChain);
      }
    }
    if (SDL_GetWindowFlags(window->sdlWindow) & SDL_WINDOW_MINIMIZED)
    {
      SDL_Delay(10);
      continue;
    }
    pollGeneration(state, rhi, textureUploadPass, previewRasterPass);
    // Spherical worlds used to defer their raster atlases until Save, so the
    // erosion, lithology and biome maps could not be inspected before being
    // committed to disk. Build them once here, after generation has settled,
    // where a failure only costs the map tabs rather than the planet.
    if (state.generated && state.generatedFromSphericalWorld &&
        !state.textures.height.valid() && !state.previewRefreshFailed)
      state.previewRefreshFailed =
          !updatePreviewTextures(state, rhi, textureUploadPass, previewRasterPass);
    // Bake the working package the adaptive globe displaces from, once per
    // generation. Latched on both success and failure so a broken package is
    // attempted once rather than every frame, and a failure only costs the
    // subdivision: the fixed dual mesh keeps drawing the planet.
    if (state.generated && state.generatedFromSphericalWorld &&
        state.adaptiveGlobeSupported && state.adaptiveGlobeEnabled &&
        !state.adaptiveGlobeAvailable && !state.adaptiveGlobeFailed)
    {
      std::string globeError;
      state.status = "Baking the adaptive globe's surface atlas...";
      std::cout << state.status << "\n";
      if (buildAdaptiveGlobe(state, globeError))
      {
        state.adaptiveGlobeAvailable = true;
        state.adaptiveGlobeStatus =
            "Adaptive globe ready at " + std::to_string(state.adaptiveGlobeFaceResolution) +
            " per face; zoom subdivides the surface.";
        state.status = state.adaptiveGlobeStatus;
      }
      else
      {
        state.adaptiveGlobeFailed = true;
        state.adaptiveGlobeStatus = "Adaptive globe unavailable: " + globeError;
        state.status = state.adaptiveGlobeStatus;
      }
      std::cout << state.adaptiveGlobeStatus << "\n";
    }
    // Construction is separate from baking so a resize can rebuild the graph
    // without re-baking the package.
    if (state.adaptiveGlobeAvailable && state.adaptiveGlobeEnabled &&
        adaptiveGlobe == nullptr && !state.adaptiveGlobeFailed)
    {
      try
      {
        adaptiveGlobe = std::make_unique<polygonterrain::PolygonTerrainAdaptiveGlobe>(
            rhi, swapChain, window->getWidth(), window->getHeight(), state.planetOverview);
      }
      catch (const std::exception &exception)
      {
        state.adaptiveGlobeFailed = true;
        state.adaptiveGlobeStatus =
            std::string("Adaptive globe could not be built: ") + exception.what();
        state.status = state.adaptiveGlobeStatus;
        std::cout << state.adaptiveGlobeStatus << "\n";
        adaptiveGlobe.reset();
      }
    }
    if ((!state.adaptiveGlobeEnabled || !state.generatedFromSphericalWorld) &&
        adaptiveGlobe != nullptr)
      adaptiveGlobe.reset();
    if (state.autoErodeAfterGeneration && state.generated && state.generatedFromSphericalWorld && !state.autoErosionStarted)
    {
      state.autoErosionStarted = true;
      state.erosionRequested = true;
    }
    if (state.erosionCancelRequested)
    {
      erosionPass.cancel();
      state.erosionCancelRequested = false;
    }
    if (state.erosionRunning)
    {
      std::vector<uint16_t> erodedHeightmap;
      std::vector<uint8_t> erosionFlowMap;
      std::string erosionError;
      if (erosionPass.poll(erodedHeightmap, erosionFlowMap, erosionError))
      {
        state.erosionRunning = false;
        state.erosionProgress = erosionPass.progress();
        state.erosionCompletedIterations = erosionPass.completedIterations();
        if (!erosionError.empty())
          state.status = erosionError;
        else if (erodedHeightmap.size() == state.maps.heightmap.size() && erosionFlowMap.size() == state.maps.rivers.size())
        {
          state.maps.heightmap = std::move(erodedHeightmap);
          state.maps.rivers = std::move(erosionFlowMap);
          refreshDerivedMapsAfterErosion(state);
          state.status = "GPU hydraulic erosion complete. Height and flow maps now include the eroded result.";
          std::cout << state.status << "\n";
          updatePreviewTextures(state, rhi, textureUploadPass, previewRasterPass);
          if (state.saveAfterErosion)
          {
            state.saveAfterErosion = false;
            std::string saveError;
            if (saveMaps(state, saveError))
            {
              state.status = "GPU erosion complete; saved the final height, runoff, diagnostics, configuration, and terrain package.";
              std::cout << state.status << "\n";
            }
            else
              state.status = saveError;
          }
        }
        else
          state.status = "GPU erosion returned maps with invalid dimensions";
      }
      else
      {
        state.erosionProgress = erosionPass.progress();
        state.erosionCompletedIterations = erosionPass.completedIterations();
      }
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    drawControls(state);
    drawPreviews(state);
    ImGui::Render();

    if (state.erosionRequested)
    {
      state.erosionRequested = false;
      std::string erosionError;
      if (!state.generatedFromSphericalWorld || !state.generated)
        state.status = "Generate a spherical terrain atlas before running GPU erosion.";
      else if (erosionPass.start(
                   state.maps.width, state.maps.height, state.maps.heightmap, state.maps.precipitation, state.maps.tectonics, state.erosionSettings, erosionError))
      {
        state.erosionRunning = true;
        state.erosionProgress = 0.0f;
        state.erosionCompletedIterations = 0u;
        state.status = "GPU hydraulic erosion running asynchronously.";
      }
      else
        state.status = erosionError;
    }

    const gpu::TextureView currentSwapView = rhi.getCurrentSwapChainTextureView(swapChain);
    // The adaptive globe owns the whole viewport when it is up: its RenderGraph
    // writes the acquired swapchain image, and the ImGui overlay then loads that
    // image instead of clearing it. The fixed dual mesh keeps its original path,
    // drawing inside the overlay's own render pass over a cleared image.
    const bool drawAdaptiveGlobe = adaptiveGlobe != nullptr &&
        state.generated && state.generatedFromSphericalWorld;
    if (drawAdaptiveGlobe)
    {
      polygonterrain::AdaptiveGlobeCamera camera{};
      camera.yawDegrees = state.sphereYawDegrees;
      camera.pitchDegrees = state.spherePitchDegrees;
      camera.zoom = state.sphereZoom;
      camera.lighting = state.sphereLighting ? 1.0f : 0.0f;
      const SpherePreviewLayer layer = static_cast<SpherePreviewLayer>(state.spherePreviewLayer);
      state.adaptiveGlobe.showOcean =
          layer == SpherePreviewLayer::Terrain || layer == SpherePreviewLayer::Materials;
      adaptiveGlobe->update(camera, state.adaptiveGlobe);
      adaptiveGlobe->render(currentSwapView.index);
      state.adaptiveGlobeActiveBisectors = adaptiveGlobe->activeBisectors();
      state.adaptiveGlobeCapacity = adaptiveGlobe->cbtCapacity();
    }
    else
      state.adaptiveGlobeActiveBisectors = 0u;
    overlayPass.render(
        swapChain,
        currentSwapView,
        ImGui::GetDrawData(),
        [&](VkCommandBuffer commandBuffer, VkExtent2D extent)
        {
          if (drawAdaptiveGlobe)
            return;
          if (!state.generatedFromSphericalWorld || !state.generated)
            return;
          const GpuPreviewTexture &atlas = selectedSphereAtlas(state);
          spherePass.setAtlases(atlas.valid() ? atlas : graphFallbackTexture, state.textures.regions.valid() ? state.textures.regions : graphFallbackTexture);
          updateSphereDirectPreview(state, spherePass);
          updateSphereLineOverlay(state, spherePass);
          spherePass.render(commandBuffer, extent, sphereRenderSettings(state));
        },
        drawAdaptiveGlobe);
  }

  if (state.generation.worker.joinable())
    state.generation.worker.join();
  // Before the swapchain and the RHI go, since the graph holds resources from
  // both and its destructor waits on any frame still in flight.
  adaptiveGlobe.reset();
  rhi.waitIdle();
  erosionPass.destroy();
  state.textures.reset(&rhi);
  destroyGpuPreviewTexture(rhi, graphFallbackTexture);
  spherePass.destroy();
  overlayPass.destroy();
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  rhi.deleteSampler(previewSampler);
  rhi.destroySwapChain(swapChain);
  os::Logger::shutdown();
  return 0;
}
