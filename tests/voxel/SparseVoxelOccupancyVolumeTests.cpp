#include "voxel/svraster/SparseVoxelGenerator.hpp"
#include "voxel/svraster/SparseVoxelOccupancyVolume.hpp"

#include <algorithm>
#include <array>
// These tests use assert() as their check mechanism, and Release builds
// define NDEBUG. Without this the assertions vanish - and because many of
// them wrap the call they are checking, the call under test vanishes with
// them, leaving objects unconfigured and the suite crashing or silently
// passing. cassert re-reads NDEBUG on every include, so undefining it here
// restores assert regardless of what included cassert earlier.
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{

using namespace voxel::svraster;

SparseVoxelOccupancySettings settings(const std::array<uint32_t, 3> resolution = {35u, 3u, 2u})
{
  SparseVoxelOccupancySettings value{};
  value.logicalResolution = resolution;
  value.worldExtent = {
    static_cast<float>(resolution[0]),
    static_cast<float>(resolution[1]),
    static_cast<float>(resolution[2]),
  };
  value.memoryCeilingBytes = 1u << 20u;
  return value;
}

SparseVoxelOccupancyLimits limits()
{
  return {
    .maxTextureDimension3D = 256u,
    .maxStorageBufferBytes = 1u << 20u,
  };
}

uint64_t occupiedBitCount(const std::vector<uint32_t> &words)
{
  uint64_t count = 0u;
  for (uint32_t word : words)
    while (word != 0u)
    {
      word &= word - 1u;
      ++count;
    }
  return count;
}

void testCheckedLayoutAndABI()
{
  SparseVoxelOccupancyLayout layout{};
  std::string error;
  assert(makeSparseVoxelOccupancyLayout(settings(), limits(), layout, &error));
  assert(error.empty());
  assert(layout.wordResolution == (std::array<uint32_t, 3>{2u, 3u, 2u}));
  assert(layout.wordCount == 12u);
  assert(layout.residentTextureBytes == 48u);
  assert(layout.persistentBytes == 96u);
  assert(layout.cellSize == (std::array<float, 3>{1.0f, 1.0f, 1.0f}));
  assert(sizeof(SparseVoxelOccupancyVolumeGPU) == 144u);
  assert(sizeof(SparseVoxelOccupancyDispatchGPU) == 16u);
  assert(sizeof(SparseVoxelOccupancyAABBGPU) == 32u);
  assert(sizeof(SparseVoxelOccupancyProbeGPU) == 160u);
  assert(sizeof(SparseVoxelOccupancyMipLevelGPU) == 80u);
  assert(sizeof(SparseVoxelOccupancyMipBuildGPU) == 320u);
  assert(offsetof(SparseVoxelOccupancyVolumeGPU, wrapOffset) == 80u);
  assert(offsetof(SparseVoxelOccupancyVolumeGPU, cellSize) == 112u);
  assert(sparseVoxelOccupancyTailMask(35u) == 0x7u);
  assert(sparseVoxelOccupancyTailMask(32u) == 0xffffffffu);

  auto invalid = settings();
  invalid.logicalResolution[0] = 0u;
  assert(!makeSparseVoxelOccupancyLayout(invalid, limits(), layout, &error));
  invalid = settings({65u, 3u, 2u});
  auto lowTextureLimit = limits();
  lowTextureLimit.maxTextureDimension3D = 2u;
  assert(!makeSparseVoxelOccupancyLayout(invalid, lowTextureLimit, layout, &error));
  invalid = settings();
  invalid.memoryCeilingBytes = 95u;
  assert(!makeSparseVoxelOccupancyLayout(invalid, limits(), layout, &error));
  assert(makeSparseVoxelOccupancyLayout(settings(), limits(), layout, &error));
  SparseVoxelOccupancyPyramidLayout pyramid{};
  assert(makeSparseVoxelOccupancyPyramidLayout(settings(), limits(), layout, pyramid, &error));
  assert(pyramid.levels.size() == 6u);
  assert(pyramid.wordCount == 24u);
  assert(pyramid.residentBytes == 96u);
  assert(pyramid.persistentBytes == 192u);
  for (uint32_t level = 0u; level < pyramid.levels.size(); ++level)
  {
    assert(pyramid.levels[level].gpu.level == level + 1u);
    assert(pyramid.levels[level].gpu.scale == (2u << level));
    assert(pyramid.levels[level].gpu.wordOffset == level * 4u);
    assert(pyramid.levels[level].gpu.wordCount == 4u);
  }
  auto pyramidTooLarge = settings();
  pyramidTooLarge.memoryCeilingBytes = 200u;
  assert(!makeSparseVoxelOccupancyPyramidLayout(pyramidTooLarge, limits(), layout, pyramid, &error));
  invalid = settings();
  invalid.worldExtent[1] = std::numeric_limits<float>::infinity();
  assert(!makeSparseVoxelOccupancyLayout(invalid, limits(), layout, &error));
}

void testToroidalAddressingBijection()
{
  SparseVoxelOccupancyLayout layout{};
  std::string error;
  assert(makeSparseVoxelOccupancyLayout(settings(), limits(), layout, &error));
  SparseVoxelOccupancyUpdate update{};
  assert(prepareSparseVoxelOccupancyUpdate(layout, nullptr, {-0.25f, -1.75f, 0.5f}, 7u, update, &error));
  auto &state = update.pendingState;
  assert(state.cameraCell == (std::array<int32_t, 3>{-1, -2, 0}));
  assert(std::abs(state.cameraInnerCell[0] - 0.75f) < 1.0e-6f);
  assert(std::abs(state.cameraInnerCell[1] - 0.25f) < 1.0e-6f);
  assert(state.originCell == (std::array<int32_t, 3>{-18, -3, -1}));
  state.wrapOffset = {34u, 2u, 1u};

  std::vector<uint32_t> seen(static_cast<size_t>(layout.wordCount), 0u);
  for (uint32_t z = 0u; z < state.logicalResolution[2]; ++z)
    for (uint32_t y = 0u; y < state.logicalResolution[1]; ++y)
      for (uint32_t x = 0u; x < state.logicalResolution[0]; ++x)
      {
        const std::array<uint32_t, 3> logical{x, y, z};
        const auto address = sparseVoxelOccupancyAddress(logical, state);
        assert(address.wordIndex < layout.wordCount);
        assert((seen[address.wordIndex] & address.bitMask) == 0u);
        seen[address.wordIndex] |= address.bitMask;
        assert(sparseVoxelOccupancyLogicalCellFromPhysical(address.physicalCell, state) == logical);
      }
  assert(occupiedBitCount(seen) == 35u * 3u * 2u);
  for (uint32_t z = 0u; z < 2u; ++z)
    for (uint32_t y = 0u; y < 3u; ++y)
      assert((seen[1u + 2u * (y + 3u * z)] & ~0x7u) == 0u);
}

void testNewlyExposedToroidalSlabs()
{
  SparseVoxelOccupancyLayout layout{};
  std::string error;
  assert(makeSparseVoxelOccupancyLayout(settings(), limits(), layout, &error));
  SparseVoxelOccupancyUpdate first{};
  assert(prepareSparseVoxelOccupancyUpdate(layout, nullptr, {0.25f, 0.25f, 0.25f}, 1u, first, &error));
  first.pendingState.valid = 1u;
  first.pendingState.contentGeneration = 1u;
  assert(first.dirtyWords.size() == layout.wordCount);

  SparseVoxelOccupancyUpdate xScroll{};
  assert(prepareSparseVoxelOccupancyUpdate(layout, &first.pendingState, {1.25f, 0.25f, 0.25f}, 1u, xScroll, &error));
  assert(xScroll.pendingState.fullRebuild == 0u);
  assert(xScroll.pendingState.scrollDelta == (std::array<int32_t, 3>{1, 0, 0}));
  assert(xScroll.pendingState.wrapOffset == (std::array<uint32_t, 3>{1u, 0u, 0u}));
  assert(xScroll.dirtyWords.size() == 3u * 2u);
  for (const auto dirty : xScroll.dirtyWords)
    assert(dirty.bitMask == 1u);

  SparseVoxelOccupancyUpdate diagonal{};
  assert(prepareSparseVoxelOccupancyUpdate(layout, &first.pendingState, {1.25f, 1.25f, 1.25f}, 1u, diagonal, &error));
  uint64_t dirtyBits = 0u;
  for (const auto dirty : diagonal.dirtyWords)
    dirtyBits += occupiedBitCount({dirty.bitMask});
  const uint64_t total = 35u * 3u * 2u;
  const uint64_t retained = 34u * 2u * 1u;
  assert(dirtyBits == total - retained);

  SparseVoxelOccupancyUpdate subCell{};
  assert(prepareSparseVoxelOccupancyUpdate(layout, &first.pendingState, {0.75f, 0.75f, 0.75f}, 1u, subCell, &error));
  assert(subCell.dirtyWords.empty());
  assert(subCell.pendingState.cameraCell == first.pendingState.cameraCell);
  assert(subCell.pendingState.cameraInnerCell != first.pendingState.cameraInnerCell);

  SparseVoxelOccupancyUpdate teleport{};
  assert(prepareSparseVoxelOccupancyUpdate(layout, &first.pendingState, {35.25f, 0.25f, 0.25f}, 1u, teleport, &error));
  assert(teleport.pendingState.fullRebuild != 0u);
  assert(teleport.dirtyWords.size() == layout.wordCount);

  SparseVoxelOccupancyUpdate sceneChange{};
  assert(prepareSparseVoxelOccupancyUpdate(layout, &first.pendingState, {0.25f, 0.25f, 0.25f}, 2u, sceneChange, &error));
  assert(sceneChange.pendingState.fullRebuild != 0u);
  assert(sceneChange.dirtyWords.size() == layout.wordCount);
}

std::vector<std::array<float, 3>> samplePoints(const SparseVoxelOccupancyVolumeGPU &state)
{
  std::vector<std::array<float, 3>> points;
  for (uint32_t z = 0u; z < state.logicalResolution[2]; ++z)
    for (uint32_t y = 0u; y < state.logicalResolution[1]; ++y)
      for (uint32_t x = 0u; x < state.logicalResolution[0]; ++x)
        points.push_back({
          (static_cast<float>(state.originCell[0]) + x + 0.5f) * state.cellSize[0],
          (static_cast<float>(state.originCell[1]) + y + 0.5f) * state.cellSize[1],
          (static_cast<float>(state.originCell[2]) + z + 0.5f) * state.cellSize[2],
        });
  return points;
}

void assertPyramidReduction(const SparseVoxelOccupancyVolume &volume)
{
  for (uint32_t level = 1u; level <= volume.mipLevels().size(); ++level)
  {
    const auto &mip = volume.mipLevels()[level - 1u];
    for (uint32_t z = 0u; z < mip.state.activeResolution[2]; ++z)
      for (uint32_t y = 0u; y < mip.state.activeResolution[1]; ++y)
        for (uint32_t x = 0u; x < mip.state.activeResolution[0]; ++x)
        {
          const std::array<int32_t, 3> parent{
            static_cast<int32_t>(mip.state.originCell[0] + x),
            static_cast<int32_t>(mip.state.originCell[1] + y),
            static_cast<int32_t>(mip.state.originCell[2] + z),
          };
          bool childOccupied = false;
          for (uint32_t childIndex = 0u; childIndex < 8u; ++childIndex)
          {
            const std::array<int32_t, 3> child{
              static_cast<int32_t>(static_cast<int64_t>(parent[0]) * 2 + ((childIndex & 1u) != 0u ? 1 : 0)),
              static_cast<int32_t>(static_cast<int64_t>(parent[1]) * 2 + ((childIndex & 2u) != 0u ? 1 : 0)),
              static_cast<int32_t>(static_cast<int64_t>(parent[2]) * 2 + ((childIndex & 4u) != 0u ? 1 : 0)),
            };
            childOccupied |= volume.occupiedAtGlobalCell(level - 1u, child);
          }
          assert(volume.occupiedAtGlobalCell(level, parent) == childOccupied);
        }

    for (uint32_t z = 0u; z < mip.state.logicalResolution[2]; ++z)
      for (uint32_t y = 0u; y < mip.state.logicalResolution[1]; ++y)
        for (uint32_t x = 0u; x < mip.state.logicalResolution[0]; ++x)
        {
          if (x < mip.state.activeResolution[0] && y < mip.state.activeResolution[1] && z < mip.state.activeResolution[2])
            continue;
          const auto address = sparseVoxelOccupancyMipAddress({x, y, z}, mip.state);
          assert((mip.words[address.wordIndex] & address.bitMask) == 0u);
        }
  }
}

void assertPyramidsEquivalent(const SparseVoxelOccupancyVolume &left, const SparseVoxelOccupancyVolume &right)
{
  assert(left.mipLevels().size() == right.mipLevels().size());
  for (uint32_t level = 1u; level <= left.mipLevels().size(); ++level)
  {
    const auto &leftMip = left.mipLevels()[level - 1u];
    const auto &rightMip = right.mipLevels()[level - 1u];
    assert(leftMip.state.logicalResolution == rightMip.state.logicalResolution);
    assert(leftMip.state.originCell == rightMip.state.originCell);
    assert(leftMip.state.activeResolution == rightMip.state.activeResolution);
    for (uint32_t z = 0u; z < leftMip.state.activeResolution[2]; ++z)
      for (uint32_t y = 0u; y < leftMip.state.activeResolution[1]; ++y)
        for (uint32_t x = 0u; x < leftMip.state.activeResolution[0]; ++x)
        {
          const std::array<int32_t, 3> global{
            static_cast<int32_t>(leftMip.state.originCell[0] + x),
            static_cast<int32_t>(leftMip.state.originCell[1] + y),
            static_cast<int32_t>(leftMip.state.originCell[2] + z),
          };
          assert(left.occupiedAtGlobalCell(level, global) == right.occupiedAtGlobalCell(level, global));
        }
  }
}

void testIncrementalSplatsMatchFullRebuild()
{
  const auto volumeSettings = settings({8u, 6u, 4u});
  const std::vector<SparseVoxelAABB> boxes = {
    {{-3.4f, -1.4f, -0.4f}, {-1.6f, 1.4f, 1.4f}},
    {{1.2f, -2.2f, -1.2f}, {3.6f, -0.2f, 0.2f}},
    {{4.2f, 0.2f, 0.2f}, {6.8f, 2.8f, 2.8f}},
  };
  SparseVoxelOccupancyVolume incremental;
  SparseVoxelOccupancyVolume rebuilt;
  std::string error;
  assert(incremental.configure(volumeSettings, limits(), &error));
  assert(incremental.update({0.25f, 0.25f, 0.25f}, 1u, boxes, &error));
  assert(occupiedBitCount(incremental.words()) > 0u);
  assertPyramidReduction(incremental);
  const auto initialWords = incremental.words();
  const uint32_t initialGeneration = incremental.state().contentGeneration;
  assert(incremental.update({0.75f, 0.75f, 0.75f}, 1u, boxes, &error));
  assert(incremental.lastDirtyWords().empty());
  assert(incremental.words() == initialWords);
  assert(incremental.state().contentGeneration == initialGeneration);

  const std::array<float, 3> movedCamera{1.25f, -0.75f, 1.25f};
  const auto beforeScrollWords = incremental.words();
  const auto beforeScrollMips = incremental.mipLevels();
  assert(incremental.update(movedCamera, 1u, boxes, &error));
  assert(!incremental.lastDirtyWords().empty());
  uint64_t incrementalDirtyBits = 0u;
  for (const auto dirty : incremental.lastDirtyWords())
    incrementalDirtyBits += occupiedBitCount({dirty.bitMask});
  assert(incrementalDirtyBits < 8u * 6u * 4u);
  std::vector<uint32_t> dirtyMasks(incremental.layout().wordCount, 0u);
  for (const auto dirty : incremental.lastDirtyWords())
    dirtyMasks[dirty.wordIndex] = dirty.bitMask;
  for (uint32_t index = 0u; index < incremental.words().size(); ++index)
    assert(((incremental.words()[index] ^ beforeScrollWords[index]) & ~dirtyMasks[index]) == 0u);
  for (uint32_t mipIndex = 0u; mipIndex < incremental.mipLevels().size(); ++mipIndex)
  {
    const auto &before = beforeScrollMips[mipIndex];
    const auto &after = incremental.mipLevels()[mipIndex];
    std::vector<uint32_t> mipDirtyMasks(after.state.wordCount, 0u);
    for (const auto dirty : after.lastDirtyWords)
      mipDirtyMasks[dirty.wordIndex] |= dirty.bitMask;
    for (uint32_t index = 0u; index < after.words.size(); ++index)
      assert(((after.words[index] ^ before.words[index]) & ~mipDirtyMasks[index]) == 0u);

    std::array<int32_t, 3> retainedFirst{};
    std::array<int32_t, 3> retainedLast{};
    bool retained = true;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      retainedFirst[axis] = std::max(before.state.originCell[axis], after.state.originCell[axis]);
      retainedLast[axis] =
          std::min(static_cast<int32_t>(before.state.originCell[axis] + before.state.activeResolution[axis] - 1u), static_cast<int32_t>(after.state.originCell[axis] + after.state.activeResolution[axis] - 1u));
      retained &= retainedFirst[axis] <= retainedLast[axis];
    }
    if (retained)
      for (int32_t z = retainedFirst[2]; z <= retainedLast[2]; ++z)
        for (int32_t y = retainedFirst[1]; y <= retainedLast[1]; ++y)
          for (int32_t x = retainedFirst[0]; x <= retainedLast[0]; ++x)
          {
            const std::array<uint32_t, 3> beforeLogical{
              static_cast<uint32_t>(x - before.state.originCell[0]),
              static_cast<uint32_t>(y - before.state.originCell[1]),
              static_cast<uint32_t>(z - before.state.originCell[2]),
            };
            const std::array<uint32_t, 3> afterLogical{
              static_cast<uint32_t>(x - after.state.originCell[0]),
              static_cast<uint32_t>(y - after.state.originCell[1]),
              static_cast<uint32_t>(z - after.state.originCell[2]),
            };
            assert(sparseVoxelOccupancyMipAddress(beforeLogical, before.state).physicalCell == sparseVoxelOccupancyMipAddress(afterLogical, after.state).physicalCell);
          }
  }
  assert(rebuilt.configure(volumeSettings, limits(), &error));
  assert(rebuilt.update(movedCamera, 1u, boxes, &error));
  assertPyramidReduction(incremental);
  assertPyramidsEquivalent(incremental, rebuilt);

  // Physical words differ because a fresh build has wrap zero. Public world
  // lookups must nevertheless be identical for every logical cell.
  for (const auto point : samplePoints(incremental.state()))
    assert(incremental.occupiedAtWorldPosition(point) == rebuilt.occupiedAtWorldPosition(point));
  assert(!incremental.occupiedAtWorldPosition({100.0f, 0.0f, 0.0f}));

  const std::array<std::array<float, 3>, 8> walk = {
    std::array<float, 3>{0.25f, 0.25f, 0.25f},
    std::array<float, 3>{-1.25f, 0.25f, 0.25f},
    std::array<float, 3>{-2.25f, -1.25f, 1.25f},
    std::array<float, 3>{2.25f, 1.25f, -1.25f},
    std::array<float, 3>{3.25f, 2.25f, -1.25f},
    std::array<float, 3>{4.25f, -2.25f, 0.25f},
    std::array<float, 3>{-3.25f, 1.25f, 1.25f},
    std::array<float, 3>{-4.25f, -1.25f, -1.25f},
  };
  for (const auto camera : walk)
  {
    assert(incremental.update(camera, 1u, boxes, &error));
    SparseVoxelOccupancyVolume full;
    assert(full.configure(volumeSettings, limits(), &error));
    assert(full.update(camera, 1u, boxes, &error));
    for (const auto point : samplePoints(incremental.state()))
      assert(incremental.occupiedAtWorldPosition(point) == full.occupiedAtWorldPosition(point));
    assertPyramidReduction(incremental);
    assertPyramidsEquivalent(incremental, full);
  }

  const auto committedState = incremental.state();
  const auto committedWords = incremental.words();
  const std::vector<SparseVoxelAABB> invalidBounds = {
    {{0.0f, 0.0f, 0.0f}, {std::numeric_limits<float>::infinity(), 1.0f, 1.0f}},
  };
  assert(!incremental.update({0.25f, 0.25f, 0.25f}, 1u, invalidBounds, &error));
  assert(incremental.state().contentGeneration == committedState.contentGeneration);
  assert(incremental.words() == committedWords);
}

