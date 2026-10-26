#include "gpusigneddistancefields/GPUSignedDistanceFieldsSceneBridge.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>

#include "os/Logger.hpp"
#include "virtualgeometry/VirtualMaterialFile.hpp"

namespace gpusigneddistancefields
{

namespace
{

using virtualgeometry::AABB;
using virtualgeometry::VirtualGeometryScene;

static GPUSignedDistanceFieldsManager::SurfaceMaterialGPU makeSurfaceMaterialFromDefinition(const virtualgeometry::Material::Definition &definition)
{
  GPUSignedDistanceFieldsManager::SurfaceMaterialGPU material{};
  for (uint32_t channel = 0u; channel < 4u; ++channel)
  {
    material.baseColorOpacity[channel] = definition.baseColorFactor[channel];
  }
  material.baseColorOpacity[3] = std::clamp(definition.baseColorFactor[3] * definition.opacity, 0.0f, 1.0f);
  material.normalRoughness[3] = std::clamp(definition.roughnessFactor, 0.0f, 1.0f);
  material.emissiveMetallic[0] = std::max(0.0f, definition.emissiveFactor[0] * definition.emissiveStrength);
  material.emissiveMetallic[1] = std::max(0.0f, definition.emissiveFactor[1] * definition.emissiveStrength);
  material.emissiveMetallic[2] = std::max(0.0f, definition.emissiveFactor[2] * definition.emissiveStrength);
  material.emissiveMetallic[3] = std::clamp(definition.metallicFactor, 0.0f, 1.0f);
  return material;
}

static GPUSignedDistanceFieldsManager::SurfaceMaterialGPU loadFallbackSurfaceMaterial(
    const virtualgeometry::FallbackMeshData &mesh,
    const std::string &fallbackPath)
{
  GPUSignedDistanceFieldsManager::SurfaceMaterialGPU material{};
  const uint32_t meshMaterialIndex = mesh.sections.empty() ? 0u : mesh.sections.front().materialIndex;
  material.flags[0] = meshMaterialIndex;
  if (meshMaterialIndex >= mesh.materialFiles.size() || mesh.materialFiles[meshMaterialIndex].empty())
  {
    return material;
  }

  std::filesystem::path materialPath(mesh.materialFiles[meshMaterialIndex]);
  if (materialPath.is_relative())
  {
    materialPath = std::filesystem::path(fallbackPath).parent_path() / materialPath;
  }

  virtualgeometry::Material::Definition definition{};
  if (!virtualgeometry::VirtualMaterialFile::loadDefinition(materialPath.string(), definition))
  {
    return material;
  }

  material = makeSurfaceMaterialFromDefinition(definition);
  material.flags[0] = meshMaterialIndex;
  return material;
}

static std::vector<uint32_t> buildFallbackVertexMaterialIds(const virtualgeometry::FallbackMeshData &mesh)
{
  std::vector<uint32_t> materialIds(mesh.vertices.size(), 0u);
  for (const virtualgeometry::FallbackMeshSection &section : mesh.sections)
  {
    const uint32_t triangleBegin = section.indexOffset / 3u;
    const uint32_t triangleEnd = std::min<uint32_t>(
        static_cast<uint32_t>(mesh.indices.size() / 3u),
        (section.indexOffset + section.indexCount + 2u) / 3u);
    for (uint32_t triangleIndex = triangleBegin; triangleIndex < triangleEnd; ++triangleIndex)
    {
      const uint32_t baseIndex = triangleIndex * 3u;
      for (uint32_t corner = 0u; corner < 3u; ++corner)
      {
        const uint32_t vertexIndex = mesh.indices[baseIndex + corner];
        if (vertexIndex < materialIds.size())
          materialIds[vertexIndex] = section.materialIndex;
      }
    }
  }
  return materialIds;
}

static std::filesystem::path buildFallbackMeshPath(const std::string &virtualGeometryPath, uint32_t lodLevel)
{
  const std::filesystem::path path = std::filesystem::path(virtualGeometryPath).lexically_normal();
  return path.parent_path() / (path.stem().string() + ".lod" + std::to_string(lodLevel) + ".fallback.mesh");
}

static std::vector<std::filesystem::path> buildFallbackMeshCandidates(const std::string &virtualGeometryPath, uint32_t requestedLod)
{
  static constexpr uint32_t kMaxFallbackLodSearch = 15u;
  std::vector<std::filesystem::path> candidates;
  candidates.reserve(kMaxFallbackLodSearch + 1u);

  const uint32_t clampedRequestedLod = std::min(requestedLod, kMaxFallbackLodSearch);
  candidates.push_back(buildFallbackMeshPath(virtualGeometryPath, clampedRequestedLod));
  for (uint32_t lodLevel = clampedRequestedLod + 1u; lodLevel <= kMaxFallbackLodSearch; ++lodLevel)
  {
    candidates.push_back(buildFallbackMeshPath(virtualGeometryPath, lodLevel));
  }
  for (uint32_t lodLevel = clampedRequestedLod; lodLevel > 0u; --lodLevel)
  {
    candidates.push_back(buildFallbackMeshPath(virtualGeometryPath, lodLevel - 1u));
  }
  return candidates;
}

static bool isValidAabb(const AABB &bounds)
{
  return bounds.minPoint[0] <= bounds.maxPoint[0] &&
         bounds.minPoint[1] <= bounds.maxPoint[1] &&
         bounds.minPoint[2] <= bounds.maxPoint[2];
}

static bool intersectsAabb(const AABB &bounds, const math::Vec3f &minPoint, const math::Vec3f &maxPoint)
{
  return bounds.maxPoint[0] >= minPoint[0] && bounds.minPoint[0] <= maxPoint[0] &&
         bounds.maxPoint[1] >= minPoint[1] && bounds.minPoint[1] <= maxPoint[1] &&
         bounds.maxPoint[2] >= minPoint[2] && bounds.minPoint[2] <= maxPoint[2];
}

static AABB expandAabb(const AABB &bounds, float amount)
{
  AABB expanded = bounds;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    expanded.minPoint[axis] -= amount;
    expanded.maxPoint[axis] += amount;
  }
  return expanded;
}

static AABB makeFallbackAabb(const virtualgeometry::FallbackMeshData &mesh)
{
  AABB bounds;
  bounds.minPoint[0] = mesh.boundsMin[0];
  bounds.minPoint[1] = mesh.boundsMin[1];
  bounds.minPoint[2] = mesh.boundsMin[2];
  bounds.maxPoint[0] = mesh.boundsMax[0];
  bounds.maxPoint[1] = mesh.boundsMax[1];
  bounds.maxPoint[2] = mesh.boundsMax[2];
  return bounds;
}

static AABB transformAabb(const AABB &localBounds, const math::Mat4f &transform)
{
  AABB transformed;
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

static bool matricesEqual(const math::Mat4f &a, const math::Mat4f &b, float epsilon = 1.0e-5f)
{
  for (size_t index = 0u; index < 16u; ++index)
  {
    if (std::abs(a.data[index] - b.data[index]) > epsilon)
    {
      return false;
    }
  }
  return true;
}

static bool palettesEqual(const std::vector<math::Mat4f> &a, const std::vector<math::Mat4f> &b, float epsilon = 1.0e-5f)
{
  if (a.size() != b.size())
  {
    return false;
  }

  for (size_t index = 0u; index < a.size(); ++index)
  {
    if (!matricesEqual(a[index], b[index], epsilon))
    {
      return false;
    }
  }

  return true;
}

static math::Vec3f skinVertexPosition(
    const virtualgeometry::FallbackMeshVertex &vertex,
    const std::vector<math::Mat4f> &palette)
{
  const math::Vec4f localPosition(vertex.position[0], vertex.position[1], vertex.position[2], 1.0f);

  if (palette.empty())
  {
    return math::Vec3f(localPosition[0], localPosition[1], localPosition[2]);
  }

  math::Vec4f skinnedPosition(0.0f, 0.0f, 0.0f, 0.0f);
  float totalWeight = 0.0f;
  for (uint32_t influenceIndex = 0u; influenceIndex < 4u; ++influenceIndex)
  {
    const float weight = vertex.boneWeights[influenceIndex];
    const uint32_t boneIndex = vertex.boneIndices[influenceIndex];
    if (weight <= 0.0f || boneIndex >= palette.size())
    {
      continue;
    }

    const math::Vec4f transformed = palette[boneIndex] * localPosition;
    for (uint32_t component = 0u; component < 4u; ++component)
    {
      skinnedPosition[component] += transformed[component] * weight;
    }
    totalWeight += weight;
  }

  if (totalWeight <= 1.0e-5f)
  {
    return math::Vec3f(localPosition[0], localPosition[1], localPosition[2]);
  }

  return math::Vec3f(skinnedPosition[0], skinnedPosition[1], skinnedPosition[2]);
}

static AABB computeSkinnedWorldBounds(
    const virtualgeometry::FallbackMeshData &mesh,
    const math::Mat4f &modelMatrix,
    const std::vector<math::Mat4f> &palette)
{
  AABB bounds;
  for (const virtualgeometry::FallbackMeshVertex &vertex : mesh.vertices)
  {
    const math::Vec3f skinnedLocal = skinVertexPosition(vertex, palette);
    const math::Vec4f worldPosition = modelMatrix * math::Vec4f(skinnedLocal[0], skinnedLocal[1], skinnedLocal[2], 1.0f);
    const float point[3] = {worldPosition[0], worldPosition[1], worldPosition[2]};
    bounds.expandBy(point);
  }
  return bounds;
}

static uint32_t clampVoxelMin(float value, float gridMin, float invVoxelSize, uint32_t resolution)
{
  const int32_t quantized = static_cast<int32_t>(std::floor((value - gridMin) * invVoxelSize));
  return static_cast<uint32_t>(std::max<int32_t>(0, std::min<int32_t>(quantized, static_cast<int32_t>(resolution))));
}

static uint32_t clampVoxelMax(float value, float gridMin, float invVoxelSize, uint32_t resolution)
{
  const int32_t quantized = static_cast<int32_t>(std::ceil((value - gridMin) * invVoxelSize));
  return static_cast<uint32_t>(std::max<int32_t>(0, std::min<int32_t>(quantized, static_cast<int32_t>(resolution))));
}

} // namespace

GPUSignedDistanceFieldsSceneBridge::GPUSignedDistanceFieldsSceneBridge(GPUSignedDistanceFieldsManager &manager) : GPUSignedDistanceFieldsSceneBridge(manager, Settings{})
{
}

GPUSignedDistanceFieldsSceneBridge::GPUSignedDistanceFieldsSceneBridge(GPUSignedDistanceFieldsManager &manager, Settings settings) : manager_(manager), settings_(settings)
{
}

bool GPUSignedDistanceFieldsSceneBridge::isQueued(virtualgeometry::InstanceId instanceId) const
{
  return queuedInstances_.find(instanceId) != queuedInstances_.end();
}

bool GPUSignedDistanceFieldsSceneBridge::hasCompletedInitialBuild(virtualgeometry::InstanceId instanceId) const
{
  const auto stateIt = recordBuildStates_.find(instanceId);
  return stateIt != recordBuildStates_.end() && stateIt->second.hasCompletedInitialBuild;
}

void GPUSignedDistanceFieldsSceneBridge::setSettings(Settings settings)
{
  if (settings.fallbackLod != settings_.fallbackLod)
  {
    recordBuildStates_.clear();
    pendingUpdateQueue_.clear();
    queuedInstances_.clear();
  }
  settings_ = settings;
}

void GPUSignedDistanceFieldsSceneBridge::sync(VirtualGeometryScene &scene, const math::Vec3f &clipmapCenter)
{
  struct PreparedRecord
  {
    uint32_t denseIndex = UINT32_MAX;
    uint32_t meshIndex = UINT32_MAX;
    uint32_t pathInstanceIndex = UINT32_MAX;
    bool hasSkinning = false;
    AABB currentWorldBounds;
  };

  manager_.updateClipmaps(clipmapCenter);
  lastFrameUpdatedCount_ = 0u;
  diagnostics_.preparedInstanceCount = 0u;
  diagnostics_.staticInstanceCount = 0u;
  diagnostics_.skinnedInstanceCount = 0u;
  diagnostics_.uploadedMeshCount = 0u;
  diagnostics_.uploadedVertexCount = 0u;
  diagnostics_.uploadedIndexCount = 0u;
  diagnostics_.uploadedBoneWeightCount = 0u;
  diagnostics_.skippedEmptySceneInstanceCount = 0u;
  diagnostics_.skippedMissingRuntimeDataCount = 0u;
  diagnostics_.skippedMissingFallbackMeshCount = 0u;
  diagnostics_.skippedMissingSceneInstanceCount = 0u;
  diagnostics_.skippedInvalidBoundsCount = 0u;
  diagnostics_.staticJobCounts.assign(manager_.getCascadeCount(), 0u);
  diagnostics_.skinnedJobCounts.assign(manager_.getCascadeCount(), 0u);
  diagnostics_.staticJobVoxelCounts.assign(manager_.getCascadeCount(), 0u);
  diagnostics_.skinnedJobVoxelCounts.assign(manager_.getCascadeCount(), 0u);

  for (auto stateIt = recordBuildStates_.begin(); stateIt != recordBuildStates_.end();)
  {
    const virtualgeometry::InstanceId instanceId = stateIt->first;
    const bool isAlive =
        instanceId < scene.objectSparseToDense.size() &&
        scene.objectSparseToDense[instanceId] != virtualgeometry::INVALID_INSTANCE_ID;
    if (!isAlive)
    {
      queuedInstances_.erase(instanceId);
      stateIt = recordBuildStates_.erase(stateIt);
    }
    else
    {
      ++stateIt;
    }
  }

  auto getOrCreateMeshIndex = [&](const std::string &virtualGeometryPath) -> uint32_t
  {
    CachedFallbackMesh *cached = loadFallbackMesh(virtualGeometryPath, settings_.fallbackLod);
    if (cached == nullptr || !cached->available)
    {
      return UINT32_MAX;
    }

    if (cached->meshIndex != UINT32_MAX)
    {
      return cached->meshIndex;
    }

    if ((cached->mesh.indices.size() % 3u) != 0u)
    {
      throw std::runtime_error(
          "Fallback mesh index buffer must contain complete triangles: " + cached->fallbackPath);
    }

    const uint32_t vertexOffset = static_cast<uint32_t>(vertices_.size());
    const uint32_t indexOffset = static_cast<uint32_t>(indices_.size());
    const uint32_t meshIndex = static_cast<uint32_t>(meshTable_.size());
    const bool isSkinnedMesh = cached->mesh.skeleton.getBoneCount() > 0u;
    const uint32_t boneWeightsPerVertex = isSkinnedMesh ? 4u : 0u;
    const std::vector<uint32_t> vertexMaterialIds = buildFallbackVertexMaterialIds(cached->mesh);

    for (uint32_t vertexIndex = 0u; vertexIndex < cached->mesh.vertices.size(); ++vertexIndex)
    {
      const virtualgeometry::FallbackMeshVertex &vertex = cached->mesh.vertices[vertexIndex];
      GPUSignedDistanceFieldsManager::VertexGPU gpuVertex{};
      gpuVertex.position = math::Vec3f(vertex.position[0], vertex.position[1], vertex.position[2]);
      gpuVertex.boneWeightOffset = static_cast<uint32_t>(boneWeights_.size());
      gpuVertex.uvMaterial[0] = vertex.uv[0];
      gpuVertex.uvMaterial[1] = vertex.uv[1];
      for (uint32_t channel = 0u; channel < 4u; ++channel)
      {
        gpuVertex.surfaceBaseColorOpacity[channel] = vertex.surfaceBaseColorOpacity[channel];
        gpuVertex.surfaceNormalRoughness[channel] = vertex.surfaceNormalRoughness[channel];
        gpuVertex.surfaceEmissiveMetallic[channel] = vertex.surfaceEmissiveMetallic[channel];
        gpuVertex.surfaceFlags[channel] = vertex.surfaceFlags[channel];
      }
      gpuVertex.surfaceFlags[0] = vertexIndex < vertexMaterialIds.size() ? vertexMaterialIds[vertexIndex] : 0u;
      vertices_.push_back(gpuVertex);

      if (isSkinnedMesh)
      {
        for (uint32_t influenceIndex = 0u; influenceIndex < 4u; ++influenceIndex)
        {
          GPUSignedDistanceFieldsManager::BoneWeightGPU gpuBoneWeight{};
          gpuBoneWeight.weight = vertex.boneWeights[influenceIndex];
          gpuBoneWeight.boneIndex = vertex.boneIndices[influenceIndex];
          boneWeights_.push_back(gpuBoneWeight);
        }
      }
    }

    indices_.insert(indices_.end(), cached->mesh.indices.begin(), cached->mesh.indices.end());

    GPUSignedDistanceFieldsManager::MeshTableEntryGPU meshEntry{};
    meshEntry.vertexOffset = vertexOffset;
    meshEntry.vertexCount = static_cast<uint32_t>(cached->mesh.vertices.size());
    meshEntry.indexOffset = indexOffset;
    meshEntry.triangleCount = static_cast<uint32_t>(cached->mesh.indices.size() / 3u);
    meshEntry.boneWeightsPerVertex = boneWeightsPerVertex;
    meshEntry.flags = isSkinnedMesh ? GPUSignedDistanceFieldsManager::MeshFlag_Skinned : GPUSignedDistanceFieldsManager::MeshFlag_None;
    meshEntry.surfaceMaterialIndex = static_cast<uint32_t>(surfaceMaterials_.size());
    meshTable_.push_back(meshEntry);
    surfaceMaterials_.push_back(loadFallbackSurfaceMaterial(cached->mesh, cached->fallbackPath));

    cached->meshIndex = meshIndex;
    meshIndexByFallbackPath_[cached->fallbackPath] = meshIndex;
    sharedGeometryDirty_ = true;
    return meshIndex;
  };

  std::vector<GPUSignedDistanceFieldsManager::InstanceGPU> staticInstances;
  std::vector<GPUSignedDistanceFieldsManager::InstanceGPU> skinnedInstances;
  std::vector<math::Mat4f> boneMatrices;
  std::unordered_map<virtualgeometry::InstanceId, PreparedRecord> preparedRecords;

  for (uint32_t denseIndex = 0u; denseIndex < scene.objectInstances.size(); ++denseIndex)
  {
    if (denseIndex >= scene.objectDenseToSparse.size())
    {
      continue;
    }

    const virtualgeometry::InstanceId instanceId = scene.objectDenseToSparse[denseIndex];
    if (instanceId == virtualgeometry::INVALID_INSTANCE_ID)
    {
      continue;
    }

    const VirtualGeometryScene::ObjectInstanceRecord &record = scene.objectInstances[denseIndex];
    if (record.sceneInstanceIds.empty())
    {
      ++diagnostics_.skippedEmptySceneInstanceCount;
      continue;
    }

    auto runtimeDataIt = scene.files.find(record.objectName);
    if (runtimeDataIt == scene.files.end())
    {
      ++diagnostics_.skippedMissingRuntimeDataCount;
      continue;
    }

    VirtualGeometryScene::VirtualGeometryObjectRuntimeData *runtimeData = runtimeDataIt.value();
    const uint32_t meshIndex = getOrCreateMeshIndex(runtimeData->file.getPath());
    if (meshIndex == UINT32_MAX)
    {
      ++diagnostics_.skippedMissingFallbackMeshCount;
      continue;
    }

    CachedFallbackMesh *cachedMesh = loadFallbackMesh(runtimeData->file.getPath(), settings_.fallbackLod);
    if (cachedMesh == nullptr || !cachedMesh->available)
    {
      continue;
    }

    const virtualgeometry::VirtualGeometryInstanceGPUData *sceneInstance = scene.instanceManager.getInstance(record.sceneInstanceIds.front());
    if (sceneInstance == nullptr)
    {
      ++diagnostics_.skippedMissingSceneInstanceCount;
      continue;
    }

    const bool hasSkinning = cachedMesh->mesh.skeleton.getBoneCount() > 0u && !record.currentMeshPartTransforms.empty();
    const AABB currentWorldBounds = hasSkinning ? computeSkinnedWorldBounds(cachedMesh->mesh, sceneInstance->modelMatrix, record.currentMeshPartTransforms)
                                                : transformAabb(makeFallbackAabb(cachedMesh->mesh), sceneInstance->modelMatrix);
    if (!isValidAabb(currentWorldBounds))
    {
      ++diagnostics_.skippedInvalidBoundsCount;
      continue;
    }

    GPUSignedDistanceFieldsManager::InstanceGPU instance{};
    instance.modelMatrix = sceneInstance->modelMatrix;
    instance.previousModelMatrix = sceneInstance->previousModelMatrix;
    instance.aabbMin = math::Vec3f(currentWorldBounds.minPoint[0], currentWorldBounds.minPoint[1], currentWorldBounds.minPoint[2]);
    instance.aabbMax = math::Vec3f(currentWorldBounds.maxPoint[0], currentWorldBounds.maxPoint[1], currentWorldBounds.maxPoint[2]);
    instance.meshIndex = meshIndex;
    instance.flags = hasSkinning ? GPUSignedDistanceFieldsManager::InstanceFlag_Skinned : GPUSignedDistanceFieldsManager::InstanceFlag_None;

    uint32_t pathInstanceIndex = 0u;
    if (hasSkinning)
    {
      instance.boneMatrixOffset = static_cast<uint32_t>(boneMatrices.size());
      boneMatrices.insert(boneMatrices.end(), record.currentMeshPartTransforms.begin(), record.currentMeshPartTransforms.end());
      instance.previousBoneMatrixOffset = static_cast<uint32_t>(boneMatrices.size());
      boneMatrices.insert(boneMatrices.end(), record.previousMeshPartTransforms.begin(), record.previousMeshPartTransforms.end());
      instance.boneCount = static_cast<uint32_t>(record.currentMeshPartTransforms.size());
      pathInstanceIndex = static_cast<uint32_t>(skinnedInstances.size());
      skinnedInstances.push_back(instance);
    }
    else
    {
      pathInstanceIndex = static_cast<uint32_t>(staticInstances.size());
      staticInstances.push_back(instance);
    }

    PreparedRecord prepared{};
    prepared.denseIndex = denseIndex;
    prepared.meshIndex = meshIndex;
    prepared.pathInstanceIndex = pathInstanceIndex;
    prepared.hasSkinning = hasSkinning;
    prepared.currentWorldBounds = currentWorldBounds;
    preparedRecords.emplace(instanceId, prepared);
    ++diagnostics_.preparedInstanceCount;

    RecordBuildState &buildState = recordBuildStates_[instanceId];
    const bool needsInitialBuild = !buildState.hasCompletedInitialBuild;
    const bool needsDynamicUpdate =
        record.gpuSignedDistanceFields.dynamic &&
        record.gpuSignedDistanceFields.contentRevision > buildState.lastCompletedContentRevision;
    const bool needsManualRebuild =
        record.gpuSignedDistanceFields.rebuildRevision > buildState.lastCompletedRebuildRevision;
    if ((needsInitialBuild || needsDynamicUpdate || needsManualRebuild) &&
        !buildState.queued &&
        queuedInstances_.insert(instanceId).second)
    {
      buildState.queued = true;
      pendingUpdateQueue_.push_back(instanceId);
    }
  }

  if (sharedGeometryDirty_)
  {
    manager_.uploadMeshTable(meshTable_);
    manager_.uploadSurfaceMaterials(surfaceMaterials_);
    manager_.uploadVertices(vertices_);
    manager_.uploadIndices(indices_);
    manager_.uploadBoneWeights(boneWeights_);
    os::Logger::logf(
        "[GPUSignedDistanceFields] Uploaded fallback geometry: meshes=%zu surfaceMaterials=%zu vertices=%zu indices=%zu boneWeights=%zu",
        meshTable_.size(),
        surfaceMaterials_.size(),
        vertices_.size(),
        indices_.size(),
        boneWeights_.size());
    sharedGeometryDirty_ = false;
  }

  manager_.uploadBoneMatrices(boneMatrices);
  manager_.uploadInstances(GPUSignedDistanceFieldsManager::Path::Static, staticInstances);
  manager_.uploadInstances(GPUSignedDistanceFieldsManager::Path::Skinned, skinnedInstances);
  diagnostics_.staticInstanceCount = static_cast<uint32_t>(staticInstances.size());
  diagnostics_.skinnedInstanceCount = static_cast<uint32_t>(skinnedInstances.size());
  diagnostics_.cachedFallbackMeshCount = static_cast<uint32_t>(fallbackMeshCache_.size());
  diagnostics_.availableFallbackMeshCount = 0u;
  diagnostics_.missingFallbackMeshCount = 0u;
  diagnostics_.uploadedMeshCount = static_cast<uint32_t>(meshTable_.size());
  diagnostics_.uploadedVertexCount = static_cast<uint32_t>(vertices_.size());
  diagnostics_.uploadedIndexCount = static_cast<uint32_t>(indices_.size());
  diagnostics_.uploadedBoneWeightCount = static_cast<uint32_t>(boneWeights_.size());
  for (const auto &fallbackEntry : fallbackMeshCache_)
  {
    if (fallbackEntry.second.available)
    {
      ++diagnostics_.availableFallbackMeshCount;
    }
    else
    {
      ++diagnostics_.missingFallbackMeshCount;
    }
  }

  std::vector<std::vector<GPUSignedDistanceFieldsManager::JobGPU>> staticJobs(manager_.getCascadeCount());
  std::vector<std::vector<GPUSignedDistanceFieldsManager::JobGPU>> skinnedJobs(manager_.getCascadeCount());
  std::unordered_set<virtualgeometry::InstanceId> updatedThisSync;

  auto cascadeWasScrolled = [](const GPUSignedDistanceFieldsManager::CascadeInfoGPU &cascade) -> bool
  {
    return cascade.clipmapInvalidationOffset[0] != 0 ||
           cascade.clipmapInvalidationOffset[1] != 0 ||
           cascade.clipmapInvalidationOffset[2] != 0;
  };

  auto appendJobsForRecord = [&](
                                 const PreparedRecord &prepared,
                                 const AABB &jobBounds,
                                 bool needsInvalidate,
                                 bool scrolledCascadesOnly)
  {
    for (uint32_t cascadeIndex = 0u; cascadeIndex < manager_.getCascadeCount(); ++cascadeIndex)
    {
      const GPUSignedDistanceFieldsManager::CascadeInfoGPU &cascade = manager_.getCascadeInfo(cascadeIndex);
      if (scrolledCascadesOnly && !cascadeWasScrolled(cascade))
      {
        continue;
      }

      const AABB inflatedJobBounds = expandAabb(jobBounds, cascade.voxelSize);
      if (!intersectsAabb(inflatedJobBounds, cascade.gridMin, cascade.gridMax))
      {
        continue;
      }

      GPUSignedDistanceFieldsManager::JobGPU job{};
      job.instanceIndex = prepared.pathInstanceIndex;
      job.flags = needsInvalidate ? GPUSignedDistanceFieldsManager::JobFlag_Invalidate : GPUSignedDistanceFieldsManager::JobFlag_None;
      job.aabbMin[0] = clampVoxelMin(inflatedJobBounds.minPoint[0], cascade.gridMin[0], cascade.iVoxelSize, GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION);
      job.aabbMin[1] = clampVoxelMin(inflatedJobBounds.minPoint[1], cascade.gridMin[1], cascade.iVoxelSize, GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION);
      job.aabbMin[2] = clampVoxelMin(inflatedJobBounds.minPoint[2], cascade.gridMin[2], cascade.iVoxelSize, GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION);
      job.aabbMax[0] = clampVoxelMax(inflatedJobBounds.maxPoint[0], cascade.gridMin[0], cascade.iVoxelSize, GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION);
      job.aabbMax[1] = clampVoxelMax(inflatedJobBounds.maxPoint[1], cascade.gridMin[1], cascade.iVoxelSize, GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION);
      job.aabbMax[2] = clampVoxelMax(inflatedJobBounds.maxPoint[2], cascade.gridMin[2], cascade.iVoxelSize, GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION);

      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        if (job.aabbMax[axis] <= job.aabbMin[axis])
        {
          job.aabbMax[axis] = std::min<uint32_t>(GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION, job.aabbMin[axis] + 1u);
        }
      }

      if (prepared.hasSkinning)
        skinnedJobs[cascadeIndex].push_back(job);
      else
        staticJobs[cascadeIndex].push_back(job);
    }
  };

