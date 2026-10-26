#include "voxel/svraster/SparseVoxelTree64Directory.hpp"
#include "voxel/svraster/SparseVoxelTree64Pages.hpp"

#include <algorithm>
#include <array>
// These tests use assert() as their check mechanism, and Release builds
// define NDEBUG. Without this the assertions vanish - and because many of
// them wrap the call they are checking, the call under test vanishes with
// them, leaving objects unconfigured and the suite crashing or silently
// passing. cassert re-reads NDEBUG on every include, so undefining it here
// restores assert regardless of what included cassert earlier.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{

using namespace voxel::svraster;

constexpr uint32_t kChunkSide = 16u;
constexpr uint32_t kChunkAxis = 9u;
constexpr uint32_t kDirectoryCapacity = kChunkAxis * kChunkAxis * kChunkAxis;
constexpr uint32_t kSlabSize = kChunkAxis * kChunkAxis;

SparseVoxelTree64DirectorySettings settings(const uint32_t slotCount)
{
  return {
    {128u, 128u, 128u},
    kChunkSide,
    {1.0f, 1.0f, 1.0f},
    sparseVoxelTree64MaximumNodeCount(kChunkSide),
    slotCount,
  };
}

std::vector<SparseVoxelTree64BuildResultGPU> successfulResults(const SparseVoxelTree64DirectoryUpdate &update)
{
  std::vector<SparseVoxelTree64BuildResultGPU> results;
  results.reserve(update.buildJobs.size());
  for (const auto &job : update.buildJobs)
    results.push_back({
      kSparseVoxelTree64BuildSuccess,
      1u,
      0u,
      job.nodeSlot,
      job.contentGeneration,
      0u,
      1u,
      0u,
    });
  return results;
}

std::vector<uint8_t> occupiedResults(const SparseVoxelTree64DirectoryUpdate &update)
{
  std::vector<uint8_t> occupied;
  occupied.reserve(update.buildChunks.size());
  for (const auto &chunk : update.buildChunks)
    occupied.push_back(static_cast<uint8_t>(((chunk[0] + chunk[1] + chunk[2]) & 1) == 0));
  return occupied;
}

void commitSuccess(SparseVoxelTree64Directory &directory, const SparseVoxelTree64DirectoryUpdate &update, const uint64_t token)
{
  std::string error;
  assert(directory.commitPending(successfulResults(update), occupiedResults(update), token, &error));
}

uint32_t physicalIndex(const std::array<uint32_t, 3> &slot, const std::array<uint32_t, 3> &resolution)
{
  return slot[0] + resolution[0] * (slot[1] + resolution[1] * slot[2]);
}

const SparseVoxelTree64ChunkGPU &descriptor(const SparseVoxelTree64Directory &directory, const std::array<int32_t, 3> &worldChunk)
{
  std::array<uint32_t, 3> physical{};
  assert(sparseVoxelTree64DirectorySlot(worldChunk, directory.state().originWorldChunk, directory.state().logicalChunkResolution, directory.state().wrapOffset, physical));
  const auto &result = directory.directory()[physicalIndex(physical, directory.state().logicalChunkResolution)];
  assert((result.flags & kSparseVoxelTree64ChunkValid) != 0u);
  assert(result.worldChunk == worldChunk);
  return result;
}

std::map<std::array<int32_t, 3>, SparseVoxelTree64ChunkGPU> descriptorMap(const SparseVoxelTree64Directory &directory)
{
  std::map<std::array<int32_t, 3>, SparseVoxelTree64ChunkGPU> result;
  for (uint32_t z = 0u; z < directory.state().logicalChunkResolution[2]; ++z)
    for (uint32_t y = 0u; y < directory.state().logicalChunkResolution[1]; ++y)
      for (uint32_t x = 0u; x < directory.state().logicalChunkResolution[0]; ++x)
      {
        const std::array<int32_t, 3> worldChunk{
          static_cast<int32_t>(directory.state().originWorldChunk[0] + static_cast<int32_t>(x)),
          static_cast<int32_t>(directory.state().originWorldChunk[1] + static_cast<int32_t>(y)),
          static_cast<int32_t>(directory.state().originWorldChunk[2] + static_cast<int32_t>(z)),
        };
        result.emplace(worldChunk, descriptor(directory, worldChunk));
      }
  assert(result.size() == kDirectoryCapacity);
  return result;
}

