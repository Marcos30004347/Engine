#include "voxel/svraster/SparseVoxelOccupancyDDA.hpp"

#include <array>
// These tests use assert() as their check mechanism, and Release builds
// define NDEBUG. Without this the assertions vanish - and because many of
// them wrap the call they are checking, the call under test vanishes with
// them, leaving objects unconfigured and the suite crashing or silently
// passing. cassert re-reads NDEBUG on every include, so undefining it here
// restores assert regardless of what included cassert earlier.
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace voxel::svraster;

SparseVoxelOccupancySettings settings(
    const std::array<uint32_t, 3> resolution)
{
  SparseVoxelOccupancySettings value{};
  value.logicalResolution = resolution;
  value.worldExtent = {
    static_cast<float>(resolution[0]),
    static_cast<float>(resolution[1]),
    static_cast<float>(resolution[2]),
  };
  value.memoryCeilingBytes = 1u << 24u;
  return value;
}

void assertEquivalent(
    const SparseVoxelOccupancyVolume &volume,
    const SparseVoxelOccupancyRay &ray)
{
  const SparseVoxelOccupancyTraceResult scalar =
      traceSparseVoxelOccupancyMip0(volume, ray);
  const SparseVoxelOccupancyTraceResult hierarchy =
      traceSparseVoxelOccupancyHierarchy(volume, ray);
  assert(scalar.status != SparseVoxelOccupancyTraceStatus::Invalid);
  assert(scalar.status != SparseVoxelOccupancyTraceStatus::QueryLimit);
  assert(hierarchy.status != SparseVoxelOccupancyTraceStatus::Invalid);
  assert(hierarchy.status != SparseVoxelOccupancyTraceStatus::QueryLimit);
  assert(scalar.hit() == hierarchy.hit());
  if (scalar.hit())
  {
    assert(scalar.globalCell == hierarchy.globalCell);
    assert(volume.occupiedAtGlobalCell(0u, scalar.globalCell));
    assert(std::abs(scalar.distance - hierarchy.distance) < 1.0e-9);
  }
}

void testAnalyticRays()
{
  SparseVoxelOccupancyVolume volume;
  std::string error;
  assert(volume.configure(settings({16u, 16u, 16u}), {}, &error));
  const SparseVoxelAABB occupied{{2.1f, 2.1f, 2.1f}, {2.9f, 2.9f, 2.9f}};
  assert(volume.update({0.25f, 0.25f, 0.25f}, 1u, {occupied}, &error));

  const std::vector<SparseVoxelOccupancyRay> rays{
    {{-20.0f, 2.5f, 2.5f}, {1.0f, 0.0f, 0.0f}},
    {{20.0f, 2.5f, 2.5f}, {-1.0f, 0.0f, 0.0f}},
    {{2.5f, 2.5f, 2.5f}, {1.0f, 0.0f, 0.0f}},
    {{3.0f, 2.5f, 2.5f}, {-1.0f, 0.0f, 0.0f}},
    {{2.0f, 2.5f, 2.5f}, {1.0f, 0.0f, 0.0f}},
    {{-20.0f, -20.0f, -20.0f}, {1.0f, 1.0f, 1.0f}},
    {{-20.0f, 5.5f, 5.5f}, {1.0f, 0.0f, 0.0f}},
    {{-20.0f, 2.5f, 2.5f}, {1.0f, 0.0f, 0.0f}, 10.0},
    {{8.0f, 2.5f, 2.5f}, {-1.0f, 0.0f, 0.0f}},
  };
  for (const SparseVoxelOccupancyRay &ray : rays)
    assertEquivalent(volume, ray);

  const auto forward =
      traceSparseVoxelOccupancyHierarchy(volume, rays[0]);
  assert(forward.hit());
  assert(forward.globalCell == (std::array<int32_t, 3>{2, 2, 2}));
  const auto backward =
      traceSparseVoxelOccupancyHierarchy(volume, rays[1]);
  assert(backward.hit());
  assert(backward.globalCell == (std::array<int32_t, 3>{2, 2, 2}));
  const auto occupiedStart =
      traceSparseVoxelOccupancyHierarchy(volume, rays[2]);
  assert(occupiedStart.hit() && occupiedStart.distance == 0.0);
  assert(!traceSparseVoxelOccupancyHierarchy(volume, rays[6]).hit());
  assert(!traceSparseVoxelOccupancyHierarchy(volume, rays[7]).hit());

  SparseVoxelOccupancyRay invalid = rays[0];
  invalid.direction = {0.0f, 0.0f, 0.0f};
  assert(
      traceSparseVoxelOccupancyHierarchy(volume, invalid).status ==
      SparseVoxelOccupancyTraceStatus::Invalid);
}

uint32_t nextRandom(uint32_t &state)
{
  state = state * 1664525u + 1013904223u;
  return state;
}

