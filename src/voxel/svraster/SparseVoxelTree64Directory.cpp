#include "voxel/svraster/SparseVoxelTree64Directory.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace voxel::svraster
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool checkedProduct(const std::array<uint32_t, 3> &value, uint64_t &result)
{
  result = 1u;
  for (const uint32_t component : value)
  {
    if (component == 0u || result > std::numeric_limits<uint64_t>::max() / component)
      return false;
    result *= component;
  }
  return true;
}

bool sameLayout(const SparseVoxelTree64DirectorySettings &settings, const SparseVoxelTree64StateGPU &state)
{
  return state.chunkSide == settings.chunkSide && state.logicalCellResolution == settings.logicalCellResolution && state.cellSize == settings.cellSize;
}

bool containsChunk(const SparseVoxelTree64StateGPU &state, const std::array<int32_t, 3> &worldChunk)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int64_t logical = static_cast<int64_t>(worldChunk[axis]) - state.originWorldChunk[axis];
    if (logical < 0 || logical >= state.logicalChunkResolution[axis])
      return false;
  }
  return true;
}

std::vector<std::array<int32_t, 3>> enumerateChunks(const SparseVoxelTree64StateGPU &state)
{
  std::vector<std::array<int32_t, 3>> result;
  uint64_t count = 0u;
  if (!checkedProduct(state.logicalChunkResolution, count) || count > std::numeric_limits<size_t>::max())
    return result;
  result.reserve(static_cast<size_t>(count));
  for (uint32_t z = 0u; z < state.logicalChunkResolution[2]; ++z)
    for (uint32_t y = 0u; y < state.logicalChunkResolution[1]; ++y)
      for (uint32_t x = 0u; x < state.logicalChunkResolution[0]; ++x)
        result.push_back({
          static_cast<int32_t>(static_cast<int64_t>(state.originWorldChunk[0]) + x),
          static_cast<int32_t>(static_cast<int64_t>(state.originWorldChunk[1]) + y),
          static_cast<int32_t>(static_cast<int64_t>(state.originWorldChunk[2]) + z),
        });
  return result;
}

uint32_t physicalIndex(const std::array<uint32_t, 3> &slot, const std::array<uint32_t, 3> &resolution)
{
  return slot[0] + resolution[0] * (slot[1] + resolution[1] * slot[2]);
}

bool descriptorForChunk(const SparseVoxelTree64StateGPU &state, const std::vector<SparseVoxelTree64ChunkGPU> &directory, const std::array<int32_t, 3> &worldChunk, uint32_t &index, std::string *error)
{
  std::array<uint32_t, 3> physical{};
  if (!sparseVoxelTree64DirectorySlot(worldChunk, state.originWorldChunk, state.logicalChunkResolution, state.wrapOffset, physical))
    return fail(error, "Tree64 chunk is outside the committed directory");
  index = physicalIndex(physical, state.logicalChunkResolution);
  if (index >= directory.size())
    return fail(error, "Tree64 physical directory index is out of range");
  const auto &descriptor = directory[index];
  if ((descriptor.flags & kSparseVoxelTree64ChunkValid) == 0u || descriptor.worldChunk != worldChunk)
    return fail(error, "Tree64 committed directory identity is invalid");
  return true;
}

} // namespace