void assertClassification(const SparseVoxelTree64DirectoryUpdate &update, const uint32_t retained, const uint32_t entering, const uint32_t leaving)
{
  assert(update.retainedChunks.size() == retained);
  assert(update.enteringChunks.size() == entering);
  assert(update.leavingChunks.size() == leaving);
  std::set<std::array<int32_t, 3>> unique;
  unique.insert(update.retainedChunks.begin(), update.retainedChunks.end());
  unique.insert(update.enteringChunks.begin(), update.enteringChunks.end());
  assert(unique.size() == retained + entering);
}

void testPlanner()
{
  const auto config = settings(2u * kDirectoryCapacity);
  std::string error;
  SparseVoxelTree64DirectoryUpdate initial{};
  assert(prepareSparseVoxelTree64DirectoryUpdate(config, nullptr, {0.25f, 0.25f, 0.25f}, initial, &error));
  assert(initial.fullRebuild);
  assert(!initial.metadataOnly);
  assert(initial.targetState.logicalChunkResolution == (std::array<uint32_t, 3>{9u, 9u, 9u}));
  assert(initial.targetState.originWorldChunk == (std::array<int32_t, 3>{-4, -4, -4}));
  assert(initial.targetState.originCell == (std::array<int32_t, 3>{-64, -64, -64}));
  assertClassification(initial, 0u, kDirectoryCapacity, 0u);

  SparseVoxelTree64StateGPU committed = initial.targetState;
  SparseVoxelTree64DirectoryUpdate withinChunk{};
  assert(prepareSparseVoxelTree64DirectoryUpdate(config, &committed, {15.25f, 7.25f, 1.25f}, withinChunk, &error));
  assert(withinChunk.metadataOnly);
  assert(!withinChunk.fullRebuild);
  assert(withinChunk.targetState.originWorldChunk == committed.originWorldChunk);
  assert(withinChunk.targetState.contentGeneration == committed.contentGeneration);
  assert(withinChunk.targetState.originCell != committed.originCell);
  assertClassification(withinChunk, 0u, 0u, 0u);

  SparseVoxelTree64DirectoryUpdate positive{};
  assert(prepareSparseVoxelTree64DirectoryUpdate(config, &committed, {16.25f, 0.25f, 0.25f}, positive, &error));
  assert(!positive.fullRebuild);
  assert(positive.scrollDelta == (std::array<int32_t, 3>{1, 0, 0}));
  assert(positive.targetState.wrapOffset == (std::array<uint32_t, 3>{1u, 0u, 0u}));
  assertClassification(positive, kDirectoryCapacity - kSlabSize, kSlabSize, kSlabSize);

  SparseVoxelTree64DirectoryUpdate negativeDiagonal{};
  assert(prepareSparseVoxelTree64DirectoryUpdate(config, &committed, {-0.25f, -0.25f, -0.25f}, negativeDiagonal, &error));
  assert(!negativeDiagonal.fullRebuild);
  assert(negativeDiagonal.scrollDelta == (std::array<int32_t, 3>{-1, -1, -1}));
  assert(negativeDiagonal.targetState.wrapOffset == (std::array<uint32_t, 3>{8u, 8u, 8u}));
  assertClassification(negativeDiagonal, 8u * 8u * 8u, kDirectoryCapacity - 8u * 8u * 8u, kDirectoryCapacity - 8u * 8u * 8u);

  SparseVoxelTree64DirectoryUpdate teleport{};
  assert(prepareSparseVoxelTree64DirectoryUpdate(config, &committed, {144.25f, 0.25f, 0.25f}, teleport, &error));
  assert(teleport.fullRebuild);
  assert(teleport.scrollDelta == (std::array<int32_t, 3>{0, 0, 0}));
  assert(teleport.targetState.wrapOffset == (std::array<uint32_t, 3>{0u, 0u, 0u}));
  assertClassification(teleport, 0u, kDirectoryCapacity, kDirectoryCapacity);

  committed.contentGeneration = std::numeric_limits<uint32_t>::max();
  assert(!prepareSparseVoxelTree64DirectoryUpdate(config, &committed, {16.25f, 0.25f, 0.25f}, positive, &error));
}