void testInteriorInvalidationForMovedAndRemovedSources()
{
  const auto volumeSettings = settings({8u, 8u, 8u});
  const std::array<float, 3> camera{0.25f, 0.25f, 0.25f};
  const SparseVoxelAABB oldBounds{{-2.4f, -0.4f, -0.4f}, {-1.6f, 0.4f, 0.4f}};
  const SparseVoxelAABB newBounds{{1.6f, -0.4f, -0.4f}, {2.4f, 0.4f, 0.4f}};

  SparseVoxelOccupancyVolume incremental;
  std::string error;
  assert(incremental.configure(volumeSettings, limits(), &error));
  assert(incremental.update(camera, 1u, {oldBounds}, &error));
  const uint32_t initialGeneration = incremental.state().contentGeneration;
  assert(incremental.occupiedAtWorldPosition({-2.0f, 0.0f, 0.0f}));
  assert(!incremental.occupiedAtWorldPosition({2.0f, 0.0f, 0.0f}));

  // Moving an instance invalidates both its old and new bounds. The current
  // source list contains only the new transform and repopulates the union.
  assert(incremental.update(camera, 2u, {newBounds}, {oldBounds, newBounds}, &error));
  assert(incremental.state().fullRebuild == 0u);
  assert(incremental.state().contentGeneration == initialGeneration + 1u);
  assert(!incremental.lastDirtyWords().empty());
  assert(occupiedBitCount(incremental.words()) > 0u);
  assert(!incremental.occupiedAtWorldPosition({-2.0f, 0.0f, 0.0f}));
  assert(incremental.occupiedAtWorldPosition({2.0f, 0.0f, 0.0f}));
  assertPyramidReduction(incremental);
  const auto editQueryBoxes = sparseVoxelOccupancyQueryBoxes(incremental.state(), {oldBounds, newBounds});
  assert(editQueryBoxes.size() == 2u);
  const auto mergedQueryBoxes =
      sparseVoxelOccupancyQueryBoxes(incremental.state(), {oldBounds, newBounds, oldBounds, newBounds});
  assert(mergedQueryBoxes.size() == 1u);
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    assert(mergedQueryBoxes[0].minimum[axis] <= oldBounds.minimum[axis]);
    assert(mergedQueryBoxes[0].minimum[axis] <= newBounds.minimum[axis]);
    assert(mergedQueryBoxes[0].maximum[axis] >= oldBounds.maximum[axis]);
    assert(mergedQueryBoxes[0].maximum[axis] >= newBounds.maximum[axis]);
  }

  SparseVoxelOccupancyVolume movedFull;
  assert(movedFull.configure(volumeSettings, limits(), &error));
  assert(movedFull.update(camera, 2u, {newBounds}, &error));
  assertPyramidsEquivalent(incremental, movedFull);
  for (const auto point : samplePoints(incremental.state()))
    assert(incremental.occupiedAtWorldPosition(point) == movedFull.occupiedAtWorldPosition(point));

  // Removal invalidates only the old occupied bounds; no source resplats it.
  assert(incremental.update(camera, 3u, {}, {newBounds}, &error));
  assert(incremental.state().fullRebuild == 0u);
  assert(!incremental.occupiedAtWorldPosition({2.0f, 0.0f, 0.0f}));
  assert(occupiedBitCount(incremental.words()) == 0u);
  assertPyramidReduction(incremental);

  SparseVoxelOccupancyVolume emptyFull;
  assert(emptyFull.configure(volumeSettings, limits(), &error));
  assert(emptyFull.update(camera, 3u, {}, &error));
  assertPyramidsEquivalent(incremental, emptyFull);
}