bool makeSparseVoxelTree64DirectoryState(const SparseVoxelTree64DirectorySettings &settings, const std::array<float, 3> &cameraWorldPosition, SparseVoxelTree64StateGPU &state, std::string *error)
{
  if (error != nullptr)
    error->clear();
  state = {};
  if (!sparseVoxelTree64SupportedSide(settings.chunkSide))
    return fail(error, "Tree64 directory chunk side must be 4, 16, or 64");
  if (settings.nodeSlotCapacity < sparseVoxelTree64MaximumNodeCount(settings.chunkSide) || settings.nodeSlotCount == 0u)
    return fail(error, "Tree64 directory node-slot capacity is invalid");

  std::array<int32_t, 3> cameraCell{};
  std::array<int32_t, 3> cameraChunk{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (settings.logicalCellResolution[axis] == 0u || settings.logicalCellResolution[axis] > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) || !std::isfinite(settings.cellSize[axis]) ||
        settings.cellSize[axis] <= 0.0f || !std::isfinite(cameraWorldPosition[axis]))
      return fail(error, "Tree64 directory layout or camera position is invalid");
    const double scaled = static_cast<double>(cameraWorldPosition[axis]) / settings.cellSize[axis];
    const double floored = std::floor(scaled);
    if (floored < std::numeric_limits<int32_t>::min() || floored > std::numeric_limits<int32_t>::max())
      return fail(error, "Tree64 directory camera cell exceeds signed 32-bit range");
    cameraCell[axis] = static_cast<int32_t>(floored);
    cameraChunk[axis] = sparseVoxelTree64FloorDivide(cameraCell[axis], settings.chunkSide);

    const uint64_t chunkResolution = (uint64_t{settings.logicalCellResolution[axis]} + settings.chunkSide - 1u) / settings.chunkSide + 1u;
    if (chunkResolution > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
      return fail(error, "Tree64 directory chunk resolution exceeds signed 32-bit range");
    state.logicalChunkResolution[axis] = static_cast<uint32_t>(chunkResolution);

    const int64_t originCell = static_cast<int64_t>(cameraCell[axis]) - settings.logicalCellResolution[axis] / 2u;
    const int64_t lastCell = originCell + settings.logicalCellResolution[axis] - 1u;
    const int64_t originChunk = static_cast<int64_t>(cameraChunk[axis]) - state.logicalChunkResolution[axis] / 2u;
    const int64_t lastChunk = originChunk + state.logicalChunkResolution[axis] - 1u;
    if (originCell < std::numeric_limits<int32_t>::min() || lastCell > std::numeric_limits<int32_t>::max() || originChunk < std::numeric_limits<int32_t>::min() || lastChunk > std::numeric_limits<int32_t>::max())
      return fail(error, "Tree64 represented range exceeds signed 32-bit coordinates");
    state.originCell[axis] = static_cast<int32_t>(originCell);
    state.originWorldChunk[axis] = static_cast<int32_t>(originChunk);

    const int32_t exactFirstChunk = sparseVoxelTree64FloorDivide(static_cast<int32_t>(originCell), settings.chunkSide);
    const int32_t exactLastChunk = sparseVoxelTree64FloorDivide(static_cast<int32_t>(lastCell), settings.chunkSide);
    if (exactFirstChunk < state.originWorldChunk[axis] || exactLastChunk > lastChunk)
      return fail(error, "Tree64 alignment halo does not contain the exact shadow range");

    const double worldExtent = static_cast<double>(settings.logicalCellResolution[axis]) * settings.cellSize[axis];
    if (!std::isfinite(worldExtent) || worldExtent > std::numeric_limits<float>::max())
      return fail(error, "Tree64 world extent is invalid");
    state.worldExtent[axis] = static_cast<float>(worldExtent);
  }

  uint64_t directoryCapacity = 0u;
  if (!checkedProduct(state.logicalChunkResolution, directoryCapacity) || directoryCapacity > settings.nodeSlotCount)
    return fail(error, "Tree64 node-slot pool cannot hold one complete directory generation");

  state.chunkSide = settings.chunkSide;
  state.logicalCellResolution = settings.logicalCellResolution;
  state.cellSize = settings.cellSize;
  return true;
}