void testToroidalPublicationAndRetirement()
{
  SparseVoxelTree64Directory directory;
  std::string error;
  assert(directory.configure(settings(kDirectoryCapacity + 3u * kSlabSize), &error));
  SparseVoxelTree64DirectoryUpdate initial{};
  assert(directory.beginUpdate({0.25f, 0.25f, 0.25f}, initial, &error));
  commitSuccess(directory, initial, 0u);
  assert(directory.stats().committedSlots == kDirectoryCapacity);
  assert(directory.stats().freeSlots == 3u * kSlabSize);
  const auto initialDescriptors = descriptorMap(directory);
  const auto initialGeneration = directory.state().contentGeneration;

  SparseVoxelTree64DirectoryUpdate metadata{};
  assert(directory.beginUpdate({15.25f, 4.25f, 2.25f}, metadata, &error));
  assert(metadata.metadataOnly);
  assert(metadata.buildJobs.empty());
  assert(!directory.hasPendingUpdate());
  assert(directory.stats().committedSlots == kDirectoryCapacity);
  assert(directory.state().contentGeneration == initialGeneration);
  assert(descriptorMap(directory).size() == initialDescriptors.size());

  SparseVoxelTree64DirectoryUpdate first{};
  assert(directory.beginUpdate({16.25f, 0.25f, 0.25f}, first, &error));
  assert(first.buildJobs.size() == kSlabSize);
  std::set<uint32_t> firstLeavingSlots;
  for (const auto &chunk : first.leavingChunks)
    firstLeavingSlots.insert(descriptor(directory, chunk).nodeSlot);
  for (const auto &job : first.buildJobs)
    assert(firstLeavingSlots.find(job.nodeSlot) == firstLeavingSlots.end());
  commitSuccess(directory, first, 1u);
  assert(directory.stats().retiredSlots == kSlabSize);
  const auto afterFirst = descriptorMap(directory);
  for (const auto &chunk : first.retainedChunks)
  {
    const auto before = initialDescriptors.at(chunk);
    const auto after = afterFirst.at(chunk);
    assert(std::memcmp(&before, &after, sizeof(before)) == 0);
  }

  SparseVoxelTree64DirectoryUpdate second{};
  assert(directory.beginUpdate({0.25f, 0.25f, 0.25f}, second, &error));
  commitSuccess(directory, second, 2u);
  assert(directory.stats().retiredSlots == 2u * kSlabSize);

  SparseVoxelTree64DirectoryUpdate third{};
  assert(directory.beginUpdate({16.25f, 0.25f, 0.25f}, third, &error));
  commitSuccess(directory, third, 3u);
  assert(directory.stats().retiredSlots == 3u * kSlabSize);
  assert(directory.stats().freeSlots == 0u);

  const auto committedState = directory.state();
  const auto committedDirectory = directory.directory();
  SparseVoxelTree64DirectoryUpdate exhausted{};
  assert(!directory.beginUpdate({0.25f, 0.25f, 0.25f}, exhausted, &error));
  assert(!directory.hasPendingUpdate());
  assert(std::memcmp(&directory.state(), &committedState, sizeof(committedState)) == 0);
  assert(std::memcmp(directory.directory().data(), committedDirectory.data(), committedDirectory.size() * sizeof(SparseVoxelTree64ChunkGPU)) == 0);
  assert(directory.stats().failedAllocations == 1u);

  assert(directory.reclaimCompleted(1u, &error));
  assert(directory.stats().freeSlots == kSlabSize);
  SparseVoxelTree64DirectoryUpdate reused{};
  assert(directory.beginUpdate({0.25f, 0.25f, 0.25f}, reused, &error));
  std::set<uint32_t> reusedSlots;
  for (const auto &job : reused.buildJobs)
    reusedSlots.insert(job.nodeSlot);
  assert(reusedSlots == firstLeavingSlots);
  assert(directory.abortPending(4u, &error));
  assert(directory.stats().retiredSlots == 3u * kSlabSize);
  assert(directory.reclaimCompleted(3u, &error));
  assert(directory.stats().freeSlots == 2u * kSlabSize);
  assert(directory.stats().retiredSlots == kSlabSize);
  assert(directory.reclaimCompleted(4u, &error));
  assert(directory.stats().freeSlots == 3u * kSlabSize);
  assert(directory.stats().retiredSlots == 0u);
}

