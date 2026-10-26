#include "softwarebvh/SoftwareBLASSceneBridge.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <unordered_set>

#include "os/Logger.hpp"
#include "softwarebvh/RBVHBuilder.hpp"

namespace softwarebvh
{

namespace
{

struct FallbackCandidate
{
  std::filesystem::path path;
  uint32_t lodLevel = 0u;
};

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

static std::string buildPrimitiveSourceKey(
    const std::string &shapeSourceKey,
    const std::optional<virtualgeometry::SceneInstanceId> sceneInstanceId,
    const SoftwareBLASSceneBridge::PrimitiveMode primitiveMode)
{
  if (primitiveMode == SoftwareBLASSceneBridge::PrimitiveMode::RBVHCard && sceneInstanceId.has_value())
  {
    return buildShapeInstanceSourceKey(shapeSourceKey, *sceneInstanceId);
  }
  return shapeSourceKey;
}

static std::filesystem::path buildFallbackMeshPath(const std::string &virtualGeometryPath, const uint32_t lodLevel, const std::optional<uint32_t> shapeIndex = std::nullopt)
{
  const std::filesystem::path path(virtualGeometryPath);
  const std::string shapeSuffix = shapeIndex.has_value() ? ".shape" + std::to_string(*shapeIndex) : "";
  return path.parent_path() / (path.stem().string() + shapeSuffix + ".lod" + std::to_string(lodLevel) + ".fallback.mesh");
}

static std::filesystem::path buildSWRTFallbackMeshPath(const std::string &virtualGeometryPath, const uint32_t lodLevel, const std::optional<uint32_t> shapeIndex = std::nullopt)
{
  const std::filesystem::path path(virtualGeometryPath);
  const std::string shapeSuffix = shapeIndex.has_value() ? ".shape" + std::to_string(*shapeIndex) : "";
  return path.parent_path() / (path.stem().string() + shapeSuffix + ".lod" + std::to_string(lodLevel) + ".SWRTFallback.mesh");
}

static std::filesystem::path buildSoftwareRaytraceScenePath(const std::string &virtualGeometryPath)
{
  const std::filesystem::path path(virtualGeometryPath);
  return path.parent_path() / (path.stem().string() + ".softwareraytrace");
}

static std::filesystem::path buildSWRBVHFallbackMeshPath(const std::string &virtualGeometryPath, const uint32_t lodLevel, const std::optional<uint32_t> shapeIndex = std::nullopt)
{
  const std::filesystem::path path(virtualGeometryPath);
  const std::string shapeSuffix = shapeIndex.has_value() ? ".shape" + std::to_string(*shapeIndex) : "";
  return path.parent_path() / (path.stem().string() + shapeSuffix + ".lod" + std::to_string(lodLevel) + ".SWRBVH.mesh");
}

static bool isShapeSpecificFallbackPath(const std::filesystem::path &path, const std::optional<uint32_t> shapeIndex)
{
  if (!shapeIndex.has_value())
  {
    return false;
  }

  return path.filename().string().find(".shape" + std::to_string(*shapeIndex) + ".lod") != std::string::npos;
}

static bool isPrebuiltSoftwareFallbackPath(const std::filesystem::path &path)
{
  const std::string pathString = path.string();
  return (path.extension() == ".mesh" &&
          (pathString.find(".SWRTFallback.mesh") != std::string::npos || pathString.find(".SWRBVH.mesh") != std::string::npos)) ||
         path.extension() == ".softwareraytrace";
}

static bool isSWRBVHFallbackPath(const std::filesystem::path &path)
{
  return path.extension() == ".mesh" && path.string().find(".SWRBVH.mesh") != std::string::npos;
}

static bool isUnifiedSoftwareRaytraceScenePath(const std::filesystem::path &path)
{
  return path.extension() == ".softwareraytrace";
}

static void appendPrimitiveFallbackCandidates(
    std::vector<FallbackCandidate> &candidates,
    const std::string &virtualGeometryPath,
    const uint32_t lodLevel,
    const std::optional<uint32_t> shapeIndex,
    const SoftwareBLASSceneBridge::PrimitiveMode primitiveMode,
    const bool preferRuntimeRbvhBuilds)
{
  if (primitiveMode == SoftwareBLASSceneBridge::PrimitiveMode::RBVHCard)
  {
    if (!preferRuntimeRbvhBuilds)
    {
      candidates.push_back(FallbackCandidate{.path = buildSWRBVHFallbackMeshPath(virtualGeometryPath, lodLevel, shapeIndex), .lodLevel = lodLevel});
    }
    candidates.push_back(FallbackCandidate{.path = buildSoftwareRaytraceScenePath(virtualGeometryPath), .lodLevel = lodLevel});
    candidates.push_back(FallbackCandidate{.path = buildSWRTFallbackMeshPath(virtualGeometryPath, lodLevel, shapeIndex), .lodLevel = lodLevel});
    candidates.push_back(FallbackCandidate{.path = buildFallbackMeshPath(virtualGeometryPath, lodLevel, shapeIndex), .lodLevel = lodLevel});
  }
  else
  {
    candidates.push_back(FallbackCandidate{.path = buildSoftwareRaytraceScenePath(virtualGeometryPath), .lodLevel = lodLevel});
    candidates.push_back(FallbackCandidate{.path = buildSWRTFallbackMeshPath(virtualGeometryPath, lodLevel, shapeIndex), .lodLevel = lodLevel});
    candidates.push_back(FallbackCandidate{.path = buildFallbackMeshPath(virtualGeometryPath, lodLevel, shapeIndex), .lodLevel = lodLevel});
  }
}

static RBVHBuildOptions makeRuntimeRBVHBuildOptions(const uint32_t rbvhLod)
{
  const uint32_t clampedLod = std::min(rbvhLod, SWRBVH_FALLBACK_MESH_LOD_COUNT - 1u);
  const uint32_t resolutionShift = std::min(clampedLod, 3u);
  const float lodT = static_cast<float>(clampedLod);

  RBVHBuildOptions options{};
  options.minCardResolution = 4u;
  options.maxCardResolution = std::max(64u >> resolutionShift, options.minCardResolution);
  options.maxAtlasSize = std::max(2048u >> resolutionShift, options.maxCardResolution);
  options.maxCards = std::max(4096u >> resolutionShift, 512u);
  options.projectedAreaThreshold = std::clamp(0.55f - 0.05f * lodT, 0.45f, 1.0f);
  options.minNormalDot = std::clamp(0.5f - 0.05f * lodT, 0.25f, 1.0f);
  options.splitOverlapFraction = 0.0f;
  options.cardBoundsPadding = 5.0e-4f;
  return options;
}

static std::vector<FallbackCandidate> buildFallbackMeshCandidates(
    const std::string &virtualGeometryPath,
    const uint32_t requestedLod,
    const std::optional<uint32_t> shapeIndex,
    const SoftwareBLASSceneBridge::PrimitiveMode primitiveMode,
    const bool preferRuntimeRbvhBuilds)
{
  constexpr uint32_t kMaxLod = 3u;

  const uint32_t clampedRequestedLod = std::min(requestedLod, kMaxLod);
  std::vector<FallbackCandidate> candidates;
  candidates.reserve((kMaxLod + 1u) * 6u + 3u);
  if (shapeIndex.has_value())
  {
    appendPrimitiveFallbackCandidates(candidates, virtualGeometryPath, clampedRequestedLod, shapeIndex, primitiveMode, preferRuntimeRbvhBuilds);
  }
  appendPrimitiveFallbackCandidates(candidates, virtualGeometryPath, clampedRequestedLod, std::nullopt, primitiveMode, preferRuntimeRbvhBuilds);
  for (uint32_t lodLevel = clampedRequestedLod + 1u; lodLevel <= kMaxLod; ++lodLevel)
  {
    if (shapeIndex.has_value())
    {
      appendPrimitiveFallbackCandidates(candidates, virtualGeometryPath, lodLevel, shapeIndex, primitiveMode, preferRuntimeRbvhBuilds);
    }
    appendPrimitiveFallbackCandidates(candidates, virtualGeometryPath, lodLevel, std::nullopt, primitiveMode, preferRuntimeRbvhBuilds);
  }
  for (uint32_t lodLevel = clampedRequestedLod; lodLevel > 0u; --lodLevel)
  {
    if (shapeIndex.has_value())
    {
      appendPrimitiveFallbackCandidates(candidates, virtualGeometryPath, lodLevel - 1u, shapeIndex, primitiveMode, preferRuntimeRbvhBuilds);
    }
    appendPrimitiveFallbackCandidates(candidates, virtualGeometryPath, lodLevel - 1u, std::nullopt, primitiveMode, preferRuntimeRbvhBuilds);
  }
  return candidates;
}

} // namespace

SoftwareBLASSceneBridge::SoftwareBLASSceneBridge(SoftwareBLASManager &manager) : SoftwareBLASSceneBridge(manager, Settings{})
{
}

SoftwareBLASSceneBridge::SoftwareBLASSceneBridge(SoftwareBLASManager &manager, Settings settings) : manager_(manager), settings_(settings)
{
}

void SoftwareBLASSceneBridge::setRuntimeRBVHSourceMeshes(const std::string &virtualGeometryPath, std::vector<virtualgeometry::FallbackMeshData> meshes)
{
  runtimeRbvhSourceMeshes_[virtualGeometryPath] = std::move(meshes);
  sourceStates_.clear();
  sourcesDirty_ = true;
  hasAppliedSettings_ = false;
}

bool SoftwareBLASSceneBridge::settingsMatchApplied() const
{
  return hasAppliedSettings_ &&
         appliedSettings_.fallbackLod == settings_.fallbackLod &&
         appliedSettings_.rbvhLod == settings_.rbvhLod &&
         appliedSettings_.primitiveMode == settings_.primitiveMode &&
         appliedSettings_.preferRuntimeRbvhBuilds == settings_.preferRuntimeRbvhBuilds;
}

bool SoftwareBLASSceneBridge::addObjectInstanceSources(virtualgeometry::VirtualGeometryScene &scene, virtualgeometry::InstanceId objectInstanceId)
{
  if (objectInstanceId == virtualgeometry::INVALID_INSTANCE_ID ||
      objectInstanceId >= scene.objectSparseToDense.size())
  {
    return false;
  }

  const uint32_t denseIndex = scene.objectSparseToDense[objectInstanceId];
  if (denseIndex >= scene.objectInstances.size() ||
      denseIndex >= scene.objectDenseToSparse.size() ||
      scene.objectDenseToSparse[denseIndex] != objectInstanceId)
  {
    return false;
  }

  const virtualgeometry::VirtualGeometryScene::ObjectInstanceRecord &record = scene.objectInstances[denseIndex];
  const auto runtimeData = scene.files.find(record.objectName);
  if (runtimeData == scene.files.end())
  {
    ++diagnostics_.skippedMissingRuntimeDataCount;
    return false;
  }

  const virtualgeometry::VirtualGeometryScene::VirtualGeometryObjectRuntimeData *objectData = runtimeData.value();
  if (objectData == nullptr)
  {
    ++diagnostics_.skippedMissingRuntimeDataCount;
    return false;
  }

  bool changed = false;
  const std::string &virtualGeometryPath = objectData->file.getPath();
  const uint32_t shapeCount = std::max<uint32_t>(1u, static_cast<uint32_t>(objectData->shapes.size()));
  for (uint32_t shapeIndex = 0u; shapeIndex < shapeCount; ++shapeIndex)
  {
    const std::optional<uint32_t> optionalShapeIndex = objectData->shapes.empty() ? std::nullopt : std::optional<uint32_t>(shapeIndex);
    const std::optional<virtualgeometry::SceneInstanceId> optionalSceneInstanceId =
        shapeIndex < record.sceneInstanceIds.size()
            ? std::optional<virtualgeometry::SceneInstanceId>(record.sceneInstanceIds[shapeIndex])
            : std::nullopt;
    const std::string shapeSourceKey = buildShapeSourceKey(virtualGeometryPath, optionalShapeIndex);
    const std::string sourceKey = buildPrimitiveSourceKey(shapeSourceKey, optionalSceneInstanceId, settings_.primitiveMode);
    const uint32_t resolvedShapeIndex = optionalShapeIndex.value_or(0u);
    ++diagnostics_.preparedMeshCount;

    auto existingState = sourceStates_.find(sourceKey);
    if (existingState != sourceStates_.end() &&
        existingState->second.primitiveMode == settings_.primitiveMode &&
        existingState->second.fallbackLod == settings_.fallbackLod &&
        existingState->second.rbvhLod == settings_.rbvhLod &&
        existingState->second.preferRuntimeRbvhBuilds == settings_.preferRuntimeRbvhBuilds &&
        !existingState->second.cacheKey.empty())
    {
      ++diagnostics_.uniqueMeshCount;
      continue;
    }

    CachedFallbackMesh *cachedMesh = loadFallbackMesh(virtualGeometryPath, settings_.fallbackLod, optionalShapeIndex);
    if (cachedMesh == nullptr || !cachedMesh->available)
    {
      ++diagnostics_.skippedMissingFallbackMeshCount;
      continue;
    }

    ++diagnostics_.uniqueMeshCount;

    SourceState &state = sourceStates_[sourceKey];
    if (state.cacheKey != cachedMesh->cacheKey ||
        state.fallbackPath != cachedMesh->fallbackPath ||
        state.isSWRT != cachedMesh->isSWRT ||
        state.primitiveMode != settings_.primitiveMode ||
        state.fallbackLod != settings_.fallbackLod ||
        state.rbvhLod != settings_.rbvhLod ||
        state.preferRuntimeRbvhBuilds != settings_.preferRuntimeRbvhBuilds ||
        state.lastWriteTime != cachedMesh->lastWriteTime ||
        state.vertexCount != cachedMesh->mesh.vertices.size() ||
        state.indexCount != cachedMesh->mesh.indices.size() ||
        state.lodLevel != cachedMesh->mesh.lodLevel)
    {
      changed = true;
      state.cacheKey = cachedMesh->cacheKey;
      state.fallbackPath = cachedMesh->fallbackPath;
      state.virtualGeometryPath = virtualGeometryPath;
      state.isSWRT = cachedMesh->isSWRT;
      state.primitiveMode = settings_.primitiveMode;
      state.fallbackLod = settings_.fallbackLod;
      state.rbvhLod = settings_.rbvhLod;
      state.preferRuntimeRbvhBuilds = settings_.preferRuntimeRbvhBuilds;
      state.shapeIndex = resolvedShapeIndex;
      state.lastWriteTime = cachedMesh->lastWriteTime;
      state.vertexCount = static_cast<uint32_t>(cachedMesh->mesh.vertices.size());
      state.indexCount = static_cast<uint32_t>(cachedMesh->mesh.indices.size());
      state.lodLevel = cachedMesh->mesh.lodLevel;
    }
  }

  if (changed)
  {
    sourcesDirty_ = true;
  }
  return changed;
}

void SoftwareBLASSceneBridge::registerObjectInstance(virtualgeometry::VirtualGeometryScene &scene, virtualgeometry::InstanceId objectInstanceId)
{
  diagnostics_ = Diagnostics{};
  diagnostics_.primitiveMode = static_cast<uint32_t>(settings_.primitiveMode);
  diagnostics_.rbvhLod = settings_.rbvhLod;
  diagnostics_.preferRuntimeRbvhBuilds = settings_.preferRuntimeRbvhBuilds;
  addObjectInstanceSources(scene, objectInstanceId);
  updateCacheDiagnostics();
}

void SoftwareBLASSceneBridge::rebuildFromScene(virtualgeometry::VirtualGeometryScene &scene)
{
  sourceStates_.clear();
  for (uint32_t denseIndex = 0u; denseIndex < static_cast<uint32_t>(scene.objectInstances.size()); ++denseIndex)
  {
    if (denseIndex >= scene.objectDenseToSparse.size())
      continue;

    addObjectInstanceSources(scene, scene.objectDenseToSparse[denseIndex]);
  }
}

void SoftwareBLASSceneBridge::sync(virtualgeometry::VirtualGeometryScene &scene)
{
  diagnostics_ = Diagnostics{};
  diagnostics_.primitiveMode = static_cast<uint32_t>(settings_.primitiveMode);
  diagnostics_.rbvhLod = settings_.rbvhLod;
  diagnostics_.preferRuntimeRbvhBuilds = settings_.preferRuntimeRbvhBuilds;

  if (!settingsMatchApplied())
  {
    rebuildFromScene(scene);
    appliedSettings_ = settings_;
    hasAppliedSettings_ = true;
    sourcesDirty_ = true;
  }

  diagnostics_.preparedMeshCount = static_cast<uint32_t>(sourceStates_.size());
  diagnostics_.uniqueMeshCount = static_cast<uint32_t>(sourceStates_.size());

  diagnostics_.requestedRebuild =
      sourcesDirty_ ||
      manager_.getLastBuildSummary().blasCount != sourceStates_.size();

  if (sourceStates_.empty())
  {
    if (manager_.getLastBuildSummary().blasCount > 0u)
    {
      manager_.clear();
    }
    else
    {
      manager_.markClean();
    }
    sourcesDirty_ = false;
  }
  else if (diagnostics_.requestedRebuild)
  {
    struct RuntimeRBVHBuildTask
    {
      size_t inputIndex = 0u;
      std::string sourcePath;
      uint32_t triangleCount = 0u;
      bool usesOriginalSource = false;
      struct Output
      {
        RBVHBuildResult build;
        double milliseconds = 0.0;
      };
      std::future<Output> future;
    };

    std::vector<SoftwareBLASManager::BuildInput> residentBuildInputs;
    residentBuildInputs.reserve(sourceStates_.size());
    std::vector<RuntimeRBVHBuildTask> runtimeRbvhBuildTasks;
    for (const auto &[sourceKey, state] : sourceStates_)
    {
      const auto cachedMeshIt = fallbackMeshCache_.find(state.cacheKey);
      if (cachedMeshIt == fallbackMeshCache_.end() || !cachedMeshIt->second.available)
      {
        continue;
      }

      const CachedFallbackMesh &cachedMesh = cachedMeshIt->second;
      SoftwareBLASManager::BuildInput input{};
      input.sourcePath = sourceKey;
      input.fallbackPath = cachedMesh.fallbackPath;
      input.meshRef = &cachedMesh.mesh;
      input.prebuiltRef = cachedMesh.prebuilt.has_value() ? &*cachedMesh.prebuilt : nullptr;
      input.rbvhBuildRef = cachedMesh.rbvhBuild.has_value() ? &*cachedMesh.rbvhBuild : nullptr;
      if (settings_.primitiveMode == PrimitiveMode::RBVHCard && input.rbvhBuildRef == nullptr)
      {
        input.prebuiltRef = nullptr;
        const virtualgeometry::FallbackMeshData *rbvhSourceMesh = &cachedMesh.mesh;
        bool usesOriginalSource = false;
        auto sourceMeshesIt = runtimeRbvhSourceMeshes_.find(state.virtualGeometryPath);
        if (sourceMeshesIt != runtimeRbvhSourceMeshes_.end() && state.shapeIndex < sourceMeshesIt->second.size())
        {
          rbvhSourceMesh = &sourceMeshesIt->second[state.shapeIndex];
          usesOriginalSource = true;
          ++diagnostics_.runtimeRbvhOriginalSourceCount;
        }
        else
        {
          ++diagnostics_.runtimeRbvhFallbackSourceCount;
          os::Logger::warningf(
              "[SWRT][RBVH] missing original runtime RBVH source for %s shape=%u; building cards from fallback proxy",
              state.virtualGeometryPath.c_str(),
              state.shapeIndex);
        }

        const uint32_t sourceTriangleCount = static_cast<uint32_t>(rbvhSourceMesh->indices.size() / 3u);
        diagnostics_.runtimeRbvhSourceTriangleCount += sourceTriangleCount;
        diagnostics_.lastRuntimeRbvhBuildSourcePath = sourceKey;
        const RBVHBuildOptions options = makeRuntimeRBVHBuildOptions(settings_.rbvhLod);
        const size_t inputIndex = residentBuildInputs.size();
        runtimeRbvhBuildTasks.push_back(RuntimeRBVHBuildTask{
            .inputIndex = inputIndex,
            .sourcePath = sourceKey,
            .triangleCount = sourceTriangleCount,
            .usesOriginalSource = usesOriginalSource,
            .future = std::async(
                std::launch::async,
                [rbvhSourceMesh, options]()
                {
                  const auto startTime = std::chrono::steady_clock::now();
                  RBVHBuildResult build = RBVHBuilder::build(*rbvhSourceMesh, options);
                  const auto endTime = std::chrono::steady_clock::now();
                  return RuntimeRBVHBuildTask::Output{
                      .build = std::move(build),
                      .milliseconds = std::chrono::duration<double, std::milli>(endTime - startTime).count(),
                  };
                }),
        });
      }
      residentBuildInputs.push_back(std::move(input));
    }

    for (RuntimeRBVHBuildTask &task : runtimeRbvhBuildTasks)
    {
      os::Logger::logf(
          "[SWRT][RBVH] runtime build start source=%s triangles=%u sourceMesh=%s",
          task.sourcePath.c_str(),
          task.triangleCount,
          task.usesOriginalSource ? "original" : "fallback-proxy");
    }
    for (RuntimeRBVHBuildTask &task : runtimeRbvhBuildTasks)
    {
      RuntimeRBVHBuildTask::Output output = task.future.get();
      diagnostics_.runtimeRbvhBuildMilliseconds += output.milliseconds;
      ++diagnostics_.runtimeRbvhBuildCount;
      os::Logger::logf(
          "[SWRT][RBVH] runtime build done source=%s triangles=%u cards=%zu atlas=%u elapsed=%.2fms",
          task.sourcePath.c_str(),
          task.triangleCount,
          output.build.cards.size(),
          output.build.atlasSize,
          output.milliseconds);
      if (task.inputIndex < residentBuildInputs.size())
      {
        residentBuildInputs[task.inputIndex].rbvhBuild = std::move(output.build);
      }
    }

    if (residentBuildInputs.empty())
    {
      manager_.clear();
    }
    else
    {
      manager_.syncFromCPU(residentBuildInputs);
    }
    sourcesDirty_ = false;
  }
  else
  {
    manager_.markClean();
  }

  updateCacheDiagnostics();
}

void SoftwareBLASSceneBridge::updateCacheDiagnostics()
{
  diagnostics_.cachedFallbackMeshCount = static_cast<uint32_t>(fallbackMeshCache_.size());
  diagnostics_.availableFallbackMeshCount = 0u;
  diagnostics_.missingFallbackMeshCount = 0u;
  for (const auto &[_, cachedMesh] : fallbackMeshCache_)
  {
    if (cachedMesh.available)
    {
      ++diagnostics_.availableFallbackMeshCount;
    }
    else
    {
      ++diagnostics_.missingFallbackMeshCount;
    }
  }
}

SoftwareBLASSceneBridge::CachedFallbackMesh *SoftwareBLASSceneBridge::loadFallbackMesh(
    const std::string &virtualGeometryPath,
    const uint32_t fallbackLod,
    const std::optional<uint32_t> shapeIndex)
{
  const std::vector<FallbackCandidate> candidates = buildFallbackMeshCandidates(virtualGeometryPath, fallbackLod, shapeIndex, settings_.primitiveMode, settings_.preferRuntimeRbvhBuilds);
  std::string lastUnavailableCacheKey;

  for (uint32_t candidateIndex = 0u; candidateIndex < static_cast<uint32_t>(candidates.size()); ++candidateIndex)
  {
    const FallbackCandidate &candidate = candidates[candidateIndex];
    const std::filesystem::path &selectedPath = candidate.path;
    const uint32_t selectedLod = candidate.lodLevel;
    const bool exists = std::filesystem::exists(selectedPath);
    const bool shapeSpecificPath = isShapeSpecificFallbackPath(selectedPath, shapeIndex);
    const uint32_t meshIndex = shapeSpecificPath ? 0u : shapeIndex.value_or(0u);
    if (!exists && candidateIndex + 1u < static_cast<uint32_t>(candidates.size()))
    {
      continue;
    }

    std::string cacheKey = shapeIndex.has_value() ? buildShapeSourceKey(selectedPath.string(), shapeIndex) : selectedPath.string();
    if (!isUnifiedSoftwareRaytraceScenePath(selectedPath))
    {
      cacheKey += "::lod" + std::to_string(selectedLod);
    }
    if (isSWRBVHFallbackPath(selectedPath))
    {
      cacheKey += "::rbvhLod" + std::to_string(settings_.rbvhLod);
    }
    const std::filesystem::file_time_type lastWriteTime = exists ? std::filesystem::last_write_time(selectedPath) : std::filesystem::file_time_type{};

    auto existing = fallbackMeshCache_.find(cacheKey);
    if (existing != fallbackMeshCache_.end() &&
        existing->second.available == exists &&
        (!exists || existing->second.lastWriteTime == lastWriteTime))
    {
      if (existing->second.available)
      {
        return &existing->second;
      }
      lastUnavailableCacheKey = cacheKey;
      continue;
    }

    CachedFallbackMesh cached{};
    cached.available = false;
    cached.cacheKey = cacheKey;
    cached.fallbackPath = selectedPath.string();
    cached.isSWRT = isPrebuiltSoftwareFallbackPath(selectedPath);
    cached.lastWriteTime = lastWriteTime;
    if (!exists)
    {
      diagnostics_.lastMissingFallbackPath = cacheKey;
    }
    else if (cached.isSWRT)
    {
      const std::string collectionPath = selectedPath.string();
      std::string collectionKey = collectionPath;
      if (isUnifiedSoftwareRaytraceScenePath(selectedPath))
      {
        collectionKey += "::shape" + std::to_string(shapeIndex.value_or(0u));
      }
      auto existingCollection = fallbackCollectionCache_.find(collectionKey);
      if (existingCollection == fallbackCollectionCache_.end() ||
          existingCollection->second.available != exists ||
          existingCollection->second.lastWriteTime != lastWriteTime)
      {
        CachedFallbackCollection collection{};
        collection.available = false;
        collection.fallbackPath = collectionKey;
        collection.isSWRT = cached.isSWRT;
        collection.lastWriteTime = lastWriteTime;

        if (isSWRBVHFallbackPath(selectedPath))
        {
          std::vector<SWRBVHFallbackMeshData> swrbvhEntries;
          if (SWRBVHFallbackMeshFile::load(collectionPath, swrbvhEntries))
          {
            collection.available = true;
            collection.meshes.reserve(swrbvhEntries.size());
            collection.rbvhBuilds.reserve(swrbvhEntries.size());
            collection.rbvhBuildLods.reserve(swrbvhEntries.size());
            for (SWRBVHFallbackMeshData &entry : swrbvhEntries)
            {
              collection.meshes.push_back(std::move(entry.fallbackMesh));
              if (entry.rbvhBuilds.empty())
              {
                entry.rbvhBuilds.push_back(std::move(entry.rbvhBuild));
              }
              collection.rbvhBuilds.push_back(entry.rbvhBuilds.front());
              collection.rbvhBuildLods.push_back(std::move(entry.rbvhBuilds));
            }
          }
        }
        else if (isUnifiedSoftwareRaytraceScenePath(selectedPath))
        {
          SoftwareRayTraceSceneInstanceData entry{};
          const uint32_t selectedShapeIndex = shapeIndex.value_or(0u);
          if (SoftwareRayTraceSceneFile::loadEntryForShape(collectionPath, selectedShapeIndex, entry))
          {
            collection.available = true;
            collection.meshes.push_back(std::move(entry.fallbackMesh));
            collection.prebuiltBuilds.push_back(std::move(entry.build));
          }
        }
        else
        {
          std::vector<SWRTFallbackMeshData> swrtEntries;
          if (SWRTFallbackMeshFile::load(collectionPath, swrtEntries))
          {
            collection.available = true;
            collection.meshes.reserve(swrtEntries.size());
            collection.prebuiltBuilds.reserve(swrtEntries.size());
            for (SWRTFallbackMeshData &entry : swrtEntries)
            {
              collection.meshes.push_back(std::move(entry.fallbackMesh));
              collection.prebuiltBuilds.push_back(std::move(entry.build));
            }
          }
        }

        fallbackCollectionCache_[collectionKey] = std::move(collection);
        existingCollection = fallbackCollectionCache_.find(collectionKey);
      }

      if (existingCollection != fallbackCollectionCache_.end() && existingCollection->second.available)
      {
        const uint32_t selectedMeshIndex = isUnifiedSoftwareRaytraceScenePath(selectedPath) ? 0u : meshIndex;
        if (selectedMeshIndex < existingCollection->second.meshes.size())
        {
          cached.mesh = existingCollection->second.meshes[selectedMeshIndex];
          if (selectedMeshIndex < existingCollection->second.prebuiltBuilds.size())
            cached.prebuilt = existingCollection->second.prebuiltBuilds[selectedMeshIndex];
          else
            cached.prebuilt.reset();
          if (selectedMeshIndex < existingCollection->second.rbvhBuildLods.size() && !existingCollection->second.rbvhBuildLods[selectedMeshIndex].empty())
          {
            const uint32_t selectedRbvhLod = std::min(settings_.rbvhLod, static_cast<uint32_t>(existingCollection->second.rbvhBuildLods[selectedMeshIndex].size() - 1u));
            cached.rbvhBuild = existingCollection->second.rbvhBuildLods[selectedMeshIndex][selectedRbvhLod];
          }
          else if (selectedMeshIndex < existingCollection->second.rbvhBuilds.size())
          {
            cached.rbvhBuild = existingCollection->second.rbvhBuilds[selectedMeshIndex];
          }
          else
          {
            cached.rbvhBuild.reset();
          }
          cached.available = true;
          diagnostics_.lastLoadedFallbackPath = cached.fallbackPath;
        }
      }
    }
    else
    {
      const std::string collectionKey = selectedPath.string();
      auto existingCollection = fallbackCollectionCache_.find(collectionKey);
      if (existingCollection == fallbackCollectionCache_.end() ||
          existingCollection->second.available != exists ||
          existingCollection->second.lastWriteTime != lastWriteTime)
      {
        CachedFallbackCollection collection{};
        collection.available = false;
        collection.fallbackPath = collectionKey;
        collection.isSWRT = false;
        collection.lastWriteTime = lastWriteTime;
        if (virtualgeometry::FallbackMeshFile::load(collectionKey, collection.meshes))
          collection.available = true;
        fallbackCollectionCache_[collectionKey] = std::move(collection);
        existingCollection = fallbackCollectionCache_.find(collectionKey);
      }

      if (existingCollection != fallbackCollectionCache_.end() && existingCollection->second.available)
      {
        if (meshIndex < existingCollection->second.meshes.size())
        {
          cached.mesh = existingCollection->second.meshes[meshIndex];
          cached.prebuilt.reset();
          cached.available = true;
          diagnostics_.lastLoadedFallbackPath = cached.fallbackPath;
        }
      }
    }

    if (!cached.available)
    {
      diagnostics_.lastMissingFallbackPath = cached.fallbackPath;
    }

    fallbackMeshCache_[cacheKey] = std::move(cached);
    CachedFallbackMesh *loaded = &fallbackMeshCache_[cacheKey];
    if (loaded->available)
    {
      return loaded;
    }
    lastUnavailableCacheKey = cacheKey;
  }

  if (!lastUnavailableCacheKey.empty())
  {
    auto existing = fallbackMeshCache_.find(lastUnavailableCacheKey);
    if (existing != fallbackMeshCache_.end())
    {
      return &existing->second;
    }
  }
  return nullptr;
}

} // namespace softwarebvh