bool prepareSparseVoxelTree64DirectoryUpdate(
    const SparseVoxelTree64DirectorySettings &settings,
    const SparseVoxelTree64StateGPU *committedState,
    const std::array<float, 3> &cameraWorldPosition,
    SparseVoxelTree64DirectoryUpdate &update,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  update = {};
  if (!makeSparseVoxelTree64DirectoryState(settings, cameraWorldPosition, update.targetState, error))
    return false;

  const bool hasCommitted = committedState != nullptr && committedState->valid != 0u;
  if (hasCommitted && !sameLayout(settings, *committedState))
    return fail(error, "Tree64 committed directory layout does not match its settings");
  if (!hasCommitted)
  {
    update.fullRebuild = true;
    update.targetState.contentGeneration = 1u;
    update.targetState.valid = 1u;
    update.enteringChunks = enumerateChunks(update.targetState);
    return true;
  }

  update.targetState.wrapOffset = committedState->wrapOffset;
  update.targetState.contentGeneration = committedState->contentGeneration;
  update.targetState.valid = 1u;
  bool fullRebuild = false;
  bool originChanged = false;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int64_t delta = static_cast<int64_t>(update.targetState.originWorldChunk[axis]) - committedState->originWorldChunk[axis];
    originChanged = originChanged || delta != 0;
    if (std::abs(delta) >= static_cast<int64_t>(update.targetState.logicalChunkResolution[axis]))
      fullRebuild = true;
    else
      update.scrollDelta[axis] = static_cast<int32_t>(delta);
  }

  if (!originChanged)
  {
    update.metadataOnly = true;
    return true;
  }
  if (committedState->contentGeneration == std::numeric_limits<uint32_t>::max())
    return fail(error, "Tree64 directory content generation overflow");
  update.targetState.contentGeneration = committedState->contentGeneration + 1u;
  update.fullRebuild = fullRebuild;
  if (fullRebuild)
  {
    update.scrollDelta = {0, 0, 0};
    update.targetState.wrapOffset = {0u, 0u, 0u};
  }
  else
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const int64_t shifted = static_cast<int64_t>(committedState->wrapOffset[axis]) + update.scrollDelta[axis];
      const int64_t divisor = update.targetState.logicalChunkResolution[axis];
      const int64_t remainder = shifted % divisor;
      update.targetState.wrapOffset[axis] = static_cast<uint32_t>(remainder < 0 ? remainder + divisor : remainder);
    }
  }

  const auto oldChunks = enumerateChunks(*committedState);
  const auto newChunks = enumerateChunks(update.targetState);
  for (const auto &chunk : oldChunks)
  {
    if (containsChunk(update.targetState, chunk))
      update.retainedChunks.push_back(chunk);
    else
      update.leavingChunks.push_back(chunk);
  }
  for (const auto &chunk : newChunks)
    if (!containsChunk(*committedState, chunk))
      update.enteringChunks.push_back(chunk);
  return true;
}

void SparseVoxelTree64Directory::reset()
{
  settings_ = {};
  state_ = {};
  directory_.clear();
  slotStates_.clear();
  freeSlots_.clear();
  retiredSlots_.clear();
  pending_.reset();
  stats_ = {};
  completedSubmission_ = 0u;
  latestRetirementSubmission_ = 0u;
  sceneRevision_ = 0u;
  pendingSceneBound_ = false;
  configured_ = false;
}

bool SparseVoxelTree64Directory::configure(const SparseVoxelTree64DirectorySettings &settings, std::string *error)
{
  if (error != nullptr)
    error->clear();
  SparseVoxelTree64StateGPU validation{};
  if (!makeSparseVoxelTree64DirectoryState(settings, {0.0f, 0.0f, 0.0f}, validation, error))
    return false;
  uint64_t directoryCapacity = 0u;
  if (!checkedProduct(validation.logicalChunkResolution, directoryCapacity) || directoryCapacity > std::numeric_limits<size_t>::max())
    return fail(error, "Tree64 directory capacity exceeds host address space");

  reset();
  settings_ = settings;
  directory_.resize(static_cast<size_t>(directoryCapacity));
  slotStates_.resize(settings.nodeSlotCount, SlotState::Free);
  freeSlots_.reserve(settings.nodeSlotCount);
  for (uint32_t slot = settings.nodeSlotCount; slot > 0u; --slot)
    freeSlots_.push_back(slot - 1u);
  configured_ = true;
  refreshStats();
  return true;
}

bool SparseVoxelTree64Directory::beginUpdate(const std::array<float, 3> &cameraWorldPosition, SparseVoxelTree64DirectoryUpdate &update, std::string *error)
{
  return beginUpdateInternal(cameraWorldPosition, sceneRevision_, {}, false, true, update, error);
}

