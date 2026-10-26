#include "voxel/svraster/SparseVoxelTree64Directory.hpp"
#include "voxel/svraster/SparseVoxelTree64Dynamic.hpp"

#include <array>
// These tests use assert() as their check mechanism, and Release builds
// define NDEBUG. Without this the assertions vanish - and because many of
// them wrap the call they are checking, the call under test vanishes with
// them, leaving objects unconfigured and the suite crashing or silently
// passing. cassert re-reads NDEBUG on every include, so undefining it here
// restores assert regardless of what included cassert earlier.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace
{

using namespace voxel::svraster;

std::array<float, 16> inverseTranslation(const float x, const float y, const float z)
{
  return {
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    -x,
    -y,
    -z,
    1.0f,
  };
}

SparseVoxelTree64StateGPU state(const uint32_t chunkSide = 64u)
{
  SparseVoxelTree64StateGPU result{};
  result.logicalChunkResolution = {9u, 9u, 9u};
  result.chunkSide = chunkSide;
  result.originWorldChunk = {-4, -4, -4};
  result.valid = 1u;
  result.wrapOffset = {3u, 7u, 2u};
  result.contentGeneration = 9u;
  result.originCell = {
    -4 * static_cast<int32_t>(chunkSide),
    -4 * static_cast<int32_t>(chunkSide),
    -4 * static_cast<int32_t>(chunkSide),
  };
  result.logicalCellResolution = {8u * chunkSide, 8u * chunkSide, 8u * chunkSide};
  result.cellSize = {1.0f, 0.1f, 1.25f};
  result.worldExtent = {
    static_cast<float>(result.logicalCellResolution[0]) * result.cellSize[0],
    static_cast<float>(result.logicalCellResolution[1]) * result.cellSize[1],
    static_cast<float>(result.logicalCellResolution[2]) * result.cellSize[2],
  };
  return result;
}

SparseVoxelTree64DynamicSettings settings()
{
  return {
    4u,
    512u,
    729u,
    8192u,
    1024u,
    512u,
  };
}

SparseVoxelTree64DynamicModelSource model()
{
  return {
    31u,
    4u,
    {
      {0.0f, 0.0f, 0.0f},
      {4.0f, 4.0f, 4.0f},
    },
    {
      {
        {0.5f, 0.5f, 0.5f},
        {1.5f, 1.5f, 1.5f},
      },
      {
        {2.0f, 2.0f, 2.0f},
        {3.0f, 3.0f, 3.0f},
      },
    },
  };
}

SparseVoxelTree64DynamicInstanceSource instance(const uint32_t id, const std::array<float, 3> &minimum, const std::array<float, 3> &extent)
{
  return {
    id,
    31u,
    inverseTranslation(minimum[0], minimum[1], minimum[2]),
    {
      minimum,
      {
        minimum[0] + extent[0],
        minimum[1] + extent[1],
        minimum[2] + extent[2],
      },
    },
  };
}

template <typename T> bool byteEqual(const std::vector<T> &left, const std::vector<T> &right)
{
  return left.size() == right.size() && (left.empty() || std::memcmp(left.data(), right.data(), left.size() * sizeof(T)) == 0);
}

void assertSameSnapshot(const SparseVoxelTree64DynamicSnapshot &optimized, const SparseVoxelTree64DynamicSnapshot &reference)
{
  assert(optimized.contentGeneration == reference.contentGeneration);
  assert(optimized.sceneRevision == reference.sceneRevision);
  assert(optimized.conservativeFallback == reference.conservativeFallback);
  assert(byteEqual(optimized.models, reference.models));
  assert(byteEqual(optimized.nodes, reference.nodes));
  assert(byteEqual(optimized.instances, reference.instances));
  assert(byteEqual(optimized.memberships, reference.memberships));
  assert(byteEqual(optimized.references, reference.references));
}

std::vector<SparseVoxelTree64DynamicInstanceSource> fixtureInstances(const uint32_t count)
{
  std::vector<SparseVoxelTree64DynamicInstanceSource> result;
  result.reserve(count);
  std::mt19937 random(0x6438u + count);
  std::uniform_real_distribution<float> x(-250.0f, 300.0f);
  std::uniform_real_distribution<float> y(-25.0f, 28.0f);
  std::uniform_real_distribution<float> z(-315.0f, 370.0f);
  std::uniform_real_distribution<float> extent(0.125f, 12.0f);
  for (uint32_t index = 0u; index < count; ++index)
  {
    std::array<float, 3> minimum{x(random), y(random), z(random)};
    if (index % 8u == 0u)
    {
      // Exact boundaries exercise the closed-AABB rule: an instance beginning
      // on a boundary also belongs to the chunk ending at that boundary.
      minimum = {
        static_cast<float>(static_cast<int32_t>(index % 7u) - 3) * 64.0f,
        static_cast<float>(static_cast<int32_t>(index % 5u) - 2) * 6.4f,
        static_cast<float>(static_cast<int32_t>(index % 9u) - 4) * 80.0f,
      };
    }
    result.push_back(instance(index + 1u, minimum, {extent(random), extent(random), extent(random)}));
  }
  return result;
}

void testRangeMembershipParity()
{
  const std::vector models{model()};
  std::string error;
  for (const uint32_t chunkSide : {16u, 64u})
    for (const uint32_t count : {0u, 1u, 16u, 257u})
    {
      const auto instances = fixtureInstances(count);
      SparseVoxelTree64DynamicSnapshot optimized;
      SparseVoxelTree64DynamicSnapshot reference;
      assert(sparseVoxelTree64BuildDynamicSnapshot(state(chunkSide), settings(), 11u, 23u, models, instances, optimized, &error));
      assert(sparseVoxelTree64BuildDynamicSnapshotBruteForceReference(state(chunkSide), settings(), 11u, 23u, models, instances, reference, &error));
      assertSameSnapshot(optimized, reference);
    }
}

void benchmarkRangeMembership()
{
  const auto active = state();
  const std::vector models{model()};
  constexpr uint32_t iterations = 20u;
  std::string error;
  for (const uint32_t count : {1u, 16u, 64u, 256u})
  {
    const auto instances = fixtureInstances(count);
    SparseVoxelTree64DynamicSnapshot optimized;
    SparseVoxelTree64DynamicSnapshot reference;
    assert(sparseVoxelTree64BuildDynamicSnapshot(active, settings(), 11u, 23u, models, instances, optimized, &error));
    assert(sparseVoxelTree64BuildDynamicSnapshotBruteForceReference(active, settings(), 11u, 23u, models, instances, reference, &error));
    assertSameSnapshot(optimized, reference);

    const auto optimizedBegin = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0u; iteration < iterations; ++iteration)
      assert(sparseVoxelTree64BuildDynamicSnapshot(active, settings(), 11u, 23u, models, instances, optimized, &error));
    const auto optimizedMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - optimizedBegin).count();

    const auto referenceBegin = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0u; iteration < iterations; ++iteration)
      assert(sparseVoxelTree64BuildDynamicSnapshotBruteForceReference(active, settings(), 11u, 23u, models, instances, reference, &error));
    const auto referenceMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - referenceBegin).count();
    assertSameSnapshot(optimized, reference);

    const double speedup = optimizedMicroseconds == 0 ? 0.0 : static_cast<double>(referenceMicroseconds) / optimizedMicroseconds;
    std::cout << "TREE64_T8_CPU specialization=range_membership"
              << " instances=" << count << " active_chunks=729"
              << " references=" << optimized.references.size() << " iterations=" << iterations << " optimized_elapsed_us=" << optimizedMicroseconds << " brute_force_elapsed_us=" << referenceMicroseconds
              << " speedup=" << speedup << '\n';
  }
}

