#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "virtualgeometry/FallbackMeshFile.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

namespace gpusigneddistancefields
{

// Replacement for GPUSignedDistanceFieldsSceneBridge.
//
// Key improvements over SceneBridge:
//  - Fallback meshes are loaded once at registration time; no per-frame filesystem I/O.
//  - Each instance occupies a stable GPU slot; only dirty instances write buffer slices.
//  - Skinned world bounds are cached; CPU skinning only runs when bone transforms change.
//  - Cascade-scroll jobs are limited to instances that intersect the newly exposed slab.
class GPUSignedDistanceFieldsSceneManager
{
public:
  struct Settings
  {
    uint32_t fallbackLod = 3u;
    uint32_t maxMeshUpdatesPerFrame = 4u;
  };

  explicit GPUSignedDistanceFieldsSceneManager(GPUSignedDistanceFieldsManager &manager);
  GPUSignedDistanceFieldsSceneManager(GPUSignedDistanceFieldsManager &manager, Settings settings);

  void sync(virtualgeometry::VirtualGeometryScene &scene, const math::Vec3f &clipmapCenter);

  const Settings &getSettings() const { return settings_; }
  void setSettings(Settings settings);

  uint32_t getPendingUpdateCount() const { return static_cast<uint32_t>(queuedInstances_.size()); }
  uint32_t getLastFrameUpdatedCount() const { return lastFrameUpdatedCount_; }

  struct Diagnostics
  {
    // Fallback mesh registry
    uint32_t cachedFallbackMeshCount = 0u;
    uint32_t availableFallbackMeshCount = 0u;
    uint32_t missingFallbackMeshCount = 0u;
    std::string lastLoadedFallbackPath;
    std::string lastMissingFallbackPath;

    // Instance counts
    uint32_t preparedInstanceCount = 0u;
    uint32_t staticInstanceCount = 0u;
    uint32_t skinnedInstanceCount = 0u;
    uint32_t skippedEmptySceneInstanceCount = 0u;
    uint32_t skippedMissingRuntimeDataCount = 0u;
    uint32_t skippedMissingFallbackMeshCount = 0u;
    uint32_t skippedMissingSceneInstanceCount = 0u;
    uint32_t skippedInvalidBoundsCount = 0u;

    // Per-frame upload stats
    uint32_t instanceSlicesUploadedThisFrame = 0u;
    uint32_t skinnedBoundsRecomputedThisFrame = 0u;
    uint32_t skinnedBoundsCachedThisFrame = 0u;

    // Shared geometry sizes (total, not per-frame)
    uint32_t uploadedMeshCount = 0u;
    uint32_t uploadedVertexCount = 0u;
    uint32_t uploadedIndexCount = 0u;
    uint32_t uploadedBoneWeightCount = 0u;

    // CPU sync timing breakdown (per frame, in milliseconds).
    double syncTotalMs = 0.0;
    double syncClipmapUpdateMs = 0.0;
    double syncSceneWalkMs = 0.0;
    double syncMeshLookupMs = 0.0;
    double syncBoundsMs = 0.0;
    double syncInstanceUploadMs = 0.0;
    double syncBoneUploadMs = 0.0;
    double syncEvictMs = 0.0;
    double syncSharedGeometryUploadMs = 0.0;
    double syncRebuildJobsMs = 0.0;
    double syncScrollJobsMs = 0.0;
    double syncJobUploadMs = 0.0;

    // Per-cascade job counts
    std::vector<uint32_t> staticJobCounts;
    std::vector<uint32_t> skinnedJobCounts;
    std::vector<uint32_t> staticJobVoxelCounts;
    std::vector<uint32_t> skinnedJobVoxelCounts;
  };

  const Diagnostics &getDiagnostics() const { return diagnostics_; }
  bool isQueued(virtualgeometry::InstanceId instanceId) const;
  bool hasCompletedInitialBuild(virtualgeometry::InstanceId instanceId) const;

private:
  // ---------------------------------------------------------------------------
  // Internal types
  // ---------------------------------------------------------------------------

  struct CachedFallbackMesh
  {
    bool available = false;
    std::string fallbackPath;
    virtualgeometry::FallbackMeshData mesh;
    uint32_t meshIndex = UINT32_MAX;
    // True if at least one bone in the skeleton has a non-zero capsuleRadius.
    // False for v1 files or non-skinned meshes — bounds cannot be computed and
    // the mesh is skipped for SDF job submission.
    bool hasCapsuleData = false;
  };

  struct InstanceState
  {
    virtualgeometry::InstanceId objectInstanceId = virtualgeometry::INVALID_INSTANCE_ID;
    uint32_t shapeIndex = 0u;
    bool hasSkinning = false;
    uint32_t gpuSlot = UINT32_MAX;
    // Stable start index into the shared bone matrices GPU buffer.
    // Stores current palette followed immediately by previous palette:
    //   [boneMatrixSlot .. boneMatrixSlot + boneCount)     → current
    //   [boneMatrixSlot + boneCount .. boneMatrixSlot + 2*boneCount) → previous
    uint32_t boneMatrixSlot = UINT32_MAX;
    uint32_t boneCount = 0u;
    uint32_t meshIndex = UINT32_MAX;