bool SparseVoxelTree64Directory::beginSceneUpdate(
    const std::array<float, 3> &cameraWorldPosition,
    const uint64_t sceneRevision,
    const std::vector<std::array<int32_t, 3>> &invalidatedWorldChunks,
    const bool completeInvalidation,
    SparseVoxelTree64DirectoryUpdate &update,
    std::string *error)
{
  return beginUpdateInternal(cameraWorldPosition, sceneRevision, invalidatedWorldChunks, true, completeInvalidation, update, error);
}

bool SparseVoxelTree64Directory::beginUpdateInternal(
    const std::array<float, 3> &cameraWorldPosition,
    const uint64_t sceneRevision,
    const std::vector<std::array<int32_t, 3>> &invalidatedWorldChunks,
    const bool sceneBound,
    const bool completeInvalidation,
    SparseVoxelTree64DirectoryUpdate &update,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  update = {};
  if (!configured_)
    return fail(error, "Tree64 directory is not configured");
  if (pending_.has_value())
    return fail(error, "Tree64 directory already has a pending update");
  if (sceneBound && sceneRevision < sceneRevision_)
    return fail(error, "Tree64 scene update revision is stale");

  SparseVoxelTree64DirectoryUpdate next{};
  const SparseVoxelTree64StateGPU *committed = state_.valid != 0u ? &state_ : nullptr;
  if (!prepareSparseVoxelTree64DirectoryUpdate(settings_, committed, cameraWorldPosition, next, error))
    return false;
  next.sceneRevision = sceneRevision;

  std::vector<std::array<int32_t, 3>> dirtyChunks;
  if (sceneBound && state_.valid != 0u && !completeInvalidation)
  {
    next.fullSceneRebuild = true;
    dirtyChunks = enumerateChunks(next.targetState);
  }
  else if (sceneBound)
  {
    dirtyChunks.reserve(invalidatedWorldChunks.size());
    for (const auto &chunk : invalidatedWorldChunks)
      if (containsChunk(next.targetState, chunk))
        dirtyChunks.push_back(chunk);
  }

  std::sort(dirtyChunks.begin(), dirtyChunks.end());
  dirtyChunks.erase(std::unique(dirtyChunks.begin(), dirtyChunks.end()), dirtyChunks.end());
  for (const auto &chunk : dirtyChunks)
    if (std::find(next.enteringChunks.begin(), next.enteringChunks.end(), chunk) == next.enteringChunks.end())
      next.rebuiltChunks.push_back(chunk);

  next.buildChunks = next.enteringChunks;
  next.buildChunks.insert(next.buildChunks.end(), next.rebuiltChunks.begin(), next.rebuiltChunks.end());
  std::sort(next.buildChunks.begin(), next.buildChunks.end());
  next.buildChunks.erase(std::unique(next.buildChunks.begin(), next.buildChunks.end()), next.buildChunks.end());

  if (!next.buildChunks.empty() && next.metadataOnly)
  {
    if (state_.contentGeneration == std::numeric_limits<uint32_t>::max())
      return fail(error, "Tree64 directory content generation overflow");
    next.metadataOnly = false;
    next.targetState.contentGeneration = state_.contentGeneration + 1u;
  }
  if (next.metadataOnly)
  {
    state_ = next.targetState;
    if (sceneBound)
      sceneRevision_ = sceneRevision;
    update = std::move(next);
    return true;
  }
  if (freeSlots_.size() < next.buildChunks.size())
  {
    ++stats_.failedAllocations;
    return fail(error, "Tree64 node-slot pool is exhausted");
  }

  std::vector<uint32_t> publicationIndices;
  publicationIndices.reserve(next.buildChunks.size());
  for (const auto &worldChunk : next.buildChunks)
  {
    std::array<uint32_t, 3> physical{};
    if (!sparseVoxelTree64DirectorySlot(worldChunk, next.targetState.originWorldChunk, next.targetState.logicalChunkResolution, next.targetState.wrapOffset, physical))
      return fail(error, "Tree64 build chunk has no target publication slot");
    publicationIndices.push_back(physicalIndex(physical, next.targetState.logicalChunkResolution));
  }

  next.buildJobs.reserve(next.buildChunks.size());
  next.publicationRecords.reserve(next.buildChunks.size());
  for (uint32_t index = 0u; index < next.buildChunks.size(); ++index)
  {
    const auto &worldChunk = next.buildChunks[index];
    const uint32_t slot = freeSlots_.back();
    freeSlots_.pop_back();
    slotStates_[slot] = SlotState::Reserved;
    next.buildJobs.push_back({
      worldChunk,
      slot,
      next.targetState.contentGeneration,
      kSparseVoxelTree64BuildJobValid,
      0u,
      0u,
    });
    next.publicationRecords.push_back({
      worldChunk,
      publicationIndices[index],
      slot,
      next.targetState.contentGeneration,
      index,
      0u,
    });
  }
  pending_ = next;
  pendingSceneBound_ = sceneBound;
  update = next;
  refreshStats();
  return true;
}

