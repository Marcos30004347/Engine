#include "voxel/svraster/TerrainGPUV2Stability.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace svr = voxel::svraster;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

svr::TerrainGPUV2VisualCapture capture(const svr::TerrainGPUV2CaptureLayer layer, const uint64_t frame, const uint64_t group, const bool temporal = false, const double clearance = 1.0)
{
  svr::TerrainGPUV2VisualCapture result{};
  result.layer = layer;
  result.frameIndex = frame;
  result.comparisonGroup = group;
  result.width = 6u;
  result.height = 4u;
  result.projectionJitterEnabled = true;
  result.temporalUpscaling = temporal;
  result.cameraClearanceMetres = clearance;
  result.pixels.resize(result.width * result.height);
  for (uint32_t y = 0u; y < result.height; ++y)
    for (uint32_t x = 0u; x < result.width; ++x)
    {
      const uint32_t index = y * result.width + x;
      auto &pixel = result.pixels[index];
      pixel.primitiveId = 100u + index;
      pixel.brickId = 10u + x / 2u + 4u * (y / 2u);
      pixel.cellId = x + 8u * y;
      pixel.faceId = 3u;
      pixel.depthMetres = static_cast<float>(clearance + 0.001 * index);
      pixel.color = {0.25f + 0.01f * x, 0.35f + 0.01f * y, 0.45f, 1.0f};
      pixel.lightVisibility = 0.75f;
      pixel.expectedTerrain = 1u;
      pixel.coverageCount = 1u;
      pixel.boundaryConsistent = 1u;
      pixel.cpuOccupied = 1u;
      pixel.gpuOccupied = 1u;
    }
  return result;
}

std::vector<svr::TerrainGPUV2VisualCapture> stableMatrix()
{
  std::vector<svr::TerrainGPUV2VisualCapture> captures;
  for (uint64_t frame = 0u; frame < 2u; ++frame)
  {
    captures.push_back(capture(svr::TerrainGPUV2CaptureLayer::RawGBuffer, frame, 10u));
    captures.push_back(capture(svr::TerrainGPUV2CaptureLayer::RawGBuffer, frame, 10u, true));
    captures.push_back(capture(svr::TerrainGPUV2CaptureLayer::UnlitAlbedo, frame, 20u));
    captures.push_back(capture(svr::TerrainGPUV2CaptureLayer::DirectLightOnly, frame, 30u));
    captures.push_back(capture(svr::TerrainGPUV2CaptureLayer::FullLighting, frame, 40u));
    captures.push_back(capture(svr::TerrainGPUV2CaptureLayer::Upscaled, frame, 50u, true));
    captures.push_back(capture(svr::TerrainGPUV2CaptureLayer::RawGBuffer, frame + 2u, 60u, false, 0.01));
  }
  return captures;
}

svr::TerrainGPUV2VisualCapture &findCapture(std::vector<svr::TerrainGPUV2VisualCapture> &captures, const svr::TerrainGPUV2CaptureLayer layer, const uint64_t frame, const uint64_t group, const bool temporal = false)
{
  for (auto &value : captures)
    if (value.layer == layer && value.frameIndex == frame && value.comparisonGroup == group && value.temporalUpscaling == temporal)
      return value;
  std::abort();
}

void testGroundResolvedPath()
{
  std::cout << "ground-resolved close camera path\n";
  svr::TerrainGPUV2GroundHeightField heightField{};
  heightField.originMetres = {-2.0, -2.0};
  heightField.spacingMetres = 1.0;
  heightField.width = 8u;
  heightField.depth = 8u;
  heightField.fineSurfaceHeightMetres.resize(64u);
  for (uint32_t z = 0u; z < 8u; ++z)
    for (uint32_t x = 0u; x < 8u; ++x)
      heightField.fineSurfaceHeightMetres[z * 8u + x] = -120.0 + 0.5 * x - 0.25 * z;
  svr::TerrainGPUV2GroundPathSettings settings{};
  settings.startMetres = {-1.5, -1.5};
  settings.travelMetres = {4.0, 4.0};
  settings.travelFrames = 9u;
  settings.stationaryFrames = 4u;
  settings.closeFrames = 4u;
  std::vector<svr::TerrainGPUV2GroundCameraSample> samples;
  std::string error;
  check(svr::buildTerrainGPUV2GroundPath(heightField, settings, samples, &error), "camera path resolves against the fine terrain surface: " + error);
  bool exactClearance = samples.size() == 17u;
  uint32_t close = 0u;
  uint32_t stationaryJitter = 0u;
  for (const auto &sample : samples)
  {
    exactClearance &= std::abs(sample.positionMetres[1] - sample.surfaceHeightMetres - sample.clearanceMetres) < 1.0e-10;
    exactClearance &= sample.positionMetres[1] < 0.0; // Never mistakes a hypothetical coarse placeholder at height zero for the floor.
    close += sample.closeRange ? 1u : 0u;
    stationaryJitter += !sample.cameraMoving && (sample.projectionJitterPixels[0] != 0.0 || sample.projectionJitterPixels[1] != 0.0) ? 1u : 0u;
  }
  check(exactClearance && close == 4u && stationaryJitter >= 4u, "path includes travel, stationary jitter, and four exact 1 cm ground samples");
}

