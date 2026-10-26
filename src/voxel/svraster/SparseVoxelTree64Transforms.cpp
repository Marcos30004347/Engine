#include "voxel/svraster/SparseVoxelTree64Transforms.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

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

bool validBounds(const SparseVoxelAABB &bounds)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    if (!std::isfinite(bounds.minimum[axis]) || !std::isfinite(bounds.maximum[axis]) || bounds.minimum[axis] > bounds.maximum[axis])
      return false;
  return true;
}

bool validMatrix(const std::array<float, 16> &matrix)
{
  for (const float value : matrix)
    if (!std::isfinite(value))
      return false;
  return true;
}

bool recordLess(const SparseVoxelTree64InstanceTransformRecord &left, const SparseVoxelTree64InstanceTransformRecord &right)
{
  return left.stableInstanceId < right.stableInstanceId;
}

bool validateRecords(const std::vector<SparseVoxelTree64InstanceTransformRecord> &records, std::string *error)
{
  uint32_t previousId = 0u;
  bool first = true;
  for (const auto &record : records)
  {
    if (!first && record.stableInstanceId <= previousId)
      return fail(error, "Tree64 transform records are not uniquely sorted");
    if (!validMatrix(record.modelToWorld) || !validBounds(record.worldBounds))
      return fail(error, "Tree64 transform record is invalid");
    first = false;
    previousId = record.stableInstanceId;
  }
  return true;
}

bool sameRecord(const SparseVoxelTree64InstanceTransformRecord &left, const SparseVoxelTree64InstanceTransformRecord &right)
{
  return left.blasIndex == right.blasIndex && left.modelToWorld == right.modelToWorld && left.worldBounds.minimum == right.worldBounds.minimum && left.worldBounds.maximum == right.worldBounds.maximum;
}

void appendChanged(const SparseVoxelTree64InstanceTransformRecord *previous, const SparseVoxelTree64InstanceTransformRecord *current, SparseVoxelTree64TransformUpdate &update)
{
  const uint32_t id = current != nullptr ? current->stableInstanceId : previous->stableInstanceId;
  update.changedInstanceIds.push_back(id);
  if (previous != nullptr)
    update.invalidatedWorldBounds.push_back(previous->worldBounds);
  if (current != nullptr)
    update.invalidatedWorldBounds.push_back(current->worldBounds);
}

} // namespace

bool sparseVoxelTree64CaptureInstanceTransforms(const SparseVoxelSceneManager &scene, std::vector<SparseVoxelTree64InstanceTransformRecord> &records, std::string *error)
{
  if (error != nullptr)
    error->clear();
  records.clear();
  records.reserve(scene.runtimeInstances().size());
  for (const auto &instance : scene.runtimeInstances())
  {
    records.push_back({
      instance.stableId,
      instance.gpu.blasIndex,
      instance.gpu.modelToWorld,
      {
        {
          instance.gpu.worldBoundsMin[0],
          instance.gpu.worldBoundsMin[1],
          instance.gpu.worldBoundsMin[2],
        },
        {
          instance.gpu.worldBoundsMax[0],
          instance.gpu.worldBoundsMax[1],
          instance.gpu.worldBoundsMax[2],
        },
      },
    });
  }
  std::sort(records.begin(), records.end(), recordLess);
  return validateRecords(records, error);
}

