#include "globalIllumination/worldgi/WorldGITypes.hpp"

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

int main()
{
  using namespace globalillumination::worldgi;
  assert(WorldGILightType_Directional == 1u);
  assert(WorldGILightType_Point == 2u);
  assert(WorldGILightType_Sphere == 6u);
  assert(WorldGILightType_Planar == 7u);
  assert(WorldGILightType_Sky == 8u);
  assert(sizeof(WorldGILightGPU) == 80u);
  assert(offsetof(WorldGILightGPU, shapeParams) == 32u);
  assert(offsetof(WorldGILightGPU, colorIntensity) == 48u);
  assert(offsetof(WorldGILightGPU, flags) == 64u);
  assert(sizeof(WorldGILightBVHNodeGPU) == 48u);
  assert(sizeof(WorldGILightBVHInfoGPU) == 16u);
  WorldGILightGPU light{};
  // flags.z/w are the ABI-reserved stable handle and revision fields.
  light.flags[2] = 42u;
  light.flags[3] = 9u;
  light.shapeParams[0] = 0.5f;
  light.shapeParams[1] = 2.0f;
  light.shapeParams[2] = 1.0f;
  assert(light.flags[2] == 42u && light.flags[3] == 9u);
  assert(light.shapeParams[0] == 0.5f && light.shapeParams[1] == 2.0f && light.shapeParams[2] == 1.0f);
  assert(worldGILightBVHNodeCapacity(4u) == 7u);
  std::cout << "WorldGILightBVHContractTests: passed\n";
  return 0;
}
