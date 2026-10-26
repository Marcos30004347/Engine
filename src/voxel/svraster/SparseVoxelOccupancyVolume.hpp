#pragma once

#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelSceneManager.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

class SparseVoxelFileRuntime;

struct SparseVoxelOccupancySettings
{
  std::array<float, 3> worldExtent{64.0f, 64.0f, 64.0f};
  std::array<uint32_t, 3> logicalResolution{128u, 128u, 128u};
  uint64_t memoryCeilingBytes = 256ull * 1024ull * 1024ull;
  // Tests retain a scalar mirror as an oracle. Runtime GPU producers may
  // disable it and use this object only for toroidal dirty planning/state.
  bool maintainCPUReference = true;
};

struct SparseVoxelOccupancyLimits
{
  uint32_t maxTextureDimension3D = 2048u;
  uint64_t maxStorageBufferBytes = 1ull << 30u;
};

struct SparseVoxelOccupancyLayout
{
  std::array<uint32_t, 3> logicalResolution{};
  std::array<uint32_t, 3> wordResolution{};
  std::array<float, 3> cellSize{};
  std::array<float, 3> worldExtent{};
  uint64_t wordCount = 0u;
  uint64_t residentTextureBytes = 0u;
  uint64_t persistentBytes = 0u;
};

struct SparseVoxelOccupancyMipLayout
{
  SparseVoxelOccupancyMipLevelGPU gpu{};
  uint64_t residentBytes = 0u;
};

struct SparseVoxelOccupancyPyramidLayout
{
  std::vector<SparseVoxelOccupancyMipLayout> levels;
  uint64_t wordCount = 0u;
  uint64_t residentBytes = 0u;
  uint64_t persistentBytes = 0u;
};

struct SparseVoxelOccupancyAddress
{
  std::array<uint32_t, 3> physicalCell{};
  uint32_t wordIndex = 0u;
  uint32_t bitMask = 0u;
};

struct SparseVoxelOccupancyUpdate
{
  SparseVoxelOccupancyVolumeGPU pendingState{};
  std::vector<SparseVoxelOccupancyDirtyWordGPU> dirtyWords;
};

struct SparseVoxelOccupancyMipLevel
{
  SparseVoxelOccupancyMipLevelGPU state{};
  std::vector<uint32_t> words;
  std::vector<SparseVoxelOccupancyDirtyWordGPU> lastDirtyWords;
};

bool makeSparseVoxelOccupancyLayout(const SparseVoxelOccupancySettings &settings, const SparseVoxelOccupancyLimits &limits, SparseVoxelOccupancyLayout &layout, std::string *error = nullptr);

bool makeSparseVoxelOccupancyPyramidLayout(
    const SparseVoxelOccupancySettings &settings,
    const SparseVoxelOccupancyLimits &limits,
    const SparseVoxelOccupancyLayout &baseLayout,
    SparseVoxelOccupancyPyramidLayout &layout,
    std::string *error = nullptr);

bool prepareSparseVoxelOccupancyUpdate(
    const SparseVoxelOccupancyLayout &layout,
    const SparseVoxelOccupancyVolumeGPU *committedState,
    const std::array<float, 3> &cameraWorldPosition,
    uint64_t sceneRevision,
    SparseVoxelOccupancyUpdate &update,
    std::string *error = nullptr);

bool prepareSparseVoxelOccupancyUpdate(
    const SparseVoxelOccupancyLayout &layout,
    const SparseVoxelOccupancyVolumeGPU *committedState,
    const std::array<float, 3> &cameraWorldPosition,
    uint64_t sceneRevision,
    const std::vector<SparseVoxelAABB> &invalidatedWorldBounds,
    SparseVoxelOccupancyUpdate &update,
    std::string *error = nullptr);

SparseVoxelOccupancyAddress sparseVoxelOccupancyAddress(const std::array<uint32_t, 3> &logicalCell, const SparseVoxelOccupancyVolumeGPU &state);

std::array<uint32_t, 3> sparseVoxelOccupancyLogicalCellFromPhysical(const std::array<uint32_t, 3> &physicalCell, const SparseVoxelOccupancyVolumeGPU &state);

bool sparseVoxelOccupancyDirtyLogicalCell(const std::array<uint32_t, 3> &logicalCell, const SparseVoxelOccupancyVolumeGPU &state);