bool sparseVoxelTree64PlanTransformInvalidation(
    const SparseVoxelTree64StateGPU &state,
    const std::vector<SparseVoxelTree64InstanceTransformRecord> &previous,
    const std::vector<SparseVoxelTree64InstanceTransformRecord> &current,
    const bool topologyReplacement,
    const bool completePreviousBounds,
    SparseVoxelTree64TransformUpdate &update,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto revisions = std::array{
    update.transaction,
    update.previousSceneRevision,
    update.targetSceneRevision,
    update.previousTopologyRevision,
    update.targetTopologyRevision,
    update.previousTransformRevision,
    update.targetTransformRevision,
  };
  update = {};
  update.transaction = revisions[0];
  update.previousSceneRevision = revisions[1];
  update.targetSceneRevision = revisions[2];
  update.previousTopologyRevision = revisions[3];
  update.targetTopologyRevision = revisions[4];
  update.previousTransformRevision = revisions[5];
  update.targetTransformRevision = revisions[6];
  update.topologyReplacement = topologyReplacement;

  if (!validateRecords(previous, error) || !validateRecords(current, error))
    return false;
  if (!completePreviousBounds)
  {
    update.fullActiveRebuild = true;
    SparseVoxelTree64SceneClassificationStats classification{};
    if (!sparseVoxelTree64ClassifyDirtyChunks(state, {}, false, update.dirtyChunks, &classification, error))
      return false;
    return true;
  }

  if (topologyReplacement)
  {
    std::set<uint32_t> ids;
    for (const auto &record : previous)
    {
      ids.insert(record.stableInstanceId);
      update.invalidatedWorldBounds.push_back(record.worldBounds);
    }
    for (const auto &record : current)
    {
      ids.insert(record.stableInstanceId);
      update.invalidatedWorldBounds.push_back(record.worldBounds);
    }
    update.changedInstanceIds.assign(ids.begin(), ids.end());
  }
  else
  {
    size_t previousIndex = 0u;
    size_t currentIndex = 0u;
    while (previousIndex < previous.size() || currentIndex < current.size())
    {
      if (currentIndex >= current.size() || (previousIndex < previous.size() && previous[previousIndex].stableInstanceId < current[currentIndex].stableInstanceId))
      {
        appendChanged(&previous[previousIndex++], nullptr, update);
      }
      else if (previousIndex >= previous.size() || current[currentIndex].stableInstanceId < previous[previousIndex].stableInstanceId)
      {
        appendChanged(nullptr, &current[currentIndex++], update);
      }
      else
      {
        if (!sameRecord(previous[previousIndex], current[currentIndex]))
          appendChanged(&previous[previousIndex], &current[currentIndex], update);
        ++previousIndex;
        ++currentIndex;
      }
    }
  }

  SparseVoxelTree64SceneClassificationStats classification{};
  if (!sparseVoxelTree64ClassifyDirtyChunks(state, update.invalidatedWorldBounds, true, update.dirtyChunks, &classification, error))
    return false;
  return true;
}

bool SparseVoxelTree64TransformTracker::initialize(const SparseVoxelSceneManager &scene, std::string *error)
{
  if (error != nullptr)
    error->clear();
  std::vector<SparseVoxelTree64InstanceTransformRecord> records;
  if (!sparseVoxelTree64CaptureInstanceTransforms(scene, records, error))
    return false;
  published_ = std::move(records);
  pending_.reset();
  sceneRevision_ = scene.sceneRevision();
  topologyRevision_ = scene.topologyRevision();
  transformRevision_ = scene.transformRevision();
  nextTransaction_ = 1u;
  stats_ = {};
  stats_.trackedInstances = static_cast<uint32_t>(published_.size());
  initialized_ = true;
  return true;
}

