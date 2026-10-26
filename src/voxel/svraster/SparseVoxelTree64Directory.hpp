#pragma once

#include "voxel/svraster/SparseVoxelTree64.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct SparseVoxelTree64DirectorySettings
{
  std::array<uint32_t, 3> logicalCellResolution{};
  uint32_t chunkSide = 0u;
  std::array<float, 3> cellSize{};
  uint32_t nodeSlotCapacity = 0u;
  uint32_t nodeSlotCount = 0u;
};

struct SparseVoxelTree64DirectoryUpdate
{
  SparseVoxelTree64StateGPU targetState{};
  uint64_t sceneRevision = 0u;
  std::array<int32_t, 3> scrollDelta{};
  bool fullRebuild = false;
  bool fullSceneRebuild = false;
  bool metadataOnly = false;
  std::vector<std::array<int32_t, 3>> retainedChunks;
  std::vector<std::array<int32_t, 3>> enteringChunks;
  std::vector<std::array<int32_t, 3>> leavingChunks;
  // Retained chunks whose scene content is being replaced. buildChunks is the
  // stable, unique union of enteringChunks and rebuiltChunks and defines the
  // job/result/publication index space.
  std::vector<std::array<int32_t, 3>> rebuiltChunks;
  std::vector<std::array<int32_t, 3>> buildChunks;
  std::vector<SparseVoxelTree64BuildJobGPU> buildJobs;
  std::vector<SparseVoxelTree64PublishRecordGPU> publicationRecords;
};

struct SparseVoxelTree64DirectoryStats
{
  uint32_t nodeSlotCount = 0u;
  uint32_t nodesPerSlot = 0u;
  uint32_t freeSlots = 0u;
  uint32_t reservedSlots = 0u;
  uint32_t committedSlots = 0u;
  uint32_t retiredSlots = 0u;
  uint32_t highWatermark = 0u;
  uint64_t failedAllocations = 0u;
};

enum class SparseVoxelTree64FailurePolicy : uint8_t
{
  RetainCommitted,
  PublishUnknown,
};

bool makeSparseVoxelTree64DirectoryState(const SparseVoxelTree64DirectorySettings &settings, const std::array<float, 3> &cameraWorldPosition, SparseVoxelTree64StateGPU &state, std::string *error = nullptr);

bool prepareSparseVoxelTree64DirectoryUpdate(
    const SparseVoxelTree64DirectorySettings &settings,
    const SparseVoxelTree64StateGPU *committedState,
    const std::array<float, 3> &cameraWorldPosition,
    SparseVoxelTree64DirectoryUpdate &update,
    std::string *error = nullptr);

class SparseVoxelTree64Directory
{
public:
  // Completion values are caller-owned, monotonically increasing GPU
  // submission tokens. reclaimCompleted must only receive a token after the
  // corresponding RenderGraph/RHI future has actually completed.
  bool configure(const SparseVoxelTree64DirectorySettings &settings, std::string *error = nullptr);
  bool beginUpdate(const std::array<float, 3> &cameraWorldPosition, SparseVoxelTree64DirectoryUpdate &update, std::string *error = nullptr);
  // invalidatedWorldChunks must be the complete old/new affected chunk set
  // for sceneRevision. If completeInvalidation is false, all active chunks are
  // rebuilt conservatively. A changed revision with a complete set entirely
  // outside the active range updates metadata without allocating slots.
  bool beginSceneUpdate(
      const std::array<float, 3> &cameraWorldPosition,
      uint64_t sceneRevision,
      const std::vector<std::array<int32_t, 3>> &invalidatedWorldChunks,
      bool completeInvalidation,
      SparseVoxelTree64DirectoryUpdate &update,
      std::string *error = nullptr);
  bool commitPending(
      const std::vector<SparseVoxelTree64BuildResultGPU> &results,
      const std::vector<uint8_t> &occupied,
      uint64_t retireAfterCompletion,
      std::string *error = nullptr,
      SparseVoxelTree64FailurePolicy failurePolicy = SparseVoxelTree64FailurePolicy::RetainCommitted);
  bool commitPendingScene(
      uint64_t sceneRevision,
      const std::vector<SparseVoxelTree64BuildResultGPU> &results,
      const std::vector<uint8_t> &occupied,
      uint64_t retireAfterCompletion,
      std::string *error = nullptr,
      SparseVoxelTree64FailurePolicy failurePolicy = SparseVoxelTree64FailurePolicy::RetainCommitted);
  bool abortPending(uint64_t retireAfterCompletion, std::string *error = nullptr);
  bool reclaimCompleted(uint64_t completedSubmission, std::string *error = nullptr);

  bool configured() const;
  bool hasPendingUpdate() const;
  const SparseVoxelTree64StateGPU &state() const;
  const std::vector<SparseVoxelTree64ChunkGPU> &directory() const;
  const SparseVoxelTree64DirectoryUpdate *pendingUpdate() const;
  SparseVoxelTree64DirectoryStats stats() const;
  uint64_t completedSubmission() const;
  uint64_t sceneRevision() const;

private:
  enum class SlotState : uint8_t
  {
    Free,
    Reserved,
    Committed,
    Retired,
  };

  struct RetiredSlot
  {
    uint32_t slot = 0u;
    uint64_t completion = 0u;
  };

  void reset();
  void refreshStats();
  void releaseSlot(uint32_t slot, uint64_t completion);
  void sortFreeSlots();
  bool beginUpdateInternal(
      const std::array<float, 3> &cameraWorldPosition,
      uint64_t sceneRevision,
      const std::vector<std::array<int32_t, 3>> &invalidatedWorldChunks,
      bool sceneBound,
      bool completeInvalidation,
      SparseVoxelTree64DirectoryUpdate &update,
      std::string *error);
  bool commitPendingInternal(
      const std::optional<uint64_t> &sceneRevision,
      const std::vector<SparseVoxelTree64BuildResultGPU> &results,
      const std::vector<uint8_t> &occupied,
      uint64_t retireAfterCompletion,
      std::string *error,
      SparseVoxelTree64FailurePolicy failurePolicy);

  SparseVoxelTree64DirectorySettings settings_{};
  SparseVoxelTree64StateGPU state_{};
  std::vector<SparseVoxelTree64ChunkGPU> directory_;
  std::vector<SlotState> slotStates_;
  std::vector<uint32_t> freeSlots_;
  std::vector<RetiredSlot> retiredSlots_;
  std::optional<SparseVoxelTree64DirectoryUpdate> pending_;
  SparseVoxelTree64DirectoryStats stats_{};
  uint64_t completedSubmission_ = 0u;
  uint64_t latestRetirementSubmission_ = 0u;
  uint64_t sceneRevision_ = 0u;
  bool pendingSceneBound_ = false;
  bool configured_ = false;
};

} // namespace voxel::svraster