bool SparseVoxelTree64Directory::commitPending(
    const std::vector<SparseVoxelTree64BuildResultGPU> &results,
    const std::vector<uint8_t> &occupied,
    const uint64_t retireAfterCompletion,
    std::string *error,
    const SparseVoxelTree64FailurePolicy failurePolicy)
{
  return commitPendingInternal({}, results, occupied, retireAfterCompletion, error, failurePolicy);
}

bool SparseVoxelTree64Directory::commitPendingScene(
    const uint64_t sceneRevision,
    const std::vector<SparseVoxelTree64BuildResultGPU> &results,
    const std::vector<uint8_t> &occupied,
    const uint64_t retireAfterCompletion,
    std::string *error,
    const SparseVoxelTree64FailurePolicy failurePolicy)
{
  return commitPendingInternal(sceneRevision, results, occupied, retireAfterCompletion, error, failurePolicy);
}

bool SparseVoxelTree64Directory::commitPendingInternal(
    const std::optional<uint64_t> &sceneRevision,
    const std::vector<SparseVoxelTree64BuildResultGPU> &results,
    const std::vector<uint8_t> &occupied,
    const uint64_t retireAfterCompletion,
    std::string *error,
    const SparseVoxelTree64FailurePolicy failurePolicy)
{
  if (error != nullptr)
    error->clear();
  if (!pending_.has_value())
    return fail(error, "Tree64 directory has no pending update");
  const auto &pending = *pending_;
  if (pendingSceneBound_)
  {
    if (!sceneRevision.has_value() || *sceneRevision != pending.sceneRevision)
      return fail(error, "Tree64 scene completion revision is stale");
  }
  else if (sceneRevision.has_value())
    return fail(error, "Tree64 pending camera update is not scene-bound");
  if (results.size() != pending.buildJobs.size() || occupied.size() != pending.buildJobs.size())
    return fail(error, "Tree64 build completion count does not match the pending update");
  if (retireAfterCompletion < completedSubmission_)
    return fail(error, "Tree64 retirement completion precedes the completed submission");
  if (retireAfterCompletion < latestRetirementSubmission_)
    return fail(error, "Tree64 retirement submission token moved backwards");

  std::vector<uint8_t> successful(results.size(), 0u);
  for (size_t index = 0u; index < results.size(); ++index)
  {
    const auto &result = results[index];
    const auto &job = pending.buildJobs[index];
    if (result.nodeSlot != job.nodeSlot || result.contentGeneration != job.contentGeneration || job.nodeSlot >= slotStates_.size() || slotStates_[job.nodeSlot] != SlotState::Reserved)
      return fail(error, "Tree64 pending build result is invalid or incomplete");
    if (result.status == kSparseVoxelTree64BuildSuccess)
    {
      if (result.nodeCount == 0u || result.nodeCount > settings_.nodeSlotCapacity || result.rootNodeOffset >= result.nodeCount)
        return fail(error, "Tree64 successful build result has invalid node bounds");
      successful[index] = 1u;
    }
    else if (failurePolicy != SparseVoxelTree64FailurePolicy::PublishUnknown)
    {
      return fail(error, "Tree64 pending build did not complete successfully");
    }
  }

  std::vector<uint32_t> retiredCommittedSlots;
  retiredCommittedSlots.reserve(pending.leavingChunks.size() + pending.rebuiltChunks.size());
  std::vector<std::array<int32_t, 3>> replacedOrLeaving = pending.leavingChunks;
  replacedOrLeaving.insert(replacedOrLeaving.end(), pending.rebuiltChunks.begin(), pending.rebuiltChunks.end());
  std::sort(replacedOrLeaving.begin(), replacedOrLeaving.end());
  replacedOrLeaving.erase(std::unique(replacedOrLeaving.begin(), replacedOrLeaving.end()), replacedOrLeaving.end());
  for (const auto &worldChunk : replacedOrLeaving)
  {
    uint32_t index = 0u;
    if (!descriptorForChunk(state_, directory_, worldChunk, index, error))
      return false;
    const auto &descriptor = directory_[index];
    if ((descriptor.flags & kSparseVoxelTree64ChunkUnknown) != 0u)
      continue;
    const uint32_t slot = descriptor.nodeSlot;
    if (slot >= slotStates_.size() || slotStates_[slot] != SlotState::Committed)
      return fail(error, "Tree64 replaced chunk does not own a committed node slot");
    retiredCommittedSlots.push_back(slot);
  }

  std::vector<SparseVoxelTree64ChunkGPU> nextDirectory = pending.fullRebuild ? std::vector<SparseVoxelTree64ChunkGPU>(directory_.size()) : directory_;
  for (const auto &worldChunk : pending.retainedChunks)
  {
    uint32_t oldIndex = 0u;
    uint32_t newIndex = 0u;
    if (!descriptorForChunk(state_, directory_, worldChunk, oldIndex, error))
      return false;
    std::array<uint32_t, 3> physical{};
    if (!sparseVoxelTree64DirectorySlot(worldChunk, pending.targetState.originWorldChunk, pending.targetState.logicalChunkResolution, pending.targetState.wrapOffset, physical))
      return fail(error, "Tree64 retained chunk is outside the target directory");
    newIndex = physicalIndex(physical, pending.targetState.logicalChunkResolution);
    if (oldIndex != newIndex)
      return fail(error, "Tree64 toroidal scroll moved a retained physical slot");
    nextDirectory[newIndex] = directory_[oldIndex];
  }
  for (size_t index = 0u; index < pending.buildChunks.size(); ++index)
  {
    std::array<uint32_t, 3> physical{};
    if (!sparseVoxelTree64DirectorySlot(pending.buildChunks[index], pending.targetState.originWorldChunk, pending.targetState.logicalChunkResolution, pending.targetState.wrapOffset, physical))
      return fail(error, "Tree64 build chunk is outside the target directory");
    const uint32_t directoryIndex = physicalIndex(physical, pending.targetState.logicalChunkResolution);
    const auto &result = results[index];
    if (successful[index] != 0u)
      nextDirectory[directoryIndex] = {
        pending.buildChunks[index],
        result.nodeSlot,
        result.rootNodeOffset,
        result.nodeCount,
        result.contentGeneration,
        kSparseVoxelTree64ChunkValid | (occupied[index] != 0u ? kSparseVoxelTree64ChunkOccupied : 0u),
      };
    else
      nextDirectory[directoryIndex] = {
        pending.buildChunks[index],
        0u,
        0u,
        0u,
        result.contentGeneration,
        kSparseVoxelTree64ChunkValid | kSparseVoxelTree64ChunkUnknown,
      };
  }

  for (size_t index = 0u; index < pending.buildJobs.size(); ++index)
  {
    const uint32_t slot = pending.buildJobs[index].nodeSlot;
    if (successful[index] != 0u)
      slotStates_[slot] = SlotState::Committed;
    else
      releaseSlot(slot, retireAfterCompletion);
  }
  for (const uint32_t slot : retiredCommittedSlots)
    releaseSlot(slot, retireAfterCompletion);
  directory_ = std::move(nextDirectory);
  state_ = pending.targetState;
  if (pendingSceneBound_)
    sceneRevision_ = pending.sceneRevision;
  pending_.reset();
  pendingSceneBound_ = false;
  sortFreeSlots();
  refreshStats();
  return true;
}