    // Cached state for dirty detection
    math::Mat4f cachedModelMatrix = math::Mat4f::identity();
    std::vector<math::Mat4f> cachedBonePalette;
    virtualgeometry::AABB cachedWorldBounds{};
    bool hasCachedBounds = false;

    // SDF rebuild tracking
    bool hasCompletedInitialBuild = false;
    bool queued = false;
    uint64_t lastCompletedContentRevision = 0u;
    uint64_t lastCompletedRebuildRevision = 0u;
    virtualgeometry::AABB lastBuiltBounds{};
    bool hasLastBuiltBounds = false;
  };

  // ---------------------------------------------------------------------------
  // Private helpers
  // ---------------------------------------------------------------------------

  using SceneInstanceKey = uint64_t;

  static SceneInstanceKey makeSceneInstanceKey(virtualgeometry::InstanceId instanceId, uint32_t shapeIndex);
  static virtualgeometry::InstanceId sceneInstanceKeyObjectId(SceneInstanceKey key);
  static uint32_t sceneInstanceKeyShapeIndex(SceneInstanceKey key);

  CachedFallbackMesh *getOrLoadMesh(const std::string &vgPath, std::optional<uint32_t> shapeIndex);

  uint32_t allocateGpuSlot(GPUSignedDistanceFieldsManager::Path path);
  void freeGpuSlot(GPUSignedDistanceFieldsManager::Path path, uint32_t slot);
  // Bone matrix bump allocator: allocates `matrixCount` consecutive slots.
  uint32_t allocateBoneMatrixSlot(uint32_t matrixCount);

  struct ExposedSlabs
  {
    struct Slab
    {
      float min[3];
      float max[3];
    };
    // Up to one slab per world axis (x, y, z).
    Slab slabs[3];
    bool active[3] = {false, false, false};
  };

  static ExposedSlabs computeExposedSlabs(const GPUSignedDistanceFieldsManager::CascadeInfoGPU &cascade);
  static bool instanceOverlapsExposedSlabs(const virtualgeometry::AABB &bounds, const ExposedSlabs &slabs);

  void appendJobsForInstance(
      const InstanceState &state,
      const virtualgeometry::AABB &jobBounds,
      bool needsInvalidate,
      std::vector<std::vector<GPUSignedDistanceFieldsManager::JobGPU>> &staticJobs,
      std::vector<std::vector<GPUSignedDistanceFieldsManager::JobGPU>> &skinnedJobs) const;

  // ---------------------------------------------------------------------------
  // State
  // ---------------------------------------------------------------------------

  GPUSignedDistanceFieldsManager &manager_;
  Settings settings_{};

  // Mesh registry: vgPath → cached entry.
  // Populated lazily on first encounter of each unique vgPath.
  std::unordered_map<std::string, CachedFallbackMesh> meshCache_;
  // Candidate search uses a separate key-to-cache-key indirection so that
  // different requested paths that resolve to the same file share one entry.
  std::unordered_map<std::string, std::string> candidateToCacheKey_;

  // Shared geometry accumulated from all registered meshes.
  // Uploaded to GPU once when a new mesh is first registered.
  std::vector<GPUSignedDistanceFieldsManager::MeshTableEntryGPU> meshTable_;
  std::vector<GPUSignedDistanceFieldsManager::SurfaceMaterialGPU> surfaceMaterials_;
  std::vector<GPUSignedDistanceFieldsManager::VertexGPU> vertices_;
  std::vector<uint32_t> indices_;
  std::vector<GPUSignedDistanceFieldsManager::BoneWeightGPU> boneWeights_;
  bool meshTableDirty_ = false;

  // Per-instance state keyed by object instance id + shape index.
  std::unordered_map<SceneInstanceKey, InstanceState> instances_;

  // Slot free-lists for static and skinned GPU instance buffers.
  std::vector<uint32_t> staticFreeSlots_;
  uint32_t nextStaticSlot_ = 0u;
  uint32_t staticActiveCount_ = 0u;

  std::vector<uint32_t> skinnedFreeSlots_;
  uint32_t nextSkinnedSlot_ = 0u;
  uint32_t skinnedActiveCount_ = 0u;

  // Bone matrix bump allocator (no GC; slots are stable for instance lifetime).
  uint32_t nextBoneMatrixSlot_ = 0u;

  // SDF rebuild queue.
  std::deque<SceneInstanceKey> pendingUpdateQueue_;
  std::unordered_set<SceneInstanceKey> queuedInstances_;
  uint32_t lastFrameUpdatedCount_ = 0u;

  Diagnostics diagnostics_{};
};

} // namespace gpusigneddistancefields
