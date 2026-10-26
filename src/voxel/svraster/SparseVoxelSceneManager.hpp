#pragma once

#include "voxel/svraster/SparseVoxelScene.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct SparseVoxelAABB
{
  std::array<float, 3> minimum{};
  std::array<float, 3> maximum{};
};

struct SparseVoxelRuntimeInstance
{
  uint32_t stableId = 0u;
  SparseVoxelInstanceGPU gpu{};
};

struct SparseVoxelTLAS
{
  std::vector<SparseVoxelTLASNodeGPU> nodes;
  std::vector<uint32_t> instanceIndices;
  uint32_t rootNode = kInvalidSparseVoxelIndex;
  uint32_t maxDepth = 0u;
};

// Owns renderer-derived scene state. SparseVoxelScene remains an immutable
// authoring/file contract; transforms, inverses, bounds and the TLAS live here.
class SparseVoxelSceneManager
{
public:
  bool setScene(const SparseVoxelScene &scene, std::string *error = nullptr);
  bool setInstanceTransform(uint32_t stableInstanceId, const std::array<float, 16> &modelToWorld, std::string *error = nullptr);
  void commitPreviousTransforms();

  const SparseVoxelScene &scene() const { return scene_; }
  const std::vector<SparseVoxelRuntimeInstance> &runtimeInstances() const { return runtimeInstances_; }
  const std::vector<SparseVoxelBLASRootRangeGPU> &blasRootRanges() const { return blasRootRanges_; }
  const SparseVoxelTLAS &tlas() const { return tlas_; }

  uint64_t sceneRevision() const { return sceneRevision_; }
  uint64_t topologyRevision() const { return topologyRevision_; }
  uint64_t transformRevision() const { return transformRevision_; }

private:
  bool rebuildDerivedState(std::string *error);

  SparseVoxelScene scene_{};
  std::vector<SparseVoxelRuntimeInstance> runtimeInstances_;
  std::vector<SparseVoxelBLASRootRangeGPU> blasRootRanges_;
  SparseVoxelTLAS tlas_{};
  uint64_t sceneRevision_ = 0u;
  uint64_t topologyRevision_ = 0u;
  uint64_t transformRevision_ = 0u;
};

bool validateSparseVoxelTLAS(const SparseVoxelSceneManager &manager, std::string *error = nullptr);

} // namespace voxel::svraster