  const uint32_t updateBudget = settings_.maxMeshUpdatesPerFrame;
  while (lastFrameUpdatedCount_ < updateBudget && !pendingUpdateQueue_.empty())
  {
    const virtualgeometry::InstanceId instanceId = pendingUpdateQueue_.front();
    pendingUpdateQueue_.pop_front();
    queuedInstances_.erase(instanceId);

    auto stateIt = recordBuildStates_.find(instanceId);
    if (stateIt == recordBuildStates_.end())
    {
      continue;
    }
    stateIt->second.queued = false;

    if (instanceId >= scene.objectSparseToDense.size())
    {
      recordBuildStates_.erase(stateIt);
      continue;
    }

    const uint32_t denseIndex = scene.objectSparseToDense[instanceId];
    if (denseIndex == virtualgeometry::INVALID_INSTANCE_ID || denseIndex >= scene.objectInstances.size())
    {
      recordBuildStates_.erase(stateIt);
      continue;
    }

    auto preparedIt = preparedRecords.find(instanceId);
    if (preparedIt == preparedRecords.end())
    {
      continue;
    }

    const VirtualGeometryScene::ObjectInstanceRecord &record = scene.objectInstances[denseIndex];
    RecordBuildState &buildState = stateIt->second;
    const bool hasPendingContentUpdate =
        record.gpuSignedDistanceFields.contentRevision > buildState.lastCompletedContentRevision;
    const bool hasPendingManualRebuild =
        record.gpuSignedDistanceFields.rebuildRevision > buildState.lastCompletedRebuildRevision;
    const bool needsInvalidate =
        buildState.hasCompletedInitialBuild &&
        buildState.hasLastBuiltBounds &&
        isValidAabb(buildState.lastBuiltBounds) &&
        (hasPendingContentUpdate || hasPendingManualRebuild);

    const PreparedRecord &prepared = preparedIt->second;
    size_t jobCountBefore = 0u;
    for (uint32_t c = 0u; c < manager_.getCascadeCount(); ++c)
      jobCountBefore += staticJobs[c].size() + skinnedJobs[c].size();

    if (needsInvalidate)
    {
      appendJobsForRecord(prepared, AABB::Union(buildState.lastBuiltBounds, prepared.currentWorldBounds), true, false);
      appendJobsForRecord(prepared, prepared.currentWorldBounds, false, false);
    }
    else
    {
      appendJobsForRecord(prepared, prepared.currentWorldBounds, false, false);
    }

    size_t jobCountAfter = 0u;
    for (uint32_t c = 0u; c < manager_.getCascadeCount(); ++c)
      jobCountAfter += staticJobs[c].size() + skinnedJobs[c].size();

    if (jobCountAfter > jobCountBefore)
      buildState.hasCompletedInitialBuild = true;
    buildState.lastCompletedContentRevision = record.gpuSignedDistanceFields.contentRevision;
    buildState.lastCompletedRebuildRevision = record.gpuSignedDistanceFields.rebuildRevision;
    buildState.lastBuiltBounds = prepared.currentWorldBounds;
    buildState.hasLastBuiltBounds = true;
    updatedThisSync.insert(instanceId);
    ++lastFrameUpdatedCount_;
  }

