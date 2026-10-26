#include "globalIllumination/worldgi/WorldGIDDGI.hpp"

#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstddef>
#include <iostream>

int main()
{
  using namespace globalillumination::worldgi;

  static_assert(sizeof(WorldGIDDGIVolumeGPU) == 64u);
  static_assert(sizeof(WorldGIDDGIProbeStateGPU) == 32u);
  static_assert(offsetof(WorldGIDDGIVolumeGPU, dimensionsCascade) == 16u);
  static_assert(offsetof(WorldGIDDGIVolumeGPU, scrollOffsetGeneration) == 32u);
  static_assert(offsetof(WorldGIDDGIProbeStateGPU,
                    generationClassificationSamplesFlags) == 16u);

  const auto sanitized = worldGIDDGISanitize({
      .cascadeCount = 99u,
      .resolution = 2u,
      .raysPerProbe = 999u,
      .updateFraction = -1.0f,
      .hysteresis = 2.0f});
  assert(sanitized.cascadeCount == kWorldGIDDGIMaxCascades);
  assert(sanitized.resolution == kWorldGIDDGIMinResolution);
  assert(sanitized.raysPerProbe == 64u);
  assert(std::abs(sanitized.updateFraction - 0.01f) < 1.0e-6f);
  assert(std::abs(sanitized.hysteresis - 0.999f) < 1.0e-6f);

  assert(worldGIDDGIPositiveModulo(-1, 12u) == 11);
  assert(worldGIDDGIPositiveModulo(-13, 12u) == 11);
  assert(worldGIDDGIPositiveModulo(12, 12u) == 0);
  assert(worldGIDDGIProbeCount({12u, 12u, 12u}) == 1728u);

  const std::array<uint32_t, 3> dimensions{12u, 12u, 12u};
  const std::array<int32_t, 3> offset{11, 2, 0};
  assert((worldGIDDGIPhysicalCoordinate({0, 0, 0}, offset, dimensions) ==
          std::array<uint32_t, 3>{11u, 2u, 0u}));
  assert((worldGIDDGIPhysicalCoordinate({1, -3, 12}, offset, dimensions) ==
          std::array<uint32_t, 3>{0u, 11u, 0u}));
  assert(worldGIDDGIFlatten({3u, 2u, 1u}, dimensions) == 171u);

  const auto originA = worldGIDDGISnappedOrigin(
      {3.9f, -0.1f, 8.0f}, 4.0f, dimensions);
  const auto originB = worldGIDDGISnappedOrigin(
      {4.1f, -0.1f, 8.0f}, 4.0f, dimensions);
  assert((originA == std::array<float, 3>{-24.0f, -28.0f, -16.0f}));
  assert((originB == std::array<float, 3>{-20.0f, -28.0f, -16.0f}));
  const auto delta = worldGIDDGIScrollDelta(originA, originB, 4.0f);
  assert((delta == std::array<int32_t, 3>{1, 0, 0}));
  const auto nextOffset = worldGIDDGIAdvanceScrollOffset(
      {0, 0, 0}, delta, dimensions);
  assert((nextOffset == std::array<int32_t, 3>{1, 0, 0}));

  uint32_t exposed = 0u;
  for (uint32_t z = 0u; z < dimensions[2]; ++z)
    for (uint32_t y = 0u; y < dimensions[1]; ++y)
      for (uint32_t x = 0u; x < dimensions[0]; ++x)
        exposed += !worldGIDDGIProbePreservesHistory(
            {x, y, z}, delta, dimensions);
  assert(exposed == dimensions[1] * dimensions[2]);
  assert(!worldGIDDGIProbePreservesHistory({11u, 3u, 4u}, delta, dimensions));
  assert(worldGIDDGIProbePreservesHistory({0u, 3u, 4u}, delta, dimensions));

  const std::array<int32_t, 3> negativeDelta{-1, 0, 0};
  assert(!worldGIDDGIProbePreservesHistory(
      {0u, 3u, 4u}, negativeDelta, dimensions));
  assert(worldGIDDGIProbePreservesHistory(
      {11u, 3u, 4u}, negativeDelta, dimensions));

  const auto position = worldGIDDGIProbePosition(
      originA, 4.0f, {6u, 6u, 6u}, {0.25f, -0.5f, 1.0f});
  assert((position == std::array<float, 3>{0.25f, -4.5f, 9.0f}));

  std::cout << "WorldGIDDGIGridTests: passed\n";
  return 0;
}