void testFailedBuildAndTeleport()
{
  std::string error;
  SparseVoxelTree64Directory directory;
  assert(directory.configure(settings(2u * kDirectoryCapacity), &error));
  SparseVoxelTree64DirectoryUpdate initial{};
  assert(directory.beginUpdate({0.25f, 0.25f, 0.25f}, initial, &error));
  commitSuccess(directory, initial, 0u);

  SparseVoxelTree64DirectoryUpdate failed{};
  assert(directory.beginUpdate({16.25f, 0.25f, 0.25f}, failed, &error));
  auto results = successfulResults(failed);
  results.front().status = kSparseVoxelTree64BuildOverflow;
  const auto priorState = directory.state();
  const auto priorDirectory = directory.directory();
  assert(!directory.commitPending(results, occupiedResults(failed), 7u, &error));
  assert(directory.hasPendingUpdate());
  assert(std::memcmp(&directory.state(), &priorState, sizeof(priorState)) == 0);
  assert(std::memcmp(directory.directory().data(), priorDirectory.data(), priorDirectory.size() * sizeof(SparseVoxelTree64ChunkGPU)) == 0);
  assert(directory.abortPending(7u, &error));
  assert(directory.stats().retiredSlots == kSlabSize);
  assert(directory.reclaimCompleted(6u, &error));
  assert(directory.stats().freeSlots == kDirectoryCapacity - kSlabSize);
  assert(directory.reclaimCompleted(7u, &error));
  assert(directory.stats().freeSlots == kDirectoryCapacity);

  SparseVoxelTree64DirectoryUpdate teleport{};
  assert(directory.beginUpdate({144.25f, 0.25f, 0.25f}, teleport, &error));
  assert(teleport.fullRebuild);
  assert(teleport.buildJobs.size() == kDirectoryCapacity);
  commitSuccess(directory, teleport, 8u);
  assert(directory.state().originWorldChunk == (std::array<int32_t, 3>{5, -4, -4}));
  assert(directory.state().wrapOffset == (std::array<uint32_t, 3>{0u, 0u, 0u}));
  assert(descriptorMap(directory).size() == kDirectoryCapacity);
  assert(directory.stats().committedSlots == kDirectoryCapacity);
  assert(directory.stats().retiredSlots == kDirectoryCapacity);
}