  bool anyCascadeScrolled = false;
  for (uint32_t cascadeIndex = 0u; cascadeIndex < manager_.getCascadeCount(); ++cascadeIndex)
  {
    if (cascadeWasScrolled(manager_.getCascadeInfo(cascadeIndex)))
    {
      anyCascadeScrolled = true;
      break;
    }
  }

  if (anyCascadeScrolled)
  {
    for (const auto &preparedEntry : preparedRecords)
    {
      const virtualgeometry::InstanceId instanceId = preparedEntry.first;
      if (updatedThisSync.find(instanceId) != updatedThisSync.end())
      {
        continue;
      }

      const auto stateIt = recordBuildStates_.find(instanceId);
      if (stateIt == recordBuildStates_.end() || !stateIt->second.hasCompletedInitialBuild)
      {
        continue;
      }

      appendJobsForRecord(preparedEntry.second, preparedEntry.second.currentWorldBounds, false, true);
    }
  }

  bool anyJobsUploaded = false;
  for (uint32_t cascadeIndex = 0u; cascadeIndex < manager_.getCascadeCount(); ++cascadeIndex)
  {
    manager_.uploadJobs(GPUSignedDistanceFieldsManager::Path::Static, cascadeIndex, staticJobs[cascadeIndex]);
    manager_.uploadJobs(GPUSignedDistanceFieldsManager::Path::Skinned, cascadeIndex, skinnedJobs[cascadeIndex]);
    const auto &staticCascade = manager_.getPathResources(GPUSignedDistanceFieldsManager::Path::Static).cascades[cascadeIndex];
    const auto &skinnedCascade = manager_.getPathResources(GPUSignedDistanceFieldsManager::Path::Skinned).cascades[cascadeIndex];
    diagnostics_.staticJobCounts[cascadeIndex] = staticCascade.jobCount;
    diagnostics_.skinnedJobCounts[cascadeIndex] = skinnedCascade.jobCount;
    diagnostics_.staticJobVoxelCounts[cascadeIndex] = staticCascade.jobVoxelCount;
    diagnostics_.skinnedJobVoxelCounts[cascadeIndex] = skinnedCascade.jobVoxelCount;
    if (staticCascade.jobCount > 0u || skinnedCascade.jobCount > 0u)
      anyJobsUploaded = true;
  }