// Returns one full-volume box for a rebuild, or the logical X/Y/Z dirty-slab
// union represented by at most three overlapping world-space boxes.
std::vector<SparseVoxelAABB> sparseVoxelOccupancyQueryBoxes(const SparseVoxelOccupancyVolumeGPU &state);

// Adds interior invalidations to the camera-scroll query. If more than three
// boxes are needed, conservatively merges them so the fixed TLAS query ABI
// cannot reject a source that may repopulate a dirty cell.
std::vector<SparseVoxelAABB> sparseVoxelOccupancyQueryBoxes(
    const SparseVoxelOccupancyVolumeGPU &state,
    const std::vector<SparseVoxelAABB> &invalidatedWorldBounds);

uint32_t sparseVoxelOccupancyTailMask(uint32_t logicalResolutionX);

SparseVoxelOccupancyAddress sparseVoxelOccupancyMipAddress(const std::array<uint32_t, 3> &logicalCell, const SparseVoxelOccupancyMipLevelGPU &state);

std::array<uint32_t, 3> sparseVoxelOccupancyMipLogicalCellFromPhysical(const std::array<uint32_t, 3> &physicalCell, const SparseVoxelOccupancyMipLevelGPU &state);

std::vector<SparseVoxelAABB> collectSparseVoxelOccupancyLeafBounds(const SparseVoxelSceneManager &sceneManager);

// Streaming scenes keep only their roots in SparseVoxelScene. Use the
// published physical hierarchy so newly installed fine leaves contribute to
// occupancy without rebuilding the scene manager.
std::vector<SparseVoxelAABB> collectSparseVoxelOccupancyLeafBounds(
    const SparseVoxelSceneManager &sceneManager,
    const SparseVoxelFileRuntime &fileRuntime);

// Scalar transactional reference used by tests and as the oracle for the GPU
// clear/splat/copy implementation. Words use the exact public 3D texture layout.
class SparseVoxelOccupancyVolume
{
public:
  bool configure(const SparseVoxelOccupancySettings &settings, const SparseVoxelOccupancyLimits &limits = {}, std::string *error = nullptr);

  bool update(const std::array<float, 3> &cameraWorldPosition, uint64_t sceneRevision, const std::vector<SparseVoxelAABB> &sourceLeafBounds, std::string *error = nullptr);
  bool update(
      const std::array<float, 3> &cameraWorldPosition,
      uint64_t sceneRevision,
      const std::vector<SparseVoxelAABB> &sourceLeafBounds,
      const std::vector<SparseVoxelAABB> &invalidatedWorldBounds,
      std::string *error = nullptr);
  // Production metadata-only update. The GPU derives dirty masks, compacts
  // intersecting sources, clears/splats mip 0, and rebuilds the hierarchy.
  // This function performs no cell scan, source rasterization, or dirty-cell
  // propagation.
  bool updateGPU(
      const std::array<float, 3> &cameraWorldPosition,
      uint64_t sceneRevision,
      const std::vector<SparseVoxelAABB> &invalidatedWorldBounds,
      std::string *error = nullptr);

  bool occupiedAtWorldPosition(const std::array<float, 3> &worldPosition) const;
  bool occupiedAtGlobalCell(uint32_t level, const std::array<int32_t, 3> &globalCell) const;

  const SparseVoxelOccupancyLayout &layout() const
  {
    return layout_;
  }
  const SparseVoxelOccupancyVolumeGPU &state() const
  {
    return state_;
  }
  const std::vector<uint32_t> &words() const
  {
    return words_;
  }
  const std::vector<SparseVoxelOccupancyDirtyWordGPU> &lastDirtyWords() const
  {
    return lastDirtyWords_;
  }
  const SparseVoxelOccupancyPyramidLayout &pyramidLayout() const
  {
    return pyramidLayout_;
  }
  const std::vector<SparseVoxelOccupancyMipLevel> &mipLevels() const
  {
    return mipLevels_;
  }

private:
  SparseVoxelOccupancyLayout layout_{};
  SparseVoxelOccupancyPyramidLayout pyramidLayout_{};
  SparseVoxelOccupancyVolumeGPU state_{};
  std::vector<uint32_t> words_;
  std::vector<SparseVoxelOccupancyDirtyWordGPU> lastDirtyWords_;
  std::vector<SparseVoxelOccupancyMipLevel> mipLevels_;
  bool maintainCPUReference_ = true;
  bool configured_ = false;
};

} // namespace voxel::svraster
