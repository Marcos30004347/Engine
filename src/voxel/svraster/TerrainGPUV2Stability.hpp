#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

enum class TerrainGPUV2CaptureLayer : uint32_t
{
  RawGBuffer = 0u,
  UnlitAlbedo,
  DirectLightOnly,
  FullLighting,
  Upscaled,
};

inline constexpr uint32_t kTerrainGPUV2CaptureLayerCount = static_cast<uint32_t>(TerrainGPUV2CaptureLayer::Upscaled) + 1u;

struct TerrainGPUV2GroundHeightField
{
  std::array<double, 2u> originMetres{};
  double spacingMetres = 1.0;
  uint32_t width = 0u;
  uint32_t depth = 0u;
  std::vector<double> fineSurfaceHeightMetres{};
};

struct TerrainGPUV2GroundPathSettings
{
  std::array<double, 2u> startMetres{};
  std::array<double, 2u> travelMetres{};
  uint32_t travelFrames = 16u;
  uint32_t stationaryFrames = 8u;
  uint32_t closeFrames = 8u;
  double standingClearanceMetres = 1.5;
  double closeClearanceMetres = 0.01;
  double projectionJitterPixels = 0.25;
};

struct TerrainGPUV2GroundCameraSample
{
  uint64_t frameIndex = 0u;
  uint64_t comparisonGroup = 0u;
  std::array<double, 3u> positionMetres{};
  std::array<double, 2u> projectionJitterPixels{};
  double surfaceHeightMetres = 0.0;
  double clearanceMetres = 0.0;
  bool cameraMoving = false;
  bool closeRange = false;
};

bool buildTerrainGPUV2GroundPath(const TerrainGPUV2GroundHeightField &heightField, const TerrainGPUV2GroundPathSettings &settings, std::vector<TerrainGPUV2GroundCameraSample> &samples, std::string *error = nullptr);

struct TerrainGPUV2CapturePixel
{
  uint64_t primitiveId = 0u;
  uint64_t brickId = 0u;
  uint32_t cellId = 0u;
  uint32_t faceId = 0u;
  float depthMetres = 0.0f;
  std::array<float, 4u> color{};
  float lightVisibility = 1.0f;
  uint8_t expectedTerrain = 0u;
  uint8_t coverageCount = 0u;
  uint8_t boundaryConsistent = 1u;
  uint8_t cpuOccupied = 0u;
  uint8_t gpuOccupied = 0u;
  std::array<uint8_t, 3u> reserved{};
};

struct TerrainGPUV2VisualCapture
{
  TerrainGPUV2CaptureLayer layer = TerrainGPUV2CaptureLayer::RawGBuffer;
  uint64_t frameIndex = 0u;
  uint64_t comparisonGroup = 0u;
  uint32_t width = 0u;
  uint32_t height = 0u;
  bool cameraMoving = false;
  bool projectionJitterEnabled = false;
  bool temporalUpscaling = false;
  bool lightStationary = true;
  double cameraClearanceMetres = 0.0;
  double primaryRayOriginOffsetMetres = 0.0;
  double directShadowRayOriginOffsetMetres = 0.0;
  std::vector<TerrainGPUV2CapturePixel> pixels{};
};

struct TerrainGPUV2StabilityThresholds
{
  float depthMetres = 1.0e-5f;
  float color = 1.0e-4f;
  float lightVisibility = 1.0e-4f;
  double rayOriginOffsetMetres = 1.0e-9;
  double closeRangeMaximumMetres = 0.011;
};

struct TerrainGPUV2StabilityReport
{
  bool qualified = false;
  uint32_t captureLayerMask = 0u;
  uint64_t captures = 0u;
  uint64_t comparedPixels = 0u;
  uint64_t occupancyComparedPixels = 0u;
  uint64_t nonFinitePixels = 0u;
  uint64_t blackCrackPixels = 0u;
  uint64_t overlapPixels = 0u;
  uint64_t tJunctionPixels = 0u;
  uint64_t identityChanges = 0u;
  uint64_t depthChanges = 0u;
  uint64_t imageChanges = 0u;
  uint64_t shadowChanges = 0u;
  uint64_t upscalerGeometryMismatches = 0u;
  uint64_t upscalerHiddenDefects = 0u;
  uint64_t rayOriginOffsetViolations = 0u;
  uint64_t occupancyMismatches = 0u;
  uint64_t closeRangePixels = 0u;
  uint64_t closeRangeUnstablePixels = 0u;
  uint64_t missingCaptureRequirements = 0u;
};

bool analyzeTerrainGPUV2Stability(const std::vector<TerrainGPUV2VisualCapture> &captures, const TerrainGPUV2StabilityThresholds &thresholds, TerrainGPUV2StabilityReport &report, std::string *error = nullptr);

enum class TerrainGPUV2QualificationStatus : uint32_t
{
  Qualified,
  Failed,
  Unsupported,
};

struct TerrainGPUV2DeviceQualification
{
  std::string backend{};
  std::string device{};
  std::string driver{};
  uint64_t deviceProfile = 0u;
  TerrainGPUV2QualificationStatus status = TerrainGPUV2QualificationStatus::Unsupported;
  TerrainGPUV2StabilityReport report{};
  std::string reason{};
};

class TerrainGPUV2QualificationTable
{
public:
  bool add(const TerrainGPUV2DeviceQualification &qualification, std::string *error = nullptr);
  const std::vector<TerrainGPUV2DeviceQualification> &entries() const
  {
    return entries_;
  }
  std::string markdown() const;

private:
  std::vector<TerrainGPUV2DeviceQualification> entries_{};
};

const char *terrainGPUV2CaptureLayerName(TerrainGPUV2CaptureLayer layer);
const char *terrainGPUV2QualificationStatusName(TerrainGPUV2QualificationStatus status);

} // namespace voxel::svraster
