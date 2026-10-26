#include "voxel/svraster/SparseVoxelGenerator.hpp"
#include "voxel/svraster/SparseVoxelTree64Transforms.hpp"

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
#include <string>
#include <vector>

namespace
{

using namespace voxel::svraster;

std::array<float, 16> transform(const float tx, const float ty, const float tz, const float sx = 1.0f, const float sy = 1.0f, const float sz = 1.0f)
{
  return {
    sx,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    sy,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    sz,
    0.0f,
    tx,
    ty,
    tz,
    1.0f,
  };
}

std::array<float, 16> rotateZ90(const float tx = 0.0f, const float ty = 0.0f, const float tz = 0.0f)
{
  return {
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    -1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    tx,
    ty,
    tz,
    1.0f,
  };
}

SparseVoxelTree64InstanceTransformRecord record(const uint32_t id, const std::array<float, 16> &matrix, const SparseVoxelAABB &bounds)
{
  return {id, 0u, matrix, bounds};
}

SparseVoxelTree64DirectorySettings settings(const uint32_t logicalResolution = 24u, const uint32_t side = 4u, const uint32_t slotMultiplier = 2u)
{
  const uint32_t axis = (logicalResolution + side - 1u) / side + 1u;
  return {
    {logicalResolution, logicalResolution, logicalResolution},
    side,
    {1.0f, 1.0f, 1.0f},
    sparseVoxelTree64MaximumNodeCount(side),
    slotMultiplier * axis * axis * axis,
  };
}

SparseVoxelTree64StateGPU state(const uint32_t logicalResolution = 24u, const uint32_t side = 4u)
{
  SparseVoxelTree64StateGPU result{};
  std::string error;
  assert(makeSparseVoxelTree64DirectoryState(settings(logicalResolution, side), {0.25f, 0.25f, 0.25f}, result, &error));
  result.valid = 1u;
  return result;
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

void testTransformClassification()
{
  const auto active = state();
  std::string error;
  const SparseVoxelAABB original{
    {0.25f, 0.25f, 0.25f},
    {1.75f, 1.75f, 1.75f},
  };
  const auto identity = transform(0.0f, 0.0f, 0.0f);
  SparseVoxelTree64TransformUpdate update{};
  assert(sparseVoxelTree64PlanTransformInvalidation(active, {record(7u, identity, original)}, {record(7u, identity, original)}, false, true, update, &error));
  assert(update.changedInstanceIds.empty());
  assert(update.dirtyChunks.empty());

  const SparseVoxelAABB translated{
    {4.25f, 0.25f, 0.25f},
    {5.75f, 1.75f, 1.75f},
  };
  update = {};
  assert(sparseVoxelTree64PlanTransformInvalidation(active, {record(7u, identity, original)}, {record(7u, transform(4.0f, 0.0f, 0.0f), translated)}, false, true, update, &error));
  assert(update.changedInstanceIds == std::vector<uint32_t>{7u});
  assert(update.invalidatedWorldBounds.size() == 2u);
  assert(
      update.dirtyChunks == (std::vector<std::array<int32_t, 3>>{
                              {0, 0, 0},
                              {1, 0, 0},
                            }));

  // A symmetric root AABB can remain identical under rotation or reflection;
  // matrix identity still marks it dirty.
  update = {};
  assert(sparseVoxelTree64PlanTransformInvalidation(active, {record(7u, identity, original)}, {record(7u, rotateZ90(), original)}, false, true, update, &error));
  assert(update.changedInstanceIds.size() == 1u);
  assert(update.dirtyChunks.size() == 1u);

  update = {};
  assert(sparseVoxelTree64PlanTransformInvalidation(active, {record(7u, identity, original)}, {record(7u, transform(0.0f, 0.0f, 0.0f, -1.0f), original)}, false, true, update, &error));
  assert(update.changedInstanceIds.size() == 1u);

  const SparseVoxelAABB scaled{
    {-2.0f, -0.5f, -4.0f},
    {2.0f, 0.5f, 4.0f},
  };
  update = {};
  assert(sparseVoxelTree64PlanTransformInvalidation(active, {record(7u, identity, original)}, {record(7u, transform(0.0f, 0.0f, 0.0f, 2.0f, 0.5f, 4.0f), scaled)}, false, true, update, &error));
  assert(update.dirtyChunks.size() > 1u);

  const SparseVoxelAABB outsideOld{
    {100.25f, 100.25f, 100.25f},
    {101.25f, 101.25f, 101.25f},
  };
  const SparseVoxelAABB outsideNew{
    {104.25f, 100.25f, 100.25f},
    {105.25f, 101.25f, 101.25f},
  };
  update = {};
  assert(sparseVoxelTree64PlanTransformInvalidation(active, {record(7u, identity, outsideOld)}, {record(7u, transform(4.0f, 0.0f, 0.0f), outsideNew)}, false, true, update, &error));
  assert(update.changedInstanceIds.size() == 1u);
  assert(update.dirtyChunks.empty());

  // Crossing the active-volume edge rebuilds the old in-range chunk, but the
  // new out-of-range bound does not schedule unrelated work.
  const SparseVoxelAABB insideEdge{
    {14.25f, 0.25f, 0.25f},
    {15.25f, 1.25f, 1.25f},
  };
  const SparseVoxelAABB outsideEdge{
    {16.25f, 0.25f, 0.25f},
    {17.25f, 1.25f, 1.25f},
  };
  update = {};
  assert(sparseVoxelTree64PlanTransformInvalidation(active, {record(7u, identity, insideEdge)}, {record(7u, transform(2.0f, 0.0f, 0.0f), outsideEdge)}, false, true, update, &error));
  assert(
      update.dirtyChunks == (std::vector<std::array<int32_t, 3>>{
                              {3, 0, 0},
                            }));

  update = {};
  assert(sparseVoxelTree64PlanTransformInvalidation(active, {}, {}, false, false, update, &error));
  assert(update.fullActiveRebuild);
  assert(update.dirtyChunks.size() == active.logicalChunkResolution[0] * active.logicalChunkResolution[1] * active.logicalChunkResolution[2]);
}

void testOverlapRemovalMatchesFresh()
{
  const auto active = state();
  std::string error;
  const auto identity = transform(0.0f, 0.0f, 0.0f);
  const SparseVoxelAABB overlap{
    {1.25f, 1.25f, 1.25f},
    {2.75f, 2.75f, 2.75f},
  };
  const SparseVoxelAABB moved{
    {5.25f, 1.25f, 1.25f},
    {6.75f, 2.75f, 2.75f},
  };
  const std::vector previous{
    record(0u, identity, overlap),
    record(1u, identity, overlap),
  };
  const std::vector current{
    record(0u, transform(4.0f, 0.0f, 0.0f), moved),
    record(1u, identity, overlap),
  };
  SparseVoxelTree64TransformUpdate move{};
  assert(sparseVoxelTree64PlanTransformInvalidation(active, previous, current, false, true, move, &error));
  assert(move.changedInstanceIds == std::vector<uint32_t>{0u});
  assert(move.dirtyChunks.size() == 2u);

  std::vector<SparseVoxelTree64BuildSourceGPU> sources;
  assert(sparseVoxelTree64MakeBuildSources(active, move.dirtyChunks, {moved, overlap}, sources, nullptr, &error));
  bool oldChunkStillOccupied = false;
  for (const auto &source : sources)
    if (move.dirtyChunks[source.jobIndex] == std::array<int32_t, 3>{0, 0, 0})
      oldChunkStillOccupied = true;
  assert(oldChunkStillOccupied);

  // Remove the mover after its previous movement. Its last published moved
  // bound is invalidated, and the overlapping retained contributor remains.
  SparseVoxelTree64TransformUpdate removal{};
  assert(sparseVoxelTree64PlanTransformInvalidation(active, current, {record(1u, identity, overlap)}, false, true, removal, &error));
  assert(removal.changedInstanceIds == std::vector<uint32_t>{0u});
  assert(
      removal.dirtyChunks == (std::vector<std::array<int32_t, 3>>{
                               {1, 0, 0},
                             }));

  // Topology replacement has complete old/new root bounds and therefore need
  // not invalidate unrelated active chunks.
  SparseVoxelTree64TransformUpdate replacement{};
  assert(sparseVoxelTree64PlanTransformInvalidation(active, current, {record(0u, identity, overlap)}, true, true, replacement, &error));
  assert(!replacement.fullActiveRebuild);
  assert(replacement.dirtyChunks.size() == 2u);
}

void testTrackerDirectoryTransaction()
{
  std::string error;
  SparseVoxelScene scene = makeExteriorShellScene(0x716u);
  SparseVoxelSceneManager manager;
  assert(manager.setScene(scene, &error));

  SparseVoxelTree64Directory directory;
  assert(directory.configure(settings(8u, 4u, 3u), &error));
  SparseVoxelTree64DirectoryUpdate initial{};
  assert(directory.beginSceneUpdate({0.25f, 0.25f, 0.25f}, manager.sceneRevision(), {}, true, initial, &error));
  assert(directory.commitPendingScene(manager.sceneRevision(), results(initial), occupied(initial), 0u, &error));

  SparseVoxelTree64TransformTracker tracker;
  assert(tracker.initialize(manager, &error));
  assert(manager.setInstanceTransform(0u, transform(4.0f, 0.0f, 0.0f), &error));
  SparseVoxelTree64TransformUpdate first{};
  assert(tracker.prepare(manager, directory.state(), first, &error));
  assert(!first.dirtyChunks.empty());
  SparseVoxelTree64DirectoryUpdate rebuild{};
  assert(beginSparseVoxelTree64TransformRebuild(directory, {0.25f, 0.25f, 0.25f}, first, rebuild, &error));
  assert(rebuild.buildChunks == first.dirtyChunks);

  // A second move while the first build is in flight invalidates that
  // completion. Both trackers retain the last published transform.
  assert(manager.setInstanceTransform(0u, transform(8.0f, 0.0f, 0.0f), &error));
  assert(!directory.commitPendingScene(manager.sceneRevision(), results(rebuild), occupied(rebuild), 1u, &error));
  assert(!tracker.commit(manager, first, &error));
  assert(directory.abortPending(1u, &error));
  assert(tracker.abort(first, &error));
  assert(directory.reclaimCompleted(1u, &error));

  SparseVoxelTree64TransformUpdate latest{};
  assert(tracker.prepare(manager, directory.state(), latest, &error));
  assert(latest.invalidatedWorldBounds.size() == 2u);
  assert(beginSparseVoxelTree64TransformRebuild(directory, {0.25f, 0.25f, 0.25f}, latest, rebuild, &error));
  assert(directory.commitPendingScene(manager.sceneRevision(), results(rebuild), occupied(rebuild), 2u, &error));
  assert(tracker.commit(manager, latest, &error));
  assert(tracker.transformRevision() == manager.transformRevision());
  assert(tracker.stats().staleCommits == 1u);
}

void benchmarkRebakePlanning()
{
  const auto active = state(128u, 16u);
  const auto identity = transform(0.0f, 0.0f, 0.0f);
  struct Scenario
  {
    const char *name;
    uint32_t instances;
    float extent;
    bool overlap;
  };
  constexpr std::array scenarios{
    Scenario{"small", 1u, 1.0f, false},
    Scenario{"medium", 16u, 12.0f, false},
    Scenario{"large", 1u, 80.0f, false},
    Scenario{"overlap", 64u, 8.0f, true},
  };
  std::string error;
  constexpr uint32_t planningIterations = 100u;
  constexpr uint32_t rebakeIterations = 10u;
  for (const auto &scenario : scenarios)
  {
    std::vector<SparseVoxelTree64InstanceTransformRecord> previous;
    std::vector<SparseVoxelTree64InstanceTransformRecord> current;
    std::vector<SparseVoxelAABB> currentBounds;
    for (uint32_t index = 0u; index < scenario.instances; ++index)
    {
      const float base = scenario.overlap ? 0.0f : static_cast<float>(static_cast<int32_t>(index % 8u) - 4) * 8.0f;
      const float half = scenario.extent * 0.5f;
      previous.push_back(record(
          index,
          identity,
          {
            {base - half, -half, -half},
            {base + half, half, half},
          }));
      current.push_back(record(
          index,
          transform(1.0f, 0.5f, 0.25f),
          {
            {base + 1.0f - half, 0.5f - half, 0.25f - half},
            {base + 1.0f + half, 0.5f + half, 0.25f + half},
          }));
      currentBounds.push_back(current.back().worldBounds);
    }
    SparseVoxelTree64TransformUpdate update{};
    const auto planningBegin = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0u; iteration < planningIterations; ++iteration)
    {
      update = {};
      assert(sparseVoxelTree64PlanTransformInvalidation(active, previous, current, false, true, update, &error));
    }
    const auto planningElapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - planningBegin).count();

    uint64_t emittedNodes = 0u;
    const auto rebakeBegin = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0u; iteration < rebakeIterations; ++iteration)
    {
      std::vector<SparseVoxelTree64BuildSourceGPU> sources;
      assert(sparseVoxelTree64MakeBuildSources(active, update.dirtyChunks, currentBounds, sources, nullptr, &error));
      for (uint32_t jobIndex = 0u; jobIndex < update.dirtyChunks.size(); ++jobIndex)
      {
        std::vector<SparseVoxelTree64AABB> local;
        for (const auto &source : sources)
          if (source.jobIndex == jobIndex)
            local.push_back({
              {source.minimum[0], source.minimum[1], source.minimum[2]},
              {source.maximum[0], source.maximum[1], source.maximum[2]},
            });
        SparseVoxelTree64Chunk tree{};
        assert(buildSparseVoxelTree64(active.chunkSide, local, tree, &error));
        emittedNodes += tree.nodes.size();
      }
    }
    const auto rebakeElapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - rebakeBegin).count();
    std::cout << "TREE64_T6_BENCH scenario=" << scenario.name << " instances=" << scenario.instances << " dirty_chunks=" << update.dirtyChunks.size() << " planning_iterations=" << planningIterations
              << " planning_elapsed_us=" << planningElapsed << " rebake_iterations=" << rebakeIterations << " reference_rebake_elapsed_us=" << rebakeElapsed << " emitted_nodes=" << emittedNodes << '\n';
  }
}

} // namespace

int main()
{
  testTransformClassification();
  testOverlapRemovalMatchesFresh();
  testTrackerDirectoryTransaction();
  benchmarkRebakePlanning();
  std::cout << "Sparse voxel Tree64 T6 transform CPU tests passed\n";
  return 0;
}
