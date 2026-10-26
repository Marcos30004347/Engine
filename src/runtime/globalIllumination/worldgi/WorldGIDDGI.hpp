#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace globalillumination::worldgi
{

// Eight cascades at doubling spacing reach +-3 km from a 4 m finest ring. Four
// reached only +-192 m, so every pixel beyond that fell through to the screen
// probe gather - the expensive path the cache exists to replace.
inline constexpr uint32_t kWorldGIDDGIMaxCascades = 8u;
inline constexpr uint32_t kWorldGIDDGIDefaultResolution = 12u;
inline constexpr uint32_t kWorldGIDDGIMinResolution = 4u;
inline constexpr uint32_t kWorldGIDDGIMaxResolution = 16u;
inline constexpr uint32_t kWorldGIDDGIDefaultRaysPerProbe = 32u;
inline constexpr uint32_t kWorldGIDDGIVisibilityResolution = 8u;
inline constexpr uint32_t kWorldGIDDGISHCoefficientCount = 9u;
inline constexpr uint32_t kWorldGIDDGIInvalidProbe = UINT32_MAX;

enum WorldGIDDGIProbeClassification : uint32_t
{
  WorldGIDDGIProbeClassification_Invalid = 0u,
  WorldGIDDGIProbeClassification_New = 1u,
  WorldGIDDGIProbeClassification_Active = 2u,
  WorldGIDDGIProbeClassification_Inactive = 3u,
};

enum WorldGIDDGIProbeFlags : uint32_t
{
  WorldGIDDGIProbeFlag_None = 0u,
  WorldGIDDGIProbeFlag_ValidHistory = 1u << 0u,
  WorldGIDDGIProbeFlag_Relocated = 1u << 1u,
  WorldGIDDGIProbeFlag_PriorityUpdate = 1u << 2u,
};

// GPU ABI for one cascaded volume. The integer scroll offset maps logical
// coordinates into a toroidal physical array; generation invalidates slots
// exposed by a scroll without clearing unchanged probes.
struct alignas(16) WorldGIDDGIVolumeGPU
{
  float originSpacing[4]{0.0f, 0.0f, 0.0f, 1.0f};
  uint32_t dimensionsCascade[4]{1u, 1u, 1u, 0u};
  int32_t scrollOffsetGeneration[4]{0, 0, 0, 0};
  uint32_t probeBaseFlagsCursor[4]{0u, 0u, 0u, 0u};
};
static_assert(sizeof(WorldGIDDGIVolumeGPU) == 64u);

struct alignas(16) WorldGIDDGIProbeStateGPU
{
  float relocationConfidence[4]{0.0f, 0.0f, 0.0f, 0.0f};
  uint32_t generationClassificationSamplesFlags[4]{
      0u, WorldGIDDGIProbeClassification_Invalid, 0u,
      WorldGIDDGIProbeFlag_None};
};
static_assert(sizeof(WorldGIDDGIProbeStateGPU) == 32u);

struct WorldGIDDGISettings
{
  uint32_t cascadeCount = kWorldGIDDGIMaxCascades;
  uint32_t resolution = kWorldGIDDGIDefaultResolution;
  uint32_t raysPerProbe = kWorldGIDDGIDefaultRaysPerProbe;
  float updateFraction = 0.125f;
  float hysteresis = 0.97f;
  bool relocation = true;
  bool multibounce = false;
};

inline WorldGIDDGISettings worldGIDDGISanitize(
    WorldGIDDGISettings settings)
{
  settings.cascadeCount =
      std::clamp(settings.cascadeCount, 1u, kWorldGIDDGIMaxCascades);
  settings.resolution = std::clamp(
      settings.resolution, kWorldGIDDGIMinResolution,
      kWorldGIDDGIMaxResolution);
  settings.raysPerProbe = std::clamp(settings.raysPerProbe, 1u, 64u);
  settings.updateFraction = std::isfinite(settings.updateFraction)
      ? std::clamp(settings.updateFraction, 0.01f, 1.0f)
      : 0.125f;
  settings.hysteresis = std::isfinite(settings.hysteresis)
      ? std::clamp(settings.hysteresis, 0.0f, 0.999f)
      : 0.97f;
  return settings;
}

inline constexpr uint64_t worldGIDDGIProbeCount(
    const std::array<uint32_t, 3> dimensions)
{
  return static_cast<uint64_t>(dimensions[0]) * dimensions[1] * dimensions[2];
}

inline constexpr int32_t worldGIDDGIPositiveModulo(
    const int64_t value, const uint32_t modulus)
{
  if (modulus == 0u)
    return 0;
  const int64_t remainder = value % static_cast<int64_t>(modulus);
  return static_cast<int32_t>(
      remainder < 0 ? remainder + static_cast<int64_t>(modulus) : remainder);
}

inline constexpr std::array<uint32_t, 3> worldGIDDGIPhysicalCoordinate(
    const std::array<int32_t, 3> logical,
    const std::array<int32_t, 3> scrollOffset,
    const std::array<uint32_t, 3> dimensions)
{
  return {
      static_cast<uint32_t>(worldGIDDGIPositiveModulo(
          static_cast<int64_t>(logical[0]) + scrollOffset[0], dimensions[0])),
      static_cast<uint32_t>(worldGIDDGIPositiveModulo(
          static_cast<int64_t>(logical[1]) + scrollOffset[1], dimensions[1])),
      static_cast<uint32_t>(worldGIDDGIPositiveModulo(
          static_cast<int64_t>(logical[2]) + scrollOffset[2], dimensions[2]))};
}

inline constexpr uint32_t worldGIDDGIFlatten(
    const std::array<uint32_t, 3> coordinate,
    const std::array<uint32_t, 3> dimensions)
{
  return coordinate[0] + dimensions[0] *
      (coordinate[1] + dimensions[1] * coordinate[2]);
}

// The origin is the world position of logical probe (0,0,0). Snapping the
// central cell to the camera prevents sub-cell movement from invalidating the
// volume and leaves only whole newly exposed slabs on a scroll.
inline std::array<float, 3> worldGIDDGISnappedOrigin(
    const std::array<float, 3> cameraPosition, const float spacing,
    const std::array<uint32_t, 3> dimensions)
{
  const float safeSpacing =
      std::isfinite(spacing) ? std::max(spacing, 1.0e-4f) : 1.0f;
  std::array<float, 3> result{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const double cell = std::floor(
        static_cast<double>(cameraPosition[axis]) / safeSpacing);
    result[axis] = static_cast<float>(
        (cell - static_cast<double>(dimensions[axis] / 2u)) * safeSpacing);
  }
  return result;
}

inline std::array<int32_t, 3> worldGIDDGIScrollDelta(
    const std::array<float, 3> previousOrigin,
    const std::array<float, 3> nextOrigin, const float spacing)
{
  const double safeSpacing = std::max(
      std::isfinite(spacing) ? static_cast<double>(spacing) : 1.0, 1.0e-4);
  std::array<int32_t, 3> result{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const double cells =
        (static_cast<double>(nextOrigin[axis]) - previousOrigin[axis]) /
        safeSpacing;
    result[axis] = static_cast<int32_t>(std::clamp(
        std::llround(cells),
        static_cast<long long>(std::numeric_limits<int32_t>::min()),
        static_cast<long long>(std::numeric_limits<int32_t>::max())));
  }
  return result;
}

inline constexpr std::array<int32_t, 3> worldGIDDGIAdvanceScrollOffset(
    const std::array<int32_t, 3> previousOffset,
    const std::array<int32_t, 3> scrollDelta,
    const std::array<uint32_t, 3> dimensions)
{
  return {
      worldGIDDGIPositiveModulo(
          static_cast<int64_t>(previousOffset[0]) + scrollDelta[0],
          dimensions[0]),
      worldGIDDGIPositiveModulo(
          static_cast<int64_t>(previousOffset[1]) + scrollDelta[1],
          dimensions[1]),
      worldGIDDGIPositiveModulo(
          static_cast<int64_t>(previousOffset[2]) + scrollDelta[2],
          dimensions[2])};
}

// A new logical coordinate reuses history only when the corresponding old
// logical coordinate (new + delta) was inside the previous volume.
inline constexpr bool worldGIDDGIProbePreservesHistory(
    const std::array<uint32_t, 3> nextLogical,
    const std::array<int32_t, 3> scrollDelta,
    const std::array<uint32_t, 3> dimensions)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int64_t previousLogical =
        static_cast<int64_t>(nextLogical[axis]) + scrollDelta[axis];
    if (previousLogical < 0 ||
        previousLogical >= static_cast<int64_t>(dimensions[axis]))
      return false;
  }
  return true;
}

inline std::array<float, 3> worldGIDDGIProbePosition(
    const std::array<float, 3> origin, const float spacing,
    const std::array<uint32_t, 3> logical,
    const std::array<float, 3> relocation = {0.0f, 0.0f, 0.0f})
{
  return {
      origin[0] + spacing * logical[0] + relocation[0],
      origin[1] + spacing * logical[1] + relocation[1],
      origin[2] + spacing * logical[2] + relocation[2]};
}

} // namespace globalillumination::worldgi
