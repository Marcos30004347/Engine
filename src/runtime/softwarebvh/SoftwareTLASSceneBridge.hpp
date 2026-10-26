#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "softwarebvh/SoftwareTLASManager.hpp"

namespace softwarebvh
{

class SoftwareBLASManager;

class SoftwareTLASSceneBridge
{
public:
  struct Diagnostics
  {
    uint32_t preparedInstanceCount = 0u;
    uint32_t validInstanceCount = 0u;
    uint32_t dirtyInstanceCount = 0u;
    uint32_t rebuildDirtyInstanceCount = 0u;
    uint32_t refitDirtyInstanceCount = 0u;
    uint32_t skippedMissingObjectCount = 0u;
    uint32_t skippedMissingSceneInstanceCount = 0u;
    uint32_t skippedMissingBlasCount = 0u;
    uint32_t skippedInvalidBoundsCount = 0u;
    uint32_t candidateInstanceCount = 0u;
    uint32_t filteredInstanceCount = 0u;
    virtualgeometry::SceneInstanceId selectedSceneInstanceId = virtualgeometry::INVALID_SCENE_INSTANCE_ID;
    bool topologyChanged = false;
    bool requestedRebuild = false;
    bool requestedRefit = false;
  };

  struct DebugInstanceFilter
  {
    bool isolateSingleInstance = false;
    uint32_t selectedInstanceIndex = 0u;
  };

  explicit SoftwareTLASSceneBridge(SoftwareTLASManager &manager);
  SoftwareTLASSceneBridge(SoftwareTLASManager &manager, const SoftwareBLASManager *blasManager);

  void sync(virtualgeometry::VirtualGeometryScene &scene);
  void setDebugInstanceFilter(const DebugInstanceFilter &filter);

  const DebugInstanceFilter &getDebugInstanceFilter() const
  {
    return debugInstanceFilter_;
  }

  const Diagnostics &getDiagnostics() const
  {
    return diagnostics_;
  }

private:
  struct InstanceState
  {
    uint64_t contentRevision = 0u;
    uint64_t rebuildRevision = 0u;
    math::Mat4f transform = math::Mat4f::identity();
    math::Mat4f previousTransform = math::Mat4f::identity();
    virtualgeometry::AABB worldBounds{};
    uint32_t blasHandle = SoftwareTLASManager::INVALID_BLAS_HANDLE;
    bool hasCachedInputs = false;
    bool seenThisFrame = false;
  };

  static bool isValidAabb(const virtualgeometry::AABB &bounds);
  static virtualgeometry::AABB transformAabb(const virtualgeometry::AABB &localBounds, const math::Mat4f &transform);
  static virtualgeometry::AABB computeWorldBounds(
      const virtualgeometry::VirtualGeometryScene::InstanceManager &instanceManager,
      const std::vector<virtualgeometry::SceneInstanceId> &sceneInstanceIds,
      const virtualgeometry::AABB &localBounds,
      bool *outHadMissingSceneInstance);

  SoftwareTLASManager &manager_;
  const SoftwareBLASManager *blasManager_ = nullptr;
  DebugInstanceFilter debugInstanceFilter_{};
  std::unordered_map<virtualgeometry::SceneInstanceId, InstanceState> instanceStates_;
  std::vector<virtualgeometry::SceneInstanceId> previousInstanceOrder_;
  Diagnostics diagnostics_{};
};

} // namespace softwarebvh