bool SparseVoxelTree64Directory::abortPending(const uint64_t retireAfterCompletion, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!pending_.has_value())
    return fail(error, "Tree64 directory has no pending update");
  if (retireAfterCompletion < completedSubmission_)
    return fail(error, "Tree64 abort completion precedes the completed submission");
  if (retireAfterCompletion < latestRetirementSubmission_)
    return fail(error, "Tree64 retirement submission token moved backwards");
  for (const auto &job : pending_->buildJobs)
  {
    if (job.nodeSlot >= slotStates_.size() || slotStates_[job.nodeSlot] != SlotState::Reserved)
      return fail(error, "Tree64 pending update lost a reserved slot");
  }
  for (const auto &job : pending_->buildJobs)
    releaseSlot(job.nodeSlot, retireAfterCompletion);
  pending_.reset();
  pendingSceneBound_ = false;
  sortFreeSlots();
  refreshStats();
  return true;
}

bool SparseVoxelTree64Directory::reclaimCompleted(const uint64_t completedSubmission, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (completedSubmission < completedSubmission_)
    return fail(error, "Tree64 completed submission token moved backwards");
  completedSubmission_ = completedSubmission;
  auto iterator = retiredSlots_.begin();
  while (iterator != retiredSlots_.end())
  {
    if (iterator->completion > completedSubmission_)
    {
      ++iterator;
      continue;
    }
    if (iterator->slot >= slotStates_.size() || slotStates_[iterator->slot] != SlotState::Retired)
      return fail(error, "Tree64 retirement queue contains an invalid slot");
    slotStates_[iterator->slot] = SlotState::Free;
    freeSlots_.push_back(iterator->slot);
    iterator = retiredSlots_.erase(iterator);
  }
  sortFreeSlots();
  refreshStats();
  return true;
}