float randomRange(uint32_t &state, const float minimum, const float maximum)
{
  const float unit =
      static_cast<float>(nextRandom(state) >> 8u) /
      static_cast<float>(0x00ffffffu);
  return minimum + (maximum - minimum) * unit;
}

void testRandomizedToroidalAndInteriorEquivalence()
{
  SparseVoxelOccupancyVolume volume;
  std::string error;
  assert(volume.configure(settings({19u, 13u, 11u}), {}, &error));

  uint32_t random = 0x9e3779b9u;
  std::vector<SparseVoxelAABB> sources;
  for (uint32_t sourceIndex = 0u; sourceIndex < 18u; ++sourceIndex)
  {
    std::array<float, 3> center{};
    std::array<float, 3> half{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      center[axis] = randomRange(random, -12.0f, 12.0f);
      half[axis] = randomRange(random, 0.1f, 1.4f);
    }
    sources.push_back({
      {center[0] - half[0], center[1] - half[1], center[2] - half[2]},
      {center[0] + half[0], center[1] + half[1], center[2] + half[2]},
    });
  }

  const std::array<std::array<float, 3>, 6> cameraWalk{{
    {0.25f, 0.25f, 0.25f},
    {1.25f, -0.75f, 0.25f},
    {-4.25f, 2.25f, -1.75f},
    {8.25f, -3.75f, 2.25f},
    {-9.25f, 4.25f, -3.75f},
    {2.25f, -1.75f, 1.25f},
  }};

  for (uint32_t cameraIndex = 0u; cameraIndex < cameraWalk.size(); ++cameraIndex)
  {
    assert(volume.update(cameraWalk[cameraIndex], 1u, sources, &error));
    for (uint32_t rayIndex = 0u; rayIndex < 300u; ++rayIndex)
    {
      SparseVoxelOccupancyRay ray{};
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        ray.origin[axis] = randomRange(random, -25.0f, 25.0f);
        ray.direction[axis] = randomRange(random, -1.0f, 1.0f);
      }
      if ((rayIndex % 7u) == 0u)
        ray.direction[rayIndex % 3u] = 0.0f;
      if (ray.direction == (std::array<float, 3>{0.0f, 0.0f, 0.0f}))
        ray.direction[0] = 1.0f;
      ray.maximumDistance = randomRange(random, 0.0f, 80.0f);
      assertEquivalent(volume, ray);
    }
  }

  const SparseVoxelAABB oldBounds = sources.front();
  SparseVoxelAABB newBounds = oldBounds;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    newBounds.minimum[axis] += 3.25f;
    newBounds.maximum[axis] += 3.25f;
  }
  sources.front() = newBounds;
  assert(
      volume.update(
          cameraWalk.back(),
          2u,
          sources,
          {oldBounds, newBounds},
          &error));
  assert(volume.state().fullRebuild == 0u);
  for (uint32_t rayIndex = 0u; rayIndex < 500u; ++rayIndex)
  {
    SparseVoxelOccupancyRay ray{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      ray.origin[axis] = randomRange(random, -25.0f, 25.0f);
      ray.direction[axis] = randomRange(random, -1.0f, 1.0f);
    }
    if (ray.direction == (std::array<float, 3>{0.0f, 0.0f, 0.0f}))
      ray.direction[2] = 1.0f;
    assertEquivalent(volume, ray);
  }
}

void testSparseSkippingAndQueryLimit()
{
  SparseVoxelOccupancyVolume empty;
  std::string error;
  assert(empty.configure(settings({64u, 32u, 16u}), {}, &error));
  assert(empty.update({0.25f, 0.25f, 0.25f}, 1u, {}, &error));
  const SparseVoxelOccupancyRay ray{
    {-40.0f, 0.5f, 0.5f},
    {1.0f, 0.0f, 0.0f},
  };
  const auto scalar = traceSparseVoxelOccupancyMip0(empty, ray);
  const auto hierarchy = traceSparseVoxelOccupancyHierarchy(empty, ray);
  assert(scalar.status == SparseVoxelOccupancyTraceStatus::Miss);
  assert(hierarchy.status == SparseVoxelOccupancyTraceStatus::Miss);
  assert(hierarchy.queryCount < scalar.queryCount);
  assert(hierarchy.skippedDistance > 0.0);
  assert(hierarchy.highestLevel > 0u);
  assert(
      traceSparseVoxelOccupancyMip0(empty, ray, 1u).status ==
      SparseVoxelOccupancyTraceStatus::QueryLimit);
}

} // namespace

int main()
{
  testAnalyticRays();
  testRandomizedToroidalAndInteriorEquivalence();
  testSparseSkippingAndQueryLimit();
  std::cout << "Sparse voxel occupancy G29 CPU DDA tests passed\n";
  return 0;
}