void benchmarkChunkLayouts()
{
  constexpr uint32_t iterations = 20u;
  for (const uint32_t chunkSide : {16u, 64u})
  {
    SparseVoxelTree64DirectorySettings layout{
      {512u, 512u, 512u},
      chunkSide,
      {1.0f, 1.0f, 1.0f},
      sparseVoxelTree64MaximumNodeCount(chunkSide),
      100000u,
    };
    SparseVoxelTree64DirectoryUpdate initial;
    std::string error;
    assert(prepareSparseVoxelTree64DirectoryUpdate(layout, nullptr, {0.25f, 0.25f, 0.25f}, initial, &error));
    const auto committed = initial.targetState;
    SparseVoxelTree64DirectoryUpdate scroll;
    const auto begin = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0u; iteration < iterations; ++iteration)
      assert(prepareSparseVoxelTree64DirectoryUpdate(layout, &committed, {static_cast<float>(chunkSide) + 0.25f, 0.25f, 0.25f}, scroll, &error));
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
    const uint64_t directoryEntries = uint64_t{committed.logicalChunkResolution[0]} * committed.logicalChunkResolution[1] * committed.logicalChunkResolution[2];
    const uint64_t fixedNodePoolBytes = directoryEntries * layout.nodeSlotCapacity * sizeof(SparseVoxelTree64NodeGPU);
    const uint64_t descriptorBytes = directoryEntries * sizeof(SparseVoxelTree64ChunkGPU);
    std::cout << "TREE64_T8_CPU specialization=chunk_side"
              << " side=" << chunkSide << " logical_cells=512"
              << " directory_entries=" << directoryEntries << " entering_chunks_per_axis_scroll=" << scroll.enteringChunks.size() << " descriptor_bytes=" << descriptorBytes
              << " fixed_node_pool_bytes=" << fixedNodePoolBytes << " iterations=" << iterations << " planner_elapsed_us=" << elapsed << '\n';
  }
}

} // namespace

int main()
{
  testRangeMembershipParity();
  benchmarkRangeMembership();
  benchmarkChunkLayouts();
  std::cout << "Sparse voxel Tree64 T8 specialization CPU tests passed\n";
  return 0;
}