void testGPUOnlyMetadataPlanning()
{
  SparseVoxelOccupancySettings volumeSettings = settings();
  volumeSettings.maintainCPUReference = false;
  SparseVoxelOccupancyVolume volume;
  std::string error;
  assert(volume.configure(volumeSettings, limits(), &error));
  assert(volume.words().empty());
  for (const SparseVoxelOccupancyMipLevel &level : volume.mipLevels())
    assert(level.words.empty());
  assert(volume.updateGPU({0.25f, 0.25f, 0.25f}, 1u, {}, &error));
  assert(volume.state().valid != 0u);
  assert(volume.state().fullRebuild != 0u);
  assert(volume.state().dirtyWordCount == 1u);
  assert(volume.lastDirtyWords().empty());
  for (const SparseVoxelOccupancyMipLevel &level : volume.mipLevels())
    assert(level.lastDirtyWords.empty());
  assert(sparseVoxelOccupancyQueryBoxes(volume.state()).size() == 1u);

  const uint32_t initialGeneration = volume.state().contentGeneration;
  assert(volume.updateGPU({0.75f, 0.75f, 0.75f}, 1u, {}, &error));
  assert(volume.state().contentGeneration == initialGeneration);
  assert(volume.state().dirtyWordCount == 0u);
  assert(sparseVoxelOccupancyQueryBoxes(volume.state()).empty());

  assert(volume.updateGPU({1.25f, 0.25f, 0.25f}, 1u, {}, &error));
  assert(volume.state().contentGeneration == initialGeneration + 1u);
  assert(volume.state().fullRebuild == 0u);
  assert(volume.state().scrollDelta ==
         (std::array<int32_t, 3>{1, 0, 0}));
  assert(volume.state().wrapOffset ==
         (std::array<uint32_t, 3>{1u, 0u, 0u}));
  const auto scrollBoxes = sparseVoxelOccupancyQueryBoxes(volume.state());
  assert(scrollBoxes.size() == 1u);
  assert(scrollBoxes[0].minimum[0] == 18.0f);
  assert(scrollBoxes[0].maximum[0] == 19.0f);

  // A zero-volume edit on cell boundaries is expanded to the exact
  // conservative nextafter/floor cell span for the GPU classifier.
  const SparseVoxelAABB point{{0.0f, 0.0f, 0.0f},
                              {0.0f, 0.0f, 0.0f}};
  assert(volume.updateGPU(
      {1.25f, 0.25f, 0.25f}, 2u, {point}, &error));
  assert(volume.state().fullRebuild == 0u);
  const auto pointBoxes =
      sparseVoxelOccupancyQueryBoxes(volume.state(), {point});
  assert(pointBoxes.size() == 1u);
  assert(pointBoxes[0].minimum ==
         (std::array<float, 3>{-1.0f, -1.0f, -1.0f}));
  assert(pointBoxes[0].maximum ==
         (std::array<float, 3>{1.0f, 1.0f, 1.0f}));

  assert(volume.updateGPU({36.25f, 0.25f, 0.25f}, 2u, {}, &error));
  assert(volume.state().fullRebuild != 0u);
  assert(sparseVoxelOccupancyQueryBoxes(volume.state()).size() == 1u);
}