  const auto sumU32 = [](const std::vector<uint32_t> &values) -> uint32_t
  {
    uint32_t sum = 0u;
    for (uint32_t value : values)
    {
      sum += value;
    }
    return sum;
  };

  if (diagnostics_.availableFallbackMeshCount != lastLoggedAvailableFallbackMeshCount_ ||
      diagnostics_.missingFallbackMeshCount != lastLoggedMissingFallbackMeshCount_ ||
      diagnostics_.preparedInstanceCount != lastLoggedPreparedInstanceCount_ ||
      diagnostics_.staticInstanceCount != lastLoggedStaticInstanceCount_ ||
      diagnostics_.skinnedInstanceCount != lastLoggedSkinnedInstanceCount_ ||
      diagnostics_.uploadedMeshCount != lastLoggedUploadedMeshCount_ ||
      diagnostics_.uploadedVertexCount != lastLoggedUploadedVertexCount_ ||
      diagnostics_.uploadedIndexCount != lastLoggedUploadedIndexCount_ ||
      diagnostics_.uploadedBoneWeightCount != lastLoggedUploadedBoneWeightCount_ ||
      diagnostics_.skippedEmptySceneInstanceCount != lastLoggedSkippedEmptySceneInstanceCount_ ||
      diagnostics_.skippedMissingRuntimeDataCount != lastLoggedSkippedMissingRuntimeDataCount_ ||
      diagnostics_.skippedMissingFallbackMeshCount != lastLoggedSkippedMissingFallbackMeshCount_ ||
      diagnostics_.skippedMissingSceneInstanceCount != lastLoggedSkippedMissingSceneInstanceCount_ ||
      diagnostics_.skippedInvalidBoundsCount != lastLoggedSkippedInvalidBoundsCount_)
  {
    os::Logger::logf(
        "[GPUSignedDistanceFields] Sync summary: fallbackMeshes loaded=%u missing=%u prepared=%u staticInstances=%u skinnedInstances=%u jobs(static=%u/%u voxels skinned=%u/%u voxels) uploadedMeshes=%u uploadedVertices=%u uploadedIndices=%u uploadedBoneWeights=%u pendingUpdates=%u updatedThisFrame=%u skipped(emptyIds=%u runtime=%u fallback=%u scene=%u bounds=%u)",
        diagnostics_.availableFallbackMeshCount,
        diagnostics_.missingFallbackMeshCount,
        diagnostics_.preparedInstanceCount,
        diagnostics_.staticInstanceCount,
        diagnostics_.skinnedInstanceCount,
        sumU32(diagnostics_.staticJobCounts),
        sumU32(diagnostics_.staticJobVoxelCounts),
        sumU32(diagnostics_.skinnedJobCounts),
        sumU32(diagnostics_.skinnedJobVoxelCounts),
        diagnostics_.uploadedMeshCount,
        diagnostics_.uploadedVertexCount,
        diagnostics_.uploadedIndexCount,
        diagnostics_.uploadedBoneWeightCount,
        getPendingUpdateCount(),
        lastFrameUpdatedCount_,
        diagnostics_.skippedEmptySceneInstanceCount,
        diagnostics_.skippedMissingRuntimeDataCount,
        diagnostics_.skippedMissingFallbackMeshCount,
        diagnostics_.skippedMissingSceneInstanceCount,
        diagnostics_.skippedInvalidBoundsCount);
    if (diagnostics_.availableFallbackMeshCount > 0u &&
        diagnostics_.staticInstanceCount == 0u &&
        diagnostics_.skinnedInstanceCount == 0u)
    {
      os::Logger::warningf(
          "[GPUSignedDistanceFields] Fallback meshes are loaded but no instances were uploaded. Last loaded fallback: %s",
          diagnostics_.lastLoadedFallbackPath.empty() ? "<none>" : diagnostics_.lastLoadedFallbackPath.c_str());
    }
    if ((diagnostics_.skippedEmptySceneInstanceCount > 0u ||
         diagnostics_.skippedMissingRuntimeDataCount > 0u ||
         diagnostics_.skippedMissingFallbackMeshCount > 0u ||
         diagnostics_.skippedMissingSceneInstanceCount > 0u ||
         diagnostics_.skippedInvalidBoundsCount > 0u) &&
        diagnostics_.staticInstanceCount == 0u &&
        diagnostics_.skinnedInstanceCount == 0u)
    {
      os::Logger::warningf(
          "[GPUSignedDistanceFields] All fallback scene candidates were skipped: emptySceneIds=%u missingRuntime=%u missingFallback=%u missingSceneInstance=%u invalidBounds=%u",
          diagnostics_.skippedEmptySceneInstanceCount,
          diagnostics_.skippedMissingRuntimeDataCount,
          diagnostics_.skippedMissingFallbackMeshCount,
          diagnostics_.skippedMissingSceneInstanceCount,
          diagnostics_.skippedInvalidBoundsCount);
    }
    lastLoggedAvailableFallbackMeshCount_ = diagnostics_.availableFallbackMeshCount;
    lastLoggedMissingFallbackMeshCount_ = diagnostics_.missingFallbackMeshCount;
    lastLoggedPreparedInstanceCount_ = diagnostics_.preparedInstanceCount;
    lastLoggedStaticInstanceCount_ = diagnostics_.staticInstanceCount;
    lastLoggedSkinnedInstanceCount_ = diagnostics_.skinnedInstanceCount;
    lastLoggedUploadedMeshCount_ = diagnostics_.uploadedMeshCount;
    lastLoggedUploadedVertexCount_ = diagnostics_.uploadedVertexCount;
    lastLoggedUploadedIndexCount_ = diagnostics_.uploadedIndexCount;
    lastLoggedUploadedBoneWeightCount_ = diagnostics_.uploadedBoneWeightCount;
    lastLoggedSkippedEmptySceneInstanceCount_ = diagnostics_.skippedEmptySceneInstanceCount;
    lastLoggedSkippedMissingRuntimeDataCount_ = diagnostics_.skippedMissingRuntimeDataCount;
    lastLoggedSkippedMissingFallbackMeshCount_ = diagnostics_.skippedMissingFallbackMeshCount;
    lastLoggedSkippedMissingSceneInstanceCount_ = diagnostics_.skippedMissingSceneInstanceCount;
    lastLoggedSkippedInvalidBoundsCount_ = diagnostics_.skippedInvalidBoundsCount;
  }
}