bool SparseVoxelTree64Directory::configured() const
{
  return configured_;
}

bool SparseVoxelTree64Directory::hasPendingUpdate() const
{
  return pending_.has_value();
}

const SparseVoxelTree64StateGPU &SparseVoxelTree64Directory::state() const
{
  return state_;
}

const std::vector<SparseVoxelTree64ChunkGPU> &SparseVoxelTree64Directory::directory() const
{
  return directory_;
}

const SparseVoxelTree64DirectoryUpdate *SparseVoxelTree64Directory::pendingUpdate() const
{
  return pending_.has_value() ? &*pending_ : nullptr;
}

SparseVoxelTree64DirectoryStats SparseVoxelTree64Directory::stats() const
{
  return stats_;
}

uint64_t SparseVoxelTree64Directory::completedSubmission() const
{
  return completedSubmission_;
}

uint64_t SparseVoxelTree64Directory::sceneRevision() const
{
  return sceneRevision_;
}

void SparseVoxelTree64Directory::refreshStats()
{
  stats_.nodeSlotCount = static_cast<uint32_t>(slotStates_.size());
  stats_.nodesPerSlot = settings_.nodeSlotCapacity;
  stats_.freeSlots = 0u;
  stats_.reservedSlots = 0u;
  stats_.committedSlots = 0u;
  stats_.retiredSlots = 0u;
  for (const SlotState state : slotStates_)
  {
    switch (state)
    {
    case SlotState::Free:
      ++stats_.freeSlots;
      break;
    case SlotState::Reserved:
      ++stats_.reservedSlots;
      break;
    case SlotState::Committed:
      ++stats_.committedSlots;
      break;
    case SlotState::Retired:
      ++stats_.retiredSlots;
      break;
    }
  }
  stats_.highWatermark = std::max(stats_.highWatermark, stats_.nodeSlotCount - stats_.freeSlots);
}

void SparseVoxelTree64Directory::releaseSlot(const uint32_t slot, const uint64_t completion)
{
  latestRetirementSubmission_ = std::max(latestRetirementSubmission_, completion);
  if (completion <= completedSubmission_)
  {
    slotStates_[slot] = SlotState::Free;
    freeSlots_.push_back(slot);
    return;
  }
  slotStates_[slot] = SlotState::Retired;
  retiredSlots_.push_back({slot, completion});
}

void SparseVoxelTree64Directory::sortFreeSlots()
{
  std::sort(freeSlots_.begin(), freeSlots_.end(), std::greater<uint32_t>());
}

} // namespace voxel::svraster