void testDiagonalPublication()
{
  std::string error;
  SparseVoxelTree64Directory directory;
  assert(directory.configure(settings(2u * kDirectoryCapacity), &error));
  SparseVoxelTree64DirectoryUpdate initial{};
  assert(directory.beginUpdate({0.25f, 0.25f, 0.25f}, initial, &error));
  commitSuccess(directory, initial, 0u);
  const auto before = descriptorMap(directory);

  SparseVoxelTree64DirectoryUpdate diagonal{};
  assert(directory.beginUpdate({-0.25f, -0.25f, -0.25f}, diagonal, &error));
  assert(diagonal.scrollDelta == (std::array<int32_t, 3>{-1, -1, -1}));
  commitSuccess(directory, diagonal, 1u);
  assert(directory.state().wrapOffset == (std::array<uint32_t, 3>{8u, 8u, 8u}));
  const auto after = descriptorMap(directory);
  for (const auto &chunk : diagonal.retainedChunks)
  {
    const auto oldDescriptor = before.at(chunk);
    const auto newDescriptor = after.at(chunk);
    assert(std::memcmp(&oldDescriptor, &newDescriptor, sizeof(oldDescriptor)) == 0);
  }
}

void testConservativeUnknownPublication()
{
  std::string error;
  SparseVoxelTree64Directory directory;
  assert(directory.configure(settings(2u * kDirectoryCapacity), &error));
  SparseVoxelTree64DirectoryUpdate initial{};
  assert(directory.beginUpdate({0.25f, 0.25f, 0.25f}, initial, &error));
  commitSuccess(directory, initial, 0u);

  SparseVoxelTree64DirectoryUpdate update{};
  assert(directory.beginUpdate({16.25f, 0.25f, 0.25f}, update, &error));
  auto results = successfulResults(update);
  results.front().status = kSparseVoxelTree64BuildOverflow;
  assert(directory.commitPending(results, occupiedResults(update), 8u, &error, SparseVoxelTree64FailurePolicy::PublishUnknown));
  const auto &unknown = descriptor(directory, update.enteringChunks.front());
  assert((unknown.flags & kSparseVoxelTree64ChunkValid) != 0u);
  assert((unknown.flags & kSparseVoxelTree64ChunkUnknown) != 0u);
  assert(unknown.nodeCount == 0u);
  assert(directory.stats().committedSlots == kDirectoryCapacity - 1u);
  assert(directory.stats().retiredSlots == kSlabSize + 1u);
  assert(directory.reclaimCompleted(8u, &error));
  assert(directory.stats().retiredSlots == 0u);
}