SparseVoxelScene oneLeafScene()
{
  SparseVoxelScene scene{};
  SparseVoxelBLAS model{};
  model.rootCenter = {0.0f, 0.0f, 0.0f};
  model.rootExtent = 2.0f;
  model.maxLevel = 1u;
  model.firstRoot = 0u;
  model.rootCount = 1u;
  model.hierarchyNodes.push_back({0u, 0u, 0u, 1u});
  model.octreePaths.push_back(encodeOctreePath(0u, 0u, 0u, 1u));
  model.octreeLevels.push_back(1u);
  model.centersLengths.push_back({{-0.5f, -0.5f, -0.5f}, 1.0f});
  SparseVoxelMaterialGPU material{};
  material.albedoRGBA8 = 0xff808080u;
  model.materials.push_back(material);
  scene.models.push_back(model);
  scene.instances.push_back({
    0u,
    0u,
    {2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 3.0f, 4.0f, 5.0f, 1.0f},
  });
  return scene;
}

void testResidentSceneLeafBounds()
{
  SparseVoxelSceneManager manager;
  std::string error;
  SparseVoxelScene scene = oneLeafScene();
  assert(manager.setScene(scene, &error));
  const auto bounds = collectSparseVoxelOccupancyLeafBounds(manager);
  assert(bounds.size() == 1u);
  assert(bounds[0].minimum == (std::array<float, 3>{1.0f, 3.0f, 4.5f}));
  assert(bounds[0].maximum == (std::array<float, 3>{3.0f, 4.0f, 5.0f}));

  // Occupancy is geometry-only. Appearance edits must not alter its sources.
  scene.models[0].materials[0].albedoRGBA8 = 0xff00ffffu;
  scene.models[0].materials[0].materialId = 93u;
  SparseVoxelSceneManager materialEdited;
  assert(materialEdited.setScene(scene, &error));
  const auto materialEditedBounds =
      collectSparseVoxelOccupancyLeafBounds(materialEdited);
  assert(materialEditedBounds.size() == bounds.size());
  assert(materialEditedBounds[0].minimum == bounds[0].minimum);
  assert(materialEditedBounds[0].maximum == bounds[0].maximum);
}

} // namespace

int main()
{
  testCheckedLayoutAndABI();
  testToroidalAddressingBijection();
  testNewlyExposedToroidalSlabs();
  testIncrementalSplatsMatchFullRebuild();
  testInteriorInvalidationForMovedAndRemovedSources();
  testGPUOnlyMetadataPlanning();
  testResidentSceneLeafBounds();
  std::cout << "Sparse voxel occupancy G23/G25 CPU tests passed\n";
  return 0;
}