void testStableCaptureMatrix()
{
  std::cout << "raw, lighting, temporal and 1 cm stability matrix\n";
  const auto captures = stableMatrix();
  svr::TerrainGPUV2StabilityReport report{};
  std::string error;
  check(svr::analyzeTerrainGPUV2Stability(captures, {}, report, &error), "complete stability matrix analyzes: " + error);
  check(
      report.qualified && report.captureLayerMask == 0x1fu && report.comparedPixels != 0u && report.closeRangePixels != 0u && report.occupancyComparedPixels == captures.size() * 24u,
      "stable canonical faces pass raw-before-lighting, upscaler, shadow, occupancy, and 1 cm gates");
  check(
      report.blackCrackPixels == 0u && report.overlapPixels == 0u && report.tJunctionPixels == 0u && report.identityChanges == 0u && report.depthChanges == 0u && report.shadowChanges == 0u &&
          report.rayOriginOffsetViolations == 0u,
      "clean frames contain no cracks, overlaps, shifting identities, depth movement, shadow movement, or ray offsets");
}

void testDefectAttribution()
{
  std::cout << "visual defect attribution\n";
  auto captures = stableMatrix();
  auto &native0 = findCapture(captures, svr::TerrainGPUV2CaptureLayer::RawGBuffer, 0u, 10u);
  auto &native1 = findCapture(captures, svr::TerrainGPUV2CaptureLayer::RawGBuffer, 1u, 10u);
  native0.pixels[0].coverageCount = 0u;
  native0.pixels[1].coverageCount = 2u;
  native0.pixels[2].boundaryConsistent = 0u;
  native0.pixels[3].gpuOccupied = 0u;
  native1.pixels[4].faceId = 4u;
  native1.pixels[5].depthMetres += 0.1f;
  native0.primaryRayOriginOffsetMetres = 0.001;
  auto &temporal0 = findCapture(captures, svr::TerrainGPUV2CaptureLayer::RawGBuffer, 0u, 10u, true);
  temporal0.pixels[6].cellId += 1u;
  auto &unlit1 = findCapture(captures, svr::TerrainGPUV2CaptureLayer::UnlitAlbedo, 1u, 20u);
  unlit1.pixels[7].color[0] += 0.25f;
  auto &direct0 = findCapture(captures, svr::TerrainGPUV2CaptureLayer::DirectLightOnly, 0u, 30u);
  auto &direct1 = findCapture(captures, svr::TerrainGPUV2CaptureLayer::DirectLightOnly, 1u, 30u);
  direct0.directShadowRayOriginOffsetMetres = 0.002;
  direct1.pixels[8].lightVisibility -= 0.5f;
  auto &close1 = findCapture(captures, svr::TerrainGPUV2CaptureLayer::RawGBuffer, 3u, 60u);
  close1.pixels[9].depthMetres = std::numeric_limits<float>::quiet_NaN();

  svr::TerrainGPUV2StabilityReport report{};
  std::string error;
  check(svr::analyzeTerrainGPUV2Stability(captures, {}, report, &error), "defective matrix analyzes without hiding failures: " + error);
  check(
      !report.qualified && report.blackCrackPixels != 0u && report.overlapPixels != 0u && report.tJunctionPixels != 0u && report.identityChanges != 0u && report.depthChanges != 0u && report.imageChanges != 0u,
      "raw capture attributes cracks, overlaps, boundary errors, shifting faces, depth, and image changes");
  check(
      report.shadowChanges != 0u && report.upscalerGeometryMismatches != 0u && report.upscalerHiddenDefects != 0u && report.rayOriginOffsetViolations == 2u && report.occupancyMismatches != 0u &&
          report.closeRangeUnstablePixels != 0u && report.nonFinitePixels != 0u,
      "lighting, upscaler masking, ray offsets, CPU/GPU disagreement, and 1 cm instability remain independently visible");
}

void testQualificationTable()
{
  std::cout << "backend/device qualification table\n";
  svr::TerrainGPUV2StabilityReport qualifiedReport{};
  std::string error;
  check(svr::analyzeTerrainGPUV2Stability(stableMatrix(), {}, qualifiedReport, &error) && qualifiedReport.qualified, "qualification fixture passes: " + error);
  svr::TerrainGPUV2QualificationTable table;
  svr::TerrainGPUV2DeviceQualification vulkan{"Vulkan", "headless-v2", "test-driver", 42u, svr::TerrainGPUV2QualificationStatus::Qualified, qualifiedReport, "all Phase 9 gates"};
  svr::TerrainGPUV2DeviceQualification metal{"Metal", "unavailable", "none", 0u, svr::TerrainGPUV2QualificationStatus::Unsupported, {}, "backend unavailable in this test"};
  check(table.add(vulkan, &error) && table.add(metal, &error), "qualified and unsupported backend records are accepted: " + error);
  check(!table.add(vulkan, &error), "duplicate backend/device/profile records are rejected");
  const auto markdown = table.markdown();
  check(
      markdown.find("| Vulkan | headless-v2 |") != std::string::npos && markdown.find("qualified") != std::string::npos && markdown.find("unsupported") != std::string::npos,
      "qualification table is deterministic and human-readable");

  auto duplicated = stableMatrix();
  duplicated.push_back(duplicated.front());
  svr::TerrainGPUV2StabilityReport ignored{};
  check(!svr::analyzeTerrainGPUV2Stability(duplicated, {}, ignored, &error), "duplicate layer/frame/group capture keys are rejected");
}
} // namespace

int main()
{
  std::cout << "Sparse voxel terrain GPU V2 Phase 9 tests\n";
  testGroundResolvedPath();
  testStableCaptureMatrix();
  testDefectAttribution();
  testQualificationTable();
  if (failures != 0)
    std::cerr << failures << " Phase 9 checks failed\n";
  return failures == 0 ? 0 : 1;
}
