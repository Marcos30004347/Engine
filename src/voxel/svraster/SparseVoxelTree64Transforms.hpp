#pragma once

#include "voxel/svraster/SparseVoxelTree64Directory.hpp"
#include "voxel/svraster/SparseVoxelTree64Scene.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct SparseVoxelTree64InstanceTransformRecord
{
  uint32_t stableInstanceId = 0u;
  uint32_t blasIndex = 0u;
  std::array<float, 16> modelToWorld{};
  SparseVoxelAABB worldBounds{};
};

struct SparseVoxelTree64TransformUpdate
{
  uint64_t transaction = 0u;
  uint64_t previousSceneRevision = 0u;
  uint64_t targetSceneRevision = 0u;
  uint64_t previousTopologyRevision = 0u;
  uint64_t targetTopologyRevision = 0u;
  uint64_t previousTransformRevision = 0u;
  uint64_t targetTransformRevision = 0u;
  bool topologyReplacement = false;
  bool fullActiveRebuild = false;
  std::vector<uint32_t> changedInstanceIds;
  std::vector<SparseVoxelAABB> invalidatedWorldBounds;
  std::vector<std::array<int32_t, 3>> dirtyChunks;
};

struct SparseVoxelTree64TransformStats
{
  uint32_t trackedInstances = 0u;
  uint32_t changedInstances = 0u;
  uint32_t invalidationBounds = 0u;
  uint32_t dirtyChunks = 0u;
  uint32_t activeChunks = 0u;
  uint64_t preparedUpdates = 0u;
  uint64_t committedUpdates = 0u;
  uint64_t staleCommits = 0u;
  uint64_t fullActiveRebuilds = 0u;
};

bool sparseVoxelTree64CaptureInstanceTransforms(const SparseVoxelSceneManager &scene, std::vector<SparseVoxelTree64InstanceTransformRecord> &records, std::string *error = nullptr);

bool sparseVoxelTree64PlanTransformInvalidation(
    const SparseVoxelTree64StateGPU &state,
    const std::vector<SparseVoxelTree64InstanceTransformRecord> &previous,
    const std::vector<SparseVoxelTree64InstanceTransformRecord> &current,
    bool topologyReplacement,
    bool completePreviousBounds,
    SparseVoxelTree64TransformUpdate &update,
    std::string *error = nullptr);

// Tracks the last Tree64-published transform snapshot. prepare is
// non-destructive; commit must happen only after the matching directory
// generation is published. This keeps repeated transforms relative to the
// actual shadow representation rather than merely the prior CPU frame.
class SparseVoxelTree64TransformTracker
{
public:
  bool initialize(const SparseVoxelSceneManager &scene, std::string *error = nullptr);
  bool prepare(const SparseVoxelSceneManager &scene, const SparseVoxelTree64StateGPU &state, SparseVoxelTree64TransformUpdate &update, std::string *error = nullptr);
  bool commit(const SparseVoxelSceneManager &scene, const SparseVoxelTree64TransformUpdate &update, std::string *error = nullptr);
  bool abort(const SparseVoxelTree64TransformUpdate &update, std::string *error = nullptr);

  bool initialized() const;
  bool hasPendingUpdate() const;
  uint64_t sceneRevision() const;
  uint64_t topologyRevision() const;
  uint64_t transformRevision() const;
  const std::vector<SparseVoxelTree64InstanceTransformRecord> &publishedRecords() const;
  SparseVoxelTree64TransformStats stats() const;

private:
  struct Pending
  {
    SparseVoxelTree64TransformUpdate update;
    std::vector<SparseVoxelTree64InstanceTransformRecord> records;
  };

  std::vector<SparseVoxelTree64InstanceTransformRecord> published_;
  std::optional<Pending> pending_;
  uint64_t sceneRevision_ = 0u;
  uint64_t topologyRevision_ = 0u;
  uint64_t transformRevision_ = 0u;
  uint64_t nextTransaction_ = 1u;
  SparseVoxelTree64TransformStats stats_{};
  bool initialized_ = false;
};

bool beginSparseVoxelTree64TransformRebuild(
    SparseVoxelTree64Directory &directory,
    const std::array<float, 3> &cameraWorldPosition,
    const SparseVoxelTree64TransformUpdate &transformUpdate,
    SparseVoxelTree64DirectoryUpdate &directoryUpdate,
    std::string *error = nullptr);

} // namespace voxel::svraster
