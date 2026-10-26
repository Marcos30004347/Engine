#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct SparseVoxelTree64PageHandle
{
  uint32_t slot = std::numeric_limits<uint32_t>::max();
  uint32_t generation = 0u;

  bool valid() const
  {
    return slot != std::numeric_limits<uint32_t>::max() && generation != 0u;
  }

  friend bool operator==(
      const SparseVoxelTree64PageHandle &left,
      const SparseVoxelTree64PageHandle &right)
  {
    return left.slot == right.slot && left.generation == right.generation;
  }

  friend bool operator!=(
      const SparseVoxelTree64PageHandle &left,
      const SparseVoxelTree64PageHandle &right)
  {
    return !(left == right);
  }
};

enum class SparseVoxelTree64PageState : uint8_t
{
  Free,
  Reserved,
  Uploading,
  Uploaded,
  Committed,
  Retired,
};

struct SparseVoxelTree64PageAllocatorSettings
{
  uint32_t pageCount = 0u;
  uint32_t nodesPerPage = 0u;
};

struct SparseVoxelTree64PageAllocatorStats
{
  uint32_t pageCount = 0u;
  uint32_t nodesPerPage = 0u;
  uint32_t freePages = 0u;
  uint32_t reservedPages = 0u;
  uint32_t uploadingPages = 0u;
  uint32_t uploadedPages = 0u;
  uint32_t committedPages = 0u;
  uint32_t retiredPages = 0u;
  uint32_t highWatermark = 0u;
  uint64_t failedAllocations = 0u;
  uint64_t staleOperations = 0u;
};

// One atomically validated logical-owner update. An invalid replacement
// retires the previous owner; an invalid previous handle installs a new owner.
// At least one handle must be valid.
struct SparseVoxelTree64PageReplacement
{
  SparseVoxelTree64PageHandle previous{};
  SparseVoxelTree64PageHandle replacement{};
};

// CPU ownership state for immutable Tree64 pages. This class deliberately
// does not expose page memory: the renderer owns staging and GPU buffers, and
// uses these transitions to prove that no committed descriptor can reach a
// partially uploaded page.
class SparseVoxelTree64PageAllocator
{
public:
  bool configure(
      const SparseVoxelTree64PageAllocatorSettings &settings,
      std::string *error = nullptr);

  bool reserve(
      SparseVoxelTree64PageHandle &handle,
      std::string *error = nullptr);
  bool beginUpload(
      SparseVoxelTree64PageHandle handle,
      std::string *error = nullptr);
  bool finishUpload(
      SparseVoxelTree64PageHandle handle,
      std::string *error = nullptr);
  bool commit(
      SparseVoxelTree64PageHandle handle,
      std::string *error = nullptr);

  // Validate the complete batch before changing any page state. The
  // visibility completion token is the real RHI/render-graph submission after
  // which no command can still resolve `previous` through the old descriptor.
  bool publish(
      const std::vector<SparseVoxelTree64PageReplacement> &replacements,
      uint64_t visibilityCompletion,
      std::string *error = nullptr);
  bool validatePublish(
      const std::vector<SparseVoxelTree64PageReplacement> &replacements,
      uint64_t visibilityCompletion,
      std::string *error = nullptr) const;

  // Reserved pages that never reached the GPU may use the current completed
  // token and return immediately. Uploading/uploaded pages must be retired
  // behind the submission that could still write them.
  bool abort(
      SparseVoxelTree64PageHandle handle,
      uint64_t retireAfterCompletion,
      std::string *error = nullptr);
  bool abortAll(
      const std::vector<SparseVoxelTree64PageHandle> &handles,
      uint64_t retireAfterCompletion,
      std::string *error = nullptr);
  bool retire(
      SparseVoxelTree64PageHandle handle,
      uint64_t retireAfterCompletion,
      std::string *error = nullptr);
  bool reclaimCompleted(
      uint64_t completedSubmission,
      std::string *error = nullptr);

  bool configured() const;
  bool isCurrent(SparseVoxelTree64PageHandle handle) const;
  bool isReachable(SparseVoxelTree64PageHandle handle) const;
  std::optional<SparseVoxelTree64PageState> state(
      SparseVoxelTree64PageHandle handle) const;
  SparseVoxelTree64PageAllocatorStats stats() const;
  uint64_t completedSubmission() const;

private:
  struct Page
  {
    uint32_t generation = 0u;
    SparseVoxelTree64PageState state = SparseVoxelTree64PageState::Free;
  };

  struct RetiredPage
  {
    SparseVoxelTree64PageHandle handle{};
    uint64_t completion = 0u;
  };

  bool transition(
      SparseVoxelTree64PageHandle handle,
      SparseVoxelTree64PageState expected,
      SparseVoxelTree64PageState target,
      const char *operation,
      std::string *error);
  bool release(
      SparseVoxelTree64PageHandle handle,
      uint64_t retireAfterCompletion,
      std::string *error);
  void refreshStats();
  void sortFreePages();
  void reset();

  SparseVoxelTree64PageAllocatorSettings settings_{};
  std::vector<Page> pages_;
  std::vector<uint32_t> freePages_;
  std::vector<RetiredPage> retiredPages_;
  SparseVoxelTree64PageAllocatorStats stats_{};
  uint64_t completedSubmission_ = 0u;
  bool configured_ = false;
};

} // namespace voxel::svraster
