#pragma once

#include <algorithm>
#include <cstdint>

namespace voxel::svraster
{

// The guide is stored in the same two uniform parameters used by cosine
// hemisphere sampling: squared radius and azimuth. A piecewise-constant
// density in this domain has an exact solid-angle PDF after the ordinary
// cosine Jacobian is applied, unlike selecting discrete octahedral directions.
inline constexpr uint32_t kSparseVoxelSkyGuideTileSize = 16u;
inline constexpr uint32_t kSparseVoxelSkyGuideRadialBins = 4u;
inline constexpr uint32_t kSparseVoxelSkyGuideAzimuthBins = 4u;
inline constexpr uint32_t kSparseVoxelSkyGuideBinCount =
    kSparseVoxelSkyGuideRadialBins * kSparseVoxelSkyGuideAzimuthBins;
inline constexpr uint32_t kSparseVoxelSkyGuideFeedbackValuesPerBin = 2u;
// The high bit of the first bin's observation count requests that the
// screen-space guide discard its old CDF before consuming current feedback.
inline constexpr uint32_t kSparseVoxelSkyGuideFeedbackResetBit = 1u << 31u;

constexpr uint32_t sparseVoxelSkyGuideTileCount(
    const uint32_t width, const uint32_t height)
{
  return ((width + kSparseVoxelSkyGuideTileSize - 1u) /
          kSparseVoxelSkyGuideTileSize) *
      ((height + kSparseVoxelSkyGuideTileSize - 1u) /
       kSparseVoxelSkyGuideTileSize);
}

constexpr uint64_t sparseVoxelSkyGuideCDFBufferSize(
    const uint32_t width, const uint32_t height)
{
  return uint64_t{sparseVoxelSkyGuideTileCount(width, height)} *
      kSparseVoxelSkyGuideBinCount * sizeof(float);
}

constexpr uint64_t sparseVoxelSkyGuideFeedbackBufferSize(
    const uint32_t width, const uint32_t height)
{
  return uint64_t{sparseVoxelSkyGuideTileCount(width, height)} *
      kSparseVoxelSkyGuideBinCount *
      kSparseVoxelSkyGuideFeedbackValuesPerBin * sizeof(uint32_t);
}

// The guide is mixed with the original cosine proposal. This scale multiplies
// cos(theta)/pi and integrates to one when binProbability sums to one.
inline float sparseVoxelSkyGuidePDFScale(
    const float strength, const float binProbability)
{
  const float mixture = std::clamp(strength, 0.0f, 1.0f);
  return (1.0f - mixture) + mixture *
      std::max(binProbability, 0.0f) *
      static_cast<float>(kSparseVoxelSkyGuideBinCount);
}

} // namespace voxel::svraster