void testImmutablePageAllocatorLifecycle()
{
  std::string error;
  SparseVoxelTree64PageAllocator allocator;
  assert(!allocator.configure({0u, 64u}, &error));
  assert(!allocator.configure({2u, 0u}, &error));
  assert(allocator.configure({2u, 4096u}, &error));
  assert(allocator.stats().freePages == 2u);
  assert(allocator.stats().nodesPerPage == 4096u);

  SparseVoxelTree64PageHandle first{};
  SparseVoxelTree64PageHandle second{};
  SparseVoxelTree64PageHandle exhausted{};
  assert(allocator.reserve(first, &error));
  assert(allocator.reserve(second, &error));
  assert(first.valid() && second.valid() && first.slot != second.slot);
  assert(!allocator.isReachable(first));
  assert(!allocator.reserve(exhausted, &error));
  assert(!exhausted.valid());
  assert(allocator.stats().failedAllocations == 1u);

  // Upload transitions never make a page reachable. Only publication does.
  assert(allocator.beginUpload(first, &error));
  assert(!allocator.isReachable(first));
  assert(allocator.finishUpload(first, &error));
  assert(!allocator.isReachable(first));
  assert(allocator.commit(first, &error));
  assert(allocator.isReachable(first));
  assert(allocator.stats().committedPages == 1u);

  // A CPU-only abandoned reservation can return immediately. Once reused,
  // the old handle must not validate even though its physical slot matches.
  assert(allocator.abort(second, 0u, &error));
  SparseVoxelTree64PageHandle replacement{};
  assert(allocator.reserve(replacement, &error));
  assert(replacement.slot == second.slot);
  assert(replacement.generation != second.generation);
  assert(!allocator.isCurrent(second));
  assert(!allocator.beginUpload(second, &error));
  assert(allocator.stats().staleOperations == 1u);

  assert(allocator.beginUpload(replacement, &error));
  assert(allocator.finishUpload(replacement, &error));
  assert(allocator.commit(replacement, &error));
  assert(allocator.isReachable(first));
  assert(allocator.isReachable(replacement));

  // Replacing a logical owner does not mutate its old page. It remains
  // unavailable for allocation until the last possible reader completes.
  assert(allocator.retire(first, 7u, &error));
  assert(!allocator.isReachable(first));
  assert(allocator.stats().retiredPages == 1u);
  assert(allocator.reclaimCompleted(6u, &error));
  assert(allocator.stats().freePages == 0u);
  assert(allocator.reclaimCompleted(7u, &error));
  assert(allocator.stats().freePages == 1u);

  SparseVoxelTree64PageHandle reused{};
  assert(allocator.reserve(reused, &error));
  assert(reused.slot == first.slot);
  assert(reused.generation != first.generation);
  assert(!allocator.isCurrent(first));
  assert(!allocator.retire(first, 8u, &error));
  assert(allocator.stats().staleOperations == 2u);

  // An uploaded-but-unpublished page can be cancelled safely behind its copy
  // submission without disturbing the remaining committed page.
  assert(allocator.beginUpload(reused, &error));
  assert(allocator.finishUpload(reused, &error));
  assert(allocator.abort(reused, 9u, &error));
  assert(allocator.isReachable(replacement));
  assert(allocator.stats().retiredPages == 1u);
  assert(allocator.reclaimCompleted(9u, &error));
  assert(allocator.stats().freePages == 1u);
  assert(allocator.stats().committedPages == 1u);
}

void testAtomicPagePublicationAndRollback()
{
  std::string error;
  SparseVoxelTree64PageAllocator allocator;
  assert(allocator.configure({6u, 4096u}, &error));
  const auto uploaded = [&]()
  {
    SparseVoxelTree64PageHandle handle{};
    assert(allocator.reserve(handle, &error));
    assert(allocator.beginUpload(handle, &error));
    assert(allocator.finishUpload(handle, &error));
    return handle;
  };

  const auto firstOwner = uploaded();
  const auto secondOwner = uploaded();
  assert(allocator.publish(
      {{{}, firstOwner}, {{}, secondOwner}}, 0u, &error));
  assert(allocator.isReachable(firstOwner));
  assert(allocator.isReachable(secondOwner));

  const auto validReplacement = uploaded();
  SparseVoxelTree64PageHandle incompleteReplacement{};
  assert(allocator.reserve(incompleteReplacement, &error));
  const std::vector<SparseVoxelTree64PageReplacement> rejected{
    {firstOwner, validReplacement},
    {secondOwner, incompleteReplacement},
  };
  assert(!allocator.publish(rejected, 3u, &error));
  // Validation is batch-atomic: neither the good entry nor either old owner
  // changed state when another replacement was incomplete.
  assert(allocator.isReachable(firstOwner));
  assert(allocator.isReachable(secondOwner));
  assert(allocator.state(validReplacement) ==
         SparseVoxelTree64PageState::Uploaded);
  assert(allocator.state(incompleteReplacement) ==
         SparseVoxelTree64PageState::Reserved);

  assert(allocator.abortAll(
      {validReplacement, incompleteReplacement}, 3u, &error));
  assert(allocator.isReachable(firstOwner));
  assert(allocator.isReachable(secondOwner));
  assert(allocator.stats().retiredPages == 2u);
  assert(allocator.reclaimCompleted(3u, &error));
  assert(allocator.stats().committedPages == 2u);
  assert(allocator.stats().freePages == 4u);

  // Independent asynchronous owners may receive retirement tokens out of
  // order. Each page is guarded by its own completion, not a global latest
  // token that can spuriously reject an otherwise safe publication.
  const auto laterReplacement = uploaded();
  assert(allocator.publish(
      {{firstOwner, laterReplacement}}, 7u, &error));
  const auto earlierReplacement = uploaded();
  assert(allocator.publish(
      {{secondOwner, earlierReplacement}}, 5u, &error));
  assert(allocator.reclaimCompleted(5u, &error));
  assert(allocator.state(firstOwner) ==
         SparseVoxelTree64PageState::Retired);
  assert(allocator.state(secondOwner) ==
         SparseVoxelTree64PageState::Free);
  assert(allocator.isReachable(laterReplacement));
  assert(allocator.isReachable(earlierReplacement));
}