bool SparseVoxelTree64TransformTracker::prepare(const SparseVoxelSceneManager &scene, const SparseVoxelTree64StateGPU &state, SparseVoxelTree64TransformUpdate &update, std::string *error)
{
  if (error != nullptr)
    error->clear();
  update = {};
  if (!initialized_)
    return fail(error, "Tree64 transform tracker is not initialized");
  if (pending_.has_value())
    return fail(error, "Tree64 transform tracker already has a pending update");
  if (scene.sceneRevision() < sceneRevision_ || scene.topologyRevision() < topologyRevision_ || scene.transformRevision() < transformRevision_)
    return fail(error, "Tree64 transform source revision is stale");

  std::vector<SparseVoxelTree64InstanceTransformRecord> current;
  if (!sparseVoxelTree64CaptureInstanceTransforms(scene, current, error))
    return false;
  if (nextTransaction_ == std::numeric_limits<uint64_t>::max())
    return fail(error, "Tree64 transform transaction space is exhausted");
  update.transaction = nextTransaction_++;
  update.previousSceneRevision = sceneRevision_;
  update.targetSceneRevision = scene.sceneRevision();
  update.previousTopologyRevision = topologyRevision_;
  update.targetTopologyRevision = scene.topologyRevision();
  update.previousTransformRevision = transformRevision_;
  update.targetTransformRevision = scene.transformRevision();
  update.topologyReplacement = scene.topologyRevision() != topologyRevision_;
  if (!sparseVoxelTree64PlanTransformInvalidation(state, published_, current, update.topologyReplacement, true, update, error))
    return false;

  pending_ = Pending{update, std::move(current)};
  ++stats_.preparedUpdates;
  stats_.changedInstances = static_cast<uint32_t>(update.changedInstanceIds.size());
  stats_.invalidationBounds = static_cast<uint32_t>(update.invalidatedWorldBounds.size());
  stats_.dirtyChunks = static_cast<uint32_t>(update.dirtyChunks.size());
  const uint64_t activeChunks = uint64_t{state.logicalChunkResolution[0]} * state.logicalChunkResolution[1] * state.logicalChunkResolution[2];
  stats_.activeChunks = static_cast<uint32_t>(std::min<uint64_t>(activeChunks, std::numeric_limits<uint32_t>::max()));
  if (update.fullActiveRebuild)
    ++stats_.fullActiveRebuilds;
  return true;
}

bool SparseVoxelTree64TransformTracker::commit(const SparseVoxelSceneManager &scene, const SparseVoxelTree64TransformUpdate &update, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!pending_.has_value() || update.transaction != pending_->update.transaction)
    return fail(error, "Tree64 transform update transaction is stale");
  if (scene.sceneRevision() != pending_->update.targetSceneRevision || scene.topologyRevision() != pending_->update.targetTopologyRevision || scene.transformRevision() != pending_->update.targetTransformRevision)
  {
    ++stats_.staleCommits;
    return fail(error, "Tree64 transform scene changed before publication");
  }
  published_ = std::move(pending_->records);
  sceneRevision_ = pending_->update.targetSceneRevision;
  topologyRevision_ = pending_->update.targetTopologyRevision;
  transformRevision_ = pending_->update.targetTransformRevision;
  pending_.reset();
  stats_.trackedInstances = static_cast<uint32_t>(published_.size());
  ++stats_.committedUpdates;
  return true;
}

bool SparseVoxelTree64TransformTracker::abort(const SparseVoxelTree64TransformUpdate &update, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!pending_.has_value() || update.transaction != pending_->update.transaction)
    return fail(error, "Tree64 transform update transaction is stale");
  pending_.reset();
  return true;
}

bool SparseVoxelTree64TransformTracker::initialized() const
{
  return initialized_;
}

bool SparseVoxelTree64TransformTracker::hasPendingUpdate() const
{
  return pending_.has_value();
}

uint64_t SparseVoxelTree64TransformTracker::sceneRevision() const
{
  return sceneRevision_;
}

uint64_t SparseVoxelTree64TransformTracker::topologyRevision() const
{
  return topologyRevision_;
}

uint64_t SparseVoxelTree64TransformTracker::transformRevision() const
{
  return transformRevision_;
}

const std::vector<SparseVoxelTree64InstanceTransformRecord> &SparseVoxelTree64TransformTracker::publishedRecords() const
{
  return published_;
}

SparseVoxelTree64TransformStats SparseVoxelTree64TransformTracker::stats() const
{
  return stats_;
}

bool beginSparseVoxelTree64TransformRebuild(
    SparseVoxelTree64Directory &directory,
    const std::array<float, 3> &cameraWorldPosition,
    const SparseVoxelTree64TransformUpdate &transformUpdate,
    SparseVoxelTree64DirectoryUpdate &directoryUpdate,
    std::string *error)
{
  return directory.beginSceneUpdate(cameraWorldPosition, transformUpdate.targetSceneRevision, transformUpdate.dirtyChunks, !transformUpdate.fullActiveRebuild, directoryUpdate, error);
}

} // namespace voxel::svraster
