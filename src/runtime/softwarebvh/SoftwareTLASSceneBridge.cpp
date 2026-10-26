#include "softwarebvh/SoftwareTLASSceneBridge.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <unordered_set>

#include "softwarebvh/SoftwareBLASManager.hpp"

namespace softwarebvh
{

namespace
{

using virtualgeometry::AABB;
using virtualgeometry::VirtualGeometryScene;

static std::string buildShapeSourceKey(const std::string &virtualGeometryPath, const std::optional<uint32_t> shapeIndex)
{
  if (!shapeIndex.has_value())
  {
    return virtualGeometryPath;
  }

  return virtualGeometryPath + "::shape" + std::to_string(*shapeIndex);
}

static std::string buildShapeInstanceSourceKey(const std::string &shapeSourceKey, const virtualgeometry::SceneInstanceId sceneInstanceId)
{
  return shapeSourceKey + "::sceneInstance" + std::to_string(sceneInstanceId);
}

} // namespace

SoftwareTLASSceneBridge::SoftwareTLASSceneBridge(SoftwareTLASManager &manager) : manager_(manager)
{
}

SoftwareTLASSceneBridge::SoftwareTLASSceneBridge(SoftwareTLASManager &manager, const SoftwareBLASManager *blasManager) : manager_(manager), blasManager_(blasManager)
{
}

void SoftwareTLASSceneBridge::sync(VirtualGeometryScene &scene)
{
  diagnostics_ = Diagnostics{};

  std::vector<SoftwareTLASManager::InstanceBuildInput> inputs;
  inputs.reserve(scene.instanceManager.getInstanceCount());
  std::vector<uint32_t> dirtyInputIndices;
  std::vector<virtualgeometry::SceneInstanceId> currentInstanceOrder;
  currentInstanceOrder.reserve(scene.instanceManager.getInstanceCount());
  std::unordered_set<virtualgeometry::SceneInstanceId> transformUpdatedSceneInstances;
  const uint32_t frameSlot = scene.renderGraph == nullptr ? 0u : scene.renderGraph->getCurrentFrameIndex();
  const auto &transformUpdates = scene.getFrameTransformUpdates(frameSlot);
  transformUpdatedSceneInstances.reserve(transformUpdates.size());
  for (const auto &update : transformUpdates)
  {
    if (update.sceneInstanceId != virtualgeometry::INVALID_SCENE_INSTANCE_ID)
      transformUpdatedSceneInstances.insert(update.sceneInstanceId);
  }

  for (auto &[_, state] : instanceStates_)
  {
    state.seenThisFrame = false;
  }

  bool requestedRebuild = false;

  for (uint32_t denseIndex = 0u; denseIndex < static_cast<uint32_t>(scene.objectInstances.size()); ++denseIndex)
  {
    if (denseIndex >= scene.objectDenseToSparse.size())
    {
      continue;
    }

    const virtualgeometry::InstanceId objectInstanceId = scene.objectDenseToSparse[denseIndex];
    if (objectInstanceId == virtualgeometry::INVALID_INSTANCE_ID)
    {
      continue;
    }

    ++diagnostics_.preparedInstanceCount;

    const VirtualGeometryScene::ObjectInstanceRecord &record = scene.objectInstances[denseIndex];
    const auto runtimeData = scene.files.find(record.objectName);
    if (runtimeData == scene.files.end())
    {
      ++diagnostics_.skippedMissingObjectCount;
      requestedRebuild = true;
      continue;
    }

    const VirtualGeometryScene::VirtualGeometryObjectRuntimeData *objectData = runtimeData.value();
    const uint32_t shapeCount = std::max<uint32_t>(1u, static_cast<uint32_t>(record.sceneInstanceIds.size()));
    for (uint32_t shapeIndex = 0u; shapeIndex < shapeCount; ++shapeIndex)
    {
      const virtualgeometry::SceneInstanceId sceneInstanceId = shapeIndex < record.sceneInstanceIds.size() ? record.sceneInstanceIds[shapeIndex] : virtualgeometry::INVALID_SCENE_INSTANCE_ID;
      if (sceneInstanceId == virtualgeometry::INVALID_SCENE_INSTANCE_ID)
      {
        ++diagnostics_.skippedMissingSceneInstanceCount;
        requestedRebuild = true;
        continue;
      }

      const virtualgeometry::VirtualGeometryInstanceGPUData *sceneInstance = scene.instanceManager.getInstance(sceneInstanceId);
      if (sceneInstance == nullptr)
      {
        ++diagnostics_.skippedMissingSceneInstanceCount;
        requestedRebuild = true;
        continue;
      }

      const std::optional<uint32_t> optionalShapeIndex = objectData->shapes.empty() ? std::nullopt : std::optional<uint32_t>(shapeIndex);
      virtualgeometry::AABB localBounds = objectData->localBounds;
      uint32_t blasHandle = SoftwareTLASManager::INVALID_BLAS_HANDLE;
      const std::string shapeSourceKey = buildShapeSourceKey(objectData->file.getPath(), optionalShapeIndex);
      std::string blasSourceKey = shapeSourceKey;
      if (blasManager_ != nullptr)
      {
        blasHandle = blasManager_->findHandleBySourcePath(shapeSourceKey);
        if (blasHandle == SoftwareTLASManager::INVALID_BLAS_HANDLE)
        {
          const std::string instanceSourceKey = buildShapeInstanceSourceKey(shapeSourceKey, sceneInstanceId);
          blasHandle = blasManager_->findHandleBySourcePath(instanceSourceKey);
          if (blasHandle != SoftwareTLASManager::INVALID_BLAS_HANDLE)
          {
            blasSourceKey = instanceSourceKey;
          }
        }
        if (blasHandle == SoftwareTLASManager::INVALID_BLAS_HANDLE)
        {
          ++diagnostics_.skippedMissingBlasCount;
          requestedRebuild = true;
          continue;
        }

        virtualgeometry::AABB blasBounds{};
        if (blasManager_->tryGetLocalBounds(blasHandle, &blasBounds) && isValidAabb(blasBounds))
        {
          localBounds = blasBounds;
        }
      }

      if (!isValidAabb(localBounds))
      {
        ++diagnostics_.skippedInvalidBoundsCount;
        requestedRebuild = true;
        continue;
      }

      const AABB worldBounds = transformAabb(localBounds, sceneInstance->modelMatrix);
      if (!isValidAabb(worldBounds))
      {
        ++diagnostics_.skippedInvalidBoundsCount;
        requestedRebuild = true;
        continue;
      }

      const uint32_t candidateInstanceIndex = diagnostics_.candidateInstanceCount++;
      if (debugInstanceFilter_.isolateSingleInstance && candidateInstanceIndex != debugInstanceFilter_.selectedInstanceIndex)
      {
        ++diagnostics_.filteredInstanceCount;
        requestedRebuild = true;
        continue;
      }

      SoftwareTLASManager::InstanceBuildInput input{};
      input.instanceId = sceneInstanceId;
      input.blasHandle = blasHandle;
      input.flags = record.gpuSignedDistanceFields.dynamic ? SoftwareTLASManager::INSTANCE_FLAG_DYNAMIC : 0u;
      input.transform = sceneInstance->modelMatrix;
      input.previousTransform = sceneInstance->previousModelMatrix;
      input.worldBounds = worldBounds;
      input.sourceName = record.objectName;
      input.sourcePath = blasSourceKey;
      inputs.push_back(input);
      currentInstanceOrder.push_back(sceneInstanceId);
      diagnostics_.selectedSceneInstanceId = sceneInstanceId;
      ++diagnostics_.validInstanceCount;

      InstanceState &state = instanceStates_[sceneInstanceId];
      state.seenThisFrame = true;

      const bool rebuildDirty = state.rebuildRevision != record.gpuSignedDistanceFields.rebuildRevision;
      const bool blasDirty = state.blasHandle != input.blasHandle;
      const bool transformDirty = transformUpdatedSceneInstances.find(sceneInstanceId) != transformUpdatedSceneInstances.end() ||
                                  !state.hasCachedInputs || std::memcmp(state.transform.data, input.transform.data, sizeof(input.transform.data)) != 0 ||
                                  std::memcmp(state.previousTransform.data, input.previousTransform.data, sizeof(input.previousTransform.data)) != 0;
      const bool boundsDirty = !state.hasCachedInputs || !std::equal(std::begin(state.worldBounds.minPoint), std::end(state.worldBounds.minPoint), std::begin(input.worldBounds.minPoint)) ||
                               !std::equal(std::begin(state.worldBounds.maxPoint), std::end(state.worldBounds.maxPoint), std::begin(input.worldBounds.maxPoint));
      const bool refitDirty = !rebuildDirty && !blasDirty && (state.contentRevision != record.gpuSignedDistanceFields.contentRevision || transformDirty || boundsDirty);
      if (rebuildDirty || blasDirty || refitDirty)
      {
        ++diagnostics_.dirtyInstanceCount;
        if (rebuildDirty || blasDirty)
        {
          ++diagnostics_.rebuildDirtyInstanceCount;
        }
        else
        {
          ++diagnostics_.refitDirtyInstanceCount;
          dirtyInputIndices.push_back(static_cast<uint32_t>(inputs.size() - 1u));
        }
      }

      state.contentRevision = record.gpuSignedDistanceFields.contentRevision;
      state.rebuildRevision = record.gpuSignedDistanceFields.rebuildRevision;
      state.transform = input.transform;
      state.previousTransform = input.previousTransform;
      state.worldBounds = input.worldBounds;
      state.blasHandle = input.blasHandle;
      state.hasCachedInputs = true;
    }
  }

  bool removedInstances = false;
  for (auto it = instanceStates_.begin(); it != instanceStates_.end();)
  {
    if (!it->second.seenThisFrame)
    {
      removedInstances = true;
      it = instanceStates_.erase(it);
    }
    else
    {
      ++it;
    }
  }

  const bool orderChanged = currentInstanceOrder != previousInstanceOrder_;
  diagnostics_.topologyChanged = removedInstances || requestedRebuild || orderChanged || diagnostics_.rebuildDirtyInstanceCount > 0u || manager_.getLastBuildSummary().instanceCount != diagnostics_.validInstanceCount;

  diagnostics_.requestedRebuild = diagnostics_.topologyChanged;
  diagnostics_.requestedRefit = !diagnostics_.requestedRebuild && !dirtyInputIndices.empty();

  if (diagnostics_.requestedRebuild)
  {
    manager_.rebuild(inputs, diagnostics_.dirtyInstanceCount);
  }
  else if (diagnostics_.requestedRefit)
  {
    manager_.refit(inputs, dirtyInputIndices);
  }
  else
  {
    manager_.markClean();
  }

  previousInstanceOrder_ = std::move(currentInstanceOrder);
}

void SoftwareTLASSceneBridge::setDebugInstanceFilter(const DebugInstanceFilter &filter)
{
  debugInstanceFilter_ = filter;
}

bool SoftwareTLASSceneBridge::isValidAabb(const AABB &bounds)
{
  return bounds.minPoint[0] <= bounds.maxPoint[0] && bounds.minPoint[1] <= bounds.maxPoint[1] && bounds.minPoint[2] <= bounds.maxPoint[2];
}

AABB SoftwareTLASSceneBridge::transformAabb(const AABB &localBounds, const math::Mat4f &transform)
{
  AABB transformed{};
  for (uint32_t cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
  {
    const math::Vec3f localCorner(
        (cornerIndex & 1u) ? localBounds.maxPoint[0] : localBounds.minPoint[0],
        (cornerIndex & 2u) ? localBounds.maxPoint[1] : localBounds.minPoint[1],
        (cornerIndex & 4u) ? localBounds.maxPoint[2] : localBounds.minPoint[2]);
    const math::Vec4f worldCorner = transform * math::Vec4f(localCorner[0], localCorner[1], localCorner[2], 1.0f);
    const float point[3] = {worldCorner[0], worldCorner[1], worldCorner[2]};
    transformed.expandBy(point);
  }
  return transformed;
}

AABB SoftwareTLASSceneBridge::computeWorldBounds(
    const VirtualGeometryScene::InstanceManager &instanceManager,
    const std::vector<virtualgeometry::SceneInstanceId> &sceneInstanceIds,
    const AABB &localBounds,
    bool *outHadMissingSceneInstance)
{
  AABB worldBounds{};
  bool hasWorldBounds = false;
  bool hadMissingSceneInstance = false;

  for (const virtualgeometry::SceneInstanceId sceneInstanceId : sceneInstanceIds)
  {
    const virtualgeometry::VirtualGeometryInstanceGPUData *instance = instanceManager.getInstance(sceneInstanceId);
    if (instance == nullptr)
    {
      hadMissingSceneInstance = true;
      continue;
    }

    const AABB instanceBounds = transformAabb(localBounds, instance->modelMatrix);
    if (!hasWorldBounds)
    {
      worldBounds = instanceBounds;
      hasWorldBounds = true;
    }
    else
    {
      worldBounds.expandBy(instanceBounds);
    }
  }

  if (outHadMissingSceneInstance != nullptr)
  {
    *outHadMissingSceneInstance = hadMissingSceneInstance;
  }

  return worldBounds;
}

} // namespace softwarebvh
