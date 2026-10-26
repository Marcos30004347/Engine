#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelProbe.hpp"
#include "voxel/svraster/SparseVoxelSkyGuide.hpp"

// These tests use assert() as their check mechanism, and Release builds
// define NDEBUG. Without this the assertions vanish - and because many of
// them wrap the call they are checking, the call under test vanishes with
// them, leaving objects unconfigured and the suite crashing or silently
// passing. cassert re-reads NDEBUG on every include, so undefining it here
// restores assert regardless of what included cassert earlier.
#undef NDEBUG
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

namespace
{
using namespace voxel::svraster;

void testSharedABI()
{
  assert(sizeof(SparseVoxelSceneRayGPU) == 80u);
  assert(alignof(SparseVoxelSceneRayGPU) == 16u);
  assert(offsetof(SparseVoxelSceneRayGPU, originMinimumDistance) == 0u);
  assert(offsetof(SparseVoxelSceneRayGPU, directionMaximumDistance) == 16u);
  assert(offsetof(SparseVoxelSceneRayGPU, pixelRayLight) == 32u);
  assert(offsetof(SparseVoxelSceneRayGPU, seedFlags) == 48u);
  assert(offsetof(SparseVoxelSceneRayGPU, lightSamplePdfWeight) == 64u);
  assert(sizeof(SparseVoxelSceneRayHitGPU) == 48u);
  assert(alignof(SparseVoxelSceneRayHitGPU) == 16u);
  assert(offsetof(SparseVoxelSceneRayHitGPU, hitPositionDistance) == 0u);
  assert(offsetof(SparseVoxelSceneRayHitGPU, globalCell) == 16u);
  assert(offsetof(SparseVoxelSceneRayHitGPU, status) == 28u);
  assert(offsetof(SparseVoxelSceneRayHitGPU, identity) == 32u);
  assert(sizeof(SparseVoxelDIReservoirGPU) == 80u);
  assert(alignof(SparseVoxelDIReservoirGPU) == 16u);
  assert(offsetof(SparseVoxelDIReservoirGPU, identity) == 0u);
  assert(offsetof(SparseVoxelDIReservoirGPU, weights) == 16u);
  assert(offsetof(SparseVoxelDIReservoirGPU, state) == 32u);
  assert(offsetof(SparseVoxelDIReservoirGPU, receiverNormalDepth) == 48u);
  assert(offsetof(SparseVoxelDIReservoirGPU, receiverIdentity) == 64u);
  assert(sizeof(SparseVoxelTemporalSceneRayHitGPU) == 96u);
  assert(alignof(SparseVoxelTemporalSceneRayHitGPU) == 16u);
  assert(offsetof(SparseVoxelTemporalSceneRayHitGPU, hit) == 0u);
  assert(offsetof(SparseVoxelTemporalSceneRayHitGPU, receiverVisibility) == 48u);
  assert(offsetof(SparseVoxelTemporalSceneRayHitGPU, momentsConfidence) == 64u);
  assert(offsetof(SparseVoxelTemporalSceneRayHitGPU, state) == 80u);
  assert(sizeof(SparseVoxelSceneRayStatisticsGPU) == 112u);
}

void testStatusContract()
{
  assert(static_cast<uint32_t>(SparseVoxelSceneRayStatus::Invalid) == 0u);
  assert(static_cast<uint32_t>(SparseVoxelSceneRayStatus::Pending) == 1u);
  assert(static_cast<uint32_t>(SparseVoxelSceneRayStatus::Hit) == 2u);
  assert(static_cast<uint32_t>(SparseVoxelSceneRayStatus::MissTMax) == 3u);
  assert(static_cast<uint32_t>(SparseVoxelSceneRayStatus::EscapedScene) == 4u);
  assert(static_cast<uint32_t>(SparseVoxelSceneRayStatus::EscapedTreeVolume) == 5u);
  assert(static_cast<uint32_t>(SparseVoxelSceneRayStatus::UnknownResidency) == 6u);
  assert(static_cast<uint32_t>(SparseVoxelSceneRayStatus::QueryLimit) == 7u);
  assert(static_cast<uint32_t>(SparseVoxelSceneRayBackend::None) == 0u);
  assert(static_cast<uint32_t>(SparseVoxelSceneRayBackend::Screen) == 1u);
  assert(static_cast<uint32_t>(SparseVoxelSceneRayBackend::Tree64) == 2u);
  assert((kSparseVoxelSceneRayFlagActive & kSparseVoxelSceneRayFlagShadow) == 0u);
  assert(kSparseVoxelRegionStabilityHeaderWords == 4u);
  assert(kSparseVoxelRegionStabilityLightCapacity == 8u);
  assert(kSparseVoxelRegionStabilityWordsPerTile == 12u);
  assert(sparseVoxelRegionStabilityTileCount(17u, 33u, 16u) == 6u);
  assert(sparseVoxelRegionStabilityBufferSize(17u, 33u, 16u) ==
      uint64_t{6u * 12u * sizeof(uint32_t)});
  assert(static_cast<uint32_t>(SparseVoxelDebugMode::SceneRayShading) == 14u);
  assert(static_cast<uint32_t>(SparseVoxelDebugMode::SceneRayStatus) == 15u);
  assert(static_cast<uint32_t>(SparseVoxelDebugMode::SceneRayScreenTrace) == 16u);
  assert(static_cast<uint32_t>(SparseVoxelDebugMode::SceneRayWorldTrace) == 17u);
  assert(static_cast<uint32_t>(SparseVoxelDebugMode::SceneRayLightingAccumulation) == 18u);
}

void testDefaultInvalidHit()
{
  const SparseVoxelSceneRayHitGPU hit{};
  assert(hit.status == static_cast<uint32_t>(SparseVoxelSceneRayStatus::Invalid));
  for (const int32_t cell : hit.globalCell)
    assert(cell == std::numeric_limits<int32_t>::min());
}

void testSkyGuideContract()
{
  assert(kSparseVoxelSkyGuideBinCount == 16u);
  assert(kSparseVoxelSkyGuideFeedbackResetBit == 0x80000000u);
  assert(sparseVoxelSkyGuideTileCount(1u, 1u) == 1u);
  assert(sparseVoxelSkyGuideTileCount(16u, 16u) == 1u);
  assert(sparseVoxelSkyGuideTileCount(17u, 33u) == 6u);
  assert(sparseVoxelSkyGuideCDFBufferSize(17u, 33u) ==
      uint64_t{6u * 16u * sizeof(float)});
  assert(sparseVoxelSkyGuideFeedbackBufferSize(17u, 33u) ==
      uint64_t{6u * 16u * 2u * sizeof(uint32_t)});
  assert(sparseVoxelSkyGuidePDFScale(0.0f, 0.0f) == 1.0f);
  assert(sparseVoxelSkyGuidePDFScale(0.75f, 1.0f / 16.0f) == 1.0f);
  assert(sparseVoxelSkyGuidePDFScale(1.0f, 0.25f) == 4.0f);
  const float probabilities[16] = {
      0.25f, 0.25f, 0.125f, 0.125f,
      0.0625f, 0.0625f, 0.03125f, 0.03125f,
      0.015625f, 0.015625f, 0.0078125f, 0.0078125f,
      0.00390625f, 0.00390625f, 0.00390625f, 0.00390625f};
  float probabilitySum = 0.0f;
  float densityIntegral = 0.0f;
  for (const float probability : probabilities)
  {
    probabilitySum += probability;
    densityIntegral += sparseVoxelSkyGuidePDFScale(0.75f, probability) /
        static_cast<float>(kSparseVoxelSkyGuideBinCount);
  }
  assert(probabilitySum == 1.0f);
  assert(densityIntegral > 0.999999f && densityIntegral < 1.000001f);
}

void testProbeDistanceThresholdContract()
{
  const auto automatic = sparseVoxelProbeDistanceThresholds(
      1000.0f, 0.0f, 0.0f, 0.0f, 0.0f);
  assert(automatic[0] == 350.0f);
  assert(automatic[1] == 650.0f);
  assert(automatic[2] == 550.0f);
  assert(automatic[3] == 700.0f);

  const auto explicitDistances = sparseVoxelProbeDistanceThresholds(
      1000.0f, 120.0f, 360.0f, 500.0f, 800.0f);
  assert(explicitDistances[0] == 120.0f);
  assert(explicitDistances[1] == 360.0f);
  assert(explicitDistances[2] == 500.0f);
  assert(explicitDistances[3] == 800.0f);

  // Reversed density tiers are ordered, and a cutoff at/before the fade start
  // is moved just beyond it so smoothstep never receives a zero-width range.
  const auto sanitized = sparseVoxelProbeDistanceThresholds(
      1000.0f, 400.0f, 200.0f, 900.0f, 700.0f);
  assert(sanitized[0] == 400.0f);
  assert(sanitized[1] == 400.0f);
  assert(sanitized[2] == 900.0f);
  assert(sanitized[3] > 900.0f);
}
}

int main()
{
  testSharedABI();
  testStatusContract();
  testDefaultInvalidHit();
  testSkyGuideContract();
  testProbeDistanceThresholdContract();
  std::cout << "SparseVoxelSceneRayTests: passed\n";
  return 0;
}
