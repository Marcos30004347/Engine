#include "virtualshadowmap/SoftShadowLayers.hpp"
#include "virtualshadowmap/SoftShadowSettings.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace virtualgeometry;

namespace
{

void require(bool condition, const char *message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

SoftShadowLightVolume point(float x, float y, float z, float range)
{
  SoftShadowLightVolume light{};
  light.type = SoftShadowLightType::Point;
  light.position = math::Vec3f(x, y, z);
  light.range = range;
  return light;
}

SoftShadowLightVolume spot(float x, float y, float z, float range)
{
  SoftShadowLightVolume light{};
  light.type = SoftShadowLightType::Spot;
  light.position = math::Vec3f(x, y, z);
  light.direction = math::Vec3f(0.0f, -1.0f, 0.0f);
  light.range = range;
  light.outerAngleRadians = 0.75f;
  return light;
}

SoftShadowLightVolume directional()
{
  SoftShadowLightVolume light{};
  light.type = SoftShadowLightType::Directional;
  light.direction = math::Vec3f(0.0f, -1.0f, 0.0f);
  return light;
}

void testEmptyLightListProducesZeroLayers()
{
  const SoftShadowLayerBuildResult result = buildSoftShadowLayerAssignment({}, 4u);
  require(result.activeLayerCount == 0u, "empty list should produce zero layers");
  require(result.assignments.empty(), "empty list should produce no assignments");
}

void testSeparatedPointLightsShareLayer()
{
  const SoftShadowLayerBuildResult result = buildSoftShadowLayerAssignment({point(0.0f, 0.0f, 0.0f, 1.0f), point(4.0f, 0.0f, 0.0f, 1.0f)}, 4u);
  require(result.activeLayerCount == 1u, "separated points should share one layer");
  require(result.assignments[0].layer == result.assignments[1].layer, "separated points should have same layer");
}

void testTouchingPointLightsOverlap()
{
  require(softShadowLightVolumesOverlap(point(0.0f, 0.0f, 0.0f, 1.0f), point(2.0f, 0.0f, 0.0f, 1.0f)), "touching points should overlap conservatively");
}

void testOverlappingPointLightsUseDifferentLayers()
{
  const SoftShadowLayerBuildResult result = buildSoftShadowLayerAssignment({point(0.0f, 0.0f, 0.0f, 2.0f), point(3.0f, 0.0f, 0.0f, 2.0f)}, 4u);
  require(result.activeLayerCount == 2u, "overlapping points should use two layers");
  require(result.assignments[0].layer != result.assignments[1].layer, "overlapping points should have different layers");
}

void testOverlapChainUsesTwoLayers()
{
  const SoftShadowLayerBuildResult result = buildSoftShadowLayerAssignment({point(0.0f, 0.0f, 0.0f, 1.1f), point(2.0f, 0.0f, 0.0f, 1.1f), point(4.0f, 0.0f, 0.0f, 1.1f)}, 4u);
  require(result.activeLayerCount == 2u, "overlap chain should use two layers");
  require(result.assignments[0].layer == result.assignments[2].layer, "non-neighbor chain endpoints should share layer");
}

void testFullyConnectedThreeLightGraphUsesThreeLayers()
{
  const SoftShadowLayerBuildResult result = buildSoftShadowLayerAssignment({point(0.0f, 0.0f, 0.0f, 4.0f), point(1.0f, 0.0f, 0.0f, 4.0f), point(0.0f, 1.0f, 0.0f, 4.0f)}, 4u);
  require(result.activeLayerCount == 3u, "triangle overlap should use three layers");
  require(result.maxGraphDegree == 2u, "triangle max degree should be two");
}

void testDirectionalLightGetsDedicatedLayer()
{
  const SoftShadowLayerBuildResult result = buildSoftShadowLayerAssignment({directional(), point(10.0f, 0.0f, 0.0f, 1.0f), point(14.0f, 0.0f, 0.0f, 1.0f)}, 4u);
  require(result.activeLayerCount == 2u, "directional plus separated locals should use two layers");
  require(result.assignments[0].layer != result.assignments[1].layer, "directional should not share with local light");
  require(result.assignments[1].layer == result.assignments[2].layer, "separated locals should still share a layer");
}

void testSpotLightsUseConservativeBounds()
{
  require(softShadowLightVolumesOverlap(spot(0.0f, 0.0f, 0.0f, 2.0f), spot(3.0f, 0.0f, 0.0f, 2.0f)), "spot conservative bounds should overlap");
  require(!softShadowLightVolumesOverlap(spot(0.0f, 0.0f, 0.0f, 1.0f), spot(4.0f, 0.0f, 0.0f, 1.0f)), "separated spot conservative bounds should not overlap");
}

void testMaxLayerOverflowMarksFallbackLights()
{
  const SoftShadowLayerBuildResult result = buildSoftShadowLayerAssignment({point(0.0f, 0.0f, 0.0f, 4.0f), point(1.0f, 0.0f, 0.0f, 4.0f), point(0.0f, 1.0f, 0.0f, 4.0f)}, 2u);
  require(result.activeLayerCount == 2u, "overflow case should use max available layers");
  require(result.overflowCount == 1u, "one triangle light should fall back with two max layers");
  require(result.assignments[2].fallback, "fallback choice should be deterministic for triangle input");
}

void testLayerAssignmentIsDeterministic()
{
  const std::vector<SoftShadowLightVolume> lights = {directional(), point(0.0f, 0.0f, 0.0f, 2.0f), point(3.0f, 0.0f, 0.0f, 2.0f), point(8.0f, 0.0f, 0.0f, 1.0f)};
  const SoftShadowLayerBuildResult a = buildSoftShadowLayerAssignment(lights, 4u);
  const SoftShadowLayerBuildResult b = buildSoftShadowLayerAssignment(lights, 4u);
  require(a.assignments.size() == b.assignments.size(), "deterministic assignment size mismatch");
  for (size_t i = 0; i < a.assignments.size(); ++i)
  {
    require(a.assignments[i].layer == b.assignments[i].layer, "deterministic assignment layer mismatch");
    require(a.assignments[i].fallback == b.assignments[i].fallback, "deterministic assignment fallback mismatch");
  }
}

void testSettingsSanitization()
{
  require(sanitizeScreenSpaceSoftShadowResolutionScale(3u) == 2u, "invalid resolution scale should sanitize to default");
  require(sanitizeScreenSpaceSoftShadowResolutionScale(4u) == 4u, "valid resolution scale should be preserved");
  require(sanitizeScreenSpaceSoftShadowKernelSize(9u) == 11u, "invalid kernel size should sanitize to default");
  require(sanitizeScreenSpaceSoftShadowKernelSize(23u) == 23u, "valid kernel size should be preserved");
  require(sanitizeScreenSpaceSoftShadowBlockerSearchTaps(4u) == 5u, "even blocker taps should round up");
  require(scaledScreenSpaceSoftShadowExtent(1919u, 2u) == 960u, "scaled odd extent should round up");
}

} // namespace

int main()
{
  try
  {
    testEmptyLightListProducesZeroLayers();
    testSeparatedPointLightsShareLayer();
    testTouchingPointLightsOverlap();
    testOverlappingPointLightsUseDifferentLayers();
    testOverlapChainUsesTwoLayers();
    testFullyConnectedThreeLightGraphUsesThreeLayers();
    testDirectionalLightGetsDedicatedLayer();
    testSpotLightsUseConservativeBounds();
    testMaxLayerOverflowMarksFallbackLights();
    testLayerAssignmentIsDeterministic();
    testSettingsSanitization();
  }
  catch (const std::exception &error)
  {
    std::cerr << "VirtualShadowSoftShadowLayerTests failed: " << error.what() << std::endl;
    return EXIT_FAILURE;
  }

  std::cout << "VirtualShadowSoftShadowLayerTests passed" << std::endl;
  return EXIT_SUCCESS;
}