void testRapidRewriteCompletionSafety()
{
  constexpr uint64_t maxFramesInFlight = 2u;
  // This is deliberately much longer than the physical-page ring. It forces
  // every slot through many generations and catches generation wrap/reuse,
  // retirement, or high-watermark drift that a four-publication smoke test
  // cannot expose.
  constexpr uint64_t rewriteCount = 512u;
  std::string error;
  SparseVoxelTree64PageAllocator allocator;
  assert(allocator.configure(
      {static_cast<uint32_t>(maxFramesInFlight + 2u), 4096u}, &error));

  SparseVoxelTree64PageHandle owner{};
  assert(allocator.reserve(owner, &error));
  assert(allocator.beginUpload(owner, &error));
  assert(allocator.finishUpload(owner, &error));
  assert(allocator.publish({{{}, owner}}, 0u, &error));

  struct Retired
  {
    SparseVoxelTree64PageHandle handle{};
    uint64_t completion = 0u;
  };
  std::vector<Retired> retired;
  for (uint64_t submission = 1u; submission <= rewriteCount; ++submission)
  {
    const uint64_t completed = submission > maxFramesInFlight
        ? submission - maxFramesInFlight
        : 0u;
    assert(allocator.reclaimCompleted(completed, &error));

    SparseVoxelTree64PageHandle replacement{};
    assert(allocator.reserve(replacement, &error));
    for (const auto &entry : retired)
      if (entry.completion > completed)
        assert(replacement.slot != entry.handle.slot);
    assert(allocator.beginUpload(replacement, &error));
    assert(allocator.finishUpload(replacement, &error));
    const auto previous = owner;
    assert(allocator.publish(
        {{previous, replacement}}, submission, &error));
    retired.push_back({previous, submission});
    owner = replacement;

    assert(allocator.isReachable(owner));
    assert(!allocator.isReachable(previous));
    assert(allocator.state(previous) ==
           SparseVoxelTree64PageState::Retired);
    assert(allocator.stats().committedPages == 1u);
    assert(allocator.stats().reservedPages == 0u);
    assert(allocator.stats().uploadingPages == 0u);
    assert(allocator.stats().uploadedPages == 0u);
    assert(allocator.stats().retiredPages <= maxFramesInFlight);
  }

  assert(allocator.reclaimCompleted(rewriteCount, &error));
  assert(allocator.isReachable(owner));
  assert(allocator.stats().committedPages == 1u);
  assert(allocator.stats().retiredPages == 0u);
  assert(allocator.stats().freePages == maxFramesInFlight + 1u);
  assert(allocator.stats().failedAllocations == 0u);
  assert(allocator.stats().staleOperations == 0u);
}

} // namespace

int main()
{
  testPlanner();
  testToroidalPublicationAndRetirement();
  testFailedBuildAndTeleport();
  testDiagonalPublication();
  testConservativeUnknownPublication();
  testImmutablePageAllocatorLifecycle();
  testAtomicPagePublicationAndRollback();
  testRapidRewriteCompletionSafety();
  std::cout << "Sparse voxel Tree64 T4 directory streaming tests passed\n";
  return 0;
}
