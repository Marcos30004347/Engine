#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "virtualgeometry/FallbackMeshFile.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

namespace gpusigneddistancefields
{

class GPUSignedDistanceFieldsSceneBridge
{
public:
  struct Settings
  {
    uint32_t fallbackLod = 3u;
    uint32_t maxMeshUpdatesPerFrame = 4u;
  };

  explicit GPUSignedDistanceFieldsSceneBridge(GPUSignedDistanceFieldsManager &manager);
  GPUSignedDistanceFieldsSceneBridge(GPUSignedDistanceFieldsManager &manager, Settings settings);

  void sync(virtualgeometry::VirtualGeometryScene &scene, const math::Vec3f &clipmapCenter);

  const Settings &getSettings() const
  {
    return settings_;
  }

  void setSettings(Settings settings);

  uint32_t getPendingUpdateCount() const
  {
    return static_cast<uint32_t>(queuedInstances_.size());
  }

  uint32_t getLastFrameUpdatedCount() const
  {
    return lastFrameUpdatedCount_;
  }

  struct Diagnostics
  {
    uint32_t cachedFallbackMeshCount = 0u;
    uint32_t availableFallbackMeshCount = 0u;
    uint32_t missingFallbackMeshCount = 0u;
    uint32_t preparedInstanceCount = 0u;
    uint32_t staticInstanceCount = 0u;
    uint32_t skinnedInstanceCount = 0u;
    uint32_t uploadedMeshCount = 0u;
    uint32_t uploadedVertexCount = 0u;
    uint32_t uploadedIndexCount = 0u;
    uint32_t uploadedBoneWeightCount = 0u;
    uint32_t skippedEmptySceneInstanceCount = 0u;
    uint32_t skippedMissingRuntimeDataCount = 0u;
    uint32_t skippedMissingFallbackMeshCount = 0u;
    uint32_t skippedMissingSceneInstanceCount = 0u;
    uint32_t skippedInvalidBoundsCount = 0u;
    std::vector<uint32_t> staticJobCounts;
    std::vector<uint32_t> skinnedJobCounts;
    std::vector<uint32_t> staticJobVoxelCounts;
    std::vector<uint32_t> skinnedJobVoxelCounts;
    std::string lastLoadedFallbackPath;
    std::string lastMissingFallbackPath;
  };

  const Diagnostics &getDiagnostics() const
  {
    return diagnostics_;
  }

  bool isQueued(virtualgeometry::InstanceId instanceId) const;
  bool hasCompletedInitialBuild(virtualgeometry::InstanceId instanceId) const;

private:
  struct CachedFallbackMesh
  {
    bool available = false;
    std::string fallbackPath;
    virtualgeometry::FallbackMeshData mesh;
    uint32_t meshIndex = UINT32_MAX;
  };

  struct RecordBuildState
  {
    bool hasCompletedInitialBuild = false;
    bool queued = false;
    uint64_t lastCompletedContentRevision = 0u;
    uint64_t lastCompletedRebuildRevision = 0u;
    virtualgeometry::AABB lastBuiltBounds;
    bool hasLastBuiltBounds = false;
  };

  CachedFallbackMesh *loadFallbackMesh(const std::string &virtualGeometryPath, uint32_t fallbackLod);

  GPUSignedDistanceFieldsManager &manager_;
  Settings settings_{};
  std::unordered_map<std::string, CachedFallbackMesh> fallbackMeshCache_;
  std::unordered_map<virtualgeometry::InstanceId, RecordBuildState> recordBuildStates_;
  std::deque<virtualgeometry::InstanceId> pendingUpdateQueue_;
  std::unordered_set<virtualgeometry::InstanceId> queuedInstances_;
  std::unordered_map<std::string, uint32_t> meshIndexByFallbackPath_;
  std::vector<GPUSignedDistanceFieldsManager::MeshTableEntryGPU> meshTable_;
  std::vector<GPUSignedDistanceFieldsManager::SurfaceMaterialGPU> surfaceMaterials_;
  std::vector<GPUSignedDistanceFieldsManager::VertexGPU> vertices_;
  std::vector<uint32_t> indices_;
  std::vector<GPUSignedDistanceFieldsManager::BoneWeightGPU> boneWeights_;
  bool sharedGeometryDirty_ = false;
  uint32_t lastFrameUpdatedCount_ = 0u;
  Diagnostics diagnostics_{};
  uint32_t lastLoggedAvailableFallbackMeshCount_ = UINT32_MAX;
  uint32_t lastLoggedMissingFallbackMeshCount_ = UINT32_MAX;
  uint32_t lastLoggedPreparedInstanceCount_ = UINT32_MAX;
  uint32_t lastLoggedStaticInstanceCount_ = UINT32_MAX;
  uint32_t lastLoggedSkinnedInstanceCount_ = UINT32_MAX;
  uint32_t lastLoggedUploadedMeshCount_ = UINT32_MAX;
  uint32_t lastLoggedUploadedVertexCount_ = UINT32_MAX;
  uint32_t lastLoggedUploadedIndexCount_ = UINT32_MAX;
  uint32_t lastLoggedUploadedBoneWeightCount_ = UINT32_MAX;
  uint32_t lastLoggedSkippedEmptySceneInstanceCount_ = UINT32_MAX;
  uint32_t lastLoggedSkippedMissingRuntimeDataCount_ = UINT32_MAX;
  uint32_t lastLoggedSkippedMissingFallbackMeshCount_ = UINT32_MAX;
  uint32_t lastLoggedSkippedMissingSceneInstanceCount_ = UINT32_MAX;
  uint32_t lastLoggedSkippedInvalidBoundsCount_ = UINT32_MAX;
};

} // namespace gpusigneddistancefields