GPUSignedDistanceFieldsSceneBridge::CachedFallbackMesh *GPUSignedDistanceFieldsSceneBridge::loadFallbackMesh(const std::string &virtualGeometryPath, uint32_t fallbackLod)
{
  const std::vector<std::filesystem::path> candidates = buildFallbackMeshCandidates(virtualGeometryPath, fallbackLod);
  for (const std::filesystem::path &candidate : candidates)
  {
    auto existing = fallbackMeshCache_.find(candidate.string());
    if (existing != fallbackMeshCache_.end() && existing->second.available)
    {
      return &existing->second;
    }
  }

  std::filesystem::path selectedPath;
  for (const std::filesystem::path &candidate : candidates)
  {
    if (std::filesystem::exists(candidate))
    {
      selectedPath = candidate;
      break;
    }
  }

  if (selectedPath.empty())
  {
    selectedPath = candidates.front();
  }

  const std::string cacheKey = selectedPath.string();
  auto existing = fallbackMeshCache_.find(cacheKey);
  if (existing != fallbackMeshCache_.end())
  {
    return &existing->second;
  }

  CachedFallbackMesh cached{};
  cached.available = false;
  cached.fallbackPath = cacheKey;
  if (!virtualgeometry::FallbackMeshFile::load(cacheKey, cached.mesh))
  {
    diagnostics_.lastMissingFallbackPath = cacheKey;
    os::Logger::warningf(
        "[GPUSignedDistanceFields] Missing fallback mesh for '%s' near requested lod=%u; searched first path '%s'",
        virtualGeometryPath.c_str(),
        fallbackLod,
        candidates.front().string().c_str());
  }
  else
  {
    cached.available = true;
    diagnostics_.lastLoadedFallbackPath = cacheKey;
    if (selectedPath != candidates.front())
    {
      os::Logger::logf(
          "[GPUSignedDistanceFields] Requested fallback lod=%u for '%s' but using '%s'",
          fallbackLod,
          virtualGeometryPath.c_str(),
          cacheKey.c_str());
    }
    os::Logger::logf(
        "[GPUSignedDistanceFields] Loaded fallback mesh '%s' from '%s' (lod=%u vertices=%zu triangles=%zu bones=%u)",
        virtualGeometryPath.c_str(),
        cacheKey.c_str(),
        cached.mesh.lodLevel,
        cached.mesh.vertices.size(),
        cached.mesh.indices.size() / 3u,
        cached.mesh.skeleton.getBoneCount());
  }

  auto inserted = fallbackMeshCache_.emplace(cacheKey, std::move(cached));
  return &inserted.first->second;
}

} // namespace gpusigneddistancefields
