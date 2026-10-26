#include "voxel/svraster/SparseVoxelTree64Directory.hpp"
#include "voxel/svraster/SparseVoxelTree64Scene.hpp"

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
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace
{

using namespace voxel::svraster;

constexpr uint32_t kSide = 4u;
constexpr uint32_t kAxis = 3u;
constexpr uint32_t kChunkCount = kAxis * kAxis * kAxis;

SparseVoxelTree64DirectorySettings settings()
{
  return {
    {8u, 8u, 8u},
    kSide,
    {1.0f, 1.0f, 1.0f},
    sparseVoxelTree64MaximumNodeCount(kSide),
    2u * kChunkCount,
  };
}

std::vector<SparseVoxelTree64BuildResultGPU> results(const SparseVoxelTree64DirectoryUpdate &update)
{
  std::vector<SparseVoxelTree64BuildResultGPU> value;
  for (const auto &job : update.buildJobs)
    value.push_back({
      kSparseVoxelTree64BuildSuccess,
      1u,
      0u,
      job.nodeSlot,
      job.contentGeneration,
      0u,
      1u,
      0u,
    });
  return value;
}

std::vector<uint8_t> occupied(const SparseVoxelTree64DirectoryUpdate &update)
{
  return std::vector<uint8_t>(update.buildJobs.size(), 1u);
}

uint32_t physicalIndex(const SparseVoxelTree64StateGPU &state, const std::array<int32_t, 3> &chunk)
{
  std::array<uint32_t, 3> slot{};
  assert(sparseVoxelTree64DirectorySlot(chunk, state.originWorldChunk, state.logicalChunkResolution, state.wrapOffset, slot));
  return slot[0] + state.logicalChunkResolution[0] * (slot[1] + state.logicalChunkResolution[1] * slot[2]);
}

void testClassificationAndSceneTransactions()
{
  std::string error;
  SparseVoxelTree64Directory directory;
  assert(directory.configure(settings(), &error));

  SparseVoxelTree64DirectoryUpdate initial{};
  assert(directory.beginSceneUpdate({0.25f, 0.25f, 0.25f}, 1u, {}, true, initial, &error));
  assert(initial.buildChunks.size() == kChunkCount);
  assert(directory.commitPendingScene(1u, results(initial), occupied(initial), 0u, &error));
  assert(directory.sceneRevision() == 1u);
  const auto before = directory.directory();

  std::vector<std::array<int32_t, 3>> dirty;
  SparseVoxelTree64SceneClassificationStats classification{};
  assert(sparseVoxelTree64ClassifyDirtyChunks(directory.state(), {{{0.25f, 0.25f, 0.25f}, {0.75f, 0.75f, 0.75f}}}, true, dirty, &classification, &error));
  assert(dirty == (std::vector<std::array<int32_t, 3>>{{0, 0, 0}}));
  assert(classification.dirtyChunks == 1u);
  assert(!classification.fullActiveRebuild);

  SparseVoxelTree64DirectoryUpdate add{};
  assert(directory.beginSceneUpdate({0.25f, 0.25f, 0.25f}, 2u, dirty, true, add, &error));
  assert(!add.metadataOnly);
  assert(add.enteringChunks.empty());
  assert(add.rebuiltChunks == dirty);
  assert(add.buildChunks == dirty);
  const uint32_t dirtyIndex = physicalIndex(directory.state(), dirty.front());
  const uint32_t oldSlot = before[dirtyIndex].nodeSlot;
  assert(add.buildJobs.front().nodeSlot != oldSlot);

  // A completion built from revision 1 cannot publish into revision 2.
  assert(!directory.commitPendingScene(1u, results(add), occupied(add), 1u, &error));
  assert(directory.hasPendingUpdate());
  assert(directory.sceneRevision() == 1u);
  assert(directory.commitPendingScene(2u, results(add), occupied(add), 1u, &error));
  assert(directory.sceneRevision() == 2u);
  assert(directory.directory()[dirtyIndex].nodeSlot != oldSlot);
  for (uint32_t index = 0u; index < directory.directory().size(); ++index)
    if (index != dirtyIndex)
      assert(std::memcmp(&before[index], &directory.directory()[index], sizeof(SparseVoxelTree64ChunkGPU)) == 0);

  const uint32_t generation = directory.state().contentGeneration;
  SparseVoxelTree64DirectoryUpdate outside{};
  assert(directory.beginSceneUpdate({0.25f, 0.25f, 0.25f}, 3u, {{100, 100, 100}}, true, outside, &error));
  assert(outside.metadataOnly);
  assert(outside.buildChunks.empty());
  assert(!directory.hasPendingUpdate());
  assert(directory.sceneRevision() == 3u);
  assert(directory.state().contentGeneration == generation);
  assert(directory.reclaimCompleted(1u, &error));

  SparseVoxelTree64DirectoryUpdate missingBounds{};
  assert(directory.beginSceneUpdate({0.25f, 0.25f, 0.25f}, 4u, {}, false, missingBounds, &error));
  assert(missingBounds.fullSceneRebuild);
  assert(missingBounds.buildChunks.size() == kChunkCount);
  assert(directory.abortPending(2u, &error));
  assert(directory.reclaimCompleted(2u, &error));

  SparseVoxelTree64DirectoryUpdate sameRevisionMissingBounds{};
  assert(directory.beginSceneUpdate({0.25f, 0.25f, 0.25f}, 3u, {}, false, sameRevisionMissingBounds, &error));
  assert(sameRevisionMissingBounds.fullSceneRebuild);
  assert(sameRevisionMissingBounds.buildChunks.size() == kChunkCount);
  assert(directory.abortPending(3u, &error));
}

void testPageInstallEvictionAndFreshReference()
{
  std::string error;
  SparseVoxelTree64StateGPU state{};
  assert(makeSparseVoxelTree64DirectoryState(settings(), {0.25f, 0.25f, 0.25f}, state, &error));
  state.valid = 1u;

  // One installed page spans the x=0/x=1 chunk boundary.
  const SparseVoxelAABB page{
    {3.25f, 1.25f, 1.25f},
    {4.75f, 2.75f, 2.75f},
  };
  std::vector<std::array<int32_t, 3>> dirty;
  assert(sparseVoxelTree64ClassifyDirtyChunks(state, {page}, true, dirty, nullptr, &error));
  assert(
      dirty == (std::vector<std::array<int32_t, 3>>{
                 {0, 0, 0},
                 {1, 0, 0},
               }));

  // Evicting page A while overlapping page B remains resident reconstructs
  // both chunks from B; it never subtracts A's cells in place.
  const SparseVoxelAABB retained{
    {3.5f, 1.5f, 1.5f},
    {4.5f, 2.5f, 2.5f},
  };
  std::vector<SparseVoxelTree64BuildSourceGPU> sources;
  SparseVoxelTree64SceneSourceStats sourceStats{};
  assert(sparseVoxelTree64MakeBuildSources(state, dirty, {retained}, sources, &sourceStats, &error));
  assert(sources.size() == 2u);
  assert(sourceStats.emittedChunkLeaves == 2u);

  for (uint32_t job = 0u; job < dirty.size(); ++job)
  {
    std::vector<SparseVoxelTree64AABB> local;
    for (const auto &source : sources)
      if (source.jobIndex == job)
        local.push_back({
          {source.minimum[0], source.minimum[1], source.minimum[2]},
          {source.maximum[0], source.maximum[1], source.maximum[2]},
        });
    SparseVoxelTree64Chunk incremental{};
    SparseVoxelTree64Chunk fresh{};
    assert(buildSparseVoxelTree64(kSide, local, incremental, &error));
    assert(buildSparseVoxelTree64(kSide, local, fresh, &error));
    assert(incremental.nodes.size() == fresh.nodes.size());
    assert(std::memcmp(incremental.nodes.data(), fresh.nodes.data(), fresh.nodes.size() * sizeof(SparseVoxelTree64NodeGPU)) == 0);
  }

  // A selective install wholly outside the active directory schedules no
  // work, while incomplete eviction metadata selects every active chunk.
  assert(sparseVoxelTree64ClassifyDirtyChunks(state, {{{100.0f, 100.0f, 100.0f}, {101.0f, 101.0f, 101.0f}}}, true, dirty, nullptr, &error));
  assert(dirty.empty());
  assert(sparseVoxelTree64ClassifyDirtyChunks(state, {}, false, dirty, nullptr, &error));
  assert(dirty.size() == kChunkCount);
}

void benchmarkClassification()
{
  std::string error;
  SparseVoxelTree64StateGPU state{};
  assert(makeSparseVoxelTree64DirectoryState(settings(), {0.25f, 0.25f, 0.25f}, state, &error));
  state.valid = 1u;
  const std::vector<SparseVoxelAABB> bounds{
    {{-2.0f, -2.0f, -2.0f}, {1.0f, 1.0f, 1.0f}},
    {{3.5f, 3.5f, 3.5f}, {6.5f, 6.5f, 6.5f}},
  };
  std::vector<std::array<int32_t, 3>> dirty;
  constexpr uint32_t iterations = 2000u;
  const auto begin = std::chrono::steady_clock::now();
  for (uint32_t iteration = 0u; iteration < iterations; ++iteration)
    assert(sparseVoxelTree64ClassifyDirtyChunks(state, bounds, true, dirty, nullptr, &error));
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
  std::cout << "TREE64_T5_BENCH classification_iterations=" << iterations << " active_chunks=" << kChunkCount << " dirty_chunks=" << dirty.size() << " elapsed_us=" << elapsed << '\n';
}

} // namespace

int main()
{
  testClassificationAndSceneTransactions();
  testPageInstallEvictionAndFreshReference();
  benchmarkClassification();
  std::cout << "Sparse voxel Tree64 T5 scene incremental CPU tests passed\n";
  return 0;
}
