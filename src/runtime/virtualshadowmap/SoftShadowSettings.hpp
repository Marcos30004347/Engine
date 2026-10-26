#pragma once

#include <algorithm>
#include <cstdint>

namespace virtualgeometry
{

enum class SoftShadowMethod : uint32_t
{
  Hard = 0u,
  PCF = 1u,
  PCSS = 2u,
  LayeredScreenSpaceSoftShadows = 3u,
};

struct ScreenSpaceSoftShadowSettings
{
  enum class DebugMode : uint32_t
  {
    HardShadow = 0u,
    Penumbra = 1u,
    Translucency = 2u,
    HorizontalBlur = 3u,
    BlurredVisibility = 4u,
    LayerOccupancy = 5u,
  };

  uint32_t resolutionScale = 2u;
  uint32_t maxSoftShadowLayers = 8u;
  uint32_t kernelSize = 11u;
  uint32_t blockerSearchTaps = 3u;
  bool useMinFilter = false;
  float defaultLightRadiusWorld = 0.5f;
  float penumbraScale = 1.0f;
  float grazingThreshold = 0.25f;
  float depthRejectThreshold = 0.003f;
  bool translucentShadowsEnabled = false;
  DebugMode debugMode = DebugMode::BlurredVisibility;
  uint32_t debugLayer = 0u;
  float debugValueScale = 1.0f;
};

inline uint32_t sanitizeScreenSpaceSoftShadowResolutionScale(uint32_t scale)
{
  return (scale == 1u || scale == 2u || scale == 4u) ? scale : 2u;
}

inline uint32_t sanitizeScreenSpaceSoftShadowKernelSize(uint32_t kernelSize)
{
  return (kernelSize == 5u || kernelSize == 11u || kernelSize == 23u) ? kernelSize : 11u;
}

inline uint32_t sanitizeScreenSpaceSoftShadowBlockerSearchTaps(uint32_t taps)
{
  taps = std::max(taps, 1u);
  return (taps & 1u) != 0u ? taps : taps + 1u;
}

inline uint32_t scaledScreenSpaceSoftShadowExtent(uint32_t extent, uint32_t scale)
{
  const uint32_t sanitizedScale = sanitizeScreenSpaceSoftShadowResolutionScale(scale);
  return std::max(1u, (extent + sanitizedScale - 1u) / sanitizedScale);
}

} // namespace virtualgeometry
